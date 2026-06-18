#ifndef WLAN_OPC_OPCD_NL80211_PARSE_H
#define WLAN_OPC_OPCD_NL80211_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure nl80211 netlink event parser. No sockets, no syscalls, no globals —
 * input is a raw netlink message buffer, output is a decoded intermediate
 * struct. platform_nxp.c owns the socket I/O, ifindex→idx mapping, and the
 * mapping of this struct into opcd_platform_evt_t. Splitting the byte parsing
 * out keeps it host-testable in `make check` (PLATFORM=stub), which never
 * compiles platform_nxp.c.
 *
 * Raw parsing only: no libnl, matching the project's "raw netlink, no libnl"
 * convention (same as mlanutl's direct ioctl path).
 */

typedef enum opcd_nl_kind {
    OPCD_NL_IGNORE = 0,
    OPCD_NL_CONNECT,
    OPCD_NL_DISCONNECT,
    OPCD_NL_ROAM,
    OPCD_NL_CH_SWITCH,
    OPCD_NL_INTERFACE,   /* NL80211_CMD_NEW_INTERFACE (GET_INTERFACE reply)          */
    OPCD_NL_DEAUTH,      /* NL80211_CMD_DEAUTHENTICATE — AP-sent deauth (Host MLME)  */
    OPCD_NL_DISASSOC,    /* NL80211_CMD_DISASSOCIATE — AP-sent disassoc (Host MLME)  */
} opcd_nl_kind_t;

typedef struct opcd_nl_evt {
    opcd_nl_kind_t kind;
    int      ifindex;       /* NL80211_ATTR_IFINDEX, 0 if absent           */
    uint32_t freq_mhz;      /* NL80211_ATTR_WIPHY_FREQ, 0 if absent        */
    uint16_t channel;       /* derived from freq_mhz (0 if absent/unknown) */
    uint8_t  mac[6];        /* NL80211_ATTR_MAC, zeroed if absent          */
    bool     mac_present;
    uint16_t reason_code;   /* NL80211_ATTR_REASON_CODE (DISCONNECT)       */
    uint16_t status_code;   /* NL80211_ATTR_STATUS_CODE (CONNECT)          */
    bool     by_ap;         /* NL80211_ATTR_DISCONNECTED_BY_AP (DISCONNECT) */
    char     ssid[33];      /* NL80211_ATTR_SSID, NUL-terminated; "" if absent (max 32B) */
    bool     ssid_present;
} opcd_nl_evt_t;

/*
 * Parse one netlink message. Returns 0 and fills *out for a recognized
 * nl80211 event whose generic-netlink family == family_id; returns -1 for
 * ignore (wrong family / unknown cmd / malformed / truncated). out->kind is
 * set to OPCD_NL_IGNORE on -1.
 *
 * Netlink is host byte order; all reads are host-endianness and use memcpy
 * into aligned locals (never misaligned pointer casts — ARM alignment).
 * Every read is bounds-checked against len.
 */
int nl80211_parse_evt(const uint8_t *msg, size_t len, int family_id,
                      opcd_nl_evt_t *out);

/*
 * Parse a generic-netlink CTRL (GENL_ID_CTRL) CTRL_CMD_NEWFAMILY reply for the
 * nl80211 family. Extracts the top-level CTRL_ATTR_FAMILY_ID (u16) and walks
 * the nested CTRL_ATTR_MCAST_GROUPS array to find the group named "mlme",
 * returning its CTRL_ATTR_MCAST_GRP_ID (u16).
 *
 * On success returns 0 and writes *family_id and *mlme_grp_id (both required;
 * pointers must be non-NULL). Returns -1 if either is absent, the message is
 * malformed/truncated, or args are NULL. Same defensive style as
 * nl80211_parse_evt: every read bounds-checked, memcpy for unaligned access,
 * netlink host byte order.
 */
int nl80211_parse_ctrl_family(const uint8_t *msg, size_t len,
                              uint16_t *family_id, uint16_t *mlme_grp_id);

/*
 * Build an NL80211_CMD_GET_INTERFACE request for `ifindex` into buf (host byte
 * order). Returns total length, or 0 if cap is too small. The reply is an
 * NL80211_CMD_NEW_INTERFACE message carrying NL80211_ATTR_WIPHY_FREQ, parsed by
 * nl80211_parse_evt (kind OPCD_NL_INTERFACE). Used for a direct kernel channel
 * query when a CONNECT event omits WIPHY_FREQ (link.json lags association).
 */
size_t nl80211_build_get_interface(uint8_t *buf, size_t cap, uint16_t family_id,
                                   uint32_t ifindex);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_NL80211_PARSE_H */
