/*
 * test_codec — round-trip tests for wlan-opc/protocol.
 *
 * Each test packs a fully-populated host struct, then unpacks the resulting
 * wire bytes and asserts every field round-tripped. The point is to catch
 * endian errors, offset mistakes, and missing fields cheaply on the host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"

#define ASSERT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, (msg));     \
        return 1;                                                           \
    }                                                                       \
} while (0)

/* ---- header sanity ---- */

static int test_header_round_trip(void)
{
    uint8_t buf[OPC_HEADER_SIZE];
    opc_header_t in = {
        .protocol_version  = 1,
        .command_type      = OPC_CMD_REQUEST,
        .req_indication_id = 0x1234,
        .sequence_number   = 0x5678,
        .length            = 0x09AB,
    };
    ASSERT(opc_header_pack(buf, &in) == 0, "pack");
    ASSERT(buf[2] == 0x12 && buf[3] == 0x34, "req_id big-endian");
    ASSERT(buf[4] == 0x56 && buf[5] == 0x78, "seq big-endian");
    ASSERT(buf[6] == 0x09 && buf[7] == 0xAB, "len big-endian");
    for (size_t i = 8; i < OPC_HEADER_SIZE; ++i) {
        ASSERT(buf[i] == 0, "reserve zero");
    }
    opc_header_t out;
    ASSERT(opc_header_unpack(buf, sizeof buf, &out) == 0, "unpack");
    ASSERT(out.protocol_version == in.protocol_version, "ver");
    ASSERT(out.command_type     == in.command_type,     "type");
    ASSERT(out.length           == in.length,           "len");
    return 0;
}

/* ---- helpers used by the bigger tests ---- */

static void fill_mac(uint8_t mac[6], uint8_t seed)
{
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(seed + i);
}

static int macs_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

/* ---- 0xF001 Login ---- */

static int test_login(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_login_req_t in; memset(&in, 0, sizeof in);
    strcpy(in.password, "MyPassword");
    ssize_t n = opc_login_req_pack(frame, sizeof frame, 0x0011, &in);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_LOGIN_REQ_LENGTH, "req length");
    opc_login_req_t out;
    ASSERT(opc_login_req_unpack(frame, n, &out) == 0, "req unpack");
    ASSERT(strcmp(in.password, out.password) == 0, "password");

    opc_login_ack_t ai = { .result = OPC_RESULT_NG, .error_cause = OPC_ERR_LOGIN_CONDITION };
    n = opc_login_ack_pack(frame, sizeof frame, 0x0011, &ai);
    ASSERT(n > 0, "ack pack");
    opc_login_ack_t ao;
    ASSERT(opc_login_ack_unpack(frame, n, &ao) == 0, "ack unpack");
    ASSERT(ao.result == ai.result && ao.error_cause == ai.error_cause, "ack fields");
    return 0;
}

/* ---- 0xF002 Logout ---- */

static int test_logout(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t nq = opc_logout_req_pack(frame, sizeof frame, 0x0012);
    ASSERT(nq == OPC_HEADER_SIZE, "req size");
    ASSERT(opc_be16_read(&frame[6]) == OPC_LOGOUT_REQ_LENGTH, "req length");
    ASSERT(opc_logout_req_unpack(frame, nq) == 0, "req unpack");

    opc_logout_ack_t ai = { .result = OPC_RESULT_OK, .error_cause = OPC_ERR_NONE };
    ssize_t na = opc_logout_ack_pack(frame, sizeof frame, 0x0012, &ai);
    ASSERT(na > 0, "ack pack");
    opc_logout_ack_t ao;
    ASSERT(opc_logout_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.result == ai.result, "ack result");
    return 0;
}

/* ---- 0x0001 GetBasicInfo ---- */

