/*
 * timesyncd.conf NTP= line parser. See ntp_parse.h for grammar.
 *
 * Extracted from platform_nxp.c so the parser can be unit-tested without
 * a live timesyncd.conf at /etc.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ntp_parse.h"

int opc_ntp_parse_line(const char *line, uint32_t *server_host)
{
    if (!line || !server_host) return -EINVAL;

    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "NTP=", 4) != 0) return -ENOENT;
    line += 4;

    const char *p = line;
    while (*p && *p != '\n' && *p != '\r' && *p != '#') {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r' && *p != '#') p++;
        size_t len = (size_t)(p - tok);
        if (len == 0) break;
        char tokbuf[64];
        if (len >= sizeof tokbuf) continue;     /* unreasonable token */
        memcpy(tokbuf, tok, len);
        tokbuf[len] = '\0';
        struct in_addr a;
        if (inet_pton(AF_INET, tokbuf, &a) == 1) {
            *server_host = ntohl(a.s_addr);
            return 0;
        }
    }
    return -ENOENT;
}
