/*
 * NXP 88W9098 platform backend.
 *
 * Reads runtime device state from JSON files maintained by the cantops
 * logger pipeline:
 *   /var/log/cantops/json/eth0/link.json   — ethernet mac / ip / netmask
 *   /var/log/cantops/json/mlan0/link.json  — wlan0 mac / ssid / channel / ...
 *   /var/log/cantops/json/mlan1/link.json  — wlan1 (when enabled)
 *
 * Design choice: no ioctl, no netlink, no mlanutl exec. The logger pipeline
 * already collects everything opcd needs, so this backend is a thin reader
 * over those files — matching the user's stated convention and avoiding new
 * runtime dependencies.
 *
 * First PR scope: get_eth_mac + get_eth_ipv4_host only. The remaining 16
 * vtable members are placeholders (zero/empty/canned) so behaviour parity
 * with platform_stub.c is preserved until subsequent PRs add WLAN identity,
 * link readback, mutation, and event multiplexing.
 *
 * No external JSON library: a small key-then-sscanf extractor handles the
 * flat top-level keys in eth0/link.json. nested-scope handling lands when
 * mlan0/link.json (with duplicate `address` keys in info/link) arrives.
 */

#define _GNU_SOURCE        /* pipe2(2), if_indextoname(3) */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>            /* if_indextoname, IF_NAMESIZE */
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/netlink.h>    /* struct sockaddr_nl, NETLINK_GENERIC, NETLINK_ADD_MEMBERSHIP, SOL_NETLINK */
#include <linux/genetlink.h>  /* GENL_ID_CTRL (referenced as a literal below) */

#include "platform.h"
#include "json_util.h"
#include "nl80211_parse.h"
#include "ntp_parse.h"
#include "../protocol/indications.h"   /* OPC_AP_MSG_DEAUTHENTICATION */

#define ETH0_LINK_JSON   "/var/log/cantops/json/eth0/link.json"
#define MLAN0_LINK_JSON  "/var/log/cantops/json/mlan0/link.json"
#define MLAN1_LINK_JSON  "/var/log/cantops/json/mlan1/link.json"
#define WIFI_SH          "/usr/local/scripts/wifi.sh"
/* platform.h requires a bounded short timeout for apply_radio_config.
 * Budget 900 ms (not 1000) leaves ~100 ms slack for inter-call overhead
 * in DUAL mode (per-call timeout = TIMEOUT_MS/2 = 450 ms × 2 = 900 ms). */
#define WIFI_SH_TIMEOUT_MS 900
#define WIFI_SH_POLL_MS    10

/* ChangeIpAddress replaces eth0's management IP at runtime via `ip addr`.
 * Verified on-target: systemd-networkd does NOT revert the change (a .network
 * /run override is instead reconciled away by wifi_init.sh, which owns eth0
 * routing), and wifi_init.sh's host-scope mlan IP /32 is left intact. Runtime
 * only — a reboot restores 22-eth0.network's Address, matching the spec's
 * "volatile, reverts on power cycle". Board-specific (eth0 + iproute2). */
#define IP_BIN_SH            "/bin/sh"
#define IP_CHANGE_TIMEOUT_MS 900

/* ---- nl80211 event socket (raw AF_NETLINK / NETLINK_GENERIC, no libnl) ----
 * ABI-stable genl constants, hardcoded so the build does not depend on the
 * host kernel headers carrying these (same rationale as nl80211_parse.c).
 * The pure byte parsing of both the family-resolution reply and the events
 * lives in nl80211_parse.c; this file owns only the socket I/O, the
 * ifindex→idx mapping, and the per-drain coalesce. */
#define NL_GENL_ID_CTRL         16   /* generic-netlink CTRL family id        */
#define NL_MSG_ERROR             2   /* nlmsghdr.nlmsg_type == NLMSG_ERROR    */
#define NL_CTRL_CMD_GETFAMILY    3
#define NL_CTRL_ATTR_FAMILY_NAME 2
#define NL_NLA_HDR_LEN           4   /* u16 nla_len + u16 nla_type            */
#define NL_GENLMSGHDR_LEN        4   /* u8 cmd, u8 version, u16 reserved      */
#define NL_FAMILY_NAME           "nl80211"
#define NL_MLME_GROUP            "mlme"
/* Bounded receive: one nl80211 multicast datagram is small (a few attrs);
 * 8 KiB comfortably holds a coalesced burst's largest single datagram. */
#define NL_RECV_BUF             8192
/* Per-drain coalesce table: collapse duplicate (kind, idx) to the last seen.
 * 3 nl80211-staged kinds × 2 interfaces = 6 distinct (kind,idx) slots; 8 is a
 * safe over-allocation. */
#define NL_COALESCE_MAX          8

/* netlink message-header field offsets (struct nlmsghdr is 16 bytes:
 * u32 len, u16 type, u16 flags, u32 seq, u32 pid).
 * NL_NLMSGHDR_LEN/NL_GENLMSGHDR_LEN deliberately mirror the same constants in
 * nl80211_parse.c — the duplication is intentional (host-header portability,
 * same rationale as the file header), not an accident. */
#define NL_NLMSGHDR_LEN         16

static int      g_nl_fd = -1;
static uint16_t g_nl80211_family_id;
static uint16_t g_mlme_grp_id;

/* Parse "-66 dBm" / "-66dBm" into an int8 (signed dBm). */
static int parse_signed_dbm(const char *s, int8_t *out)
{
    int v;
    if (sscanf(s, "%d", &v) != 1) return -EINVAL;
    if (v < INT8_MIN) v = INT8_MIN;
    if (v > INT8_MAX) v = INT8_MAX;
    *out = (int8_t)v;
    return 0;
}

/* Parse "20 MHz" / "40 MHz" / "80 MHz" / "160 MHz" to OPC_BANDWIDTH_*. */
static int parse_width_to_bw(const char *s, uint8_t *out)
{
    int mhz;
    if (sscanf(s, "%d", &mhz) != 1) return -EINVAL;
    switch (mhz) {
    case 20:  *out = OPC_BANDWIDTH_20;  return 0;
    case 40:  *out = OPC_BANDWIDTH_40;  return 0;
    case 80:  *out = OPC_BANDWIDTH_80;  return 0;
    case 160: *out = OPC_BANDWIDTH_160; return 0;
    case 320: *out = OPC_BANDWIDTH_320; return 0;
    default:  return -EINVAL;
    }
}

