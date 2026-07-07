/* Host unit tests for roam_datagram.c — roam_parse_bssid() and
 * roam_datagram_to_evt() (the loopback roam-notify wire parser). Covers MAC
 * edge cases, mandatory-field / range validation, iface→idx mapping, and the
 * rssi/snr clamp. Pure parser: links roam_datagram.o + json_util.o +
 * chan_encode.o, no platform backend, no libopcproto. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chan_encode.h"       /* opc_chan_field (expected channel value) */
#include "platform.h"          /* opcd_platform_evt_t, OPCD_PEVT_ROAMING */
#include "roam_datagram.h"

static int fails;
#define CHECK(cond, msg) do {                       \
        if (cond) { printf("PASS %s\n", (msg)); }   \
        else { printf("FAIL %s\n", (msg)); fails++; } \
    } while (0)

int main(void)
{
    uint8_t mac[6];

    /* ---- roam_parse_bssid ---- */
    CHECK(roam_parse_bssid("04:ba:d6:ec:0b:08", mac) == 0 &&
          mac[0]==0x04 && mac[1]==0xba && mac[2]==0xd6 &&
          mac[3]==0xec && mac[4]==0x0b && mac[5]==0x08,
          "parse_bssid: valid lowercase");
    CHECK(roam_parse_bssid("AA:BB:CC:DD:EE:FF", mac) == 0 &&
          mac[0]==0xAA && mac[5]==0xFF, "parse_bssid: valid uppercase");
    CHECK(roam_parse_bssid("04:ba:d6:ec:0b", mac) != 0, "parse_bssid: too few octets");
    CHECK(roam_parse_bssid("zz:ba:d6:ec:0b:08", mac) != 0, "parse_bssid: non-hex");
    CHECK(roam_parse_bssid("", mac) != 0, "parse_bssid: empty");

    /* ---- roam_datagram_to_evt ---- */
    opcd_platform_evt_t evt;

    const char *valid = "{\"iface\":\"mlan0\",\"ap_mac\":\"04:ba:d6:ec:0b:08\","
                        "\"rssi\":-46,\"snr\":40,\"channel\":40,\"freq\":5200}";
    CHECK(roam_datagram_to_evt(valid, &evt) == 0 &&
          evt.kind == OPCD_PEVT_ROAMING &&
          evt.u.roaming.idx == 0 &&
          evt.u.roaming.rssi == -46 &&
          evt.u.roaming.snr == 40 &&
          evt.u.roaming.mac[0]==0x04 && evt.u.roaming.mac[5]==0x08 &&
          evt.u.roaming.channel == opc_chan_field(5200, 40),
          "to_evt: valid mlan0 -> filled");

    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":5200}", &evt) == 0 &&
          evt.u.roaming.idx == 0, "to_evt: absent iface -> idx 0");
    CHECK(roam_datagram_to_evt(
          "{\"iface\":\"mlan1\",\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":5200}", &evt) == 0 &&
          evt.u.roaming.idx == 1, "to_evt: mlan1 -> idx 1");
    CHECK(roam_datagram_to_evt(
          "{\"iface\":\"eth0\",\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":5200}", &evt) != 0,
          "to_evt: unknown iface -> drop");

    CHECK(roam_datagram_to_evt("{\"channel\":40,\"freq\":5200}", &evt) != 0,
          "to_evt: missing ap_mac -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"freq\":5200}", &evt) != 0,
          "to_evt: missing channel -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":0,\"freq\":5200}", &evt) != 0,
          "to_evt: channel 0 -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":256,\"freq\":5200}", &evt) != 0,
          "to_evt: channel 256 -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40}", &evt) != 0,
          "to_evt: missing freq -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":2399}", &evt) != 0,
          "to_evt: freq < 2400 -> drop");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":7301}", &evt) != 0,
          "to_evt: freq > 7300 -> drop");

    /* rssi/snr clamp + best-effort defaults */
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":5200,"
          "\"rssi\":-200,\"snr\":200}", &evt) == 0 &&
          evt.u.roaming.rssi == -128 && evt.u.roaming.snr == 127,
          "to_evt: rssi/snr clamped to int8 range");
    CHECK(roam_datagram_to_evt(
          "{\"ap_mac\":\"04:ba:d6:ec:0b:08\",\"channel\":40,\"freq\":5200}", &evt) == 0 &&
          evt.u.roaming.rssi == 0 && evt.u.roaming.snr == 0,
          "to_evt: missing rssi/snr -> 0 best-effort");

    if (fails == 0) { printf("all roam_datagram tests passed\n"); return 0; }
    printf("%d roam_datagram test(s) FAILED\n", fails);
    return 1;
}
