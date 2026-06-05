/*
 * fielddump — field-level hex disassembly for vhlctl --hex.
 *
 * Cumulative-offset model: each table lists fields by length only; the dump
 * walker accumulates the running offset from a start point. A wire field whose
 * length changes needs a single `len` edit — following fields shift on their
 * own. Reserve/pad gaps are explicit FD_HEX rows so the running offset stays
 * locked to protocol/commands.c and indications.c. The vhlctl unit tests pack
 * real frames as ground truth, so any drift between these tables and the codec
 * fails `make check`.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "fielddump.h"
#include "codec.h"   /* opc_be16_read / opc_be32_read */
#include "ids.h"
#include "proto.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* "hh hh hh" for up to 16 bytes; trailing " ..." when the field is longer. */
static void build_hex(char *out, size_t cap, const uint8_t *p, size_t len)
{
    size_t shown = (len < 16) ? len : 16;
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < shown && o + 4 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%02x ", p[i]);
    if (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
    if (len > shown && o + 5 < cap) snprintf(out + o, cap - o, " ...");
}

/* Decode the field value per kind into `dec` (empty for FD_HEX). */
static void build_decoded(char *dec, size_t cap, const uint8_t *p, const fd_field_t *f)
{
    dec[0] = '\0';
    switch (f->kind) {
    case FD_HEX:
        break;
    case FD_U8:
        snprintf(dec, cap, "%u (0x%02x)", p[0], p[0]);
        break;
    case FD_I8:
        snprintf(dec, cap, "%d", (int)(int8_t)p[0]);
        break;
    case FD_U16BE: {
        uint16_t v = opc_be16_read(p);
        snprintf(dec, cap, "0x%04x (%u)", v, v);
        break;
    }
    case FD_U32BE:
        snprintf(dec, cap, "0x%08x", opc_be32_read(p));
        break;
    case FD_MAC:
        snprintf(dec, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                 p[0], p[1], p[2], p[3], p[4], p[5]);
        break;
    case FD_STR: {
        char tmp[64];
        size_t m = (f->len < sizeof tmp - 1) ? f->len : sizeof tmp - 1;
        size_t k = 0;
        for (size_t i = 0; i < m; i++) {
            char c = (char)p[i];
            if (c == '\0') break;
            tmp[k++] = (c >= 0x20 && c < 0x7f) ? c : '.';
        }
        tmp[k] = '\0';
        snprintf(dec, cap, "'%s'", tmp);
        break;
    }
    case FD_DATE:
        snprintf(dec, cap, "%04u-%02u-%02u", opc_be16_read(p), p[2], p[3]);
        break;
    case FD_IPV4:
        snprintf(dec, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
        break;
    }
}

void fd_render(char *out, size_t outcap,
               const uint8_t *frame, size_t frame_len, size_t off, const fd_field_t *f)
{
    if (!outcap) return;
    out[0] = '\0';
    if (off + f->len > frame_len) {
        snprintf(out, outcap, "  %-18s @%03zu (%2zuB): (truncated)",
                 f->label, off, f->len);
        return;
    }
    const uint8_t *p = frame + off;
    char hex[80];
    char dec[80];
    build_hex(hex, sizeof hex, p, f->len);
    build_decoded(dec, sizeof dec, p, f);
    if (dec[0])
        snprintf(out, outcap, "  %-18s @%03zu (%2zuB): %s = %s",
                 f->label, off, f->len, hex, dec);
    else
        snprintf(out, outcap, "  %-18s @%03zu (%2zuB): %s",
                 f->label, off, f->len, hex);
}

/* ---- descriptor tables (length-only; offsets accumulate at dump time) ---- */

/* Common 60-byte header — first 8 bytes are meaningful, 8..59 reserve. */
static const fd_field_t HDR[] = {
    {"protocol_ver", 1, FD_U8},
    {"command_type", 1, FD_U8},
    {"req_id",       2, FD_U16BE},
    {"sequence",     2, FD_U16BE},
    {"length",       2, FD_U16BE},
};

/* GetBasicInfo Ack body (commands.h: 16 B). */
static const fd_field_t BASIC[] = {
    {"vendor_code",     4, FD_U32BE},
    {"product_code",    2, FD_U16BE},
    {"product_subcode", 2, FD_U16BE},
    {"device_status",   4, FD_U32BE},
    {"station_type",    2, FD_U16BE},
};

/* GetDeviceInfo Ack body (commands.h offset table; reserve/pad explicit). */
static const fd_field_t DEVINFO[] = {
    {"result",           2, FD_U16BE},
    {"error_cause",      2, FD_U16BE},
    {"vendor_code",      4, FD_U32BE},
    {"product_code",     2, FD_U16BE},
    {"product_subcode",  2, FD_U16BE},
    {"manufacture",      4, FD_DATE},
    {"shipment",         4, FD_DATE},
    {"firmware_version",32, FD_STR},
    {"hardware_version",32, FD_STR},
    {"serial_number",   32, FD_STR},
    {"ethernet_mac",     6, FD_MAC},
    {"(reserve)",        2, FD_HEX},
    {"ip_address",       4, FD_IPV4},
    {"subnet_mask",      4, FD_IPV4},
    {"default_gateway",  4, FD_IPV4},
    {"ntp_server",       4, FD_IPV4},
    {"essid",           32, FD_STR},
    {"device_status",    4, FD_U32BE},
    {"station_type",     2, FD_U16BE},
    {"priority_ch",      2, FD_U16BE},
    {"ieee_11r",         1, FD_U8},
    {"ieee_11ai",        1, FD_U8},
    {"ieee_11k",         1, FD_U8},
    {"ieee_11v",         1, FD_U8},
    {"(reserve)",       36, FD_HEX},
    {"wlan1.mac",        6, FD_MAC},
    {"wlan1.mode",       1, FD_U8},
    {"wlan1.bandwidth",  1, FD_U8},
    {"wlan1.freq_mhz",   2, FD_U16BE},
    {"wlan1.channel",    2, FD_U16BE},
    {"wlan1.status",     2, FD_U16BE},
    {"wlan1.snr",        1, FD_I8},
    {"wlan1.rssi",       1, FD_I8},
    {"wlan1.ap_mac",     6, FD_MAC},
    {"(pad)",            2, FD_HEX},
    {"(reserve)",       36, FD_HEX},
    {"wlan2.mac",        6, FD_MAC},
    {"wlan2.mode",       1, FD_U8},
    {"wlan2.bandwidth",  1, FD_U8},
    {"wlan2.freq_mhz",   2, FD_U16BE},
    {"wlan2.channel",    2, FD_U16BE},
    {"wlan2.status",     2, FD_U16BE},
    {"wlan2.snr",        1, FD_I8},
    {"wlan2.rssi",       1, FD_I8},
    {"wlan2.ap_mac",     6, FD_MAC},
};

/* Indication bodies (indications.c). */
static const fd_field_t IND_INIT[]      = {{"status", 4, FD_U32BE}};
static const fd_field_t IND_WLANSTAT[]  = {{"wlan_status", 2, FD_U16BE},
                                           {"indication_ch", 2, FD_U16BE}};
static const fd_field_t IND_ROAMING[]   = {{"current_snr", 1, FD_I8},
                                           {"current_rssi", 1, FD_I8},
                                           {"(reserve)", 2, FD_HEX},
                                           {"connect_ap_mac", 6, FD_MAC},
                                           {"ch_number", 2, FD_U16BE}};
static const fd_field_t IND_APDISC[]    = {{"message_id", 2, FD_U16BE},
                                           {"result_code", 2, FD_U16BE},
                                           {"disconnect_ap_mac", 6, FD_MAC},
                                           {"(reserve)", 2, FD_HEX}};
static const fd_field_t IND_FAULT[]     = {{"congestion_id", 2, FD_U16BE},
                                           {"current_val", 2, FD_U16BE}};
static const fd_field_t IND_RESET[]     = {{"reset_cause", 4, FD_U32BE}};
static const fd_field_t IND_KEEPALIVE[] = {{"timestamp", 32, FD_STR}};

/* ---- table walker: accumulates the offset across fields ---- */

static void dump_table(FILE *fp, const uint8_t *frame, size_t len,
                       size_t start_off, const fd_field_t *t, size_t n)
{
    char line[160];
    size_t off = start_off;
    for (size_t i = 0; i < n; i++) {
        fd_render(line, sizeof line, frame, len, off, &t[i]);
        fprintf(fp, "%s\n", line);
        off += t[i].len;
    }
}

static void dump_header(FILE *fp, const uint8_t *frame, size_t len)
{
    dump_table(fp, frame, len, 0, HDR, ARRAY_SIZE(HDR));
}

void fd_dump_basic_info(FILE *fp, const uint8_t *frame, size_t frame_len)
{
    dump_header(fp, frame, frame_len);
    dump_table(fp, frame, frame_len, OPC_HEADER_SIZE, BASIC, ARRAY_SIZE(BASIC));
}

void fd_dump_device_info(FILE *fp, const uint8_t *frame, size_t frame_len)
{
    dump_header(fp, frame, frame_len);
    dump_table(fp, frame, frame_len, OPC_HEADER_SIZE, DEVINFO, ARRAY_SIZE(DEVINFO));
}

void fd_dump_indication(FILE *fp, uint16_t ind_id, const uint8_t *frame, size_t frame_len)
{
    const fd_field_t *t = NULL;
    size_t n = 0;
    switch (ind_id) {
    case OPC_IND_INIT_COMPLETE:      t = IND_INIT;      n = ARRAY_SIZE(IND_INIT);      break;
    case OPC_IND_WLAN_STATUS_CHANGE: t = IND_WLANSTAT;  n = ARRAY_SIZE(IND_WLANSTAT);  break;
    case OPC_IND_ROAMING:            t = IND_ROAMING;   n = ARRAY_SIZE(IND_ROAMING);   break;
    case OPC_IND_AP_DISCONNECT:      t = IND_APDISC;    n = ARRAY_SIZE(IND_APDISC);    break;
    case OPC_IND_FAULT_DETECT:       t = IND_FAULT;     n = ARRAY_SIZE(IND_FAULT);     break;
    case OPC_IND_RESET_NOTICE:       t = IND_RESET;     n = ARRAY_SIZE(IND_RESET);     break;
    case OPC_IND_KEEP_ALIVE:         t = IND_KEEPALIVE; n = ARRAY_SIZE(IND_KEEPALIVE); break;
    default: break;
    }
    dump_header(fp, frame, frame_len);
    if (t) dump_table(fp, frame, frame_len, OPC_HEADER_SIZE, t, n);
}
