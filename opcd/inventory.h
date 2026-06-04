#ifndef WLAN_OPC_OPCD_INVENTORY_H
#define WLAN_OPC_OPCD_INVENTORY_H

/*
 * Static inventory data loaded from /usr/local/opc/etc/device_info.json once
 * at opcd startup. Holds the build-/manufacture-time identity fields that
 * GetDeviceInfo (and GetBasicInfo) read but the firmware itself cannot derive
 * at runtime — vendor/product codes, hardware revision, serial number, IEEE
 * capability bits, and the manufacture/shipment dates.
 *
 * Design note: anything that *can* be queried at runtime (firmware_version
 * via dpkg-query, ntp_server via timesyncd.conf, link state via cantops
 * link.json) stays on the platform_ops vtable rather than living here.
 *
 * On load failure (missing file, malformed JSON) opcd continues with a
 * zero-initialised inventory — device-info responses then carry empty
 * strings, zero codes, and "no support" capability bits — and emits a
 * single stderr warning. This is intentional: a device with a missing
 * inventory file should still answer protocol requests so the operator
 * can diagnose, instead of refusing to boot.
 */

#include <stdint.h>

#include "../protocol/commands.h"   /* opc_date_t, OPC_VERSION_FIELD_LEN, OPC_SERIAL_FIELD_LEN */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct opcd_inventory {
    uint32_t   vendor_code;        /* IEEE OUI, e.g. 0x00902CFB */
    uint16_t   product_code;
    uint16_t   product_subcode;
    char       hardware_version[OPC_VERSION_FIELD_LEN];
    char       serial_number   [OPC_SERIAL_FIELD_LEN];
    opc_date_t manufacture_date;
    opc_date_t shipment_date;
    /* Capability advertisement — currently a static hint sourced from the
     * inventory file. A future PR will replace these with a live nl80211 /
     * wpa_supplicant query; the JSON values then become the fallback when
     * the live query is unavailable. */
    uint8_t    ieee_11r;
    uint8_t    ieee_11ai;
    uint8_t    ieee_11k;
    uint8_t    ieee_11v;
} opcd_inventory_t;

/* Load the inventory JSON at `path` into the process-wide singleton.
 * On any failure (file missing, JSON malformed, fields absent) the global
 * inventory keeps its previous contents — which is zero-initialised before
 * the first successful call — and a single warning is written to stderr.
 *
 * Return value:
 *   0      — file loaded; every recognised field was parsed (missing fields
 *            leave the corresponding inventory member at its previous value).
 *  -errno  — file could not be opened or read.
 *  -EINVAL — file content was not parseable enough to recover anything.
 */
int opcd_inventory_load(const char *path);

/* Always non-NULL. Returns a pointer to the process-wide inventory; the
 * caller must NOT cache fields across calls if it expects them to react
 * to a future opcd_inventory_load() call. */
const opcd_inventory_t *opcd_inventory(void);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_INVENTORY_H */