static int test_get_basic_info(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t nq = opc_get_basic_info_req_pack(frame, sizeof frame, 0x0001);
    ASSERT(nq == OPC_HEADER_SIZE, "req size");
    ASSERT(opc_get_basic_info_req_unpack(frame, nq) == 0, "req unpack");

    opc_get_basic_info_ack_t ai = {
        .vendor_code     = 0x00902CFB,
        .product_code    = 0xFE03,
        .product_subcode = 0x0001,
        .device_status   = OPC_DEVICE_READY,
        .station_type    = OPC_STATION_DUAL,
    };
    ssize_t na = opc_get_basic_info_ack_pack(frame, sizeof frame, 0x0001, &ai);
    ASSERT(na > 0, "ack pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_GET_BASIC_INFO_ACK_LENGTH, "ack length");
    opc_get_basic_info_ack_t ao;
    ASSERT(opc_get_basic_info_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.vendor_code     == ai.vendor_code,     "vendor");
    ASSERT(ao.product_code    == ai.product_code,    "product");
    ASSERT(ao.product_subcode == ai.product_subcode, "subcode");
    ASSERT(ao.device_status   == ai.device_status,   "status");
    ASSERT(ao.station_type    == ai.station_type,    "station");
    return 0;
}

/* ---- 0x0002 GetDeviceInfo ---- */

static int test_get_device_info(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t nq = opc_get_device_info_req_pack(frame, sizeof frame, 0x0002);
    ASSERT(nq == OPC_HEADER_SIZE, "req size");
    ASSERT(opc_get_device_info_req_unpack(frame, nq) == 0, "req unpack");

    opc_get_device_info_ack_t ai;
    memset(&ai, 0, sizeof ai);
    ai.result          = OPC_RESULT_OK;
    ai.error_cause     = OPC_ERR_NONE;
    ai.vendor_code     = 0x00902CFB;
    ai.product_code    = 0xFE03;
    ai.product_subcode = 0x0042;
    ai.manufacture     = (opc_date_t){ .year = 2026, .month = 2, .day = 28 };
    ai.shipment        = (opc_date_t){ .year = 2026, .month = 3, .day = 15 };
    strcpy(ai.firmware_version, "FW-1.2.3");
    strcpy(ai.hardware_version, "HW-rev-A");
    strcpy(ai.serial_number,    "SN-0001-XYZ");
    fill_mac(ai.ethernet_mac, 0x10);
    ai.ip_address      = 0xC0A80101;     /* 192.168.1.1 */
    ai.subnet_mask     = 0xFFFFFF00;
    ai.default_gateway = 0xC0A801FE;
    ai.ntp_server      = 0xC0A80102;
    strcpy(ai.essid, "Cantops_WL");
    ai.device_status   = OPC_DEVICE_LOGGED_IN;
    ai.station_type    = OPC_STATION_DUAL;
    ai.priority_ch     = 0x0206;         /* 5GHz, ch 6 */
    ai.ieee_11r        = 1;
    ai.ieee_11ai       = 0;
    ai.ieee_11k        = 1;
    ai.ieee_11v        = 1;

    fill_mac(ai.wlan1.mac, 0x20);
    ai.wlan1.mode      = OPC_WLAN_MODE_11AX;
    ai.wlan1.bandwidth = OPC_BANDWIDTH_80;
    ai.wlan1.freq_mhz  = 5180;
    ai.wlan1.channel   = 0x0224;  /* 5GHz, ch 36 */
    ai.wlan1.status    = 0x0001;
    ai.wlan1.snr       = 35;
    ai.wlan1.rssi      = -60;
    fill_mac(ai.wlan1.connect_ap_mac, 0x30);

    fill_mac(ai.wlan2.mac, 0x40);
    ai.wlan2.mode      = OPC_WLAN_MODE_11AC;
    ai.wlan2.bandwidth = OPC_BANDWIDTH_40;
    ai.wlan2.freq_mhz  = 2412;
    ai.wlan2.channel   = 0x0101;
    ai.wlan2.status    = 0x0001;
    ai.wlan2.snr       = 28;
    ai.wlan2.rssi      = -72;
    fill_mac(ai.wlan2.connect_ap_mac, 0x50);

    ssize_t na = opc_get_device_info_ack_pack(frame, sizeof frame, 0x0002, &ai);
    ASSERT(na > 0, "ack pack");
    ASSERT((size_t)na == OPC_HEADER_SIZE + OPC_GET_DEVICE_INFO_ACK_BODY_LEN, "ack size");
    ASSERT(opc_be16_read(&frame[6]) == OPC_GET_DEVICE_INFO_ACK_LENGTH, "ack length");

    opc_get_device_info_ack_t ao;
    ASSERT(opc_get_device_info_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.result == ai.result, "result");
    ASSERT(ao.vendor_code == ai.vendor_code, "vendor");
    ASSERT(ao.product_subcode == ai.product_subcode, "subcode");
    ASSERT(ao.manufacture.year == 2026 && ao.manufacture.month == 2 && ao.manufacture.day == 28, "mfg date");
    ASSERT(ao.shipment.day == 15, "shipment day");
    ASSERT(strcmp(ao.firmware_version, ai.firmware_version) == 0, "fw");
    ASSERT(strcmp(ao.hardware_version, ai.hardware_version) == 0, "hw");
    ASSERT(strcmp(ao.serial_number,    ai.serial_number)    == 0, "sn");
    ASSERT(macs_equal(ao.ethernet_mac, ai.ethernet_mac), "eth mac");
    ASSERT(ao.ip_address == ai.ip_address, "ip");
    ASSERT(ao.subnet_mask == ai.subnet_mask, "mask");
    ASSERT(ao.default_gateway == ai.default_gateway, "gw");
    ASSERT(ao.ntp_server == ai.ntp_server, "ntp");
    ASSERT(strcmp(ao.essid, ai.essid) == 0, "essid");
    ASSERT(ao.device_status == ai.device_status, "device status");
    ASSERT(ao.station_type  == ai.station_type, "station");
    ASSERT(ao.priority_ch   == ai.priority_ch, "priority ch");
    ASSERT(ao.ieee_11r == 1 && ao.ieee_11ai == 0 && ao.ieee_11k == 1 && ao.ieee_11v == 1, "11rakv");

    ASSERT(macs_equal(ao.wlan1.mac, ai.wlan1.mac), "w1 mac");
    ASSERT(ao.wlan1.mode == ai.wlan1.mode, "w1 mode");
    ASSERT(ao.wlan1.bandwidth == ai.wlan1.bandwidth, "w1 bw");
    ASSERT(ao.wlan1.freq_mhz == ai.wlan1.freq_mhz, "w1 freq");
    ASSERT(ao.wlan1.channel == ai.wlan1.channel, "w1 ch");
    ASSERT(ao.wlan1.status == ai.wlan1.status, "w1 stat");
    ASSERT(ao.wlan1.snr == ai.wlan1.snr, "w1 snr");
    ASSERT(ao.wlan1.rssi == ai.wlan1.rssi, "w1 rssi");
    ASSERT(macs_equal(ao.wlan1.connect_ap_mac, ai.wlan1.connect_ap_mac), "w1 ap mac");

    ASSERT(macs_equal(ao.wlan2.mac, ai.wlan2.mac), "w2 mac");
    ASSERT(ao.wlan2.freq_mhz == ai.wlan2.freq_mhz, "w2 freq");
    ASSERT(ao.wlan2.channel == ai.wlan2.channel, "w2 ch");
    ASSERT(ao.wlan2.rssi == ai.wlan2.rssi, "w2 rssi");
    ASSERT(macs_equal(ao.wlan2.connect_ap_mac, ai.wlan2.connect_ap_mac), "w2 ap mac");
    return 0;
}

/* ---- 0x1001 SetPassword ---- */

static int test_set_password(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_set_password_req_t ri; memset(&ri, 0, sizeof ri);
    strcpy(ri.old_password, "MyPassword_old");
    strcpy(ri.new_password, "MyPassword_new");
    ssize_t n = opc_set_password_req_pack(frame, sizeof frame, 0x0010, &ri);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_SET_PASSWORD_REQ_LENGTH, "req length");
    opc_set_password_req_t ro;
    ASSERT(opc_set_password_req_unpack(frame, n, &ro) == 0, "req unpack");
    ASSERT(strcmp(ro.old_password, ri.old_password) == 0, "old");
    ASSERT(strcmp(ro.new_password, ri.new_password) == 0, "new");

    opc_set_password_ack_t ai = { .result = OPC_RESULT_OK };
    ssize_t na = opc_set_password_ack_pack(frame, sizeof frame, 0x0010, &ai);
    ASSERT(na > 0, "ack pack");
    opc_set_password_ack_t ao;
    ASSERT(opc_set_password_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.result == OPC_RESULT_OK, "ack result");
    return 0;
}

