/*
 * NXP 88W9098 platform backend.
 *
 * Reads runtime device state from JSON files maintained by the cantops
 * logger pipeline:
 *   /var/log/cantops/json/eth0/link.json   — ethernet mac / ip / netmask
 *   /var/log/cantops/json/mlan0/link.json  — wlan0 mac / ssid / channel / ...
 *   /var/log/cantops/json/mlan1/link.json  — wlan1 (when enabled)
 *
 * Design choice: no ioctl, no netlink, no mlanutl exec. The logger pipeline
 * already collects everything opcd needs, so this backend is a thin reader
 * over those files — matching the user's stated convention and avoiding new
 * runtime dependencies.
 *
 * First PR scope: get_eth_mac + get_eth_ipv4_host only. The remaining 16
 * vtable members are placeholders (zero/empty/canned) so behaviour parity
 * with platform_stub.c is preserved until subsequent PRs add WLAN identity,
 * link readback, mutation, and event multiplexing.
 *
 * No external JSON library: a small key-then-sscanf extractor handles the
 * flat top-level keys in eth0/link.json. nested-scope handling lands when
 * mlan0/link.json (with duplicate `address` keys in info/link) arrives.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define ETH0_LINK_JSON  "/var/log/cantops/json/eth0/link.json"

/* Cap the file read to defend against accidentally pointing at a huge log. */
#define LINK_JSON_MAX_BYTES (1u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* JSON value extraction (flat key — sufficient for eth0/link.json)   */
/* ------------------------------------------------------------------ */

/* Read entire file into a heap buffer; caller frees with free(). */
static char *slurp_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > LINK_JSON_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

/* Extract a quoted string value for `"key": "VALUE"`. Only safe when the
 * key is unique within the file (true for top-level eth0/link.json keys).
 * Returns 0 on success and a NUL-terminated `out`. */
static int json_string_value(const char *json, const char *key,
                             char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return -EINVAL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof needle) return -EINVAL;
    const char *p = strstr(json, needle);
    if (!p) return -ENOENT;
    p = strchr(p + n, ':');
    if (!p) return -ENOENT;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -ENOENT;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int parse_mac_str(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -EINVAL;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return -EINVAL;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int nxp_init(void)
{
    return 0;
}

static void nxp_teardown(void)
{
    /* no-op — async-signal-safe by construction */
}

/* ------------------------------------------------------------------ */
/* Identity — Ethernet (real, via eth0/link.json)                     */
/* ------------------------------------------------------------------ */

static int nxp_get_eth_mac(uint8_t mac[6])
{
    char *json = slurp_file(ETH0_LINK_JSON);
    if (!json) { memset(mac, 0, 6); return -errno; }
    char buf[32] = {0};
    int rc = json_string_value(json, "mac_address", buf, sizeof buf);
    free(json);
    if (rc != 0) { memset(mac, 0, 6); return rc; }
    return parse_mac_str(buf, mac);
}

static int nxp_get_eth_ipv4_host(uint32_t *ip_host)
{
    char *json = slurp_file(ETH0_LINK_JSON);
    if (!json) { *ip_host = 0; return -errno; }
    char buf[32] = {0};
    int rc = json_string_value(json, "ip_address", buf, sizeof buf);
    free(json);
    if (rc != 0) { *ip_host = 0; return rc; }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) { *ip_host = 0; return -EINVAL; }
    *ip_host = ntohl(a.s_addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Identity — WLAN (placeholder — lands in next PR)                   */
/* ------------------------------------------------------------------ */

static int nxp_get_wlan_count(void)
{
    /* SINGLE for now — DUAL handling lands with mlan1/link.json reader. */
    return 1;
}

static int nxp_get_wlan_mac(int idx, uint8_t mac[6])
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    memset(mac, 0, 6);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Identity — Static (canned — pending hostcmd/EEPROM source)         */
/* ------------------------------------------------------------------ */

static int nxp_copy_string(char *buf, size_t cap, const char *src)
{
    if (cap == 0) return -EINVAL;
    size_t n = strnlen(src, cap - 1);
    memcpy(buf, src, n);
    buf[n] = '\0';
    return 0;
}

static int nxp_get_firmware_version(char *buf, size_t cap)
{ return nxp_copy_string(buf, cap, "wlan-opc-0.1.0"); }

static int nxp_get_hardware_version(char *buf, size_t cap)
{ return nxp_copy_string(buf, cap, "NXP88W9098"); }

static int nxp_get_serial_number(char *buf, size_t cap)
{ return nxp_copy_string(buf, cap, "SN-STUB-0001"); }

static int nxp_get_manufacture_date(opc_date_t *out)
{
    out->year = 2026; out->month = 2; out->day = 28;
    return 0;
}

static int nxp_get_shipment_date(opc_date_t *out)
{
    out->year = 2026; out->month = 3; out->day = 15;
    return 0;
}

static int nxp_get_caps(opcd_platform_caps_t *out)
{
    memset(out, 0, sizeof *out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Link / Mutation / Event — placeholders                             */
/* ------------------------------------------------------------------ */

static int nxp_get_link(int idx, opcd_platform_link_t *out)
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    memset(out, 0, sizeof *out);
    out->associated = false;
    return 0;
}

static int nxp_apply_radio_config(const opc_set_radio_config_req_t *cfg)
{
    (void)cfg;
    return 0;
}

static int nxp_apply_ip_change(const opc_ipcfg_entry_t *slot)
{
    (void)slot;
    return 0;
}

static int nxp_prepare_reset(void)
{
    return 0;
}

static int nxp_event_fd(void)
{
    return -1;
}

static int nxp_drain_events(opcd_platform_evt_cb cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vtable + registration                                              */
/* ------------------------------------------------------------------ */

static const opcd_platform_ops_t g_nxp_ops = {
    .init                  = nxp_init,
    .teardown              = nxp_teardown,
    .get_eth_mac           = nxp_get_eth_mac,
    .get_eth_ipv4_host     = nxp_get_eth_ipv4_host,
    .get_wlan_mac          = nxp_get_wlan_mac,
    .get_firmware_version  = nxp_get_firmware_version,
    .get_hardware_version  = nxp_get_hardware_version,
    .get_serial_number     = nxp_get_serial_number,
    .get_manufacture_date  = nxp_get_manufacture_date,
    .get_shipment_date     = nxp_get_shipment_date,
    .get_caps              = nxp_get_caps,
    .get_wlan_count        = nxp_get_wlan_count,
    .get_link              = nxp_get_link,
    .apply_radio_config    = nxp_apply_radio_config,
    .apply_ip_change       = nxp_apply_ip_change,
    .prepare_reset         = nxp_prepare_reset,
    .event_fd              = nxp_event_fd,
    .drain_events          = nxp_drain_events,
};

static const opcd_platform_ops_t *g_ops;

const opcd_platform_ops_t *opcd_platform(void)
{
    return g_ops;
}

void opcd_platform_register(void)
{
    /* Per platform.h: surface double-register as abort, not silent clobber.
     * Explicit check (not assert) survives -DNDEBUG. */
    if (g_ops != NULL) abort();
    g_ops = &g_nxp_ops;
}
