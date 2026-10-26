/*
 * Unit tests for the FaultDetect congestion probe (T6 interim policy).
 * Pure parsers and the evaluator are tested with synthetic inputs; the
 * end-to-end sample() path runs against synthetic source files (CWD-relative
 * temp paths, same pattern as the other opcd tests).
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../fault_probe.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "FAIL write %s\n", path); failures++; return; }
    fputs(text, f);
    fclose(f);
}

/* Synthetic /proc/meminfo with `used_kb` in use under the sar -r formula
 * (Buffers + Cached + Slab = 50000 kB are not counted as used). SwapCached
 * follows Cached, as in the kernel's own order, so a substring key match
 * ("Cached" inside "SwapCached") would overwrite the right value. */
static void write_meminfo(const char *path, unsigned long long total_kb,
                          unsigned long long used_kb)
{
    char text[256];
    snprintf(text, sizeof text,
             "MemTotal:       %llu kB\n"
             "MemFree:        %llu kB\n"
             "MemAvailable:   1 kB\n"
             "Buffers:        10000 kB\n"
             "Cached:         20000 kB\n"
             "SwapCached:     7777 kB\n"
             "Slab:           20000 kB\n",
             total_kb, total_kb - used_kb - 50000u);
    write_file(path, text);
}

/* Synthetic cantops <iface>/link.json (#192), cut down to what the Network
 * probe reads. `associated` adds the link section (the station's association
 * marker); active_ms < 0 leaves the survey_in_use section out, mono_ms < 0
 * leaves only the section's mono_ms stamp out. */
static void write_link_json(const char *path, bool associated,
                            long long active_ms, long long busy_ms,
                            long long mono_ms)
{
    char mono[40] = "", survey[200] = "", text[512];
    if (mono_ms >= 0)
        snprintf(mono, sizeof mono, ", \"mono_ms\": %lld", mono_ms);
    if (active_ms >= 0)
        snprintf(survey, sizeof survey,
                 ",\n  \"survey_in_use\": {\"freq\": 5200, \"active_time_ms\": %lld, "
                 "\"busy_time_ms\": %lld%s}", active_ms, busy_ms, mono);
    snprintf(text, sizeof text,
             "{\n  \"date\": \"2026-10-26 12:00:00\",\n"
             "  \"info\": {\"address\": \"00:50:43:02:fe:01\"}%s%s\n}\n",
             associated ? ",\n  \"link\": {\"address\": \"aa:bb:cc:dd:ee:01\", "
                          "\"signal_avg\": \"-50 dBm\"}" : "",
             survey);
    write_file(path, text);
}

/* Send stderr to `path` (truncated) and return the saved descriptor, so a test
 * can count the warnings the probe printed. Restore before asserting — a FAIL
 * line goes to stderr too. */
static int stderr_to(const char *path)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }
    return saved;
}

static void stderr_restore(int saved)
{
    fflush(stderr);
    if (saved >= 0) { dup2(saved, STDERR_FILENO); close(saved); }
}

