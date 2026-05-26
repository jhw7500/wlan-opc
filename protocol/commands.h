#ifndef WLAN_OPC_COMMANDS_H
#define WLAN_OPC_COMMANDS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "codec.h"
#include "frame.h"
#include "ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Common Acknowledgment shape (Login/Logout/SetPassword/SetIpConfigList/
 * ChangeIpAddress/SetRadioConfig/SetIndicationConfig/Reset Ack all share
 * this 4-byte body: Result(2) + ErrorCause(2)).
 * ======================================================================== */

#define OPC_SIMPLE_ACK_BODY_LEN   4
#define OPC_SIMPLE_ACK_LENGTH     60   /* spec literal for most acks */

ssize_t opc_simple_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t req_id, uint16_t seq_no,
                            uint16_t length_field,
                            uint16_t result, uint16_t error_cause);

int opc_simple_ack_unpack(const uint8_t *frame, size_t frame_len,
                          uint16_t expected_req_id,
                          uint16_t *result_out, uint16_t *error_cause_out);

/* ========================================================================
 * 0xF001 — Login
 * ======================================================================== */

#define OPC_LOGIN_REQ_BODY_LEN    128
#define OPC_LOGIN_REQ_LENGTH      184  /* spec */
#define OPC_LOGIN_PASSWORD_MAX    127  /* + NULL */

typedef struct opc_login_req {
    /* NULL-terminated, up to 127 chars. */
    char password[OPC_LOGIN_REQ_BODY_LEN];
} opc_login_req_t;

typedef struct opc_login_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_login_ack_t;

ssize_t opc_login_req_pack(uint8_t *frame, size_t cap,
                           uint16_t seq_no, const opc_login_req_t *in);
int     opc_login_req_unpack(const uint8_t *frame, size_t frame_len,
                             opc_login_req_t *out);
ssize_t opc_login_ack_pack(uint8_t *frame, size_t cap,
                           uint16_t seq_no, const opc_login_ack_t *in);
int     opc_login_ack_unpack(const uint8_t *frame, size_t frame_len,
                             opc_login_ack_t *out);

/* ========================================================================
 * 0xF002 — Logout (empty body Req, simple-ack Ack)
 * ======================================================================== */

#define OPC_LOGOUT_REQ_BODY_LEN   0
#define OPC_LOGOUT_REQ_LENGTH     0    /* spec */

typedef struct opc_logout_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_logout_ack_t;

ssize_t opc_logout_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_logout_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_logout_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t seq_no, const opc_logout_ack_t *in);
int     opc_logout_ack_unpack(const uint8_t *frame, size_t frame_len,
                              opc_logout_ack_t *out);

/* ========================================================================
 * 0x0001 — Get Basic Information (empty body Req)
 * ======================================================================== */

#define OPC_GET_BASIC_INFO_REQ_BODY_LEN   0
#define OPC_GET_BASIC_INFO_REQ_LENGTH     0   /* spec */
#define OPC_GET_BASIC_INFO_ACK_BODY_LEN   16
#define OPC_GET_BASIC_INFO_ACK_LENGTH     72  /* spec */

typedef struct opc_get_basic_info_ack {
    uint32_t vendor_code;       /* IEEE OUI 3B in low bits, e.g. 0x00902CFB */
    uint16_t product_code;
    uint16_t product_subcode;
    uint32_t device_status;     /* OPC_DEVICE_* */
    uint16_t station_type;      /* OPC_STATION_SINGLE / DUAL */
} opc_get_basic_info_ack_t;

ssize_t opc_get_basic_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_get_basic_info_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_get_basic_info_ack_pack(uint8_t *frame, size_t cap,
                                    uint16_t seq_no,
                                    const opc_get_basic_info_ack_t *in);
int     opc_get_basic_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                      opc_get_basic_info_ack_t *out);

/* ========================================================================
 * 0x0002 — Get Device Information (Req side only in this commit;
 *           Ack side lands in the next Phase 1 commit.)
 * ======================================================================== */

#define OPC_GET_DEVICE_INFO_REQ_BODY_LEN  0
#define OPC_GET_DEVICE_INFO_REQ_LENGTH    0   /* spec */

ssize_t opc_get_device_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_get_device_info_req_unpack(const uint8_t *frame, size_t frame_len);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_COMMANDS_H */
