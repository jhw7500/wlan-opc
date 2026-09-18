#ifndef WLAN_OPC_PROTOCOL_SCAN_CHLIST_H
#define WLAN_OPC_PROTOCOL_SCAN_CHLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rev1.01 SCAN Frequency Band / SCAN Channel List helpers — pure, host-testable,
 * shared by opcd (validation / apply) and vhlctl (argument encoding / dump).
 *
 * SCAN Frequency Band (2B): exactly one band per WLAN.
 * SCAN Channel List (64bit): one bit per channel of that band. The spec draws
 * two 32-bit rows — row A carries the 2.4 GHz and 5 GHz assignments (and, for
 * 6 GHz, ch 1..125), row B carries the 6 GHz ch 129..233 tail. */

#define OPC_SCAN_BAND_2_4GHZ  0x0001
#define OPC_SCAN_BAND_5GHZ    0x0002
#define OPC_SCAN_BAND_6GHZ    0x0006
#define OPC_SCAN_BAND_UNSET   0xFFFF
#define OPC_SCAN_CHLIST_LEN   8

/* Wire placement of the two rows — CONFIRMED by the vendor reply of
 * 2026-09-18 (inquiry Q1, §4.3.4/§4.3.8): the 32 bits at frame offset 312
 * (GetDeviceInfo) / 72 (SetRadioConfig) carry Bit31~Bit0 big-endian, and the
 * 32 bits at 316 / 76 carry Bit63~Bit32. So row A = wire bytes 0..3, row B =
 * bytes 4..7, exactly as the spec's worked example prints them.
 *
 * Decoding is strict: a 2.4/5 GHz list with any bit in row B is malformed and
 * rejected (0x0012). The earlier leniency — take row B when row A is empty —
 * was insurance against an unknown row order; with the order confirmed it only
 * hid a peer's bug behind an OK. */
#define OPC_SCAN_ROW_A_FIRST  1

#define OPC_SCAN_ROW_A  0
#define OPC_SCAN_ROW_B  1

/* A never-configured band field (zero-initialised state) reads as unset. */
static inline uint16_t opc_scan_band_or_unset(uint16_t band)
{
    return band == 0 ? OPC_SCAN_BAND_UNSET : band;
}

bool     opc_scan_band_known(uint16_t band);       /* 2.4 / 5 / 6 GHz */
bool     opc_scan_band_supported(uint16_t band);   /* 2.4 / 5 GHz (device) */

/* Channel number assigned to `bit` (0..31) of `row` for `band`; 0 if unassigned.
 * 2.4/5 GHz assign row A only. */
uint8_t  opc_scan_row_channel(uint16_t band, int row, int bit);
/* Row/bit of `ch` in `band`; false when the channel is not in the table. */
bool     opc_scan_bit_for_channel(uint16_t band, uint8_t ch, int *row, int *bit);
/* Center frequency of `ch` in `band` (MHz); 0 when not in the table. */
uint16_t opc_scan_channel_mhz(uint16_t band, uint8_t ch);

/* Raw list helpers — `list` is in wire byte order. */
uint32_t opc_scan_row_word(const uint8_t list[OPC_SCAN_CHLIST_LEN], int row);
void     opc_scan_list_set_channel(uint8_t list[OPC_SCAN_CHLIST_LEN], uint16_t band, uint8_t ch);
bool     opc_scan_list_empty(const uint8_t list[OPC_SCAN_CHLIST_LEN]);
/* Every set bit maps to an assigned channel of `band`. For 2.4/5 GHz the list
 * must sit in one row (A, or B when A is empty — lenient row order). */
bool     opc_scan_list_valid(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN]);
/* Migration helper for state PERSISTED before the row order was confirmed: a
 * 2.4/5 GHz list written by an older build may sit in row B, which the strict
 * decoder above now rejects and enumerates as zero channels. Moves such a list
 * to row A in place and returns true; returns false when nothing was moved
 * (already row A, empty, 6 GHz, both rows populated, or an unknown band).
 * Inbound frames are NOT normalized — they are rejected with 0x0012. */
bool     opc_scan_list_normalize_rows(uint16_t band, uint8_t list[OPC_SCAN_CHLIST_LEN]);
/* Channels selected by `list`, ascending bit order, into out[0..max). An empty
 * list selects the whole band table ("band only"). Returns the number of
 * channels selected (may exceed `max`; only `max` are stored). */
size_t   opc_scan_list_channels(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN],
                                uint8_t *out, size_t max);
/* Lowest selected channel as (center MHz, OPC channel field band<<8|ch) — the
 * GetDeviceInfo "configured frequency" source. 0/0 when the band is unset or
 * unknown, or the list selects nothing. */
void     opc_scan_derive_freq_ch(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN],
                                 uint16_t *freq_mhz, uint16_t *ch_field);

#endif /* WLAN_OPC_PROTOCOL_SCAN_CHLIST_H */
