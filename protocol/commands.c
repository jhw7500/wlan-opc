#include <string.h>

#include "commands.h"

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static size_t bounded_strnlen(const char *s, size_t cap)
{
    size_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

static void copy_with_null_pad(uint8_t *dst, size_t dst_len, const char *src)
{
    memset(dst, 0, dst_len);
    if (!src || dst_len == 0) {
        return;
    }
    size_t n = bounded_strnlen(src, dst_len - 1);
    memcpy(dst, src, n);
}

static void copy_string_field(char *dst, size_t dst_len, const uint8_t *src)
{
    memcpy(dst, src, dst_len);
    dst[dst_len - 1] = '\0';
}

static void pack_date(uint8_t *p, const opc_date_t *d)
{
    opc_be16_write(&p[0], d->year);
    p[2] = d->month;
    p[3] = d->day;
}

static void unpack_date(const uint8_t *p, opc_date_t *d)
{
    d->year  = opc_be16_read(&p[0]);
    d->month = p[2];
    d->day   = p[3];
}

/* ========================================================================
 * Common simple-ack
 * ======================================================================== */

ssize_t opc_simple_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t req_id, uint16_t seq_no,
                            uint16_t length_field,
                            uint16_t result, uint16_t error_cause)
{
    uint8_t body[OPC_SIMPLE_ACK_BODY_LEN];
    opc_be16_write(&body[0], result);
    opc_be16_write(&body[2], error_cause);
    return opc_frame_build(frame, cap, OPC_CMD_ACK, req_id, seq_no,
                           length_field, body, sizeof body);
}

int opc_simple_ack_unpack(const uint8_t *frame, size_t frame_len,
                          uint16_t expected_req_id,
                          uint16_t *result_out, uint16_t *error_cause_out)
{
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_ACK)            return -1;
    if (hdr.req_indication_id != expected_req_id)   return -1;
    if (body_len < OPC_SIMPLE_ACK_BODY_LEN)          return -1;
    if (result_out)      *result_out      = opc_be16_read(&body[0]);
    if (error_cause_out) *error_cause_out = opc_be16_read(&body[2]);
    return 0;
}

/* Empty-body request helpers */

static ssize_t empty_req_pack(uint8_t *frame, size_t cap,
                              uint16_t req_id, uint16_t seq_no, uint16_t length_field)
{
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, req_id, seq_no,
                           length_field, NULL, 0);
}

static int empty_req_unpack(const uint8_t *frame, size_t frame_len, uint16_t expected_req_id)
{
    opc_header_t hdr;
    if (opc_frame_parse(frame, frame_len, &hdr, NULL, NULL) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)        return -1;
    if (hdr.req_indication_id != expected_req_id)   return -1;
    return 0;
}

/* ========================================================================
 * 0xF001 — Login
 * ======================================================================== */

ssize_t opc_login_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_login_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_LOGIN_REQ_BODY_LEN];
    copy_with_null_pad(body, sizeof body, in->password);
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_LOGIN, seq_no,
                           OPC_LOGIN_REQ_LENGTH, body, sizeof body);
}

int opc_login_req_unpack(const uint8_t *frame, size_t frame_len, opc_login_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)         return -1;
    if (hdr.req_indication_id != OPC_REQ_LOGIN)      return -1;
    if (body_len < OPC_LOGIN_REQ_BODY_LEN)           return -1;
    copy_string_field(out->password, OPC_LOGIN_REQ_BODY_LEN, body);
    return 0;
}

ssize_t opc_login_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_login_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_LOGIN, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_login_ack_unpack(const uint8_t *frame, size_t frame_len, opc_login_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_LOGIN,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0xF002 — Logout
 * ======================================================================== */

ssize_t opc_logout_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no)
{
    return empty_req_pack(frame, cap, OPC_REQ_LOGOUT, seq_no, OPC_LOGOUT_REQ_LENGTH);
}

int opc_logout_req_unpack(const uint8_t *frame, size_t frame_len)
{
    return empty_req_unpack(frame, frame_len, OPC_REQ_LOGOUT);
}

ssize_t opc_logout_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_logout_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_LOGOUT, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_logout_ack_unpack(const uint8_t *frame, size_t frame_len, opc_logout_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_LOGOUT,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x0001 — GetBasicInformation
 * ======================================================================== */

ssize_t opc_get_basic_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no)
{
    return empty_req_pack(frame, cap, OPC_REQ_GET_BASIC_INFO, seq_no,
                          OPC_GET_BASIC_INFO_REQ_LENGTH);
}

