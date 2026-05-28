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
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

#define ETH0_LINK_JSON   "/var/log/cantops/json/eth0/link.json"
#define MLAN0_LINK_JSON  "/var/log/cantops/json/mlan0/link.json"
#define MLAN1_LINK_JSON  "/var/log/cantops/json/mlan1/link.json"
#define WIFI_SH          "/usr/local/scripts/wifi.sh"
/* platform.h requires a bounded short timeout for apply_radio_config.
 * Budget 900 ms (not 1000) leaves ~100 ms slack for inter-call overhead
 * in DUAL mode (per-call timeout = TIMEOUT_MS/2 = 450 ms × 2 = 900 ms). */
#define WIFI_SH_TIMEOUT_MS 900
#define WIFI_SH_POLL_MS    10

/* Cap the file read to defend against accidentally pointing at a huge log. */
#define LINK_JSON_MAX_BYTES (1u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* JSON value extraction (flat key — sufficient for eth0/link.json)   */
/* ------------------------------------------------------------------ */

/* Read entire file into a heap buffer; caller frees with free().
 * On failure, errno is set to a meaningful value (preserved across fclose,
 * which otherwise may clobber it; synthesized for non-syscall failures
 * like the LINK_JSON_MAX_BYTES cap). */
static char *slurp_file(const char *path)
{
    int saved_errno;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;       /* errno already set by fopen */
    if (fseek(f, 0, SEEK_END) != 0) {
        saved_errno = errno;
        fclose(f);
        errno = saved_errno;
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        saved_errno = errno;
        fclose(f);
        errno = saved_errno;
        return NULL;
    }
    if ((unsigned long)sz > LINK_JSON_MAX_BYTES) {
        fclose(f);
        errno = EFBIG;          /* synthetic — cap violation has no syscall */
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        errno = ENOMEM;
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    saved_errno = errno;        /* fread may have set EIO etc. */
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        errno = saved_errno ? saved_errno : EIO;
        return NULL;
    }
    buf[sz] = '\0';
    return buf;
}

/* Extract a quoted string value for `"key": "VALUE"`. Safe across keys that
 * are prefixes of each other ("mac" vs "mac_address") because the match
 * requires the character after the closing quote to be a colon or JSON
 * whitespace. Returns 0 on success with NUL-terminated `out`. */
static int json_string_value(const char *json, const char *key,
                             char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return -EINVAL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof needle) return -EINVAL;

    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        char after = p[n];
        if (after == ':' || after == ' ' || after == '\t' ||
            after == '\n' || after == '\r') {
            break;
        }
        p++;    /* prefix match like "mac" inside "mac_address" — keep searching */
    }
    if (!p) return -ENOENT;

    p = strchr(p + n, ':');
    if (!p) return -ENOENT;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -ENOENT;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/* Extract `"key": "VALUE"` confined to a `"section": { ... }` object body.
 * Brace-depth tracking handles nested objects; this assumes cantops link.json
 * never embeds `{`/`}` inside string values (verified against the eth0 /
 * mlan0 / mlan1 schemas). Same prefix-match guard as json_string_value. */
static int json_string_in_section(const char *json, const char *section,
                                  const char *key, char *out, size_t cap)
{
    if (!json || !section || !key || !out || cap == 0) return -EINVAL;

    char needle_sec[64];
    int ns = snprintf(needle_sec, sizeof needle_sec, "\"%s\"", section);
    if (ns < 0 || (size_t)ns >= sizeof needle_sec) return -EINVAL;

    const char *sec = json;
    while ((sec = strstr(sec, needle_sec)) != NULL) {
        char a = sec[ns];
        if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') break;
        sec++;
    }
    if (!sec) return -ENOENT;

    sec = strchr(sec, '{');
    if (!sec) return -ENOENT;

    char needle_key[64];
    int nk = snprintf(needle_key, sizeof needle_key, "\"%s\"", key);
    if (nk < 0 || (size_t)nk >= sizeof needle_key) return -EINVAL;

    int depth = 1;
    const char *p = sec + 1;
    while (*p && depth > 0) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) break;
        }
        if (depth > 0 && strncmp(p, needle_key, (size_t)nk) == 0) {
            char a = p[nk];
            if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
                const char *colon = strchr(p + nk, ':');
                if (!colon) return -ENOENT;
                colon++;
                while (*colon == ' ' || *colon == '\t' ||
                       *colon == '\n' || *colon == '\r') colon++;
                if (*colon != '"') return -ENOENT;
                colon++;
                size_t i = 0;
                while (*colon && *colon != '"' && i + 1 < cap) {
                    out[i++] = *colon++;
                }
                out[i] = '\0';
                return 0;
            }
        }
        p++;
    }
    return -ENOENT;
}

