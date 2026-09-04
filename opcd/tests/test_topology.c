/* Host unit test for the management-topology resolver (opcd/topology.{c,h},
 * #122). Mirrors test_ip_iface.c: opc.conf `management_topology` is a
 * TEST/OVERRIDE knob (explicit eth0_ip / peer_route); the production source
 * is wifi_init_conf.json `.wbridge.peer_route.enabled` (auto, the default). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../topology.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static char g_conf[64], g_json[64];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* A faithful excerpt of the shipped wifi_init_conf.json: the peer_route
 * object carries a _comment array whose strings mention "peer_route.enabled"
 * and "enabled: (bool)" — a naive substring scan must not be fooled. */
static const char *json_off =
    "{\n"
    "  \"_comment\": [\"WiFi Configuration File\"],\n"
    "  \"wbridge\": {\n"
    "    \"mac_clone_require_peer\": true,\n"
    "    \"ip_discovery\": false,\n"
    "    \"peer_route\": {\n"
    "      \"_comment\": [\"peer_route.enabled: 양방향 BD↔유선peer 라우팅 마스터 토글. 기본 false.\",\n"
    "                   \"enabled=false면 부팅 시 관련 라우팅/sysctl을 모두 revert\", \"{ braces } inside\"],\n"
    "      \"enabled\": false\n"
    "    },\n"
    "    \"arp_ignore_always\": { \"_comment\": [\"enabled: (bool) arp_ignore=1\"], \"enabled\": true },\n"
    "    \"eth_fallback\": { \"enabled\": true }\n"
    "  },\n"
    "  \"mlan0\": { \"arping\": { \"enabled\": true } }\n"
    "}\n";

static const char *json_on =
    "{ \"wbridge\": { \"ip_discovery\": true,\n"
    "  \"peer_route\": { \"_comment\": [\"x\"], \"enabled\" : true },\n"
    "  \"arp_ignore_always\": { \"enabled\": false } } }\n";