/* Parse iw-style bitrate string prefix to OPC_WLAN_MODE_*.
 *   "258.0 MBit/s HE-MCS 10 HE-NSS 2 ..."   → 11AX  (HE- prefix)
 *   "433.3 MBit/s VHT-MCS 9 ..."            → 11AC  (VHT- prefix)
 *   "144.4 MBit/s MCS 15 short GI ..."      → 11N   (iw emits no HT- prefix)
 *   plain "54 MBit/s"                       → legacy (caller leaves mode=0)
 *   "... EHT-MCS ..." (802.11be)            → -EINVAL: no OPC enum yet;
 *                                              caller falls back to cache.
 *
 * Order is defensive: in current iw output "HE-MCS" / "VHT-MCS" have no
 * space between "-" and "M", so the bare " MCS " check would not misfire
 * today. Checking the explicit HE-/VHT- prefixes first future-proofs
 * against driver variants that might emit a standalone " MCS " token
 * alongside HE/VHT fields. */
static int parse_bitrate_to_mode(const char *s, uint8_t *out)
{
    if (strstr(s, " HE-")  != NULL) { *out = OPC_WLAN_MODE_11AX; return 0; }
    if (strstr(s, " VHT-") != NULL) { *out = OPC_WLAN_MODE_11AC; return 0; }
    if (strstr(s, " MCS ") != NULL) { *out = OPC_WLAN_MODE_11N;  return 0; }
    return -EINVAL;
}

static int parse_mac_str(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -EINVAL;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return -EINVAL;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* nl80211 event socket setup (genl family resolution + mlme join)    */
/* ------------------------------------------------------------------ */

/* Build a CTRL_CMD_GETFAMILY request for the nl80211 family into buf and
 * return its total length, or 0 if buf is too small. Layout:
 *   nlmsghdr(16) + genlmsghdr(4) + nlattr(FAMILY_NAME, "nl80211\0").
 * All multi-byte fields are host byte order (netlink convention). */
static size_t nl_build_getfamily(uint8_t *buf, size_t cap)
{
    const char *name = NL_FAMILY_NAME;
    uint16_t name_len = (uint16_t)(strlen(name) + 1);       /* incl. NUL */
    uint16_t nla_len  = (uint16_t)(NL_NLA_HDR_LEN + name_len);
    uint16_t nla_pad  = (uint16_t)((nla_len + 3) & ~3u);    /* NLA_ALIGN */
    size_t   total    = NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN + nla_pad;
    if (cap < total) return 0;

    memset(buf, 0, total);
    uint32_t u32;
    uint16_t u16;

    /* nlmsghdr */
    u32 = (uint32_t)total;            memcpy(buf + 0,  &u32, 4);  /* nlmsg_len   */
    u16 = NL_GENL_ID_CTRL;            memcpy(buf + 4,  &u16, 2);  /* nlmsg_type  */
    u16 = NLM_F_REQUEST;              memcpy(buf + 6,  &u16, 2);  /* nlmsg_flags */
    /* seq (8) and pid (12) left zero — kernel does not require them here. */

    /* genlmsghdr */
    buf[16] = NL_CTRL_CMD_GETFAMILY;  /* cmd     */
    buf[17] = 1;                      /* version */
    /* reserved (18..19) zero */

    /* nlattr: CTRL_ATTR_FAMILY_NAME */
    size_t aoff = NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN;
    u16 = nla_len;                    memcpy(buf + aoff,     &u16, 2);
    u16 = NL_CTRL_ATTR_FAMILY_NAME;   memcpy(buf + aoff + 2, &u16, 2);
    memcpy(buf + aoff + NL_NLA_HDR_LEN, name, name_len);

    return total;
}

/* Resolve the nl80211 generic-netlink family id + the "mlme" multicast group
 * id over `fd`. Sends CTRL_CMD_GETFAMILY and parses the reply (bounded recv,
 * waits up to the socket's SO_RCVTIMEO). Returns 0 and fills the fam and grp
 * out-params on success, -1 otherwise (caller degrades to no-event). */
static int nl_resolve_family(int fd, uint16_t *fam, uint16_t *grp)
{
    uint8_t req[64];
    size_t  reqlen = nl_build_getfamily(req, sizeof req);
    if (reqlen == 0) return -1;

    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };  /* nl_pid=0 → kernel */
    ssize_t sn = sendto(fd, req, reqlen, 0,
                        (struct sockaddr *)&kernel, sizeof kernel);
    if (sn < 0 || (size_t)sn != reqlen) return -1;

    /* Blocking recv bounded by SO_RCVTIMEO (set by the caller). One datagram
     * carries the whole NEWFAMILY reply for nl80211. */
    uint8_t reply[NL_RECV_BUF];
    ssize_t rn = recv(fd, reply, sizeof reply, 0);
    if (rn < (ssize_t)(NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN)) return -1;

    /* A kernel rejection comes back as nlmsg_type == NLMSG_ERROR (the body is
     * a struct nlmsgerr, not a CTRL_CMD_NEWFAMILY reply). Detect it before the
     * parser, which would otherwise scan the error payload as garbage attrs. */
    uint16_t reply_type;
    memcpy(&reply_type, reply + 4, sizeof reply_type);  /* nlmsg_type @ off 4 */
    if (reply_type == NL_MSG_ERROR) {
        fprintf(stderr, "opcd: nl80211: CTRL GETFAMILY returned NLMSG_ERROR\n");
        return -1;
    }

    return nl80211_parse_ctrl_family(reply, (size_t)rn, fam, grp);
}

/* Open + configure the nl80211 event socket. On success g_nl_fd is a
 * non-blocking, mlme-joined NETLINK_GENERIC socket and the family/group ids
 * are populated. On ANY failure every partial resource is
 * released, g_nl_fd is left -1, and -1 is returned — opcd then runs exactly as
 * it did with the old no-op event_fd (no events, no degraded behaviour
 * elsewhere). Logs once on the failing step. */
