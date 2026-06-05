/*
 * Unit tests for vhlctl field-level hex disassembly (fielddump).
 *
 * Strategy: use the protocol pack functions (opc_*_ack_pack / opc_ind_*_pack)
 * as ground truth. We build a frame with known values, run fd_dump_*, and
 * assert the decoded value appears in the output. If a descriptor offset is
 * wrong relative to commands.c/indications.c, the value won't match and the
 * test fails — keeping fielddump.c in sync with the wire codec automatically.
 *
 * fd_render is tested directly with hand-built buffers for each kind.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fielddump.h"
#include "commands.h"
#include "indications.h"
#include "ids.h"
#include "proto.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                          \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }     \
    else         { fprintf(stdout, "PASS %s\n", label); }                 \
} while (0)

static uint32_t ipv4_host(const char *s)
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return ntohl(a.s_addr);
}

/* True if the line containing `label` also contains `val` (on the same line).
 * Catches offset drift: a shifted field puts the wrong value on a label's line. */
static int line_has(const char *buf, const char *label, const char *val)
{
    const char *p = strstr(buf, label);
    if (!p) return 0;
    const char *eol = strchr(p, '\n');
    const char *v = strstr(p, val);
    return v != NULL && (eol == NULL || v < eol);
}

/* ---- fd_render: one field per kind ---- */

static void test_render_kinds(void)
{
    char out[256];

    uint8_t u32[4] = {0x00, 0x90, 0x2c, 0xfb};
    fd_field_t fu32 = {"vendor_code", 4, FD_U32BE};
    fd_render(out, sizeof out, u32, sizeof u32, 0, &fu32);
    ASSERT(strstr(out, "vendor_code") != NULL, "render u32: label");
    ASSERT(strstr(out, "00 90 2c fb") != NULL, "render u32: raw hex");
    ASSERT(strstr(out, "0x00902cfb") != NULL, "render u32: decoded");

    uint8_t u16[2] = {0x02, 0x24};
    fd_field_t fu16 = {"channel", 2, FD_U16BE};
    fd_render(out, sizeof out, u16, sizeof u16, 0, &fu16);
    ASSERT(strstr(out, "0x0224") != NULL, "render u16: decoded");

    uint8_t mac[6] = {0xda, 0x87, 0xc8, 0x88, 0xcd, 0x80};
    fd_field_t fmac = {"mac", 6, FD_MAC};
    fd_render(out, sizeof out, mac, sizeof mac, 0, &fmac);
    ASSERT(strstr(out, "da:87:c8:88:cd:80") != NULL, "render mac");

    uint8_t neg[1] = {0xb9}; /* -71 */
    fd_field_t fi8 = {"rssi", 1, FD_I8};
    fd_render(out, sizeof out, neg, sizeof neg, 0, &fi8);
    ASSERT(strstr(out, "-71") != NULL, "render i8 negative");

    uint8_t str[8] = {'j','h','w','_','w','l','a','n'};
    fd_field_t fstr = {"essid", 8, FD_STR};
    fd_render(out, sizeof out, str, sizeof str, 0, &fstr);
    ASSERT(strstr(out, "jhw_wlan") != NULL, "render str");

    uint8_t ip[4] = {192, 168, 0, 99};
    fd_field_t fip = {"ntp_server", 4, FD_IPV4};
    fd_render(out, sizeof out, ip, sizeof ip, 0, &fip);
    ASSERT(strstr(out, "192.168.0.99") != NULL, "render ipv4");

    uint8_t date[4] = {0x07, 0xea, 0x02, 0x1c}; /* 2026-02-28 */
    fd_field_t fdate = {"manufacture", 4, FD_DATE};
    fd_render(out, sizeof out, date, sizeof date, 0, &fdate);
    ASSERT(strstr(out, "2026-02-28") != NULL, "render date");
}

static void test_render_bounds(void)
{
    char out[256];
    uint8_t frame[2] = {0, 0};
    fd_field_t f = {"vendor_code", 4, FD_U32BE}; /* needs 4, only 2 avail */
    fd_render(out, sizeof out, frame, sizeof frame, 0, &f);
    ASSERT(strstr(out, "truncated") != NULL, "render out-of-range -> truncated");
}