int opc_get_basic_info_req_unpack(const uint8_t *frame, size_t frame_len)
{
    return empty_req_unpack(frame, frame_len, OPC_REQ_GET_BASIC_INFO);
}

ssize_t opc_get_basic_info_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                    const opc_get_basic_info_ack_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_GET_BASIC_INFO_ACK_BODY_LEN];
    memset(body, 0, sizeof body);
    opc_be32_write(&body[0],  in->vendor_code);
    opc_be16_write(&body[4],  in->product_code);
    opc_be16_write(&body[6],  in->product_subcode);
    opc_be32_write(&body[8],  in->device_status);
    opc_be16_write(&body[14], in->station_type);
    return opc_frame_build(frame, cap, OPC_CMD_ACK, OPC_REQ_GET_BASIC_INFO, seq_no,
                           OPC_GET_BASIC_INFO_ACK_LENGTH, body, sizeof body);
}

int opc_get_basic_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                  opc_get_basic_info_ack_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_ACK)                      return -1;
    if (hdr.req_indication_id != OPC_REQ_GET_BASIC_INFO)      return -1;
    if (body_len < OPC_GET_BASIC_INFO_ACK_BODY_LEN)            return -1;
    out->vendor_code     = opc_be32_read(&body[0]);
    out->product_code    = opc_be16_read(&body[4]);
    out->product_subcode = opc_be16_read(&body[6]);
    out->device_status   = opc_be32_read(&body[8]);
    out->station_type    = opc_be16_read(&body[14]);
    return 0;
}

/* ========================================================================
 * 0x0002 — GetDeviceInformation
 * ======================================================================== */

static void pack_radio_state(uint8_t *p, const opc_wlan_radio_state_t *r)
{
    /* p[0..5] MAC, p[6] Mode, p[7] BW, p[8..9] FREQ, p[10..11] CH,
     * p[12..13] Status, p[14] SNR, p[15] RSSI, p[16..21] connect AP MAC,
     * p[22..23] pad — 24B per radio block. */
    memcpy(&p[0], r->mac, 6);
    p[6] = r->mode;
    p[7] = r->bandwidth;
    opc_be16_write(&p[8],  r->freq_mhz);
    opc_be16_write(&p[10], r->channel);
    opc_be16_write(&p[12], r->status);
    p[14] = (uint8_t)r->snr;
    p[15] = (uint8_t)r->rssi;
    memcpy(&p[16], r->connect_ap_mac, 6);
    p[22] = 0;
    p[23] = 0;
}

static void unpack_radio_state(const uint8_t *p, opc_wlan_radio_state_t *r)
{
    memcpy(r->mac, &p[0], 6);
    r->mode      = p[6];
    r->bandwidth = p[7];
    r->freq_mhz  = opc_be16_read(&p[8]);
    r->channel   = opc_be16_read(&p[10]);
    r->status    = opc_be16_read(&p[12]);
    r->snr       = (int8_t)p[14];
    r->rssi      = (int8_t)p[15];
    memcpy(r->connect_ap_mac, &p[16], 6);
}

ssize_t opc_get_device_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no)
{
    return empty_req_pack(frame, cap, OPC_REQ_GET_DEVICE_INFO, seq_no,
                          OPC_GET_DEVICE_INFO_REQ_LENGTH);
}

int opc_get_device_info_req_unpack(const uint8_t *frame, size_t frame_len)
{
    return empty_req_unpack(frame, frame_len, OPC_REQ_GET_DEVICE_INFO);
}

ssize_t opc_get_device_info_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                     const opc_get_device_info_ack_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_GET_DEVICE_INFO_ACK_BODY_LEN];
    memset(body, 0, sizeof body);

    opc_be16_write(&body[0],   in->result);
    opc_be16_write(&body[2],   in->error_cause);
    opc_be32_write(&body[4],   in->vendor_code);
    opc_be16_write(&body[8],   in->product_code);
    opc_be16_write(&body[10],  in->product_subcode);
    pack_date(&body[12],       &in->manufacture);
    pack_date(&body[16],       &in->shipment);
    copy_with_null_pad(&body[20],  OPC_VERSION_FIELD_LEN, in->firmware_version);
    copy_with_null_pad(&body[52],  OPC_VERSION_FIELD_LEN, in->hardware_version);
    copy_with_null_pad(&body[84],  OPC_SERIAL_FIELD_LEN,  in->serial_number);
    memcpy(&body[116], in->ethernet_mac, 6);
    /* body[122..123] reserve */
    opc_be32_write(&body[124], in->ip_address);
    opc_be32_write(&body[128], in->subnet_mask);
    opc_be32_write(&body[132], in->default_gateway);
    opc_be32_write(&body[136], in->ntp_server);
    copy_with_null_pad(&body[140], OPC_ESSID_FIELD_LEN, in->essid);
    opc_be32_write(&body[172], in->device_status);
    opc_be16_write(&body[176], in->station_type);
    opc_be16_write(&body[178], in->priority_ch);
    body[180] = in->ieee_11r;
    body[181] = in->ieee_11ai;
    body[182] = in->ieee_11k;
    body[183] = in->ieee_11v;
    /* body[184..219] reserve */
    pack_radio_state(&body[220], &in->wlan1);
    /* body[244..279] reserve */
    pack_radio_state(&body[280], &in->wlan2);
    /* body[304..351] reserve */

    return opc_frame_build(frame, cap, OPC_CMD_ACK, OPC_REQ_GET_DEVICE_INFO, seq_no,
                           OPC_GET_DEVICE_INFO_ACK_LENGTH, body, sizeof body);
}

