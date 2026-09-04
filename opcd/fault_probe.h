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
 *   - threshold: 80% for every resource, overridable via opc.conf
 *     (congestion_threshold_pct / congestion_net_if / congestion_disk_dev /
 *      congestion_net_capacity_mbps)
 *   - sampling: on a DEVICE-INTERNAL interval (congestion_probe_interval_s,
 *     default 10 s), independent of the Indication Period (#121 — the spec
 *     leaves the resource watch period to the vendor, like Reset Cause)
 *   - notification: §4.3.9 state-change semantics — ONCE on congestion ENTRY
 *     (below→above threshold transition), per resource; a persisting
 *     congestion is not re-notified. Period 0 → immediate, Period ≥ 1 →
 *     staged in the coalesce slot and flushed at the period end (#105).
 *     A drop below threshold clears the latch; whether/how to notify the
 *     clear is inquiry Q6 — hook only, nothing emitted.
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
#define OPCD_FAULT_NET_CAPACITY_DEFAULT  1000   /* Mbps, when sysfs speed is absent */
#define OPCD_FAULT_PROBE_INTERVAL_DEFAULT 10    /* s, device-internal watch period (#121) */
#define OPCD_FAULT_PROBE_INTERVAL_MAX     3600

typedef struct opcd_fault_probe {
    /* config */
    unsigned threshold_pct;         /* NG threshold, percent (1..100) */
    unsigned net_capacity_mbps;     /* fallback when <net_dir>/speed unusable */
    char     disk_dev[33];          /* /proc/diskstats device name —
                                     * kernel DISK_NAME_LEN(32) + NUL */
    unsigned probe_interval_s;      /* watch period, 1..OPCD_FAULT_PROBE_INTERVAL_MAX */
    uint32_t probe_countdown_s;     /* seconds accumulated toward the next sample */
    /* entry latch (#121): true while the resource is over threshold as of the
     * last sample. A sample flips it and reports the transition in
     * opcd_fault_report_t.*_entered / *_cleared. */
    bool     cpu_congested, disk_congested, net_congested;
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
    /* transitions since the previous sample (#121): entered = below→above
     * (notify once), cleared = above→below (Q6 hook, not notified). */
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

/* Forget the entry latch so an ONGOING congestion is reported as a fresh
 * entry on the next sample — for a new indication recipient / config
 * (opcd_ind_coalesce_reset). Counter baselines are kept. */
void opcd_fault_probe_reset_latch(opcd_fault_probe_t *p);

/* Sample the sources and evaluate utilisation since the previous call.
 * The first call only primes the counters (*out zeroed, returns 0). An
 * unreadable source leaves its resource un-flagged. */
int  opcd_fault_probe_sample(opcd_fault_probe_t *p, opcd_fault_report_t *out);

/* Pure helpers, unit-tested directly. opcd_fault_evaluate expects
 * elapsed_ms >= 1 (the sampler enforces a 1 ms floor); 0 silently skips the
 * disk and network calculations. */
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
