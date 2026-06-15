#include "chan_encode.h"

#include "ids.h"   /* OPC_BAND_2_4GHZ / _5GHZ / _6GHZ */

/* The 2.4/5/6 GHz ranges below MUST stay in sync with nl80211_parse.c's
 * freq_to_channel(): the event path derives the channel there and encodes the
 * band here. freq_to_channel() returns 0 outside these ranges, so on the event
 * path channel>0 always implies a band match — a band byte of 0 only ever
 * arises from the link-readback fallback (see opc_assoc_chan_field). */
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
                              bool link_assoc, uint32_t link_freq,
                              uint16_t link_ch)
{
    /* Prefer the event's own channel; CONNECT often omits WIPHY_FREQ, so fall
     * back to the cached link readback — but only when it reports an active
     * association (an unassociated/stale readback must not leak a channel). */
    uint16_t ch = opc_chan_field(evt_freq, evt_ch);
    if (ch == 0 && link_assoc) {
        /* Accept the link readback only when it yields a band-qualified
         * channel. link.json parses freq and channel independently, so an
         * associated readback can carry a channel with no recognizable
         * frequency (freq 0 / band-gap) — a band byte of 0 means exactly that.
         * Emit 0 rather than a half-populated (band-less) field. */
        uint16_t link_field = opc_chan_field(link_freq, link_ch);
        if ((link_field >> 8) != 0)
            ch = link_field;
    }
    return ch;
}
