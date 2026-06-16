#ifndef WLAN_OPC_OPCD_PLATFORM_H
#define WLAN_OPC_OPCD_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../protocol/commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * opcd ↔ platform (NXP driver) boundary.
 *
 * Pull API   : handler.c calls these synchronously while building Ack frames
 *              or applying NVM-staged config. All return 0 on success, -errno
 *              on failure. Output buffers are caller-provided; the platform
 *              layer must not allocate. Calls must complete well under the
 *              regulation 1-second response budget.
 *
 * Push API   : platform_event_fd() exposes a single edge-or-level fd that
 *              becomes readable when one or more NXP events are queued.
 *              The opcd main loop multiplexes it alongside the UDP socket
 *              and the keep-alive timerfd. When the fd fires, opcd calls
 *              platform_drain_events() which invokes the supplied callback
 *              once per queued event. Implementations must coalesce / debounce
 *              internally — opcd will not throttle.
 *
 * Implementations:
 *   platform_stub.c  — zeros / canned values, no kernel I/O. Default build.
 *   platform_nxp.c   — mlanutl + nl80211 + netlink. Selected at link time.
 *
 * Stub fault-injection env vars (STUB ONLY — ignored by platform_nxp.c):
 *   OPCD_STUB_APPLY_RADIO_RC=<negative errno>
 *       stub_apply_radio_config() returns this value instead of 0.
 *       Examples: -71 (-EPROTO), -110 (-ETIMEDOUT).
 *       Unset or 0 → success (default).
 *
 * Exactly one implementation is linked. Selection is a Makefile concern;
 * opcd code only sees this header.
 */

/* Steady-state link snapshot used to fill GetDeviceInformation per-WLAN fields
 * and as the seed for Roaming Indication payloads. Field semantics match the
 * OPC protocol; freq_mhz=0 / channel=0 mean "no association".
 * snr/rssi naming AND order match opc_wlan_radio_state_t (commands.h),
 * opcd_platform_evt_t.u.roaming (below), and the opcd_ind_roaming(snr, rssi)
 * signature in indication.h. */
typedef struct opcd_platform_link {
    uint16_t freq_mhz;
    uint16_t channel;
    uint8_t  mode;            /* OPC_WLAN_MODE_*; 0 = unknown/legacy   */
    uint8_t  bandwidth;       /* OPC_BANDWIDTH_*; only valid if
                               * bandwidth_valid (BANDWIDTH_20 == 0
                               * collides with the zero-init default) */
    bool     bandwidth_valid; /* bandwidth holds a real driver value   */
    uint8_t  bssid[6];        /* zeros when not associated */
    int8_t   snr;             /* dB                 */
    int8_t   rssi;            /* dBm                */
    bool     associated;
} opcd_platform_link_t;

/* Platform-side trigger for an Indication that opcd should emit.
 * Drained one-at-a-time via platform_drain_events(). The opcd indication.c
 * layer is the only thing that knows about UDP framing; platform stays
 * protocol-agnostic in case other UIs grow later. */
typedef enum opcd_platform_evt_kind {
    OPCD_PEVT_NONE = 0,
    OPCD_PEVT_INIT_COMPLETE,     /* device boot status transition          */
    OPCD_PEVT_WLAN_STATUS,       /* mlan0/mlan1 up/down/channel-change     */
    OPCD_PEVT_ROAMING,           /* nl80211 ROAM event                     */
    OPCD_PEVT_AP_DISCONNECT,     /* nl80211 DISCONNECT                     */
    OPCD_PEVT_FAULT_DETECT,      /* CPU/Mem/Disk/Net congestion (T6)       */
    OPCD_PEVT_RESET_NOTICE,      /* watchdog / fault driven reset (T9)     */
} opcd_platform_evt_kind_t;

/* OPCD_PEVT_WLAN_STATUS.status values. Defined here (not just in
 * platform_stub.c / platform_nxp.c) so the two implementations cannot
 * silently diverge on encoding. */
#define OPCD_WLAN_STATUS_DOWN            0   /* link administratively down or carrier loss */
#define OPCD_WLAN_STATUS_UP              1   /* link up, awaiting association              */
#define OPCD_WLAN_STATUS_ASSOCIATED      2   /* associated with an AP                      */
#define OPCD_WLAN_STATUS_CHANNEL_CHANGE  3   /* operating channel changed (DFS/CSA)        */

