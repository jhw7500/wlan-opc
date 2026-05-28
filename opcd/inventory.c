/*
 * Inventory loader for /usr/local/opc/etc/device_info.json.
 *
 * JSON shape (see installer/etc/device_info.json for the canonical example):
 *
 *   {
 *     "vendor_code":      "0x00902CFB",
 *     "product_code":     "0xFE03",
 *     "product_subcode":  "0x0001",
 *     "hardware_version": "HW-1.0.0",
 *     "serial_number":    "SN-0000-0000",
 *     "manufacture_date": "2026-02-28",
 *     "shipment_date":    "2026-03-15",
 *     "ieee_11r":  1,
 *     "ieee_11ai": 1,
 *     "ieee_11k":  1,
 *     "ieee_11v":  1
 *   }
 *
 * Numeric codes are quoted strings (hex literals) rather than JSON numbers
 * so that operators editing the file can tell at a glance these are IDs,
 * not measurements. The loader accepts any base via strtoul("0", ...).
 *
 * Dates are "YYYY-MM-DD" strings — easier to author than the binary
 * opc_date_t triple, and tooling-friendly.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inventory.h"
#include "json_util.h"

/* Process-wide inventory. Zero-initialised — that is what gets served if
 * opcd_inventory_load() is never called or fails. */
static opcd_inventory_t g_inv;

const opcd_inventory_t *opcd_inventory(void)
{
    return &g_inv;
}

/* Parse "0x00902CFB" / "9434363" / "0o12" → unsigned long. Returns 0 on
 * success. strtoul base=0 honours the C-style prefix. */
static int parse_unsigned(const char *s, unsigned long *out)
{
    if (!s || !*s) return -EINVAL;
    char *endp = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &endp, 0);
    if (errno != 0 || endp == s) return -EINVAL;
    *out = v;
    return 0;
}

/* Parse "YYYY-MM-DD" into opc_date_t. Other separators (slash, dot) and
 * 2-digit years are rejected — the file is authored, not user-typed, so
 * strictness here surfaces accidental hand-edits early. */
static int parse_date(const char *s, opc_date_t *out)
{
    unsigned y = 0, m = 0, d = 0;
    if (!s) return -EINVAL;
    if (sscanf(s, "%4u-%2u-%2u", &y, &m, &d) != 3) return -EINVAL;
    if (y < 1970 || y > 9999) return -EINVAL;
    if (m < 1 || m > 12)      return -EINVAL;
    if (d < 1 || d > 31)      return -EINVAL;
    out->year  = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day   = (uint8_t)d;
    return 0;
}

/* Extract a quoted string into `out`. Truncation is silent (consistent with
 * the rest of opcd's string-field policy). Returns 0 on success, negative
 * if the key is missing or unreadable. */
static int load_string(const char *json, const char *key,
                       char *out, size_t cap)
{
    return opc_json_string(json, key, out, cap);
}

/* Load a quoted hex/decimal field into an unsigned 32-bit code. */
static int load_code32(const char *json, const char *key, uint32_t *out)
{
    char buf[32] = {0};
    int rc = opc_json_string(json, key, buf, sizeof buf);
    if (rc != 0) return rc;
    unsigned long v = 0;
    if (parse_unsigned(buf, &v) != 0) return -EINVAL;
    *out = (uint32_t)v;
    return 0;
}

static int load_code16(const char *json, const char *key, uint16_t *out)
{
    char buf[32] = {0};
    int rc = opc_json_string(json, key, buf, sizeof buf);
    if (rc != 0) return rc;
    unsigned long v = 0;
    if (parse_unsigned(buf, &v) != 0) return -EINVAL;
    if (v > 0xFFFFu) return -EINVAL;
    *out = (uint16_t)v;
    return 0;
}

/* Load a JSON integer (0/1) into a uint8 capability flag. Any value other
 * than 0 is clamped to 1, so an operator who writes `"ieee_11r": 7` still
 * gets a sensible "supported" answer. */
static int load_cap_bit(const char *json, const char *key, uint8_t *out)
{
    long v = 0;
    int rc = opc_json_integer(json, key, &v);
    if (rc != 0) return rc;
    *out = (v != 0) ? 1u : 0u;
    return 0;
}

int opcd_inventory_load(const char *path)
{
    if (!path) return -EINVAL;

    char *json = opc_json_slurp_file(path);
    if (!json) {
        int e = errno;
        fprintf(stderr, "opcd: inventory: cannot read %s: %s\n",
                path, strerror(e));
        return -e;
    }

    opcd_inventory_t scratch = g_inv;     /* preserve previous on missing keys */
    int recovered = 0;

    if (load_code32(json, "vendor_code", &scratch.vendor_code) == 0)
        recovered++;
    if (load_code16(json, "product_code", &scratch.product_code) == 0)
        recovered++;
    if (load_code16(json, "product_subcode", &scratch.product_subcode) == 0)
        recovered++;

    /* Read directly into the destination so the loader does not need an
     * intermediate buffer or strncpy with its truncation warning quirks.
     * load_string truncates silently to the destination capacity. */
    if (load_string(json, "hardware_version", scratch.hardware_version,
                    sizeof scratch.hardware_version) == 0) {
        recovered++;
    }
    if (load_string(json, "serial_number", scratch.serial_number,
                    sizeof scratch.serial_number) == 0) {
        recovered++;
    }
    char sbuf[16] = {0};

    memset(sbuf, 0, sizeof sbuf);
    if (load_string(json, "manufacture_date", sbuf, sizeof sbuf) == 0 &&
        parse_date(sbuf, &scratch.manufacture_date) == 0) {
        recovered++;
    }
    memset(sbuf, 0, sizeof sbuf);
    if (load_string(json, "shipment_date", sbuf, sizeof sbuf) == 0 &&
        parse_date(sbuf, &scratch.shipment_date) == 0) {
        recovered++;
    }

    if (load_cap_bit(json, "ieee_11r",  &scratch.ieee_11r)  == 0) recovered++;
    if (load_cap_bit(json, "ieee_11ai", &scratch.ieee_11ai) == 0) recovered++;
    if (load_cap_bit(json, "ieee_11k",  &scratch.ieee_11k)  == 0) recovered++;
    if (load_cap_bit(json, "ieee_11v",  &scratch.ieee_11v)  == 0) recovered++;

    free(json);

    if (recovered == 0) {
        fprintf(stderr, "opcd: inventory: %s has no recognised fields\n", path);
        return -EINVAL;
    }
    g_inv = scratch;
    return 0;
}
