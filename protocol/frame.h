#ifndef WLAN_OPC_FRAME_H
#define WLAN_OPC_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build a complete body-bearing OPC frame (64-byte common header + body).
 *
 * The 64-byte common header has its 56-byte reserve area (bytes 8..63)
 * zero-filled; header fields are written big-endian per spec. The body is
 * placed at byte 64.
 *
 * `length_field` is the literal value placed in the header's Length field.
 * Per spec Rev1.00 (confirmed against the docx figures): Length =
 * total_frame_bytes - 8 = reserve(56) + body_len. Per-command helpers in
 * commands.h supply this via OPC_*_REQ_LENGTH / OPC_*_ACK_LENGTH macros.
 *
 * Returns total frame size (OPC_HEADER_SIZE + body_len) on success,
 * or -1 on invalid argument / insufficient capacity.
 */
ssize_t opc_frame_build(uint8_t *frame, size_t cap,
                        uint8_t cmd_type, uint16_t req_id, uint16_t seq_no,
                        uint16_t length_field,
                        const uint8_t *body, size_t body_len);

/*
 * Build an empty-body request frame: only the 8-byte fixed header
 * (OPC_FIXED_HEADER_SIZE), Length=0, no reserve, no body. Used by Logout /
 * GetBasicInfo / GetDeviceInfo / Reset requests.
 *
 * Returns OPC_FIXED_HEADER_SIZE (8) on success, -1 on bad arg / small cap.
 */
ssize_t opc_empty_frame_build(uint8_t *frame, size_t cap,
                              uint8_t cmd_type, uint16_t req_id, uint16_t seq_no);

/*
 * Parse an OPC frame:
 *   - Requires at least the 8-byte fixed header; copies header fields into
 *     *hdr_out.
 *   - If the frame reaches the full 64-byte common header, *body_out (skippable
 *     with NULL) points at byte 64 and *body_len_out is frame_len - 64.
 *   - For an 8-byte empty-request frame, *body_out is NULL and *body_len_out 0.
 *
 * Returns 0 on success, -1 on insufficient input / argument error.
 */
int opc_frame_parse(const uint8_t *frame, size_t frame_len,
                    opc_header_t *hdr_out,
                    const uint8_t **body_out, size_t *body_len_out);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_FRAME_H */
