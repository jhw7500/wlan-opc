#ifndef WLAN_OPC_INDICATIONS_H
#define WLAN_OPC_INDICATIONS_H

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
 * 0x0001 — InitComplete (Length=60, body 4 B)
 * ======================================================================== */

#define OPC_IND_INIT_COMPLETE_BODY_LEN   4
#define OPC_IND_INIT_COMPLETE_LENGTH     60

typedef struct opc_ind_init_complete {
    uint32_t status;        /* OPC_INIT_STATE_* */
} opc_ind_init_complete_t;

ssize_t opc_ind_init_complete_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                   const opc_ind_init_complete_t *in);
int     opc_ind_init_complete_unpack(const uint8_t *frame, size_t frame_len,
                                     opc_ind_init_complete_t *out);

/* ========================================================================
 * 0x0002 — WlanStatusChange (Length=60, body 4 B)
 * ======================================================================== */

#define OPC_IND_WLAN_STATUS_CHANGE_BODY_LEN   4
#define OPC_IND_WLAN_STATUS_CHANGE_LENGTH     60

#define OPC_WLAN_STATUS_CONNECTED       0x0001
#define OPC_WLAN_STATUS_DISCONNECTED    0x0002

typedef struct opc_ind_wlan_status_change {
    uint16_t wlan_status;       /* OPC_WLAN_STATUS_* */
    uint16_t indication_ch;     /* upper byte band, lower byte CH number */
} opc_ind_wlan_status_change_t;

ssize_t opc_ind_wlan_status_change_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                        const opc_ind_wlan_status_change_t *in);
int     opc_ind_wlan_status_change_unpack(const uint8_t *frame, size_t frame_len,
                                          opc_ind_wlan_status_change_t *out);

/* ========================================================================
 * 0x0004 — Roaming (Length=68, body 12 B)
 *
 * Body layout (body offset):
 *    0   SNR(1) + RSSI(1) + reserve(2)
 *    4   Connect AP MAC (6)
 *   10   CH Number (2)        ── spec table mis-aligned (see T14)
 * ======================================================================== */

#define OPC_IND_ROAMING_BODY_LEN   12
#define OPC_IND_ROAMING_LENGTH     68

typedef struct opc_ind_roaming {
    int8_t   current_snr;
    int8_t   current_rssi;
    uint8_t  connect_ap_mac[6];
    uint16_t ch_number;     /* upper byte band, lower byte CH number */
} opc_ind_roaming_t;

ssize_t opc_ind_roaming_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                             const opc_ind_roaming_t *in);
int     opc_ind_roaming_unpack(const uint8_t *frame, size_t frame_len,
                               opc_ind_roaming_t *out);

/* ========================================================================
 * 0x0008 — ApDisconnect (Length=68, body 12 B)
 *
 * Body layout:
 *    0   Message ID (2) + Result Code (2)
 *    4   Disconnect AP MAC (6) + reserve (2)
 * ======================================================================== */

#define OPC_IND_AP_DISCONNECT_BODY_LEN   12
#define OPC_IND_AP_DISCONNECT_LENGTH     68

#define OPC_AP_MSG_DISASSOCIATION     0x000A
#define OPC_AP_MSG_DEAUTHENTICATION   0x000C

typedef struct opc_ind_ap_disconnect {
    uint16_t message_id;        /* OPC_AP_MSG_* */
    uint16_t result_code;       /* IEEE 802.11 reason code */
    uint8_t  disconnect_ap_mac[6];
} opc_ind_ap_disconnect_t;

ssize_t opc_ind_ap_disconnect_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                   const opc_ind_ap_disconnect_t *in);
int     opc_ind_ap_disconnect_unpack(const uint8_t *frame, size_t frame_len,
                                     opc_ind_ap_disconnect_t *out);

/* ========================================================================
 * 0x0010 — FaultDetect (Length=60, body 4 B)
 * ======================================================================== */

#define OPC_IND_FAULT_DETECT_BODY_LEN   4
#define OPC_IND_FAULT_DETECT_LENGTH     60

#define OPC_CONGESTION_CPU       0x0001
#define OPC_CONGESTION_MEMORY    0x0002
#define OPC_CONGESTION_DISK_IO   0x0003
#define OPC_CONGESTION_NETWORK   0x0004

typedef struct opc_ind_fault_detect {
    uint16_t congestion_id;     /* OPC_CONGESTION_* */
    uint16_t current_val;
} opc_ind_fault_detect_t;

ssize_t opc_ind_fault_detect_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_ind_fault_detect_t *in);
int     opc_ind_fault_detect_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_ind_fault_detect_t *out);

/* ========================================================================
 * 0x0020 — ResetNotice (Length=60, body 4 B)
 * ======================================================================== */

#define OPC_IND_RESET_NOTICE_BODY_LEN   4
#define OPC_IND_RESET_NOTICE_LENGTH     60

typedef struct opc_ind_reset_notice {
    uint32_t reset_cause;       /* vendor-defined reset reason ID */
} opc_ind_reset_notice_t;

ssize_t opc_ind_reset_notice_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                  const opc_ind_reset_notice_t *in);
int     opc_ind_reset_notice_unpack(const uint8_t *frame, size_t frame_len,
                                    opc_ind_reset_notice_t *out);

/* ========================================================================
 * 0x0080 — KeepAlive (Length=88, body 32 B)
 *
 * Timestamp is an ISO-8601-like NULL-terminated string fitting in 32 B
 * (e.g. "2026-02-16T15:47:00Z").
 * ======================================================================== */

#define OPC_IND_KEEP_ALIVE_BODY_LEN   32
#define OPC_IND_KEEP_ALIVE_LENGTH     88
#define OPC_TIMESTAMP_MAX_LEN         31  /* + NULL */

typedef struct opc_ind_keep_alive {
    char timestamp[OPC_IND_KEEP_ALIVE_BODY_LEN];
} opc_ind_keep_alive_t;

ssize_t opc_ind_keep_alive_pack(uint8_t *frame, size_t cap, uint16_t seq_no,
                                const opc_ind_keep_alive_t *in);
int     opc_ind_keep_alive_unpack(const uint8_t *frame, size_t frame_len,
                                  opc_ind_keep_alive_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_INDICATIONS_H */
