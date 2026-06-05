#ifndef WLAN_OPC_VHLCTL_FIELDDUMP_H
#define WLAN_OPC_VHLCTL_FIELDDUMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Field-level hex disassembly for vhlctl --hex.
 *
 * A response is described by a table of fd_field_t entries carrying only a
 * label, byte length, and decode kind — NOT an absolute offset. The dump
 * walker assigns offsets cumulatively from a start offset (0 for the header,
 * OPC_HEADER_SIZE for the body), so when a wire field's length changes you
 * edit a single `len` and every following field shifts automatically.
 * Reserve/pad gaps are listed as explicit FD_HEX rows to keep the running
 * offset aligned with the codec in protocol/commands.c and indications.c.
 *
 * fd_render formats one field as:
 *   "label  @OFF (NB): hh hh .. = decoded"
 */

typedef enum {
    FD_HEX,      /* raw bytes only (reserve / pad / opaque)      */
    FD_U8,       /* unsigned 8-bit       -> %u (+0x)             */
    FD_I8,       /* signed 8-bit         -> %d  (SNR/RSSI)       */
    FD_U16BE,    /* big-endian u16       -> 0x%04x (%u)          */
    FD_U32BE,    /* big-endian u32       -> 0x%08x               */
    FD_MAC,      /* 6 bytes              -> xx:xx:xx:xx:xx:xx    */
    FD_STR,      /* NUL-terminated ascii -> 'text'               */
    FD_DATE,     /* year(be16)+month(u8)+day(u8) -> YYYY-MM-DD   */
    FD_IPV4      /* 4 bytes              -> a.b.c.d              */
} fd_kind_t;

typedef struct {
    const char *label;
    size_t      len;
    fd_kind_t   kind;
} fd_field_t;

/* Render one field at absolute offset `off` into `out` (always NUL-terminated).
 * Bounds-checked: if off+len exceeds frame_len the decoded part reads
 * "(truncated)". */
void fd_render(char *out, size_t outcap,
               const uint8_t *frame, size_t frame_len, size_t off, const fd_field_t *f);

/* Per-response disassembly (common header + body) written to fp. */
void fd_dump_basic_info (FILE *fp, const uint8_t *frame, size_t frame_len);
void fd_dump_device_info(FILE *fp, const uint8_t *frame, size_t frame_len);
void fd_dump_indication (FILE *fp, uint16_t ind_id, const uint8_t *frame, size_t frame_len);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_VHLCTL_FIELDDUMP_H */
