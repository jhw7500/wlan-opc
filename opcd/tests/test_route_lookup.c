/* Host unit test for the /proc/net/route direct-reachability lookup
 * (opcd/route_lookup.{c,h}, #159). Route values are the kernel's raw
 * little-endian hex words: 192.168.1.0 -> 0001A8C0, /24 -> 00FFFFFF. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../route_lookup.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

#define HDR "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"

/* eth0 192.168.1.0/24 + wired peer host route 10.9.9.9 + 172.16/16 via GW;
 * mlan0 192.0.2.0/24; default via 192.0.2.1 */
static const char *table =
    HDR
    "mlan0\t00000000\t010200C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
    "eth0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
    "mlan0\t000200C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n"
    "eth0\t0909090A\t00000000\t0005\t0\t0\t0\tFFFFFFFF\t0\t0\t0\n"
    "mlan0\t000010AC\t010200C0\t0003\t0\t0\t600\t0000FFFF\t0\t0\t0\n";

int main(void)
{
    uint32_t m = 0xDEADBEEFu;
    int rc;

    rc = opcd_route_direct_lookup(table, 0xC0A80132u, &m);
    ASSERT(rc == 0 && m == 0xFFFFFF00u, "direct: eth0 subnet host -> /24");
    rc = opcd_route_direct_lookup(table, 0xC0000232u, &m);
    ASSERT(rc == 0 && m == 0xFFFFFF00u, "direct: mlan0 subnet host -> /24");
    rc = opcd_route_direct_lookup(table, 0x0A090909u, &m);
    ASSERT(rc == 0 && m == 0xFFFFFFFFu, "direct: host route -> /32 (UP|HOST flags)");
    rc = opcd_route_direct_lookup(table, 0xAC100505u, &m);
    ASSERT(rc == -ENETUNREACH, "gateway route -> -ENETUNREACH");
    rc = opcd_route_direct_lookup(table, 0x0A0A0A0Au, &m);
    ASSERT(rc == -ENETUNREACH, "no matching route -> -ENETUNREACH");

    /* a default route via a gateway must not make everything reachable, and a
     * more specific direct route still wins over it */
    {
        const char *t =
            HDR
            "mlan0\t00000000\t010200C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "mlan0\t000200C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n";
        rc = opcd_route_direct_lookup(t, 0x08080808u, &m);
        ASSERT(rc == -ENETUNREACH, "default via gateway -> -ENETUNREACH");
        rc = opcd_route_direct_lookup(t, 0xC0000250u, &m);
        ASSERT(rc == 0 && m == 0xFFFFFF00u, "longest prefix: connected /24 beats default");
    }
    /* a more specific route through a gateway beats a covering direct route */
    {
        const char *t =
            HDR
            "eth0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
            "eth0\t0A01A8C0\t020100C0\t0003\t0\t0\t100\tFFFFFFFF\t0\t0\t0\n";
        rc = opcd_route_direct_lookup(t, 0xC0A8010Au, &m);
        ASSERT(rc == -ENETUNREACH, "longest prefix: /32 via gateway beats connected /24");
    }
    /* equal prefix: the lower metric decides */
    {
        const char *t =
            HDR
            "eth0\t0001A8C0\t020100C0\t0003\t0\t0\t10\t00FFFFFF\t0\t0\t0\n"
            "mlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n";
        rc = opcd_route_direct_lookup(t, 0xC0A80132u, &m);
        ASSERT(rc == -ENETUNREACH, "equal prefix: lower-metric gateway route wins");
    }
    /* not-UP and reject routes */
    {
        const char *t =
            HDR
            "eth0\t0001A8C0\t00000000\t0000\t0\t0\t100\t00FFFFFF\t0\t0\t0\n";
        rc = opcd_route_direct_lookup(t, 0xC0A80132u, &m);
        ASSERT(rc == -ENETUNREACH, "route without RTF_UP is ignored");
        t = HDR "eth0\t0001A8C0\t00000000\t0201\t0\t0\t100\t00FFFFFF\t0\t0\t0\n";
        rc = opcd_route_direct_lookup(t, 0xC0A80132u, &m);
        ASSERT(rc == -ENETUNREACH, "RTF_REJECT route is not reachable");
    }
    /* junk / empty input */
    /* throw / unreachable / blackhole routes print Iface "*" and no gateway;
     * throw and blackhole carry only RTF_UP (0001), unreachable RTF_UP|RTF_REJECT
     * (0201) — as measured in a network namespace. None of them delivers. */
    {
        static const char *t = HDR
            "d0\t0001010A\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"    /* 10.1.1.0/24 direct */
            "*\t0000620A\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"     /* throw 10.98.0.0/24 */
            "*\t0000630A\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"     /* blackhole 10.99.0.0/24 */
            "*\t0000610A\t00000000\t0201\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"     /* unreachable 10.97.0.0/24 */
            "d0\t0000000A\t00000000\t0001\t0\t0\t0\t000000FF\t0\t0\t0\n";   /* 10.0.0.0/8 direct, shorter */
        rc = opcd_route_direct_lookup(t, 0x0A620005u, &m);   /* 10.98.0.5 */
        ASSERT(rc == -ENETUNREACH, "throw route (Iface *, UP only) is not reachable even over a shorter direct prefix");
        rc = opcd_route_direct_lookup(t, 0x0A630005u, &m);   /* 10.99.0.5 */
        ASSERT(rc == -ENETUNREACH, "blackhole route (Iface *, UP only) is not reachable");
        rc = opcd_route_direct_lookup(t, 0x0A610005u, &m);   /* 10.97.0.5 */
        ASSERT(rc == -ENETUNREACH, "unreachable route (Iface *, UP|REJECT) is not reachable");
        rc = opcd_route_direct_lookup(t, 0x0A010105u, &m);   /* 10.1.1.5 */
        ASSERT(rc == 0 && m == 0xFFFFFF00u, "a real interface route next to * rows stays direct");
        rc = opcd_route_direct_lookup(t, 0x0A050505u, &m);   /* 10.5.5.5 -> the /8 */
        ASSERT(rc == 0 && m == 0xFF000000u, "the shorter direct prefix still serves hosts no * row covers");
    }

    rc = opcd_route_direct_lookup("", 0xC0A80132u, &m);
    ASSERT(rc == -ENETUNREACH, "empty table -> -ENETUNREACH");
    rc = opcd_route_direct_lookup(NULL, 0xC0A80132u, &m);
    ASSERT(rc == -ENETUNREACH, "NULL table -> -ENETUNREACH");
    rc = opcd_route_direct_lookup("garbage line\nIface Destination\n", 0xC0A80132u, &m);
    ASSERT(rc == -ENETUNREACH, "unparsable rows -> -ENETUNREACH");

    if (failures == 0) { printf("all route_lookup tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