/* ---- 0x1002 SetIpConfigList ---- */

static int test_set_ip_config_list(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_set_ip_config_list_req_t ri; memset(&ri, 0, sizeof ri);
    ri.entry_count = 3;
    for (size_t i = 0; i < ri.entry_count; i++) {
        ri.entries[i].boundary_flag   = (i == 0) ? OPC_LIST_BOUNDARY_START
                                       : (i == ri.entry_count - 1) ? OPC_LIST_BOUNDARY_END
                                       : OPC_LIST_BOUNDARY_CONTINUE;
        ri.entries[i].list_number      = (uint16_t)(i + 1);
        ri.entries[i].ip_address       = 0xAC110000 | (uint32_t)i;
        ri.entries[i].subnet_mask      = 0xFFFFC000;
        ri.entries[i].default_gateway  = 0xAC1141FE;
        ri.entries[i].ntp_server       = 0xAC110201;
        snprintf(ri.entries[i].essid, sizeof ri.entries[i].essid, "ESSID_%zu", i);
    }
    ssize_t n = opc_set_ip_config_list_req_pack(frame, sizeof frame, 0x0020, &ri);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6])
           == OPC_SET_IP_CONFIG_LIST_REQ_LENGTH(ri.entry_count), "req length");
    opc_set_ip_config_list_req_t ro;
    ASSERT(opc_set_ip_config_list_req_unpack(frame, n, &ro) == 0, "req unpack");
    ASSERT(ro.entry_count == ri.entry_count, "entry count");
    for (size_t i = 0; i < ri.entry_count; i++) {
        ASSERT(ro.entries[i].boundary_flag   == ri.entries[i].boundary_flag,   "bf");
        ASSERT(ro.entries[i].list_number      == ri.entries[i].list_number,    "ln");
        ASSERT(ro.entries[i].ip_address       == ri.entries[i].ip_address,     "ip");
        ASSERT(ro.entries[i].subnet_mask      == ri.entries[i].subnet_mask,    "mask");
        ASSERT(ro.entries[i].default_gateway  == ri.entries[i].default_gateway,"gw");
        ASSERT(ro.entries[i].ntp_server       == ri.entries[i].ntp_server,     "ntp");
        ASSERT(strcmp(ro.entries[i].essid, ri.entries[i].essid) == 0,          "essid");
    }

    opc_set_ip_config_list_ack_t ai = { .result = OPC_RESULT_OK };
    ssize_t na = opc_set_ip_config_list_ack_pack(frame, sizeof frame, 0x0020, &ai);
    ASSERT(na > 0, "ack pack");
    opc_set_ip_config_list_ack_t ao;
    ASSERT(opc_set_ip_config_list_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    return 0;
}