static int nl_event_socket_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) {
        fprintf(stderr, "opcd: nl80211: socket failed: %s — events disabled\n",
                strerror(errno));
        return -1;
    }

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };  /* nl_pid=0 → kernel assigns */
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "opcd: nl80211: bind failed: %s — events disabled\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Bound the family-resolution recv so a silent kernel cannot wedge init().
     * 1 s is generous: CTRL replies are immediate on a live stack. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        /* Without the timeout the family-resolution recv could block forever
         * and wedge nxp_init at startup — fail closed like the bind above. */
        fprintf(stderr, "opcd: nl80211: set SO_RCVTIMEO failed: %s "
                        "— events disabled\n", strerror(errno));
        close(fd);
        return -1;
    }

    uint16_t fam = 0, grp = 0;
    if (nl_resolve_family(fd, &fam, &grp) != 0) {
        fprintf(stderr, "opcd: nl80211: family/mlme-group resolution failed "
                        "— events disabled\n");
        close(fd);
        return -1;
    }

    /* Join the mlme multicast group so DISCONNECT/CONNECT/ROAM/CH_SWITCH are
     * delivered to this socket. */
    int gid = (int)grp;
    if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &gid, sizeof gid) != 0) {
        fprintf(stderr, "opcd: nl80211: join mlme group %d failed: %s "
                        "— events disabled\n", gid, strerror(errno));
        close(fd);
        return -1;
    }

    /* Drain is poll-driven and must never block the opcd loop. */
    int fl = fcntl(fd, F_GETFL);
    if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        fprintf(stderr, "opcd: nl80211: set O_NONBLOCK failed: %s "
                        "— events disabled\n", strerror(errno));
        close(fd);
        return -1;
    }

    g_nl80211_family_id = fam;
    g_mlme_grp_id       = grp;
    g_nl_fd             = fd;
    fprintf(stderr, "opcd: nl80211 events live: family=%u mlme-grp=%u\n",
            fam, grp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int nxp_init(void)
{
    /* Best-effort nl80211 event socket. Failure degrades to no-event
     * operation (event_fd stays -1) and MUST NOT fail nxp_init — opcd still
     * serves the pull API and the polled FaultDetect probe. */
    (void)nl_event_socket_open();
    return 0;
}

static void nxp_teardown(void)
{
    /* Close the nl80211 socket if open. close() is on the POSIX async-signal-
     * safe list, so this stays safe for the SIGTERM/SIGINT teardown path.
     * Idempotent: guard on the fd and reset to -1. */
    if (g_nl_fd >= 0) {
        close(g_nl_fd);
        g_nl_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Identity — Ethernet (real, via eth0/link.json)                     */
/* ------------------------------------------------------------------ */

static int nxp_get_eth_mac(uint8_t mac[6])
{
    char *json = opc_json_slurp_file(ETH0_LINK_JSON);
    if (!json) { memset(mac, 0, 6); return -errno; }
    char buf[32] = {0};
    int rc = opc_json_string(json, "mac_address", buf, sizeof buf);
    free(json);
    if (rc != 0) { memset(mac, 0, 6); return rc; }
    return parse_mac_str(buf, mac);
}

/* Read one IPv4-typed field (ip_address/netmask/gateway) from eth0/link.json
 * as a host-order uint32_t. gateway is JSON null when unconfigured; the
 * string-matcher then returns -ENOENT and *out stays 0 — caller maps that
 * to "no gateway" per best-effort policy. */
static int nxp_get_eth_ipv4_field(const char *key, uint32_t *out_host)
{
    char *json = opc_json_slurp_file(ETH0_LINK_JSON);
    if (!json) { *out_host = 0; return -errno; }
    char buf[32] = {0};
    int rc = opc_json_string(json, key, buf, sizeof buf);
    free(json);
    if (rc != 0) { *out_host = 0; return rc; }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) { *out_host = 0; return -EINVAL; }
    *out_host = ntohl(a.s_addr);
    return 0;
}

static int nxp_get_eth_ipv4_host(uint32_t *ip_host)
{
    return nxp_get_eth_ipv4_field("ip_address", ip_host);
}

static int nxp_get_eth_netmask_host(uint32_t *netmask_host)
{
    return nxp_get_eth_ipv4_field("netmask", netmask_host);
}

static int nxp_get_eth_gateway_host(uint32_t *gateway_host)
{
    return nxp_get_eth_ipv4_field("gateway", gateway_host);
}

/* ------------------------------------------------------------------ */
/* Identity — WLAN (mlan0 / mlan1 via link.json)                      */
/* ------------------------------------------------------------------ */

static int nxp_get_wlan_count(void)
{
    /* mlan0 is always present in a wlan-opc target. mlan1 is created by the
     * cantops wifi_init pipeline only when the interface is enabled in
     * /usr/local/etc/wifi_init_conf.json — its link.json existence is the
     * canonical signal.
     *
     * platform.h contract: only ENOENT (mlan1 disabled) is a known-good
     * "single" result. Other access() errors (EACCES, EIO, ENOMEM, ...)
     * mean the platform genuinely cannot query — surface them so the boot
     * path treats them as fatal-at-startup. */
    if (access(MLAN1_LINK_JSON, F_OK) == 0) return 2;
    if (errno == ENOENT)                    return 1;
    return -errno;
}

static int nxp_get_wlan_mac(int idx, uint8_t mac[6])
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = opc_json_slurp_file(path);
    if (!json) { memset(mac, 0, 6); return -errno; }
    char buf[32] = {0};
    /* mlan link.json has `address` in two places (info, link); info holds
     * the interface MAC (the device's own), link holds the AP's MAC under
     * the same key. json_string_in_section disambiguates. */
    int rc = opc_json_string_section(json, "info", "address", buf, sizeof buf);
    free(json);
    if (rc != 0) { memset(mac, 0, 6); return rc; }
    return parse_mac_str(buf, mac);
}

static int nxp_get_essid(int idx, char *buf, size_t cap)
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    if (cap == 0) return -EINVAL;
    buf[0] = '\0';
    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = opc_json_slurp_file(path);
    if (!json) return -errno;
    int rc = opc_json_string_section(json, "info", "ssid", buf, cap);
    free(json);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Identity — Static (canned — pending hostcmd/EEPROM source)         */
/* ------------------------------------------------------------------ */

static int nxp_copy_string(char *buf, size_t cap, const char *src)
{
    if (cap == 0) return -EINVAL;
    size_t n = strnlen(src, cap - 1);
    memcpy(buf, src, n);
    buf[n] = '\0';
    return 0;
}

/* dpkg-query result cache. Populated lazily on the first
 * nxp_get_firmware_version() call after opcd boot. Empty string means
 * "not yet queried OR dpkg-query failed" — the caller cannot tell the
 * difference, which is fine because both yield the same empty wire field. */
#define WLAN_PROC_PKG       "wlan-proc"
#define DPKG_QUERY_BIN      "/usr/bin/dpkg-query"
#define FW_CACHE_LEN        OPC_VERSION_FIELD_LEN
static char  g_fw_cache[FW_CACHE_LEN];
static bool  g_fw_cache_ready;

/* Compare two monotonic timespecs; returns true if `a` is at or past `b`. */
static bool ts_at_or_past(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec  > b->tv_sec)  return true;
    if (a->tv_sec  < b->tv_sec)  return false;
    return a->tv_nsec >= b->tv_nsec;
}

