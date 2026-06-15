/*
 * Host-side unit tests for the pure OPC channel-field encoder
 * (opcd/chan_encode.{c,h}). No sockets, no platform backend — exercises the
 * band/channel encoding and the ASSOCIATED-channel fallback decision directly.
 * Mirrors the ASSERT(cond,label) style of test_nl80211_parse.c: PASS/FAIL per
 * check, failure counter, nonzero exit on any failure.
 */

#include <stdint.h>
#include <stdio.h>

#include "../chan_encode.h"
#include "ids.h"   /* OPC_BAND_2_4GHZ / _5GHZ / _6GHZ (-I../../protocol) */

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

#define CH(band, ch) ((uint16_t)(((uint16_t)(band) << 8) | (uint16_t)(ch)))

int main(void)
{
    /* ---- opc_chan_field: band derivation + (band<<8|ch) packing ---------- */

    /* Channel 0 means "no association / unknown" → bare 0, band suppressed. */
    ASSERT(opc_chan_field(5180, 0) == 0, "chan_field: ch 0 → 0");
    ASSERT(opc_chan_field(0, 0)    == 0, "chan_field: freq+ch 0 → 0");

    /* 2.4 GHz band (2412..2484). */
    ASSERT(opc_chan_field(2412, 1)  == CH(OPC_BAND_2_4GHZ, 1),  "chan_field: 2412 → 2.4G ch1");
    ASSERT(opc_chan_field(2484, 14) == CH(OPC_BAND_2_4GHZ, 14), "chan_field: 2484 → 2.4G ch14");

    /* 5 GHz band (5000..5895). */
    ASSERT(opc_chan_field(5180, 36)  == CH(OPC_BAND_5GHZ, 36),  "chan_field: 5180 → 5G ch36");
    ASSERT(opc_chan_field(5895, 179) == CH(OPC_BAND_5GHZ, 179), "chan_field: 5895 → 5G ch179");

    /* 6 GHz band (5955..7115). */
    ASSERT(opc_chan_field(5955, 1)   == CH(OPC_BAND_6GHZ, 1),   "chan_field: 5955 → 6G ch1");
    ASSERT(opc_chan_field(7115, 233) == CH(OPC_BAND_6GHZ, 233), "chan_field: 7115 → 6G ch233");

    /* Frequency outside every known band → band 0, channel preserved. */
    ASSERT(opc_chan_field(3000, 50) == CH(0, 50), "chan_field: unknown band → ch only");

    /* 5 GHz lower edge (symmetry with the 2.4/6 GHz edges above). */
    ASSERT(opc_chan_field(5000, 1) == CH(OPC_BAND_5GHZ, 1), "chan_field: 5000 → 5G ch1 (lower edge)");

    /* Off-by-one just outside the 2.4 GHz band → no band (band byte 0). */
    ASSERT(opc_chan_field(2411, 1) == CH(0, 1), "chan_field: 2411 (below 2.4G) → no band");
    ASSERT(opc_chan_field(2485, 1) == CH(0, 1), "chan_field: 2485 (above 2.4G) → no band");

    /* Channel must fit the 8-bit wire field; a >255 value (e.g. corrupt
     * link.json) would overflow into the band byte — reject defensively. */
    ASSERT(opc_chan_field(2412, 256) == 0, "chan_field: ch >255 → 0 (overflow guard)");
    ASSERT(opc_chan_field(0, 256) == 0, "chan_field: band-less ch >255 → 0");

    /* ---- opc_assoc_chan_field: event-preferred, link fallback ------------ */

    /* Event carries its own freq/channel → use it, ignore the link readback. */
    ASSERT(opc_assoc_chan_field(5180, 36, true, 2412, 1) == CH(OPC_BAND_5GHZ, 36),
           "assoc: event freq wins over link");
    ASSERT(opc_assoc_chan_field(2412, 1, false, 0, 0) == CH(OPC_BAND_2_4GHZ, 1),
           "assoc: event freq used, no link needed");

    /* Event has no freq (CONNECT omitted WIPHY_FREQ) + associated link →
     * fall back to the link's channel. */
    ASSERT(opc_assoc_chan_field(0, 0, true, 5180, 36) == CH(OPC_BAND_5GHZ, 36),
           "assoc: empty event → link fallback");

    /* Event has no freq + link reports NOT associated → stay 0 (no stale
     * channel from a prior session). */
    ASSERT(opc_assoc_chan_field(0, 0, false, 5180, 36) == 0,
           "assoc: empty event + unassociated link → 0");

    /* Event has no freq + associated link but link channel also 0 → 0. */
    ASSERT(opc_assoc_chan_field(0, 0, true, 5180, 0) == 0,
           "assoc: empty event + link ch 0 → 0");

    /* Everything empty → 0. */
    ASSERT(opc_assoc_chan_field(0, 0, true, 0, 0) == 0,
           "assoc: all empty → 0");

    /* Link fallback must be band-qualified. nxp_get_link parses link.json's
     * freq and channel independently, so an associated readback can carry a
     * channel with no recognizable frequency (freq 0, or a band-gap freq like
     * 5896..5954). The event path can never produce this (its channel is
     * derived from freq via freq_to_channel), so reject the band-less result
     * rather than emit a half-populated field (band byte 0 + channel). */
    ASSERT(opc_assoc_chan_field(0, 0, true, 0, 36) == 0,
           "assoc: link ch without freq → 0 (no band)");
    ASSERT(opc_assoc_chan_field(0, 0, true, 5900, 40) == 0,
           "assoc: link band-gap freq (5896-5954) → 0");

    /* Corrupt link channel (>255) must not overflow into the band byte and be
     * mistaken for a valid band-qualified fallback. */
    ASSERT(opc_assoc_chan_field(0, 0, true, 0, 256) == 0,
           "assoc: link overflow ch (>255) → 0");

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all chan_encode tests passed\n");
    return 0;
}