typedef struct opcd_platform_evt {
    opcd_platform_evt_kind_t kind;
    union {
        struct {
            uint32_t status;
        } init_complete;
        struct {
            uint8_t  idx;       /* 0=mlan0, 1=mlan1 — required for DUAL builds */
            uint16_t status;
            uint16_t channel;
        } wlan_status;
        struct {
            uint8_t  idx;       /* 0=mlan0, 1=mlan1 — DUAL routes via this */
            int8_t   snr;
            int8_t   rssi;
            uint8_t  mac[6];    /* new AP MAC */
            uint16_t channel;
        } roaming;
        struct {
            uint8_t  idx;       /* 0=mlan0, 1=mlan1 */
            uint16_t reason_msg_id;
            uint16_t result_code;
            uint8_t  mac[6];    /* AP MAC at disconnect */
        } ap_disconnect;
        struct {
            uint16_t congestion_id;
            uint16_t current_val;
        } fault_detect;
        struct {
            uint32_t cause;
        } reset_notice;
    } u;
} opcd_platform_evt_t;

/* Single drained event delivered to opcd. Return semantics:
 *    0 — continue draining
 *   >0 — early-stop; drain_events() returns this exact positive value
 *  Callbacks MUST NOT return a negative value. The negative space is
 *  reserved for drain-layer failures (see drain_events). Implementations
 *  that need richer signaling should encode it in ctx, not in the return. */
typedef int (*opcd_platform_evt_cb)(const opcd_platform_evt_t *evt, void *ctx);

/* vtable. All members non-NULL after init(); stub fills missing pieces with
 * "0/empty/ok" semantics so opcd never has to NULL-check.
 *
 * Exception: get_wlan_count() must return >=1 even in the stub — opcd
 * treats 0 as a fatal-at-startup condition (zero WLAN interfaces
 * contradicts every station_type). The stub conventionally returns 1
 * (SINGLE). */
