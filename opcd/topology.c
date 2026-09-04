#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topology.h"

opcd_topology_conf_t opcd_topology_from_token(const char *val)
{
    if (!val) return OPC_TOPOLOGY_CONF_AUTO;
    if (strcmp(val, "peer_route") == 0) return OPC_TOPOLOGY_CONF_PEER_ROUTE;
    if (strcmp(val, "eth0_ip") == 0)    return OPC_TOPOLOGY_CONF_ETH0_IP;
    return OPC_TOPOLOGY_CONF_AUTO;      /* "auto" and anything unrecognized */
}

/* Advance past a JSON string literal starting at the opening quote `*p`.
 * Returns the position after the closing quote, or NULL if unterminated. */
static const char *skip_string(const char *p)
{
    if (*p != '"') return p;
    for (p++; *p; p++) {
        if (*p == '\\') { if (!p[1]) return NULL; p++; continue; }
        if (*p == '"')  return p + 1;
    }
    return NULL;
}

/* True iff `p` starts with the JSON key token `"name"` followed by optional
 * whitespace and ':' — a key, not a prefix of a longer key and not text
 * inside a string value (the caller only tests at string-literal starts). */
static int at_key(const char *p, const char *name, const char **after_colon)
{
    size_t n = strlen(name);
    if (*p != '"' || strncmp(p + 1, name, n) != 0 || p[1 + n] != '"') return 0;
    p += 2 + n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    *after_colon = p;
    return 1;
}

/* Find `"name": {` as a KEY at exactly `want_depth` brace depth, scanning
 * from `p` (string-literal aware). Returns the position of the value ('{')
 * or NULL. Stops when the enclosing object closes (depth drops below
 * want_depth) or the document ends. */
static const char *find_object_key(const char *p, const char *name, int want_depth)
{
    int depth = 0;
    while (*p) {
        if (*p == '"') {
            const char *val;
            if (depth == want_depth && at_key(p, name, &val)) return val;
            p = skip_string(p);
            if (!p) return NULL;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') { if (--depth < want_depth) return NULL; }
        p++;
    }
    return NULL;
}

/* Boolean literal followed by a JSON delimiter (not a prefix like "truex"),
 * or the quoted strings "true"/"false" — `jq -r` prints those identically
 * to the bare literals, so wifi_init.sh honours them and opcd must agree. */
static int json_bool_at(const char *v)
{
    int quoted = (*v == '"');
    if (quoted) v++;
    size_t n = strncmp(v, "true", 4) == 0 ? 4 : strncmp(v, "false", 5) == 0 ? 5 : 0;
    if (!n) return -1;
    char c = v[n];
    if (quoted ? (c != '"')
               : !(c == '\0' || c == ',' || c == '}' || c == ' ' || c == '\t' ||
                   c == '\r' || c == '\n')) return -1;
    return n == 4;
}

int opcd_topology_json_peer_route(const char *json)
{
    if (!json) return -1;
    /* .wbridge (depth 1) → .peer_route (depth 2) → .enabled (depth 3), the
     * same path jq '.wbridge.peer_route.enabled' walks in wifi_init.sh. */
    const char *wb = find_object_key(json, "wbridge", 1);
    if (!wb || *wb != '{') return -1;
    const char *pr = find_object_key(wb, "peer_route", 1);
    if (!pr || *pr != '{') return -1;   /* absent or non-object → invalid */
    int depth = 0;
    const char *p = pr;
    while (*p) {
        if (*p == '"') {
            const char *val;
            if (depth == 1 && at_key(p, "enabled", &val)) return json_bool_at(val);
            p = skip_string(p);
            if (!p) return -1;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return -1; }   /* object closed */
        p++;
    }
    return -1;   /* truncated document */
}

static opcd_topology_conf_t conf_override(const char *conf_path)
{
    opcd_topology_conf_t v = OPC_TOPOLOGY_CONF_AUTO;
    if (!conf_path) return v;
    FILE *f = fopen(conf_path, "r");
    if (!f) return v;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        /* over-long line guard, as in ip_iface.c / freq_source.c */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { }
            continue;
        }
        char key[48], val[64];
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "management_topology") != 0) continue;
        v = opcd_topology_from_token(val);   /* last occurrence wins */
    }
    fclose(f);
    return v;
}

/* Whole-file read. The shipped wifi_init_conf.json is ~49 KiB (comment
 * strings); a 1 MiB cap is far above it, and a file that fills the cap is
 * reported as unreadable rather than silently truncated (a truncation could
 * cut the toggle off and flip the topology). */
#define TOPOLOGY_JSON_CAP (1024 * 1024)
static char *slurp(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = malloc(TOPOLOGY_JSON_CAP + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, TOPOLOGY_JSON_CAP, f);
    fclose(f);
    if (n == TOPOLOGY_JSON_CAP) {
        fprintf(stderr, "opcd: topology: %s exceeds %d bytes — treated as unreadable\n",
                path, TOPOLOGY_JSON_CAP);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

opcd_topology_t opcd_topology_parse(const char *opc_conf_path,
                                    const char *wifi_init_conf_path)
{
    switch (conf_override(opc_conf_path)) {
    case OPC_TOPOLOGY_CONF_PEER_ROUTE: return OPC_TOPOLOGY_PEER_ROUTE;
    case OPC_TOPOLOGY_CONF_ETH0_IP:    return OPC_TOPOLOGY_ETH0_IP;
    case OPC_TOPOLOGY_CONF_AUTO:       break;
    }
    /* Same decision table as wifi_init.sh's peer_route block: file
     * unreadable → degraded fallback OFF; file readable → the boolean, or
     * the FACTORY DEFAULT (ON) when the key is missing/invalid. opcd must
     * land on the same side as the routing the script actually installed. */
    char *json = slurp(wifi_init_conf_path);
    if (!json) {
        fprintf(stderr, "opcd: topology: %s unreadable — management topology eth0_ip "
                        "(degraded fallback, as wifi_init.sh)\n",
                wifi_init_conf_path ? wifi_init_conf_path : "(null)");
        return OPC_TOPOLOGY_ETH0_IP;
    }
    int on = opcd_topology_json_peer_route(json);
    free(json);
    if (on < 0)
        fprintf(stderr, "opcd: topology: .wbridge.peer_route.enabled missing/invalid — "
                        "factory default peer_route (as wifi_init.sh)\n");
    return on == 0 ? OPC_TOPOLOGY_ETH0_IP : OPC_TOPOLOGY_PEER_ROUTE;
}
