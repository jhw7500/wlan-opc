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
    write_file(frx, "0\n"); write_file(ftx, "0\n");
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
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
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
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
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    write_file(frx, "0\n"); write_file(ftx, "0\n");
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
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
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
     *      shared helper, but nothing exercised net through the sampler. */
    opcd_fault_probe_init(&p);
    snprintf(p.path_proc_stat, sizeof p.path_proc_stat, "%s", fstat);
    snprintf(p.path_diskstats, sizeof p.path_diskstats, "%s", fdisk);
    snprintf(p.net_dir,        sizeof p.net_dir,        "%s", d);
    write_file(fspd, "100\n");                     /* link 100 Mbit/s is the
                                                    * authoritative source; it
                                                    * overrides net_capacity_mbps.
                                                    * pct = bytes*8 / (ms*10*mbps)
                                                    * → 1% == 125000 B per 1000 ms */
    write_file(fstat, "cpu  1000 0 0 1000 0 0 0 0\n");
    write_file(fdisk, " 179 0 mmcblk0 0 0 0 0 0 0 0 0 0 0 0\n");
    write_file(frx, "0\n"); write_file(ftx, "0\n");
    (void)opcd_fault_probe_sample(&p, &r);                       /* prime */
    write_file(frx, "11250000\n");                               /* ~90% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && r.net_over && r.net_entered,
           "hysteresis/net: above the entry level is an ENTRY");
    write_file(frx, "20625000\n");                               /* +9375000 → ~75% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 &&
           r.net_over && !r.net_cleared && !r.net_entered,
           "hysteresis/net: a dip inside the band is NOT a clear");
    write_file(frx, "28750000\n");                               /* +8125000 → ~65% */
    p.mono_ms -= 1000;
    ASSERT(opcd_fault_probe_sample(&p, &r) == 0 && !r.net_over && r.net_cleared,
           "hysteresis/net: falling below clear_pct IS the clear");

    unlink(fstat); unlink(fdisk); unlink(frx); unlink(ftx); unlink(fspd);
    rmdir(sub); rmdir(d);

    if (failures == 0) {
        fprintf(stdout, "all fault-probe tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
