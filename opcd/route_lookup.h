#ifndef WLAN_OPC_OPCD_ROUTE_LOOKUP_H
#define WLAN_OPC_OPCD_ROUTE_LOOKUP_H

#include <stdint.h>

/* Gateway-less reachability over a /proc/net/route dump (#159).
 *
 * SetIndicationConfig must refuse a recipient the device cannot reach
 * directly (spec §4.3.9 0x0012, "동일 IP 세그먼트 이외의 통지 주소"). The
 * kernel's main routing table is the authority: connected subnets and host
 * routes (peer_route's wired peers) have no gateway, anything else does.
 *
 * `text` is the whole /proc/net/route content (header line included). The
 * best route for `dst_host` is the longest-prefix match, ties broken by the
 * lowest metric; routes that are not UP are ignored. Returns 0 and writes the
 * matched route's netmask (host order) to *mask_host when that best route has
 * no gateway (Gateway 0 and no RTF_GATEWAY, not RTF_REJECT); -ENETUNREACH when
 * no route covers `dst_host` or the best one goes through a gateway (or is a
 * reject route). Standalone like ip_iface/topology so it is host-unit-testable
 * without linking opcd.c's main(). */
int opcd_route_direct_lookup(const char *text, uint32_t dst_host,
                             uint32_t *mask_host);

#endif /* WLAN_OPC_OPCD_ROUTE_LOOKUP_H */
