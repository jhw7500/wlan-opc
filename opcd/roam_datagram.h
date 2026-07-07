#ifndef OPCD_ROAM_DATAGRAM_H
#define OPCD_ROAM_DATAGRAM_H

#include <stdint.h>

#include "platform.h"   /* opcd_platform_evt_t */

/* Parse a "xx:xx:xx:xx:xx:xx" BSSID string into mac[6]. Returns 0 on success,
 * -1 on any malformed input (trailing garbage tolerated only after the 6th
 * octet). */
int roam_parse_bssid(const char *s, uint8_t mac[6]);

/* Decode one roam-notify JSON datagram (flat top-level object, per design
 * §9.1 WIRE CONTRACT) into a ROAMING platform event. `json` must be
 * NUL-terminated. Returns 0 and fills *evt on success; -1 if a mandatory field
 * (ap_mac / channel / freq) is missing or out of range, or the iface is not a
 * known WLAN index (drop silently). idx is derived from the iface field so the
 * on_platform_event single-STA guard can reject non-primary roams. */
int roam_datagram_to_evt(const char *json, opcd_platform_evt_t *evt);

#endif /* OPCD_ROAM_DATAGRAM_H */