/* ---- fd_dump_device_info: pack as ground truth ---- */

static void test_dump_device_info(void)
{
    opc_get_device_info_ack_t ack;
    memset(&ack, 0, sizeof ack);
    ack.result        = OPC_RESULT_OK;
    ack.error_cause   = OPC_ERR_NONE;
    ack.vendor_code   = 0x00902cfb;
    ack.product_code  = 0xfe03;
    ack.product_subcode = 0x0001;
    ack.manufacture   = (opc_date_t){.year = 2026, .month = 2, .day = 28};
    strcpy(ack.firmware_version, "0.3.0");
    strcpy(ack.hardware_version, "HW-1.0.0");
    strcpy(ack.serial_number,    "SN-0000-0000");
    uint8_t emac[6] = {0xda,0x87,0xc8,0x88,0xcd,0x80};
    memcpy(ack.ethernet_mac, emac, 6);
    ack.ntp_server    = ipv4_host("192.168.0.99");
    strcpy(ack.essid, "jhw_wlan");
    ack.device_status = OPC_DEVICE_LOGGED_IN;
    ack.station_type  = OPC_STATION_SINGLE;
    ack.ieee_11r = 1;
    ack.wlan1.freq_mhz = 5200;
    ack.wlan1.channel  = 0x0224;
    ack.wlan1.snr  = 21;
    ack.wlan1.rssi = -71;
    uint8_t apmac[6] = {0x04,0xba,0xd6,0xec,0x0b,0x08};
    memcpy(ack.wlan1.connect_ap_mac, apmac, 6);

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t n = opc_get_device_info_ack_pack(frame, sizeof frame, 1, &ack);
    ASSERT(n > 0, "dump devinfo: pack ok");

    char *buf = NULL; size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    fd_dump_device_info(fp, frame, (size_t)n);
    fclose(fp);

    ASSERT(strstr(buf, "0x00902cfb")     != NULL, "dump devinfo: vendor @offset");
    ASSERT(strstr(buf, "0xfe03")         != NULL, "dump devinfo: product");
    ASSERT(strstr(buf, "2026-02-28")     != NULL, "dump devinfo: manufacture date");
    ASSERT(strstr(buf, "0.3.0")          != NULL, "dump devinfo: firmware");
    ASSERT(strstr(buf, "jhw_wlan")       != NULL, "dump devinfo: essid @offset");
    ASSERT(strstr(buf, "192.168.0.99")   != NULL, "dump devinfo: ntp ipv4");
    ASSERT(strstr(buf, "da:87:c8:88:cd:80") != NULL, "dump devinfo: eth mac");
    ASSERT(strstr(buf, "0x0224")         != NULL, "dump devinfo: wlan1 channel");
    ASSERT(strstr(buf, "-71")            != NULL, "dump devinfo: wlan1 rssi @offset");
    ASSERT(strstr(buf, "04:ba:d6:ec:0b:08") != NULL, "dump devinfo: wlan1 ap mac");
    /* header common fields */
    ASSERT(strstr(buf, "command_type")   != NULL, "dump devinfo: header command_type");
    free(buf);
}

static void test_dump_basic_info(void)
{
    opc_get_basic_info_ack_t ack;
    memset(&ack, 0, sizeof ack);
    ack.vendor_code     = 0x00902cfb;
    ack.product_code    = 0xfe03;
    ack.product_subcode = 0x0001;
    ack.device_status   = OPC_DEVICE_READY;   /* 0x00000001 */
    ack.station_type    = OPC_STATION_DUAL;   /* 0x0002 — distinct from the reserve gap (0) */

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t n = opc_get_basic_info_ack_pack(frame, sizeof frame, 7, &ack);
    ASSERT(n > 0, "dump basicinfo: pack ok");

    char *buf = NULL; size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    fd_dump_basic_info(fp, frame, (size_t)n);
    fclose(fp);

    ASSERT(line_has(buf, "vendor_code",   "0x00902cfb"), "dump basicinfo: vendor @offset");
    ASSERT(line_has(buf, "product_code",  "0xfe03"),     "dump basicinfo: product @offset");
    ASSERT(line_has(buf, "device_status", "0x00000001"), "dump basicinfo: device_status @offset");
    ASSERT(line_has(buf, "station_type",  "0x0002"),     "dump basicinfo: station_type @offset (reserve gap honored)");
    free(buf);
}

