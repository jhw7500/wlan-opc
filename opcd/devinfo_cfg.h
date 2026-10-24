#ifndef WLAN_OPC_OPCD_DEVINFO_CFG_H
#define WLAN_OPC_OPCD_DEVINFO_CFG_H

/* GetDeviceInfo Mode / Band Width before a SetRadioConfig is received (#193).
 *
 * GetDeviceInfo reports the configured radio standard and channel width, never
 * the live link's. A received SetRadioConfig value wins (handler.c); without
 * one the device still has a radio configuration in wifi_init_conf.json, read
 * here the way wifi_init.sh applies it at boot:
 *
 *   Mode       .<iface>.radio.mode (b/g/a/n/ac/ax — written by SetRadioConfig
 *              and by settings made outside OPC; mlan1 has no ax), else
 *              .<iface>.STANDARD, or .global.STANDARD when that is empty
 *              (n/ac/ax or 4/5/6: the standard the driver is limited to; at or
 *              above the interface maximum — mlan0 ax, mlan1 ac — it means
 *              that maximum).
 *   Band Width .<iface>.radio.bw (20/40/80). There is no other width setting:
 *              without it the driver default applies, which is not a
 *              configured value ("auto" likewise).
 *
 * Anything absent or invalid reports OPC_WLAN_MODE_UNSET / OPC_BANDWIDTH_UNSET. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure scanner over a wifi_init_conf.json text. iface is "mlan0" or "mlan1".
 * A key counts only as a direct member of its own object (string literals,
 * such as the shipped file's _comment entries, are skipped). */
void opcd_devinfo_cfg_from_json(const char *json, const char *iface,
                                uint8_t *mode, uint8_t *bandwidth);

/* The same, reading the file at `path`; NULL or unreadable → both unset. */
void opcd_devinfo_cfg_read(const char *path, const char *iface,
                           uint8_t *mode, uint8_t *bandwidth);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_DEVINFO_CFG_H */
