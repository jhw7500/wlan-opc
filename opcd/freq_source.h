#ifndef WLAN_OPC_OPCD_FREQ_SOURCE_H
#define WLAN_OPC_OPCD_FREQ_SOURCE_H

/* device-info WLAN FREQ/CH source selector (opc.conf device_info_freq_source).
 *
 * Rev1.01 §4.3.4 (2026-08-06) defines WLAN#1/2 FREQ/CH as the frequency the
 * station is *associated on* ("액세스 포인트 접속 시의 주파수"), unset (0xFFFF)
 * while not associated — that is LIVE, the shipping default since #103 (the
 * Rev1.00 reading "configured frequency" stays as the legacy CONFIG option;
 * G11 was answered by the revision itself). LIVE is enum value 0 so that a
 * zero-initialised state already selects it.
 *
 * Kept as a standalone module (like chan_encode / nl80211_parse) so the pure
 * opc.conf key parsing is host-unit-testable without linking opcd.c's main(). */

typedef enum {
    OPC_FREQ_SRC_LIVE = 0,    /* associated value; 0xFFFF/0xFFFF while down (Rev1.01 default) */
    OPC_FREQ_SRC_AUTO,        /* live when associated, configured value otherwise */
    OPC_FREQ_SRC_CONFIG,      /* legacy Rev1.00 reading: the SetRadioConfig-derived value */
} opcd_freq_source_t;

/* Map an opc.conf value token to the enum. NULL or any unrecognized token
 * (including "") → OPC_FREQ_SRC_LIVE (Rev1.01 default). Case-sensitive. */
opcd_freq_source_t opcd_freq_source_from_token(const char *val);

/* Parse opc.conf for `device_info_freq_source = live|auto|config` (last value
 * wins). Missing/unreadable file, absent key, or unrecognized value all yield
 * OPC_FREQ_SRC_LIVE. Uses the same `key = value` line-scan as
 * opcd_fault_probe_conf(); '#'-comment and unrelated lines are skipped. */
opcd_freq_source_t opcd_freq_source_parse(const char *conf_path);

#endif /* WLAN_OPC_OPCD_FREQ_SOURCE_H */