static void test_dump_indication_keepalive(void)
{
    opc_ind_keep_alive_t in;
    memset(&in, 0, sizeof in);
    strcpy(in.timestamp, "2026-02-16T15:47:00Z");

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t n = opc_ind_keep_alive_pack(frame, sizeof frame, 3, &in);
    ASSERT(n > 0, "dump keepalive: pack ok");

    char *buf = NULL; size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    fd_dump_indication(fp, OPC_IND_KEEP_ALIVE, frame, (size_t)n);
    fclose(fp);

    ASSERT(strstr(buf, "2026-02-16T15:47:00Z") != NULL, "dump keepalive: timestamp @offset");
    free(buf);
}

static void test_dump_indication_roaming(void)
{
    opc_ind_roaming_t in;
    memset(&in, 0, sizeof in);
    in.current_snr  = 21;
    in.current_rssi = -71;
    in.ch_number    = 0x0224;
    uint8_t apmac[6] = {0x04,0xba,0xd6,0xec,0x0b,0x08};
    memcpy(in.connect_ap_mac, apmac, 6);

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t n = opc_ind_roaming_pack(frame, sizeof frame, 4, &in);
    ASSERT(n > 0, "dump roaming: pack ok");

    char *buf = NULL; size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    fd_dump_indication(fp, OPC_IND_ROAMING, frame, (size_t)n);
    fclose(fp);

    ASSERT(strstr(buf, "-71")               != NULL, "dump roaming: rssi @offset");
    ASSERT(strstr(buf, "04:ba:d6:ec:0b:08") != NULL, "dump roaming: ap mac @offset");
    ASSERT(strstr(buf, "0x0224")            != NULL, "dump roaming: ch @offset");
    free(buf);
}

/* Remaining 5 indication tables — happy-path offset check per type so drift in
 * any of them fails make check (not just keepalive/roaming). */
static void test_dump_indication_rest(void)
{
    char *buf; size_t sz; FILE *fp;
    uint8_t frame[OPC_FRAME_MAX]; ssize_t n;

    opc_ind_init_complete_t init; memset(&init, 0, sizeof init);
    init.status = 0x00000002;
    n = opc_ind_init_complete_pack(frame, sizeof frame, 1, &init);
    fp = open_memstream(&buf, &sz); fd_dump_indication(fp, OPC_IND_INIT_COMPLETE, frame, (size_t)n); fclose(fp);
    ASSERT(line_has(buf, "status", "0x00000002"), "ind init: status @offset");
    free(buf);

    opc_ind_wlan_status_change_t ws; memset(&ws, 0, sizeof ws);
    ws.wlan_status = 0x0001; ws.indication_ch = 0x0206;
    n = opc_ind_wlan_status_change_pack(frame, sizeof frame, 1, &ws);
    fp = open_memstream(&buf, &sz); fd_dump_indication(fp, OPC_IND_WLAN_STATUS_CHANGE, frame, (size_t)n); fclose(fp);
    ASSERT(line_has(buf, "wlan_status",   "0x0001"), "ind wlanstat: status @offset");
    ASSERT(line_has(buf, "indication_ch", "0x0206"), "ind wlanstat: ch @offset");
    free(buf);

    opc_ind_ap_disconnect_t ap; memset(&ap, 0, sizeof ap);
    ap.message_id = 0x000a; ap.result_code = 0x0005;
    uint8_t dm[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff}; memcpy(ap.disconnect_ap_mac, dm, 6);
    n = opc_ind_ap_disconnect_pack(frame, sizeof frame, 1, &ap);
    fp = open_memstream(&buf, &sz); fd_dump_indication(fp, OPC_IND_AP_DISCONNECT, frame, (size_t)n); fclose(fp);
    ASSERT(line_has(buf, "message_id",        "0x000a"),            "ind apdisc: msg_id @offset");
    ASSERT(line_has(buf, "disconnect_ap_mac", "aa:bb:cc:dd:ee:ff"), "ind apdisc: ap mac @offset");
    free(buf);

    opc_ind_fault_detect_t ft; memset(&ft, 0, sizeof ft);
    ft.congestion_id = 0x0001; ft.current_val = 0x0063;
    n = opc_ind_fault_detect_pack(frame, sizeof frame, 1, &ft);
    fp = open_memstream(&buf, &sz); fd_dump_indication(fp, OPC_IND_FAULT_DETECT, frame, (size_t)n); fclose(fp);
    ASSERT(line_has(buf, "congestion_id", "0x0001"), "ind fault: congestion @offset");
    ASSERT(line_has(buf, "current_val",   "0x0063"), "ind fault: val @offset");
    free(buf);

    opc_ind_reset_notice_t rst; memset(&rst, 0, sizeof rst);
    rst.reset_cause = 0x12345678;
    n = opc_ind_reset_notice_pack(frame, sizeof frame, 1, &rst);
    fp = open_memstream(&buf, &sz); fd_dump_indication(fp, OPC_IND_RESET_NOTICE, frame, (size_t)n); fclose(fp);
    ASSERT(line_has(buf, "reset_cause", "0x12345678"), "ind reset: cause @offset");
    free(buf);
}

