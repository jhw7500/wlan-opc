#include <string.h>

#include "commands.h"

/* --------- helpers --------- */

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
    /* Copy at most dst_len-1 chars from src, NUL-terminate, zero the rest. */
    memset(dst, 0, dst_len);
    if (!src || dst_len == 0) {
        return;
    }
    size_t n = bounded_strnlen(src, dst_len - 1);
    memcpy(dst, src, n);
    /* dst[n] is already 0 because of the memset above */
}

/* --------- common simple-ack codec --------- */

ssize_t opc_simple_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t req_id, uint16_t seq_no,
                            uint16_t length_field,
                            uint16_t result, uint16_t error_cause)
{
    uint8_t body[OPC_SIMPLE_ACK_BODY_LEN];
    opc_be16_write(&body[0], result);
    opc_be16_write(&body[2], error_cause);
    return opc_frame_build(frame, cap,
                           OPC_CMD_ACK, req_id, seq_no,
                           length_field,
                           body, sizeof body);
}

int opc_simple_ack_unpack(const uint8_t *frame, size_t frame_len,
                          uint16_t expected_req_id,
                          uint16_t *result_out, uint16_t *error_cause_out)
{
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) {
        return -1;
    }
    if (hdr.command_type != OPC_CMD_ACK)             return -1;
    if (hdr.req_indication_id != expected_req_id)    return -1;
    if (body_len < OPC_SIMPLE_ACK_BODY_LEN)           return -1;

    if (result_out)      *result_out      = opc_be16_read(&body[0]);
    if (error_cause_out) *error_cause_out = opc_be16_read(&body[2]);
    return 0;
}

/* --------- empty-body request helper --------- */

static ssize_t empty_req_pack(uint8_t *frame, size_t cap,
                              uint16_t req_id, uint16_t seq_no,
                              uint16_t length_field)
{
    return opc_frame_build(frame, cap,
                           OPC_CMD_REQUEST, req_id, seq_no,
                           length_field,
                           NULL, 0);
}

static int empty_req_unpack(const uint8_t *frame, size_t frame_len,
                            uint16_t expected_req_id)
{
    opc_header_t hdr;
    if (opc_frame_parse(frame, frame_len, &hdr, NULL, NULL) != 0) {
        return -1;
    }
    if (hdr.command_type != OPC_CMD_REQUEST)         return -1;
    if (hdr.req_indication_id != expected_req_id)    return -1;
    return 0;
}

/* ========================================================================
 * 0xF001 — Login
 * ======================================================================== */

ssize_t opc_login_req_pack(uint8_t *frame, size_t cap,
                           uint16_t seq_no, const opc_login_req_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_LOGIN_REQ_BODY_LEN];
    copy_with_null_pad(body, sizeof body, in->password);
    return opc_frame_build(frame, cap,
                           OPC_CMD_REQUEST, OPC_REQ_LOGIN, seq_no,
                           OPC_LOGIN_REQ_LENGTH,
                           body, sizeof body);
}

int opc_login_req_unpack(const uint8_t *frame, size_t frame_len,
                         opc_login_req_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) {
        return -1;
    }
    if (hdr.command_type != OPC_CMD_REQUEST)         return -1;
    if (hdr.req_indication_id != OPC_REQ_LOGIN)      return -1;
    if (body_len < OPC_LOGIN_REQ_BODY_LEN)           return -1;
    memcpy(out->password, body, OPC_LOGIN_REQ_BODY_LEN);
    out->password[OPC_LOGIN_REQ_BODY_LEN - 1] = '\0';
    return 0;
}

ssize_t opc_login_ack_pack(uint8_t *frame, size_t cap,
                           uint16_t seq_no, const opc_login_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap,
                               OPC_REQ_LOGIN, seq_no, OPC_SIMPLE_ACK_LENGTH,
                               in->result, in->error_cause);
}

int opc_login_ack_unpack(const uint8_t *frame, size_t frame_len,
                         opc_login_ack_t *out)
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

ssize_t opc_logout_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t seq_no, const opc_logout_ack_t *in)
{
    if (!in) return -1;
    return opc_simple_ack_pack(frame, cap,
                               OPC_REQ_LOGOUT, seq_no, OPC_SIMPLE_ACK_LENGTH,
                               in->result, in->error_cause);
}

int opc_logout_ack_unpack(const uint8_t *frame, size_t frame_len,
                          opc_logout_ack_t *out)
{
    if (!out) return -1;
    return opc_simple_ack_unpack(frame, frame_len, OPC_REQ_LOGOUT,
                                 &out->result, &out->error_cause);
}

/* ========================================================================
 * 0x0001 — Get Basic Information
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

ssize_t opc_get_basic_info_ack_pack(uint8_t *frame, size_t cap,
                                    uint16_t seq_no,
                                    const opc_get_basic_info_ack_t *in)
{
    if (!in) return -1;
    uint8_t body[OPC_GET_BASIC_INFO_ACK_BODY_LEN];
    memset(body, 0, sizeof body);
    opc_be32_write(&body[0],  in->vendor_code);
    opc_be16_write(&body[4],  in->product_code);
    opc_be16_write(&body[6],  in->product_subcode);
    opc_be32_write(&body[8],  in->device_status);
    /* body[12..13] reserve */
    opc_be16_write(&body[14], in->station_type);
    return opc_frame_build(frame, cap,
                           OPC_CMD_ACK, OPC_REQ_GET_BASIC_INFO, seq_no,
                           OPC_GET_BASIC_INFO_ACK_LENGTH,
                           body, sizeof body);
}

int opc_get_basic_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                  opc_get_basic_info_ack_t *out)
{
    if (!out) return -1;
    opc_header_t hdr;
    const uint8_t *body;
    size_t body_len;
    if (opc_frame_parse(frame, frame_len, &hdr, &body, &body_len) != 0) {
        return -1;
    }
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
 * 0x0002 — Get Device Information (Req only — Ack lands in next commit)
 * ======================================================================== */

ssize_t opc_get_device_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no)
{
    return empty_req_pack(frame, cap, OPC_REQ_GET_DEVICE_INFO, seq_no,
                          OPC_GET_DEVICE_INFO_REQ_LENGTH);
}

int opc_get_device_info_req_unpack(const uint8_t *frame, size_t frame_len)
{
    return empty_req_unpack(frame, frame_len, OPC_REQ_GET_DEVICE_INFO);
}