int main(void)
{
    /* ---- token mapper (opc.conf value) ---- */
    ASSERT(opcd_topology_from_token("peer_route") == OPC_TOPOLOGY_CONF_PEER_ROUTE, "token peer_route");
    ASSERT(opcd_topology_from_token("eth0_ip")    == OPC_TOPOLOGY_CONF_ETH0_IP,    "token eth0_ip");
    ASSERT(opcd_topology_from_token("auto")       == OPC_TOPOLOGY_CONF_AUTO,       "token auto");
    ASSERT(opcd_topology_from_token("bogus")      == OPC_TOPOLOGY_CONF_AUTO,       "token unknown -> auto");
    ASSERT(opcd_topology_from_token(NULL)         == OPC_TOPOLOGY_CONF_AUTO,       "token NULL -> auto");
    ASSERT(opcd_topology_from_token("PEER_ROUTE") == OPC_TOPOLOGY_CONF_AUTO,       "token case-sensitive -> auto");

    /* ---- pure JSON scanner: .wbridge.peer_route.enabled ---- */
    ASSERT(opcd_topology_json_peer_route(json_off) == 0, "json: peer_route.enabled=false -> 0");
    ASSERT(opcd_topology_json_peer_route(json_on)  == 1, "json: peer_route.enabled=true -> 1");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"ip_discovery\": true } }") == -1,
           "json: no peer_route object -> -1 (unknown)");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"_comment\": [\"enabled\"] } } }") == -1,
           "json: peer_route without enabled -> -1");
    ASSERT(opcd_topology_json_peer_route("{ \"peer_route_extra\": { \"enabled\": true } }") == -1,
           "json: key prefix is not a match");
    ASSERT(opcd_topology_json_peer_route(NULL) == -1, "json: NULL -> -1");
    ASSERT(opcd_topology_json_peer_route("{ \"peer_route\": { \"enabled\": tru") == -1,
           "json: truncated document -> -1");
    /* anchored to .wbridge like jq '.wbridge.peer_route.enabled' — a
     * peer_route object under another section must not be picked up */
    ASSERT(opcd_topology_json_peer_route(
               "{ \"eth_fallback\": { \"peer_route\": { \"enabled\": true } },"
               "  \"wbridge\": { \"peer_route\": { \"enabled\": false } } }") == 0,
           "json: only .wbridge.peer_route counts (other section's peer_route ignored)");
    ASSERT(opcd_topology_json_peer_route("{ \"peer_route\": { \"enabled\": true } }") == -1,
           "json: top-level peer_route (not under wbridge) -> -1");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": true } }") == -1,
           "json: non-object peer_route -> -1 (invalid, like jq)");
    /* jq -r prints a quoted "true"/"false" string exactly like the bare
     * literal, so wifi_init.sh honours it — opcd must too (not factory default) */
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"enabled\": \"false\" } } }") == 0,
           "json: quoted \"false\" honoured like jq -r");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"enabled\": \"true\" } } }") == 1,
           "json: quoted \"true\" honoured like jq -r");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"enabled\": \"yes\" } } }") == -1,
           "json: other string is invalid -> -1");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"enabled\": truex } } }") == -1,
           "json: 'truex' is not the boolean true");
    ASSERT(opcd_topology_json_peer_route("{ \"wbridge\": { \"peer_route\": { \"nested\": { \"enabled\": true }, \"enabled\": false } } }") == 0,
           "json: enabled inside a nested object is not peer_route's enabled");

    /* ---- resolver: opc.conf override vs JSON ---- */
    snprintf(g_conf, sizeof g_conf, "test_topology_%d.conf", (int)getpid());
    snprintf(g_json, sizeof g_json, "test_topology_%d.json", (int)getpid());
    unlink(g_conf); unlink(g_json);

    ASSERT(opcd_topology_parse("/nonexistent/opc.conf", "/nonexistent/wifi_init_conf.json")
               == OPC_TOPOLOGY_ETH0_IP,
           "resolve: nothing readable -> eth0_ip (shipping default)");
    ASSERT(opcd_topology_parse(NULL, NULL) == OPC_TOPOLOGY_ETH0_IP, "resolve: NULL paths -> eth0_ip");

    write_file(g_json, json_on);
    ASSERT(opcd_topology_parse("/nonexistent/opc.conf", g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: no opc.conf + json on -> peer_route");
    write_file(g_conf, "device_ip_iface = mlan0\n# no topology key\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: opc.conf without key (auto) + json on -> peer_route");
    write_file(g_conf, "management_topology = auto\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: explicit auto + json on -> peer_route");
    write_file(g_json, json_off);
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_ETH0_IP,
           "resolve: auto + json off -> eth0_ip");
    write_file(g_conf, "management_topology = peer_route\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: opc.conf peer_route overrides json off (test knob)");
    write_file(g_json, json_on);
    write_file(g_conf, "management_topology = eth0_ip\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_ETH0_IP,
           "resolve: opc.conf eth0_ip overrides json on");
    write_file(g_conf, "management_topology = peer_route\nmanagement_topology = eth0_ip\n");
    ASSERT(opcd_topology_parse(g_conf, "/nonexistent.json") == OPC_TOPOLOGY_ETH0_IP,
           "resolve: duplicate key -> last wins");
    write_file(g_conf, "management_topology = bogus\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: unknown value = auto -> json decides");
    ASSERT(opcd_topology_parse(g_conf, "/nonexistent.json") == OPC_TOPOLOGY_ETH0_IP,
           "resolve: auto + json unreadable -> eth0_ip (wifi_init.sh degraded fallback)");
    /* wifi_init.sh: config readable but key invalid/missing → FACTORY DEFAULT
     * peer_route=true. opcd must land on the same side as the routing. */
    write_file(g_json, "{ \"wbridge\": { \"ip_discovery\": false } }\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: auto + json readable but key missing -> peer_route (factory default)");
    write_file(g_json, "{ \"wbridge\": { \"peer_route\": true } }\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_PEER_ROUTE,
           "resolve: auto + json key invalid -> peer_route (factory default)");
    write_file(g_conf, "management_topology = eth0_ip\n");
    ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_ETH0_IP,
           "resolve: explicit eth0_ip still overrides the factory default");

    /* size: the shipped wifi_init_conf.json is ~49 KiB with the toggle near
     * the top, but a large file must still be read whole (a silent cap
     * would flip the result when the key sits past it). */
    {
        FILE *f = fopen(g_json, "w");
        if (!f) { perror("fopen"); exit(2); }
        fputs("{ \"pad\": \"", f);
        for (int i = 0; i < 300 * 1024; i++) fputc('x', f);
        fputs("\", \"wbridge\": { \"peer_route\": { \"enabled\": false } } }\n", f);
        fclose(f);
        write_file(g_conf, "management_topology = auto\n");
        ASSERT(opcd_topology_parse(g_conf, g_json) == OPC_TOPOLOGY_ETH0_IP,
               "resolve: 300 KiB document with the key at the end is read whole (off)");
    }

    unlink(g_conf); unlink(g_json);
    if (failures == 0) { printf("all topology tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
