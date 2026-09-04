/*
 * opcd — OPC-side UDP/IP control daemon for the VHL ↔ wireless-board protocol.
 *
 * Single-threaded epoll loop driving:
 *   - UDP socket on /usr/local/opc/etc/opc.conf::udp_port  (default 50607)
 *   - signalfd  for SIGINT / SIGTERM      → graceful shutdown
 *   - timerfd   1 s tick                  → indication period & idle check
 *   - eventfd   async NVRAM completions   → deferred Set* acks (PERF-001)
 *
 * State persists in /usr/local/opc/etc/{password, iplist.cfg, radio.conf}
 * via atomic temp+rename writes (see store.c), queued onto the store_async
 * worker thread so the fsync stall never blocks the loop (store_async.c).
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../protocol/codec.h"
#include "../protocol/indications.h"   /* OPC_WLAN_STATUS_CONNECTED/DISCONNECTED */
#include "chan_encode.h"
#include "handler.h"
#include "indication.h"
#include "inventory.h"
#include "json_util.h"
#include "opcd_log.h"
#include "reset_cause.h"
#include "opcd_state.h"
#include "platform.h"
#include "roam_datagram.h"
#include "roam_notify_conf.h"
#include "snapshot.h"
#include "store.h"
#include "store_async.h"

#define LOG(fmt, ...) fprintf(stderr, "opcd: " fmt "\n", ##__VA_ARGS__)

/* Cached teardown function pointer for AS-safe signal-handler access — see
 * platform.h. Although opcd uses signalfd (not raw handlers) today, this
 * keeps the contract honest for future hot paths. */
static void (*g_teardown)(void);

/* Drain callback — translates a platform event to the corresponding
 * indication frame. Returns 0 to keep draining (platform.h contract:
 * negative reserved for drain-layer failure, positive for early-stop).
 *
 * The evt union carries an `idx` field (0=mlan0, 1=mlan1) for wlan_status /
 * roaming / ap_disconnect, but the OPC indication spec (Rev 1.00 KO §3.4)
 * has no wlan_id field in any of these frames, so idx is intentionally not
 * propagated — VHL cannot distinguish wlan1 from wlan2 events at the
 * protocol level. This is a spec limitation, not an opcd routing bug.
 * Interim policy (2026-06-12 user decision, customer inquiry in issue #35):
 * until the spec grows a wlan_id, indications follow the primary WLAN —
 * the nl80211 integration (V1) should emit idx==0 (mlan0) events only. */
/* Translate an internal OPCD_WLAN_STATUS_* (platform.h) to the OPC wire enum
 * (protocol/indications.h), which has only two values. ASSOCIATED and
 * CHANNEL_CHANGE both mean "still associated" → CONNECTED; UP ("link up,
 * awaiting association") and DOWN both mean not associated → DISCONNECTED. */
static uint16_t wlan_status_to_wire(uint16_t internal)
{
    switch (internal) {
    case OPCD_WLAN_STATUS_ASSOCIATED:
    case OPCD_WLAN_STATUS_CHANNEL_CHANGE:
        return OPC_WLAN_STATUS_CONNECTED;
    case OPCD_WLAN_STATUS_UP:
    case OPCD_WLAN_STATUS_DOWN:
    default:
        return OPC_WLAN_STATUS_DISCONNECTED;
    }
}

static int on_platform_event(const opcd_platform_evt_t *evt, void *ctx)
{
    opcd_state_t *st = ctx;
    switch (evt->kind) {
    case OPCD_PEVT_INIT_COMPLETE:
        opcd_ind_init_complete(st, evt->u.init_complete.status);
        break;
    case OPCD_PEVT_WLAN_STATUS:
        /* mlan0-only interim policy (#35 item 6): drop mlan1 events until the
         * OPC spec grows a wlan_id field — see the function header note. */
        if (evt->u.wlan_status.idx != 0) return 0;
        opcd_ind_wlan_status(st,
                             wlan_status_to_wire(evt->u.wlan_status.status),
                             evt->u.wlan_status.channel);
        break;
    case OPCD_PEVT_ROAMING:
        /* mlan0-only interim policy (#35 item 6) — drop mlan1 roaming. */
        if (evt->u.roaming.idx != 0) return 0;
        opcd_ind_roaming(st, evt->u.roaming.snr, evt->u.roaming.rssi,
                            evt->u.roaming.mac, evt->u.roaming.channel);
        break;
    case OPCD_PEVT_AP_DISCONNECT:
        /* mlan0-only interim policy (#35 item 6) — drop mlan1 disconnects. */
        if (evt->u.ap_disconnect.idx != 0) return 0;
        opcd_ind_ap_disconnect(st, evt->u.ap_disconnect.reason_msg_id,
                                  evt->u.ap_disconnect.result_code,
                                  evt->u.ap_disconnect.mac);
        break;
    case OPCD_PEVT_FAULT_DETECT:
        opcd_ind_fault_detect(st, evt->u.fault_detect.congestion_id,
                                  evt->u.fault_detect.current_val);
        break;
    case OPCD_PEVT_RESET_NOTICE:
        opcd_ind_reset_notice(st, evt->u.reset_notice.cause);
        break;
    case OPCD_PEVT_NONE:
    default:
        break;
    }
    return 0;
}