/* ---- 0x1003 ChangeIpAddress ---- */

static int test_change_ip_address(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_change_ip_address_req_t ri = { .list_number = 25 };
    ssize_t n = opc_change_ip_address_req_pack(frame, sizeof frame, 0x0030, &ri);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_CHANGE_IP_ADDRESS_REQ_LENGTH, "req length");
    opc_change_ip_address_req_t ro;
    ASSERT(opc_change_ip_address_req_unpack(frame, n, &ro) == 0, "req unpack");
    ASSERT(ro.list_number == ri.list_number, "list number");

    opc_change_ip_address_ack_t ai = { .result = OPC_RESULT_NG, .error_cause = 0x0012 };
    ssize_t na = opc_change_ip_address_ack_pack(frame, sizeof frame, 0x0030, &ai);
    ASSERT(na > 0, "ack pack");
    opc_change_ip_address_ack_t ao;
    ASSERT(opc_change_ip_address_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.error_cause == 0x0012, "conflict cause");
    return 0;
}

/* ---- 0x1004 SetRadioConfig ---- */

static int test_set_radio_config(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_set_radio_config_req_t ri = {
        .station_type = OPC_STATION_DUAL,
        .priority_ch  = 0x0206,
        .wlan1 = { .freq_mhz = 5180, .channel = 0x0224,
                   .mode = OPC_WLAN_MODE_11AX, .bandwidth = OPC_BANDWIDTH_80 },
        .wlan2 = { .freq_mhz = 2412, .channel = 0x0101,
                   .mode = OPC_WLAN_MODE_11N,  .bandwidth = OPC_BANDWIDTH_40 },
    };
    ssize_t n = opc_set_radio_config_req_pack(frame, sizeof frame, 0x0040, &ri);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_SET_RADIO_CONFIG_REQ_LENGTH, "req length");
    opc_set_radio_config_req_t ro;
    ASSERT(opc_set_radio_config_req_unpack(frame, n, &ro) == 0, "req unpack");
    ASSERT(ro.station_type == ri.station_type, "station");
    ASSERT(ro.priority_ch == ri.priority_ch, "priority");
    ASSERT(ro.wlan1.freq_mhz == ri.wlan1.freq_mhz, "w1 freq");
    ASSERT(ro.wlan1.channel  == ri.wlan1.channel,  "w1 ch");
    ASSERT(ro.wlan1.mode     == ri.wlan1.mode,     "w1 mode");
    ASSERT(ro.wlan1.bandwidth == ri.wlan1.bandwidth, "w1 bw");
    ASSERT(ro.wlan2.freq_mhz == ri.wlan2.freq_mhz, "w2 freq");
    ASSERT(ro.wlan2.channel  == ri.wlan2.channel,  "w2 ch");
    ASSERT(ro.wlan2.mode     == ri.wlan2.mode,     "w2 mode");

    opc_set_radio_config_ack_t ai = { .result = OPC_RESULT_OK };
    ssize_t na = opc_set_radio_config_ack_pack(frame, sizeof frame, 0x0040, &ai);
    ASSERT(na > 0, "ack pack");
    opc_set_radio_config_ack_t ao;
    ASSERT(opc_set_radio_config_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    return 0;
}

