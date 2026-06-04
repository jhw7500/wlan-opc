/*
 * JSON value extractor — shared by platform_nxp.c (cantops link.json reader)
 * and inventory.c (/usr/local/opc/etc/device_info.json loader).
 *
 * History: extracted from platform_nxp.c verbatim, then prefixed with
 * opc_json_ and given top-level (no section) helpers for device_info.json.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_util.h"

/* Cap the file read to defend against accidentally pointing at a huge log.
 * Same value as the platform_nxp.c original; sized for cantops link.json
 * (~few KB) and device_info.json (~few hundred bytes). */
#define OPC_JSON_MAX_BYTES (1u * 1024u * 1024u)

char *opc_json_slurp_file(const char *path)
{
    int saved_errno;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
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
    if ((unsigned long)sz > OPC_JSON_MAX_BYTES) {
        fclose(f);
        errno = EFBIG;
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
    saved_errno = errno;
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        errno = saved_errno ? saved_errno : EIO;
        return NULL;
    }
    buf[sz] = '\0';
    return buf;
}

/* Locate `"<key>"` in `json` followed by a colon or JSON whitespace, i.e. the
 * key position itself — not its value. Returns a pointer to the opening quote
 * or NULL if not found. Used by all four public extractors below to share the
 * prefix-safe matching logic. */
static const char *find_key_quote(const char *json, const char *needle,
                                  size_t needle_len)
{
    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        char a = p[needle_len];
        if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
            return p;
        }
        p++;
    }
    return NULL;
}

/* Advance `p` past `:` and any JSON whitespace, returning the position of the
 * value's first character. Returns NULL if no colon precedes the value. */
static const char *advance_to_value(const char *p)
{
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int copy_quoted_value(const char *p, char *out, size_t cap)
{
    if (*p != '"') return -ENOENT;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

int opc_json_string(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return -EINVAL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof needle) return -EINVAL;

    const char *p = find_key_quote(json, needle, (size_t)n);
    if (!p) return -ENOENT;
    p = advance_to_value(p + n);
    if (!p) return -ENOENT;
    return copy_quoted_value(p, out, cap);
}

int opc_json_integer(const char *json, const char *key, long *out)
{
    if (!json || !key || !out) return -EINVAL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof needle) return -EINVAL;

    const char *p = find_key_quote(json, needle, (size_t)n);
    if (!p) return -ENOENT;
    p = advance_to_value(p + n);
    if (!p) return -ENOENT;
    char *endp = NULL;
    long v = strtol(p, &endp, 10);
    if (endp == p) return -EINVAL;
    *out = v;
    return 0;
}

/* Locate `"<section>": { ... }` and return a pointer to the byte AFTER the
 * opening brace. NULL if section is not present. */
static const char *find_section_body(const char *json, const char *section)
{
    char needle[64];
    int ns = snprintf(needle, sizeof needle, "\"%s\"", section);
    if (ns < 0 || (size_t)ns >= sizeof needle) return NULL;
    const char *sec = find_key_quote(json, needle, (size_t)ns);
    if (!sec) return NULL;
    sec = strchr(sec, '{');
    if (!sec) return NULL;
    return sec + 1;
}

/* Advance past a JSON string literal starting at `p` (which must point at
 * the opening quote). Handles backslash-escape (e.g. `\"`) so values like
 * `"essid": "weird}name"` cannot confuse brace-depth tracking. Returns
 * the position immediately after the closing quote, or end of buffer if
 * input is malformed. */
static const char *skip_json_string(const char *p)
{
    if (*p != '"') return p;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p += 2;
        else                    p++;
    }
    if (*p == '"') p++;
    return p;
}

int opc_json_string_section(const char *json, const char *section,
                            const char *key, char *out, size_t cap)
{
    if (!json || !section || !key || !out || cap == 0) return -EINVAL;
    const char *body = find_section_body(json, section);
    if (!body) return -ENOENT;

    char needle_key[64];
    int nk = snprintf(needle_key, sizeof needle_key, "\"%s\"", key);
    if (nk < 0 || (size_t)nk >= sizeof needle_key) return -EINVAL;

    /* Key match must happen outside any string literal — otherwise an
     * SSID like `"essid": "\"signal\": 0"` could spoof a key. The scan
     * tries a key match at the current byte first; then, on a `"` we
     * atomically skip the whole literal (escape-aware) so its contents
     * never participate in either brace-depth tracking or key matching. */
    int depth = 1;
    const char *p = body;
    while (*p && depth > 0) {
        if (strncmp(p, needle_key, (size_t)nk) == 0) {
            char a = p[nk];
            if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
                const char *v = advance_to_value(p + nk);
                if (!v) return -ENOENT;
                return copy_quoted_value(v, out, cap);
            }
        }
        if (*p == '"') { p = skip_json_string(p); continue; }
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; if (depth == 0) break; p++; continue; }
        p++;
    }
    return -ENOENT;
}

int opc_json_integer_section(const char *json, const char *section,
                             const char *key, long *out)
{
    if (!json || !section || !key || !out) return -EINVAL;
    const char *body = find_section_body(json, section);
    if (!body) return -ENOENT;

    char needle_key[64];
    int nk = snprintf(needle_key, sizeof needle_key, "\"%s\"", key);
    if (nk < 0 || (size_t)nk >= sizeof needle_key) return -EINVAL;

    int depth = 1;
    const char *p = body;
    while (*p && depth > 0) {
        if (strncmp(p, needle_key, (size_t)nk) == 0) {
            char a = p[nk];
            if (a == ':' || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
                const char *v = advance_to_value(p + nk);
                if (!v) return -ENOENT;
                char *endp = NULL;
                long val = strtol(v, &endp, 10);
                if (endp == v) return -EINVAL;
                *out = val;
                return 0;
            }
        }
        if (*p == '"') { p = skip_json_string(p); continue; }
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; if (depth == 0) break; p++; continue; }
        p++;
    }
    return -ENOENT;
}
