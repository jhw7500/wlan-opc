/*
 * FaultDetect (0x0010) congestion probe — T6 INTERIM policy.
 * See fault_probe.h for the full policy note (operator decision 2026-06-12,
 * vendor inquiry #35). Called from opcd_ind_tick() once per indication
 * reporting period when the FaultDetect info bit (0x10) is enabled.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fault_probe.h"

/* Read a small text file into buf (NUL-terminated). 32 KiB covers
 * /proc/diskstats with room to spare on the target (a handful of mmcblk
 * devices); a fill-to-capacity read is logged once so a silently truncated
 * diskstats (device row beyond the boundary) is diagnosable. */
static int read_text(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, buf, cap - 1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0) return -1;
    if ((size_t)n == cap - 1) {
        /* warn once per path (re-warn only if a different path fills) —
         * a truncated diskstats could silently hide the device row */
        static char warned_path[96];
        if (strncmp(warned_path, path, sizeof warned_path - 1) != 0) {
            snprintf(warned_path, sizeof warned_path, "%s", path);
            fprintf(stderr, "opcd: fault probe: %s filled the %zu B buffer — "
                            "content may be truncated\n", path, cap);
        }
    }
    buf[n] = '\0';
    return 0;
}

static int read_u64_file(const char *path, uint64_t *out)
{
    char buf[32];
    if (read_text(path, buf, sizeof buf) != 0) return -1;
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf) return -1;
    *out = (uint64_t)v;
    return 0;
}

static uint64_t mono_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int opcd_fault_parse_proc_stat(const char *text, uint64_t *busy, uint64_t *total)
{
    if (!text || !busy || !total) return -1;
    /* aggregate line: "cpu  user nice system idle iowait irq softirq steal" */
    if (strncmp(text, "cpu ", 4) != 0) return -1;
    unsigned long long v[8] = {0};
    int n = sscanf(text + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n < 4) return -1;                      /* need at least user..idle */
    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += v[i];
    *total = t;
    *busy  = t - v[3] - (n > 4 ? v[4] : 0);    /* total minus idle, iowait */
    return 0;
}

int opcd_fault_parse_diskstats(const char *text, const char *dev, uint64_t *io_ms)
{
    if (!text || !dev || !io_ms) return -1;
    const char *line = text;
    while (*line) {
        char name[33];                 /* kernel DISK_NAME_LEN(32) + NUL */
        unsigned long long ticks;
        /* per line: maj min name + stat fields; the 10th stat field is
         * io_ticks — milliseconds the device spent doing I/O. */
        if (sscanf(line, "%*u %*u %32s %*u %*u %*u %*u %*u %*u %*u %*u %*u %llu",
                   name, &ticks) == 2 && strcmp(name, dev) == 0) {
            *io_ms = (uint64_t)ticks;
            return 0;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    return -1;
}

void opcd_fault_evaluate(const opcd_fault_probe_t *p,
                         uint64_t d_busy, uint64_t d_total,
                         uint64_t d_disk_ms, uint64_t elapsed_ms,
                         uint64_t d_net_bytes, unsigned link_mbps,
                         opcd_fault_report_t *out)
{
    memset(out, 0, sizeof *out);
    if (d_total > 0) {
        uint64_t pct = d_busy * 100u / d_total;
        if (pct > 100) pct = 100;
        out->cpu_pct  = (uint16_t)pct;
        out->cpu_over = pct >= p->threshold_pct;
    }
    if (elapsed_ms > 0) {
        uint64_t pct = d_disk_ms * 100u / elapsed_ms;
        if (pct > 100) pct = 100;
        out->disk_pct  = (uint16_t)pct;
        out->disk_over = pct >= p->threshold_pct;

        /* bytes → Mbit/s: *8 bits, /elapsed_ms gives kbit/s, /1000 → Mbit/s */
        uint64_t mbps = (d_net_bytes * 8ULL / elapsed_ms) / 1000u;
        /* net_over is judged on the uncapped rate; net_mbps (the wire
         * current_val, uint16) saturates at 65535 — a capture showing 65535
         * means "at least 65.5 Gbit/s", not the exact trigger rate. */
        out->net_mbps = (uint16_t)(mbps > 65535u ? 65535u : mbps);
        if (link_mbps > 0)
            out->net_over = mbps * 100u >= (uint64_t)link_mbps * p->threshold_pct;
    }
}

void opcd_fault_probe_init(opcd_fault_probe_t *p)
{
    memset(p, 0, sizeof *p);
    p->threshold_pct     = OPCD_FAULT_THRESHOLD_PCT_DEFAULT;
    p->net_capacity_mbps = OPCD_FAULT_NET_CAPACITY_DEFAULT;
    snprintf(p->disk_dev,       sizeof p->disk_dev,       "mmcblk0");
    snprintf(p->path_proc_stat, sizeof p->path_proc_stat, "/proc/stat");
    snprintf(p->path_diskstats, sizeof p->path_diskstats, "/proc/diskstats");
    snprintf(p->net_dir,        sizeof p->net_dir,        "/sys/class/net/eth0");
}

void opcd_fault_probe_conf(opcd_fault_probe_t *p, const char *conf_path)
{
    if (!p || !conf_path) return;
    FILE *f = fopen(conf_path, "r");
    if (!f) return;                            /* no conf file → defaults */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char key[48], val[64];
        /* accepts both "key=value" and "key = value"; '#' comment lines
         * fail the key match and are skipped. Unknown keys (e.g. a future
         * udp_port) are ignored. Lines beyond 159 chars are split by fgets
         * — far above any congestion_* key=value pair, so no recovery. */
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "congestion_threshold_pct") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= 1 && v <= 100) p->threshold_pct = (unsigned)v;
        } else if (strcmp(key, "congestion_net_capacity_mbps") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= 1) p->net_capacity_mbps = (unsigned)v;
        } else if (strcmp(key, "congestion_disk_dev") == 0) {
            if (strlen(val) >= sizeof p->disk_dev)
                /* silent truncation would never match diskstats — refuse loudly */
                fprintf(stderr, "opcd: fault probe: congestion_disk_dev '%s' "
                                "too long — ignored\n", val);
            else
                snprintf(p->disk_dev, sizeof p->disk_dev, "%s", val);
        } else if (strcmp(key, "congestion_net_if") == 0) {
            if (strchr(val, '/') != NULL || val[0] == '.')
                /* interface names never contain '/' nor start with '.' —
                 * reject so the probe cannot be pointed outside sysfs
                 * (e.g. "../../proc"). VLAN names like eth0.100 still pass. */
                fprintf(stderr, "opcd: fault probe: congestion_net_if '%s' "
                                "rejected (path characters)\n", val);
            else
                snprintf(p->net_dir, sizeof p->net_dir, "/sys/class/net/%s", val);
        }
    }
    fclose(f);
}

