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
 *   - sampling: once per indication reporting period (period_seconds)
 *   - persistence: re-notified every period while the congestion persists
 *   - CPU (0x0001):  /proc/stat busy ratio over the interval, current_val = %
 *   - Memory (0x0002): NOT emitted — the target runs swapless, so the spec's
 *     paging-based definition cannot occur; flash pressure is covered by
 *     Disk I/O (0x0003)
 *   - Disk (0x0003): /proc/diskstats io_ticks utilisation, current_val = %
 *   - Network (0x0004): (rx+tx) rate vs link speed, threshold = 80% of the
 *     capacity, current_val = measured Mbps
 *
 * Source paths are struct fields rather than literals so unit tests can
 * point the probe at synthetic files — the same temp-path pattern the store
 * tests use.
 */

#define OPCD_FAULT_THRESHOLD_PCT_DEFAULT 80
#define OPCD_FAULT_NET_CAPACITY_DEFAULT  1000   /* Mbps, when sysfs speed is absent */

typedef struct opcd_fault_probe {
    /* config */
    unsigned threshold_pct;         /* NG threshold, percent (1..100) */
    unsigned net_capacity_mbps;     /* fallback when <net_dir>/speed unusable */
    char     disk_dev[24];          /* /proc/diskstats device name */
    /* source paths (overridable for tests) */
    char     path_proc_stat[96];
    char     path_diskstats[96];
    char     net_dir[96];           /* /sys/class/net/<if> */
    /* previous counters for delta computation */
    bool     primed;
    uint64_t cpu_busy, cpu_total;
    uint64_t disk_io_ms;
    uint64_t net_bytes;             /* rx + tx */
    uint64_t mono_ms;               /* CLOCK_MONOTONIC of the last sample */
} opcd_fault_probe_t;

typedef struct opcd_fault_report {
    bool     cpu_over;  uint16_t cpu_pct;
    bool     disk_over; uint16_t disk_pct;
    bool     net_over;  uint16_t net_mbps;
} opcd_fault_report_t;

/* Defaults: 80% threshold, mmcblk0, eth0, real /proc and /sys paths. */
void opcd_fault_probe_init(opcd_fault_probe_t *p);

/* Minimal key=value reader for the congestion_* keys in opc.conf. A missing
 * file or key leaves the defaults; never fails. */
void opcd_fault_probe_conf(opcd_fault_probe_t *p, const char *conf_path);

/* Sample the sources and evaluate utilisation since the previous call.
 * The first call only primes the counters (*out zeroed, returns 0). An
 * unreadable source leaves its resource un-flagged. */
int  opcd_fault_probe_sample(opcd_fault_probe_t *p, opcd_fault_report_t *out);

/* Pure helpers, unit-tested directly. */
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