/* Same as json_string_in_section but extracts a JSON integer
 * (`"key": -42` form, no quotes). */
static int json_integer_in_section(const char *json, const char *section,
                                   const char *key, long *out)
{
    if (!json || !section || !key || !out) return -EINVAL;

    char needle_sec[64];
    int ns = snprintf(needle_sec, sizeof needle_sec, "\"%s\"", section);
    if (ns < 0 || (size_t)ns >= sizeof needle_sec) return -EINVAL;

    const char *sec = json;
    while ((sec = strstr(sec, needle_sec)) != NULL) {
        char a = sec[ns];
        if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') break;
        sec++;
    }
    if (!sec) return -ENOENT;
    sec = strchr(sec, '{');
    if (!sec) return -ENOENT;

    char needle_key[64];
    int nk = snprintf(needle_key, sizeof needle_key, "\"%s\"", key);
    if (nk < 0 || (size_t)nk >= sizeof needle_key) return -EINVAL;

    int depth = 1;
    const char *p = sec + 1;
    while (*p && depth > 0) {
        if (*p == '{')       depth++;
        else if (*p == '}') { depth--; if (depth == 0) break; }
        if (depth > 0 && strncmp(p, needle_key, (size_t)nk) == 0) {
            char a = p[nk];
            if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
                const char *colon = strchr(p + nk, ':');
                if (!colon) return -ENOENT;
                colon++;
                while (*colon == ' ' || *colon == '\t' ||
                       *colon == '\n' || *colon == '\r') colon++;
                /* digit or sign — numeric literal */
                char *endp = NULL;
                long v = strtol(colon, &endp, 10);
                if (endp == colon) return -EINVAL;
                *out = v;
                return 0;
            }
        }
        p++;
    }
    return -ENOENT;
}

/* Parse "-66 dBm" / "-66dBm" into an int8 (signed dBm). */
static int parse_signed_dbm(const char *s, int8_t *out)
{
    int v;
    if (sscanf(s, "%d", &v) != 1) return -EINVAL;
    if (v < INT8_MIN) v = INT8_MIN;
    if (v > INT8_MAX) v = INT8_MAX;
    *out = (int8_t)v;
    return 0;
}

/* Parse "20 MHz" / "40 MHz" / "80 MHz" / "160 MHz" to OPC_BANDWIDTH_*. */
static int parse_width_to_bw(const char *s, uint8_t *out)
{
    int mhz;
    if (sscanf(s, "%d", &mhz) != 1) return -EINVAL;
    switch (mhz) {
    case 20:  *out = OPC_BANDWIDTH_20;  return 0;
    case 40:  *out = OPC_BANDWIDTH_40;  return 0;
    case 80:  *out = OPC_BANDWIDTH_80;  return 0;
    case 160: *out = OPC_BANDWIDTH_160; return 0;
    case 320: *out = OPC_BANDWIDTH_320; return 0;
    default:  return -EINVAL;
    }
}

/* Parse iw-style bitrate string prefix to OPC_WLAN_MODE_*.
 *   "258.0 MBit/s HE-MCS 10 HE-NSS 2 ..."   → 11AX  (HE- prefix)
 *   "433.3 MBit/s VHT-MCS 9 ..."            → 11AC  (VHT- prefix)
 *   "144.4 MBit/s MCS 15 short GI ..."      → 11N   (iw emits no HT- prefix)
 *   plain "54 MBit/s"                       → legacy (caller leaves mode=0)
 *   "... EHT-MCS ..." (802.11be)            → -EINVAL: no OPC enum yet;
 *                                              caller falls back to cache.
 *
 * Order is defensive: in current iw output "HE-MCS" / "VHT-MCS" have no
 * space between "-" and "M", so the bare " MCS " check would not misfire
 * today. Checking the explicit HE-/VHT- prefixes first future-proofs
 * against driver variants that might emit a standalone " MCS " token
 * alongside HE/VHT fields. */
