#ifndef WLAN_OPC_OPCD_FREQ_SOURCE_H
#define WLAN_OPC_OPCD_FREQ_SOURCE_H

/* device-info WLAN FREQ/CH source selector (opc.conf device_info_freq_source).
 *
 * Spec §3.3.4 defines these fields as the *configured* freq/CH ("설정 주파수"),
 * which is CONFIG — the shipping default, giving zero behavioral change. LIVE
 * and AUTO are opt-in deviations pending vendor confirmation (spec-inquiry G11).
 *
 * Kept as a standalone module (like chan_encode / nl80211_parse) so the pure
 * opc.conf key parsing is host-unit-testable without linking opcd.c's main(). */

typedef enum {
    OPC_FREQ_SRC_CONFIG = 0,  /* always the set-radio config value (spec) */
    OPC_FREQ_SRC_LIVE,        /* always the live associated value (0/0 if down) */
    OPC_FREQ_SRC_AUTO,        /* live when associated, config otherwise */
} opcd_freq_source_t;

/* Map an opc.conf value token to the enum. NULL or any unrecognized token
 * (including "") → OPC_FREQ_SRC_CONFIG (spec-strict fallback). Case-sensitive. */
opcd_freq_source_t opcd_freq_source_from_token(const char *val);

/* Parse opc.conf for `device_info_freq_source = config|live|auto` (last value
 * wins). Missing/unreadable file, absent key, or unrecognized value all yield
 * OPC_FREQ_SRC_CONFIG. Uses the same `key = value` line-scan as
 * opcd_fault_probe_conf(); '#'-comment and unrelated lines are skipped. */
opcd_freq_source_t opcd_freq_source_parse(const char *conf_path);

#endif /* WLAN_OPC_OPCD_FREQ_SOURCE_H */
