#ifndef WLAN_OPC_OPCD_INDICATION_H
#define WLAN_OPC_OPCD_INDICATION_H

#include "opcd_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Send a single Indication frame to the configured recipient. No-op (returns 0)
 * when indication delivery is disabled. The corresponding bit must be set in
 * indication_info_bits or the call is suppressed. */
int opcd_ind_init_complete (opcd_state_t *st, uint32_t status);
int opcd_ind_wlan_status   (opcd_state_t *st, uint16_t status, uint16_t ch);
int opcd_ind_roaming       (opcd_state_t *st, int8_t snr, int8_t rssi,
                            const uint8_t ap_mac[6], uint16_t ch);
int opcd_ind_ap_disconnect (opcd_state_t *st, uint16_t msg_id, uint16_t reason,
                            const uint8_t ap_mac[6]);
int opcd_ind_fault_detect  (opcd_state_t *st, uint16_t cong_id, uint16_t val);
int opcd_ind_reset_notice  (opcd_state_t *st, uint32_t cause);
int opcd_ind_keep_alive    (opcd_state_t *st, const char *timestamp);

/* Called once per second by the main loop's timerfd. Emits KeepAlive when the
 * configured period has elapsed. */
void opcd_ind_tick(opcd_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_INDICATION_H */
