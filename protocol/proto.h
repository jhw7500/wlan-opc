#ifndef WLAN_OPC_PROTO_H
#define WLAN_OPC_PROTO_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire-format constants for the VHL ↔ wireless-board UDP/IP common-control
 * protocol (spec Rev1.00, 2026-05-25), with vendor clarification applied.
 *
 * - All multi-byte integers on the wire are big-endian.
 * - Every frame starts with a 60-byte common header. Bytes 8..59 are
 *   reserve and MUST be transmitted as zero.
 * - The header `length` field carries (total_frame_bytes - 4): the first
 *   4 bytes (protocol version + command type + req/indication id) are
 *   excluded from the length count.
 *   Exception: empty-body commands (Logout, Reset, GetBasicInfo and
 *   GetDeviceInfo requests) carry Length=0 on the wire per spec literal,
 *   not the 56 that the (total-4) formula would otherwise produce.
 *   See OPC_LOGOUT_REQ_LENGTH and siblings in commands.h.
 *
 * Sizing: header 60 + payload max 1360 = frame max 1420; the length cap
 * is 1420 - 4 = 1416 B, matching the spec text exactly.
 */
#define OPC_PROTOCOL_VERSION  0x01

#define OPC_HEADER_SIZE       60
#define OPC_PAYLOAD_MAX       1360
#define OPC_FRAME_MAX         1420

/* Sizing invariant: header + payload exactly fills the frame. A future
 * tweak to one of these without updating the others would silently
 * desync wire-format consumers; surface it at compile time instead.
 * static_assert from <assert.h> is portable to both C11 and C++11. */
static_assert(OPC_HEADER_SIZE + OPC_PAYLOAD_MAX == OPC_FRAME_MAX,
              "OPC_HEADER_SIZE + OPC_PAYLOAD_MAX must equal OPC_FRAME_MAX");

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
