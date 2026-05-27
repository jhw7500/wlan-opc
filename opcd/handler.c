#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "handler.h"
#include "indication.h"
#include "store.h"

/* ---- session helpers ---- */

static time_t mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void session_touch(opcd_state_t *st)
{
    st->idle_deadline = mono_now() + st->conf.login_idle_s;
}

static bool session_owns(const opcd_state_t *st, uint32_t client_ip)
{
    return st->logged_in && st->holder_ip == client_ip;
}

static int check_login_required(const opcd_state_t *st, uint32_t client_ip,
                                uint16_t *result, uint16_t *err)
{
    if (!st->logged_in) {
        *result = OPC_RESULT_NG;
        *err    = OPC_ERR_LOGIN_VIOLATION;
        return -1;
    }
    if (!session_owns(st, client_ip)) {
        *result = OPC_RESULT_NG;
        *err    = OPC_ERR_LOGIN_CONDITION;
        return -1;
    }
    return 0;
}

/* ---- file persistence helpers (best-effort — log + continue on failure) ---- */

static int save_password(opcd_state_t *st)
{
    return opc_store_write_atomic(st->paths.password, st->password,
                                  strnlen(st->password, sizeof st->password), 0600);
}

static int save_radio(opcd_state_t *st)
{
    return opc_store_write_atomic(st->paths.radio, &st->radio, sizeof st->radio, 0644);
}

static int save_ip_list(opcd_state_t *st)
{
    return opc_store_write_atomic(st->paths.ip_list, &st->ip_list, sizeof st->ip_list, 0644);
}

/* ---- handlers ---- */

