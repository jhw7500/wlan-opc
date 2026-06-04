/*
 * device-info Ack → /dev/shm/opcd/device_info.json publisher.
 *
 * Failure policy: best-effort. opcd's wire response is the source of truth;
 * the snapshot is a side channel for external monitoring. We log a single
 * warning per process on first failure to avoid flooding stderr if /dev/shm
 * is misconfigured.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "snapshot.h"

/* One-shot guard for the stderr warning. opcd's dispatch path is single-
 * threaded, so a plain bool needs no synchronisation; if snapshot publishing
 * is ever moved off the main thread this must become atomic. */
static bool g_warn_emitted;

static void warn_once(const char *path, const char *what, int err)
{
    if (g_warn_emitted) return;
    g_warn_emitted = true;
    fprintf(stderr, "opcd: snapshot %s failed for %s: %s "
                    "(further snapshot errors silenced)\n",
            what, path, strerror(err));
}

int opcd_snapshot_init(const char *dir)
{
    if (!dir) return -EINVAL;
    if (mkdir(dir, 0755) == 0)          return 0;
    if (errno == EEXIST)                return 0;
    return -errno;
}

/* Format an IPv4 (host order) as dotted-quad. Buffer must hold INET_ADDRSTRLEN
 * (16). Returns the buffer pointer for chaining. */
static const char *ipv4_str(uint32_t host, char buf[/*16*/])
{
    struct in_addr a;
    a.s_addr = htonl(host);
    if (!inet_ntop(AF_INET, &a, buf, 16)) {
        snprintf(buf, 16, "0.0.0.0");
    }
    return buf;
}

