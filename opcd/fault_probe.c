/*
 * FaultDetect (0x0010) congestion probe.
 * See fault_probe.h for the policy note (thresholds: operator decision
 * 2026-06-12 / vendor inquiry #35; watch period + entry-only notification:
 * #121). Sampled from the indication tick on its own device-internal
 * interval (congestion_probe_interval_s) when the FaultDetect info bit (0x10)
 * is enabled — independent of the Indication Period.
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
#include "json_util.h"

/* cantops logger output per interface; the Network source (#192) */
#define OPCD_FAULT_NET_LINK_JSON_FMT "/var/log/cantops/json/%s/link.json"

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

/* /proc/meminfo → memory in use (#182): MemTotal minus MemFree, Buffers,
 * Cached and Slab — the kbmemused sysstat's `sar -r` printed on the target, so
 * used/total is the %memused the cpu.log logger records. All five fields are
 * required: a missing one is unknown rather than zero, so it cannot inflate
 * the usage. Keys are matched whole, so "SwapCached" is never taken for
 * "Cached". The not-in-use sum saturates at MemTotal (fields read as one
 * snapshot, but not atomically), so the usage floors at 0 and the addition
 * itself cannot wrap. */
int opcd_fault_parse_meminfo(const char *text, uint64_t *used_kb, uint64_t *total_kb)
{
    if (!text || !used_kb || !total_kb) return -1;
    static const char *const keys[] = { "MemTotal", "MemFree", "Buffers", "Cached", "Slab" };
    enum { NKEYS = sizeof keys / sizeof keys[0] };
    uint64_t v[NKEYS] = {0};
    unsigned seen = 0;
    const char *line = text;
    while (*line) {
        char key[32];
        unsigned long long kb;
        if (sscanf(line, "%31[^:]: %llu", key, &kb) == 2) {
            for (unsigned i = 0; i < NKEYS; i++) {
                if (strcmp(key, keys[i]) == 0) { v[i] = (uint64_t)kb; seen |= 1u << i; break; }
            }
        }
        const char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    if (seen != (1u << NKEYS) - 1u || v[0] == 0) return -1;
    uint64_t not_used = 0;
    for (unsigned i = 1; i < NKEYS; i++)
        not_used = v[i] > v[0] - not_used ? v[0] : not_used + v[i];
    *total_kb = v[0];
    *used_kb  = v[0] - not_used;
    return 0;
}

/* Hysteretic over-threshold test (#141). A resource that is NOT in the band
 * enters at threshold_pct; one that IS in it stays congested until utilisation
 * falls below clear_pct. clear_pct == threshold_pct is a zero-width band, i.e.
 * the pre-#141 single-level rule.
 *
 * clear_pct is normalised here as well as in _conf(): a release level above the
 * entry level (or a zeroed, hand-built probe) would latch once and never let
 * go, so it degrades to threshold_pct instead. This is struct hygiene for a
 * caller that did not go through _init/_conf — not a configuration feature;
 * opc.conf rejects both 0 and out-of-range values.
 *
 * The band state is an input here rather than being applied in the sampler so
 * the rule lives in one place instead of being repeated per resource. This
 * keeps opcd_fault_evaluate a pure function — same (probe state, deltas) in,
 * same report out — but *_over now means "considered congested", not "this
 * sample exceeded the entry level". Nothing outside this module reads *_over;
 * the sampler stores it and derives *_entered / *_cleared from it. */
static bool fault_over(const opcd_fault_probe_t *p, bool in_band, uint64_t pct)
{
    unsigned release = p->clear_pct;
    if (release == 0 || release > p->threshold_pct) release = p->threshold_pct;
    return in_band ? pct >= release : pct >= p->threshold_pct;
}

void opcd_fault_evaluate(const opcd_fault_probe_t *p,
                         uint64_t d_busy, uint64_t d_total,
                         uint64_t d_disk_ms, uint64_t elapsed_ms,
                         uint64_t d_net_busy_ms, uint64_t d_net_active_ms,
                         uint64_t mem_used_kb, uint64_t mem_total_kb,
                         opcd_fault_report_t *out)
{
    memset(out, 0, sizeof *out);
    if (d_total > 0) {
        uint64_t pct = d_busy * 100u / d_total;
        if (pct > 100) pct = 100;
        out->cpu_pct  = (uint16_t)pct;
        out->cpu_over = fault_over(p, p->cpu_congested, pct);
    }
    if (mem_total_kb > 0) {
        uint64_t pct = mem_used_kb * 100u / mem_total_kb;
        if (pct > 100) pct = 100;
        out->mem_pct  = (uint16_t)pct;
        out->mem_over = fault_over(p, p->mem_congested, pct);
    }
    if (elapsed_ms > 0) {
        uint64_t pct = d_disk_ms * 100u / elapsed_ms;
        if (pct > 100) pct = 100;
        out->disk_pct  = (uint16_t)pct;
        out->disk_over = fault_over(p, p->disk_congested, pct);
    }
    /* Network (#192): wireless channel occupancy, busy over active time of the
     * operating channel — current_val is that % (DFK 2026-06-29: 사용률 0-100%).
     * d_net_active_ms 0 is the sampler's UNKNOWN (unprimed or rejected delta),
     * so the resource stays un-flagged. Busy above active can only be a counter
     * anomaly and saturates at 100%. */
    if (d_net_active_ms > 0) {
        uint64_t pct = d_net_busy_ms >= d_net_active_ms
                           ? 100u : d_net_busy_ms * 100u / d_net_active_ms;
        out->net_pct  = (uint16_t)pct;
        out->net_over = fault_over(p, p->net_congested, pct);
    }
}

void opcd_fault_probe_init(opcd_fault_probe_t *p)
{
    memset(p, 0, sizeof *p);
    p->threshold_pct     = OPCD_FAULT_THRESHOLD_PCT_DEFAULT;
    p->clear_pct         = OPCD_FAULT_CLEAR_PCT_DEFAULT;
    p->probe_interval_s  = OPCD_FAULT_PROBE_INTERVAL_DEFAULT;
    snprintf(p->disk_dev,       sizeof p->disk_dev,       "mmcblk0");
    snprintf(p->path_proc_stat, sizeof p->path_proc_stat, "/proc/stat");
    snprintf(p->path_meminfo,   sizeof p->path_meminfo,   "/proc/meminfo");
    snprintf(p->path_diskstats, sizeof p->path_diskstats, "/proc/diskstats");
    snprintf(p->net_link_json,  sizeof p->net_link_json,
             OPCD_FAULT_NET_LINK_JSON_FMT, "mlan0");
}

void opcd_fault_probe_conf(opcd_fault_probe_t *p, const char *conf_path)
{
    if (!p || !conf_path) return;
    FILE *f = fopen(conf_path, "r");
    if (!f) return;                            /* no conf file → defaults */
    bool clear_seen = false;                   /* congestion_clear_pct accepted */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char key[48], val[64];
        /* accepts both "key=value" and "key = value"; '#' comment lines
         * fail the key match and are skipped. Keys owned by other modules
         * (udp_port, roam_notify_port, device_ip_iface, …) are ignored here.
         * Lines beyond 159 chars are split by fgets
         * — far above any congestion_* key=value pair, so no recovery. */
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "congestion_threshold_pct") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= 1 && v <= 100) p->threshold_pct = (unsigned)v;
        } else if (strcmp(key, "congestion_clear_pct") == 0) {
            char *end = NULL;
            unsigned long v = strtoul(val, &end, 10);
            /* endptr-checked: without it "abc" parses as 0
             * and would silently disable the band (reviewer A, #141). 0 is not
             * a valid release level — set clear EQUAL to threshold to turn the
             * band off. The clear <= threshold invariant is restored after the
             * loop, since keys may arrive in any order. */
            if (end && end != val && *end == '\0' && v >= 1 && v <= 100) {
                p->clear_pct = (unsigned)v;
                clear_seen = true;
            } else {
                /* Loud like the sibling string keys: a silently ignored value
                 * here leaves a band the operator did not write, and the most
                 * likely typo disables the fix outright (reviewer A). */
                fprintf(stderr, "opcd: fault probe: congestion_clear_pct '%s' "
                                "rejected (want 1..100) — derived default kept\n",
                        val);
            }
        } else if (strcmp(key, "congestion_probe_interval_s") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            /* out-of-range keeps the default, silently like the sibling
             * numeric keys above/below */
            if (v >= 1 && v <= OPCD_FAULT_PROBE_INTERVAL_MAX)
                p->probe_interval_s = (unsigned)v;
        } else if (strcmp(key, "congestion_net_capacity_mbps") == 0) {
            /* #192: there is no link-speed denominator any more. Still
             * recognised so an existing opc.conf loads unchanged, and said so
             * that a tuned value is not taken for one that works. */
            fprintf(stderr, "opcd: fault probe: congestion_net_capacity_mbps "
                            "ignored — Network congestion is the wireless "
                            "channel occupancy since #192\n");
        } else if (strcmp(key, "congestion_disk_dev") == 0) {
            if (strlen(val) >= sizeof p->disk_dev)
                /* silent truncation would never match diskstats — refuse loudly */
                fprintf(stderr, "opcd: fault probe: congestion_disk_dev '%s' "
                                "too long — ignored\n", val);
            else
                snprintf(p->disk_dev, sizeof p->disk_dev, "%s", val);
        } else if (strcmp(key, "congestion_net_if") == 0) {
            /* #192: the probe measures the channel a wireless station uses,
             * so only the station interfaces are valid. Anything else — eth0
             * from an opc.conf written for the old byte-rate probe — is
             * refused loudly and the current one (mlan0 by default) stays.
             * The two fixed names also keep the value out of the path. */
            if (strcmp(val, "mlan0") == 0 || strcmp(val, "mlan1") == 0)
                snprintf(p->net_link_json, sizeof p->net_link_json,
                         OPCD_FAULT_NET_LINK_JSON_FMT, val);
            else
                fprintf(stderr, "opcd: fault probe: congestion_net_if '%s' "
                                "ignored — the Network probe reads a wireless "
                                "station (mlan0/mlan1) since #192\n", val);
        }
    }
    fclose(f);
    /* When opc.conf did not set a release level, derive it from the threshold
     * that WAS configured. Keeping _init's 70 would collapse the band to zero
     * width for every threshold <= 70 — silently restoring the notification
     * storm this change removes (reviewer A, #141).
     *
     * The width is capped at half the entry level, not just floored: a flat
     * 10-point drop under a low threshold makes the band swallow almost the
     * whole range (threshold 10 would release only below 1%, so a resource
     * that once entered stays latched and its later re-entries are suppressed
     * — the sensitive configuration becoming the least sensitive one). Half
     * keeps the band proportional at any threshold. */
    if (!clear_seen) {
        unsigned band = p->threshold_pct / 2u;
        if (band > OPCD_FAULT_CLEAR_BAND_PCT) band = OPCD_FAULT_CLEAR_BAND_PCT;
        p->clear_pct = p->threshold_pct > band ? p->threshold_pct - band : 1;
    }
    /* Restore clear_pct <= threshold_pct: an explicit clear above the entry
     * level would latch once and never release. Clamping is silent-adjacent, so
     * say it — the clamped value equals threshold_pct, which is exactly the
     * documented way to turn the band OFF, and an operator who wrote a larger
     * number meant the opposite (reviewer A). */
    if (p->clear_pct > p->threshold_pct) {
        fprintf(stderr, "opcd: fault probe: congestion_clear_pct %u exceeds "
                        "congestion_threshold_pct %u — clamped to %u, "
                        "hysteresis band is now zero-width\n",
                p->clear_pct, p->threshold_pct, p->threshold_pct);
        p->clear_pct = p->threshold_pct;
    }
}

