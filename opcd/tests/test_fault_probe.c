/*
 * Unit tests for the FaultDetect congestion probe (T6 interim policy).
 * Pure parsers and the evaluator are tested with synthetic inputs; the
 * end-to-end sample() path runs against synthetic source files (CWD-relative
 * temp paths, same pattern as the other opcd tests).
 */

#define _POSIX_C_SOURCE 200809L
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
    opcd_fault_evaluate(&p, 90, 100, 850, 1000, 0, 0, &r);
    ASSERT(r.cpu_over && r.cpu_pct == 90,    "evaluate: cpu 90% over at 80");
    ASSERT(r.disk_over && r.disk_pct == 85,  "evaluate: disk 85% over at 80");
    ASSERT(!r.net_over,                      "evaluate: zero traffic not over");
    opcd_fault_evaluate(&p, 79, 100, 0, 1000,
                        11250000 /* bytes in 1 s = 90 Mbit/s */, 100, &r);
    ASSERT(!r.cpu_over && r.cpu_pct == 79,   "evaluate: cpu 79% under 80");
    /* Network current_val is utilisation % (DFK 2026-06-29, 사용률 0-100%), not raw
     * Mbps: 90 Mbit/s on a 100 Mbit/s link = 90% → over 80%. */
    ASSERT(r.net_over && r.net_pct == 90,    "evaluate: 90Mbps/100link = 90% over 80");
    opcd_fault_evaluate(&p, 0, 100, 0, 1000,
                        9000000 /* 72 Mbit/s */, 100, &r);
    ASSERT(!r.net_over && r.net_pct == 72,   "evaluate: 72Mbps/100link = 72% under 80");
    /* %, not Mbps: the same 90 Mbit/s on a 200 Mbit/s link = 45% → under 80. */
    opcd_fault_evaluate(&p, 0, 100, 0, 1000,
                        11250000 /* 90 Mbit/s */, 200, &r);
    ASSERT(!r.net_over && r.net_pct == 45,   "evaluate: 90Mbps/200link = 45% under 80");
    /* over-link traffic saturates at 100%, never wraps past it. */
    opcd_fault_evaluate(&p, 0, 100, 0, 1000,
                        25000000 /* 200 Mbit/s */, 100, &r);
    ASSERT(r.net_over && r.net_pct == 100,   "evaluate: 200Mbps/100link clamps to 100%");
    /* sub-Mbps precision on a slow link (Gemini review): 9.7 Mbit/s on a
     * 10 Mbit/s link = 97%, not 90% — a whole-Mbps floor before the % would
     * drop the 0.7 Mbit/s and read 9 Mbps → 90%. */
    opcd_fault_evaluate(&p, 0, 100, 0, 1000,
                        1212500 /* 9.7 Mbit/s = 1212500 B/s */, 10, &r);
    ASSERT(r.net_over && r.net_pct == 97,    "evaluate: 9.7Mbps/10link = 97% (no Mbps floor)");

    /* 4. conf overrides (key=value; unknown keys and comments ignored). */
    char conf[64];
    snprintf(conf, sizeof conf, "test_fp_conf_%d.tmp", (int)getpid());
    write_file(conf,
        "# congestion overrides\n"
        "udp_port = 50607\n"
        "congestion_threshold_pct = 50\n"
        "congestion_disk_dev = sda\n"
        "congestion_net_if = wlan9\n"
        "congestion_net_capacity_mbps = 100\n");
    opcd_fault_probe_init(&p);
    opcd_fault_probe_conf(&p, conf);
    ASSERT(p.threshold_pct == 50 &&
           strcmp(p.disk_dev, "sda") == 0 &&
           strcmp(p.net_dir, "/sys/class/net/wlan9") == 0 &&
           p.net_capacity_mbps == 100,
           "conf: overrides applied, unknown keys ignored");
    opcd_fault_probe_conf(&p, "test_fp_no_such_file.conf");
    ASSERT(p.threshold_pct == 50, "conf: missing file leaves settings");
    unlink(conf);

    /* 5. end-to-end sample() against synthetic source files. */
    char d[64], sub[96];
    snprintf(d, sizeof d, "test_fp_%d", (int)getpid());
    snprintf(sub, sizeof sub, "%s/statistics", d);
    mkdir(d, 0755);
    mkdir(sub, 0755);
    char fstat[128], fdisk[128], frx[160], ftx[160], fspd[128];
    snprintf(fstat, sizeof fstat, "%s/stat", d);
    snprintf(fdisk, sizeof fdisk, "%s/diskstats", d);
    snprintf(frx,   sizeof frx,   "%s/statistics/rx_bytes", d);
    snprintf(ftx,   sizeof ftx,   "%s/statistics/tx_bytes", d);
    snprintf(fspd,  sizeof fspd,  "%s/speed", d);
    write_file(fstat, "cpu  0 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    write_file(frx, "0\n");
    write_file(ftx, "0\n");
    write_file(fspd, "100\n");

    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           !r.cpu_over && !r.disk_over && !r.net_over,
           "sample: priming call reports nothing");

    /* advance: cpu 900/1000 busy, disk 950 ms, net 11.25 MB (~90 Mbit/s) */
    write_file(fstat, "cpu  900 0 0 1100 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 950 0\n");
    write_file(frx, "5625000\n");
    write_file(ftx, "5625000\n");
    p.mono_ms -= 1000;                       /* pretend one second elapsed */
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0, "sample: second call ok");
    ASSERT(r.cpu_over && r.cpu_pct == 90,    "sample: cpu 90% detected");
    ASSERT(r.disk_over && r.disk_pct >= 90,  "sample: disk ~95% detected");
    ASSERT(r.net_over && r.net_pct >= 85,    "sample: ~90% detected (90Mbps/100link)");

    /* 6. unreadable sources leave resources un-flagged (fail-soft). */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s/none", d);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s/none", d);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s/none", d);
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
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s/none", d);
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
    write_file(frx, "0\n"); write_file(ftx, "0\n");
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
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

    /* 8b. #121 a transient source read failure is UNKNOWN, not "below
     *     threshold": the latch must hold, so the recovered over-sample is
     *     not a duplicate ENTRY. cpu over → stat unreadable → stat readable
     *     and still over: no second entry, no clear in between. */
    write_file(fstat, "cpu  0 0 0 1000 0 0 0 0\n");
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
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

    unlink(fstat); unlink(fdisk); unlink(frx); unlink(ftx); unlink(fspd);
    rmdir(sub); rmdir(d);

    if (failures == 0) {
        fprintf(stdout, "all fault-probe tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
