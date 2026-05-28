#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "handler.h"
#include "indication.h"
#include "inventory.h"
#include "platform.h"
#include "snapshot.h"
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
    const opcd_inventory_t *inv = opcd_inventory();
    opc_get_basic_info_ack_t ack = {
        .vendor_code     = inv->vendor_code,
        .product_code    = inv->product_code,
        .product_subcode = inv->product_subcode,
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
            /* Static identity (vendor/product/hardware/serial/dates/caps)
             * comes from /usr/local/opc/etc/device_info.json via inventory.h.
             * Live-queried fields (firmware version, ntp server, MAC/IP, link
             * state) come from the platform vtable. ack was memset(0) above,
             * so any failed lookup leaves an empty/zero field rather than
             * failing the whole Ack — GetDeviceInfo on a partly-readable
             * device is more useful than NG. */
            const opcd_inventory_t   *inv  = opcd_inventory();
            const opcd_platform_ops_t *plat = opcd_platform();
            /* platform.h: opcd_platform() may return NULL before registration;
             * the dispatch path requires it to be non-NULL. Surface a missing
             * register call as abort() instead of NULL deref. Explicit check
             * (not assert) survives -DNDEBUG release builds. Logged before
             * abort() so a crash dump has triage context. */
            if (!plat) {
                fprintf(stderr, "opcd: BUG: opcd_platform() returned NULL in dispatch\n");
                abort();
            }
            ack.vendor_code     = inv->vendor_code;
            ack.product_code    = inv->product_code;
            ack.product_subcode = inv->product_subcode;
            ack.manufacture     = inv->manufacture_date;
            ack.shipment        = inv->shipment_date;
            memcpy(ack.hardware_version, inv->hardware_version,
                   sizeof ack.hardware_version);
            memcpy(ack.serial_number,    inv->serial_number,
                   sizeof ack.serial_number);
            (void)plat->get_firmware_version(ack.firmware_version, sizeof ack.firmware_version);
            (void)plat->get_ntp_server(&ack.ntp_server);
            (void)plat->get_eth_mac(ack.ethernet_mac);
            (void)plat->get_eth_ipv4_host(&ack.ip_address);
            (void)plat->get_eth_netmask_host(&ack.subnet_mask);
            (void)plat->get_eth_gateway_host(&ack.default_gateway);
            (void)plat->get_wlan_mac(0, ack.wlan1.mac);
            /* Ack carries a single essid field — DUAL always reports mlan0 */
            (void)plat->get_essid(0, ack.essid, sizeof ack.essid);
            /* Runtime link readback — overwrites the radio-state portion of
             * wlan{1,2}. mode/bandwidth are taken from the live link when
             * available; freq/channel still come from the set-radio cache
             * (set below). mode and bandwidth are tracked separately because
             * legacy associations (11a/b/g) report mode=0 (no HE-/VHT-/MCS
             * prefix in the bitrate), and bandwidth uses a valid flag because
             * BANDWIDTH_20 == 0 cannot be distinguished from a missing field. */
            opcd_platform_link_t link = {0};
            bool w1_mode_live = false, w1_bw_live = false;
            bool w2_mode_live = false, w2_bw_live = false;
            if (plat->get_link(0, &link) == 0) {
                memcpy(ack.wlan1.connect_ap_mac, link.bssid, 6);
                ack.wlan1.snr    = link.snr;
                ack.wlan1.rssi   = link.rssi;
                ack.wlan1.status = link.associated ? 0x0001 : 0x0000;
                if (link.associated && link.mode != 0) {
                    ack.wlan1.mode = link.mode;
                    w1_mode_live = true;
                }
                if (link.associated && link.bandwidth_valid) {
                    ack.wlan1.bandwidth = link.bandwidth;
                    w1_bw_live = true;
                }
            }
            if (st->radio.station_type == OPC_STATION_DUAL) {
                (void)plat->get_wlan_mac(1, ack.wlan2.mac);
                if (plat->get_link(1, &link) == 0) {
                    memcpy(ack.wlan2.connect_ap_mac, link.bssid, 6);
                    ack.wlan2.snr    = link.snr;
                    ack.wlan2.rssi   = link.rssi;
                    ack.wlan2.status = link.associated ? 0x0001 : 0x0000;
                    if (link.associated && link.mode != 0) {
                        ack.wlan2.mode = link.mode;
                        w2_mode_live = true;
                    }
                    if (link.associated && link.bandwidth_valid) {
                        ack.wlan2.bandwidth = link.bandwidth;
                        w2_bw_live = true;
                    }
                }
            }
            ack.ieee_11r  = inv->ieee_11r;
            ack.ieee_11ai = inv->ieee_11ai;
            ack.ieee_11k  = inv->ieee_11k;
            ack.ieee_11v  = inv->ieee_11v;
            session_touch(st);
            ack.device_status = st->boot_status;
            ack.station_type  = st->radio.station_type;
            ack.priority_ch   = st->radio.priority_ch;
            ack.wlan1.freq_mhz  = st->radio.wlan1.freq_mhz;
            ack.wlan1.channel   = st->radio.wlan1.channel;
            if (!w1_mode_live) ack.wlan1.mode      = st->radio.wlan1.mode;
            if (!w1_bw_live)   ack.wlan1.bandwidth = st->radio.wlan1.bandwidth;
            if (st->radio.station_type == OPC_STATION_DUAL) {
                ack.wlan2.freq_mhz  = st->radio.wlan2.freq_mhz;
                ack.wlan2.channel   = st->radio.wlan2.channel;
                if (!w2_mode_live) ack.wlan2.mode      = st->radio.wlan2.mode;
                if (!w2_bw_live)   ack.wlan2.bandwidth = st->radio.wlan2.bandwidth;
            }
        }
    }
    ack.result      = result;
    ack.error_cause = err;
    *rlen = opc_get_device_info_ack_pack(resp, rcap, seq, &ack);
    /* Side-channel snapshot for external monitoring. Best-effort: a failed
     * write does not change the wire response we just packed. */
    (void)opcd_snapshot_publish(&ack, OPCD_SNAPSHOT_PATH);
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
            uint16_t flag = e->boundary_flag;
            if (flag == OPC_LIST_BOUNDARY_START) {
                /* Fresh sequence: drop any stale staging from a prior
                 * incomplete cycle before recording this entry. */
                memset(&st->ip_list_staging, 0, sizeof st->ip_list_staging);
                st->ip_list_staging_active = true;
            }

            uint16_t slot = (uint16_t)(e->list_number - 1);
            st->ip_list_staging.slots[slot] = *e;
            st->ip_list_staging.present[slot] = 1;

            if (flag == OPC_LIST_BOUNDARY_END) {
                if (st->ip_list_staging_active) {
                    /* Commit staging atomically. */
                    st->ip_list = st->ip_list_staging;
                    if (save_ip_list(st) != 0) {
                        result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
                    }
                    st->ip_list_staging_active = false;
                    memset(&st->ip_list_staging, 0, sizeof st->ip_list_staging);
                } else {
                    /* Lone END (no prior START in this session). Spec defines
                     * no NG for this; skip commit so a stale staging buffer
                     * cannot be flushed to NVM. */
                    fprintf(stderr,
                            "opcd: SetIPConfigList: END without prior START — commit skipped\n");
                }
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
            const opcd_platform_ops_t *plat = opcd_platform();
            if (!plat) {
                fprintf(stderr, "opcd: BUG: opcd_platform() returned NULL in handle_set_radio_config\n");
                abort();
            }
            if (plat->apply_radio_config(&req) != 0) {
                /* regulation-class NG — platform refused the kernel change */
                result = OPC_RESULT_NG; err = 0x0050;
            } else {
                st->radio = req;
                if (save_radio(st) != 0) {
                    result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
                }
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
