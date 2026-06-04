/*
 * Unit tests for opcd_snapshot_publish().
 *
 * Builds a representative device-info Ack, publishes it to a per-PID tmp
 * path under /tmp (NOT /dev/shm, so the test runs in environments where
 * tmpfs is restricted), then reads back the JSON and asserts on specific
 * substrings. We check field presence and key values — not exact byte-for-
 * byte JSON layout — so reviewer-driven layout tweaks do not require a
 * fixture update.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../snapshot.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static uint32_t ipv4_host(const char *s)
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return ntohl(a.s_addr);
}

/* Whitespace-tolerant field check: locate "key", then require `val` to occur
 * before the end of that same line. Decouples the assertions from write_json's
 * column alignment, so a layout/padding tweak in the emitter does not silently
 * break the tests (the property we actually care about is key->value, not the
 * number of spaces between them). */
static int has_field(const char *body, const char *key, const char *val)
{
    const char *p = strstr(body, key);
    if (!p) return 0;
    const char *eol = strchr(p, '\n');
    const char *v   = strstr(p + strlen(key), val);
    return v != NULL && (eol == NULL || v < eol);
}

int main(void)
{
    /* opcd_snapshot_init: NULL is rejected, missing dir is created, existing
     * dir is OK. */
    ASSERT(opcd_snapshot_init(NULL) == -EINVAL, "init NULL dir rejected");
    char tmp_dir[64];
    snprintf(tmp_dir, sizeof tmp_dir, "/tmp/test_snapshot_%d", (int)getpid());
    rmdir(tmp_dir); /* ignore */
    ASSERT(opcd_snapshot_init(tmp_dir) == 0, "init creates missing dir");
    struct stat st;
    ASSERT(stat(tmp_dir, &st) == 0 && S_ISDIR(st.st_mode),
           "init: directory exists after call");
    ASSERT(opcd_snapshot_init(tmp_dir) == 0, "init is idempotent");

    /* Build a populated Ack. */
    opc_get_device_info_ack_t ack;
    memset(&ack, 0, sizeof ack);
    ack.result          = 0;
    ack.error_cause     = 0;
    ack.vendor_code     = 0x00902CFB;
    ack.product_code    = 0xFE03;
    ack.product_subcode = 0x0001;
    ack.manufacture.year = 2026; ack.manufacture.month = 2; ack.manufacture.day = 28;
    ack.shipment.year    = 2026; ack.shipment.month    = 3; ack.shipment.day    = 15;
    strcpy(ack.firmware_version, "0.3.0");
    strcpy(ack.hardware_version, "HW-1.0.0");
    strcpy(ack.serial_number,    "SN-2026-0001");
    ack.ethernet_mac[0] = 0x2e; ack.ethernet_mac[1] = 0xdb;
    ack.ethernet_mac[2] = 0x15; ack.ethernet_mac[3] = 0xc3;
    ack.ethernet_mac[4] = 0xdd; ack.ethernet_mac[5] = 0x8d;
    ack.ip_address      = ipv4_host("192.168.0.101");
    ack.subnet_mask     = ipv4_host("255.255.255.0");
    ack.default_gateway = 0;
    ack.ntp_server      = ipv4_host("192.168.0.99");
    strcpy(ack.essid, "jhw_wlan");
    ack.device_status = 2;
    ack.station_type  = 1;        /* SINGLE */
    ack.priority_ch   = 0;
    ack.ieee_11r  = 1;
    ack.ieee_11ai = 1;
    ack.ieee_11k  = 1;
    ack.ieee_11v  = 0;
    ack.wlan1.freq_mhz = 5200;
    ack.wlan1.channel  = 0x0224;
    ack.wlan1.mode     = 11;
    ack.wlan1.bandwidth = 0;
    ack.wlan1.snr  = 25;
    ack.wlan1.rssi = -67;
    ack.wlan1.status = 1;
    ack.wlan1.mac[0] = 0x00; ack.wlan1.mac[1] = 0x04;
    ack.wlan1.mac[2] = 0x9f; ack.wlan1.mac[3] = 0x06;
    ack.wlan1.mac[4] = 0xe9; ack.wlan1.mac[5] = 0xf0;
    ack.wlan1.connect_ap_mac[0] = 0x04; ack.wlan1.connect_ap_mac[1] = 0xba;
    ack.wlan1.connect_ap_mac[2] = 0xd6; ack.wlan1.connect_ap_mac[3] = 0xec;
    ack.wlan1.connect_ap_mac[4] = 0x0b; ack.wlan1.connect_ap_mac[5] = 0x08;

    char out_path[128];
    snprintf(out_path, sizeof out_path, "%s/device_info.json", tmp_dir);

    ASSERT(opcd_snapshot_publish(&ack, out_path) == 0,
           "publish returns 0");

    char *body = slurp(out_path);
    ASSERT(body != NULL, "snapshot file readable");

    /* Field presence + value checks via has_field() — whitespace-tolerant, so
     * these survive emitter alignment changes.
     * 0x00902CFB = 9448699 ; 0xFE03 = 65027. JSON emits %u, so the values
     * appear as plain decimal. */
    ASSERT(has_field(body, "\"vendor_code\":", "9448699"),
           "vendor_code numeric (0x00902CFB)");
    ASSERT(has_field(body, "\"product_code\":", "65027"),
           "product_code numeric (0xFE03)");
    ASSERT(has_field(body, "\"product_subcode\":", "1"),
           "product_subcode numeric (0x0001)");
    ASSERT(has_field(body, "\"firmware_version\":", "\"0.3.0\""),
           "firmware_version literal");
    ASSERT(has_field(body, "\"hardware_version\":", "\"HW-1.0.0\""),
           "hardware_version literal");
    ASSERT(has_field(body, "\"serial_number\":", "\"SN-2026-0001\""),
           "serial_number literal");
    ASSERT(has_field(body, "\"manufacture_date\":", "\"2026-02-28\""),
           "manufacture_date formatted");
    ASSERT(has_field(body, "\"shipment_date\":", "\"2026-03-15\""),
           "shipment_date formatted");
    ASSERT(has_field(body, "\"ethernet_mac\":", "\"2e:db:15:c3:dd:8d\""),
           "ethernet_mac formatted");
    ASSERT(has_field(body, "\"ip_address\":", "\"192.168.0.101\""),
           "ip_address dotted");
    ASSERT(has_field(body, "\"subnet_mask\":", "\"255.255.255.0\""),
           "subnet_mask dotted");
    ASSERT(has_field(body, "\"default_gateway\":", "\"0.0.0.0\""),
           "default_gateway zero rendered as dotted");
    ASSERT(has_field(body, "\"ntp_server\":", "\"192.168.0.99\""),
           "ntp_server dotted");
    ASSERT(has_field(body, "\"essid\":", "\"jhw_wlan\""),
           "essid literal");
    ASSERT(has_field(body, "\"ieee_11r\":", "1"),  "11r value");
    ASSERT(has_field(body, "\"ieee_11ai\":", "1"), "11ai value");
    ASSERT(has_field(body, "\"ieee_11k\":", "1"),  "11k value");
    ASSERT(has_field(body, "\"ieee_11v\":", "0"),  "11v value");

    /* wlan blocks emitted in stable order. */
    ASSERT(strstr(body, "\"wlan1\"") != NULL, "wlan1 block");
    ASSERT(strstr(body, "\"wlan2\"") != NULL, "wlan2 block (always present)");
    ASSERT(has_field(body, "\"mac\":", "\"00:04:9f:06:e9:f0\""),
           "wlan1 mac");
    ASSERT(has_field(body, "\"connect_ap_mac\":", "\"04:ba:d6:ec:0b:08\""),
           "wlan1 ap mac");

    /* fetched_at present — value-content matters less, just that it is a
     * non-negative integer. */
    ASSERT(strstr(body, "\"fetched_at_monotonic_s\":") != NULL,
           "fetched_at_monotonic_s field");

    free(body);

    /* JSON escaping regression: an essid carrying a double-quote and a
     * backslash (both legal in an SSID) must not break the JSON. The raw %s
     * emitter produced `"essid": "ap"x\y"` which is invalid; json_str() must
     * render it as `ap\"x\\y`. */
    opc_get_device_info_ack_t ack2 = ack;
    strcpy(ack2.essid, "ap\"x\\y");
    char esc_path[160];
    snprintf(esc_path, sizeof esc_path, "%s/escaped.json", tmp_dir);
    ASSERT(opcd_snapshot_publish(&ack2, esc_path) == 0,
           "publish with quote+backslash essid");
    char *eb = slurp(esc_path);
    ASSERT(eb != NULL, "escaped snapshot readable");
    if (eb) {
        ASSERT(strstr(eb, "ap\\\"x\\\\y") != NULL,
               "essid quote and backslash escaped");
        ASSERT(strstr(eb, "\"ap\"x") == NULL,
               "raw unescaped quote absent");
        free(eb);
    }
    unlink(esc_path);

    /* Atomicity: temp file must not be left behind on a successful publish. */
    char tmp_path[160];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", out_path);
    ASSERT(access(tmp_path, F_OK) != 0,
           "no leftover .tmp file after publish");

    /* Failure path: bad directory yields -errno, no crash. */
    int rc = opcd_snapshot_publish(&ack, "/nonexistent_dir/device_info.json");
    ASSERT(rc < 0, "publish to bad dir returns negative");

    /* Bad args. */
    ASSERT(opcd_snapshot_publish(NULL, out_path) == -EINVAL, "NULL ack");
    ASSERT(opcd_snapshot_publish(&ack, NULL)     == -EINVAL, "NULL path");

    unlink(out_path);
    rmdir(tmp_dir);

    if (failures == 0) {
        fprintf(stdout, "all snapshot tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
