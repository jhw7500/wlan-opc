#ifndef WLAN_OPC_PROTO_H
#define WLAN_OPC_PROTO_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire-format constants for the VHL ↔ wireless-board UDP/IP common-control
 * protocol (spec Rev1.00, 2026-05-25), confirmed against the original docx
 * byte-map figures (그림 3-6 통신 포맷 + the per-command format figures).
 *
 * - All multi-byte integers on the wire are big-endian.
 * - The common header is 64 bytes: an 8-byte fixed part (protocol_version,
 *   command_type, req_indication_id, sequence_number, length) at bytes 0..7,
 *   followed by a 56-byte Reserve at bytes 8..63 (transmitted as zero).
 * - The Command Payload (body) starts at byte 64.
 * - The header `length` field carries (total_frame_bytes - 8): everything
 *   after the 8-byte fixed part, i.e. Reserve(56) + payload.
 *   Empty-body requests (Logout / GetBasicInfo / GetDeviceInfo / Reset) put
 *   ONLY the 8-byte fixed header on the wire (no Reserve), so their Length=0
 *   falls straight out of (8 - 8) — no special case. See opc_empty_frame_build.
 *
 * Sizing: fixed 8 + reserve 56 + payload max 1360 = frame max 1424; the
 * length cap is 1424 - 8 = 1416 B and the payload cap is 1360 B, matching
 * the spec text and the figure ("최대 1424Byte UDP/IP 페이로드") exactly.
 */
#define OPC_PROTOCOL_VERSION  0x01

#define OPC_FIXED_HEADER_SIZE 8     /* ver+type+id+seq+length; an empty-body frame is exactly this */
#define OPC_HEADER_SIZE       64    /* common header = fixed 8 + reserve 56; body starts here */
#define OPC_PAYLOAD_MAX       1360
#define OPC_FRAME_MAX         1424

/* Sizing invariant: common header + payload exactly fills the frame. A future
 * tweak to one of these without updating the others would silently desync
 * wire-format consumers; surface it at compile time instead.
 * static_assert from <assert.h> is portable to both C11 and C++11. */
static_assert(OPC_HEADER_SIZE + OPC_PAYLOAD_MAX == OPC_FRAME_MAX,
              "OPC_HEADER_SIZE + OPC_PAYLOAD_MAX must equal OPC_FRAME_MAX");
static_assert(OPC_FIXED_HEADER_SIZE < OPC_HEADER_SIZE,
              "fixed header must fit inside the common header");

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