static int parse_bitrate_to_mode(const char *s, uint8_t *out)
{
    if (strstr(s, " HE-")  != NULL) { *out = OPC_WLAN_MODE_11AX; return 0; }
    if (strstr(s, " VHT-") != NULL) { *out = OPC_WLAN_MODE_11AC; return 0; }
    if (strstr(s, " MCS ") != NULL) { *out = OPC_WLAN_MODE_11N;  return 0; }
    return -EINVAL;
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

/* Read one IPv4-typed field (ip_address/netmask/gateway) from eth0/link.json
 * as a host-order uint32_t. gateway is JSON null when unconfigured; the
 * string-matcher then returns -ENOENT and *out stays 0 — caller maps that
 * to "no gateway" per best-effort policy. */
static int nxp_get_eth_ipv4_field(const char *key, uint32_t *out_host)
{
    char *json = slurp_file(ETH0_LINK_JSON);
    if (!json) { *out_host = 0; return -errno; }
    char buf[32] = {0};
    int rc = json_string_value(json, key, buf, sizeof buf);
    free(json);
    if (rc != 0) { *out_host = 0; return rc; }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) { *out_host = 0; return -EINVAL; }
    *out_host = ntohl(a.s_addr);
    return 0;
}

static int nxp_get_eth_ipv4_host(uint32_t *ip_host)
{
    return nxp_get_eth_ipv4_field("ip_address", ip_host);
}

static int nxp_get_eth_netmask_host(uint32_t *netmask_host)
{
    return nxp_get_eth_ipv4_field("netmask", netmask_host);
}

static int nxp_get_eth_gateway_host(uint32_t *gateway_host)
{
    return nxp_get_eth_ipv4_field("gateway", gateway_host);
}

/* ------------------------------------------------------------------ */
/* Identity — WLAN (mlan0 / mlan1 via link.json)                      */
/* ------------------------------------------------------------------ */

static int nxp_get_wlan_count(void)
{
    /* mlan0 is always present in a wlan-opc target. mlan1 is created by the
     * cantops wifi_init pipeline only when the interface is enabled in
     * /usr/local/etc/wifi_init_conf.json — its link.json existence is the
     * canonical signal.
     *
     * platform.h contract: only ENOENT (mlan1 disabled) is a known-good
     * "single" result. Other access() errors (EACCES, EIO, ENOMEM, ...)
     * mean the platform genuinely cannot query — surface them so the boot
     * path treats them as fatal-at-startup. */
    if (access(MLAN1_LINK_JSON, F_OK) == 0) return 2;
    if (errno == ENOENT)                    return 1;
    return -errno;
}

static int nxp_get_wlan_mac(int idx, uint8_t mac[6])
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = slurp_file(path);
    if (!json) { memset(mac, 0, 6); return -errno; }
    char buf[32] = {0};
    /* mlan link.json has `address` in two places (info, link); info holds
     * the interface MAC (the device's own), link holds the AP's MAC under
     * the same key. json_string_in_section disambiguates. */
    int rc = json_string_in_section(json, "info", "address", buf, sizeof buf);
    free(json);
    if (rc != 0) { memset(mac, 0, 6); return rc; }
    return parse_mac_str(buf, mac);
}

