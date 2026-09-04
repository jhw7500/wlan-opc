/* Host unit test for the autonomous-reset ResetNotice producer helpers
 * (opcd/reset_cause.{c,h}, issue #47 item 3 / T9).
 *
 *   - opcd_reset_cause_read: the cause-ID file the reboot policy script may
 *     leave at /run/opc/reset_cause (hex or decimal, one line)
 *   - opcd_system_stopping: `systemctl is-system-running` == "stopping",
 *     bounded by a timeout (fake systemctl scripts here)
 *   - opcd_shutdown_reset_cause: the decision — notify only when the system
 *     is going down; file cause wins, else OPC_RESET_CAUSE_SYSTEM */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../reset_cause.h"
#include "../../protocol/ids.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static char g_file[64], g_fake[64];

static void write_file(const char *path, const char *text, int exec)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
    if (exec) chmod(path, 0755);
}

int main(void)
{
    snprintf(g_file, sizeof g_file, "test_reset_cause_%d.tmp", (int)getpid());
    snprintf(g_fake, sizeof g_fake, "./test_reset_cause_%d.sh", (int)getpid());
    unlink(g_file); unlink(g_fake);

    /* ---- cause file ---- */
    ASSERT(opcd_reset_cause_read("/nonexistent/reset_cause") == 0, "file: absent -> 0");
    ASSERT(opcd_reset_cause_read(NULL) == 0, "file: NULL path -> 0");
    write_file(g_file, "0x12\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0x12u, "file: hex 0x12");
    write_file(g_file, "  0x0011 \n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0x11u, "file: hex with whitespace");
    write_file(g_file, "32\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 32u, "file: decimal 32");
    write_file(g_file, "0\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: 0 is not a cause -> 0");
    write_file(g_file, "zz\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: garbage -> 0");
    write_file(g_file, "0x12 trailing\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: trailing junk -> 0");
    write_file(g_file, "0x100000000\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: > 32-bit -> 0");
    write_file(g_file, "", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: empty -> 0");
    /* the contract is "0x.. hex or decimal" — a leading zero is decimal, never
     * octal (a writer using printf %03d must not produce a WRONG cause id) */
    write_file(g_file, "012\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 12u, "file: '012' is decimal 12, not octal 10");
    write_file(g_file, "-1\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: negative -> 0");
    write_file(g_file, "-18446744073709551615\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: huge negative (wraps to 1) -> 0");
    write_file(g_file, "+0x12\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: explicit '+' sign -> 0");
    write_file(g_file, "0x\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0, "file: bare '0x' -> 0");
    write_file(g_file, "0X12\n", 0);
    ASSERT(opcd_reset_cause_read(g_file) == 0x12u, "file: '0X' prefix accepted");
    unlink(g_file);

    /* ---- systemctl is-system-running probe (fake scripts) ---- */
    write_file(g_fake, "#!/bin/sh\necho stopping\n", 1);
    ASSERT(opcd_system_stopping(g_fake, 500) == 1, "probe: 'stopping' -> 1");
    write_file(g_fake, "#!/bin/sh\necho running\nexit 0\n", 1);
    ASSERT(opcd_system_stopping(g_fake, 500) == 0, "probe: 'running' -> 0");
    write_file(g_fake, "#!/bin/sh\necho degraded\nexit 1\n", 1);
    ASSERT(opcd_system_stopping(g_fake, 500) == 0, "probe: 'degraded' (non-zero exit) -> 0");
    write_file(g_fake, "#!/bin/sh\nsleep 3\necho stopping\n", 1);
    ASSERT(opcd_system_stopping(g_fake, 100) == -1, "probe: timeout -> -1 (unknown)");
    /* the timeout is a DEADLINE for the whole probe, not a per-read timer: a
     * dribbling writer must not stretch the shutdown */
    write_file(g_fake, "#!/bin/sh\nfor i in 1 2 3 4 5 6 7 8; do printf s; sleep 0.1; done\necho topping\n", 1);
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = opcd_system_stopping(g_fake, 200);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        ASSERT(r == -1 && ms < 600, "probe: dribbling writer hits the 200 ms deadline (-1, < 600 ms)");
    }
    write_file(g_fake, "#!/bin/sh\nexit 0\n", 1);
    ASSERT(opcd_system_stopping(g_fake, 500) == 0, "probe: no output -> 0");
    ASSERT(opcd_system_stopping("/nonexistent/systemctl", 500) == -1, "probe: exec failure -> -1");
    ASSERT(opcd_system_stopping(NULL, 500) == -1, "probe: NULL path -> -1");
    unlink(g_fake);

    /* ---- decision ---- */
    ASSERT(opcd_shutdown_reset_cause(1, 0) == OPC_RESET_CAUSE_SYSTEM,
           "decide: stopping + no file -> SYSTEM");
    ASSERT(opcd_shutdown_reset_cause(1, 0x12u) == 0x12u,
           "decide: stopping + file cause -> file cause");
    ASSERT(opcd_shutdown_reset_cause(0, 0x12u) == 0,
           "decide: not stopping -> no notice (plain `systemctl stop opcd`)");
    ASSERT(opcd_shutdown_reset_cause(-1, 0x12u) == 0,
           "decide: unknown -> no notice (never guess a reset)");
    ASSERT(OPC_RESET_CAUSE_USER == 0x1u && OPC_RESET_CAUSE_SYSTEM == 0x2u,
           "ids: USER=1, SYSTEM=2 (vendor-defined table)");
    /* on-target 2026-09-04: the first `is-system-running` answer during a real
     * shutdown took 1129 ms (200 ms lost the notice) — keep ≥ 2× that */
    ASSERT(OPC_RESET_PROBE_TIMEOUT_MS >= 2300, "probe deadline covers the measured shutdown latency (1129 ms x2)");

    if (failures == 0) { printf("all reset-cause tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
