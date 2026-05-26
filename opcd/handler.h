#ifndef WLAN_OPC_OPCD_HANDLER_H
#define WLAN_OPC_OPCD_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "opcd_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dispatch an inbound UDP frame to the right per-command handler.
 *
 * `frame`/`frame_len`     — inbound bytes (must contain at least a 64B header)
 * `client_ip`             — host-order source IPv4 of the sender
 * `client_port`           — host-order source UDP port
 * `resp`/`resp_cap`       — buffer for the outgoing Ack/response frame
 * `*resp_len`             — set to the response size (0 if no response)
 *
 * Returns 0 on success, -1 on unrecoverable parse failure.
 */
int opcd_dispatch(opcd_state_t *st,
                  const uint8_t *frame, size_t frame_len,
                  uint32_t client_ip, uint16_t client_port,
                  uint8_t *resp, size_t resp_cap, ssize_t *resp_len);

/* After Logout response has been transmitted, apply any pending ChangeIp. */
void opcd_apply_pending_ip_change(opcd_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_HANDLER_H */
