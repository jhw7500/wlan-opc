/* Host unit test for the pure opc.conf device_ip_iface parser
 * (opcd/ip_iface.{c,h}). Mirrors test_freq_source.c: unknown token → ETH0,
 * duplicate key → last wins, missing file → ETH0, over-long-line guard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../ip_iface.h"

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
    /* ---- iface index mapping (platform-facing encoding) ---- */
    ASSERT(opcd_ip_iface_idx(OPC_IP_IFACE_ETH0)  == 0, "idx eth0 -> 0");
    ASSERT(opcd_ip_iface_idx(OPC_IP_IFACE_MLAN0) == 1, "idx mlan0 -> 1");

    /* ---- token mapper ---- */
    ASSERT(opcd_ip_iface_from_token("eth0")  == OPC_IP_IFACE_ETH0,  "token eth0");
    ASSERT(opcd_ip_iface_from_token("mlan0") == OPC_IP_IFACE_MLAN0, "token mlan0");
    ASSERT(opcd_ip_iface_from_token("bogus") == OPC_IP_IFACE_ETH0,  "token unknown -> eth0");
    ASSERT(opcd_ip_iface_from_token("")      == OPC_IP_IFACE_ETH0,  "token empty -> eth0");
    ASSERT(opcd_ip_iface_from_token(NULL)    == OPC_IP_IFACE_ETH0,  "token NULL -> eth0");
    ASSERT(opcd_ip_iface_from_token("MLAN0") == OPC_IP_IFACE_ETH0,  "token case-sensitive -> eth0");
    ASSERT(opcd_ip_iface_from_token("mlan1") == OPC_IP_IFACE_ETH0,  "token mlan1 (out of scope) -> eth0");

    /* ---- file parser ---- */
    snprintf(g_path, sizeof g_path, "test_ip_iface_%d.tmp", (int)getpid());
    unlink(g_path);

    ASSERT(opcd_ip_iface_parse("/nonexistent_dir/opc.conf") == OPC_IP_IFACE_ETH0,
           "missing file -> eth0");
    ASSERT(opcd_ip_iface_parse(NULL) == OPC_IP_IFACE_ETH0, "NULL path -> eth0");

    write_conf("device_ip_iface = mlan0\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_MLAN0, "file mlan0");

    write_conf("device_ip_iface = eth0\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0, "file eth0");

    write_conf("device_ip_iface=mlan0\n");   /* no spaces around '=' */
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_MLAN0, "no-space '=' -> mlan0");

    /* unrelated key + commented line ignored -> default eth0 */
    write_conf("device_info_freq_source = live\n# device_ip_iface = mlan0\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0, "other-key/comment ignored -> eth0");

    /* duplicate key -> last wins */
    write_conf("device_ip_iface = mlan0\ndevice_ip_iface = eth0\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0, "duplicate key -> last wins");

    /* unknown value -> eth0 fallback */
    write_conf("device_ip_iface = wlan0\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0, "unknown value -> eth0");

    /* '#' inline comment with NO space: %63s takes "mlan0#x" as the token →
     * unknown → eth0 (documented edge; a space before '#' avoids it). */
    write_conf("device_ip_iface = mlan0#nospace\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0, "value#comment no-space -> eth0");
    write_conf("device_ip_iface = mlan0 # spaced comment\n");
    ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_MLAN0, "value + spaced #comment -> mlan0");

    /* over-long line (>159B) is discarded, not split: a directive sitting in
     * its tail must NOT take effect (same guard as freq_source, PR #61). */
    {
        char buf[256];
        memset(buf, '#', 159);
        strcpy(buf + 159, "device_ip_iface = mlan0\n");
        write_conf(buf);
        ASSERT(opcd_ip_iface_parse(g_path) == OPC_IP_IFACE_ETH0,
               "over-long line tail not parsed -> eth0");
    }

    unlink(g_path);

    if (failures == 0) { printf("all ip_iface tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
