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
 * `frame`/`frame_len`     — inbound bytes (must contain at least a 60B header)
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

/* D9: after a SetRadioConfig NG ack has been transmitted, re-apply the last-good
 * radio config the handler armed on an apply failure (deferred best-effort
 * revert). No-op unless st->radio_revert_pending. Kept off the response path so
 * the failure ack is never delayed by a second (possibly timing-out) apply. */
void opcd_radio_revert_drain(opcd_state_t *st);

/* Single owner of session teardown: emits the final LOGGED_OUT indication,
 * clears the login session, and stops the indication stream. Called from the
 * explicit Logout handler, the dispatch idle check, and the main-loop timer
 * idle check so all three paths share one set of side effects. */
void opcd_session_logout(opcd_state_t *st);

/* D12/D13: bad-length datagram → 0x0003 NG toward the logged-in session's
 * IP only; every other source is dropped silently (SEC-003). `valid_len`
 * is how many bytes of `frame` actually landed in the buffer. */
void opcd_reject_bad_length(opcd_state_t *st, const uint8_t *frame,
                            size_t valid_len, uint32_t cip, uint16_t cport);

/* Drain finished async NVRAM writes (PERF-001) and transmit the deferred
 * Set* acks they correspond to. Call when st->store_async's event fd is
 * readable. No-op when no async store is attached. */
void opcd_store_async_on_ready(opcd_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_HANDLER_H */
