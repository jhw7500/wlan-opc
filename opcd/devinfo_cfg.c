#include <stdlib.h>
#include <string.h>

#include "devinfo_cfg.h"
#include "json_util.h"
#include "../protocol/ids.h"

/* Past the string literal starting at `p` (escape-aware); NULL if unterminated. */
static const char *skip_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') { if (!p[1]) return NULL; p++; continue; }
        if (*p == '"')  return p + 1;
    }
    return NULL;
}

/* `p` starts the key token `"name"` followed by optional whitespace and ':'.
 * On a match *val points at the value's first character. */
static int at_key(const char *p, const char *name, const char **val)
{
    size_t n = strlen(name);
    if (*p != '"' || strncmp(p + 1, name, n) != 0 || p[1 + n] != '"') return 0;
    p += 2 + n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    *val = p;
    return 1;
}

/* Value of `name` as a direct member of the object `obj` points at ('{'),
 * the same path discipline as jq '.a.b'. NULL when absent. */
static const char *member(const char *obj, const char *name)
{
    if (!obj || *obj != '{') return NULL;
    int depth = 0;
    for (const char *p = obj; *p; ) {
        if (*p == '"') {
            const char *val;
            if (depth == 1 && at_key(p, name, &val)) return val;
            p = skip_string(p);
            if (!p) return NULL;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0) return NULL;
        p++;
    }
    return NULL;
}

/* A scalar value as text: a string's contents (an escaped one is not a value
 * these keys take) or a bare token such as a number. -1 for anything else. */
static int scalar(const char *v, char *out, size_t cap)
{
    if (!v) return -1;
    size_t n = 0;
    if (*v == '"') {
        for (v++; *v != '"'; v++) {
            if (!*v || *v == '\\' || n + 1 >= cap) return -1;
            out[n++] = *v;
        }
    } else {
        for (; (*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'z'); v++) {
            if (n + 1 >= cap) return -1;
            out[n++] = *v;
        }
        if (n == 0) return -1;
    }
    out[n] = '\0';
    return 0;
}

/* .radio.mode as wifi_init_mode_to_bandcfg_mask accepts it; mlan1 skips ax. */
static uint8_t mode_from_radio(const char *t, int mlan1)
{
    if (strcmp(t, "b") == 0)  return OPC_WLAN_MODE_11B;
    if (strcmp(t, "g") == 0)  return OPC_WLAN_MODE_11G;
    if (strcmp(t, "a") == 0)  return OPC_WLAN_MODE_11A;
    if (strcmp(t, "n") == 0)  return OPC_WLAN_MODE_11N;
    if (strcmp(t, "ac") == 0) return OPC_WLAN_MODE_11AC;
    if (strcmp(t, "ax") == 0 && !mlan1) return OPC_WLAN_MODE_11AX;
    return OPC_WLAN_MODE_UNSET;
}

/* STANDARD as wifi_init.sh _apply_dev_cap_mask reads it: case-insensitive,
 * 4/5/6 aliases, and a value at or above the interface maximum is that maximum. */
static uint8_t mode_from_standard(const char *t, int mlan1)
{
    char s[8];
    size_t n = strlen(t);
    if (n >= sizeof s) return OPC_WLAN_MODE_UNSET;
    for (size_t i = 0; i <= n; i++)
        s[i] = (t[i] >= 'A' && t[i] <= 'Z') ? (char)(t[i] - 'A' + 'a') : t[i];
    if (strcmp(s, "n") == 0 || strcmp(s, "4") == 0)  return OPC_WLAN_MODE_11N;
    if (strcmp(s, "ac") == 0 || strcmp(s, "5") == 0) return OPC_WLAN_MODE_11AC;
    if (strcmp(s, "ax") == 0 || strcmp(s, "6") == 0)
        return mlan1 ? OPC_WLAN_MODE_11AC : OPC_WLAN_MODE_11AX;
    return OPC_WLAN_MODE_UNSET;
}

/* jq -r '.x.STANDARD // ""': a non-empty scalar; absent, null and false are empty. */
static int std_value(const char *v, char *out, size_t cap)
{
    if (scalar(v, out, cap) != 0) return 0;
    return out[0] && strcmp(out, "null") != 0 && strcmp(out, "false") != 0;
}

static uint8_t bw_from_token(const char *t)
{
    if (strcmp(t, "20") == 0) return OPC_BANDWIDTH_20;
    if (strcmp(t, "40") == 0) return OPC_BANDWIDTH_40;
    if (strcmp(t, "80") == 0) return OPC_BANDWIDTH_80;
    return OPC_BANDWIDTH_UNSET;
}

void opcd_devinfo_cfg_from_json(const char *json, const char *iface,
                                uint8_t *mode, uint8_t *bandwidth)
{
    *mode = OPC_WLAN_MODE_UNSET;
    *bandwidth = OPC_BANDWIDTH_UNSET;
    if (!json || !iface) return;
    const char *root = json;
    if (strncmp(root, "\xEF\xBB\xBF", 3) == 0) root += 3;
    while (*root == ' ' || *root == '\t' || *root == '\r' || *root == '\n') root++;
    const int mlan1 = strcmp(iface, "mlan1") == 0;
    const char *ifo = member(root, iface);
    const char *radio = member(ifo, "radio");
    char t[16];
    if (scalar(member(radio, "mode"), t, sizeof t) == 0) *mode = mode_from_radio(t, mlan1);
    if (scalar(member(radio, "bw"), t, sizeof t) == 0)   *bandwidth = bw_from_token(t);
    if (*mode != OPC_WLAN_MODE_UNSET) return;
    /* An invalid or skipped radio.mode leaves the STANDARD limit in force. The
     * interface STANDARD wins when non-empty, even when invalid (then no limit,
     * as wifi_init.sh); only an empty one falls back to .global.STANDARD. */
    if (!std_value(member(ifo, "STANDARD"), t, sizeof t) &&
        !std_value(member(member(root, "global"), "STANDARD"), t, sizeof t)) return;
    *mode = mode_from_standard(t, mlan1);
}

void opcd_devinfo_cfg_read(const char *path, const char *iface,
                           uint8_t *mode, uint8_t *bandwidth)
{
    char *json = path ? opc_json_slurp_file(path) : NULL;
    opcd_devinfo_cfg_from_json(json, iface, mode, bandwidth);
    free(json);
}
