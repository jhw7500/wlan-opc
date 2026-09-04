#ifndef WLAN_OPC_OPCD_TOPOLOGY_H
#define WLAN_OPC_OPCD_TOPOLOGY_H

/* Management-IP topology of the board (#122).
 *
 *   eth0_ip     — shipping default: the management IP sits on eth0 (or, with
 *                 opc.conf device_ip_iface=mlan0, on mlan0) and the two
 *                 interfaces live on different subnets. opcd reads/applies
 *                 the IP on the selected interface only.
 *   peer_route  — DFK Option B (user decision 2026-09-04): ONE management IP
 *                 shared by mlan0 (primary) and an eth0 /32 mirror,
 *                 with a policy rule iif=eth0→table 100, a table-100 link
 *                 route and peer host routes owned by wifi_init.sh. opcd
 *                 reads/applies on mlan0 regardless of device_ip_iface and,
 *                 after a successful ChangeIp, refreshes those artifacts
 *                 (platform peer_route_refresh) so eth0 keeps answering for
 *                 the NEW address.
 *
 * Source of truth (production): wifi_init_conf.json
 * `.wbridge.peer_route.enabled` — the same toggle wifi_init.sh acts on, so
 * opcd cannot drift from the board's actual routing. opc.conf
 * `management_topology = eth0_ip | peer_route` is an explicit OVERRIDE for
 * tests/benches; `auto` (default, or any unknown value) defers to the JSON.
 *
 * Standalone module (like ip_iface) so the resolver is host-unit-testable
 * without linking opcd.c's main(). */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPC_TOPOLOGY_ETH0_IP = 0,   /* shipping default */
    OPC_TOPOLOGY_PEER_ROUTE,
} opcd_topology_t;

/* opc.conf value space: AUTO defers to wifi_init_conf.json. */
typedef enum {
    OPC_TOPOLOGY_CONF_AUTO = 0,
    OPC_TOPOLOGY_CONF_ETH0_IP,
    OPC_TOPOLOGY_CONF_PEER_ROUTE,
} opcd_topology_conf_t;

/* "eth0_ip" / "peer_route" / "auto"; anything else (incl. NULL) → AUTO. */
opcd_topology_conf_t opcd_topology_from_token(const char *val);

/* Pure scanner for `.wbridge.peer_route.enabled` in a wifi_init_conf.json
 * text. Returns 1 (true), 0 (false), or -1 when the peer_route object or its
 * boolean `enabled` is absent/malformed. Skips string literals (the shipped
 * file's _comment strings mention the key names) and tracks brace depth so
 * only the peer_route object's own `enabled` counts. */
int opcd_topology_json_peer_route(const char *json);

/* Resolve the effective topology: opc.conf override first (last occurrence
 * of management_topology wins), else the JSON with wifi_init.sh's decision
 * table — file unreadable → ETH0_IP (degraded fallback), readable with a
 * boolean → that value, readable but key missing/invalid → PEER_ROUTE
 * (factory default). NULL paths are treated as unreadable. Never fails. */
opcd_topology_t opcd_topology_parse(const char *opc_conf_path,
                                    const char *wifi_init_conf_path);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_TOPOLOGY_H */
