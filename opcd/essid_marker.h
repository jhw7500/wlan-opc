#ifndef WLAN_OPC_OPCD_ESSID_MARKER_H
#define WLAN_OPC_OPCD_ESSID_MARKER_H

#include <time.h>

/* ChangeIpAddress ESSID volatility (#161). The spec says the IP and the ESSID
 * set by ChangeIp both return to their config values after a power cycle. The
 * IP is applied at runtime (`ip addr`) and returns by itself; the ESSID is
 * applied by editing the persistent wpa_supplicant conf, so it needs a marker:
 *
 *   arm     — BEFORE the conf is edited, persist the original `ssid=` value
 *             (plus the ChangeIp list slot and a timestamp) in the marker file.
 *             Marker first, edit second: a power cut in between still leaves
 *             a marker, and a conf that was never edited is not touched.
 *   commit  — AFTER the apply succeeded, record the conf's current `ssid=`
 *             token in the marker as the value this chain applied.
 *   disarm  — after a FAILED or refused apply, remove a marker that commit
 *             never reached (a committed chain from an earlier successful
 *             ChangeIp stays).
 *   restore — at boot (`opcd -R`), before any other writer of the conf runs
 *             (wifi_init.sh's conf transforms as well as wpa_supplicant; the
 *             unit ordering is the package's, and -R takes no conf writer
 *             lock): put the original value back into the conf's `ssid=` line
 *             and delete the marker — but ONLY when the conf still holds the
 *             committed applied value. Any other value (a later change by
 *             wifi.sh, a factory reset, an apply interrupted before commit)
 *             drops the marker and leaves the conf untouched: this marker
 *             never reverts a change it did not make. Nothing else in the conf
 *             (SetRadioConfig's freq_list is persistent by design) is touched.
 *
 * A chain is the sequence of ChangeIp applies since the original was recorded.
 * arm keeps the first original only while the conf still holds the value the
 * chain committed; otherwise another writer changed the SSID in between and a
 * new chain starts from the conf's current value.
 *
 * The original is stored as the raw conf token (quoted text or hex), so no
 * SSID re-encoding can alter it. Pure file I/O, no platform dependency. */

#ifndef OPC_PATH_ESSID_MARKER
#define OPC_PATH_ESSID_MARKER   "/usr/local/opc/etc/changeip_essid.restore"
#endif
#ifndef OPC_PATH_WPA_CONF_MLAN0
#define OPC_PATH_WPA_CONF_MLAN0 "/etc/wpa_supplicant/wpa_supplicant-mlan0.conf"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = marker durable, conf may now be edited. Negative errno = do NOT edit
 * the conf (no marker, or the conf does not hold exactly one `ssid=` line —
 * the apply script refuses a multi-network conf anyway). */
int opc_essid_marker_arm(const char *marker_path, const char *conf_path,
                         unsigned slot_no, time_t now);

/* 0 = the conf's current `ssid=` token is recorded as the applied value.
 * Negative errno = marker or conf unusable; the marker stays uncommitted and
 * restore will then drop it without editing. */
int opc_essid_marker_commit(const char *marker_path, const char *conf_path);

/* 0 = an uncommitted marker was removed, or there was nothing to do (no
 * marker, or a committed chain that must stay). Negative errno = unusable
 * marker, left in place for restore to report. */
int opc_essid_marker_disarm(const char *marker_path);

/* 1 = original restored and marker removed; 0 = nothing to restore (no marker,
 * or the chain applied nothing — conf still holds the original) and the marker
 * is removed; 2 = marker removed WITHOUT editing because the conf's `ssid=` is
 * not the value this chain applied; negative errno = marker or conf unusable
 * (both left untouched). */
int opc_essid_marker_restore(const char *marker_path, const char *conf_path);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_ESSID_MARKER_H */