bool opcd_fault_probe_due(opcd_fault_probe_t *p, uint32_t elapsed_s)
{
    if (!p) return false;
    uint32_t interval = p->probe_interval_s ? p->probe_interval_s : 1;
    /* Clamp like opcd_ind_tick_elapsed: a stall longer than the interval
     * yields one sample, not a burst. */
    if (elapsed_s > interval) elapsed_s = interval;
    p->probe_countdown_s += elapsed_s;
    if (p->probe_countdown_s < interval) return false;
    p->probe_countdown_s = 0;
    return true;
}

void opcd_fault_probe_rearm(opcd_fault_probe_t *p, opcd_fault_res_t res)
{
    if (!p) return;
    /* Notify latch only — the band state stays, so a congestion that has
     * settled inside [clear_pct, threshold_pct) is still congested and the next
     * sample reports it as a fresh ENTRY instead of vanishing (#141). */
    switch (res) {
    case OPCD_FAULT_RES_CPU:  p->cpu_notified  = false; break;
    case OPCD_FAULT_RES_DISK: p->disk_notified = false; break;
    case OPCD_FAULT_RES_NET:  p->net_notified  = false; break;
    case OPCD_FAULT_RES_MEM:  p->mem_notified  = false; break;
    }
}

void opcd_fault_probe_reset_latch(opcd_fault_probe_t *p)
{
    if (!p) return;
    /* Notify latch only — see opcd_fault_probe_rearm. */
    p->cpu_notified = p->mem_notified = p->disk_notified = p->net_notified = false;
}

