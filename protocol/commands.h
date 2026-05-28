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
 * Common Acknowledgment shape (Result + ErrorCause body, 4 bytes).
 * Used by Login/Logout/SetPassword/SetIpConfigList/ChangeIpAddress/
 * SetRadioConfig/SetIndicationConfig/Reset Acks.
 * The header `length_field` differs per command — Reset Ack uses 0 (T11),
 * everyone else uses 60.
 * ======================================================================== */

#define OPC_SIMPLE_ACK_BODY_LEN   4
#define OPC_SIMPLE_ACK_LENGTH     60   /* spec literal for non-Reset acks */

ssize_t opc_simple_ack_pack(uint8_t *frame, size_t cap,
                            uint16_t req_id, uint16_t seq_no,
                            uint16_t length_field,
                            uint16_t result, uint16_t error_cause);

int opc_simple_ack_unpack(const uint8_t *frame, size_t frame_len,
                          uint16_t expected_req_id,
                          uint16_t *result_out, uint16_t *error_cause_out);

/* ========================================================================
 * Shared sub-records used by multiple commands.
 * ======================================================================== */

typedef struct opc_date {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
} opc_date_t;

/* Per-WLAN radio block as seen in GetDeviceInfo Ack (full + measured state). */
typedef struct opc_wlan_radio_state {
    uint8_t  mac[6];
    uint8_t  mode;          /* OPC_WLAN_MODE_* */
    uint8_t  bandwidth;     /* OPC_BANDWIDTH_* */
    uint16_t freq_mhz;
    uint16_t channel;       /* upper byte band, lower byte CH number */
    uint16_t status;        /* 0x0000 SCAN / 0x0001 connected */
    int8_t   snr;
    int8_t   rssi;
    uint8_t  connect_ap_mac[6];
} opc_wlan_radio_state_t;

/* Per-WLAN radio block as set in SetRadioConfig Req (config-only). */
typedef struct opc_wlan_radio_cfg {
    uint16_t freq_mhz;
    uint16_t channel;       /* upper byte band, lower byte CH number */
    uint8_t  mode;
    uint8_t  bandwidth;
} opc_wlan_radio_cfg_t;

/* ========================================================================
 * 0xF001 — Login
 * ======================================================================== */

#define OPC_LOGIN_REQ_BODY_LEN    128
#define OPC_LOGIN_REQ_LENGTH      184  /* spec */
#define OPC_LOGIN_PASSWORD_MAX    127  /* + NULL */

typedef struct opc_login_req {
    char password[OPC_LOGIN_REQ_BODY_LEN];
} opc_login_req_t;

typedef struct opc_login_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_login_ack_t;

ssize_t opc_login_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_login_req_t *in);
int     opc_login_req_unpack(const uint8_t *frame, size_t frame_len, opc_login_req_t *out);
ssize_t opc_login_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_login_ack_t *in);
int     opc_login_ack_unpack(const uint8_t *frame, size_t frame_len, opc_login_ack_t *out);

/* ========================================================================
 * 0xF002 — Logout (empty Req)
 * ======================================================================== */

#define OPC_LOGOUT_REQ_BODY_LEN   0
#define OPC_LOGOUT_REQ_LENGTH     0    /* spec */

typedef struct opc_logout_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_logout_ack_t;

ssize_t opc_logout_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_logout_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_logout_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_logout_ack_t *in);
int     opc_logout_ack_unpack(const uint8_t *frame, size_t frame_len, opc_logout_ack_t *out);

/* ========================================================================
 * 0x0001 — GetBasicInformation
 * ======================================================================== */

#define OPC_GET_BASIC_INFO_REQ_BODY_LEN   0
#define OPC_GET_BASIC_INFO_REQ_LENGTH     0
#define OPC_GET_BASIC_INFO_ACK_BODY_LEN   16
#define OPC_GET_BASIC_INFO_ACK_LENGTH     72

typedef struct opc_get_basic_info_ack {
    uint32_t vendor_code;
    uint16_t product_code;
    uint16_t product_subcode;
    uint32_t device_status;
    uint16_t station_type;
} opc_get_basic_info_ack_t;

ssize_t opc_get_basic_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_get_basic_info_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_get_basic_info_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                    const opc_get_basic_info_ack_t *in);
int     opc_get_basic_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                      opc_get_basic_info_ack_t *out);

