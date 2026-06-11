#include <string.h>

#include "frame.h"

ssize_t opc_frame_build(uint8_t *frame, size_t cap,
                        uint8_t cmd_type, uint16_t req_id, uint16_t seq_no,
                        uint16_t length_field,
                        const uint8_t *body, size_t body_len)
{
    if (!frame) {
        return -1;
    }
    if (body_len > 0 && !body) {
        return -1;
    }
    if (body_len > OPC_PAYLOAD_MAX) {
        return -1;
    }
    if (cap < OPC_HEADER_SIZE + body_len) {
        return -1;
    }

    opc_header_t hdr = {
        .protocol_version  = OPC_PROTOCOL_VERSION,
        .command_type      = cmd_type,
        .req_indication_id = req_id,
        .sequence_number   = seq_no,
        .length            = length_field,
    };
    if (opc_header_pack(frame, &hdr) != 0) {
        return -1;
    }
    if (body_len > 0) {
        memcpy(frame + OPC_HEADER_SIZE, body, body_len);
    }
    return (ssize_t)(OPC_HEADER_SIZE + body_len);
}

ssize_t opc_empty_frame_build(uint8_t *frame, size_t cap,
                              uint8_t cmd_type, uint16_t req_id, uint16_t seq_no)
{
    if (!frame || cap < OPC_FIXED_HEADER_SIZE) {
        return -1;
    }
    opc_header_t hdr = {
        .protocol_version  = OPC_PROTOCOL_VERSION,
        .command_type      = cmd_type,
        .req_indication_id = req_id,
        .sequence_number   = seq_no,
        .length            = 0,
    };
    if (opc_fixed_header_pack(frame, &hdr) != 0) {
        return -1;
    }
    return (ssize_t)OPC_FIXED_HEADER_SIZE;
}

int opc_frame_parse(const uint8_t *frame, size_t frame_len,
                    opc_header_t *hdr_out,
                    const uint8_t **body_out, size_t *body_len_out)
{
    if (!frame || !hdr_out) {
        return -1;
    }
    if (frame_len < OPC_FIXED_HEADER_SIZE) {
        return -1;
    }
    if (frame_len > OPC_FRAME_MAX) {
        return -1;
    }
    /* Valid sizes: exactly the 8-byte fixed header (empty request, no reserve)
     * or at least the full 64-byte common header. 9..63 B is malformed. */
    if (frame_len != OPC_FIXED_HEADER_SIZE && frame_len < OPC_HEADER_SIZE) {
        return -1;
    }
    if (opc_fixed_header_unpack(frame, frame_len, hdr_out) != 0) {
        return -1;
    }
    /* SEC-003: cross-check the declared length against the datagram. The header
     * length field carries (total_frame_bytes - OPC_FIXED_HEADER_SIZE); a frame
     * whose length disagrees with its actual size is malformed, so reject it
     * here rather than trusting a lying length downstream. frame_len >= the
     * fixed header is already guaranteed above, so the subtraction cannot
     * underflow, and (1424 - 8) fits a uint16_t. Self-consistent short-body
     * frames still pass this gate and reach the per-command unpack, which
     * issues OPC_ERR_PACKET_SIZE as before. */
    if (hdr_out->length != (uint16_t)(frame_len - OPC_FIXED_HEADER_SIZE)) {
        return -1;
    }
    /* Body present only once the frame reaches the full common header.
     * Empty-request frames stop at the 8-byte fixed header (no reserve/body). */
    if (frame_len >= OPC_HEADER_SIZE) {
        if (body_out)     *body_out     = frame + OPC_HEADER_SIZE;
        if (body_len_out) *body_len_out = frame_len - OPC_HEADER_SIZE;
    } else {
        if (body_out)     *body_out     = NULL;
        if (body_len_out) *body_len_out = 0;
    }
    return 0;
}