/* Number of lines in `path` that contain `needle` (-1: unreadable). */
static int count_lines(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

int main(void)
{
    /* 1. /proc/stat parser: busy excludes idle (field 4) and iowait (5). */
    uint64_t busy = 0, total = 0;
    ASSERT(opcd_fault_parse_proc_stat(
               "cpu  100 0 50 800 50 0 0 0\ncpu0 1 2 3 4\n", &busy, &total) == 0 &&
           total == 1000 && busy == 150,
           "proc_stat: busy excludes idle+iowait");
    ASSERT(opcd_fault_parse_proc_stat("intr 1 2 3\n", &busy, &total) != 0,
           "proc_stat: non-cpu first line rejected");

    /* 2. diskstats parser: exact device row, 10th stat field (io_ticks). */
    const char *ds =
        " 179       0 mmcblk0 1 2 3 4 5 6 7 8 0 900 10\n"
        " 179       1 mmcblk0p1 1 2 3 4 5 6 7 8 0 100 10\n";
    uint64_t io = 0;
    ASSERT(opcd_fault_parse_diskstats(ds, "mmcblk0", &io) == 0 && io == 900,
           "diskstats: device row found, io_ticks read");
    ASSERT(opcd_fault_parse_diskstats(ds, "sda", &io) != 0,
           "diskstats: missing device rejected");

    /* 3. evaluator thresholds (default 80%). */
    opcd_fault_probe_t p;
    opcd_fault_probe_init(&p);
    opcd_fault_report_t r;
    opcd_fault_evaluate(&p, 90, 100, 850, 1000, 0, 0, 0, 0, &r);
    ASSERT(r.cpu_over && r.cpu_pct == 90,    "evaluate: cpu 90% over at 80");
    ASSERT(r.disk_over && r.disk_pct == 85,  "evaluate: disk 85% over at 80");
    ASSERT(!r.net_over && r.net_pct == 0,    "evaluate: net active 0 (UNKNOWN) not over");
    opcd_fault_evaluate(&p, 79, 100, 0, 1000, 900, 1000, 0, 0, &r);
    ASSERT(!r.cpu_over && r.cpu_pct == 79,   "evaluate: cpu 79% under 80");
    /* #192 Network current_val is the channel occupancy % (DFK 2026-06-29,
     * 사용률 0-100%): Δbusy / Δactive of the operating channel. */
    ASSERT(r.net_over && r.net_pct == 90,    "evaluate: net busy 900 of 1000 ms active = 90% over 80");
    opcd_fault_evaluate(&p, 0, 100, 0, 1000, 500, 1000, 0, 0, &r);
    ASSERT(!r.net_over && r.net_pct == 50,   "evaluate: net 500/1000 = 50% under 80");
    /* busy above active can only be a counter anomaly — saturates at 100%. */
    opcd_fault_evaluate(&p, 0, 100, 0, 1000, 1500, 1000, 0, 0, &r);
    ASSERT(r.net_over && r.net_pct == 100,   "evaluate: net busy > active clamps to 100%");

    /* 4. conf overrides (key=value; unknown keys and comments ignored). */
    char conf[64];
    snprintf(conf, sizeof conf, "test_fp_conf_%d.tmp", (int)getpid());
    write_file(conf,
        "# congestion overrides\n"
        "udp_port = 50607\n"
        "congestion_threshold_pct = 50\n"
        "congestion_disk_dev = sda\n"
        "congestion_net_if = mlan1\n");
    opcd_fault_probe_init(&p);
    opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 50 &&
           strcmp(p.disk_dev, "sda") == 0 &&
           strcmp(p.net_link_json, "/var/log/cantops/json/mlan1/link.json") == 0,
           "conf: overrides applied, unknown keys ignored");
    opcd_fault_probe_conf(&p, "test_fp_no_such_file.conf");
    ASSERT(p.threshold_pct == 50, "conf: missing file leaves settings");

    /* 4b. #192 Network keys. The source is the cantops link.json of a wireless
     *     station (mlan0 by default). Any other congestion_net_if — eth0 from an
     *     opc.conf written for the old byte-rate probe, or a path — is refused
     *     with one warning and mlan0 stays. congestion_net_capacity_mbps is
     *     still recognised so such a file loads, but it changes nothing and
     *     says so once. */
    char errf[64];
    snprintf(errf, sizeof errf, "test_fp_err_%d.tmp", (int)getpid());
    opcd_fault_probe_init(&p);
    ASSERT(strcmp(p.net_link_json, "/var/log/cantops/json/mlan0/link.json") == 0,
           "conf/net: default source is mlan0's link.json");
    write_file(conf, "congestion_net_if = eth0\n");
    {
        int saved = stderr_to(errf);
        opcd_fault_probe_conf(&p, conf);
        stderr_restore(saved);
    }
    ASSERT(strcmp(p.net_link_json, "/var/log/cantops/json/mlan0/link.json") == 0 &&
           count_lines(errf, "congestion_net_if 'eth0' ignored") == 1,
           "conf/net: congestion_net_if eth0 warns once and keeps mlan0");
    write_file(conf, "congestion_net_if = ../../proc\n");
    opcd_fault_probe_init(&p);
    {
        int saved = stderr_to(errf);
        opcd_fault_probe_conf(&p, conf);
        stderr_restore(saved);
    }
    ASSERT(strcmp(p.net_link_json, "/var/log/cantops/json/mlan0/link.json") == 0 &&
           count_lines(errf, "congestion_net_if '../../proc' ignored") == 1,
           "conf/net: a path as congestion_net_if is refused, mlan0 kept");
    {
        opcd_fault_probe_t q;
        write_file(conf, "congestion_threshold_pct = 60\n"
                         "congestion_clear_pct = 55\n");
        opcd_fault_probe_init(&q);
        opcd_fault_probe_conf(&q, conf);
        write_file(conf, "congestion_threshold_pct = 60\n"
                         "congestion_net_capacity_mbps = 100\n"
                         "congestion_clear_pct = 55\n");
        opcd_fault_probe_init(&p);
        int saved = stderr_to(errf);
        opcd_fault_probe_conf(&p, conf);
        stderr_restore(saved);
        ASSERT(p.threshold_pct == 60 && p.clear_pct == 55 &&
               memcmp(&p, &q, sizeof p) == 0 &&
               count_lines(errf, "congestion_net_capacity_mbps ignored") == 1,
               "conf/net: congestion_net_capacity_mbps loads, changes nothing, warns once");
    }
    unlink(conf);

    /* 5. end-to-end sample() against synthetic source files. */
    char d[64];
    snprintf(d, sizeof d, "test_fp_%d", (int)getpid());
    mkdir(d, 0755);
    char fstat[128], fdisk[128], flink[96];  /* flink fits net_link_json whole */
    snprintf(fstat, sizeof fstat, "%s/stat", d);
    snprintf(fdisk, sizeof fdisk, "%s/diskstats", d);
    snprintf(flink, sizeof flink, "%s/link.json", d);
    write_file(fstat, "cpu  0 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    write_link_json(flink, true, 10000, 1000, 900000);

    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           !r.cpu_over && !r.disk_over && !r.net_over,
           "sample: priming call reports nothing");

    /* advance: cpu 900/1000 busy, disk 950 ms, channel busy 900 of 1000 ms */
    write_file(fstat, "cpu  900 0 0 1100 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 950 0\n");
    write_link_json(flink, true, 11000, 1900, 901000);
    p.mono_ms -= 1000;                       /* pretend one second elapsed */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0, "sample: second call ok");
    ASSERT(r.cpu_over && r.cpu_pct == 90,    "sample: cpu 90% detected");
    ASSERT(r.disk_over && r.disk_pct >= 90,  "sample: disk ~95% detected");
    ASSERT(r.net_over && r.net_pct == 90,    "sample: channel occupancy 90% detected");

    /* 6. unreadable sources leave resources un-flagged (fail-soft). */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0, "sample: prime with dead sources ok");
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           !r.cpu_over && !r.disk_over && !r.net_over,
           "sample: dead sources stay un-flagged");

    /* 7. a source that appears only after priming must re-prime instead of
     *    treating its since-boot absolute counter as one period's delta
     *    (false-positive regression — PR #39 review, Codex P2/Gemini high). */
    char flate[128];
    snprintf(flate, sizeof flate, "%s/late_stat", d);
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", flate);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0,
           "late-source: prime with absent cpu source");
    write_file(flate, "cpu  900000 0 0 100000 0 0 0 0\n");  /* huge since-boot busy */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.cpu_over,
           "late-source: first readable sample re-primes, no false positive");
    write_file(flate, "cpu  900900 0 0 100100 0 0 0 0\n");  /* +900/+1000 = 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           r.cpu_over && r.cpu_pct == 90,
           "late-source: subsequent delta evaluated normally");
    unlink(flate);

    /* 8. #121 entry latch: a resource's congestion is reported as an ENTRY
     *    (below→above threshold transition) once; a persisting congestion is
     *    over but not entered; dropping below clears the latch (cleared hook)
     *    and a later rise is a fresh entry. Per resource — disk stays under
     *    the whole time and never enters. */
    write_file(fstat, "cpu  0 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.cpu_entered && !r.cpu_cleared,
           "latch: priming call enters nothing");
    write_file(fstat, "cpu  900 0 0 1100 0 0 0 0\n");          /* 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && r.cpu_entered &&
           !r.cpu_cleared && !r.disk_entered && !r.net_entered,
           "latch: first over-threshold sample is an ENTRY (cpu only)");
    write_file(fstat, "cpu  1800 0 0 1200 0 0 0 0\n");         /* still 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && !r.cpu_entered &&
           !r.cpu_cleared,
           "latch: persisting congestion is over but NOT entered again");
    write_file(fstat, "cpu  1800 0 0 2200 0 0 0 0\n");         /* 0% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.cpu_over && !r.cpu_entered &&
           r.cpu_cleared,
           "latch: dropping below threshold is a CLEAR");
    write_file(fstat, "cpu  2700 0 0 2300 0 0 0 0\n");         /* 90% again */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && r.cpu_entered,
           "latch: rising again after a clear is a fresh ENTRY");
    write_file(fstat, "cpu  3600 0 0 2400 0 0 0 0\n");         /* still 90% */
    p.mono_ms -= 1000;
    opcd_fault_probe_reset_latch(&p);
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && r.cpu_entered,
           "latch: reset_latch makes an ongoing congestion a fresh ENTRY (new recipient)");

    /* 8a-2. D4(i): opcd_fault_probe_rearm re-arms ONE resource's latch. The
     *       latch is committed inside sample(), i.e. before the caller has had
     *       a chance to send; when that send fails at Indication Period 0 there
     *       is no staging buffer to retry from, so "notify once on entry" would
     *       become "never notify". Re-arming restores the entry for the next
     *       due sample, and is one-shot — the sample commits it again. */
    write_file(fstat, "cpu  4500 0 0 2500 0 0 0 0\n");         /* still 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && !r.cpu_entered,
           "rearm: baseline — an ongoing congestion is not re-entered on its own");
    opcd_fault_probe_rearm(&p, OPCD_FAULT_RES_CPU);
    write_file(fstat, "cpu  5400 0 0 2600 0 0 0 0\n");         /* still 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && r.cpu_entered &&
           !r.cpu_cleared,
           "D4(i) rearm: the still-standing congestion is reported as a fresh ENTRY");
    write_file(fstat, "cpu  6300 0 0 2700 0 0 0 0\n");         /* still 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && !r.cpu_entered,
           "D4(i) rearm: one-shot — that sample committed the latch again");

    /* 8b. #121 a transient source read failure is UNKNOWN, not "below
     *     threshold": the latch must hold, so the recovered over-sample is
     *     not a duplicate ENTRY. cpu over → stat unreadable → stat readable
     *     and still over: no second entry, no clear in between. */
    write_file(fstat, "cpu  0 0 0 1000 0 0 0 0\n");
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_file(fstat, "cpu  900 0 0 1100 0 0 0 0\n");           /* 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_entered,
           "latch/readfail: entry");
    unlink(fstat);                                               /* source vanishes */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.cpu_over &&
           !r.cpu_entered && !r.cpu_cleared,
           "latch/readfail: unreadable source is neither entry nor clear");
    write_file(fstat, "cpu  900 0 0 1100 0 0 0 0\n");           /* re-prime read */
    p.mono_ms -= 1000;
    (void)opcd_fault_probe_sample(&p, &r);                       /* re-primes cpu */
    write_file(fstat, "cpu  1800 0 0 1200 0 0 0 0\n");          /* still 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_over && !r.cpu_entered,
           "latch/readfail: recovered over-sample is NOT a duplicate entry");
    write_file(fstat, "cpu  1800 0 0 2200 0 0 0 0\n");          /* 0% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_cleared,
           "latch/readfail: a real drop after recovery is the clear");

    /* 9. #121 device-internal probe interval, decoupled from the Indication
     *    Period: default 10 s, advanced by elapsed seconds, due exactly once
     *    per interval, a long stall clamps to one due; opc.conf
     *    congestion_probe_interval_s overrides within 1..3600. */
    opcd_fault_probe_init(&p);
    ASSERT(p.probe_interval_s == OPCD_FAULT_PROBE_INTERVAL_DEFAULT &&
           OPCD_FAULT_PROBE_INTERVAL_DEFAULT == 10,
           "interval: default 10 s");
    {
        int dues = 0;
        for (int i = 0; i < 9; i++) dues += opcd_fault_probe_due(&p, 1) ? 1 : 0;
        ASSERT(dues == 0, "interval: not due before 10 s");
        ASSERT(opcd_fault_probe_due(&p, 1), "interval: due at 10 s");
        ASSERT(!opcd_fault_probe_due(&p, 1), "interval: countdown restarts after a due");
        ASSERT(opcd_fault_probe_due(&p, 100000), "interval: long stall → due once");
        ASSERT(!opcd_fault_probe_due(&p, 1), "interval: stall does not bank extra dues");
    }
    write_file(conf,
        "congestion_probe_interval_s = 5\n");
    opcd_fault_probe_init(&p);
    opcd_fault_probe_conf(&p, conf);
    ASSERT(p.probe_interval_s == 5, "interval: conf override 5 s");
    write_file(conf, "congestion_probe_interval_s = 0\n");
    opcd_fault_probe_init(&p);
    opcd_fault_probe_conf(&p, conf);
    ASSERT(p.probe_interval_s == 10, "interval: 0 rejected, default kept");
    write_file(conf, "congestion_probe_interval_s = 3601\n");
    opcd_fault_probe_init(&p);
    opcd_fault_probe_conf(&p, conf);
    ASSERT(p.probe_interval_s == 10, "interval: >3600 rejected, default kept");
    unlink(conf);

    /* 10. #141 hysteresis band: ENTRY at threshold_pct, release only below
     *     clear_pct. A resource hovering at the entry level (81/79/81 ...)
     *     must latch ONCE. Before #141 the single level made every dip a
     *     CLEAR and every rise a fresh ENTRY, so a noisy ~80% load notified
     *     on every sample. Exercised on cpu AND disk — the rule lives in one
     *     helper shared by all three resources. */
    opcd_fault_probe_init(&p);
    ASSERT(p.threshold_pct == 80 && p.clear_pct == 70 &&
           OPCD_FAULT_CLEAR_PCT_DEFAULT == 70,
           "hysteresis: defaults are 80 entry / 70 clear");
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */

    write_file(fstat, "cpu  1081 0 0 1019 0 0 0 0\n");           /* 81% */
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 850 0\n");   /* ~85% */
    p.mono_ms -= 1000;
    /* cpu_pct is a busy/total ratio and is exact; disk and net divide by the
     * measured elapsed time, which carries the sample's own file-I/O cost, so
     * those resources are driven well clear of the 80/70 edges and asserted on
     * the transition rather than on an exact percentage. */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           r.cpu_pct == 81 && r.cpu_entered &&
           r.disk_over && r.disk_entered,
           "hysteresis: entry (cpu 81% exact, disk ~85%)");

    write_file(fstat, "cpu  1160 0 0 1040 0 0 0 0\n");           /* 79% */
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 1600 0\n");   /* ~75% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_pct == 79 &&
           r.cpu_over  && !r.cpu_cleared  && !r.cpu_entered &&
           r.disk_over && !r.disk_cleared && !r.disk_entered,
           "hysteresis: a dip inside the band (cpu 79%) is NOT a clear");

    write_file(fstat, "cpu  1241 0 0 1059 0 0 0 0\n");           /* 81% */
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 2450 0\n");   /* ~85% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_pct == 81 &&
           !r.cpu_entered && !r.disk_entered,
           "hysteresis: rising back to 81% is NOT a second entry");

    write_file(fstat, "cpu  1310 0 0 1090 0 0 0 0\n");           /* 69% */
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 3100 0\n");   /* ~65% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_pct == 69 &&
           !r.cpu_over  && r.cpu_cleared &&
           !r.disk_over && r.disk_cleared,
           "hysteresis: falling below clear_pct IS the clear (cpu+disk)");

    /* 10b. clear_pct 0 turns the band off — the same dip that 10 held through
     *      is a clear again, i.e. exactly the pre-#141 single-level rule. */
    opcd_fault_probe_init(&p);
    p.clear_pct = 0;
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_file(fstat, "cpu  1081 0 0 1019 0 0 0 0\n");           /* 81% */
    p.mono_ms -= 1000;
    (void)opcd_fault_probe_sample(&p, &r);                       /* entry */
    write_file(fstat, "cpu  1160 0 0 1040 0 0 0 0\n");           /* 79% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.cpu_over && r.cpu_cleared,
           "hysteresis off (clear_pct 0): 79% clears at the entry level");

    /* 10c. congestion_clear_pct parsing, and the clear <= threshold invariant
     *      restored after the loop so key order cannot leave a clear above the
     *      entry level (which would latch and never release). */
    write_file(conf, "congestion_clear_pct = 60\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 60, "hysteresis/conf: clear_pct override");
    write_file(conf, "congestion_clear_pct = 95\ncongestion_threshold_pct = 80\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 80,
           "hysteresis/conf: clear above threshold clamped (clear key first)");
    write_file(conf, "congestion_threshold_pct = 50\ncongestion_clear_pct = 90\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 50 && p.clear_pct == 50,
           "hysteresis/conf: clamp holds when clear key comes last");
    write_file(conf, "congestion_clear_pct = 101\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 70, "hysteresis/conf: >100 rejected, default kept");
    write_file(conf, "congestion_clear_pct = 0\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 70, "hysteresis/conf: 0 rejected, default kept");
    write_file(conf, "congestion_clear_pct = abc\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 70,
           "hysteresis/conf: unparseable rejected (endptr), default kept");
    write_file(conf, "congestion_clear_pct = 7O\n");   /* letter O, not zero */
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 70,
           "hysteresis/conf: trailing garbage rejected, default kept");
    write_file(conf, "congestion_clear_pct = 80\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.clear_pct == 80 && p.threshold_pct == 80,
           "hysteresis/conf: clear == threshold is the documented band-off form");

    /* 10d. The band width follows a configured entry threshold. Pinning init's
     *      70 would give a zero-width band for every threshold <= 70 — the
     *      pre-#141 storm, silently (reviewer A). */
    write_file(conf, "congestion_threshold_pct = 60\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 60 && p.clear_pct == 50,
           "hysteresis/conf: lowering only the threshold keeps a band (60/50)");
    write_file(conf, "congestion_threshold_pct = 50\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 50 && p.clear_pct == 40,
           "hysteresis/conf: threshold 50 keeps a band (50/40)");
    /* The derived width is capped at half the entry level, so a low threshold
     * keeps a proportional band instead of one that swallows the whole range
     * (a flat -10 would make threshold 10 release only below 1%). */
    write_file(conf, "congestion_threshold_pct = 20\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 20 && p.clear_pct == 10,
           "hysteresis/conf: threshold 20 keeps the full 10-point band (20/10)");
    write_file(conf, "congestion_threshold_pct = 10\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 10 && p.clear_pct == 5,
           "hysteresis/conf: threshold 10 halves the band (10/5), not 10/1");
    write_file(conf, "congestion_threshold_pct = 5\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 5 && p.clear_pct == 3,
           "hysteresis/conf: threshold 5 halves the band (5/3)");
    write_file(conf, "congestion_threshold_pct = 1\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 1 && p.clear_pct == 1,
           "hysteresis/conf: threshold 1 degenerates to a zero-width band, not 0");
    write_file(conf, "congestion_threshold_pct = 60\ncongestion_clear_pct = 55\n");
    opcd_fault_probe_init(&p); opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 60 && p.clear_pct == 55,
           "hysteresis/conf: an explicit clear is not overwritten by the derivation");
    unlink(conf);

    /* 10e. #141 + D4(i): rearm and reset_latch must clear ONLY the notify
     *      latch. Before the split they cleared the band state too, so a
     *      congestion that had settled inside [clear_pct, threshold_pct) was
     *      neither an entry nor a clear on the next sample — the notification
     *      rearm exists to save was lost for good (reviewer A, HIGH). */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_file(fstat, "cpu  1085 0 0 1015 0 0 0 0\n");           /* 85% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_entered,
           "rearm/band: 85% is the entry (send then fails at Period 0)");
    opcd_fault_probe_rearm(&p, OPCD_FAULT_RES_CPU);
    write_file(fstat, "cpu  1160 0 0 1040 0 0 0 0\n");           /* 75%, in band */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_pct == 75 &&
           r.cpu_over && r.cpu_entered && !r.cpu_cleared,
           "rearm/band: a congestion resting inside the band re-enters after rearm");

    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_file(fstat, "cpu  1085 0 0 1015 0 0 0 0\n");           /* 85% */
    p.mono_ms -= 1000;
    (void)opcd_fault_probe_sample(&p, &r);                       /* entry */
    write_file(fstat, "cpu  1160 0 0 1040 0 0 0 0\n");           /* 75%, in band */
    p.mono_ms -= 1000;
    (void)opcd_fault_probe_sample(&p, &r);                       /* still congested */
    opcd_fault_probe_reset_latch(&p);                            /* new recipient */
    write_file(fstat, "cpu  1235 0 0 1065 0 0 0 0\n");           /* 75%, in band */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.cpu_pct == 75 && r.cpu_entered,
           "reset_latch/band: a new recipient is told about a congestion inside the band");

    /* 10f. The band applies to the network resource too — the rule is one
     *      shared helper, but nothing exercised net through the sampler. The
     *      channel occupancy is a ratio of the file's own counters (Δbusy /
     *      Δactive, #192), so it is exact and the 80 / 70 edges are hit on the
     *      dot: 79 stays out, 80 enters, 70 holds, 69 clears. */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_link_json(flink, true, 10000, 0, 900000);
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_link_json(flink, true, 11000, 790, 901000);            /* 79% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 79 &&
           !r.net_over && !r.net_entered,
           "hysteresis/net: 79% is below the entry level");
    write_link_json(flink, true, 12000, 1590, 902000);           /* 80% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 80 &&
           r.net_over && r.net_entered,
           "hysteresis/net: reaching the entry level (80%) is an ENTRY");
    write_link_json(flink, true, 13000, 2290, 903000);           /* 70%, in band */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 70 &&
           r.net_over && !r.net_cleared && !r.net_entered,
           "hysteresis/net: a dip to clear_pct (70%) is NOT a clear");
    write_link_json(flink, true, 14000, 2980, 904000);           /* 69% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 69 &&
           !r.net_over && r.net_cleared,
           "hysteresis/net: falling below clear_pct (69%) IS the clear");

    /* 11. #182 Memory (0x0002). The target is swapless, so the spec's
     *     swap-paging definition is replaced by total memory usage, computed
     *     like sysstat's sar -r %memused (the MEM figure in cpu.log):
     *     used = MemTotal - MemFree - Buffers - Cached - Slab. */
    {
        uint64_t used = 0, mtot = 0;
        /* Board sample of 2026-10-21 (sysstat 12.7.4): sar printed kbmemused
         * 247924 for exactly these fields. SwapCached follows Cached as in the
         * kernel's order, so a substring key match would overwrite Cached. */
        const char *mi =
            "MemTotal:        1961704 kB\n"
            "MemFree:         1435244 kB\n"
            "MemAvailable:    1556908 kB\n"
            "Buffers:           39032 kB\n"
            "Cached:           189180 kB\n"
            "SwapCached:         4242 kB\n"
            "Slab:              50324 kB\n"
            "SReclaimable:      22700 kB\n";
        ASSERT(opcd_fault_parse_meminfo(mi, &used, &mtot) == 0 &&
               mtot == 1961704 && used == 247924,
               "meminfo: used = total-free-buffers-cached-slab (sar kbmemused 247924)");
        ASSERT(opcd_fault_parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\n"
                                        "Buffers: 0 kB\nCached: 0 kB\n",
                                        &used, &mtot) != 0,
               "meminfo: a missing field (Slab) is unknown, not zero");
        ASSERT(opcd_fault_parse_meminfo("MemTotal: 0 kB\nMemFree: 0 kB\nBuffers: 0 kB\n"
                                        "Cached: 0 kB\nSlab: 0 kB\n",
                                        &used, &mtot) != 0,
               "meminfo: MemTotal 0 rejected");
        ASSERT(opcd_fault_parse_meminfo("MemTotal: 1000 kB\nMemFree: 600 kB\n"
                                        "Buffers: 200 kB\nCached: 200 kB\nSlab: 100 kB\n",
                                        &used, &mtot) == 0 && mtot == 1000 && used == 0,
               "meminfo: not-in-use sum above total clamps used to 0 (no wrap)");
        /* The sum saturates at MemTotal before it can wrap: a plain uint64
         * addition here would wrap to 1 kB not-in-use and report 999/1000. */
        ASSERT(opcd_fault_parse_meminfo("MemTotal: 1000 kB\nMemFree: 18446744073709551615 kB\n"
                                        "Buffers: 2 kB\nCached: 0 kB\nSlab: 0 kB\n",
                                        &used, &mtot) == 0 && mtot == 1000 && used == 0,
               "meminfo: an absurd field saturates instead of wrapping to ~100% used");
    }
    opcd_fault_probe_init(&p);
    opcd_fault_evaluate(&p, 0, 0, 0, 1000, 0, 0, 850, 1000, &r);
    ASSERT(r.mem_over && r.mem_pct == 85,   "evaluate: memory 85% over at 80");
    opcd_fault_evaluate(&p, 0, 0, 0, 1000, 0, 0, 790, 1000, &r);
    ASSERT(!r.mem_over && r.mem_pct == 79,  "evaluate: memory 79% under 80");
    opcd_fault_evaluate(&p, 0, 0, 0, 1000, 0, 0, 850, 0, &r);
    ASSERT(!r.mem_over && r.mem_pct == 0,   "evaluate: memory total 0 (unread) is not over");

    /* 11b. Through the sampler. Memory is a level, not a counter, yet it shares
     *      the first-sample rule (the priming call reports nothing) and the
     *      entry latch, band, rearm, read-failure and reset_latch handling of
     *      the other resources. The other sources are dead so only memory flags. */
    {
        char fmem[96];                       /* fits path_meminfo whole */
        snprintf(fmem, sizeof fmem, "%s/meminfo", d);
        opcd_fault_probe_init(&p);
        ASSERT(strcmp(p.path_meminfo, "/proc/meminfo") == 0,
               "memory: default source is /proc/meminfo");
        snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
        snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
        snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s/none", d);
        snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s", fmem);
        write_meminfo(fmem, 1000000, 850000);                        /* 85% */
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.mem_over && !r.mem_entered,
               "memory: priming call reports nothing (shared first-sample rule)");
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_over && r.mem_pct == 85 &&
               r.mem_entered && !r.cpu_entered && !r.disk_entered && !r.net_entered,
               "memory: 85% is an ENTRY (memory only)");
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_over && !r.mem_entered,
               "memory: persisting congestion is NOT entered again");
        write_meminfo(fmem, 1000000, 750000);                        /* 75%, in band */
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_over && r.mem_pct == 75 &&
               !r.mem_cleared && !r.mem_entered,
               "memory: a dip inside the band is NOT a clear");
        opcd_fault_probe_rearm(&p, OPCD_FAULT_RES_MEM);
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_over && r.mem_entered,
               "memory: rearm re-enters a congestion resting inside the band");
        unlink(fmem);
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.mem_over &&
               !r.mem_entered && !r.mem_cleared,
               "memory: unreadable source is neither entry nor clear");
        write_meminfo(fmem, 1000000, 650000);                        /* 65% */
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.mem_over && r.mem_cleared,
               "memory: falling below clear_pct IS the clear (latch held across the read failure)");
        write_meminfo(fmem, 1000000, 900000);                        /* 90% */
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_entered,
               "memory: rising again after a clear is a fresh ENTRY");
        opcd_fault_probe_reset_latch(&p);
        p.mono_ms -= 1000;
        ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.mem_over && r.mem_entered,
               "memory: reset_latch makes an ongoing congestion a fresh ENTRY");
        unlink(fmem);
    }

    /* 12. #192 Network (0x0004) through the sampler: the operating channel's
     *     cumulative active / busy time from link.json survey_in_use, stamped
     *     with the logger's monotonic mono_ms. A delta counts only when both
     *     counters and the stamp moved forward and Δactive matches Δmono within
     *     OPCD_FAULT_NET_CLOCK_SLACK_MS; anything else is UNKNOWN — neither
     *     entry nor clear, latch held — and re-baselines. Only the network
     *     source is live; the probe's own elapsed time plays no part. */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_link_json(flink, true, 50000, 5000, 200000);
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.net_over && !r.net_entered,
           "net: priming call reports nothing");
    write_link_json(flink, true, 51000, 5500, 201000);           /* 500 of 1000 */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 50 &&
           !r.net_over && !r.net_entered,
           "net: busy 500 of 1000 ms active = 50%, under");
    write_link_json(flink, true, 52000, 6700, 202000);           /* 1200 of 1000 */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 100 &&
           r.net_over && r.net_entered,
           "net: busy above active clamps to 100% (an ENTRY)");
    p.mono_ms -= 1000;          /* logger stopped: file and its mono_ms unchanged */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net: a stale link.json (Δmono 0) is UNKNOWN, not a 0% clear");
    /* Each guard on its own, inside the clock slack: a stamp that did not move
     * while active did, and active that did not move while the stamp did. */
    write_link_json(flink, true, 52300, 6900, 202000);
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net: mono_ms not advancing (active +300) is UNKNOWN, not a 66% clear");
    write_link_json(flink, true, 52300, 6900, 202300);
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net: active not advancing (mono +300) is UNKNOWN, not a clear");
    write_link_json(flink, true, 54300, 8700, 204300);  /* resumed: 1800 of 2000 */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 90 &&
           r.net_over && !r.net_entered,
           "net: the latch held through the stale sample (no second ENTRY)");
    write_link_json(flink, true, 600, 500, 205000);     /* roam: counters restart */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net: a roam reset (active went down) is UNKNOWN, latch held");
    /* busy alone going down while active kept pace with mono — a counter
     * anomaly the clock check cannot see */
    write_link_json(flink, true, 1600, 100, 206000);
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net: busy going down alone (active kept pace) is UNKNOWN, not 100%");
    write_link_json(flink, true, 2600, 1000, 207000);            /* 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 90 &&
           r.net_over && !r.net_entered,
           "net: measured again from the post-roam baseline, latch held (no second ENTRY)");
    write_link_json(flink, true, 3600, 1100, 208000);            /* 10% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 10 &&
           !r.net_over && r.net_cleared,
           "net: a real drop after the roam is the clear");

    /* 12a. Wall-clock steps (tribunal A-R1-001). moal measures active time on
     *      CLOCK_REALTIME while busy is firmware airtime, so a step inside the
     *      window skews Δbusy / Δactive; the logger's mono_ms does not step.
     *      10 s windows (Δmono 10000). */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_link_json(flink, true, 100000, 10000, 1000000);
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_link_json(flink, true, 110000, 15000, 1010000);        /* 50% */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 50 && !r.net_over,
           "net/clock: 5000 of 10000 ms = 50%, under");
    /* stepped back 2.5 s: read as 6000 / 7500 it would be a false 80% ENTRY */
    write_link_json(flink, true, 117500, 21000, 1020000);
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/clock: clock stepped back (Δmono 10000, Δactive 7500, Δbusy 6000) is UNKNOWN, no ENTRY");
    write_link_json(flink, true, 127500, 29500, 1030000);        /* 85% */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 85 &&
           r.net_over && r.net_entered,
           "net/clock: the step re-baselined; a real 85% is the ENTRY");
    /* stepped forward 2.5 s: read as 6000 / 12500 it would be a false 48% clear */
    write_link_json(flink, true, 140000, 35500, 1040000);
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/clock: clock stepped forward (Δmono 10000, Δactive 12500) is UNKNOWN, the held latch does not clear");
    write_link_json(flink, true, 150400, 40700, 1050000);        /* 5200 of 10400 */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 50 &&
           !r.net_over && r.net_cleared,
           "net/clock: Δactive 400 ms over Δmono is jitter — measured (50%, the clear)");
    write_link_json(flink, true, 160000, 45500, 1060000);        /* 4800 of 9600 */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 50 && !r.net_over,
           "net/clock: Δactive 400 ms under Δmono is jitter — measured (50%)");

    /* 12b. A failed read — survey_in_use without mono_ms, no survey_in_use,
     *      no link.json — is UNKNOWN like any unreadable source: the latch
     *      holds, and the next good read re-primes instead of spanning the gap. */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
    snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);   /* host-independent */
    write_link_json(flink, true, 1000, 0, 300000);
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_link_json(flink, true, 2000, 900, 301000);             /* 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_entered,
           "net/readfail: entry");
    write_link_json(flink, true, 3000, 1000, -1);        /* no mono_ms: 10% unstamped */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/readfail: survey_in_use without mono_ms is UNKNOWN (no 10% clear)");
    write_link_json(flink, true, -1, 0, 0);                      /* no survey row */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/readfail: link.json without survey_in_use is UNKNOWN");
    unlink(flink);
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_pct == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/readfail: a missing link.json is UNKNOWN");
    write_link_json(flink, true, 6000, 3600, 306000);
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           !r.net_over && !r.net_entered && !r.net_cleared,
           "net/readfail: the first good read re-primes (nothing reported)");
    write_link_json(flink, true, 7000, 4500, 307000);            /* 90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_over && !r.net_entered,
           "net/readfail: recovered over-sample is NOT a duplicate entry");
    write_link_json(flink, true, 8000, 5000, 308000);            /* 50% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_cleared,
           "net/readfail: a real drop after recovery is the clear");

    /* 12c. Associated (link section present) yet no survey_in_use, sample after
     *      sample: the firmware is probably not reporting the channel busy
     *      time. ONE stderr line after OPCD_FAULT_NET_MISSING_LOG_SAMPLES (6)
     *      such samples — none before (a scan can hide the row for a moment),
     *      no repeat after; a good read re-arms it. A station that is not
     *      associated has no operating channel and never counts. */
    {
        const char *diag = "no usable operating-channel survey";
        int n_unassoc, n_5, n_6, n_9, n_rearm;
        opcd_fault_probe_init(&p);
        snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
        snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
        snprintf(p.net_link_json,  sizeof p.net_link_json,  "%s", flink);
        snprintf(p.path_meminfo,   sizeof p.path_meminfo,   "%s/none", d);
        int saved = stderr_to(errf);
        write_link_json(flink, false, -1, 0, 0);                 /* not associated */
        for (int i = 0; i < 8; i++) (void)opcd_fault_probe_sample(&p, &r);
        n_unassoc = count_lines(errf, diag);
        write_link_json(flink, true, -1, 0, 0);                  /* associated, no row */
        for (int i = 0; i < 5; i++) (void)opcd_fault_probe_sample(&p, &r);
        n_5 = count_lines(errf, diag);
        (void)opcd_fault_probe_sample(&p, &r);
        n_6 = count_lines(errf, diag);
        for (int i = 0; i < 3; i++) (void)opcd_fault_probe_sample(&p, &r);
        n_9 = count_lines(errf, diag);
        write_link_json(flink, true, 1000, 100, 400000);         /* a good read */
        (void)opcd_fault_probe_sample(&p, &r);
        write_link_json(flink, true, -1, 0, 0);
        for (int i = 0; i < 6; i++) (void)opcd_fault_probe_sample(&p, &r);
        n_rearm = count_lines(errf, diag);
        stderr_restore(saved);
        ASSERT(n_unassoc == 0,
               "net/diag: an unassociated station without survey_in_use never logs");
        ASSERT(n_5 == 0,
               "net/diag: 5 associated samples without the survey row stay quiet");
        ASSERT(n_6 == 1,
               "net/diag: the 6th associated sample without the survey row logs ONE line");
        ASSERT(n_9 == 1, "net/diag: further missing samples do not repeat it");
        ASSERT(n_rearm == 2,
               "net/diag: a good read re-arms it — 6 more missing samples log again");
    }

    unlink(fstat); unlink(fdisk); unlink(flink); unlink(errf);
    rmdir(d);

    if (failures == 0) {
        fprintf(stdout, "all fault-probe tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