static int nxp_get_essid(int idx, char *buf, size_t cap)
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    if (cap == 0) return -EINVAL;
    buf[0] = '\0';
    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = slurp_file(path);
    if (!json) return -errno;
    int rc = json_string_in_section(json, "info", "ssid", buf, cap);
    free(json);
    return rc;
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

    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = slurp_file(path);
    if (!json) return -errno;

    char buf[64] = {0};
    long ival = 0;

    /* info.freq / info.channel — operating frequency/channel */
    if (json_integer_in_section(json, "info", "freq", &ival) == 0 &&
        ival > 0 && ival <= UINT16_MAX) {
        out->freq_mhz = (uint16_t)ival;
    }
    if (json_integer_in_section(json, "info", "channel", &ival) == 0 &&
        ival >= 0 && ival <= UINT16_MAX) {
        out->channel = (uint16_t)ival;
    }

    /* link.address — AP MAC. Presence also signals associated. */
    if (json_string_in_section(json, "link", "address", buf, sizeof buf) == 0 &&
        parse_mac_str(buf, out->bssid) == 0) {
        out->associated = true;
    }

    /* info.width → OPC_BANDWIDTH_*. Parsed only when associated so a stale
     * driver-cached width cannot leak bandwidth_valid=true while we report
     * associated=false. Only set on successful parse — OPC_BANDWIDTH_20==0
     * collides with the zero-init default. */
    if (out->associated &&
        json_string_in_section(json, "info", "width", buf, sizeof buf) == 0) {
        uint8_t bw;
        if (parse_width_to_bw(buf, &bw) == 0) {
            out->bandwidth = bw;
            out->bandwidth_valid = true;
        }
    }

    /* link.tx_bitrate prefix → OPC_WLAN_MODE_*. Parsed only when associated
     * so a stale driver bitrate from a prior session cannot leak a non-zero
     * mode while we report associated=false. Falls back to rx_bitrate. */
    if (out->associated &&
        json_string_in_section(json, "link", "tx_bitrate", buf, sizeof buf) == 0) {
        uint8_t mode;
        if (parse_bitrate_to_mode(buf, &mode) == 0) out->mode = mode;
    }
    if (out->associated && out->mode == 0 &&
        json_string_in_section(json, "link", "rx_bitrate", buf, sizeof buf) == 0) {
        uint8_t mode;
        if (parse_bitrate_to_mode(buf, &mode) == 0) out->mode = mode;
    }

    /* link.signal_avg "-66 dBm" → rssi */
    if (json_string_in_section(json, "link", "signal_avg", buf, sizeof buf) == 0) {
        (void)parse_signed_dbm(buf, &out->rssi);
    }

    /* channel_info.<freq>.noise → snr = rssi - noise.
     * channel_info is keyed by the operating freq (dynamic), but `noise`
     * is unique within channel_info for the active channel. The nested
     * search walks into the freq sub-object via brace depth. */
    if (out->associated &&
        json_integer_in_section(json, "channel_info", "noise", &ival) == 0 &&
        ival >= INT8_MIN && ival < 0) {   /* 0 = uninitialized driver field */
        int snr = (int)out->rssi - (int)ival;
        if (snr < INT8_MIN) snr = INT8_MIN;
        if (snr > INT8_MAX) snr = INT8_MAX;
        out->snr = (int8_t)snr;
    }

    free(json);
    return 0;
}

/* Run "wifi.sh <iface> freq <mhz>" synchronously. wifi.sh only rewrites
 * scan_freq= / freq_list= in wpa_supplicant-<iface>.conf — fast (~ms) and
 * comfortably within the OPC 1-second response budget. wpa_supplicant
 * restart is intentionally NOT triggered here so the active link is not
 * dropped on every Set-Radio; the operator triggers reconnect explicitly. */
static int run_wifi_sh_freq(const char *iface, uint16_t freq_mhz, long timeout_ms)
{
    char freq_buf[8];
    snprintf(freq_buf, sizeof freq_buf, "%u", freq_mhz);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "opcd: nxp_apply_radio_config: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(WIFI_SH, "wifi.sh", iface, "freq", freq_buf, (char *)NULL);
        /* execl returned: fprintf is not async-signal-safe post-fork.
         * Diagnose via write() so the parent's "status=0x7f00" is
         * distinguishable from wifi.sh itself returning 127. */
        static const char msg[] =
            "opcd: nxp_apply_radio_config: execl " WIFI_SH " failed\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
        _exit(127);
    }

    /* Bounded wait: poll with WNOHANG + monotonic deadline. wifi.sh freq is
     * awk + install_0644_sync (normally ms), but backing-store stalls (SD
     * card / NFS fsync) could block indefinitely. platform.h's "bounded
     * short timeout" contract requires we cap and SIGKILL on overrun. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            /* opcd installs no SIGCHLD handler / SIG_IGN, so this is
             * unreachable for our own forked pid under the current model.
             * Still SIGKILL+reap defensively so a future signal-handling
             * change cannot cause this branch to leak the child. */
            fprintf(stderr, "opcd: nxp_apply_radio_config: waitpid failed: %s\n",
                    strerror(errno));
            kill(pid, SIGKILL);
            pid_t wr;
            while ((wr = waitpid(pid, &status, 0)) < 0 && errno == EINTR) { }
            if (wr < 0) {
                fprintf(stderr,
                        "opcd: nxp_apply_radio_config: defensive reap failed: %s\n",
                        strerror(errno));
            }
            return -1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - start.tv_sec)  * 1000L
                        + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr,
                    "opcd: nxp_apply_radio_config: wifi.sh %s freq %u timed out after %ldms, sending SIGKILL\n",
                    iface, freq_mhz, elapsed_ms);
            kill(pid, SIGKILL);
            /* Reap so the SIGKILL'd child is not left as a zombie. */
            pid_t wr;
            while ((wr = waitpid(pid, &status, 0)) < 0 && errno == EINTR) { }
            if (wr < 0) {
                fprintf(stderr,
                        "opcd: nxp_apply_radio_config: waitpid reap after SIGKILL failed: %s\n",
                        strerror(errno));
            }
            return -1;
        }
        /* Cap the sleep so the last poll never overruns the deadline.
         * Without this, child can live up to timeout_ms + POLL_MS. */
        long remain_ms = timeout_ms - elapsed_ms;
        long sleep_ms  = (remain_ms < WIFI_SH_POLL_MS) ? remain_ms : WIFI_SH_POLL_MS;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = sleep_ms * 1000L * 1000L };
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: wifi.sh %s freq %u failed (status=0x%x)\n",
                iface, freq_mhz, (unsigned)status);
        return -1;
    }
    return 0;
}