int opc_get_device_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                   opc_get_device_info_ack_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_ACK)                       return -1;
    if (hdr.req_indication_id != OPC_REQ_GET_DEVICE_INFO)      return -1;
    if (body_len < OPC_GET_DEVICE_INFO_ACK_BODY_LEN)            return -1;

    out->result          = opc_be16_read(&body[0]);
    out->error_cause     = opc_be16_read(&body[2]);
    out->vendor_code     = opc_be32_read(&body[4]);
    out->product_code    = opc_be16_read(&body[8]);
    out->product_subcode = opc_be16_read(&body[10]);
    unpack_date(&body[12], &out->manufacture);
    unpack_date(&body[16], &out->shipment);
    copy_string_field(out->firmware_version, OPC_VERSION_FIELD_LEN, &body[20]);
    copy_string_field(out->hardware_version, OPC_VERSION_FIELD_LEN, &body[52]);
    copy_string_field(out->serial_number,    OPC_SERIAL_FIELD_LEN,  &body[84]);
    memcpy(out->ethernet_mac, &body[116], 6);
    out->ip_address      = opc_be32_read(&body[124]);
    out->subnet_mask     = opc_be32_read(&body[128]);
    out->default_gateway = opc_be32_read(&body[132]);
    out->ntp_server      = opc_be32_read(&body[136]);
    copy_string_field(out->essid, OPC_ESSID_FIELD_LEN, &body[140]);
    out->device_status   = opc_be32_read(&body[172]);
    out->station_type    = opc_be16_read(&body[176]);
    out->priority_ch     = opc_be16_read(&body[178]);
    out->ieee_11r        = body[180];
    out->ieee_11ai       = body[181];
    out->ieee_11k        = body[182];
    out->ieee_11v        = body[183];
    unpack_radio_state(&body[220], &out->wlan1);
    unpack_radio_state(&body[280], &out->wlan2);
    return 0;
}

/* ========================================================================
 * 0x1001 — SetPassword
 * ======================================================================== */

ssize_t opc_set_password_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_set_password_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_SET_PASSWORD_REQ_BODY_LEN];
    copy_with_null_pad(&body[0],   128, in->old_password);
    copy_with_null_pad(&body[128], 128, in->new_password);
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_SET_PASSWORD, seq_no,
                           OPC_SET_PASSWORD_REQ_LENGTH, body, sizeof body);
}

int opc_set_password_req_unpack(const uint8_t *frame, size_t frame_len,
                                opc_set_password_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)                  return -1;
    if (hdr.req_indication_id != OPC_REQ_SET_PASSWORD)        return -1;
    if (body_len < OPC_SET_PASSWORD_REQ_BODY_LEN)              return -1;
    copy_string_field(out->old_password, 128, &body[0]);
    copy_string_field(out->new_password, 128, &body[128]);
    return 0;
}

