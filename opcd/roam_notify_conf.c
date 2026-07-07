#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roam_notify_conf.h"

uint16_t opcd_roam_notify_port_parse(const char *conf_path, uint16_t dflt)
{
    uint16_t port = dflt;
    if (!conf_path) return port;
    FILE *f = fopen(conf_path, "r");
    if (!f) return port;                       /* no conf file → default */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        /* Over-long line (no newline read and not at EOF): discard the rest of
         * the physical line so its tail is not re-parsed as a separate directive
         * (mirrors opcd_freq_source_parse, Gemini review PR #61). */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { /* skip to line end */ }
            continue;   /* a >159B line is malformed for this key — ignore it */
        }
        char key[48], val[64];
        /* Same scan as opcd_freq_source_parse: accepts "key=value" and
         * "key = value"; '#'-comment lines fail the key match and are skipped. */
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "roam_notify_port") != 0) continue;
        char *end = NULL;
        long n = strtol(val, &end, 10);
        /* Reject non-numeric / trailing garbage and out-of-range values; keep
         * the previous (last valid, else default) on rejection. */
        if (end == val || *end != '\0') continue;
        if (n < 1 || n > 65535) continue;
        port = (uint16_t)n;                    /* last valid occurrence wins */
    }
    fclose(f);
    return port;
}