/* Network source (#192): the operating channel's cumulative active / busy time,
 * which the cantops logger copies from the driver survey into link.json's
 * survey_in_use section (the file is replaced atomically about once a second),
 * stamped with the logger's CLOCK_MONOTONIC time of that read (mono_ms). A
 * missing file, a missing section or a missing / negative value — mono_ms
 * included, since producer and consumer ship together — is a failed read:
 * UNKNOWN, like any other unreadable source.
 *
 * A station that IS associated (link.address present — the association test
 * the platform readers use) yet keeps arriving without the section most likely
 * runs firmware that does not report the channel busy time, and the resource
 * would stay unknown without a word. Say so ONCE, after
 * OPCD_FAULT_NET_MISSING_LOG_SAMPLES such samples rather than the first — a
 * periodic scan can hide the row for a moment. Only a good read re-arms it;
 * an unassociated or unreadable sample neither counts nor re-arms. */
static int read_net_survey(opcd_fault_probe_t *p, uint64_t *active_ms,
                           uint64_t *busy_ms, uint64_t *mono_ms)
{
    char *json = opc_json_slurp_file(p->net_link_json);
    if (!json) return -1;
    long active = -1, busy = -1, mono = -1;
    bool ok = opc_json_integer_section(json, "survey_in_use", "active_time_ms", &active) == 0 &&
              opc_json_integer_section(json, "survey_in_use", "busy_time_ms",   &busy)   == 0 &&
              opc_json_integer_section(json, "survey_in_use", "mono_ms",        &mono)   == 0 &&
              active >= 0 && busy >= 0 && mono >= 0;
    if (ok) {
        *active_ms = (uint64_t)active;
        *busy_ms   = (uint64_t)busy;
        *mono_ms   = (uint64_t)mono;
        p->net_missing = 0;
    } else {
        char ap[32];
        if (opc_json_string_section(json, "link", "address", ap, sizeof ap) == 0 &&
            p->net_missing < OPCD_FAULT_NET_MISSING_LOG_SAMPLES &&
            ++p->net_missing == OPCD_FAULT_NET_MISSING_LOG_SAMPLES)
            fprintf(stderr, "opcd: fault probe: %s has had no usable operating-"
                            "channel survey (survey_in_use) for %u samples while "
                            "associated — the firmware may not report the channel "
                            "busy time (CCA); Network congestion stays unknown "
                            "(#192)\n",
                    p->net_link_json, (unsigned)OPCD_FAULT_NET_MISSING_LOG_SAMPLES);
    }
    free(json);
    return ok ? 0 : -1;
}

