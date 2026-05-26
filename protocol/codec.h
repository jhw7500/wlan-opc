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
 * opc_header_pack:
 *   Writes OPC_HEADER_SIZE bytes into buf with the supplied header values and
 *   zero-fills the reserve area. Returns 0 on success, -1 on null arguments.
 *
 * opc_header_unpack:
 *   Reads OPC_HEADER_SIZE bytes from buf and fills *hdr. Reserve bytes are
 *   not surfaced. Returns 0 on success, -1 on insufficient input.
 */
int opc_header_pack(uint8_t *buf, const opc_header_t *hdr);
int opc_header_unpack(const uint8_t *buf, size_t buf_len, opc_header_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_CODEC_H */
