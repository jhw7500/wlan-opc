#ifndef WLAN_OPC_OPCD_RESET_CAUSE_H
#define WLAN_OPC_OPCD_RESET_CAUSE_H

#include <stdint.h>

/* ResetNotice (0x0020) producer for AUTONOMOUS resets — issue #47 item 3 /
 * proto-todo T9. §3.4.6: "무선 기판이 자율적으로 리셋하기 전에 통지한다 …
 * Reset Cause: 장치별로 통지 가능한 경우, 리셋 요인 ID". Cause IDs are
 * vendor-defined (inquiry Q10, 참고 항목) — table in protocol/ids.h.
 *
 * Every autonomous reboot on the board (wifi_checker, wlan_fw_watch,
 * wifi_logger_temp, wlan_emergency_reboot, factory_reset …) ends in
 * `wlan_reboot_policy.sh` / `systemctl reboot`, i.e. a systemd shutdown in
 * which opcd — ordered After=network-online.target — receives SIGTERM while
 * the network is still up. That SIGTERM is the one common hook point: on it
 * opcd asks systemd whether the SYSTEM is going down (a plain
 * `systemctl stop opcd` is not a reset and must stay silent), takes the
 * cause ID the reboot policy may have left in OPC_PATH_RESET_CAUSE, and
 * emits ResetNotice once before exiting. Hardware watchdog timeouts and
 * sysrq resets cannot be announced — the spec limits the notice to
 * "통지 가능한 경우".
 *
 * Standalone module so the three pure/bounded pieces are host-testable. */

#define OPC_PATH_RESET_CAUSE "/run/opc/reset_cause"   /* one line: 0x.. or decimal */
#define OPC_PATH_SYSTEMCTL   "/usr/bin/systemctl"
/* Deadline for the systemctl probe. Measured on cts-wlan (systemd 254,
 * 2026-09-04): 22 ms while running, but 1129 ms for the FIRST answer during
 * an actual shutdown (PID1 busy with stop jobs) — a 200 ms deadline timed
 * out and lost the notice. 3 s = 2.5× the observed worst case; only a
 * shutdown ever waits that long (TimeoutStopSec is 90 s), a plain
 * `systemctl stop opcd` still answers in tens of ms. */
#define OPC_RESET_PROBE_TIMEOUT_MS 3000

#ifdef __cplusplus
extern "C" {
#endif

/* Cause ID left by the reboot policy script (hex "0x12" or decimal, one
 * line, surrounding whitespace allowed). 0 when the file is absent, empty,
 * malformed, has trailing junk, or the value is 0 / > 32 bits. */
uint32_t opcd_reset_cause_read(const char *path);

/* Run `<systemctl> is-system-running` under one deadline of `timeout_ms`
 * covering read and reap (a dribbling writer cannot stretch it). Returns 1 when
 * stdout is "stopping" (a system shutdown/reboot is in progress), 0 for any
 * other answer (running/degraded/…; the exit status is ignored — systemd
 * exits non-zero for every state but "running"), -1 when the probe could
 * not answer (exec failure, timeout — the child is killed). */
int opcd_system_stopping(const char *systemctl_path, int timeout_ms);

/* The decision: notify only when the system is actually going down
 * (stopping == 1) — the file cause if present, else OPC_RESET_CAUSE_SYSTEM.
 * 0 (no notice) for "not stopping" and for "unknown" alike: a reset is
 * never announced on a guess. */
uint32_t opcd_shutdown_reset_cause(int stopping, uint32_t file_cause);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_RESET_CAUSE_H */
