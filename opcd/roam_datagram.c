/* roam-notify datagram decoding — the loopback UDP wire parser for the
 * host-driven Roaming(0x04) indication (design §9.1 WIRE CONTRACT). Kept in its
 * own translation unit so the parsing/validation logic is host unit-testable
 * (test_roam_datagram.c) without dragging in opcd's main loop. */
#include "roam_datagram.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chan_encode.h"   /* opc_chan_field */
#include "json_util.h"     /* opc_json_string, opc_json_integer */

int roam_parse_bssid(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    char tail = 0;
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail);
    /* Accept exactly the 6 octets (n==6), or 6 octets followed by one trailing
     * char (n==7) — the closing quote is stripped by opc_json_string, so a
     * clean address yields n==6. Reject anything with fewer octets. */
    if (n < 6) return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

int roam_datagram_to_evt(const char *json, opcd_platform_evt_t *evt)
{
    char mac_str[32];
    if (opc_json_string(json, "ap_mac", mac_str, sizeof mac_str) != 0)
        return -1;
    uint8_t mac[6];
    if (roam_parse_bssid(mac_str, mac) != 0)
        return -1;

    /* iface → idx: mlan0=0, mlan1=1, anything else dropped. Absent iface
     * defaults to the primary (0). An mlan1 notify is then dropped downstream
     * by the on_platform_event single-STA guard rather than mis-emitted as
     * mlan0. */
    char iface[16];
    uint8_t idx = 0;
    if (opc_json_string(json, "iface", iface, sizeof iface) == 0) {
        if (strcmp(iface, "mlan0") == 0)      idx = 0;
        else if (strcmp(iface, "mlan1") == 0) idx = 1;
        else return -1;
    }

    long rssi = 0, snr = 0, channel = 0, freq = 0;
    (void)opc_json_integer(json, "rssi", &rssi);      /* optional → 0 (best-effort) */
    (void)opc_json_integer(json, "snr", &snr);        /* optional → 0 (best-effort) */
    /* channel/freq are mandatory: a Roaming(0x04) carrying channel 0 is
     * meaningless, so drop rather than emit opc_chan_field(0, 0). */
    if (opc_json_integer(json, "channel", &channel) != 0 || channel < 1 || channel > 255)
        return -1;
    if (opc_json_integer(json, "freq", &freq) != 0 || freq < 2400 || freq > 7300)
        return -1;

    /* Clamp to int8_t range: real rssi(dBm)/snr(dB) are always in range, but a
     * stray out-of-range value would be implementation-defined on cast. */
    if (snr  >  127) snr  =  127; else if (snr  < -128) snr  = -128;
    if (rssi >  127) rssi =  127; else if (rssi < -128) rssi = -128;

    memset(evt, 0, sizeof *evt);
    evt->kind = OPCD_PEVT_ROAMING;
    evt->u.roaming.idx     = idx;
    evt->u.roaming.snr     = (int8_t)snr;
    evt->u.roaming.rssi    = (int8_t)rssi;
    memcpy(evt->u.roaming.mac, mac, sizeof mac);
    evt->u.roaming.channel = opc_chan_field((uint32_t)freq, (uint16_t)channel);
    return 0;
}