ssize_t opc_set_password_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_set_password_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_SET_PASSWORD, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_set_password_ack_unpack(const uint8_t *frame, size_t frame_len,
                                opc_set_password_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_SET_PASSWORD,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x1002 — SetIpConfigList
 * ======================================================================== */

static void pack_ipcfg_entry(uint8_t *p, const opc_ipcfg_entry_t *e)
{
    opc_be16_write(&p[0],  e->boundary_flag);
    opc_be16_write(&p[2],  e->list_number);
    opc_be32_write(&p[4],  e->ip_address);
    opc_be32_write(&p[8],  e->subnet_mask);
    opc_be32_write(&p[12], e->default_gateway);
    opc_be32_write(&p[16], e->ntp_server);
    copy_with_null_pad(&p[20], 32, e->essid);
    memset(&p[52], 0, 12);
}

static void unpack_ipcfg_entry(const uint8_t *p, opc_ipcfg_entry_t *e)
{
    e->boundary_flag    = opc_be16_read(&p[0]);
    e->list_number      = opc_be16_read(&p[2]);
    e->ip_address       = opc_be32_read(&p[4]);
    e->subnet_mask      = opc_be32_read(&p[8]);
    e->default_gateway  = opc_be32_read(&p[12]);
    e->ntp_server       = opc_be32_read(&p[16]);
    copy_string_field(e->essid, 32, &p[20]);
}

ssize_t opc_set_ip_config_list_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_set_ip_config_list_req_t *in)
{
    if (!in) return -1;
    if (in->entry_count < 1 || in->entry_count > OPC_IPCFG_LIST_MAX_PER_REQ) return -1;

    uint8_t body[OPC_SET_IP_CONFIG_LIST_BODY_MAX];
    size_t body_len = in->entry_count * OPC_IPCFG_ENTRY_LEN;
    memset(body, 0, body_len);
    for (size_t i = 0; i < in->entry_count; i++) {
        pack_ipcfg_entry(&body[i * OPC_IPCFG_ENTRY_LEN], &in->entries[i]);
    }
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_SET_IP_CONFIG_LIST, seq_no,
                           (uint16_t)OPC_SET_IP_CONFIG_LIST_REQ_LENGTH(in->entry_count),
                           body, body_len);
}

int opc_set_ip_config_list_req_unpack(const uint8_t *frame, size_t frame_len,
                                      opc_set_ip_config_list_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0)    return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)                                return -1;
    if (hdr.req_indication_id != OPC_REQ_SET_IP_CONFIG_LIST)                return -1;
    if (body_len == 0 || (body_len % OPC_IPCFG_ENTRY_LEN) != 0)             return -1;
    size_t n = body_len / OPC_IPCFG_ENTRY_LEN;
    if (n > OPC_IPCFG_LIST_MAX_PER_REQ)                                     return -1;

    memset(out, 0, sizeof *out);
    out->entry_count = n;
    for (size_t i = 0; i < n; i++) {
        unpack_ipcfg_entry(&body[i * OPC_IPCFG_ENTRY_LEN], &out->entries[i]);
    }
    return 0;
}

ssize_t opc_set_ip_config_list_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_set_ip_config_list_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_SET_IP_CONFIG_LIST, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_set_ip_config_list_ack_unpack(const uint8_t *frame, size_t frame_len,
                                      opc_set_ip_config_list_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_SET_IP_CONFIG_LIST,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x1003 — ChangeIpAddress
 * ======================================================================== */

ssize_t opc_change_ip_address_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                       const opc_change_ip_address_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_CHANGE_IP_ADDRESS_REQ_BODY_LEN];
    memset(body, 0, sizeof body);
    /* body[0..1] reserve, body[2..3] list_number */
    opc_be16_write(&body[2], in->list_number);
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_CHANGE_IP_ADDRESS, seq_no,
                           OPC_CHANGE_IP_ADDRESS_REQ_LENGTH, body, sizeof body);
}

int opc_change_ip_address_req_unpack(const uint8_t *frame, size_t frame_len,
                                     opc_change_ip_address_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)                       return -1;
    if (hdr.req_indication_id != OPC_REQ_CHANGE_IP_ADDRESS)        return -1;
    if (body_len < OPC_CHANGE_IP_ADDRESS_REQ_BODY_LEN)              return -1;
    out->list_number = opc_be16_read(&body[2]);
    return 0;
}

ssize_t opc_change_ip_address_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                       const opc_change_ip_address_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_CHANGE_IP_ADDRESS, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_change_ip_address_ack_unpack(const uint8_t *frame, size_t frame_len,
                                     opc_change_ip_address_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_CHANGE_IP_ADDRESS,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x1004 — SetRadioConfig
 * ======================================================================== */

ssize_t opc_set_radio_config_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                      const opc_set_radio_config_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_SET_RADIO_CONFIG_REQ_BODY_LEN];
    memset(body, 0, sizeof body);
    opc_be16_write(&body[0],  in->station_type);
    opc_be16_write(&body[2],  in->priority_ch);
    /* WLAN#1: FREQ then CH (per spec line "68 WLAN#1 FREQ | WLAN#1 CH") */
    opc_be16_write(&body[4],  in->wlan1.freq_mhz);
    opc_be16_write(&body[6],  in->wlan1.channel);
    body[8]  = in->wlan1.mode;
    body[9]  = in->wlan1.bandwidth;
    /* body[10..11] reserve */
    /* WLAN#2: symmetric with WLAN#1 (FREQ then CH) per vendor clarification. */
    opc_be16_write(&body[12], in->wlan2.freq_mhz);
    opc_be16_write(&body[14], in->wlan2.channel);
    body[16] = in->wlan2.mode;
    body[17] = in->wlan2.bandwidth;
    /* body[18..19] reserve */
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_SET_RADIO_CONFIG, seq_no,
                           OPC_SET_RADIO_CONFIG_REQ_LENGTH, body, sizeof body);
}

