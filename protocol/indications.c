#include <assert.h>
#include <string.h>

#include "indications.h"
#include "strutil.h"

/* STYLE-007: tie the documentary length macro to the KeepAlive timestamp field
 * width so it is compile-time-checked rather than a dead constant. */
static_assert(OPC_TIMESTAMP_MAX_LEN == OPC_IND_KEEP_ALIVE_BODY_LEN - 1,
              "timestamp max chars must be the field width minus the NUL");

/* ---- generic indication-frame helpers ---- */

static int indication_parse(const uint8_t *frame, size_t frame_len, uint16_t expected_ind_id,
                            const uint8_t **body_out, size_t *body_len_out)
{
    opc_header_t hdr;
    if (opc_frame_parse(frame, frame_len, &hdr, body_out, body_len_out) != 0) return -1;
    if (hdr.command_type != OPC_CMD_INDICATION)      return -1;
    if (hdr.req_indication_id != expected_ind_id)    return -1;
    return 0;
}

/* ========================================================================
 * 0x0001 — InitComplete
 * ======================================================================== */

ssize_t opc_ind_init_complete_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                   const opc_ind_init_complete_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_INIT_COMPLETE_BODY_LEN];
    opc_be32_write(&body[0], in->status);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_INIT_COMPLETE, seq_no,
                           OPC_IND_INIT_COMPLETE_LENGTH, body, sizeof body);
}

int opc_ind_init_complete_unpack(const uint8_t *frame, size_t frame_len,
                                 opc_ind_init_complete_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_INIT_COMPLETE, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_INIT_COMPLETE_BODY_LEN) return -1;
    out->status = opc_be32_read(&body[0]);
    return 0;
}

/* ========================================================================
 * 0x0002 — WlanStatusChange
 * ======================================================================== */

ssize_t opc_ind_wlan_status_change_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_ind_wlan_status_change_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_WLAN_STATUS_CHANGE_BODY_LEN];
    opc_be16_write(&body[0], in->wlan_status);
    opc_be16_write(&body[2], in->indication_ch);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_WLAN_STATUS_CHANGE, seq_no,
                           OPC_IND_WLAN_STATUS_CHANGE_LENGTH, body, sizeof body);
}

int opc_ind_wlan_status_change_unpack(const uint8_t *frame, size_t frame_len,
                                      opc_ind_wlan_status_change_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_WLAN_STATUS_CHANGE, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_WLAN_STATUS_CHANGE_BODY_LEN) return -1;
    out->wlan_status   = opc_be16_read(&body[0]);
    out->indication_ch = opc_be16_read(&body[2]);
    return 0;
}

/* ========================================================================
 * 0x0004 — Roaming
 * ======================================================================== */

ssize_t opc_ind_roaming_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                             const opc_ind_roaming_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_ROAMING_BODY_LEN];
    memset(body, 0, sizeof body);
    body[0] = (uint8_t)in->current_snr;
    body[1] = (uint8_t)in->current_rssi;
    /* body[2..3] reserve */
    memcpy(&body[4], in->connect_ap_mac, 6);
    /* body[10..11] CH Number */
    opc_be16_write(&body[10], in->ch_number);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_ROAMING, seq_no,
                           OPC_IND_ROAMING_LENGTH, body, sizeof body);
}

int opc_ind_roaming_unpack(const uint8_t *frame, size_t frame_len,
                           opc_ind_roaming_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_ROAMING, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_ROAMING_BODY_LEN) return -1;
    out->current_snr  = (int8_t)body[0];
    out->current_rssi = (int8_t)body[1];
    memcpy(out->connect_ap_mac, &body[4], 6);
    out->ch_number    = opc_be16_read(&body[10]);
    return 0;
}

/* ========================================================================
 * 0x0008 — ApDisconnect
 * ======================================================================== */

ssize_t opc_ind_ap_disconnect_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                   const opc_ind_ap_disconnect_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_AP_DISCONNECT_BODY_LEN];
    memset(body, 0, sizeof body);
    opc_be16_write(&body[0], in->message_id);
    opc_be16_write(&body[2], in->result_code);
    memcpy(&body[4], in->disconnect_ap_mac, 6);
    /* body[10..11] reserve */
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_AP_DISCONNECT, seq_no,
                           OPC_IND_AP_DISCONNECT_LENGTH, body, sizeof body);
}

int opc_ind_ap_disconnect_unpack(const uint8_t *frame, size_t frame_len,
                                 opc_ind_ap_disconnect_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_AP_DISCONNECT, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_AP_DISCONNECT_BODY_LEN) return -1;
    out->message_id  = opc_be16_read(&body[0]);
    out->result_code = opc_be16_read(&body[2]);
    memcpy(out->disconnect_ap_mac, &body[4], 6);
    return 0;
}

/* ========================================================================
 * 0x0010 — FaultDetect
 * ======================================================================== */

ssize_t opc_ind_fault_detect_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_ind_fault_detect_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_FAULT_DETECT_BODY_LEN];
    opc_be16_write(&body[0], in->congestion_id);
    opc_be16_write(&body[2], in->current_val);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_FAULT_DETECT, seq_no,
                           OPC_IND_FAULT_DETECT_LENGTH, body, sizeof body);
}

int opc_ind_fault_detect_unpack(const uint8_t *frame, size_t frame_len,
                                opc_ind_fault_detect_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_FAULT_DETECT, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_FAULT_DETECT_BODY_LEN) return -1;
    out->congestion_id = opc_be16_read(&body[0]);
    out->current_val   = opc_be16_read(&body[2]);
    return 0;
}

/* ========================================================================
 * 0x0020 — ResetNotice
 * ======================================================================== */

ssize_t opc_ind_reset_notice_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_ind_reset_notice_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_RESET_NOTICE_BODY_LEN];
    opc_be32_write(&body[0], in->reset_cause);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_RESET_NOTICE, seq_no,
                           OPC_IND_RESET_NOTICE_LENGTH, body, sizeof body);
}

int opc_ind_reset_notice_unpack(const uint8_t *frame, size_t frame_len,
                                opc_ind_reset_notice_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_RESET_NOTICE, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_RESET_NOTICE_BODY_LEN) return -1;
    out->reset_cause = opc_be32_read(&body[0]);
    return 0;
}

/* ========================================================================
 * 0x0080 — KeepAlive
 * ======================================================================== */

ssize_t opc_ind_keep_alive_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                const opc_ind_keep_alive_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_IND_KEEP_ALIVE_BODY_LEN];
    memset(body, 0, sizeof body);
    size_t n = opc_bounded_strnlen(in->timestamp, sizeof body - 1);
    memcpy(body, in->timestamp, n);
    return opc_frame_build(frame, cap, OPC_CMD_INDICATION, OPC_IND_KEEP_ALIVE, seq_no,
                           OPC_IND_KEEP_ALIVE_LENGTH, body, sizeof body);
}

int opc_ind_keep_alive_unpack(const uint8_t *frame, size_t frame_len,
                              opc_ind_keep_alive_t *out)
{
    if (!out) return -1;
    const uint8_t *body;
    size_t body_len;
    if (indication_parse(frame, frame_len, OPC_IND_KEEP_ALIVE, &body, &body_len) != 0) return -1;
    if (body_len < OPC_IND_KEEP_ALIVE_BODY_LEN) return -1;
    memcpy(out->timestamp, body, OPC_IND_KEEP_ALIVE_BODY_LEN);
    out->timestamp[OPC_IND_KEEP_ALIVE_BODY_LEN - 1] = '\0';
    return 0;
}
