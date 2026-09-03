#include "scan_chlist.h"

#include <string.h>

/* ---- band tables --------------------------------------------------------- */

bool opc_scan_band_known(uint16_t band)
{
    return band == OPC_SCAN_BAND_2_4GHZ || band == OPC_SCAN_BAND_5GHZ ||
           band == OPC_SCAN_BAND_6GHZ;
}

bool opc_scan_band_supported(uint16_t band)
{
    return band == OPC_SCAN_BAND_2_4GHZ || band == OPC_SCAN_BAND_5GHZ;
}

uint8_t opc_scan_row_channel(uint16_t band, int row, int bit)
{
    if (bit < 0 || bit > 31) return 0;
    switch (band) {
    case OPC_SCAN_BAND_2_4GHZ:
        /* row A bit0..13 = ch1..14 */
        return (row == OPC_SCAN_ROW_A && bit <= 13) ? (uint8_t)(bit + 1) : 0;
    case OPC_SCAN_BAND_5GHZ:
        /* row A bit0..7 = 36..64 (4 step), bit8..19 = 100..144 (4 step),
         * bit20..24 = 149,153,157,161,165 */
        if (row != OPC_SCAN_ROW_A) return 0;
        if (bit <= 7)  return (uint8_t)(36 + 4 * bit);
        if (bit <= 19) return (uint8_t)(100 + 4 * (bit - 8));
        if (bit <= 24) return (uint8_t)(149 + 4 * (bit - 20));
        return 0;
    case OPC_SCAN_BAND_6GHZ:
        /* row A bit0..31 = 1..125 (1+4k), row B bit0..26 = 129..233 (129+4k) */
        if (row == OPC_SCAN_ROW_A) return (uint8_t)(1 + 4 * bit);
        if (row == OPC_SCAN_ROW_B && bit <= 26) return (uint8_t)(129 + 4 * bit);
        return 0;
    default:
        return 0;
    }
}

bool opc_scan_bit_for_channel(uint16_t band, uint8_t ch, int *row, int *bit)
{
    if (ch == 0 || !opc_scan_band_known(band)) return false;
    for (int r = OPC_SCAN_ROW_A; r <= OPC_SCAN_ROW_B; r++) {
        for (int b = 0; b < 32; b++) {
            if (opc_scan_row_channel(band, r, b) == ch) {
                if (row) *row = r;
                if (bit) *bit = b;
                return true;
            }
        }
    }
    return false;
}

uint16_t opc_scan_channel_mhz(uint16_t band, uint8_t ch)
{
    if (!opc_scan_bit_for_channel(band, ch, NULL, NULL)) return 0;
    switch (band) {
    case OPC_SCAN_BAND_2_4GHZ: return ch == 14 ? 2484 : (uint16_t)(2407 + 5 * ch);
    case OPC_SCAN_BAND_5GHZ:   return (uint16_t)(5000 + 5 * ch);
    case OPC_SCAN_BAND_6GHZ:   return (uint16_t)(5950 + 5 * ch);
    default:                   return 0;
    }
}

/* ---- raw list (wire byte order) ------------------------------------------ */

/* Byte offset of a row inside the 8-byte list — the only place the row-order
 * assumption (OPC_SCAN_ROW_A_FIRST) is consulted. */
static size_t row_offset(int row)
{
    int idx = OPC_SCAN_ROW_A_FIRST ? row : 1 - row;
    return (size_t)idx * 4;
}