/* Elapsed milliseconds from `start` to `end` (monotonic clock, C99
 * truncation-toward-zero — always non-negative when end >= start). */
static long timespec_diff_ms(const struct timespec *end,
                             const struct timespec *start)
{
    return (end->tv_sec  - start->tv_sec)  * 1000L
         + (end->tv_nsec - start->tv_nsec) / 1000000L;
}

/* Run `dpkg-query -W -f='${Version}' wlan-proc` once and copy stdout into
 * g_fw_cache (truncated to fit). Stays silent on every failure mode — the
 * empty cache string is the documented "unknown" response. fork+exec rather
 * than popen() so we can give the child a bounded close-on-exec pipe and
 * waitpid() it deterministically.
 *
 * Robustness: the parent never blocks on read(). pipefd[0] is set
 * non-blocking up front, and a single select()+read()+waitpid() loop
 * polls both the pipe and the child against a 2s monotonic deadline.
 * If the child wedges without ever closing stdout, the deadline triggers
 * SIGKILL — the original blocking read() loop could not reach the
 * waitpid timeout in that scenario. */
static void nxp_refresh_fw_cache(void)
{
    g_fw_cache_ready = true;
    g_fw_cache[0] = '\0';

    /* pipe2(O_CLOEXEC) closes both ends across any future exec() in opcd.
     * O_CLOEXEC on the *child's* write end is dropped by dup2() below,
     * which is the standard pattern for redirecting stdout to a pipe. */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        /* silence stderr — dpkg-query prints "no packages found matching"
         * to stderr when wlan-proc is absent (e.g. dev image), and that
         * noise should not pollute opcd's own log stream. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl(DPKG_QUERY_BIN, "dpkg-query",
              "-W", "-f=${Version}", WLAN_PROC_PKG, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(pipefd[1]);
    /* Non-blocking read: a child that wedges without writing must not
     * stall the parent inside read(2). Errors from F_SETFL are ignored —
     * the worst case is a blocking read, which the deadline below still
     * times out via SIGKILL on the child (closing the pipe and waking
     * read), but defence in depth is cheap. */
    int fl = fcntl(pipefd[0], F_GETFL);
    if (fl != -1) (void)fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;       /* 2s budget for cold dpkg DB */

    char buf[FW_CACHE_LEN];
    size_t off = 0;
    bool pipe_eof = false;
    int status = 0;
    bool child_reaped = false;
    bool killed = false;

    while (!(pipe_eof && child_reaped)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_at_or_past(&now, &deadline)) {
            if (!killed) { kill(pid, SIGKILL); killed = true; }
            /* After SIGKILL we still must reap to avoid a zombie; allow a
             * blocking waitpid because the child has been signalled. */
            if (!child_reaped) { (void)waitpid(pid, &status, 0); child_reaped = true; }
            break;
        }

        /* Drain whatever is ready on the pipe without blocking. */
        if (!pipe_eof && off + 1 < sizeof buf) {
            ssize_t n = read(pipefd[0], buf + off, sizeof buf - 1 - off);
            if (n > 0) {
                off += (size_t)n;
                continue;     /* prefer to drain before sleeping */
            }
            if (n == 0) pipe_eof = true;
            else if (errno != EAGAIN && errno != EINTR) pipe_eof = true;
        }
        /* Pipe full? Treat as EOF so we stop polling. */
        if (off + 1 >= sizeof buf) pipe_eof = true;

        if (!child_reaped) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid)      child_reaped = true;
            else if (r < 0)    { child_reaped = true; status = 0; }
        }
        if (pipe_eof && child_reaped) break;

        /* Short sleep, but cap to time remaining so we never overshoot the
         * deadline. 10 ms is a balance: short enough that dpkg-query (~tens
         * of ms warm) finishes in 1–2 ticks, long enough not to busy-spin. */
        struct timespec sl = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&sl, NULL);
    }
    close(pipefd[0]);

    if (killed) return;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return;
    if (off == 0) return;

    /* dpkg-query may end the version with a trailing newline depending on
     * stdio buffering — trim it. */
    while (off > 0 && (buf[off-1] == '\n' || buf[off-1] == '\r' ||
                       buf[off-1] == ' '  || buf[off-1] == '\t')) {
        off--;
    }
    if (off == 0) return;
    if (off >= sizeof g_fw_cache) off = sizeof g_fw_cache - 1;
    memcpy(g_fw_cache, buf, off);
    g_fw_cache[off] = '\0';
}

static int nxp_get_firmware_version(char *buf, size_t cap)
{
    if (!g_fw_cache_ready) nxp_refresh_fw_cache();
    return nxp_copy_string(buf, cap, g_fw_cache);
}

#define TIMESYNCD_CONF  "/etc/systemd/timesyncd.conf"