typedef struct opcd_platform_ops {
    /* Lifecycle. init() is allowed to fail (-errno); opcd will refuse to
     * start. init() MAY block during driver probe (mlanutl, nl80211 family
     * resolution), but implementations SHOULD bound the wait — opcd has no
     * way to time-out a stuck init(). Suggested bound: 5 seconds.
     *
     * teardown() (renamed from shutdown — POSIX shutdown(2) ambiguity in
     * files that include this header) must be both idempotent AND
     * async-signal-safe (POSIX.1): implementations may only call functions
     * on the AS-safe list — no fprintf, no malloc/free, no pthread mutexes,
     * no mlanutl exec — because opcd may invoke it from a SIGTERM/SIGINT
     * handler. State that cannot be torn down safely from a signal handler
     * must be deferred to the main loop's post-signal cleanup path.
     *
     * IMPORTANT for signal-handler use: do NOT call opcd_platform()->teardown
     * from inside a signal handler — reading the global ops pointer plus the
     * indirect function-pointer call is not atomic under POSIX. The boot
     * path should cache the teardown function pointer once registration is
     * done, then call the cached pointer from the handler:
     *   static void (*g_teardown)(void);
     *   // after register: g_teardown = opcd_platform()->teardown;
     *   // in handler:    if (g_teardown) g_teardown(); */
    int  (*init)(void);
    void (*teardown)(void);

    /* Identity / inventory (GetBasicInfo + GetDeviceInfo).
     * Strings are written NUL-terminated, truncated to cap (silent — no
     * overflow signal; callers that need overflow detection should size cap
     * larger than the expected maximum).
     * get_eth_ipv4_host (named with _host suffix to make the byte order
     * visible at every call site — easy to miss with bare get_eth_ipv4)
     * writes the IPv4 address in host byte order, matching
     * opcd_state_t::holder_ip and indication_recipient_ip. Callers
     * serializing to the wire are responsible for htonl().
     * get_wlan_mac returns -ENODEV for idx outside [0, wlan_count). */
    int  (*get_eth_mac)(uint8_t mac[6]);
    int  (*get_eth_ipv4_host)(uint32_t *ip_host);
    /* netmask / gateway: same byte-order convention as get_eth_ipv4_host —
     * host order; serializer is responsible for htonl(). gateway returns 0
     * (host order) when unconfigured. */
    int  (*get_eth_netmask_host)(uint32_t *netmask_host);
    int  (*get_eth_gateway_host)(uint32_t *gateway_host);
    int  (*get_wlan_mac)(int idx /*0=mlan0,1=mlan1*/, uint8_t mac[6]);
    /* SSID string for the indexed WLAN interface. NUL-terminated, silently
     * truncated to cap. Empty string when not associated. */
    int  (*get_essid)(int idx, char *buf, size_t cap);
    /* Live-queried fields. Static identity (hardware version / serial /
     * manufacture+shipment date / capability bits / vendor+product codes)
     * lives in inventory.h, not on this vtable. */
    int  (*get_firmware_version)(char *buf, size_t cap);
    /* IPv4 NTP server in host byte order; 0 when unconfigured or when the
     * platform cannot determine it (e.g. timesyncd.conf carries a hostname
     * rather than a literal IP). */
    int  (*get_ntp_server)(uint32_t *server_host);

    /* Returns the number of WLAN interfaces the platform actually exposes
     * (1 for SINGLE-station builds, 2 for DUAL). Callers must NOT assume the
     * station_type field implies count; a misconfigured target may report a
     * lower count than station_type suggests, in which case higher idx values
     * return -ENODEV. Returns -errno if the platform itself cannot query the
     * driver — opcd treats this as fatal at startup, since station_type
     * negotiation requires a known interface count. */
    int  (*get_wlan_count)(void);

    /* Steady-state link readback. idx 0/1 = mlan0/mlan1.
     * Implementations return -ENODEV for idx outside [0, get_wlan_count())
     * and 0 with associated=false for "interface exists but not associated". */
    int  (*get_link)(int idx, opcd_platform_link_t *out);

    /* Mutations. Called from handler.c right after the parsed request passes
     * validation and right before opcd writes NVM. Return 0 means the kernel
     * has accepted the change; any non-zero is mapped by opcd to the dedicated
     * apply-failure NG (Error Cause 0x0050 — the spec defines no apply-failure
     * code; see ids.h OPC_ERR_RADIO_APPLY / D9). After sending that NG ack, opcd
     * invokes this hook a second time with the last-good cfg (a deferred
     * best-effort revert run in the main loop) so a partial apply leaves no net
     * change without delaying the failure response.
     * The return value carries no field attribution: EVERY apply failure —
     * including a non-frequency one (e.g. a future bandwidth path) — reaches
     * the wire as 0x0050.
     * Implementations MUST be non-blocking (no mlanutl exec/wait without a
     * bounded short timeout) — stalling here freezes the opcd main loop and
     * blows the regulation 1-second response budget. Long-running work must
     * be queued and reported via an event later. */
    int  (*apply_radio_config)(const opc_set_radio_config_req_t *cfg);
    int  (*apply_ip_change)(const opc_ipcfg_entry_t *slot);

    /* Deterministic reset notice → soft reboot ack path.
     * opcd exits cleanly after Ack (main returns 0; systemd Restart=always
     * handles the reboot). This hook lets the platform pre-emptively quiesce
     * the radio so the next boot is clean. May be a no-op. */
    int  (*prepare_reset)(void);

    /* Event multiplexing. event_fd() returns -1 if no async events are
     * possible (e.g. stub) — opcd then skips registering it. drain_events
     * may be called even with fd=-1 and must simply return 0 in that case.
     * Otherwise drain_events returns:
     *    0  — drained all queued events, cb returned 0 each time
     *   >0  — cb returned a positive value (early-stop); that exact value
     *         is propagated back so the caller can identify the stop reason
     *   <0  — drain-layer failure (-errno). Reserved for the drain
     *         implementation only — callbacks MUST NOT use this space,
     *         see opcd_platform_evt_cb. */
    int  (*event_fd)(void);
    int  (*drain_events)(opcd_platform_evt_cb cb, void *ctx);
} opcd_platform_ops_t;

/* Global accessor. Returns NULL until a registration function has run; the
 * opcd boot path is expected to register exactly one implementation before
 * any handler/indication code calls this. Callers in the main code path may
 * therefore treat a NULL return as a fatal programming error and abort()
 * rather than silently producing UB. Once non-NULL the implementation must
 * NOT change identity at runtime — callers may cache the pointer. */
const opcd_platform_ops_t *opcd_platform(void);

/* Implementation registration — exported by the corresponding .c file.
 * EXACTLY ONE platform_*.c file is linked into opcd; the symbol below
 * resolves to that single implementation's registration function. Linking
 * two platform_*.c files yields a duplicate-symbol link error on this
 * function, which is the intentional build-time mutual-exclusion guard.
 * Build selection is a Makefile concern (e.g. PLATFORM=stub|nxp).
 *
 * Implementations SHOULD assert(g_ops == NULL) before writing the global
 * ops pointer so that an accidental double-call at runtime surfaces as an
 * abort rather than a silent clobber. */
void opcd_platform_register(void);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_PLATFORM_H */