static int read_net_bytes(const opcd_fault_probe_t *p, uint64_t *bytes)
{
    char path[128];
    uint64_t rx = 0, tx = 0;
    snprintf(path, sizeof path, "%s/statistics/rx_bytes", p->net_dir);
    if (read_u64_file(path, &rx) != 0) return -1;
    snprintf(path, sizeof path, "%s/statistics/tx_bytes", p->net_dir);
    if (read_u64_file(path, &tx) != 0) return -1;
    *bytes = rx + tx;
    return 0;
}

static unsigned read_link_mbps(const opcd_fault_probe_t *p)
{
    char path[128], buf[16];
    snprintf(path, sizeof path, "%s/speed", p->net_dir);
    if (read_text(path, buf, sizeof buf) == 0) {
        long v = strtol(buf, NULL, 10);
        if (v > 0) return (unsigned)v;
    }
    /* wireless interfaces report -1 / no node — use the configured value */
    return p->net_capacity_mbps;
}

int opcd_fault_probe_sample(opcd_fault_probe_t *p, opcd_fault_report_t *out)
{
    if (!p || !out) return -1;
    memset(out, 0, sizeof *out);

    /* 32 KiB stack buffer is deliberate: diskstats on the NXP target is
     * well under 1 KiB; the headroom absorbs many-device dev/CI hosts.
     * One frame per reporting period on the daemon stack — no recursion. */
    char buf[32768];
    uint64_t busy = 0, total = 0, disk = 0, net = 0;
    bool cpu_ok  = read_text(p->path_proc_stat, buf, sizeof buf) == 0 &&
                   opcd_fault_parse_proc_stat(buf, &busy, &total) == 0;
    bool disk_ok = read_text(p->path_diskstats, buf, sizeof buf) == 0 &&
                   opcd_fault_parse_diskstats(buf, p->disk_dev, &disk) == 0;
    bool net_ok  = read_net_bytes(p, &net) == 0;
    uint64_t now = mono_now_ms();

    /* Deltas are computed only for a source that was primed by a previous
     * successful read. A source that failed (now or earlier) re-primes on
     * its next successful read instead — otherwise the since-boot absolute
     * counter would land in a single-period delta and report a spurious
     * 100% congestion. */
    if (p->primed) {
        /* elapsed floor of 1 ms only guards the division — the production
         * period is >= 1 s, so two same-millisecond samples never happen
         * outside the tests. */
        uint64_t elapsed = now > p->mono_ms ? now - p->mono_ms : 1;
        /* a counter reset (interface re-init) clamps to 0 instead of wrapping */
        uint64_t d_busy  = (cpu_ok && p->cpu_primed && busy  >= p->cpu_busy)
                               ? busy  - p->cpu_busy   : 0;
        uint64_t d_total = (cpu_ok && p->cpu_primed && total >= p->cpu_total)
                               ? total - p->cpu_total  : 0;
        uint64_t d_disk  = (disk_ok && p->disk_primed && disk >= p->disk_io_ms)
                               ? disk  - p->disk_io_ms : 0;
        uint64_t d_net   = (net_ok && p->net_primed && net   >= p->net_bytes)
                               ? net   - p->net_bytes  : 0;

        opcd_fault_evaluate(p, d_busy, d_total, d_disk, elapsed, d_net,
                            (net_ok && p->net_primed) ? read_link_mbps(p) : 0,
                            out);
    }

    /* Refresh baselines: a successful read (re)primes its source; a failed
     * read drops the primed flag so recovery starts from a fresh baseline. */
    if (cpu_ok)  { p->cpu_busy = busy; p->cpu_total = total; }
    if (disk_ok) p->disk_io_ms = disk;
    if (net_ok)  p->net_bytes  = net;
    p->cpu_primed  = cpu_ok;
    p->disk_primed = disk_ok;
    p->net_primed  = net_ok;
    p->mono_ms = now;
    p->primed  = true;
    return 0;
}