static void state_set_defaults(opcd_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->conf.udp_port             = OPC_DEFAULT_UDP_PORT;
    st->conf.roam_notify_port     = OPC_DEFAULT_ROAM_NOTIFY_PORT;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->conf.login_idle_s         = OPC_LOGIN_IDLE_S;
    st->conf.device_info_freq_source = OPC_FREQ_SRC_LIVE;   /* Rev1.01 §4.3.4 (#103) */
    st->conf.device_ip_iface         = OPC_IP_IFACE_ETH0;
    st->conf.management_topology     = OPC_TOPOLOGY_ETH0_IP;
    st->paths.conf        = OPC_PATH_CONF;
    st->paths.wifi_init_conf = OPC_PATH_WIFI_INIT_CONF;
    st->paths.reset_cause    = OPC_PATH_RESET_CAUSE;
    st->paths.password    = OPC_PATH_PASSWORD;
    st->paths.ip_list     = OPC_PATH_IPLIST;
    st->paths.radio       = OPC_PATH_RADIO;
    st->paths.device_info = OPC_PATH_DEVICE_INFO;
    st->paths.temp_dir    = OPC_PATH_TEMP;
    strncpy(st->password, OPC_PASSWORD_DEFAULT, sizeof st->password - 1);
    st->radio.station_type    = OPC_STATION_SINGLE;
    st->radio.wlan1.scan_band = OPC_SCAN_BAND_UNSET;   /* Rev1.01: no band lock */
    st->radio.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
    st->udp_fd     = -1;
    st->boot_status = OPC_DEVICE_BOOTING;
    opcd_fault_probe_init(&st->fault_probe);
}

static void state_load_from_disk(opcd_state_t *st)
{
    /* Inventory file is read-only and tolerant of failure: a missing file
     * yields a zero-initialised inventory rather than refusing to boot, so
     * an operator can still talk to a freshly-imaged device and inspect
     * status. opcd_inventory_load() emits its own stderr warning. */
    (void)opcd_inventory_load(st->paths.device_info);

    /* Best-effort: create the tmpfs snapshot directory so per-response
     * device-info JSON dumps under /dev/shm/opcd/ have a place to land.
     * Failure here is non-fatal — snapshots are an external observation
     * side channel, not a protocol dependency. */
    (void)opcd_snapshot_init(OPCD_SNAPSHOT_DIR);

    char pw_buf[128] = {0};
    ssize_t n = opc_store_read_all(st->paths.password, pw_buf, sizeof pw_buf - 1);
    if (n > 0) {
        memset(st->password, 0, sizeof st->password);
        memcpy(st->password, pw_buf, (size_t)n);
    } else if (n < 0 && errno != ENOENT) {
        LOG("password load failed: %s", strerror(errno));
    }
    {
        /* radio.conf: exact current layout, or the 16-byte Rev1.00 layout which
         * is converted (#102). Any other size is a mismatch → defaults (the
         * short-read acceptance of the old `<= 0` check is gone). */
        uint8_t rbuf[64];
        ssize_t rn = opc_store_read_all(st->paths.radio, rbuf, sizeof rbuf);
        int rc = rn > 0 ? opcd_radio_conf_decode(rbuf, (size_t)rn, &st->radio) : -1;
        /* Only an exact-layout file counts as committed. A converted Rev1.00
         * file (rc == 1) describes what the old semantics applied (e.g. one
         * frequency), not what the converted band/list would apply now (e.g.
         * a whole band) — so the next matching request must run apply +
         * persist instead of being skipped as "already there" (Codex P2). */
        st->radio_committed = (rc == 0);
        if (rc < 0) {
            if (rn > 0)
                LOG("radio.conf size mismatch (%zd vs %zu) — discarding", rn, sizeof st->radio);
            memset(&st->radio, 0, sizeof st->radio);
            st->radio.station_type    = st->conf.default_station_type;
            st->radio.wlan1.scan_band = OPC_SCAN_BAND_UNSET;
            st->radio.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
        } else if (rc == 1) {
            LOG("radio.conf: legacy Rev1.00 layout converted to SCAN band/channel list — "
                "re-send SetRadioConfig to confirm");
        }
    }
    n = opc_store_read_all(st->paths.ip_list, &st->ip_list, sizeof st->ip_list);
    if (n > 0 && (size_t)n != sizeof st->ip_list) {
        LOG("iplist size mismatch (%zd vs %zu) — discarding", n, sizeof st->ip_list);
        memset(&st->ip_list, 0, sizeof st->ip_list);
    }
}

