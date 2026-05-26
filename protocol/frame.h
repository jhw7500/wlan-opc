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
 * Build a complete OPC frame (header + body) in `frame`.
 *
 * The 64-byte common header is zero-filled in the reserve area (bytes 8..63)
 * and the header fields are written big-endian per spec.
 *
 * `length_field` is the literal value placed in the header's Length field.
 * NOTE: the spec is internally inconsistent about how Length is computed
 * (see docs/proto-todo.md T3, T10, T11). Per-command helpers in commands.h
 * pass the spec-literal value for their command.
 *
 * Returns total frame size (OPC_HEADER_SIZE + body_len) on success,
 * or -1 on invalid argument / insufficient capacity.
 */
ssize_t opc_frame_build(uint8_t *frame, size_t cap,
                        uint8_t cmd_type, uint16_t req_id, uint16_t seq_no,
                        uint16_t length_field,
                        const uint8_t *body, size_t body_len);

/*
 * Parse an OPC frame:
 *   - Copies header fields into *hdr_out.
 *   - Sets *body_out (may be NULL to skip) to point inside `frame` (no copy).
 *   - Sets *body_len_out (may be NULL to skip) to the remaining bytes after
 *     the 64-byte header.
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
