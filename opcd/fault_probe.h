#ifndef WLAN_OPC_OPCD_FAULT_PROBE_H
#define WLAN_OPC_OPCD_FAULT_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FaultDetect (0x0010) congestion probe — T6 INTERIM policy.
 *
 * Operator decision 2026-06-12; every figure below is provisional pending
 * the vendor inquiry tracked in issue #35 / proto-todo T6:
 *   - threshold: entry 80% for every resource, cleared at 70% — a
 *     hysteresis band, not one level (#141). Both overridable via opc.conf
 *     (congestion_threshold_pct / congestion_clear_pct / congestion_net_if /
 *      congestion_disk_dev / congestion_net_capacity_mbps)
 *   - sampling: on a DEVICE-INTERNAL interval (congestion_probe_interval_s,
 *     default 10 s), independent of the Indication Period (#121 — the spec
 *     leaves the resource watch period to the vendor, like Reset Cause)
 *   - notification: §4.3.9 state-change semantics — ONCE on congestion ENTRY
 *     (below→above threshold transition), per resource; a persisting
 *     congestion is not re-notified. Period 0 → immediate, Period ≥ 1 →
 *     staged in the coalesce slot and flushed at the period end (#105).
 *     The latch clears when utilisation drops below clear_pct, which is at
 *     or under threshold_pct — so a resource hovering at the entry level
 *     (79/81/79/81 ...) latches ONCE instead of re-entering on every sample
 *     (#141). Whether/how to notify the clear is inquiry Q6 — hook only,
 *     nothing emitted.
 *
 *     WIRE CONSEQUENCE of the band: a re-announced congestion (after
 *     SetIndicationConfig / logout / a failed Period-0 send) reports the
 *     utilisation measured at that moment, which for a resource resting
 *     inside the band is BELOW threshold_pct — as low as clear_pct. So a
 *     FaultDetect current_val of 75 with an 80% entry threshold is correct,
 *     not a contradiction: the resource is still congested by the band rule
 *     that governs release. Before #141 the same situation emitted no frame
 *     at all (reviewer A).
 *   - CPU (0x0001):  /proc/stat busy ratio over the interval, current_val = %
 *   - Memory (0x0002): NOT emitted — the target runs swapless, so the spec's
 *     paging-based definition cannot occur; flash pressure is covered by
 *     Disk I/O (0x0003)
 *   - Disk (0x0003): /proc/diskstats io_ticks utilisation, current_val = %
 *   - Network (0x0004): (rx+tx) rate vs link speed, threshold = 80% of the
 *     capacity, current_val = link utilisation % (DFK 2026-06-29: 사용률 0-100%;
 *     proto-todo T6 — was Mbps before the answer)
 *
 * Source paths are struct fields rather than literals so unit tests can
 * point the probe at synthetic files — the same temp-path pattern the store
 * tests use.
 */

#define OPCD_FAULT_THRESHOLD_PCT_DEFAULT 80
#define OPCD_FAULT_CLEAR_BAND_PCT        10   /* default band width below the
                                              * entry threshold (#141) */
#define OPCD_FAULT_CLEAR_PCT_DEFAULT \
    (OPCD_FAULT_THRESHOLD_PCT_DEFAULT - OPCD_FAULT_CLEAR_BAND_PCT)
#define OPCD_FAULT_NET_CAPACITY_DEFAULT  1000   /* Mbps, when sysfs speed is absent */
#define OPCD_FAULT_PROBE_INTERVAL_DEFAULT 10    /* s, device-internal watch period (#121) */
#define OPCD_FAULT_PROBE_INTERVAL_MAX     3600

typedef struct opcd_fault_probe {
    /* config */
    unsigned threshold_pct;         /* NG entry threshold, percent (1..100) */
    unsigned clear_pct;             /* band releases below this, percent (1..100).
                                     * Invariant clear_pct <= threshold_pct,
                                     * enforced by _conf() after parsing (keys
                                     * may appear in any order) and again in
                                     * fault_over(), so a hand-built probe
                                     * cannot latch forever. Set it EQUAL to
                                     * threshold_pct to turn the band off; there
                                     * is no sentinel value. When opc.conf does
                                     * not set it, _conf() re-derives it from
                                     * the configured threshold so lowering the
                                     * entry level never silently collapses the
                                     * band (reviewer A, #141). */
    unsigned net_capacity_mbps;     /* fallback when <net_dir>/speed unusable */
    char     disk_dev[33];          /* /proc/diskstats device name —
                                     * kernel DISK_NAME_LEN(32) + NUL */
    unsigned probe_interval_s;      /* watch period, 1..OPCD_FAULT_PROBE_INTERVAL_MAX */
    uint32_t probe_countdown_s;     /* seconds accumulated toward the next sample */
    /* Two flags per resource, separate since #141 — before the hysteresis band
     * they always coincided and one flag carried both meanings.
     *
     *   *_congested : BAND state. True while the resource counts as congested,
     *                 which with a band means "entered at threshold_pct and has
     *                 not yet fallen below clear_pct". fault_over() reads it.
     *   *_notified  : NOTIFY-ONCE latch (#121). True once an ENTRY has been
     *                 reported for the current congestion.
     *
     * opcd_fault_probe_rearm / _reset_latch clear ONLY *_notified. Clearing
     * *_congested too would tell the next sample the resource is not congested,
     * and a utilisation sitting inside [clear_pct, threshold_pct) would then be
     * neither an entry nor a clear — losing the one notification the spec
     * allows, which is exactly what rearm exists to prevent (reviewer A, #141). */
    bool     cpu_congested, disk_congested, net_congested;
    bool     cpu_notified,  disk_notified,  net_notified;
    /* source paths (overridable for tests) */
    char     path_proc_stat[96];
    char     path_diskstats[96];
    char     net_dir[96];           /* /sys/class/net/<if> */
    /* previous counters for delta computation. Each source carries its own
     * primed flag: a source that could not be read keeps (or drops back to)
     * un-primed, so the first readable sample re-establishes the baseline
     * instead of computing a since-boot delta — which would report a
     * spurious 100% congestion. */
    bool     primed;                /* first sample taken (nothing reported) */
    bool     cpu_primed, disk_primed, net_primed;
    uint64_t cpu_busy, cpu_total;
    uint64_t disk_io_ms;
    uint64_t net_bytes;             /* rx + tx */
    uint64_t mono_ms;               /* CLOCK_MONOTONIC of the last sample */
} opcd_fault_probe_t;

typedef struct opcd_fault_report {
    bool     cpu_over;  uint16_t cpu_pct;
    bool     disk_over; uint16_t disk_pct;
    bool     net_over;  uint16_t net_pct;
    /* Transitions since the previous sample (#121). Both are derived from the
     * NOTIFY latch, not from the band state: entered = the resource counts as
     * congested and no entry has been announced yet; cleared = it no longer
     * counts as congested and an entry HAD been announced. The difference
     * shows only inside a rearm/reset window, where the notify latch is open
     * while the band state still stands — a release there reports neither
     * transition, which is right because there is no announced entry left to
     * withdraw (opcd_ind_fault_clear's only job). cleared stays a Q6 hook:
     * nothing is emitted. */
    bool     cpu_entered,  disk_entered,  net_entered;
    bool     cpu_cleared,  disk_cleared,  net_cleared;
} opcd_fault_report_t;

/* Defaults: 80% threshold, mmcblk0, eth0, real /proc and /sys paths. */
void opcd_fault_probe_init(opcd_fault_probe_t *p);

/* Minimal key=value reader for the congestion_* keys in opc.conf. A missing
 * file or key leaves the defaults; never fails. */
void opcd_fault_probe_conf(opcd_fault_probe_t *p, const char *conf_path);

/* Advance the device-internal watch countdown by `elapsed_s`; true when a
 * sample is due (once per interval — a long stall clamps to a single due,
 * mirroring the indication tick). */
bool opcd_fault_probe_due(opcd_fault_probe_t *p, uint32_t elapsed_s);

/* Forget the notify-once latch so an ONGOING congestion is reported as a fresh
 * entry on the next sample — for a new indication recipient / config
 * (opcd_ind_coalesce_reset). Counter baselines AND the band state are kept, so
 * a resource resting inside [clear_pct, threshold_pct) is still congested and
 * is re-announced rather than silently dropped (#141). */
void opcd_fault_probe_reset_latch(opcd_fault_probe_t *p);

/* Re-arm ONE resource's entry latch (D4(i), 2026-09-18). The latch is committed
 * inside opcd_fault_probe_sample(), i.e. before the caller has tried to send the
 * notification; when that send fails at Indication Period 0 there is no staging
 * buffer to retry from, so the "notify once on entry" rule would silently become
 * "never notify". Re-arming makes the next due sample report the still-standing
 * congestion as a fresh ENTRY — including a congestion that has since settled
 * inside the hysteresis band, which is why only the notify latch is cleared
 * and the band state is left alone (#141). The resource is named by this module's own enum
 * so the probe stays free of protocol headers (it is host-unit-tested alone);
 * indication.c maps OPC_CONGESTION_* onto it. */
typedef enum {
    OPCD_FAULT_RES_CPU = 0,
    OPCD_FAULT_RES_DISK,
    OPCD_FAULT_RES_NET,
} opcd_fault_res_t;

void opcd_fault_probe_rearm(opcd_fault_probe_t *p, opcd_fault_res_t res);

/* Sample the sources and evaluate utilisation since the previous call.
 * The first call only primes the counters (*out zeroed, returns 0). An
 * unreadable source leaves its resource un-flagged. */
int  opcd_fault_probe_sample(opcd_fault_probe_t *p, opcd_fault_report_t *out);

/* Pure helpers, unit-tested directly. opcd_fault_evaluate expects
 * elapsed_ms >= 1 (the sampler enforces a 1 ms floor); 0 silently skips the
 * disk and network calculations. It reads the entry latches (*_congested) as
 * well as the thresholds, so *_over is the HYSTERETIC state ("considered
 * congested"), not a bare sample-vs-entry-level comparison (#141). */
int  opcd_fault_parse_proc_stat(const char *text, uint64_t *busy, uint64_t *total);
int  opcd_fault_parse_diskstats(const char *text, const char *dev, uint64_t *io_ms);
void opcd_fault_evaluate(const opcd_fault_probe_t *p,
                         uint64_t d_busy, uint64_t d_total,
                         uint64_t d_disk_ms, uint64_t elapsed_ms,
                         uint64_t d_net_bytes, unsigned link_mbps,
                         opcd_fault_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_FAULT_PROBE_H */
