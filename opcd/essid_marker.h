#ifndef WLAN_OPC_OPCD_ESSID_MARKER_H
#define WLAN_OPC_OPCD_ESSID_MARKER_H

#include <time.h>

/* ChangeIpAddress ESSID volatility (#161). The spec says the IP and the ESSID
 * set by ChangeIp both return to their config values after a power cycle. The
 * IP is applied at runtime (`ip addr`) and returns by itself; the ESSID is
 * applied by editing the persistent wpa_supplicant conf, so it needs a marker:
 *
 *   arm     — BEFORE the conf is edited, persist the original `ssid=` value
 *             (plus the ChangeIp list slot, a timestamp and the requested SSID
 *             as `target=` hex) in the marker file. Marker first, edit second:
 *             a power cut in between still leaves a marker, and a conf that
 *             was never edited is not touched.
 *   commit  — AFTER the apply succeeded, record the conf's current `ssid=`
 *             token in the marker as the value this chain applied.
 *   settle  — after a FAILED, refused or timed-out apply: the helper may have
 *             installed the new conf before failing, so re-read it. Unchanged
 *             conf: an uncommitted marker is removed; a committed chain stays
 *             but loses the failed request's target. Conf decoding to the
 *             requested SSID: that token is committed so -R can still revert.
 *             Any other value was written by someone else while the apply was
 *             blocked (a conf writer holding the package lock is the usual
 *             reason the helper times out): nothing is committed, an
 *             uncommitted marker goes, a committed chain loses its target.
 *             Residual: after the SIGKILL deadline the runner returns once the
 *             helper's shell is reaped, so a child already inside rename(2)
 *             can land the conf a moment after settle read it; the window is
 *             that one syscall and is not closed here.
 *   disarm  — remove an uncommitted marker without looking at the conf (a
 *             committed chain stays); settle is what the apply path uses.
 *   restore — at boot (`opcd -R`), before any other writer of the conf runs
 *             (wifi_init.sh's conf transforms as well as wpa_supplicant; the
 *             unit ordering is the package's — wlan-package#362 — and -R takes
 *             no conf writer lock): put the original value back into the
 *             conf's `ssid=` line and delete the marker — ONLY when the conf
 *             holds the committed applied value, or decodes (quoted or hex) to
 *             the requested SSID recorded at arm. Any other value (a later
 *             change by wifi.sh, a factory reset) drops the marker and leaves
 *             the conf untouched. Nothing else in the conf (SetRadioConfig's
 *             freq_list is persistent by design) is touched.
 *
 * A chain is the sequence of ChangeIp applies since the original was recorded.
 * arm keeps the first original while the conf holds a value the chain wrote —
 * the committed token, or the SSID it last requested when its commit never
 * ran (the same ownership test restore uses) — and then records that value as
 * applied; otherwise another writer changed the SSID in between and a new
 * chain starts from the conf's current value. Known limit: ownership is
 * decided by value, so restore and arm treat ANY conf token that equals the
 * committed value or decodes to the recorded target as this chain's — a later
 * writer storing the same SSID as the committed value (an operator making the
 * ChangeIp SSID permanent), any rendering (quoted or hex) of it, and the SSID
 * of a request whose commit/settle never ran even if the helper never
 * installed it — and such a value is reverted at boot. Those writers delete
 * the marker on the package side (wlan-package#362).
 *
 * The original is stored as the raw conf token (quoted text or hex), so no
 * SSID re-encoding can alter it. Pure file I/O, no platform dependency. */

#ifndef OPC_PATH_ESSID_MARKER
#define OPC_PATH_ESSID_MARKER   "/usr/local/opc/etc/changeip_essid.restore"
#endif
#ifndef OPC_PATH_WPA_CONF_MLAN0
#define OPC_PATH_WPA_CONF_MLAN0 "/etc/wpa_supplicant/wpa_supplicant-mlan0.conf"
#endif
#ifndef OPC_PATH_WPA_CONF_MLAN1
#define OPC_PATH_WPA_CONF_MLAN1 "/etc/wpa_supplicant/wpa_supplicant-mlan1.conf"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = marker durable, conf may now be edited. Negative errno = do NOT edit
 * the conf (no marker, or the conf does not hold exactly one `ssid=` line —
 * the apply script refuses a multi-network conf anyway). */
int opc_essid_marker_arm(const char *marker_path, const char *conf_path,
                         unsigned slot_no, time_t now, const char *requested_ssid);

/* 0 = the conf's current `ssid=` token is recorded as the applied value.
 * Negative errno = marker or conf unusable; the marker stays uncommitted, and
 * restore then reverts the conf only if it still decodes to the requested SSID
 * recorded at arm (otherwise it drops the marker without editing). */
int opc_essid_marker_commit(const char *marker_path, const char *conf_path);

/* 0 = an uncommitted marker was removed, or there was nothing to do (no
 * marker, or a committed chain that must stay). Negative errno = unusable
 * marker, left in place for restore to report. */
int opc_essid_marker_disarm(const char *marker_path);

/* After a failed apply. 0 = conf still holds the pre-apply value: uncommitted
 * marker removed / committed chain kept without the target; 1 = the conf now
 * decodes to the requested SSID and that token was committed as applied
 * (restorable at boot); 2 = the conf holds a value this chain did not request
 * (another writer): nothing committed, uncommitted marker removed / committed
 * chain kept without the target; negative errno = marker or conf unusable,
 * marker left as is. */
int opc_essid_marker_settle(const char *marker_path, const char *conf_path);

/* 1 = original restored and marker removed; 0 = nothing to restore (no marker,
 * or the conf already holds the original) and the marker is removed; 2 =
 * marker removed WITHOUT editing because the conf's `ssid=` is neither the
 * committed applied token nor the requested SSID recorded at arm; negative
 * errno = marker or conf unusable (both left untouched). */
int opc_essid_marker_restore(const char *marker_path, const char *conf_path);

/* The SSID a wpa_supplicant conf is configured to join (#193): its first
 * `ssid=` value, decoded — "quoted" literally, bare even-length hex as bytes
 * (the two forms the package writers produce). Anything else — P"..."
 * escapes, odd or non-hex digits, a NUL byte, an SSID longer than cap-1
 * bytes — yields an empty string rather than a wrong or truncated SSID.
 * 0 = `out` set (possibly empty); negative errno = conf unreadable, `out` "". */
int opc_wpa_conf_ssid(const char *conf_path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_ESSID_MARKER_H */
