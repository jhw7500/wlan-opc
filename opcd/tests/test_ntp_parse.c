/*
 * Unit tests for opc_ntp_parse_line().
 *
 * Each case feeds a single line — exactly what platform_nxp.c sees after
 * strtok_r() splits timesyncd.conf — and checks the returned IPv4 (or its
 * absence). Lines are deliberately not NUL-terminated mid-token; the
 * parser must walk to the natural end markers (CR/LF, '#', whitespace).
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ntp_parse.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static uint32_t ipv4_host(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return 0;
    return ntohl(a.s_addr);
}

int main(void)
{
    uint32_t srv;

    /* Single literal IPv4. */
    srv = 0xdeadbeefu;
    ASSERT(opc_ntp_parse_line("NTP=192.168.0.99", &srv) == 0,
           "single literal returns 0");
    ASSERT(srv == ipv4_host("192.168.0.99"),
           "single literal value");

    /* Leading whitespace tolerated. */
    srv = 0xdeadbeefu;
    ASSERT(opc_ntp_parse_line("    NTP=10.0.0.1", &srv) == 0,
           "leading whitespace");
    ASSERT(srv == ipv4_host("10.0.0.1"),
           "leading whitespace value");

    /* Multiple servers — first literal wins. */
    srv = 0;
    ASSERT(opc_ntp_parse_line("NTP=time.example.com 172.16.0.5 1.2.3.4",
                              &srv) == 0,
           "hostname before literal");
    ASSERT(srv == ipv4_host("172.16.0.5"),
           "hostname before literal value");

    /* Trailing comment. */
    srv = 0;
    ASSERT(opc_ntp_parse_line("NTP=8.8.8.8  # cloudflare? no, google",
                              &srv) == 0,
           "trailing comment");
    ASSERT(srv == ipv4_host("8.8.8.8"),
           "trailing comment value");

    /* CR at end of line. */
    srv = 0;
    ASSERT(opc_ntp_parse_line("NTP=192.168.1.1\r", &srv) == 0,
           "trailing CR");
    ASSERT(srv == ipv4_host("192.168.1.1"),
           "trailing CR value");

    /* Hostname only — no literal — must return -ENOENT, srv unchanged. */
    srv = 0xcafebabeu;
    ASSERT(opc_ntp_parse_line("NTP=pool.ntp.org time.example.com",
                              &srv) != 0,
           "hostname-only returns non-zero");
    ASSERT(srv == 0xcafebabeu,
           "hostname-only leaves srv unchanged");

    /* Empty value list. */
    srv = 0xcafebabeu;
    ASSERT(opc_ntp_parse_line("NTP=", &srv) != 0,
           "empty value returns non-zero");

    /* Non-NTP line (FallbackNTP, Time section header, comment, blank). */
    srv = 0xcafebabeu;
    ASSERT(opc_ntp_parse_line("FallbackNTP=ntp.ubuntu.com", &srv) != 0,
           "FallbackNTP rejected");
    ASSERT(opc_ntp_parse_line("[Time]", &srv) != 0,
           "section header rejected");
    ASSERT(opc_ntp_parse_line("# NTP=1.2.3.4", &srv) != 0,
           "comment line rejected");
    ASSERT(opc_ntp_parse_line("", &srv) != 0,
           "empty line rejected");

    /* Malformed input. */
    ASSERT(opc_ntp_parse_line(NULL, &srv) != 0,
           "NULL line rejected");
    ASSERT(opc_ntp_parse_line("NTP=1.2.3.4", NULL) != 0,
           "NULL out rejected");

    /* Overlong token (skipped, not crash). */
    char overlong[200];
    memset(overlong, 'a', sizeof overlong - 1);
    overlong[sizeof overlong - 1] = '\0';
    char buf[260];
    snprintf(buf, sizeof buf, "NTP=%s 9.9.9.9", overlong);
    srv = 0;
    ASSERT(opc_ntp_parse_line(buf, &srv) == 0,
           "overlong token skipped, literal still parsed");
    ASSERT(srv == ipv4_host("9.9.9.9"),
           "overlong skip value");

    if (failures == 0) {
        fprintf(stdout, "all ntp_parse tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
