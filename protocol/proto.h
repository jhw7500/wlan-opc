#ifndef WLAN_OPC_PROTO_H
#define WLAN_OPC_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wire-format constants for the VHL ↔ wireless-board UDP/IP common-control
 * protocol (spec Rev1.00, 2026-05-25).
 *
 * - All multi-byte integers on the wire are big-endian.
 * - Every frame starts with a 64-byte common header. Bytes 8..63 are reserve
 *   and MUST be transmitted as zero.
 * - The header `length` field reports the payload length only (header excluded).
 *
 * TODO(spec-inconsistency): the spec text says "Length max 1416 B" but also
 * "UDP payload max 1424 B" with a 64-byte header — those numbers do not agree.
 * We use the geometric maximum 1424 - 64 = 1360 B as the effective payload
 * cap. Tracked in wlan-package/docs/proto-todo.md.
 */
#define OPC_PROTOCOL_VERSION  0x01

#define OPC_HEADER_SIZE       64
#define OPC_PAYLOAD_MAX       1360
#define OPC_FRAME_MAX         1424

/* CommandType (header byte 1). */
#define OPC_CMD_REQUEST       0x01
#define OPC_CMD_ACK           0x02
#define OPC_CMD_INDICATION    0x03

/* Result codes appearing in Acknowledgment payloads. */
#define OPC_RESULT_OK         0x0000
#define OPC_RESULT_NG         0x0001

/*
 * Common ErrorCause codes used by most Acknowledgment payloads.
 * Command-specific ErrorCause codes (0x0010+) live with the command's handler.
 */
#define OPC_ERR_NONE                0x0000
#define OPC_ERR_LOGIN_VIOLATION     0x0001 /* issued while device still booting */
#define OPC_ERR_LOGIN_CONDITION     0x0002 /* not logged in / other IP holds it */
#define OPC_ERR_PACKET_SIZE         0x0003 /* header.length disagrees with frame */
#define OPC_ERR_NVRAM               0x0004 /* persistent-write failure */

/* Response-timer policy (seconds). */
#define OPC_TIMER_REFERENCE_S       1
#define OPC_TIMER_NVRAM_WRITE_S     120

/* Login idle timeout — 5 minutes. */
#define OPC_LOGIN_IDLE_S            300

#endif /* WLAN_OPC_PROTO_H */