static int nxp_get_ntp_server(uint32_t *server_host)
{
    *server_host = 0;
    char *body = opc_json_slurp_file(TIMESYNCD_CONF);
    /* opc_json_slurp_file is misnamed for the use case but does exactly
     * what we want: read the whole file into a heap buffer with a 1MB cap.
     * timesyncd.conf is well under 4KB. */
    if (!body) return -errno;

    int rc = -ENOENT;
    char *save = NULL;
    for (char *ln = strtok_r(body, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        if (opc_ntp_parse_line(ln, server_host) == 0) { rc = 0; break; }
    }
    free(body);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Link / Mutation / Event — placeholders                             */
/* ------------------------------------------------------------------ */

static int nxp_get_link(int idx, opcd_platform_link_t *out)
{
    if (idx < 0 || idx >= nxp_get_wlan_count()) return -ENODEV;
    memset(out, 0, sizeof *out);

    const char *path = (idx == 0) ? MLAN0_LINK_JSON : MLAN1_LINK_JSON;
    char *json = opc_json_slurp_file(path);
    if (!json) return -errno;

    char buf[64] = {0};
    long ival = 0;

    /* info.freq / info.channel — operating frequency/channel */
    if (opc_json_integer_section(json, "info", "freq", &ival) == 0 &&
        ival > 0 && ival <= UINT16_MAX) {
        out->freq_mhz = (uint16_t)ival;
    }
    if (opc_json_integer_section(json, "info", "channel", &ival) == 0 &&
        ival >= 0 && ival <= UINT16_MAX) {
        out->channel = (uint16_t)ival;
    }

    /* link.address — AP MAC. Presence also signals associated. */
    if (opc_json_string_section(json, "link", "address", buf, sizeof buf) == 0 &&
        parse_mac_str(buf, out->bssid) == 0) {
        out->associated = true;
    }

    /* info.width → OPC_BANDWIDTH_*. Parsed only when associated so a stale
     * driver-cached width cannot leak bandwidth_valid=true while we report
     * associated=false. Only set on successful parse — OPC_BANDWIDTH_20==0
     * collides with the zero-init default. */
    if (out->associated &&
        opc_json_string_section(json, "info", "width", buf, sizeof buf) == 0) {
        uint8_t bw;
        if (parse_width_to_bw(buf, &bw) == 0) {
            out->bandwidth = bw;
            out->bandwidth_valid = true;
        }
    }

    /* link.tx_bitrate prefix → OPC_WLAN_MODE_*. Parsed only when associated
     * so a stale driver bitrate from a prior session cannot leak a non-zero
     * mode while we report associated=false. Falls back to rx_bitrate. */
    if (out->associated &&
        opc_json_string_section(json, "link", "tx_bitrate", buf, sizeof buf) == 0) {
        uint8_t mode;
        if (parse_bitrate_to_mode(buf, &mode) == 0) out->mode = mode;
    }
    if (out->associated && out->mode == 0 &&
        opc_json_string_section(json, "link", "rx_bitrate", buf, sizeof buf) == 0) {
        uint8_t mode;
        if (parse_bitrate_to_mode(buf, &mode) == 0) out->mode = mode;
    }

    /* link.signal_avg "-66 dBm" → rssi */
    if (opc_json_string_section(json, "link", "signal_avg", buf, sizeof buf) == 0) {
        (void)parse_signed_dbm(buf, &out->rssi);
    }

    /* channel_info.<freq>.noise → snr = rssi - noise.
     * channel_info is keyed by the operating freq (dynamic), but `noise`
     * is unique within channel_info for the active channel. The nested
     * search walks into the freq sub-object via brace depth. */
    if (out->associated &&
        opc_json_integer_section(json, "channel_info", "noise", &ival) == 0 &&
        ival >= INT8_MIN && ival < 0) {   /* 0 = uninitialized driver field */
        int snr = (int)out->rssi - (int)ival;
        if (snr < INT8_MIN) snr = INT8_MIN;
        if (snr > INT8_MAX) snr = INT8_MAX;
        out->snr = (int8_t)snr;
    }

    free(json);
    return 0;
}

/* Run argv[] (path = absolute binary, argv[0] = program name) to completion
 * with a bounded wait: poll waitpid(WNOHANG) against a monotonic deadline and
 * SIGKILL+reap on overrun, so a stalled child (SD/NFS fsync) cannot exceed the
 * OPC 1-second response budget. `label` tags diagnostics. Returns 0 on child
 * exit status 0, -errno otherwise (platform.h contract). */
static int run_argv_bounded(const char *label, const char *path,
                            char *const argv[], long timeout_ms)
{
    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        fprintf(stderr, "opcd: %s: fork failed: %s\n", label, strerror(err));
        return -err;
    }
    if (pid == 0) {
        execv(path, argv);
        /* execv returned: fprintf is not async-signal-safe post-fork.
         * Diagnose via write() so the parent's "status=0x7f00" is
         * distinguishable from the child itself returning 127. */
        static const char pfx[] = "opcd: run_argv_bounded: execv failed: ";
        (void)!write(STDERR_FILENO, pfx, sizeof pfx - 1);
        (void)!write(STDERR_FILENO, path, strlen(path));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    /* Bounded wait: poll with WNOHANG + monotonic deadline. The forked tools
     * are normally ms-fast, but backing-store stalls (SD card / NFS fsync)
     * could block indefinitely. platform.h's "bounded short timeout" contract
     * requires we cap and SIGKILL on overrun. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            /* opcd installs no SIGCHLD handler / SIG_IGN, so this is
             * unreachable for our own forked pid under the current model.
             * Still SIGKILL+reap defensively so a future signal-handling
             * change cannot cause this branch to leak the child. */
            int err = errno;
            fprintf(stderr, "opcd: %s: waitpid failed: %s\n", label, strerror(err));
            kill(pid, SIGKILL);
            pid_t wr;
            while ((wr = waitpid(pid, &status, 0)) < 0 && errno == EINTR) { }
            if (wr < 0)
                fprintf(stderr, "opcd: %s: defensive reap failed: %s\n", label, strerror(errno));
            return -err;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = timespec_diff_ms(&now, &start);
        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr, "opcd: %s: '%s' timed out after %ldms, sending SIGKILL\n",
                    label, path, elapsed_ms);
            kill(pid, SIGKILL);
            /* Reap so the SIGKILL'd child is not left as a zombie. */
            pid_t wr;
            while ((wr = waitpid(pid, &status, 0)) < 0 && errno == EINTR) { }
            if (wr < 0)
                fprintf(stderr, "opcd: %s: waitpid reap after SIGKILL failed: %s\n",
                        label, strerror(errno));
            return -ETIMEDOUT;
        }
        /* Cap the sleep so the last poll never overruns the deadline.
         * Without this, child can live up to timeout_ms + POLL_MS. */
        long remain_ms = timeout_ms - elapsed_ms;
        long sleep_ms  = (remain_ms < WIFI_SH_POLL_MS) ? remain_ms : WIFI_SH_POLL_MS;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = sleep_ms * 1000L * 1000L };
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "opcd: %s: '%s' failed (status=0x%x)\n",
                label, path, (unsigned)status);
        return -EPROTO;
    }
    return 0;
}

/* Run "wifi.sh <iface> freq <mhz>" synchronously. wifi.sh only rewrites
 * scan_freq= / freq_list= in wpa_supplicant-<iface>.conf — fast (~ms) and
 * comfortably within the OPC 1-second response budget. wpa_supplicant
 * restart is intentionally NOT triggered here so the active link is not
 * dropped on every Set-Radio; the operator triggers reconnect explicitly. */
static int run_wifi_sh_freq(const char *iface, uint16_t freq_mhz, long timeout_ms)
{
    char freq_buf[8];
    snprintf(freq_buf, sizeof freq_buf, "%u", freq_mhz);
    /* const char *[] + cast: execv never writes argv, and string literals must
     * not be exposed as char* (C11 6.4.5p7). */
    const char *argv[] = { "wifi.sh", iface, "freq", freq_buf, NULL };
    return run_argv_bounded("nxp_apply_radio_config", WIFI_SH,
                            (char *const *)argv, timeout_ms);
}

