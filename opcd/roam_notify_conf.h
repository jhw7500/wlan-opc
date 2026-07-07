#ifndef WLAN_OPC_OPCD_ROAM_NOTIFY_CONF_H
#define WLAN_OPC_OPCD_ROAM_NOTIFY_CONF_H

#include <stdint.h>

/* opc.conf `roam_notify_port` parser — the loopback UDP port opcd listens on
 * for roam-notify datagrams from the WLAN roaming executor (wifi_roam.py /
 * passive_roam.py). Default OPC_DEFAULT_ROAM_NOTIFY_PORT (50608) must stay in
 * lockstep with the python sender's default (design §9.3).
 *
 * Kept as a standalone module (like freq_source / chan_encode) so the pure
 * opc.conf key parsing is host-unit-testable without linking opcd.c's main(). */

/* Parse opc.conf for `roam_notify_port = <1..65535>` (last value wins).
 * Missing/unreadable file, absent key, or an out-of-range / non-numeric value
 * all yield `dflt`. Uses the same `key = value` line-scan as
 * opcd_freq_source_parse(); '#'-comment and unrelated lines are skipped. */
uint16_t opcd_roam_notify_port_parse(const char *conf_path, uint16_t dflt);

#endif /* WLAN_OPC_OPCD_ROAM_NOTIFY_CONF_H */
