#include <stdio.h>
#include <string.h>

#include "freq_source.h"

opcd_freq_source_t opcd_freq_source_from_token(const char *val)
{
    if (!val) return OPC_FREQ_SRC_CONFIG;
    if (strcmp(val, "live") == 0) return OPC_FREQ_SRC_LIVE;
    if (strcmp(val, "auto") == 0) return OPC_FREQ_SRC_AUTO;
    return OPC_FREQ_SRC_CONFIG;   /* "config" and anything unrecognized */
}

opcd_freq_source_t opcd_freq_source_parse(const char *conf_path)
{
    opcd_freq_source_t src = OPC_FREQ_SRC_CONFIG;
    if (!conf_path) return src;
    FILE *f = fopen(conf_path, "r");
    if (!f) return src;                        /* no conf file → default */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        /* Over-long line (no newline read and not at EOF): discard the rest of
         * the physical line so its tail is not re-parsed as a separate directive
         * (Gemini review, PR #61). */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { /* skip to line end */ }
            continue;   /* a >159B line is malformed for this key — ignore it */
        }
        char key[48], val[64];
        /* Same scan as opcd_fault_probe_conf: accepts "key=value" and
         * "key = value"; '#'-comment lines fail the key match and are skipped.
         * NOTE: %63s stops at whitespace, so a trailing "value#comment" with no
         * space before '#' is taken literally and falls back to CONFIG — put a
         * space before inline comments. */
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "device_info_freq_source") != 0) continue;
        src = opcd_freq_source_from_token(val);   /* last occurrence wins */
    }
    fclose(f);
    return src;
}
