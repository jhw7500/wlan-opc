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

/* ---- common header sanity ---- */

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

    /* big-endian sanity: bytes 2-3 are req_id MSB,LSB */
    ASSERT(buf[2] == 0x12 && buf[3] == 0x34, "req_id big-endian");
    ASSERT(buf[4] == 0x56 && buf[5] == 0x78, "seq big-endian");
    ASSERT(buf[6] == 0x09 && buf[7] == 0xAB, "len big-endian");

    /* reserve bytes 8..63 should all be zero */
    for (size_t i = 8; i < OPC_HEADER_SIZE; ++i) {
        ASSERT(buf[i] == 0, "reserve zero");
    }

    opc_header_t out;
    ASSERT(opc_header_unpack(buf, sizeof buf, &out) == 0, "unpack");
    ASSERT(out.protocol_version  == in.protocol_version,  "ver");
    ASSERT(out.command_type      == in.command_type,      "type");
    ASSERT(out.req_indication_id == in.req_indication_id, "rid");
    ASSERT(out.sequence_number   == in.sequence_number,   "seq");
    ASSERT(out.length            == in.length,            "len");
    return 0;
}

/* ---- 0xF001 Login ---- */

static int test_login_req(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_login_req_t in;
    memset(&in, 0, sizeof in);
    strcpy(in.password, "MyPassword");

    ssize_t n = opc_login_req_pack(frame, sizeof frame, 0x0011, &in);
    ASSERT(n > 0, "pack");
    ASSERT((size_t)n == OPC_HEADER_SIZE + OPC_LOGIN_REQ_BODY_LEN, "size");

    /* header length field == spec literal */
    ASSERT(opc_be16_read(&frame[6]) == OPC_LOGIN_REQ_LENGTH, "length field");
    /* command type & id */
    ASSERT(frame[1] == OPC_CMD_REQUEST, "cmd type");
    ASSERT(opc_be16_read(&frame[2]) == OPC_REQ_LOGIN, "req id");

    opc_login_req_t out;
    ASSERT(opc_login_req_unpack(frame, n, &out) == 0, "unpack");
    ASSERT(strcmp(in.password, out.password) == 0, "password round-trip");
    return 0;
}

static int test_login_ack(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_login_ack_t in = {
        .result = OPC_RESULT_NG,
        .error_cause = OPC_ERR_LOGIN_CONDITION,
    };
    ssize_t n = opc_login_ack_pack(frame, sizeof frame, 0x0011, &in);
    ASSERT(n > 0, "pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_SIMPLE_ACK_LENGTH, "length field");
    ASSERT(frame[1] == OPC_CMD_ACK, "cmd type");

    opc_login_ack_t out;
    ASSERT(opc_login_ack_unpack(frame, n, &out) == 0, "unpack");
    ASSERT(out.result == in.result, "result");
    ASSERT(out.error_cause == in.error_cause, "error cause");
    return 0;
}

/* ---- 0xF002 Logout ---- */

static int test_logout_round_trip(void)
{
    uint8_t frame[OPC_FRAME_MAX];

    /* req — empty body */
    ssize_t nq = opc_logout_req_pack(frame, sizeof frame, 0x0012);
    ASSERT(nq == OPC_HEADER_SIZE, "req size = header only");
    ASSERT(opc_be16_read(&frame[6]) == OPC_LOGOUT_REQ_LENGTH, "req length field");
    ASSERT(opc_logout_req_unpack(frame, nq) == 0, "req unpack");

    /* ack */
    opc_logout_ack_t in = { .result = OPC_RESULT_OK, .error_cause = OPC_ERR_NONE };
    ssize_t na = opc_logout_ack_pack(frame, sizeof frame, 0x0012, &in);
    ASSERT(na > 0, "ack pack");
    opc_logout_ack_t out;
    ASSERT(opc_logout_ack_unpack(frame, na, &out) == 0, "ack unpack");
    ASSERT(out.result == OPC_RESULT_OK, "result");
    ASSERT(out.error_cause == OPC_ERR_NONE, "error");
    return 0;
}

/* ---- 0x0001 GetBasicInformation ---- */

static int test_get_basic_info_round_trip(void)
{
    uint8_t frame[OPC_FRAME_MAX];

    /* req */
    ssize_t nq = opc_get_basic_info_req_pack(frame, sizeof frame, 0x0001);
    ASSERT(nq == OPC_HEADER_SIZE, "req empty body");
    ASSERT(opc_get_basic_info_req_unpack(frame, nq) == 0, "req unpack");

    /* ack */
    opc_get_basic_info_ack_t in = {
        .vendor_code     = 0x00902CFB, /* CANTOPS */
        .product_code    = 0xFE03,
        .product_subcode = 0x0001,
        .device_status   = OPC_DEVICE_READY,
        .station_type    = OPC_STATION_DUAL,
    };
    ssize_t na = opc_get_basic_info_ack_pack(frame, sizeof frame, 0x0001, &in);
    ASSERT(na > 0, "ack pack");
    ASSERT(opc_be16_read(&frame[6]) == OPC_GET_BASIC_INFO_ACK_LENGTH, "ack length");

    opc_get_basic_info_ack_t out;
    ASSERT(opc_get_basic_info_ack_unpack(frame, na, &out) == 0, "ack unpack");
    ASSERT(out.vendor_code     == in.vendor_code,     "vendor");
    ASSERT(out.product_code    == in.product_code,    "product");
    ASSERT(out.product_subcode == in.product_subcode, "subcode");
    ASSERT(out.device_status   == in.device_status,   "status");
    ASSERT(out.station_type    == in.station_type,    "station");
    return 0;
}

/* ---- 0x0002 GetDeviceInformation (Req only this commit) ---- */

static int test_get_device_info_req(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t nq = opc_get_device_info_req_pack(frame, sizeof frame, 0x0002);
    ASSERT(nq == OPC_HEADER_SIZE, "req empty body");
    ASSERT(opc_be16_read(&frame[2]) == OPC_REQ_GET_DEVICE_INFO, "req id");
    ASSERT(opc_get_device_info_req_unpack(frame, nq) == 0, "req unpack");
    return 0;
}

/* ---- bounds / rejection ---- */

static int test_rejects(void)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_login_req_t in;
    memset(&in, 0, sizeof in);
    strcpy(in.password, "x");

    /* capacity too small */
    ASSERT(opc_login_req_pack(frame, 16, 0, &in) < 0, "small cap");

    /* wrong req_id rejected on unpack */
    ssize_t n = opc_logout_req_pack(frame, sizeof frame, 0);
    ASSERT(n == OPC_HEADER_SIZE, "logout pack");
    opc_login_req_t lreq;
    ASSERT(opc_login_req_unpack(frame, n, &lreq) < 0, "wrong id rejected");
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "header_round_trip",          test_header_round_trip },
        { "login_req",                  test_login_req },
        { "login_ack",                  test_login_ack },
        { "logout_round_trip",          test_logout_round_trip },
        { "get_basic_info_round_trip",  test_get_basic_info_round_trip },
        { "get_device_info_req",        test_get_device_info_req },
        { "rejects",                    test_rejects },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int rc = cases[i].fn();
        if (rc == 0) {
            printf("PASS %s\n", cases[i].name);
        } else {
            printf("FAIL %s\n", cases[i].name);
            fail++;
        }
    }
    if (fail) {
        printf("%d test(s) failed\n", fail);
        return 1;
    }
    printf("all %zu test(s) passed\n", sizeof cases / sizeof cases[0]);
    return 0;
}
