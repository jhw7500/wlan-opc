#ifndef WLAN_OPC_CODEC_H
#define WLAN_OPC_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"
#include "ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Common header in host representation. Wire form is fixed at OPC_HEADER_SIZE
 * bytes (big-endian), reserve bytes are always transmitted as zero.
 */
typedef struct opc_header {
    uint8_t  protocol_version;
    uint8_t  command_type;
    uint16_t req_indication_id;
    uint16_t sequence_number;
    uint16_t length;
} opc_header_t;

/* --- big-endian primitives --- */
uint16_t opc_be16_read(const uint8_t *p);
uint32_t opc_be32_read(const uint8_t *p);
void     opc_be16_write(uint8_t *p, uint16_t v);
void     opc_be32_write(uint8_t *p, uint32_t v);

/* --- header codec ---
 *
 * opc_fixed_header_pack / opc_fixed_header_unpack:
 *   The 8-byte fixed part only (OPC_FIXED_HEADER_SIZE) — ver, type, id, seq,
 *   length. Used for empty-body request frames, which are exactly 8 bytes.
 *
 * opc_header_pack:
 *   Writes the full OPC_HEADER_SIZE (64) common header: the 8-byte fixed part
 *   plus a zero-filled 56-byte reserve. Returns 0 on success, -1 on null args.
 *
 * opc_header_unpack:
 *   Reads a full OPC_HEADER_SIZE (64) common header from buf and fills *hdr.
 *   Reserve bytes are not surfaced. Returns 0 on success, -1 on short input.
 */
int opc_fixed_header_pack(uint8_t *buf, const opc_header_t *hdr);
int opc_fixed_header_unpack(const uint8_t *buf, size_t buf_len, opc_header_t *hdr);
int opc_header_pack(uint8_t *buf, const opc_header_t *hdr);
int opc_header_unpack(const uint8_t *buf, size_t buf_len, opc_header_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_CODEC_H */