/* Every decoded field gets a distinct value; line_has verifies each value
 * lands on its own label's line. Any offset drift anywhere in the body
 * (header through wlan2.ap_mac @356) breaks at least one assertion. */
static void test_dump_device_info_all_fields(void)
{
    opc_get_device_info_ack_t ack;
    memset(&ack, 0, sizeof ack);
    ack.result          = OPC_RESULT_OK;
    ack.vendor_code     = 0x11223344;
    ack.product_code    = 0xaabb;
    ack.product_subcode = 0xccdd;
    ack.manufacture     = (opc_date_t){.year = 2026, .month = 2, .day = 28};
    ack.shipment        = (opc_date_t){.year = 2025, .month = 3, .day = 15};
    strcpy(ack.firmware_version, "FW9.9.9");
    strcpy(ack.hardware_version, "HWxyz");
    strcpy(ack.serial_number,    "SNtest123");
    uint8_t em[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
    memcpy(ack.ethernet_mac, em, 6);
    ack.ip_address      = ipv4_host("10.1.2.3");
    ack.subnet_mask     = ipv4_host("255.255.254.0");
    ack.default_gateway = ipv4_host("10.1.2.1");
    ack.ntp_server      = ipv4_host("192.168.7.7");
    strcpy(ack.essid, "MYNET");
    ack.device_status   = OPC_DEVICE_LOGGED_IN;
    ack.station_type    = OPC_STATION_DUAL;
    ack.priority_ch     = 0x0606;
    ack.ieee_11r = 1; ack.ieee_11ai = 1; ack.ieee_11k = 1; ack.ieee_11v = 1;
    ack.wlan1.mode = 11; ack.wlan1.bandwidth = 2;
    ack.wlan1.freq_mhz = 5200; ack.wlan1.channel = 0x0224;
    ack.wlan1.status = 0x0001; ack.wlan1.snr = 21; ack.wlan1.rssi = -71;
    uint8_t w1m[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0x01};
    memcpy(ack.wlan1.mac, w1m, 6);
    uint8_t w1a[6] = {0x04,0xba,0xd6,0xec,0x0b,0x08};
    memcpy(ack.wlan1.connect_ap_mac, w1a, 6);
    ack.wlan2.mode = 9; ack.wlan2.bandwidth = 4;
    ack.wlan2.freq_mhz = 5180; ack.wlan2.channel = 0x0124;
    ack.wlan2.status = 0x0002; ack.wlan2.snr = 15; ack.wlan2.rssi = -55;
    uint8_t w2m[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0x02};
    memcpy(ack.wlan2.mac, w2m, 6);
    uint8_t w2a[6] = {0x04,0xba,0xd6,0xec,0x0b,0x09};
    memcpy(ack.wlan2.connect_ap_mac, w2a, 6);

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t n = opc_get_device_info_ack_pack(frame, sizeof frame, 1, &ack);
    ASSERT(n > 0, "allfields: pack ok");

    char *buf = NULL; size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    fd_dump_device_info(fp, frame, (size_t)n);
    fclose(fp);

    ASSERT(line_has(buf, "vendor_code",     "0x11223344"),        "allfields: vendor_code");
    ASSERT(line_has(buf, "product_code",    "0xaabb"),            "allfields: product_code");
    ASSERT(line_has(buf, "product_subcode", "0xccdd"),            "allfields: product_subcode");
    ASSERT(line_has(buf, "manufacture",     "2026-02-28"),        "allfields: manufacture");
    ASSERT(line_has(buf, "shipment",        "2025-03-15"),        "allfields: shipment");
    ASSERT(line_has(buf, "firmware_version","FW9.9.9"),           "allfields: firmware");
    ASSERT(line_has(buf, "hardware_version","HWxyz"),             "allfields: hardware");
    ASSERT(line_has(buf, "serial_number",   "SNtest123"),         "allfields: serial");
    ASSERT(line_has(buf, "ethernet_mac",    "11:22:33:44:55:66"), "allfields: ethernet_mac");
    ASSERT(line_has(buf, "ip_address",      "10.1.2.3"),          "allfields: ip_address");
    ASSERT(line_has(buf, "subnet_mask",     "255.255.254.0"),     "allfields: subnet_mask");
    ASSERT(line_has(buf, "default_gateway", "10.1.2.1"),          "allfields: default_gateway");
    ASSERT(line_has(buf, "ntp_server",      "192.168.7.7"),       "allfields: ntp_server");
    ASSERT(line_has(buf, "essid",           "'MYNET'"),           "allfields: essid");
    ASSERT(line_has(buf, "device_status",   "0x00000002"),        "allfields: device_status");
    ASSERT(line_has(buf, "priority_ch",     "0x0606"),            "allfields: priority_ch");
    ASSERT(line_has(buf, "wlan1.freq_mhz",  "5200"),              "allfields: wlan1.freq");
    ASSERT(line_has(buf, "wlan1.channel",   "0x0224"),            "allfields: wlan1.channel");
    ASSERT(line_has(buf, "wlan1.snr",       "21"),                "allfields: wlan1.snr");
    ASSERT(line_has(buf, "wlan1.rssi",      "-71"),               "allfields: wlan1.rssi");
    ASSERT(line_has(buf, "wlan1.mac",       "aa:bb:cc:dd:ee:01"), "allfields: wlan1.mac");
    ASSERT(line_has(buf, "wlan1.ap_mac",    "04:ba:d6:ec:0b:08"), "allfields: wlan1.ap_mac");
    ASSERT(line_has(buf, "wlan2.freq_mhz",  "5180"),              "allfields: wlan2.freq");
    ASSERT(line_has(buf, "wlan2.channel",   "0x0124"),            "allfields: wlan2.channel");
    ASSERT(line_has(buf, "wlan2.snr",       "15"),                "allfields: wlan2.snr");
    ASSERT(line_has(buf, "wlan2.rssi",      "-55"),               "allfields: wlan2.rssi");
    ASSERT(line_has(buf, "wlan2.mac",       "aa:bb:cc:dd:ee:02"), "allfields: wlan2.mac");
    ASSERT(line_has(buf, "wlan2.ap_mac",    "04:ba:d6:ec:0b:09"), "allfields: wlan2.ap_mac");
    free(buf);
}

int main(void)
{
    test_render_kinds();
    test_render_bounds();
    test_dump_device_info();
    test_dump_device_info_all_fields();
    test_dump_basic_info();
    test_dump_indication_keepalive();
    test_dump_indication_roaming();
    test_dump_indication_rest();

    if (failures == 0) { printf("all fielddump tests passed\n"); return 0; }
    fprintf(stderr, "%d fielddump test(s) failed\n", failures);
    return 1;
}