uint32_t opc_scan_row_word(const uint8_t list[OPC_SCAN_CHLIST_LEN], int row)
{
    const uint8_t *p = &list[row_offset(row)];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void row_set_bit(uint8_t list[OPC_SCAN_CHLIST_LEN], int row, int bit)
{
    size_t off = row_offset(row) + 3 - (size_t)(bit / 8);   /* big-endian word */
    list[off] |= (uint8_t)(1u << (bit % 8));
}

void opc_scan_list_set_channel(uint8_t list[OPC_SCAN_CHLIST_LEN], uint16_t band, uint8_t ch)
{
    int row, bit;
    if (opc_scan_bit_for_channel(band, ch, &row, &bit)) row_set_bit(list, row, bit);
}

bool opc_scan_list_empty(const uint8_t list[OPC_SCAN_CHLIST_LEN])
{
    for (size_t i = 0; i < OPC_SCAN_CHLIST_LEN; i++)
        if (list[i] != 0) return false;
    return true;
}

/* Row that carries a 2.4/5 GHz list: A, or B when A is empty (lenient order).
 * -1 when both rows are populated — those bands define one row only. */
static int single_row(const uint8_t list[OPC_SCAN_CHLIST_LEN])
{
    uint32_t a = opc_scan_row_word(list, OPC_SCAN_ROW_A);
    uint32_t b = opc_scan_row_word(list, OPC_SCAN_ROW_B);
    if (a != 0 && b != 0) return -1;
    return (a != 0 || b == 0) ? OPC_SCAN_ROW_A : OPC_SCAN_ROW_B;
}

bool opc_scan_list_valid(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN])
{
    if (!opc_scan_band_known(band)) return opc_scan_list_empty(list);
    if (band == OPC_SCAN_BAND_6GHZ) {
        for (int r = OPC_SCAN_ROW_A; r <= OPC_SCAN_ROW_B; r++) {
            uint32_t w = opc_scan_row_word(list, r);
            for (int b = 0; b < 32; b++)
                if (((w >> b) & 1u) && opc_scan_row_channel(band, r, b) == 0) return false;
        }
        return true;
    }
    int row = single_row(list);
    if (row < 0) return false;
    uint32_t w = opc_scan_row_word(list, row);
    for (int b = 0; b < 32; b++)
        if (((w >> b) & 1u) && opc_scan_row_channel(band, OPC_SCAN_ROW_A, b) == 0) return false;
    return true;
}

size_t opc_scan_list_channels(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN],
                              uint8_t *out, size_t max)
{
    if (!opc_scan_band_known(band)) return 0;
    const bool all = opc_scan_list_empty(list);
    size_t count = 0;

    if (band == OPC_SCAN_BAND_6GHZ) {
        for (int r = OPC_SCAN_ROW_A; r <= OPC_SCAN_ROW_B; r++) {
            uint32_t w = opc_scan_row_word(list, r);
            for (int b = 0; b < 32; b++) {
                uint8_t ch = opc_scan_row_channel(band, r, b);
                if (ch == 0) continue;
                if (all || ((w >> b) & 1u)) {
                    if (count < max) out[count] = ch;
                    count++;
                }
            }
        }
        return count;
    }

    int row = single_row(list);
    if (row < 0) return 0;
    uint32_t w = opc_scan_row_word(list, row);
    for (int b = 0; b < 32; b++) {
        uint8_t ch = opc_scan_row_channel(band, OPC_SCAN_ROW_A, b);
        if (ch == 0) continue;
        if (all || ((w >> b) & 1u)) {
            if (count < max) out[count] = ch;
            count++;
        }
    }
    return count;
}

void opc_scan_derive_freq_ch(uint16_t band, const uint8_t list[OPC_SCAN_CHLIST_LEN],
                             uint16_t *freq_mhz, uint16_t *ch_field)
{
    if (freq_mhz) *freq_mhz = 0;
    if (ch_field) *ch_field = 0;
    if (!opc_scan_band_known(band) || opc_scan_list_empty(list)) return;
    uint8_t ch = 0;
    if (opc_scan_list_channels(band, list, &ch, 1) == 0) return;
    uint16_t mhz = opc_scan_channel_mhz(band, ch);
    if (mhz == 0) return;
    if (freq_mhz) *freq_mhz = mhz;
    if (ch_field) *ch_field = (uint16_t)(((band & 0xFFu) << 8) | ch);
}
