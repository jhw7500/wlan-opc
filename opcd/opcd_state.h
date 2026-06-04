#ifndef WLAN_OPC_OPCD_STATE_H
#define WLAN_OPC_OPCD_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../protocol/commands.h"
#include "../protocol/indications.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime configuration. Identity fields (vendor/product codes, hardware
 * revision, serial, dates, capability bits) used to live here; they now
 * live in inventory.h and are loaded from device_info.json at startup so
 * operators can edit them without rebuilding opcd. */
typedef struct opcd_conf {
    uint16_t udp_port;             /* default OPC_DEFAULT_UDP_PORT */
    uint16_t default_station_type; /* OPC_STATION_SINGLE / DUAL */
    uint32_t login_idle_s;         /* configurable for tests (default OPC_LOGIN_IDLE_S) */
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

    /* SetIpConfigList staging — accumulates entries until END boundary flag. */
    opcd_ip_list_t ip_list_staging;
    bool     ip_list_staging_active;

    /* ChangeIpAddress is deferred: applied after we send the Logout ack. */
    bool     ip_change_pending;
    uint16_t ip_change_list_no;

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

    /* Sockets / control. */
    int      udp_fd;
    bool     should_exit;
    bool     should_reset;          /* Reset handler sets this — main loop exits with 0 */
} opcd_state_t;

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_STATE_H */