static int ensure_dirs(const opcd_state_t *st)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", st->paths.temp_dir);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                LOG("mkdir %s failed: %s", buf, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        LOG("mkdir %s failed: %s", buf, strerror(errno));
    return 0;
}

/* Create a non-blocking (SOCK_NONBLOCK|SOCK_CLOEXEC) UDP socket with
 * SO_REUSEADDR, bound to bind_addr:port. `what` labels failures in the log.
 * Returns the fd, or -1 on any failure (the fd is closed before returning). */
static int open_udp_socket_addr(uint16_t port, uint32_t bind_addr, const char *what)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOG("%s socket: %s", what, strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(bind_addr);
    sa.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        LOG("%s bind port %u failed: %s", what, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* VHL control socket — wildcard bind (reachable from the wired/VHL host). */
static int open_udp_socket(uint16_t port)
{
    return open_udp_socket_addr(port, INADDR_ANY, "udp");
}

/* Roam-notify listener — loopback only; the sender is a local process
 * (wifi_roam.py / passive_roam.py) so the port is never exposed off the box.
 * Returns -1 on any failure (non-fatal at the call site). */
static int open_roam_socket(uint16_t port)
{
    return open_udp_socket_addr(port, INADDR_LOOPBACK, "roam-notify");
}

/* parse_bssid() + roam_datagram_to_evt() live in roam_datagram.c so the
 * wire-parsing/validation logic is host unit-testable (test_roam_datagram.c). */

static int open_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static int open_timerfd_1s(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return -1;
    struct itimerspec it = {
        .it_value    = { .tv_sec = 1, .tv_nsec = 0 },
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
    };
    if (timerfd_settime(fd, 0, &it, NULL) != 0) { close(fd); return -1; }
    return fd;
}

static void usage(void)
{
    fputs("usage: opcd [-p UDP_PORT] [-i IDLE_SECONDS] [-c CONFIG_PATH]\n", stderr);
}

int main(int argc, char **argv)
{
    opcd_state_t st;
    state_set_defaults(&st);

    int port_override = 0;
    int idle_override = 0;
    int opt;
    while ((opt = getopt(argc, argv, "p:i:c:h")) != -1) {
        switch (opt) {
        case 'p': port_override = atoi(optarg); break;
        case 'i': idle_override = atoi(optarg); break;
        case 'c': st.paths.conf = optarg; break;
        case 'h': default: usage(); return (opt == 'h') ? 0 : 2;
        }
    }
    if (port_override > 0) st.conf.udp_port     = (uint16_t)port_override;
    if (idle_override > 0) st.conf.login_idle_s = (uint32_t)idle_override;
    /* opc.conf currently carries only the congestion_* overrides (T6 interim
     * thresholds); other settings still come from defaults / CLI options. */
    opcd_fault_probe_conf(&st.fault_probe, st.paths.conf);
    st.conf.device_info_freq_source = opcd_freq_source_parse(st.paths.conf);
    st.conf.device_ip_iface         = opcd_ip_iface_parse(st.paths.conf);
    st.conf.management_topology     = opcd_topology_parse(st.paths.conf, st.paths.wifi_init_conf);
    if (st.conf.management_topology == OPC_TOPOLOGY_PEER_ROUTE &&
        st.conf.device_ip_iface != OPC_IP_IFACE_MLAN0)
        fprintf(stderr, "opcd: management topology peer_route — the IP read/apply "
                        "plane follows mlan0; opc.conf device_ip_iface is ignored (#122)\n");
    st.conf.roam_notify_port =
        opcd_roam_notify_port_parse(st.paths.conf, st.conf.roam_notify_port);

    /* 프로토콜 감사 로그를 가장 먼저 활성화(openlog)해, 이후의 startup 실패
     * (bind/init/자원)·정상 시작·중지가 모두 logger.log(local0)로 남게 한다. */
    opcd_log_init();
    OLOG_INFO("start: opcd starting (udp_port=%u roam_notify_port=%u idle=%us)",
              (unsigned)st.conf.udp_port, (unsigned)st.conf.roam_notify_port,
              (unsigned)st.conf.login_idle_s);

    ensure_dirs(&st);
    state_load_from_disk(&st);

    /* Bind the VHL control socket BEFORE plat->init(). Per platform.h, init()
     * is allowed to block during driver probe (the nxp backend does an nl80211
     * genetlink family resolution — a blocking netlink recv bounded by
     * SO_RCVTIMEO). Binding *after* init left a startup window where UDP :port
     * was still unbound: a client's first Login landing in that window hit an
     * unbound port and was dropped (ICMP port-unreachable, ignored by
     * connectionless UDP) → the client timed out and only its retry succeeded.
     * Binding first means a Login arriving during init is buffered by the
     * kernel receive queue and served once the epoll loop starts, never lost. */
    int udp_fd = open_udp_socket(st.conf.udp_port);
    if (udp_fd < 0) {
        OLOG_ERR("start: UDP :%u bind failed — aborting", (unsigned)st.conf.udp_port);
        return 1;
    }
    /* udp_fd is published into st.udp_fd only after init succeeds (below), so a
     * failed init never leaves a dangling descriptor in shared state. */

    opcd_platform_register();   /* backend resolved at link time — see Makefile PLATFORM */
    const opcd_platform_ops_t *plat = opcd_platform();
    if (!plat) {
        LOG("platform registration failed");
        OLOG_ERR("start: platform registration failed — aborting");
        close(udp_fd);
        return 1;
    }
    if (plat->init() != 0) {
        LOG("platform init failed");
        OLOG_ERR("start: platform init failed — aborting");
        /* teardown is idempotent and must tolerate partial init — call it
         * unconditionally so a partially-acquired netlink socket / fd does
         * not leak across a systemd restart loop. */
        plat->teardown();
        close(udp_fd);
        return 1;
    }
    g_teardown = plat->teardown;
    if (plat->get_wlan_count() < 1) {
        LOG("platform reports zero WLAN interfaces — refusing to start");
        OLOG_ERR("start: zero WLAN interfaces — refusing to start");
        g_teardown();
        close(udp_fd);
        return 1;
    }

    /* Publish the control fd into shared state now that all init checks have
     * passed (Gemini review): a failed init above returns with udp_fd closed
     * but never stored, so opcd_state never holds a dangling descriptor. */
    st.udp_fd = udp_fd;

    /* BOOTING is never observable on the wire: a Login arriving while
     * plat->init() blocks is buffered in the kernel receive queue and is not
     * dequeued until the epoll loop runs — which is after this OPC_DEVICE_READY
     * transition. handle_login's "boot in progress" (0x0001) branch is kept for
     * the spec's boot-window semantics (D15); if code is ever inserted between
     * this point and the epoll_wait loop (e.g. async init), reconsider whether a
     * buffered Login could observe BOOTING. */
    st.boot_status = OPC_DEVICE_READY;
    LOG("listening on UDP :%u (idle=%us)", st.conf.udp_port, st.conf.login_idle_s);

    /* Roam-notify listener on loopback (non-fatal — a bind failure just means
     * no local roam indications; the daemon still serves the VHL protocol).
     * Refuse to share the control UDP port: with SO_REUSEADDR the loopback roam
     * bind and the wildcard control bind would collide, and localhost VHL
     * requests to that port could be delivered to the roam socket (parsed as
     * garbage, never answered → silent client timeouts). Disable roam-notify
     * in that misconfiguration. */
    int roam_fd = -1;
    if (st.conf.roam_notify_port == st.conf.udp_port) {
        LOG("roam-notify port %u collides with control udp port — roam-notify disabled",
            st.conf.roam_notify_port);
    } else {
        roam_fd = open_roam_socket(st.conf.roam_notify_port);
        if (roam_fd < 0)
            LOG("roam-notify listener unavailable — local roam indications disabled");
    }

    int sig_fd   = open_signalfd();
    int timer_fd = open_timerfd_1s();
    int ep       = epoll_create1(EPOLL_CLOEXEC);
    if (sig_fd < 0 || timer_fd < 0 || ep < 0) {
        LOG("setup failed");
        OLOG_ERR("start: signalfd/timerfd/epoll setup failed — aborting");
        close(udp_fd);
        if (roam_fd  >= 0) close(roam_fd);
        if (sig_fd   >= 0) close(sig_fd);
        if (timer_fd >= 0) close(timer_fd);
        if (ep       >= 0) close(ep);
        g_teardown();
        return 1;
    }

    struct epoll_event ev_udp   = { .events = EPOLLIN, .data.fd = udp_fd };
    struct epoll_event ev_sig   = { .events = EPOLLIN, .data.fd = sig_fd };
    struct epoll_event ev_timer = { .events = EPOLLIN, .data.fd = timer_fd };
    epoll_ctl(ep, EPOLL_CTL_ADD, udp_fd,   &ev_udp);
    epoll_ctl(ep, EPOLL_CTL_ADD, sig_fd,   &ev_sig);
    epoll_ctl(ep, EPOLL_CTL_ADD, timer_fd, &ev_timer);

    /* Platform async events — registered only if the backend exposes a
     * pollable fd. The stub returns -1 (no async events), so this branch
     * is currently inert; nxp.c will produce a real fd from nl80211. */
    int evt_fd = plat->event_fd();
    if (evt_fd >= 0) {
        struct epoll_event ev_evt = { .events = EPOLLIN, .data.fd = evt_fd };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, evt_fd, &ev_evt) != 0) {
            LOG("epoll_ctl(evt_fd=%d) failed: %s — platform events disabled",
                evt_fd, strerror(errno));
            evt_fd = -1;
        }
    }

    /* Async NVRAM writer — keeps the fsync stall of Set* persists off the
     * epoll loop (PERF-001). Created after open_signalfd() so the worker
     * inherits the blocked SIGINT/SIGTERM mask: a process-directed signal
     * must land on the signalfd, not terminate the process through the
     * worker's default disposition. Creation failure is non-fatal —
     * handlers fall back to the original synchronous write path. */
    st.store_async = opc_store_async_create();
    if (!st.store_async)
        LOG("async NVRAM writer unavailable — Set* persists run synchronously");

    /* Async NVRAM completions → deferred Set* acks. */
    int store_fd = opc_store_async_event_fd(st.store_async);
    if (store_fd >= 0) {
        struct epoll_event ev_store = { .events = EPOLLIN, .data.fd = store_fd };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, store_fd, &ev_store) != 0) {
            LOG("epoll_ctl(store_fd=%d) failed: %s — async NVRAM writer disabled",
                store_fd, strerror(errno));
            opc_store_async_destroy(st.store_async);
            st.store_async = NULL;
            store_fd = -1;
        }
    }

    /* Roam-notify datagrams from the local WLAN roaming executor. Non-fatal on
     * registration failure (same idiom as store_fd): close and mark -1 so the
     * dispatch loop's `fd == roam_fd` arm stays inert. */
    if (roam_fd >= 0) {
        struct epoll_event ev_roam = { .events = EPOLLIN, .data.fd = roam_fd };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, roam_fd, &ev_roam) != 0) {
            LOG("epoll_ctl(roam_fd=%d) failed: %s — roam-notify disabled",
                roam_fd, strerror(errno));
            close(roam_fd);
            roam_fd = -1;
        }
    }

    uint8_t rx[OPC_FRAME_MAX], tx[OPC_FRAME_MAX];

    OLOG_INFO("start: ready — serving on UDP :%u", (unsigned)st.conf.udp_port);
    while (!st.should_exit && !st.should_reset) {
        struct epoll_event events[8];
        int n = epoll_wait(ep, events, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG("epoll_wait: %s", strerror(errno));
            OLOG_ERR("stop: epoll_wait failed: %s — exiting", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == sig_fd) {
                /* Drain all queued signals but record the FIRST one — that is the
                 * signal that actually initiated shutdown. Logging si after the
                 * drain would report the last-read signal instead (Gemini review). */
                struct signalfd_siginfo si;
                unsigned first_signo = 0;
                while (read(sig_fd, &si, sizeof si) == (ssize_t)sizeof si)
                    if (first_signo == 0) first_signo = si.ssi_signo;
                /* A readable sig_fd always means shutdown — set should_exit
                 * UNCONDITIONALLY. Gating it on first_signo would busy-loop if the
                 * fd is readable but yields no signal (EAGAIN/error/EOF): epoll is
                 * level-triggered, so EPOLLIN would re-fire every iteration and
                 * the loop would never exit nor drain (Gemini review). Only the
                 * log value is guarded — report the signal number if one was
                 * actually read, else a generic reason instead of "signal 0". */
                LOG("signal received — exiting");
                if (first_signo != 0)
                    OLOG_INFO("stop: signal %u received — shutting down", first_signo);
                else
                    OLOG_INFO("stop: shutting down (signalfd readable, no signal number)");
                /* §3.4.6 autonomous-reset notice (#47 T9, reset_cause.h): a
                 * SIGTERM that is part of a SYSTEM shutdown/reboot — every
                 * board recovery path ends in one — is announced with the
                 * cause the reboot policy left behind (else SYSTEM). A plain
                 * `systemctl stop opcd` answers "running" and stays silent;
                 * an unanswerable probe (OPC_RESET_PROBE_TIMEOUT_MS, sized from
                 * the measured shutdown latency) also stays silent — a reset
                 * is never announced on a guess. The network is still up
                 * here (opcd is After=network-online.target). */
                if (first_signo == SIGTERM) {
                    int stopping = opcd_system_stopping(OPC_PATH_SYSTEMCTL,
                                                        OPC_RESET_PROBE_TIMEOUT_MS);
                    uint32_t cause = opcd_shutdown_reset_cause(
                        stopping, opcd_reset_cause_read(st.paths.reset_cause));
                    if (cause) {
                        (void)opcd_ind_reset_notice(&st, cause);
                        OLOG_INFO("stop: system going down — ResetNotice cause=0x%08X sent (if enabled)",
                                  (unsigned)cause);
                    } else {
                        OLOG_INFO("stop: not a system shutdown (probe=%d) — no ResetNotice", stopping);
                    }
                }
                st.should_exit = true;
            } else if (fd == timer_fd) {
                /* Sum every expiration so a multi-second stall advances the
                 * indication period by the seconds actually elapsed, not 1
                 * (Codex, PR #116). timerfd reports the accumulated count. */
                uint64_t expirations, elapsed = 0;
                while (read(timer_fd, &expirations, sizeof expirations) > 0)
                    elapsed += expirations;
                if (st.logged_in) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    if (ts.tv_sec >= st.idle_deadline) {
                        LOG("idle auto-logout (holder=0x%08X)", st.holder_ip);
                        opcd_session_logout(&st);
                    }
                }
                opcd_ind_tick_elapsed(&st, elapsed > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)elapsed);
            } else if (fd == store_fd) {
                /* store_fd is -1 when no async writer is attached (creation or
                 * epoll_ctl failed); a real fd never equals -1, so this branch
                 * is inert in that case. */
                opcd_store_async_on_ready(&st);
            } else if (fd == evt_fd) {
                /* Drain all queued platform events; on_platform_event
                 * dispatches each to the corresponding indication.
                 * On drain-layer failure, remove the fd from epoll so a
                 * level-triggered EPOLLIN does not spin the loop logging
                 * the same error on every iteration. */
                int drain_rc = plat->drain_events(on_platform_event, &st);
                if (drain_rc < 0) {
                    LOG("drain_events failed (rc=%d) — disabling platform events",
                        drain_rc);
                    epoll_ctl(ep, EPOLL_CTL_DEL, evt_fd, NULL);
                    evt_fd = -1;
                }
            } else if (fd == roam_fd) {
                /* Local roam-notify datagrams (loopback only). Drain the whole
                 * non-blocking socket; each datagram is one flat JSON object
                 * (design §9.1 WIRE CONTRACT). Malformed/short → drop silently
                 * (debug log only), never block, never crash. */
                while (1) {
                    char buf[513];   /* contract caps payload at 512 B + NUL */
                    ssize_t rn = recvfrom(roam_fd, buf, sizeof buf - 1,
                                          MSG_TRUNC, NULL, NULL);
                    if (rn < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG("roam recvfrom: %s", strerror(errno));
                        break;
                    }
                    if (rn == 0) continue;   /* empty datagram — nothing to parse */
                    /* MSG_TRUNC returns the FULL datagram length even when it
                     * overflowed the buffer; a truncated payload must not be
                     * parsed as if complete. Drop anything over the 512 B cap. */
                    if (rn > (ssize_t)(sizeof buf - 1)) {
                        static unsigned long oversize_cnt;
                        if ((oversize_cnt++ % 64) == 0) {
                            LOG("roam-notify: oversized datagram (%zd B) — dropped (drops=%lu)",
                                rn, oversize_cnt);
                            OLOG_WARN("roam-notify: oversized datagram (%zdB) — dropped (drops=%lu)",
                                      rn, oversize_cnt);
                        }
                        continue;
                    }
                    buf[rn] = '\0';
                    opcd_platform_evt_t revt;
                    if (roam_datagram_to_evt(buf, &revt) != 0) {
                        /* Rate-limit: a misbehaving local sender must not flood
                         * the log. Emit 1-in-64 malformed drops with a count. */
                        static unsigned long malformed_cnt;
                        if ((malformed_cnt++ % 64) == 0) {
                            LOG("roam-notify: malformed datagram (%zd B) — ignored (drops=%lu)",
                                rn, malformed_cnt);
                            OLOG_WARN("roam-notify: malformed datagram (%zdB) — ignored (drops=%lu)",
                                      rn, malformed_cnt);
                        }
                        continue;
                    }
                    OLOG_INFO("exec: roam-notify idx=%u ap=%02x:%02x:%02x:%02x:%02x:%02x "
                              "ch_field=0x%04x rssi=%d snr=%d → Roaming indication",
                              revt.u.roaming.idx,
                              revt.u.roaming.mac[0], revt.u.roaming.mac[1],
                              revt.u.roaming.mac[2], revt.u.roaming.mac[3],
                              revt.u.roaming.mac[4], revt.u.roaming.mac[5],
                              revt.u.roaming.channel,
                              revt.u.roaming.rssi, revt.u.roaming.snr);
                    on_platform_event(&revt, &st);
                }
            } else if (fd == udp_fd) {
                while (1) {
                    struct sockaddr_in src;
                    socklen_t srclen = sizeof src;
                    /* MSG_TRUNC: rn reports the true datagram size even when
                     * it exceeds rx — an oversize frame is detected instead
                     * of silently processing a truncated prefix (D12). */
                    ssize_t rn = recvfrom(udp_fd, rx, sizeof rx, MSG_TRUNC,
                                          (struct sockaddr *)&src, &srclen);
                    if (rn < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG("recvfrom: %s", strerror(errno));
                        break;
                    }
                    uint32_t cip  = ntohl(src.sin_addr.s_addr);
                    uint16_t cprt = ntohs(src.sin_port);
                    /* Lenient receive-length model (D12/D13, 2026-06-16): trust
                     * the header's declared Length and dispatch exactly
                     * 8+Length bytes, ignoring trailing wire bytes. A valid frame
                     * always fits rx (sizeof rx == OPC_FRAME_MAX), so an
                     * MSG_TRUNC overflow only lops off bytes *past* the frame.
                     * `buffered` is what actually landed in rx. */
                    size_t buffered = ((size_t)rn > sizeof rx) ? sizeof rx
                                                               : (size_t)rn;
                    size_t want = opcd_intake_frame_len(rx, buffered);
                    if (want == 0) {
                        /* Bad length: runt / 9..63 B / declared Length over the
                         * protocol max / datagram shorter than its declared
                         * frame (truncated). 0x0003 NG to the logged-in session
                         * (header prefix echoes req/seq), else drop. An overlong
                         * datagram with a valid declared Length never reaches
                         * this branch — opcd_intake_frame_len returns the trimmed
                         * length instead, so it is handled below. */
                        if ((size_t)rn > sizeof rx)
                            LOG("oversize datagram (%zd B): declared length bad — rejected", rn);
                        {
                            /* 외부 임의 호스트가 유발 가능한 경로 — 1/64 rate
                             * limit 으로 logger.log 플러딩 방지(roam-notify
                             * malformed 와 동일 패턴). */
                            static unsigned long badlen_cnt;
                            if ((badlen_cnt++ % 64) == 0) {
                                char ipb[16];
                                OLOG_WARN("RX bad-length datagram (%zdB) from=%s:%u — 0x0003/drop (count=%lu)",
                                          rn, opcd_ip4str(cip, ipb), cprt, badlen_cnt);
                            }
                        }
                        opcd_reject_bad_length(&st, rx, buffered, cip, cprt);
                        continue;
                    }
                    /* Observability: the lenient trim happy-path is otherwise
                     * silent — surface an oversize datagram that carried a valid
                     * frame so on-target triage sees it was accepted (trimmed)
                     * rather than dropped. */
                    if ((size_t)rn > sizeof rx)
                        LOG("oversize datagram (%zd B): trimmed to declared %zu B frame, trailing ignored", rn, want);
                    struct timespec rx_ts;
                    clock_gettime(CLOCK_MONOTONIC, &rx_ts);
                    ssize_t tx_len = 0;
                    int rc = opcd_dispatch(&st, rx, want, cip, cprt,
                                           tx, sizeof tx, &tx_len);
                    if (rc == 0) {
                        /* tx_len == 0 is a legitimate no-response: a deferred
                         * Set* ack awaiting its NVRAM completion (its T7 log
                         * fires on the deferred send), or a failed ack pack
                         * (emit_ack). */
                        if (tx_len > 0) {
                            ssize_t w = sendto(udp_fd, tx, (size_t)tx_len, 0,
                                               (struct sockaddr *)&src, srclen);
                            if (w != tx_len) LOG("sendto short: %zd/%zd", w, tx_len);
                            /* T7 (proto-todo): actual service time vs the
                             * spec budget (1 s for non-persisting commands). */
                            opc_header_t hdr;
                            /* `want` (the trimmed frame length), not raw rn:
                             * rn can exceed sizeof rx under MSG_TRUNC, and
                             * passing a buf_len past the buffer violates the
                             * unpack contract even though it only reads 8 B. */
                            if (opc_fixed_header_unpack(rx, want, &hdr) == 0) {
                                struct timespec now;
                                clock_gettime(CLOCK_MONOTONIC, &now);
                                long long us =
                                    (now.tv_sec - rx_ts.tv_sec) * 1000000LL +
                                    (now.tv_nsec - rx_ts.tv_nsec) / 1000;
                                LOG("req 0x%04X seq=%u served in %lld.%03lld ms",
                                    hdr.req_indication_id, hdr.sequence_number,
                                    us / 1000, us % 1000);
                                /* 감사 로그: 응답 결과. 송신 실패는 성공으로
                                 * 기록하지 않는다(ERR). 단순/device-info ack 는
                                 * OK/NG+cause, basic-info 데이터 ack 는 길이만. */
                                uint16_t ares, acause;
                                char ipb[16];
                                if (w != tx_len)
                                    OLOG_ERR("TX %s ack seq=%u to=%s:%u send failed (%zd/%zd)",
                                             opcd_req_name(hdr.req_indication_id),
                                             hdr.sequence_number,
                                             opcd_ip4str(cip, ipb), cprt, w, tx_len);
                                else if (opcd_ack_result_peek(tx, (size_t)tx_len, &ares, &acause))
                                    OLOG_INFO("TX %s ack seq=%u to=%s:%u %s err=0x%04x (%lld.%03lldms)",
                                              opcd_req_name(hdr.req_indication_id),
                                              hdr.sequence_number,
                                              opcd_ip4str(cip, ipb), cprt,
                                              ares == 0 ? "OK" : "NG", acause,
                                              us / 1000, us % 1000);
                                else
                                    OLOG_INFO("TX %s ack seq=%u to=%s:%u len=%zd (%lld.%03lldms)",
                                              opcd_req_name(hdr.req_indication_id),
                                              hdr.sequence_number,
                                              opcd_ip4str(cip, ipb), cprt, tx_len,
                                              us / 1000, us % 1000);
                            }
                        }
                    } else {
                        LOG("frame dropped (rc=%d rn=%zd)", rc, rn);
                    }

                    /* D9: a SetRadioConfig that failed to apply armed a deferred
                     * best-effort revert to the last-good config. Run it now —
                     * AFTER the NG ack was sent above — so the failure response
                     * is never delayed by the (possibly timing-out) recovery
                     * apply. No-op unless armed. */
                    opcd_radio_revert_drain(&st);

                    /* If the datagram just handled was a Logout that committed
                     * a ChangeIp, stop draining: applying the change (below)
                     * reconfigures eth0, so any datagrams already queued for the
                     * old address must NOT be processed as post-change traffic
                     * on the new IP (a queued Login could otherwise re-take the
                     * session with a reply the client never sees). They resurface
                     * on the next epoll cycle. Breaking here also closes the
                     * same-drain interleaving window and removes the flood-induced
                     * apply delay — the change no longer waits for the socket to
                     * reach EAGAIN. The Logout ack was already sent above, so A12
                     * (apply only after the ack is on the wire) still holds. */
                    if (st.ip_change_commit_armed)
                        break;
                }
            }
        }
        opcd_apply_pending_ip_change(&st);
    }

    /* Completes any queued NVRAM writes before the worker joins; an ack
     * still in flight is dropped — the client's response timer covers it. */
    opc_store_async_destroy(st.store_async);
    st.store_async = NULL;

    if (st.should_reset) {
        LOG("reset requested — exiting (systemd will restart)");
        (void)plat->prepare_reset();   /* platform.h: all vtable members non-NULL */
    }
    if (g_teardown) g_teardown();
    close(udp_fd);
    if (roam_fd >= 0) close(roam_fd);
    close(sig_fd);
    close(timer_fd);
    close(ep);
    /* Single lifecycle stop line for both paths (symmetric): the reason arg
     * distinguishes a Reset-command restart from a signal shutdown. */
    OLOG_INFO("stop: opcd exited (%s)", st.should_reset ? "reset/restart" : "shutdown");
    return 0;
}
