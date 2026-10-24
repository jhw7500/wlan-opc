#include <stdio.h>
#include <string.h>

#include "iplist_text.h"

static const struct {
    const char *name;
    uint8_t     bit;
} keys[] = {
    { "IPAddress",        OPC_IPLIST_KEY_IP    },
    { "IPSubnetMask",     OPC_IPLIST_KEY_MASK  },
    { "IPDefaultGateway", OPC_IPLIST_KEY_GW    },
    { "WL54EssId",        OPC_IPLIST_KEY_ESSID },
    { "NTPServer",        OPC_IPLIST_KEY_NTP   },
};

static int put(char *buf, size_t cap, size_t *at, const char *key, unsigned n,
               const char *fmt_value, const char *value)
{
    int w = snprintf(buf + *at, cap - *at, "%s%u=", key, n);
    if (w < 0 || (size_t)w >= cap - *at) return -1;
    *at += (size_t)w;
    w = snprintf(buf + *at, cap - *at, fmt_value, value);
    if (w < 0 || (size_t)w >= cap - *at) return -1;
    *at += (size_t)w;
    return 0;
}

static void ipv4_str(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
}

ssize_t opc_iplist_text_format(const opcd_ip_list_t *l, char *buf, size_t cap)
{
    size_t at = 0;
    if (cap == 0) return -1;
    buf[0] = '\0';
    for (unsigned i = 0; i < OPC_IPCFG_LIST_MAX_SLOTS; i++) {
        if (!l->present[i]) continue;
        const opc_ipcfg_entry_t *e = &l->slots[i];
        char ip[16], mask[16], gw[16], ntp[16], essid[OPC_ESSID_FIELD_LEN];
        ipv4_str(e->ip_address, ip);
        ipv4_str(e->subnet_mask, mask);
        ipv4_str(e->default_gateway, gw);
        ipv4_str(e->ntp_server, ntp);
        memcpy(essid, e->essid, sizeof essid);
        essid[sizeof essid - 1] = '\0';
        if (put(buf, cap, &at, "IPAddress", i + 1, "%s\n", ip) != 0 ||
            put(buf, cap, &at, "IPSubnetMask", i + 1, "%s\n", mask) != 0 ||
            put(buf, cap, &at, "IPDefaultGateway", i + 1, "%s\n", gw) != 0 ||
            put(buf, cap, &at, "WL54EssId", i + 1, "\"%s\"\n", essid) != 0 ||
            put(buf, cap, &at, "NTPServer", i + 1, "%s\n", ntp) != 0)
            return -1;
    }
    return (ssize_t)at;
}

/* Four decimal octets 0..255, no leading zeros, nothing after. */
static bool parse_ipv4(const char *s, const char *end, uint32_t *out)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (s >= end || *s != '.') return false;
            s++;
        }
        const char *d = s;
        unsigned v = 0;
        while (s < end && *s >= '0' && *s <= '9' && s - d < 3) v = v * 10 + (unsigned)(*s++ - '0');
        size_t nd = (size_t)(s - d);
        if (nd == 0 || v > 255 || (nd > 1 && *d == '0')) return false;
        if (s < end && *s >= '0' && *s <= '9') return false;   /* a fourth digit */
        ip = (ip << 8) | v;
    }
    if (s != end) return false;
    *out = ip;
    return true;
}

static void note(opc_iplist_text_t *out, int line, char *why, size_t whycap,
                 const char *reason)
{
    if (out->first_error_line != 0) return;
    out->first_error_line = line;
    if (why && whycap) snprintf(why, whycap, "line %d: %s", line, reason);
}

/* One non-blank line [s, end). */
static void parse_line(const char *s, const char *end, int line, opc_iplist_text_t *out,
                       char *why, size_t whycap)
{
    if (memchr(s, '\0', (size_t)(end - s))) { note(out, line, why, whycap, "NUL byte"); return; }

    size_t ki = 0, klen = 0;
    for (; ki < sizeof keys / sizeof keys[0]; ki++) {
        klen = strlen(keys[ki].name);
        if ((size_t)(end - s) > klen && memcmp(s, keys[ki].name, klen) == 0 &&
            s[klen] >= '0' && s[klen] <= '9')
            break;
    }
    if (ki == sizeof keys / sizeof keys[0]) { note(out, line, why, whycap, "unknown key"); return; }

    const char *p = s + klen, *d = p;
    unsigned n = 0;
    while (p < end && *p >= '0' && *p <= '9' && p - d < 4) n = n * 10 + (unsigned)(*p++ - '0');
    if (p >= end || *p != '=' || *d == '0' || n < 1 || n > OPC_IPCFG_LIST_MAX_SLOTS) {
        note(out, line, why, whycap, "list number must be 1..128 followed by '='");
        return;
    }
    p++;
    const unsigned i = n - 1;
    opc_ipcfg_entry_t *e = &out->entry[i];

    if (out->keys[i] & keys[ki].bit) {
        out->bad[i] = true;
        note(out, line, why, whycap, "key repeated for the same list number");
        return;
    }

    bool ok;
    if (keys[ki].bit == OPC_IPLIST_KEY_ESSID) {
        size_t vlen = (size_t)(end - p);
        ok = vlen >= 2 && p[0] == '"' && end[-1] == '"' &&
             vlen - 2 < sizeof e->essid && !memchr(p + 1, '"', vlen - 2);
        if (ok) {
            memset(e->essid, 0, sizeof e->essid);
            memcpy(e->essid, p + 1, vlen - 2);
        }
    } else {
        uint32_t v = 0;
        ok = parse_ipv4(p, end, &v);
        if (ok) {
            switch (keys[ki].bit) {
            case OPC_IPLIST_KEY_IP:   e->ip_address      = v; break;
            case OPC_IPLIST_KEY_MASK: e->subnet_mask     = v; break;
            case OPC_IPLIST_KEY_GW:   e->default_gateway = v; break;
            default:                  e->ntp_server      = v; break;
            }
        }
    }
    if (!ok) {
        out->bad[i] = true;
        note(out, line, why, whycap, keys[ki].bit == OPC_IPLIST_KEY_ESSID
             ? "ESSID must be \"...\" of at most 31 characters"
             : "value must be an IPv4 address a.b.c.d");
        return;
    }
    e->list_number = (uint16_t)n;
    out->keys[i] |= keys[ki].bit;
}

int opc_iplist_text_parse(const char *buf, size_t len, opc_iplist_text_t *out,
                          char *why, size_t whycap)
{
    memset(out, 0, sizeof *out);
    if (why && whycap) why[0] = '\0';
    out->final_newline = (len == 0 || buf[len - 1] == '\n');
    const char *p = buf, *stop = buf + len;
    for (int line = 1; p < stop; line++) {
        const char *nl = memchr(p, '\n', (size_t)(stop - p));
        const char *end = nl ? nl : stop;
        const char *next = nl ? nl + 1 : stop;
        if (end > p && end[-1] == '\r') end--;
        if (end > p) parse_line(p, end, line, out, why, whycap);
        p = next;
    }
    return out->first_error_line;
}