static int nxp_apply_radio_config(const opc_set_radio_config_req_t *cfg)
{
    if (cfg->station_type == OPC_STATION_DUAL) {
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: station=DUAL priority_ch=%u "
                "w1(freq=%u ch=0x%04x mode=%u bw=%u) "
                "w2(freq=%u ch=0x%04x mode=%u bw=%u)\n",
                cfg->priority_ch,
                cfg->wlan1.freq_mhz, cfg->wlan1.channel,
                cfg->wlan1.mode, cfg->wlan1.bandwidth,
                cfg->wlan2.freq_mhz, cfg->wlan2.channel,
                cfg->wlan2.mode, cfg->wlan2.bandwidth);
    } else {
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: station=SINGLE priority_ch=%u "
                "w1(freq=%u ch=0x%04x mode=%u bw=%u)\n",
                cfg->priority_ch,
                cfg->wlan1.freq_mhz, cfg->wlan1.channel,
                cfg->wlan1.mode, cfg->wlan1.bandwidth);
    }

    /* Share the 1s regulation budget across both apply calls in DUAL so the
     * worst-case wall-clock stays within platform.h's contract. */
    const long per_call_ms = (cfg->station_type == OPC_STATION_DUAL)
                             ? WIFI_SH_TIMEOUT_MS / 2
                             : WIFI_SH_TIMEOUT_MS;

    /* freq_mhz == 0 means "no association" per OPC spec — skip apply, leave
     * wpa_supplicant.conf untouched. Mode / bandwidth mapping is deferred to
     * a follow-up PR; this PR only wires the wpa_supplicant freq list. */
    if (cfg->wlan1.freq_mhz != 0) {
        if (run_wifi_sh_freq("mlan0", cfg->wlan1.freq_mhz, per_call_ms) != 0) return -1;
    }
    if (cfg->station_type == OPC_STATION_DUAL && cfg->wlan2.freq_mhz != 0) {
        /* Make the partial-apply state visible in the journal so a post-NG
         * triage shows whether mlan0 was already reconfigured.
         * Ternary safe: an mlan0 failure would have early-returned above,
         * so wlan1.freq_mhz==0 here means an intentional skip, not error. */
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: %s; now applying mlan1 freq=%u\n",
                cfg->wlan1.freq_mhz != 0 ? "mlan0 freq already applied"
                                         : "mlan0 skipped (freq=0)",
                cfg->wlan2.freq_mhz);
        /* DUAL partial-apply risk: if mlan0 succeeded above, an mlan1
         * failure here leaves the two wpa_supplicant confs out of sync.
         * Caller gets NG (0x0050); recovery is the operator re-issuing
         * Set-Radio. End-to-end idempotency is the reconnect PR's job. */
        if (run_wifi_sh_freq("mlan1", cfg->wlan2.freq_mhz, per_call_ms) != 0) return -1;
    }
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
    .get_eth_netmask_host  = nxp_get_eth_netmask_host,
    .get_eth_gateway_host  = nxp_get_eth_gateway_host,
    .get_wlan_mac          = nxp_get_wlan_mac,
    .get_essid             = nxp_get_essid,
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
