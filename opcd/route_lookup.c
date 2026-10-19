#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "route_lookup.h"

#define RT_UP       0x0001u
#define RT_GATEWAY  0x0002u
#define RT_REJECT   0x0200u

/* Bits set in a contiguous netmask (the table's masks are prefixes). */
static int mask_bits(uint32_t m)
{
    int n = 0;
    for (; m; m &= m - 1u) n++;
    return n;
}

int opcd_route_direct_lookup(const char *text, uint32_t dst_host,
                             uint32_t *mask_host)
{
    if (!text) return -ENETUNREACH;

    int      best_bits   = -1;
    unsigned best_metric = 0;
    int      best_direct = 0;
    uint32_t best_mask   = 0;

    for (const char *line = text; *line; ) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[256];
        if (len < sizeof buf) {
            memcpy(buf, line, len);
            buf[len] = '\0';
            char iface[32];
            unsigned long dest_raw, gw_raw, mask_raw, flags;
            int refcnt, use, metric;
            /* Iface Destination Gateway Flags RefCnt Use Metric Mask ...
             * (the header line fails the hex conversions and is skipped) */
            if (sscanf(buf, "%31s %lx %lx %lx %d %d %d %lx", iface, &dest_raw,
                       &gw_raw, &flags, &refcnt, &use, &metric, &mask_raw) == 8 &&
                (flags & RT_UP)) {
                /* the file prints the raw network-order word with %08X */
                uint32_t dest = ntohl((uint32_t)dest_raw);
                uint32_t mask = ntohl((uint32_t)mask_raw);
                if ((dst_host & mask) == dest) {
                    int bits = mask_bits(mask);
                    if (bits > best_bits ||
                        (bits == best_bits && (unsigned)metric < best_metric)) {
                        best_bits   = bits;
                        best_metric = (unsigned)metric;
                        best_mask   = mask;
                        /* throw/unreachable/blackhole routes have no output
                         * device (Iface "*"); throw and blackhole carry only
                         * RTF_UP (unreachable adds RTF_REJECT), so the flag
                         * test alone would call them direct */
                        best_direct = gw_raw == 0 &&
                                      !(flags & (RT_GATEWAY | RT_REJECT)) &&
                                      strcmp(iface, "*") != 0;
                    }
                }
            }
        }
        if (!eol) break;
        line = eol + 1;
    }

    if (best_bits < 0 || !best_direct) return -ENETUNREACH;
    if (mask_host) *mask_host = best_mask;
    return 0;
}
