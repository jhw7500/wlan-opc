/* Host unit test for the pure opc.conf device_info_freq_source parser
 * (opcd/freq_source.{c,h}). Extracted from opcd.c so it links without main().
 * Covers the gap flagged in PR #60 review: unknown token → CONFIG, duplicate
 * key → last wins, missing file → CONFIG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../freq_source.h"

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
    /* ---- token mapper ---- */
    ASSERT(opcd_freq_source_from_token("config") == OPC_FREQ_SRC_CONFIG, "token config");
    ASSERT(opcd_freq_source_from_token("live")   == OPC_FREQ_SRC_LIVE,   "token live");
    ASSERT(opcd_freq_source_from_token("auto")   == OPC_FREQ_SRC_AUTO,   "token auto");
    ASSERT(opcd_freq_source_from_token("bogus")  == OPC_FREQ_SRC_CONFIG, "token unknown -> config");
    ASSERT(opcd_freq_source_from_token("")       == OPC_FREQ_SRC_CONFIG, "token empty -> config");
    ASSERT(opcd_freq_source_from_token(NULL)     == OPC_FREQ_SRC_CONFIG, "token NULL -> config");
    ASSERT(opcd_freq_source_from_token("AUTO")   == OPC_FREQ_SRC_CONFIG, "token case-sensitive -> config");

    /* ---- file parser ---- */
    snprintf(g_path, sizeof g_path, "test_freq_source_%d.tmp", (int)getpid());
    unlink(g_path);

    ASSERT(opcd_freq_source_parse("/nonexistent_dir/opc.conf") == OPC_FREQ_SRC_CONFIG,
           "missing file -> config");
    ASSERT(opcd_freq_source_parse(NULL) == OPC_FREQ_SRC_CONFIG, "NULL path -> config");

    write_conf("device_info_freq_source = live\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_LIVE, "file live");

    write_conf("device_info_freq_source = auto\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_AUTO, "file auto");

    write_conf("device_info_freq_source = config\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_CONFIG, "file config");

    write_conf("device_info_freq_source=live\n");   /* no spaces around '=' */
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_LIVE, "no-space '=' -> live");

    /* unrelated key + commented line ignored -> default config */
    write_conf("congestion_threshold_pct = 80\n# device_info_freq_source = live\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_CONFIG, "other-key/comment ignored -> config");

    /* duplicate key -> last wins */
    write_conf("device_info_freq_source = live\ndevice_info_freq_source = auto\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_AUTO, "duplicate key -> last wins");

    /* unknown value -> config fallback */
    write_conf("device_info_freq_source = bogus\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_CONFIG, "unknown value -> config");

    /* '#' inline comment with NO space: %63s takes "auto#x" as the token →
     * unknown → config (documented edge; a space before '#' avoids it). */
    write_conf("device_info_freq_source = auto#nospace\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_CONFIG, "value#comment no-space -> config");
    write_conf("device_info_freq_source = auto # spaced comment\n");
    ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_AUTO, "value + spaced #comment -> auto");

    /* over-long line (>159B) is discarded, not split: a directive sitting in its
     * tail must NOT take effect (Gemini review PR #61). 159 ignorable chars then
     * the directive, all on one physical line. */
    {
        char buf[256];
        memset(buf, '#', 159);
        strcpy(buf + 159, "device_info_freq_source = live\n");
        write_conf(buf);
        ASSERT(opcd_freq_source_parse(g_path) == OPC_FREQ_SRC_CONFIG,
               "over-long line tail not parsed -> config");
    }

    unlink(g_path);

    if (failures == 0) { printf("all freq_source tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