/* ========================================================================
 * 0x0002 — GetDeviceInformation
 *
 * Largest response payload in the spec. Body layout (offsets are body-relative,
 * i.e. add 64 to get the in-frame absolute offset):
 *   0   Result(2) + ErrorCause(2)
 *   4   VendorCode(4)
 *   8   ProductCode(2) + ProductSubcode(2)
 *  12   Date of Manufacture (year:2, month:1, day:1)
 *  16   Date of Shipment    (year:2, month:1, day:1)
 *  20   Firmware Version (32B, NULL-terminated)
 *  52   Hardware Version (32B, NULL-terminated)
 *  84   Serial number    (32B, NULL-terminated)
 * 116   Ethernet MAC (6B) + reserve(2)
 * 124   IP Address(4)
 * 128   Subnet Mask(4)
 * 132   Default Gateway(4)
 * 136   NTP Server IP(4)
 * 140   ESSID(32, NULL-terminated)
 * 172   Device Status(4)
 * 176   Station Type(2) + Priority CH(2)
 * 180   IEEE 802.11r(1) + 11ai(1) + 11k(1) + 11v(1)
 * 184   reserve(36)            ── spec table boundary inconsistent (see T12 in docs/proto-todo.md)
 * 220   WLAN#1 MAC(6) + Mode(1) + BW(1)
 * 228   WLAN#1 FREQ(2) + CH(2)
 * 232   WLAN#1 Status(2) + SNR(1) + RSSI(1)
 * 236   WLAN#1 Connect AP MAC(6) + pad(2)
 * 244   reserve(36)
 * 280   WLAN#2 MAC(6) + Mode(1) + BW(1)
 * 288   WLAN#2 FREQ(2) + CH(2)
 * 292   WLAN#2 Status(2) + SNR(1) + RSSI(1)
 * 296   WLAN#2 Connect AP MAC(6) + pad(2)
 * 304   reserve(48)
 * 352   (end of body — total 352B)
 * ======================================================================== */

#define OPC_GET_DEVICE_INFO_REQ_BODY_LEN  0
#define OPC_GET_DEVICE_INFO_REQ_LENGTH    0
#define OPC_GET_DEVICE_INFO_ACK_BODY_LEN  352
#define OPC_GET_DEVICE_INFO_ACK_LENGTH    408   /* spec literal — see T12 */

#define OPC_VERSION_FIELD_LEN     32
#define OPC_SERIAL_FIELD_LEN      32
#define OPC_ESSID_FIELD_LEN       32

typedef struct opc_get_device_info_ack {
    uint16_t result;
    uint16_t error_cause;
    uint32_t vendor_code;
    uint16_t product_code;
    uint16_t product_subcode;
    opc_date_t manufacture;
    opc_date_t shipment;
    char     firmware_version[OPC_VERSION_FIELD_LEN];
    char     hardware_version[OPC_VERSION_FIELD_LEN];
    char     serial_number   [OPC_SERIAL_FIELD_LEN];
    uint8_t  ethernet_mac[6];
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t default_gateway;
    uint32_t ntp_server;
    char     essid[OPC_ESSID_FIELD_LEN];
    uint32_t device_status;
    uint16_t station_type;
    uint16_t priority_ch;       /* high byte band, low byte CH (Dual only) */
    uint8_t  ieee_11r;
    uint8_t  ieee_11ai;
    uint8_t  ieee_11k;
    uint8_t  ieee_11v;
    opc_wlan_radio_state_t wlan1;
    opc_wlan_radio_state_t wlan2;
} opc_get_device_info_ack_t;

ssize_t opc_get_device_info_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_get_device_info_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_get_device_info_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                     const opc_get_device_info_ack_t *in);
int     opc_get_device_info_ack_unpack(const uint8_t *frame, size_t frame_len,
                                       opc_get_device_info_ack_t *out);

/* ========================================================================
 * 0x1001 — SetPassword
 * ======================================================================== */

#define OPC_SET_PASSWORD_REQ_BODY_LEN  256   /* old(128) + new(128) */
#define OPC_SET_PASSWORD_REQ_LENGTH    312   /* spec */

typedef struct opc_set_password_req {
    char old_password[128];   /* NULL-terminated, up to 127 chars */
    char new_password[128];
} opc_set_password_req_t;

