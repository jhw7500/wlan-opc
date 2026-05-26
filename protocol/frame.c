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

int opc_frame_parse(const uint8_t *frame, size_t frame_len,
                    opc_header_t *hdr_out,
                    const uint8_t **body_out, size_t *body_len_out)
{
    if (!frame || !hdr_out) {
        return -1;
    }
    if (frame_len < OPC_HEADER_SIZE) {
        return -1;
    }
    if (frame_len > OPC_FRAME_MAX) {
        return -1;
    }
    if (opc_header_unpack(frame, frame_len, hdr_out) != 0) {
        return -1;
    }
    if (body_out) {
        *body_out = frame + OPC_HEADER_SIZE;
    }
    if (body_len_out) {
        *body_len_out = frame_len - OPC_HEADER_SIZE;
    }
    return 0;
}