static int handle_login(opcd_state_t *st, const uint8_t *frame, size_t flen,
                        uint32_t ip, uint16_t port,
                        uint8_t *resp, size_t rcap, ssize_t *rlen, uint16_t seq)
{
    opc_login_req_t req;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;

    if (opc_login_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (st->boot_status == OPC_DEVICE_BOOTING) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_VIOLATION;
    } else if (st->logged_in && st->holder_ip != ip) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_CONDITION;
    } else if (strncmp(req.password, st->password, sizeof st->password - 1) != 0) {
        result = OPC_RESULT_NG; err = 0x0010;   /* password mismatch */
    } else {
        st->logged_in   = true;
        st->holder_ip   = ip;
        st->holder_port = port;
        st->boot_status = OPC_DEVICE_LOGGED_IN;
        session_touch(st);
        opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_IN);
    }

    opc_login_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_login_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_logout(opcd_state_t *st, const uint8_t *frame, size_t flen,
                         uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                         ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    if (opc_logout_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (!st->logged_in) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_VIOLATION;
    } else if (!session_owns(st, ip)) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_CONDITION;
    } else {
        st->logged_in   = false;
        st->boot_status = OPC_DEVICE_READY;
        opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_OUT);
    }
    opc_logout_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_logout_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_get_basic_info(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                 uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                 ssize_t *rlen, uint16_t seq)
{
    (void)ip; (void)port;
    if (opc_get_basic_info_req_unpack(frame, flen) != 0) {
        /* Spec says this must always answer — best effort with current state. */
    }
    opc_get_basic_info_ack_t ack = {
        .vendor_code     = st->conf.vendor_code,
        .product_code    = st->conf.product_code,
        .product_subcode = st->conf.product_subcode,
        .device_status   = st->boot_status,
        .station_type    = st->radio.station_type ? st->radio.station_type
                                                  : st->conf.default_station_type,
    };
    *rlen = opc_get_basic_info_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_get_device_info(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                  uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                  ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_get_device_info_ack_t ack;
    memset(&ack, 0, sizeof ack);

    if (opc_get_device_info_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (st->indication_enabled) {
            result = OPC_RESULT_NG; err = OPC_ERR_INDICATION_SETTING_VIOLATION;
        } else {
            ack.vendor_code     = st->conf.vendor_code;
            ack.product_code    = st->conf.product_code;
            ack.product_subcode = st->conf.product_subcode;
            /* Platform stub fields — real values come from the NXP driver later. */
            ack.manufacture     = (opc_date_t){ .year = 2026, .month = 2, .day = 28 };
            ack.shipment        = (opc_date_t){ .year = 2026, .month = 3, .day = 15 };
            strncpy(ack.firmware_version, "wlan-opc-0.1.0", sizeof ack.firmware_version - 1);
            strncpy(ack.hardware_version, "NXP88W9098",     sizeof ack.hardware_version - 1);
            strncpy(ack.serial_number,    "SN-STUB-0001",   sizeof ack.serial_number - 1);
            session_touch(st);
            ack.device_status = st->boot_status;
            ack.station_type  = st->radio.station_type;
            ack.priority_ch   = st->radio.priority_ch;
            ack.wlan1.freq_mhz  = st->radio.wlan1.freq_mhz;
            ack.wlan1.channel   = st->radio.wlan1.channel;
            ack.wlan1.mode      = st->radio.wlan1.mode;
            ack.wlan1.bandwidth = st->radio.wlan1.bandwidth;
            if (st->radio.station_type == OPC_STATION_DUAL) {
                ack.wlan2.freq_mhz  = st->radio.wlan2.freq_mhz;
                ack.wlan2.channel   = st->radio.wlan2.channel;
                ack.wlan2.mode      = st->radio.wlan2.mode;
                ack.wlan2.bandwidth = st->radio.wlan2.bandwidth;
            }
        }
    }
    ack.result      = result;
    ack.error_cause = err;
    *rlen = opc_get_device_info_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_set_password(opcd_state_t *st, const uint8_t *frame, size_t flen,
                               uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                               ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_password_req_t req;

    if (opc_set_password_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (strncmp(req.old_password, st->password, sizeof st->password - 1) != 0) {
            result = OPC_RESULT_NG; err = 0x0010;
        } else {
            size_t n = strnlen(req.new_password, sizeof st->password - 1);
            memset(st->password, 0, sizeof st->password);
            memcpy(st->password, req.new_password, n);
            if (save_password(st) != 0) {
                result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
            }
            session_touch(st);
        }
    }
    opc_set_password_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_set_password_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_set_ip_config_list(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                     uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                     ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_ip_config_list_req_t req;

    if (opc_set_ip_config_list_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        for (size_t i = 0; i < req.entry_count; i++) {
            const opc_ipcfg_entry_t *e = &req.entries[i];
            if (e->list_number < 1 || e->list_number > OPC_IPCFG_LIST_MAX_SLOTS) {
                result = OPC_RESULT_NG; err = 0x0010;   /* slot # out of range */
                break;
            }
            uint16_t slot = (uint16_t)(e->list_number - 1);
            st->ip_list_staging.slots[slot] = *e;
            st->ip_list_staging.present[slot] = 1;

            uint16_t flag = e->boundary_flag;
            if (flag == OPC_LIST_BOUNDARY_START || flag == OPC_LIST_BOUNDARY_START_END) {
                st->ip_list_staging_active = true;
            }
            if (flag == OPC_LIST_BOUNDARY_END || flag == OPC_LIST_BOUNDARY_START_END) {
                /* Commit staging atomically. */
                st->ip_list = st->ip_list_staging;
                if (save_ip_list(st) != 0) {
                    result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
                }
                st->ip_list_staging_active = false;
                memset(&st->ip_list_staging, 0, sizeof st->ip_list_staging);
            }
        }
        session_touch(st);
    }
    opc_set_ip_config_list_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_set_ip_config_list_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_change_ip_address(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                    uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                    ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_change_ip_address_req_t req;

    if (opc_change_ip_address_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (st->ip_list_staging_active) {
            result = OPC_RESULT_NG; err = 0x0012;        /* IP-change conflict during list update */
        } else if (req.list_number < 1 || req.list_number > OPC_IPCFG_LIST_MAX_SLOTS) {
            result = OPC_RESULT_NG; err = 0x0010;
        } else if (!st->ip_list.present[req.list_number - 1]) {
            result = OPC_RESULT_NG; err = 0x0011;        /* slot empty */
        } else {
            st->ip_change_pending = true;
            st->ip_change_list_no = req.list_number;
            session_touch(st);
        }
    }
    opc_change_ip_address_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_change_ip_address_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static bool valid_wlan_mode(uint8_t m)
{
    return m == OPC_WLAN_MODE_11A || m == OPC_WLAN_MODE_11B ||
           m == OPC_WLAN_MODE_11G || m == OPC_WLAN_MODE_11N ||
           m == OPC_WLAN_MODE_11AC || m == OPC_WLAN_MODE_11AX;
}

static bool valid_wlan_bw(uint8_t b)
{
    return b == OPC_BANDWIDTH_20 || b == OPC_BANDWIDTH_40 ||
           b == OPC_BANDWIDTH_80 || b == OPC_BANDWIDTH_160 ||
           b == OPC_BANDWIDTH_80_80 || b == OPC_BANDWIDTH_320;
}

static int handle_set_radio_config(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                   uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                   ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_radio_config_req_t req;

    if (opc_set_radio_config_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (req.station_type != OPC_STATION_SINGLE && req.station_type != OPC_STATION_DUAL) {
            result = OPC_RESULT_NG; err = 0x0010;
        } else if (!valid_wlan_mode(req.wlan1.mode)) {
            result = OPC_RESULT_NG; err = 0x0013;
        } else if (!valid_wlan_bw(req.wlan1.bandwidth)) {
            result = OPC_RESULT_NG; err = 0x0014;
        } else if (req.station_type == OPC_STATION_DUAL && !valid_wlan_mode(req.wlan2.mode)) {
            result = OPC_RESULT_NG; err = 0x0013;
        } else if (req.station_type == OPC_STATION_DUAL && !valid_wlan_bw(req.wlan2.bandwidth)) {
            result = OPC_RESULT_NG; err = 0x0014;
        } else {
            st->radio = req;
            if (save_radio(st) != 0) {
                result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
            }
            session_touch(st);
        }
    }
    opc_set_radio_config_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_set_radio_config_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_set_indication_config(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                        uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                        ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_indication_config_req_t req;

    if (opc_set_indication_config_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        st->indication_enabled         = (req.info_bits != 0);
        st->indication_recipient_ip    = req.recipient_ip;
        st->indication_recipient_port  = req.recipient_port;
        st->indication_info_bits       = req.info_bits;
        st->indication_period_s        = req.period_seconds;
        st->indication_tick_counter    = 0;
        session_touch(st);
        if (st->indication_enabled) {
            /* Spec: on enable, emit InitComplete state sequence. */
            opcd_ind_init_complete(st, OPC_INIT_STATE_READY);
            opcd_ind_init_complete(st, OPC_INIT_STATE_RADIO_UP);
            if (st->logged_in) opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_IN);
        }
    }
    opc_set_indication_config_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_set_indication_config_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

static int handle_reset(opcd_state_t *st, const uint8_t *frame, size_t flen,
                        uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                        ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    if (opc_reset_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        opcd_ind_reset_notice(st, 0x00000001);
        st->should_reset = true;     /* main loop will exit(0) after sending ack */
    }
    opc_reset_ack_t ack = { .result = result, .error_cause = err };
    *rlen = opc_reset_ack_pack(resp, rcap, seq, &ack);
    return 0;
}

/* ---- dispatch ---- */

int opcd_dispatch(opcd_state_t *st,
                  const uint8_t *frame, size_t frame_len,
                  uint32_t client_ip, uint16_t client_port,
                  uint8_t *resp, size_t resp_cap, ssize_t *resp_len)
{
    opc_header_t hdr;
    if (opc_frame_parse(frame, frame_len, &hdr, NULL, NULL) != 0) {
        *resp_len = 0;
        return -1;
    }
    if (hdr.command_type != OPC_CMD_REQUEST) {
        *resp_len = 0;
        return -1;
    }
    uint16_t seq = hdr.sequence_number;

    /* Idle auto-logout — check before serving any Login-requiring command. */
    if (st->logged_in && mono_now() >= st->idle_deadline) {
        st->logged_in   = false;
        st->boot_status = OPC_DEVICE_READY;
        opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_OUT);
    }

    switch (hdr.req_indication_id) {
    case OPC_REQ_LOGIN:                return handle_login(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_LOGOUT:               return handle_logout(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_GET_BASIC_INFO:       return handle_get_basic_info(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_GET_DEVICE_INFO:      return handle_get_device_info(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_SET_PASSWORD:         return handle_set_password(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_SET_IP_CONFIG_LIST:   return handle_set_ip_config_list(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_CHANGE_IP_ADDRESS:    return handle_change_ip_address(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_SET_RADIO_CONFIG:     return handle_set_radio_config(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_SET_INDICATION_CONFIG:return handle_set_indication_config(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    case OPC_REQ_RESET:                return handle_reset(st, frame, frame_len, client_ip, client_port, resp, resp_cap, resp_len, seq);
    default:
        *resp_len = 0;
        return -1;
    }
}

void opcd_apply_pending_ip_change(opcd_state_t *st)
{
    if (!st->ip_change_pending) return;
    uint16_t n = st->ip_change_list_no;
    if (n < 1 || n > OPC_IPCFG_LIST_MAX_SLOTS) { st->ip_change_pending = false; return; }
    const opc_ipcfg_entry_t *e = &st->ip_list.slots[n - 1];
    /* In a real target this would call out to a platform hook that actually
     * rewrites the active IP/ESSID. For the 1st-stage scaffold we just log it. */
    fprintf(stderr, "opcd: apply pending IP change → slot %u ip=0x%08X essid=%s\n",
            n, e->ip_address, e->essid);
    st->ip_change_pending = false;
    st->indication_enabled = false;       /* IP changes invalidate indication target */
    memset(&st->ip_change_list_no, 0, sizeof st->ip_change_list_no);
}