/* ---- 0x1005 SetIndicationConfig ---- */

static int test_set_indication_config(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_set_indication_config_req_t ri = {
        .recipient_port  = 9999,
        .info_bits       = OPC_IND_BIT_INIT_COMPLETE
                         | OPC_IND_BIT_ROAMING
                         | OPC_IND_BIT_KEEP_ALIVE,
        .period_seconds  = 5,
        .recipient_ip    = 0xC0A8010A,
    };
    ssize_t n = opc_set_indication_config_req_pack(frame, sizeof frame, 0x0050, &ri);
    ASSERT(n > 0, "req pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_SET_INDICATION_CONFIG_REQ_LENGTH, "req length");
    opc_set_indication_config_req_t ro;
    ASSERT(opc_set_indication_config_req_unpack(frame, n, &ro) == 0, "req unpack");
    ASSERT(ro.recipient_port == 9999, "port");
    ASSERT(ro.info_bits == ri.info_bits, "bits");
    ASSERT(ro.period_seconds == 5, "period");
    ASSERT(ro.recipient_ip == ri.recipient_ip, "ip");

    opc_set_indication_config_ack_t ai = { .result = OPC_RESULT_OK };
    ssize_t na = opc_set_indication_config_ack_pack(frame, sizeof frame, 0x0050, &ai);
    ASSERT(na > 0, "ack pack");
    opc_set_indication_config_ack_t ao;
    ASSERT(opc_set_indication_config_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    return 0;
}

/* ---- 0x2001 Reset ---- */

static int test_reset(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t nq = opc_reset_req_pack(frame, sizeof frame, 0x0060);
    ASSERT(nq == OPC_HEADER_SIZE, "req size");
    ASSERT(opc_be16_read(&frame[6]) == OPC_RESET_REQ_LENGTH, "req length");
    ASSERT(opc_reset_req_unpack(frame, nq) == 0, "req unpack");

    opc_reset_ack_t ai = { .result = OPC_RESULT_OK };
    ssize_t na = opc_reset_ack_pack(frame, sizeof frame, 0x0060, &ai);
    ASSERT(na > 0, "ack pack");
    /* Spec quirk T11: Length field is 0 even though body is present. */
    ASSERT(opc_be16_read(&frame[6]) == OPC_RESET_ACK_LENGTH, "ack length is 0 per spec");
    opc_reset_ack_t ao;
    ASSERT(opc_reset_ack_unpack(frame, na, &ao) == 0, "ack unpack");
    ASSERT(ao.result == OPC_RESULT_OK, "ack result");
    return 0;
}

/* ---- bounds / rejection ---- */

static int test_rejects(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_login_req_t in; memset(&in, 0, sizeof in); strcpy(in.password, "x");
    ASSERT(opc_login_req_pack(frame, 16, 0, &in) < 0, "small cap");

    ssize_t n = opc_logout_req_pack(frame, sizeof frame, 0);
    opc_login_req_t lreq;
    ASSERT(opc_login_req_unpack(frame, n, &lreq) < 0, "wrong id");

    /* SetIpConfigList rejects entry_count == 0 and > 20 */
    opc_set_ip_config_list_req_t bad; memset(&bad, 0, sizeof bad);
    bad.entry_count = 0;
    ASSERT(opc_set_ip_config_list_req_pack(frame, sizeof frame, 0, &bad) < 0, "0 entries");
    bad.entry_count = OPC_IPCFG_LIST_MAX_PER_REQ + 1;
    ASSERT(opc_set_ip_config_list_req_pack(frame, sizeof frame, 0, &bad) < 0, "21 entries");
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "header_round_trip",      test_header_round_trip },
        { "login",                  test_login },
        { "logout",                 test_logout },
        { "get_basic_info",         test_get_basic_info },
        { "get_device_info",        test_get_device_info },
        { "set_password",           test_set_password },
        { "set_ip_config_list",     test_set_ip_config_list },
        { "change_ip_address",      test_change_ip_address },
        { "set_radio_config",       test_set_radio_config },
        { "set_indication_config",  test_set_indication_config },
        { "reset",                  test_reset },
        { "rejects",                test_rejects },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (cases[i].fn() == 0) printf("PASS %s\n", cases[i].name);
        else { printf("FAIL %s\n", cases[i].name); fail++; }
    }
    if (fail) { printf("%d test(s) failed\n", fail); return 1; }
    printf("all %zu test(s) passed\n", sizeof cases / sizeof cases[0]);
    return 0;
}