int opcd_fault_probe_sample(opcd_fault_probe_t *p, opcd_fault_report_t *out)
{
    if (!p || !out) return -1;
    memset(out, 0, sizeof *out);

    /* 32 KiB stack buffer is deliberate: diskstats on the NXP target is
     * well under 1 KiB; the headroom absorbs many-device dev/CI hosts.
     * One frame per probe interval on the daemon stack — no recursion. */
    char buf[32768];
    uint64_t busy = 0, total = 0, disk = 0;
    bool cpu_ok  = read_text(p->path_proc_stat, buf, sizeof buf) == 0 &&
                   opcd_fault_parse_proc_stat(buf, &busy, &total) == 0;
    bool disk_ok = read_text(p->path_diskstats, buf, sizeof buf) == 0 &&
                   opcd_fault_parse_diskstats(buf, p->disk_dev, &disk) == 0;
    uint64_t mem_used = 0, mem_total = 0;
    bool mem_ok  = read_text(p->path_meminfo, buf, sizeof buf) == 0 &&
                   opcd_fault_parse_meminfo(buf, &mem_used, &mem_total) == 0;
    uint64_t net_active = 0, net_busy = 0, net_mono = 0;
    bool net_ok  = read_net_survey(p, &net_active, &net_busy, &net_mono) == 0;
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
        /* Network (#192): the survey counters run from association and restart
         * near 0 on every (re)association or roam, so a delta counts only when
         * both moved forward, the logger's stamp moved forward (a file that did
         * not, the logger stopped), and Δactive matches that Δmono. The last
         * test is against the producer's monotonic stamp, not our elapsed
         * time: moal takes active time from CLOCK_REALTIME while busy is
         * firmware airtime, so a wall-clock step inside the window would skew
         * the ratio — up to a false entry or clear (tribunal A-R1-001). Any
         * failure is UNKNOWN — never 0% (a false clear) nor a guess (a false
         * entry) — and the new readings still become the baseline below. */
        bool net_measured = false;
        uint64_t d_net_busy = 0, d_net_active = 0;
        if (net_ok && p->net_primed &&
            net_active >= p->net_active_ms && net_busy >= p->net_busy_ms &&
            net_mono > p->net_mono_ms) {
            uint64_t da   = net_active - p->net_active_ms;
            uint64_t dm   = net_mono   - p->net_mono_ms;
            uint64_t skew = da > dm ? da - dm : dm - da;
            if (da > 0 && skew <= OPCD_FAULT_NET_CLOCK_SLACK_MS) {
                d_net_active = da;
                d_net_busy   = net_busy - p->net_busy_ms;
                net_measured = true;
            }
        }

        opcd_fault_evaluate(p, d_busy, d_total, d_disk, elapsed,
                            d_net_busy, d_net_active,
                            mem_ok ? mem_used : 0, mem_ok ? mem_total : 0,
                            out);
        /* Entry latch (#121): report the transition, then remember the level
         * — but only for a source that was actually measured this sample
         * (read ok AND primed). An unreadable or re-priming source is
         * UNKNOWN, not "below threshold": its latch holds, so a transient
         * read failure neither clears the congestion nor makes the recovered
         * over-sample a duplicate entry (Claude review, #121). */
        /* *_over is the BAND state (fault_over already folded the previous
         * band state in); *_notified is the notify-once latch. Deriving the
         * transitions from *_notified rather than from *_congested is what
         * makes rearm/reset_latch work inside the band: they clear only
         * *_notified, so a still-congested resource re-enters (#141). */
        if (cpu_ok && p->cpu_primed) {
            out->cpu_entered  =  out->cpu_over && !p->cpu_notified;
            out->cpu_cleared  = !out->cpu_over &&  p->cpu_notified;
            p->cpu_congested  = out->cpu_over;
            p->cpu_notified   = out->cpu_over;
        }
        /* Memory is a level, not a counter, so it has no baseline of its own:
         * any readable sample is a measured one. It is still evaluated only
         * here, after the first call, so all resources share one first-sample
         * rule (#182). An unreadable meminfo is UNKNOWN like the others. */
        if (mem_ok) {
            out->mem_entered  =  out->mem_over && !p->mem_notified;
            out->mem_cleared  = !out->mem_over &&  p->mem_notified;
            p->mem_congested  = out->mem_over;
            p->mem_notified   = out->mem_over;
        }
        if (disk_ok && p->disk_primed) {
            out->disk_entered =  out->disk_over && !p->disk_notified;
            out->disk_cleared = !out->disk_over &&  p->disk_notified;
            p->disk_congested = out->disk_over;
            p->disk_notified  = out->disk_over;
        }
        if (net_measured) {
            out->net_entered  =  out->net_over && !p->net_notified;
            out->net_cleared  = !out->net_over &&  p->net_notified;
            p->net_congested  = out->net_over;
            p->net_notified   = out->net_over;
        }
    }

    /* Refresh baselines: a successful read (re)primes its source; a failed
     * read drops the primed flag so recovery starts from a fresh baseline. */
    if (cpu_ok)  { p->cpu_busy = busy; p->cpu_total = total; }
    if (disk_ok) p->disk_io_ms = disk;
    if (net_ok)  {
        p->net_active_ms = net_active; p->net_busy_ms = net_busy; p->net_mono_ms = net_mono;
    }
    p->cpu_primed  = cpu_ok;
    p->disk_primed = disk_ok;
    p->net_primed  = net_ok;
    p->mono_ms = now;
    p->primed  = true;
    return 0;
}