typedef struct opc_set_password_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_set_password_ack_t;

ssize_t opc_set_password_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_set_password_req_t *in);
int     opc_set_password_req_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_set_password_req_t *out);
ssize_t opc_set_password_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_set_password_ack_t *in);
int     opc_set_password_ack_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_set_password_ack_t *out);

/* ========================================================================
 * 0x1002 — SetIpConfigList
 *
 * Variable-length request: 1..20 entries × 64 B each + 56 B reserve.
 * Per-entry layout (entry-relative offset, 64 B each):
 *    0   List Boundary Flag (2)   — spec p.22 values (see T2)
 *    2   Configuration list Number (2, 1..128)
 *    4   IP Address (4)
 *    8   Subnet Mask (4)
 *   12   Default Gateway (4)
 *   16   NTP Server IP (4)
 *   20   ESSID (32, NULL-terminated)
 *   52   Reserve (12)
 * ======================================================================== */

#define OPC_IPCFG_ENTRY_LEN          64
#define OPC_IPCFG_LIST_MAX_PER_REQ   20
#define OPC_IPCFG_LIST_MAX_SLOTS     128
#define OPC_SET_IP_CONFIG_LIST_BODY_MIN  (1  * OPC_IPCFG_ENTRY_LEN)   /* 64 */
#define OPC_SET_IP_CONFIG_LIST_BODY_MAX  (20 * OPC_IPCFG_ENTRY_LEN)   /* 1280 */
#define OPC_SET_IP_CONFIG_LIST_REQ_LENGTH(n)  (56 + (n) * OPC_IPCFG_ENTRY_LEN)

/* Boundary-flag values per spec page 24 (field description).
 *
 * Spec ambiguity: page 22 (body description) lists START=0x0001 and
 * CONTINUE=0x0000, while page 24 (field description) lists START=0x0000
 * and CONTINUE=0x0001. The vendor confirmed the page-24 field
 * description as authoritative.
 *
 * START_END (page-22 body 0x0003) is absent from the field description
 * and has been dropped — atomic single-frame commit is no longer
 * supported; callers must send a START frame followed by an END frame. */
#define OPC_LIST_BOUNDARY_START      0x0000
#define OPC_LIST_BOUNDARY_CONTINUE   0x0001
#define OPC_LIST_BOUNDARY_END        0x0002

typedef struct opc_ipcfg_entry {
    uint16_t boundary_flag;
    uint16_t list_number;     /* 1..128 */
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t default_gateway;
    uint32_t ntp_server;
    char     essid[32];
} opc_ipcfg_entry_t;

typedef struct opc_set_ip_config_list_req {
    size_t entry_count;       /* 1..OPC_IPCFG_LIST_MAX_PER_REQ */
    opc_ipcfg_entry_t entries[OPC_IPCFG_LIST_MAX_PER_REQ];
} opc_set_ip_config_list_req_t;

typedef struct opc_set_ip_config_list_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_set_ip_config_list_ack_t;

ssize_t opc_set_ip_config_list_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_set_ip_config_list_req_t *in);
int     opc_set_ip_config_list_req_unpack(const uint8_t *frame, size_t frame_len,
                                          opc_set_ip_config_list_req_t *out);
ssize_t opc_set_ip_config_list_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_set_ip_config_list_ack_t *in);
int     opc_set_ip_config_list_ack_unpack(const uint8_t *frame, size_t frame_len,
                                          opc_set_ip_config_list_ack_t *out);

/* ========================================================================
 * 0x1003 — ChangeIpAddress
 *
 * Spec body shows "Reserve | Configuration list Number (2B)" — we interpret as
 * 2 B reserve at body offset 0..1 and the list number at body offset 2..3.
 * ======================================================================== */

#define OPC_CHANGE_IP_ADDRESS_REQ_BODY_LEN  4
#define OPC_CHANGE_IP_ADDRESS_REQ_LENGTH    60   /* spec */

typedef struct opc_change_ip_address_req {
    uint16_t list_number;     /* 1..128 */
} opc_change_ip_address_req_t;

typedef struct opc_change_ip_address_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_change_ip_address_ack_t;

