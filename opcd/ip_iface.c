#include <stdio.h>
#include <string.h>

#include "ip_iface.h"

opcd_ip_iface_t opcd_ip_iface_from_token(const char *val)
{
    if (!val) return OPC_IP_IFACE_ETH0;
    if (strcmp(val, "mlan0") == 0) return OPC_IP_IFACE_MLAN0;
    return OPC_IP_IFACE_ETH0;   /* "eth0" and anything unrecognized */
}

opcd_ip_iface_t opcd_ip_iface_parse(const char *conf_path)
{
    opcd_ip_iface_t src = OPC_IP_IFACE_ETH0;
    if (!conf_path) return src;
    FILE *f = fopen(conf_path, "r");
    if (!f) return src;                        /* no conf file → default */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        /* Over-long line (no newline read and not at EOF): discard the rest of
         * the physical line so its tail is not re-parsed as a separate directive
         * (same guard as freq_source.c — Gemini review, PR #61). */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { /* skip to line end */ }
            continue;   /* a >159B line is malformed for this key — ignore it */
        }
        char key[48], val[64];
        /* Same scan as opcd_freq_source_parse: accepts "key=value" and
         * "key = value"; '#'-comment lines fail the key match and are skipped.
         * NOTE: %63s stops at whitespace, so a trailing "value#comment" with no
         * space before '#' is taken literally and falls back to ETH0 — put a
         * space before inline comments. */
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "device_ip_iface") != 0) continue;
        src = opcd_ip_iface_from_token(val);   /* last occurrence wins */
    }
    fclose(f);
    return src;
}
