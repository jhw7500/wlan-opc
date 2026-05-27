#ifndef WLAN_OPC_OPCD_PLATFORM_H
#define WLAN_OPC_OPCD_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

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
 * Exactly one implementation is linked. Selection is a Makefile concern;
 * opcd code only sees this header.
 */

/* Capability advertisement for GetDeviceInformation (proto-todo:T5).
 * Each field is 0 (unsupported) or 1 (supported), as the spec encodes them.
 *
 * Field order intentionally matches opc_get_device_info_ack_t in
 * protocol/commands.h (r / ai / k / v) so that bulk-copy code paths cannot
 * silently swap bits. Code that mirrors fields must still use named
 * assignments — this layout is a defense-in-depth, not a license. */
typedef struct opcd_platform_caps {
    uint8_t ieee_11r;     /* Fast BSS Transition  */
    uint8_t ieee_11ai;    /* Fast Initial Link    */
    uint8_t ieee_11k;     /* RRM                  */
    uint8_t ieee_11v;     /* BSS Transition Mgmt  */
} opcd_platform_caps_t;

/* Steady-state link snapshot used to fill GetDeviceInformation per-WLAN fields
 * and as the seed for Roaming Indication payloads. Field semantics match the
 * OPC protocol; freq_mhz=0 / channel=0 mean "no association".
 * snr/rssi naming AND order match opc_wlan_radio_state_t (commands.h),
 * opcd_platform_evt_t.u.roaming (below), and the opcd_ind_roaming(snr, rssi)
 * signature in indication.h. */
typedef struct opcd_platform_link {
    uint16_t freq_mhz;
    uint16_t channel;
    uint8_t  mode;        /* OPC_WLAN_MODE_*    */
    uint8_t  bandwidth;   /* OPC_BANDWIDTH_*    */
    uint8_t  bssid[6];    /* zeros when not associated */
    int8_t   snr;         /* dB                 */
    int8_t   rssi;        /* dBm                */
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

typedef struct opcd_platform_evt {
    opcd_platform_evt_kind_t kind;
    union {
        struct { uint32_t status; }                                 init_complete;
        struct { uint16_t status; uint16_t channel; }               wlan_status;
        struct { int8_t snr; int8_t rssi; uint8_t mac[6];
                 uint16_t channel; }                                roaming;
        struct { uint16_t reason_msg_id; uint16_t result_code;
                 uint8_t  mac[6]; }                                 ap_disconnect;
        struct { uint16_t congestion_id; uint16_t current_val; }    fault_detect;
        struct { uint32_t cause; }                                  reset_notice;
    } u;
} opcd_platform_evt_t;

/* Single drained event delivered to opcd. Return non-zero to stop the drain;
 * drain_events() then returns that same non-zero value verbatim. */
typedef int (*opcd_platform_evt_cb)(const opcd_platform_evt_t *evt, void *ctx);

/* vtable. All members non-NULL after init(); stub fills missing pieces with
 * "0/empty/ok" semantics so opcd never has to NULL-check. */
typedef struct opcd_platform_ops {
    /* Lifecycle. init() is allowed to fail (-errno); opcd will refuse to start.
     * shutdown() must be both idempotent AND async-signal-safe (POSIX.1):
     * implementations may only call functions on the AS-safe list — no
     * fprintf, no malloc/free, no pthread mutexes, no mlanutl exec — because
     * opcd may invoke it from a SIGTERM/SIGINT handler. State that cannot be
     * torn down safely from a signal handler must be deferred to the main
     * loop's post-signal cleanup path. */
    int  (*init)(void);
    void (*shutdown)(void);

    /* Identity / inventory (GetBasicInfo + GetDeviceInfo).
     * Strings are written NUL-terminated, truncated to cap (silent — no
     * overflow signal; callers that need overflow detection should size cap
     * larger than the expected maximum).
     * get_eth_ipv4 writes the IPv4 address in **host byte order**, matching
     * opcd_state_t::holder_ip and indication_recipient_ip. Callers serializing
     * to the wire are responsible for htonl().
     * get_wlan_mac returns -ENODEV for idx outside [0, wlan_count). */
    int  (*get_eth_mac)(uint8_t mac[6]);
    int  (*get_eth_ipv4)(uint32_t *ip_host);
    int  (*get_wlan_mac)(int idx /*0=mlan0,1=mlan1*/, uint8_t mac[6]);
    int  (*get_firmware_version)(char *buf, size_t cap);
    int  (*get_hardware_version)(char *buf, size_t cap);
    int  (*get_serial_number)(char *buf, size_t cap);
    int  (*get_manufacture_date)(opc_date_t *out);
    int  (*get_shipment_date)(opc_date_t *out);
    int  (*get_caps)(opcd_platform_caps_t *out);

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
     * has accepted the change; any non-zero is mapped by opcd to the
     * "regulation-class" NG (Error Cause 0x0050 / vendor-specific).
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
     *   0          — drained all queued events, cb returned 0 each time
     *   <0         — drain failed before completing (-errno)
     *   non-zero   — cb returned non-zero; that exact value is propagated
     *                back so the caller can distinguish "stopped early" from
     *                "drained clean". */
    int  (*event_fd)(void);
    int  (*drain_events)(opcd_platform_evt_cb cb, void *ctx);
} opcd_platform_ops_t;

/* Global accessor. Exactly one of platform_stub_register() /
 * platform_nxp_register() runs at startup (from opcd.c, behind a build-
 * time guard) and stashes the ops table here. Callers may cache the result. */
const opcd_platform_ops_t *opcd_platform(void);

/* Implementation registration — exported by the corresponding .c file. */
void opcd_platform_stub_register(void);
void opcd_platform_nxp_register(void);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_PLATFORM_H */