ssize_t opc_change_ip_address_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                       const opc_change_ip_address_req_t *in);
int     opc_change_ip_address_req_unpack(const uint8_t *frame, size_t frame_len,
                                         opc_change_ip_address_req_t *out);
ssize_t opc_change_ip_address_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                       const opc_change_ip_address_ack_t *in);
int     opc_change_ip_address_ack_unpack(const uint8_t *frame, size_t frame_len,
                                         opc_change_ip_address_ack_t *out);

/* ========================================================================
 * 0x1004 — SetRadioConfig
 *
 * Body offset layout (20 B):
 *    0   Station Type(2) + Priority CH(2)
 *    4   WLAN#1 FREQ(2) + WLAN#1 CH(2)
 *    8   WLAN#1 Mode(1) + WLAN#1 BW(1) + reserve(2)
 *   12   WLAN#2 CH(2)   + WLAN#2 FREQ(2)        ← spec orders CH first then FREQ here (T13)
 *   16   WLAN#2 Mode(1) + WLAN#2 BW(1) + reserve(2)
 * ======================================================================== */

#define OPC_SET_RADIO_CONFIG_REQ_BODY_LEN   20
#define OPC_SET_RADIO_CONFIG_REQ_LENGTH     76   /* spec */

typedef struct opc_set_radio_config_req {
    uint16_t station_type;
    uint16_t priority_ch;
    opc_wlan_radio_cfg_t wlan1;
    opc_wlan_radio_cfg_t wlan2;   /* Dual-only — Single Station ignores fields */
} opc_set_radio_config_req_t;

typedef struct opc_set_radio_config_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_set_radio_config_ack_t;

ssize_t opc_set_radio_config_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                      const opc_set_radio_config_req_t *in);
int     opc_set_radio_config_req_unpack(const uint8_t *frame, size_t frame_len,
                                        opc_set_radio_config_req_t *out);
ssize_t opc_set_radio_config_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                      const opc_set_radio_config_ack_t *in);
int     opc_set_radio_config_ack_unpack(const uint8_t *frame, size_t frame_len,
                                        opc_set_radio_config_ack_t *out);

/* ========================================================================
 * 0x1005 — SetIndicationConfig
 *
 * Body 8 B:
 *    0   Indication UDP Port(2) + Info(1) + Period(1)
 *    4   Indication IP Address(4)
 * ======================================================================== */

#define OPC_SET_INDICATION_CONFIG_REQ_BODY_LEN  8
#define OPC_SET_INDICATION_CONFIG_REQ_LENGTH    64   /* spec */

typedef struct opc_set_indication_config_req {
    uint16_t recipient_port;
    uint8_t  info_bits;         /* OPC_IND_BIT_* mask */
    uint8_t  period_seconds;    /* 0..255 — 0 disables Keep-Alive */
    uint32_t recipient_ip;
} opc_set_indication_config_req_t;

typedef struct opc_set_indication_config_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_set_indication_config_ack_t;

ssize_t opc_set_indication_config_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                           const opc_set_indication_config_req_t *in);
int     opc_set_indication_config_req_unpack(const uint8_t *frame, size_t frame_len,
                                             opc_set_indication_config_req_t *out);
ssize_t opc_set_indication_config_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                           const opc_set_indication_config_ack_t *in);
int     opc_set_indication_config_ack_unpack(const uint8_t *frame, size_t frame_len,
                                             opc_set_indication_config_ack_t *out);

/* ========================================================================
 * 0x2001 — Reset
 * Ack body carries Result+ErrorCause but header Length=0 (see T11).
 * ======================================================================== */

#define OPC_RESET_REQ_BODY_LEN    0
#define OPC_RESET_REQ_LENGTH      0
#define OPC_RESET_ACK_LENGTH      0   /* spec literal — body still present (T11) */

typedef struct opc_reset_ack {
    uint16_t result;
    uint16_t error_cause;
} opc_reset_ack_t;

ssize_t opc_reset_req_pack(uint8_t *frame, size_t cap, uint16_t seq_no);
int     opc_reset_req_unpack(const uint8_t *frame, size_t frame_len);
ssize_t opc_reset_ack_pack(uint8_t *frame, size_t cap, uint16_t seq_no, const opc_reset_ack_t *in);
int     opc_reset_ack_unpack(const uint8_t *frame, size_t frame_len, opc_reset_ack_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_COMMANDS_H */
