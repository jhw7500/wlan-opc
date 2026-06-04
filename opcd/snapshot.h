#ifndef WLAN_OPC_OPCD_SNAPSHOT_H
#define WLAN_OPC_OPCD_SNAPSHOT_H

/*
 * device-info Ack snapshot publisher.
 *
 * After every successful GetDeviceInfo response, opcd dumps the same fields
 * it just sent on the wire into a JSON file under /dev/shm/opcd/. External
 * tools (monitoring, debugging, ops scripts) can then observe what the
 * device is reporting without having to speak the OPC protocol themselves.
 *
 * Design notes:
 *   - tmpfs path: /dev/shm/opcd/device_info.json. /dev/shm is tmpfs on every
 *     Linux distro we target, so the writes do not hit a real disk.
 *   - Write is atomic: temp file + rename(2). Readers either see the previous
 *     snapshot or the new one — never a half-written file.
 *   - Failure is non-fatal. snapshot publish is a side channel; if /dev/shm
 *     is read-only or the directory is missing, the response was already
 *     sent and opcd continues. A single stderr warning is emitted on first
 *     failure per process.
 *   - One-time mkdir at init lets the rest of the path stay write-once.
 */

#include <stddef.h>

#include "../protocol/commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default tmpfs paths. Override at link time if the deployment uses a
 * different runtime dir. */
#define OPCD_SNAPSHOT_DIR      "/dev/shm/opcd"
#define OPCD_SNAPSHOT_PATH     OPCD_SNAPSHOT_DIR "/device_info.json"

/* Create the snapshot directory if it does not already exist. Returns 0
 * on success or when the directory was already present. Returns -errno on
 * failure; the caller (opcd boot path) treats this as non-fatal and falls
 * back to "no snapshot writes". Safe to call repeatedly. */
int opcd_snapshot_init(const char *dir);

/* Dump `ack` as JSON to `path` using a temp file + rename(2) for atomicity.
 * Returns 0 on success, -errno on failure. Invoke it right after packing the
 * GetDeviceInfo ack (the current caller publishes before the subsequent UDP
 * send) — the snapshot mirrors the packed ack content, so send timing is
 * irrelevant. It must never block or fail the response path, so callers
 * ignore the return value. */
int opcd_snapshot_publish(const opc_get_device_info_ack_t *ack,
                          const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_SNAPSHOT_H */