int opc_set_radio_config_req_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_set_radio_config_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)                    return -1;
    if (hdr.req_indication_id != OPC_REQ_SET_RADIO_CONFIG)      return -1;
    if (body_len < OPC_SET_RADIO_CONFIG_REQ_BODY_LEN)            return -1;
    out->station_type     = opc_be16_read(&body[0]);
    out->priority_ch      = opc_be16_read(&body[2]);
    out->wlan1.freq_mhz   = opc_be16_read(&body[4]);
    out->wlan1.channel    = opc_be16_read(&body[6]);
    out->wlan1.mode       = body[8];
    out->wlan1.bandwidth  = body[9];
    out->wlan2.freq_mhz   = opc_be16_read(&body[12]);
    out->wlan2.channel    = opc_be16_read(&body[14]);
    out->wlan2.mode       = body[16];
    out->wlan2.bandwidth  = body[17];
    return 0;
}

ssize_t opc_set_radio_config_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                      const opc_set_radio_config_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_SET_RADIO_CONFIG, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_set_radio_config_ack_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_set_radio_config_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_SET_RADIO_CONFIG,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x1005 — SetIndicationConfig
 * ======================================================================== */

ssize_t opc_set_indication_config_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                           const opc_set_indication_config_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_SET_INDICATION_CONFIG_REQ_BODY_LEN];
    memset(body, 0, sizeof body);
    opc_be16_write(&body[0], in->recipient_port);
    body[2] = in->info_bits;
    body[3] = in->period_seconds;
    opc_be32_write(&body[4], in->recipient_ip);
    return opc_frame_build(frame, cap, OPC_CMD_REQUEST, OPC_REQ_SET_INDICATION_CONFIG, seq_no,
                           OPC_SET_INDICATION_CONFIG_REQ_LENGTH, body, sizeof body);
}

int opc_set_indication_config_req_unpack(const uint8_t *frame, size_t frame_len,
                                         opc_set_indication_config_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) return -1;
    if (hdr.command_type != OPC_CMD_REQUEST)                         return -1;
    if (hdr.req_indication_id != OPC_REQ_SET_INDICATION_CONFIG)      return -1;
    if (body_len < OPC_SET_INDICATION_CONFIG_REQ_BODY_LEN)            return -1;
    out->recipient_port  = opc_be16_read(&body[0]);
    out->info_bits       = body[2];
    out->period_seconds  = body[3];
    out->recipient_ip    = opc_be32_read(&body[4]);
    return 0;
}

ssize_t opc_set_indication_config_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                           const opc_set_indication_config_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap, OPC_REQ_SET_INDICATION_CONFIG, seq_no,
                               OPC_SIMPLE_ACK_LENGTH, in->result, in->error_cause);
}

int opc_set_indication_config_ack_unpack(const uint8_t *frame, size_t frame_len,
                                         opc_set_indication_config_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_SET_INDICATION_CONFIG,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x2001 — Reset
 * ======================================================================== */

ssize_t opc_reset_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no)
{
    return empty_req_pack(frame, cap, OPC_REQ_RESET, seq_no, OPC_RESET_REQ_LENGTH);
}

int opc_reset_req_unpack(const uint8_t *frame, size_t frame_len)
{
    return empty_req_unpack(frame, frame_len, OPC_REQ_RESET);
}

ssize_t opc_reset_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_reset_ack_t *in)
{
    if (!in) return -1;
    /* Length field = 0 per spec (T11) — body carries Result+ErrorCause anyway. */
    return opc_simple_ack_pack(frame, cap, OPC_REQ_RESET, seq_no,
                               OPC_RESET_ACK_LENGTH, in->result, in->error_cause);
}

int opc_reset_ack_unpack(const uint8_t *frame, size_t frame_len, opc_reset_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_RESET,
                                 &out->result, &out->error_cause);
}
