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

/* Wire placement of the two rows. The spec does not say which row travels
 * first; its worked example prints the 2.4 GHz word first, so row A = wire
 * bytes 0..3 (big-endian) and row B = bytes 4..7. This is the single place the
 * assumption lives (customer inquiry Q1). Decoding of 2.4/5 GHz lists is
 * lenient: when row A is all-zero and row B is not, row B is taken — a peer
 * that swaps the rows still interoperates for the supported bands. */
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
