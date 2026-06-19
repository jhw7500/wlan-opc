#ifndef WLAN_OPC_OPCD_STATE_H
#define WLAN_OPC_OPCD_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../protocol/commands.h"
#include "../protocol/indications.h"
#include "fault_probe.h"
#include "freq_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime configuration. Identity fields (vendor/product codes, hardware
 * revision, serial, dates, capability bits) used to live here; they now
 * live in inventory.h and are loaded from device_info.json at startup so
 * operators can edit them without rebuilding opcd. */
/* opcd_freq_source_t (device_info_freq_source) lives in freq_source.h so the
 * opc.conf parser is host-unit-testable without linking main(). */
typedef struct opcd_conf {
    uint16_t udp_port;             /* default OPC_DEFAULT_UDP_PORT */
    uint16_t default_station_type; /* OPC_STATION_SINGLE / DUAL */
    uint32_t login_idle_s;         /* configurable for tests (default OPC_LOGIN_IDLE_S) */
    opcd_freq_source_t device_info_freq_source; /* default OPC_FREQ_SRC_CONFIG */
} opcd_conf_t;

#define OPC_DEFAULT_UDP_PORT  50607
#define OPC_PASSWORD_DEFAULT  "MyPassword"

/* Filesystem paths under /usr/local/opc/etc — defaults. */
typedef struct opcd_paths {
    const char *conf;
    const char *password;
    const char *ip_list;
    const char *radio;
    const char *device_info;
    const char *temp_dir;
} opcd_paths_t;

#define OPC_PATH_BASE        "/usr/local/opc/etc"
#define OPC_PATH_CONF        OPC_PATH_BASE "/opc.conf"
#define OPC_PATH_PASSWORD    OPC_PATH_BASE "/password"
#define OPC_PATH_IPLIST      OPC_PATH_BASE "/iplist.cfg"
#define OPC_PATH_RADIO       OPC_PATH_BASE "/radio.conf"
#define OPC_PATH_DEVICE_INFO OPC_PATH_BASE "/device_info.json"
#define OPC_PATH_TEMP        OPC_PATH_BASE "/temp"

/* 128 IP-config slots. */
typedef struct opcd_ip_list {
    opc_ipcfg_entry_t slots[OPC_IPCFG_LIST_MAX_SLOTS];
    uint8_t present[OPC_IPCFG_LIST_MAX_SLOTS];   /* 1 = populated */
} opcd_ip_list_t;

/* Async NVRAM writer — opaque; see store_async.h. */
struct opc_store_async;

/* A Set* ack whose NVRAM write is still in flight (PERF-001 deferred ack).
 * One slot per queued store_async job; the job token is the slot index.
 * The ack is packed and sent from opcd_store_async_on_ready() once the
 * worker reports the write result. */
#define OPCD_PENDING_ACK_MAX 4
typedef struct opcd_pending_ack {
    bool     in_use;
    bool     discarded;     /* A19: superseded by a same-command retransmission
                             * — completion frees the slot without replying */
    uint16_t req_id;        /* OPC_REQ_* whose ack format to pack */
    uint16_t seq;           /* echoed sequence number */
    uint32_t client_ip;     /* host byte order */
    uint16_t client_port;   /* host byte order */
    struct timespec rx_ts;  /* request receipt (CLOCK_MONOTONIC) — T7
                             * served-in log on the deferred ack send */
} opcd_pending_ack_t;

/* Whole-daemon mutable state. */
typedef struct opcd_state {
    opcd_conf_t conf;
    opcd_paths_t paths;

    /* Login session — single, IP-bound. */
    bool     logged_in;
    uint32_t holder_ip;             /* host byte order */
    uint16_t holder_port;
    time_t   idle_deadline;         /* CLOCK_MONOTONIC seconds */

    /* Persistent app state. */
    char     password[128];
    opc_set_radio_config_req_t radio;
    opcd_ip_list_t ip_list;

    /* SetRadioConfig apply-failure revert, DEFERRED past the NG ack (D9): a
     * synchronous second apply could double the response wall-clock on a DUAL
     * timeout and blow the 1s budget (PR #53 review). The handler arms this on
     * apply failure — storing the last-good config to restore — and the main
     * loop re-applies it via opcd_radio_revert_drain() AFTER the NG ack is sent,
     * so the failure response is never delayed by the recovery apply. */
    bool     radio_revert_pending;
    opc_set_radio_config_req_t radio_revert_cfg;

    /* SetIpConfigList staging — accumulates entries until END boundary flag. */
    opcd_ip_list_t ip_list_staging;
    bool     ip_list_staging_active;

    /* ChangeIpAddress is deferred and committed ONLY by an explicit Logout (#43):
     * change-ip stages it (ip_change_pending); handle_logout snapshots the
     * resolved target entry (ip_change_armed_entry) and arms the commit
     * (ip_change_commit_armed) just before teardown; the main loop then applies
     * the snapshot after the Logout ack is sent. Snapshotting makes the armed
     * commit immune to any later list mutation (a same-drain next session cannot
     * rewrite what gets applied). Idle/abandon logout never arms, so the device
     * keeps its current IP; a fresh Login clears any inherited staging. */
    bool     ip_change_pending;
    bool     ip_change_commit_armed;
    uint16_t ip_change_list_no;
    uint16_t ip_change_armed_no;          /* slot # snapshotted at arm (audit log) */
    opc_ipcfg_entry_t ip_change_armed_entry;

    /* FaultDetect congestion probe — T6 interim policy (fault_probe.h). */
    opcd_fault_probe_t fault_probe;

    /* Indication target (volatile). */
    bool     indication_enabled;
    uint32_t indication_recipient_ip;
    uint16_t indication_recipient_port;
    uint8_t  indication_info_bits;
    uint8_t  indication_period_s;
    uint16_t indication_seq;
    int32_t  indication_tick_counter;   /* seconds since last KeepAlive */

    /* Device status as visible to GetBasicInfo (boot → ready → logged_in). */
    uint32_t boot_status;

    /* Async NVRAM persist. NULL → handlers write synchronously (unit tests,
     * or the daemon when the writer could not be created at startup). */
    struct opc_store_async *store_async;
    opcd_pending_ack_t pending_acks[OPCD_PENDING_ACK_MAX];

    /* Sockets / control. */
    int      udp_fd;
    bool     should_exit;
    bool     should_reset;          /* Reset handler sets this — main loop exits with 0 */
} opcd_state_t;

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_STATE_H */
