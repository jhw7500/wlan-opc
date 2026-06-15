#include "chan_encode.h"

#include "ids.h"   /* OPC_BAND_2_4GHZ / _5GHZ / _6GHZ */

/* The 2.4/5/6 GHz ranges below MUST stay in sync with nl80211_parse.c's
 * freq_to_channel(): the event path derives the channel there and encodes the
 * band here. freq_to_channel() returns 0 outside these ranges, so on the event
 * path channel>0 always implies a band match — a band byte of 0 only ever
 * arises from the link-readback fallback (see opc_assoc_chan_field).
 * Note the deliberate 2.4 GHz delta: freq_to_channel splits 2412-2472 + 2484,
 * while this uses the closed 2412-2484; only the link path can reach 2473-2483
 * (still 2.4 GHz space). */
uint16_t opc_chan_field(uint32_t freq_mhz, uint16_t ch)
{
    /* ch 0 = unknown; ch >255 cannot fit the 8-bit wire field and would
     * overflow into the band byte (e.g. corrupt link.json) — both → 0. */
    if (ch == 0 || ch > 0xFF) return 0;
    uint8_t band = 0;
    if (freq_mhz >= 2412 && freq_mhz <= 2484)      band = OPC_BAND_2_4GHZ;
    else if (freq_mhz >= 5000 && freq_mhz <= 5895) band = OPC_BAND_5GHZ;
    else if (freq_mhz >= 5955 && freq_mhz <= 7115) band = OPC_BAND_6GHZ;
    return (uint16_t)(((uint16_t)band << 8) | ch);
}

uint16_t opc_assoc_chan_field(uint32_t evt_freq, uint16_t evt_ch,
                              bool fb_valid, uint32_t fb_freq,
                              uint16_t fb_ch)
{
    /* Prefer the event's own channel; CONNECT often omits WIPHY_FREQ, so fall
     * back to a secondary source — but only when it is valid (a kernel query
     * that returned a freq, or an associated link readback). */
    uint16_t ch = opc_chan_field(evt_freq, evt_ch);
    if (ch == 0 && fb_valid) {
        /* Accept the fallback only when it yields a band-qualified channel. A
         * source may report a channel with no recognizable frequency (freq 0 /
         * band-gap) — a band byte of 0 means exactly that. Emit 0 rather than a
         * half-populated (band-less) field. */
        uint16_t fb_field = opc_chan_field(fb_freq, fb_ch);
        if ((fb_field >> 8) != 0)
            ch = fb_field;
    }
    return ch;
}