static int nxp_apply_radio_config(const opc_set_radio_config_req_t *cfg)
{
    if (cfg->station_type == OPC_STATION_DUAL) {
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: station=DUAL priority_ch=%u "
                "w1(freq=%u ch=0x%04x mode=%u bw=%u) "
                "w2(freq=%u ch=0x%04x mode=%u bw=%u)\n",
                cfg->priority_ch,
                cfg->wlan1.freq_mhz, cfg->wlan1.channel,
                cfg->wlan1.mode, cfg->wlan1.bandwidth,
                cfg->wlan2.freq_mhz, cfg->wlan2.channel,
                cfg->wlan2.mode, cfg->wlan2.bandwidth);
    } else {
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: station=SINGLE priority_ch=%u "
                "w1(freq=%u ch=0x%04x mode=%u bw=%u)\n",
                cfg->priority_ch,
                cfg->wlan1.freq_mhz, cfg->wlan1.channel,
                cfg->wlan1.mode, cfg->wlan1.bandwidth);
    }

    /* Share the 1s regulation budget across both apply calls in DUAL so the
     * worst-case wall-clock stays within platform.h's contract. */
    const long per_call_ms = (cfg->station_type == OPC_STATION_DUAL)
                             ? WIFI_SH_TIMEOUT_MS / 2
                             : WIFI_SH_TIMEOUT_MS;

    /* freq_mhz == 0 means "no association" per OPC spec — skip apply, leave
     * wpa_supplicant.conf untouched. Mode / bandwidth mapping is deferred to
     * a follow-up PR; this PR only wires the wpa_supplicant freq list. */
    if (cfg->wlan1.freq_mhz != 0) {
        int rc = run_wifi_sh_freq("mlan0", cfg->wlan1.freq_mhz, per_call_ms);
        if (rc != 0) return rc;
    }
    if (cfg->station_type == OPC_STATION_DUAL && cfg->wlan2.freq_mhz != 0) {
        /* Make the partial-apply state visible in the journal so a post-NG
         * triage shows whether mlan0 was already reconfigured.
         * Ternary safe: an mlan0 failure would have early-returned above,
         * so wlan1.freq_mhz==0 here means an intentional skip, not error. */
        fprintf(stderr,
                "opcd: nxp_apply_radio_config: %s; now applying mlan1 freq=%u\n",
                cfg->wlan1.freq_mhz != 0 ? "mlan0 freq already applied"
                                         : "mlan0 skipped (freq=0)",
                cfg->wlan2.freq_mhz);
        /* DUAL partial-apply risk: if mlan0 succeeded above, an mlan1
         * failure here leaves the two wpa_supplicant confs out of sync.
         * Caller gets NG (0x0011, frequency NG — D9); recovery is the
         * operator re-issuing Set-Radio. End-to-end idempotency is the
         * reconnect PR's job. */
        int rc = run_wifi_sh_freq("mlan1", cfg->wlan2.freq_mhz, per_call_ms);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Portable population count — avoids the GCC __builtin_popcount extension
 * (this is opcd's only bit-count site). */
static int count_set_bits(uint32_t v)
{
    int n = 0;
    for (; v; v >>= 1) n += (int)(v & 1u);
    return n;
}

/* Apply a committed IP-config slot to eth0's management IP at runtime.
 *
 * eth0 routing on this board is owned by wifi_init.sh (it assigns the mlan0 IP
 * as a host-scope /32 and manages a table-100 policy rule, actively overriding
 * 22-eth0.network). A systemd-networkd override therefore does not stick, but a
 * direct `ip addr` change does (verified on-target: networkd does not revert it
 * and the new address survives). Runtime only, so a reboot restores
 * 22-eth0.network's Address — the spec's "volatile, reverts on power cycle".
 *
 * We delete every scope-global, non-/32 IPv4 on eth0 (the management address;
 * wifi_init.sh's /32 is intentionally kept) and add the new one, in a single
 * /bin/sh pipeline. gateway/essid/ntp are not applied: gateway is a non-goal
 * — the board operates as a bridge on the control network, so no L3 gateway
 * is needed (user decision 2026-06-12, #27); the stored gateway field is
 * validated (D1: inside the entry's subnet) and echoed but never applied.
 * essid/ntp remain V3 on-target work (wifi_init.sh routing / wpa_supplicant). */
static int nxp_apply_ip_change(const opc_ipcfg_entry_t *slot)
{
    /* Reject a non-contiguous or empty netmask — cannot be a /prefix. */
    int prefix = count_set_bits(slot->subnet_mask);
    uint32_t recon = (prefix == 0) ? 0u : (uint32_t)(0xFFFFFFFFu << (32 - prefix));
    if (prefix < 1 || prefix > 32 || recon != slot->subnet_mask) {
        fprintf(stderr, "opcd: nxp_apply_ip_change: non-contiguous netmask 0x%08x\n",
                slot->subnet_mask);
        return -EINVAL;
    }

    /* Reject non-unicast targets BEFORE touching eth0: deleting the current
     * address and then failing to add a 0.0.0.0 / broadcast / multicast one
     * would leave eth0 with no management IP. */
    uint32_t ip = slot->ip_address;
    uint8_t  hi = (uint8_t)((ip >> 24) & 0xff);
    if (ip == 0 || ip == 0xFFFFFFFFu || hi == 127 || hi >= 224) {  /* 0/loopback/bcast/mcast+ */
        fprintf(stderr, "opcd: nxp_apply_ip_change: non-unicast target 0x%08x\n", ip);
        return -EINVAL;
    }

    char ipbuf[16];
    snprintf(ipbuf, sizeof ipbuf, "%u.%u.%u.%u",
             (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);

    /* add BEFORE delete so a failed add leaves the current management IP in
     * place (delete-then-add could strand eth0 with no management address, with
     * no recovery short of reboot). On add success, remove every OTHER
     * scope-global non-/32 address — keep=new and wifi_init.sh's /32 are
     * excluded. ipbuf is digits-and-dots and prefix is 1..32 — injection-safe. */
    char cmd[320];
    int n = snprintf(cmd, sizeof cmd,
        "ip addr add %s/%d dev eth0 && "
        "ip -4 addr show dev eth0 | "
        "awk -v keep=%s/%d '/scope global/&&!/\\/32/&&$2!=keep{print $2}' | "
        "xargs -r -I{} ip addr del {} dev eth0", ipbuf, prefix, ipbuf, prefix);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        fprintf(stderr, "opcd: nxp_apply_ip_change: command too long\n");
        return -ENAMETOOLONG;
    }

    /* const char *[] + cast: execv never writes argv, and cmd/literals must not
     * be exposed as char* (C11 6.4.5p7). */
    const char *argv[] = { "sh", "-c", cmd, NULL };
    int ret = run_argv_bounded("nxp_apply_ip_change", IP_BIN_SH,
                               (char *const *)argv, IP_CHANGE_TIMEOUT_MS);
    fprintf(stderr, "opcd: nxp_apply_ip_change: eth0 → %s/%d%s\n", ipbuf, prefix,
            ret == 0 ? " (ip addr)" : " (FAILED)");
    return ret;
}

static int nxp_prepare_reset(void)
{
    return 0;
}

static int nxp_event_fd(void)
{
    return g_nl_fd;
}

/* Map a (truthful) ifindex to the platform idx (0=mlan0, 1=mlan1), or -1 for
 * an interface we do not track. The mlan0-only interim policy is NOT applied
 * here — it belongs to the consumer (opcd.c on_platform_event) so a policy
 * change does not touch the producer.
 *
 * Resolved dynamically per event: mlan0/mlan1 can restart with a new ifindex
 * on this board (the wifi services restart the interfaces), so a cached
 * init-time ifindex would go stale. Events are infrequent, so the per-event
 * if_indextoname() syscall cost is negligible. */
static int nl_ifindex_to_idx(int ifindex)
{
    if (ifindex <= 0) return -1;
    char name[IF_NAMESIZE];
    if (if_indextoname((unsigned)ifindex, name)) {
        if (!strcmp(name, "mlan0")) return 0;
        if (!strcmp(name, "mlan1")) return 1;
    }
    return -1;
}

/* Extract the platform idx (0=mlan0, 1=mlan1) from an event's per-kind union. */
static uint8_t coalesce_idx(const opcd_platform_evt_t *e)
{
    switch (e->kind) {
    case OPCD_PEVT_WLAN_STATUS:   return e->u.wlan_status.idx;
    case OPCD_PEVT_ROAMING:       return e->u.roaming.idx;
    case OPCD_PEVT_AP_DISCONNECT: return e->u.ap_disconnect.idx;
    default:                      return 0;
    }
}

/* Replace-or-append the latest event for its (kind, idx) into the coalesce
 * table. idx is read from the evt union per kind. Table is bounded at
 * NL_COALESCE_MAX; never overflows in practice. */
static void nl_coalesce_put(opcd_platform_evt_t *tab, size_t *count,
                            const opcd_platform_evt_t *evt)
{
    uint8_t idx = coalesce_idx(evt);

    for (size_t i = 0; i < *count; i++) {
        if (tab[i].kind != evt->kind)
            continue;
        if (coalesce_idx(&tab[i]) == idx) {  /* same (kind, idx) — keep latest */
            tab[i] = *evt;
            return;
        }
    }
    if (*count < NL_COALESCE_MAX) {
        tab[*count] = *evt;
        (*count)++;
    } else {
        /* Table full — should never happen (6 distinct slots, table is 8), but
         * a future kind/iface addition could overflow. Log once so a silent
         * drop surfaces instead of vanishing. */
        static bool overflow_logged = false;
        if (!overflow_logged) {
            fprintf(stderr, "opcd: nl80211: coalesce table full "
                            "(event dropped)\n");
            overflow_logged = true;
        }
    }
}

/* Encode the OPC indication channel field: upper byte = OPC band, lower byte =
 * channel number (protocol/indications.h: indication_ch / ch_number). The band
 * is derived from the parsed frequency. Channel 0 (no association / unknown)
 * maps to a bare 0 with no band. Producer-side mapping keeps nl80211_parse.c
 * OPC-agnostic. */
static uint16_t opc_chan_field(uint32_t freq_mhz, uint16_t ch)
{
    if (ch == 0) return 0;            /* unknown channel → 0 (no band) */
    uint8_t band = 0;
    if (freq_mhz >= 2412 && freq_mhz <= 2484)      band = OPC_BAND_2_4GHZ;
    else if (freq_mhz >= 5000 && freq_mhz <= 5895) band = OPC_BAND_5GHZ;
    else if (freq_mhz >= 5955 && freq_mhz <= 7115) band = OPC_BAND_6GHZ;
    return (uint16_t)((uint16_t)band << 8) | ch;
}

/* Translate one decoded nl80211 event into 0..2 platform events and stage them
 * into the coalesce table. DISCONNECT emits AP_DISCONNECT (only when AP-
 * initiated) followed by WLAN_STATUS(UP); the others emit a single event. */
static void nl_stage_evt(opcd_platform_evt_t *tab, size_t *count,
                         const opcd_nl_evt_t *nev, int idx)
{
    opcd_platform_evt_t pe;
    switch (nev->kind) {
    case OPCD_NL_CONNECT:
        /* NL80211_CMD_CONNECT fires on BOTH success and failure; a non-zero
         * 802.11 status_code is a failed/timed-out association — do NOT stage
         * a CONNECTED status for it. */
        if (nev->status_code != 0) break;
        memset(&pe, 0, sizeof pe);
        pe.kind                = OPCD_PEVT_WLAN_STATUS;
        pe.u.wlan_status.idx   = (uint8_t)idx;
        pe.u.wlan_status.status  = OPCD_WLAN_STATUS_ASSOCIATED;
        pe.u.wlan_status.channel = opc_chan_field(nev->freq_mhz, nev->channel);
        nl_coalesce_put(tab, count, &pe);
        break;

    case OPCD_NL_DISCONNECT:
        /* nl80211 emits CMD_DISCONNECT for both local/client-initiated and
         * AP-initiated drops. Only the AP-initiated case is a genuine AP
         * deauth/disassoc indication — gate AP_DISCONNECT on the parsed
         * NL80211_ATTR_DISCONNECTED_BY_AP flag (nev->by_ap). A local disconnect
         * stages only the WLAN_STATUS(UP) transition below, no spurious deauth.
         *
         * AP_DISCONNECT carries the 802.11 reason code + AP MAC. The OPC
         * "Message ID" (Disassociation 0x000A / Deauthentication 0x000C) is
         * not distinguishable from the parsed attrs (the raw 802.11 frame is
         * not decoded), so we report Deauthentication — the dominant case. */
        if (nev->by_ap) {
            memset(&pe, 0, sizeof pe);
            pe.kind                       = OPCD_PEVT_AP_DISCONNECT;
            pe.u.ap_disconnect.idx        = (uint8_t)idx;
            pe.u.ap_disconnect.reason_msg_id = OPC_AP_MSG_DEAUTHENTICATION;
            pe.u.ap_disconnect.result_code   = nev->reason_code;
            memcpy(pe.u.ap_disconnect.mac, nev->mac, 6);
            nl_coalesce_put(tab, count, &pe);
        }

        /* Then the link returns to UP (associated→up), channel cleared.
         * OPCD_WLAN_STATUS_UP = "link up, awaiting association" (platform.h:83),
         * the correct post-disconnect state; the consumer maps it to wire
         * DISCONNECTED. */
        memset(&pe, 0, sizeof pe);
        pe.kind                  = OPCD_PEVT_WLAN_STATUS;
        pe.u.wlan_status.idx     = (uint8_t)idx;
        pe.u.wlan_status.status  = OPCD_WLAN_STATUS_UP;
        pe.u.wlan_status.channel = 0;
        nl_coalesce_put(tab, count, &pe);
        break;

    case OPCD_NL_ROAM: {
        /* nl80211 ROAM carries no SNR/RSSI — seed from the cached link
         * readback (link.json). On failure the fields stay 0 (best-effort). */
        opcd_platform_link_t link;
        memset(&link, 0, sizeof link);
        (void)nxp_get_link(idx, &link);

        memset(&pe, 0, sizeof pe);
        pe.kind              = OPCD_PEVT_ROAMING;
        pe.u.roaming.idx     = (uint8_t)idx;
        pe.u.roaming.snr     = link.snr;
        pe.u.roaming.rssi    = link.rssi;
        pe.u.roaming.channel = opc_chan_field(nev->freq_mhz, nev->channel);
        memcpy(pe.u.roaming.mac, nev->mac, 6);
        nl_coalesce_put(tab, count, &pe);
        break;
    }

    case OPCD_NL_CH_SWITCH:
        memset(&pe, 0, sizeof pe);
        pe.kind                  = OPCD_PEVT_WLAN_STATUS;
        pe.u.wlan_status.idx     = (uint8_t)idx;
        pe.u.wlan_status.status  = OPCD_WLAN_STATUS_CHANNEL_CHANGE;
        pe.u.wlan_status.channel = opc_chan_field(nev->freq_mhz, nev->channel);
        nl_coalesce_put(tab, count, &pe);
        break;

    case OPCD_NL_IGNORE:
    default:
        break;
    }
}

static int nxp_drain_events(opcd_platform_evt_cb cb, void *ctx)
{
    if (g_nl_fd < 0) return 0;

    opcd_platform_evt_t tab[NL_COALESCE_MAX];
    memset(tab, 0, sizeof tab);
    size_t count = 0;
    static bool enobufs_logged = false;

    uint8_t buf[NL_RECV_BUF];
    for (;;) {
        ssize_t n = recv(g_nl_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                       /* nothing more queued */
            if (errno == EINTR)
                continue;                    /* retry */
            if (errno == ENOBUFS) {
                /* Socket buffer overrun: events were lost. Acceptable — link
                 * state re-syncs on the next event. Log once, keep draining. */
                if (!enobufs_logged) {
                    fprintf(stderr, "opcd: nl80211: recv ENOBUFS "
                                    "(event loss; will re-sync)\n");
                    enobufs_logged = true;
                }
                continue;
            }
            /* Other errors: log and stop this drain. Do NOT return <0 — that
             * makes opcd permanently DEL the fd; the socket is not proven
             * dead by a transient errno. */
            fprintf(stderr, "opcd: nl80211: recv failed: %s\n", strerror(errno));
            break;
        }
        if (n == 0)
            break;                           /* zero-length datagram: no
                                              * nlmsghdr to parse. EAGAIN above
                                              * is the real "queue empty" signal,
                                              * so a 0 return is simply skipped. */

        /* A datagram may pack multiple nlmsghdrs — iterate them by NLMSG_ALIGN.
         * Each is bounds-checked: a header that does not fit, or a nlmsg_len
         * outside [HDR, remaining], stops the walk for this datagram. */
        size_t off = 0;
        while (off + NL_NLMSGHDR_LEN <= (size_t)n) {
            uint32_t msglen;
            memcpy(&msglen, buf + off, 4);         /* nlmsg_len (host order) */
            if (msglen < NL_NLMSGHDR_LEN || (size_t)msglen > (size_t)n - off)
                break;

            opcd_nl_evt_t nev;
            if (nl80211_parse_evt(buf + off, msglen,
                                  (int)g_nl80211_family_id, &nev) == 0) {
                int idx = nl_ifindex_to_idx(nev.ifindex);
                if (idx >= 0)
                    nl_stage_evt(tab, &count, &nev, idx);
            }

            size_t advance = (size_t)((msglen + 3u) & ~3u);  /* NLMSG_ALIGN */
            if (advance == 0 || off + advance <= off)
                break;
            off += advance;
        }
    }

    /* Deliver the coalesced set. Respect cb early-stop (>0). */
    for (size_t i = 0; i < count; i++) {
        int rc = cb(&tab[i], ctx);
        if (rc > 0)
            return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* vtable + registration                                              */
/* ------------------------------------------------------------------ */

static const opcd_platform_ops_t g_nxp_ops = {
    .init                  = nxp_init,
    .teardown              = nxp_teardown,
    .get_eth_mac           = nxp_get_eth_mac,
    .get_eth_ipv4_host     = nxp_get_eth_ipv4_host,
    .get_eth_netmask_host  = nxp_get_eth_netmask_host,
    .get_eth_gateway_host  = nxp_get_eth_gateway_host,
    .get_wlan_mac          = nxp_get_wlan_mac,
    .get_essid             = nxp_get_essid,
    .get_firmware_version  = nxp_get_firmware_version,
    .get_ntp_server        = nxp_get_ntp_server,
    .get_wlan_count        = nxp_get_wlan_count,
    .get_link              = nxp_get_link,
    .apply_radio_config    = nxp_apply_radio_config,
    .apply_ip_change       = nxp_apply_ip_change,
    .prepare_reset         = nxp_prepare_reset,
    .event_fd              = nxp_event_fd,
    .drain_events          = nxp_drain_events,
};

static const opcd_platform_ops_t *g_ops;

const opcd_platform_ops_t *opcd_platform(void)
{
    return g_ops;
}

void opcd_platform_register(void)
{
    /* Per platform.h: surface double-register as abort, not silent clobber.
     * Explicit check (not assert) survives -DNDEBUG. */
    if (g_ops != NULL) abort();
    g_ops = &g_nxp_ops;
}
