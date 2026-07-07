/* Host unit test for the pure opc.conf roam_notify_port parser
 * (opcd/roam_notify_conf.{c,h}). Extracted from opcd.c so it links without
 * main(). Mirrors test_freq_source.c: missing file -> default, valid value,
 * out-of-range -> default, duplicate key -> last wins. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../roam_notify_conf.h"

#define DFLT 50608

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static char g_path[64];

static void write_conf(const char *text)
{
    FILE *f = fopen(g_path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "test_roam_notify_conf_%d.tmp", (int)getpid());
    unlink(g_path);

    /* ---- missing file / NULL path -> default ---- */
    ASSERT(opcd_roam_notify_port_parse("/nonexistent_dir/opc.conf", DFLT) == DFLT,
           "missing file -> default");
    ASSERT(opcd_roam_notify_port_parse(NULL, DFLT) == DFLT, "NULL path -> default");

    /* ---- valid value ---- */
    write_conf("roam_notify_port = 51000\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 51000, "valid value");

    write_conf("roam_notify_port=51001\n");   /* no spaces around '=' */
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 51001, "no-space '=' -> value");

    /* boundary values 1 and 65535 accepted */
    write_conf("roam_notify_port = 1\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 1, "min boundary 1");
    write_conf("roam_notify_port = 65535\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 65535, "max boundary 65535");

    /* ---- out of range -> default ---- */
    write_conf("roam_notify_port = 0\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "zero -> default");
    write_conf("roam_notify_port = 65536\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "over 65535 -> default");
    write_conf("roam_notify_port = 99999999\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "way over -> default");

    /* ---- non-numeric / garbage -> default ---- */
    write_conf("roam_notify_port = abc\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "non-numeric -> default");
    write_conf("roam_notify_port = 5060x\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "trailing garbage -> default");

    /* ---- unrelated key + comment ignored -> default ---- */
    write_conf("congestion_threshold_pct = 80\n# roam_notify_port = 51000\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT,
           "other-key/comment ignored -> default");

    /* ---- duplicate key -> last wins ---- */
    write_conf("roam_notify_port = 51000\nroam_notify_port = 52000\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 52000, "duplicate key -> last wins");

    /* last-wins keeps the last VALID value: an out-of-range trailing entry is
     * rejected and does not clobber the earlier valid one. */
    write_conf("roam_notify_port = 51000\nroam_notify_port = 70000\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 51000,
           "trailing out-of-range keeps last valid");

    /* '#' inline comment with NO space: %63s takes "51000#x" as the token →
     * strtol trailing garbage → rejected → default (documented edge). */
    write_conf("roam_notify_port = 51000#nospace\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT, "value#comment no-space -> default");
    write_conf("roam_notify_port = 51000 # spaced comment\n");
    ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == 51000, "value + spaced #comment -> value");

    /* over-long line (>159B) is discarded, not split: a directive sitting in
     * its tail must NOT take effect (mirrors freq_source, Gemini review PR #61). */
    {
        char buf[256];
        memset(buf, '#', 159);
        strcpy(buf + 159, "roam_notify_port = 51000\n");
        write_conf(buf);
        ASSERT(opcd_roam_notify_port_parse(g_path, DFLT) == DFLT,
               "over-long line tail not parsed -> default");
    }

    unlink(g_path);

    if (failures == 0) { printf("all roam_notify_conf tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