static const char *mac_str(const uint8_t mac[6], char buf[/*18*/])
{
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

/* Escape `in` for embedding in a JSON string, writing a NUL-terminated result
 * to `out`. Field values like essid are peer- or operator-controlled and may
 * contain quotes, backslashes, or control bytes (all legal in an 802.11 SSID);
 * emitting them raw via %s would produce invalid JSON. Per RFC 8259 §7 we
 * escape `"`, `\`, and every control character U+0000–U+001F (as `\u00XX`).
 * Output is truncated to fit `cap` (worst case each input byte expands to six).
 * Returns `out` for use as a printf arg. */
static const char *json_str(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= cap) break;
            out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
            out[o++] = hex[(c >> 4) & 0x0F];
            out[o++] = hex[c & 0x0F];
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

/* Emit one WLAN radio block. `idx` is 1 or 2. Both blocks are always written
 * (see write_json for the stable-schema rationale). Emits no trailing comma or
 * newline — write_json owns the separators so that adding a field after wlan2
 * cannot silently produce trailing-comma JSON. Returns 0, or -EIO on write
 * failure. */
static int emit_wlan(FILE *f, int idx, const opc_wlan_radio_state_t *w)
{
    char macbuf[18], apbuf[18];
    if (fprintf(f,
        "  \"wlan%d\": {\n"
        "    \"mac\":            \"%s\",\n"
        "    \"mode\":           %u,\n"
        "    \"bandwidth\":      %u,\n"
        "    \"freq_mhz\":       %u,\n"
        "    \"channel\":        %u,\n"
        "    \"status\":         %u,\n"
        "    \"snr\":            %d,\n"
        "    \"rssi\":           %d,\n"
        "    \"connect_ap_mac\": \"%s\"\n"
        "  }",
        idx,
        mac_str(w->mac, macbuf),
        (unsigned)w->mode, (unsigned)w->bandwidth,
        (unsigned)w->freq_mhz, (unsigned)w->channel,
        (unsigned)w->status,
        (int)w->snr, (int)w->rssi,
        mac_str(w->connect_ap_mac, apbuf)) < 0) {
        return -EIO;
    }
    return 0;
}

/* Serialise the full ack into FILE* `f`. Layout mirrors opc_get_device_info_ack_t
 * 1:1 so future field additions are obvious to a reviewer. */
static int write_json(FILE *f, const opc_get_device_info_ack_t *ack)
{
    char ipbuf[16], maskbuf[16], gwbuf[16], ntpbuf[16];
    char ethbuf[18];
    /* Escaped copies of the operator-/peer-controlled string fields. json_str's
     * worst case is 6 output bytes per input byte (a control char -> "\u00XX"),
     * so 6x the array size covers it; json_str NUL-terminates within `cap`, so
     * no explicit +1 is needed. *_FIELD_LEN here is the source array size. */
    char fwbuf[6 * OPC_VERSION_FIELD_LEN];
    char hwbuf[6 * OPC_VERSION_FIELD_LEN];
    char snbuf[6 * OPC_SERIAL_FIELD_LEN];
    char essidbuf[6 * OPC_ESSID_FIELD_LEN];

    /* fetched_at: monotonic seconds since boot, NOT wall clock — the device
     * may have no time source synced yet. Tools that need wall clock can
     * stat() the file. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    if (fprintf(f,
        "{\n"
        "  \"fetched_at_monotonic_s\": %ld,\n"
        "  \"result\":          %u,\n"
        "  \"error_cause\":     %u,\n"
        "  \"vendor_code\":     %u,\n"
        "  \"product_code\":    %u,\n"
        "  \"product_subcode\": %u,\n"
        "  \"manufacture_date\": \"%04u-%02u-%02u\",\n"
        "  \"shipment_date\":    \"%04u-%02u-%02u\",\n"
        "  \"firmware_version\": \"%s\",\n"
        "  \"hardware_version\": \"%s\",\n"
        "  \"serial_number\":    \"%s\",\n"
        "  \"ethernet_mac\":     \"%s\",\n"
        "  \"ip_address\":       \"%s\",\n"
        "  \"subnet_mask\":      \"%s\",\n"
        "  \"default_gateway\":  \"%s\",\n"
        "  \"ntp_server\":       \"%s\",\n"
        "  \"essid\":            \"%s\",\n"
        "  \"device_status\":    %u,\n"
        "  \"station_type\":     %u,\n"
        "  \"priority_ch\":      %u,\n"
        "  \"ieee_11r\":  %u,\n"
        "  \"ieee_11ai\": %u,\n"
        "  \"ieee_11k\":  %u,\n"
        "  \"ieee_11v\":  %u,\n",
        (long)ts.tv_sec,
        (unsigned)ack->result,
        (unsigned)ack->error_cause,
        (unsigned)ack->vendor_code,
        (unsigned)ack->product_code,
        (unsigned)ack->product_subcode,
        (unsigned)ack->manufacture.year,
        (unsigned)ack->manufacture.month,
        (unsigned)ack->manufacture.day,
        (unsigned)ack->shipment.year,
        (unsigned)ack->shipment.month,
        (unsigned)ack->shipment.day,
        json_str(ack->firmware_version, fwbuf, sizeof fwbuf),
        json_str(ack->hardware_version, hwbuf, sizeof hwbuf),
        json_str(ack->serial_number,    snbuf, sizeof snbuf),
        mac_str(ack->ethernet_mac, ethbuf),
        ipv4_str(ack->ip_address,      ipbuf),
        ipv4_str(ack->subnet_mask,     maskbuf),
        ipv4_str(ack->default_gateway, gwbuf),
        ipv4_str(ack->ntp_server,      ntpbuf),
        json_str(ack->essid, essidbuf, sizeof essidbuf),
        (unsigned)ack->device_status,
        (unsigned)ack->station_type,
        (unsigned)ack->priority_ch,
        (unsigned)ack->ieee_11r,
        (unsigned)ack->ieee_11ai,
        (unsigned)ack->ieee_11k,
        (unsigned)ack->ieee_11v) < 0) {
        return -EIO;
    }
    /* Comma between the two radio blocks is owned here, not by emit_wlan, so
     * the separator stays correct if a field is ever appended after wlan2. */
    if (emit_wlan(f, 1, &ack->wlan1) != 0) return -EIO;
    if (fprintf(f, ",\n") < 0)             return -EIO;
    if (emit_wlan(f, 2, &ack->wlan2) != 0) return -EIO;
    if (fprintf(f, "\n}\n") < 0)           return -EIO;
    return 0;
}

int opcd_snapshot_publish(const opc_get_device_info_ack_t *ack,
                          const char *path)
{
    if (!ack || !path) return -EINVAL;

    /* Compose tmp path next to the target so rename(2) stays on the same
     * filesystem (atomic semantics depend on this — cross-fs rename
     * silently degrades to copy+unlink on Linux). */
    char tmp[256];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        warn_once(path, "tmp-path", ENAMETOOLONG);
        return -ENAMETOOLONG;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        warn_once(path, "open", e);
        return -e;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        int e = errno;
        close(fd);
        unlink(tmp);
        warn_once(path, "fdopen", e);
        return -e;
    }
    int rc = write_json(f, ack);
    if (fflush(f) != 0 && rc == 0) rc = -errno;
    /* Force the bytes to the page cache before rename — readers that
     * stat() then read() must see the new contents. fsync is overkill on
     * tmpfs (no underlying storage); fflush + close is enough. fclose can
     * still surface a deferred write error, so capture it. */
    if (fclose(f) != 0 && rc == 0) rc = -errno;

    if (rc != 0) {
        unlink(tmp);
        warn_once(path, "write", -rc);
        return rc;
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        warn_once(path, "rename", e);
        return -e;
    }
    /* A clean publish re-arms the one-shot warning: if a later write fails
     * after an operator has fixed a transient problem (e.g. remounted
     * /dev/shm), the recurrence is reported instead of staying silenced for
     * the lifetime of the process. */
    g_warn_emitted = false;
    return 0;
}
