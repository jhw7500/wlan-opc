#ifndef WLAN_OPC_OPCD_CHAN_ENCODE_H
#define WLAN_OPC_OPCD_CHAN_ENCODE_H

#include <stdbool.h>
#include <stdint.h>

/* OPC indication channel-field encoding — pure, host-testable, OPC-aware.
 *
 * Kept out of nl80211_parse.c (which stays OPC-agnostic) and out of the
 * cross-only platform_nxp.c (which is never compiled by `make check`) so the
 * band/channel encoding and the ASSOCIATED-channel fallback decision can be
 * unit-tested on the host. */

/* Encode the OPC indication channel field: upper byte = OPC band, lower byte =
 * channel number (protocol/indications.h: indication_ch / ch_number). The band
 * is derived from the frequency. Channel 0 (no association / unknown) maps to a
 * bare 0 with no band. */
uint16_t opc_chan_field(uint32_t freq_mhz, uint16_t ch);

/* Resolve the OPC channel field for an ASSOCIATED (CONNECT-success) event.
 * Prefer the event's own freq/channel; if it carries none (NL80211_CMD_CONNECT
 * commonly omits NL80211_ATTR_WIPHY_FREQ → freq/ch 0), fall back to the cached
 * link readback (link.json) — but only when that readback reports an active
 * association. Returns the encoded (band<<8 | ch) field, or 0 when neither
 * source yields a channel. Mirrors the best-effort SNR/RSSI seeding the ROAM
 * path already does via the link readback. */
uint16_t opc_assoc_chan_field(uint32_t evt_freq, uint16_t evt_ch,
                              bool link_assoc, uint32_t link_freq,
                              uint16_t link_ch);

#endif /* WLAN_OPC_OPCD_CHAN_ENCODE_H */
