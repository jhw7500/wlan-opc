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
#include "handler.h"
#include "indication.h"
#include "inventory.h"
#include "opcd_state.h"
#include "platform.h"
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
 * protocol level. This is a spec limitation, not an opcd routing bug. */
static int on_platform_event(const opcd_platform_evt_t *evt, void *ctx)
{
    opcd_state_t *st = ctx;
    switch (evt->kind) {
    case OPCD_PEVT_INIT_COMPLETE:
        opcd_ind_init_complete(st, evt->u.init_complete.status);
        break;
    case OPCD_PEVT_WLAN_STATUS:
        opcd_ind_wlan_status(st, evt->u.wlan_status.status,
                                evt->u.wlan_status.channel);
        break;
    case OPCD_PEVT_ROAMING:
        opcd_ind_roaming(st, evt->u.roaming.snr, evt->u.roaming.rssi,
                            evt->u.roaming.mac, evt->u.roaming.channel);
        break;
    case OPCD_PEVT_AP_DISCONNECT:
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
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->conf.login_idle_s         = OPC_LOGIN_IDLE_S;
    st->paths.conf        = OPC_PATH_CONF;
    st->paths.password    = OPC_PATH_PASSWORD;
    st->paths.ip_list     = OPC_PATH_IPLIST;
    st->paths.radio       = OPC_PATH_RADIO;
    st->paths.device_info = OPC_PATH_DEVICE_INFO;
    st->paths.temp_dir    = OPC_PATH_TEMP;
    strncpy(st->password, OPC_PASSWORD_DEFAULT, sizeof st->password - 1);
    st->radio.station_type = OPC_STATION_SINGLE;
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
    if (opc_store_read_all(st->paths.radio, &st->radio, sizeof st->radio) <= 0) {
        st->radio.station_type = st->conf.default_station_type;
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

static int open_udp_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOG("socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        LOG("bind :%u failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

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

    ensure_dirs(&st);
    state_load_from_disk(&st);

    opcd_platform_register();   /* backend resolved at link time — see Makefile PLATFORM */
    const opcd_platform_ops_t *plat = opcd_platform();
    if (!plat) {
        LOG("platform registration failed");
        return 1;
    }
    if (plat->init() != 0) {
        LOG("platform init failed");
        /* teardown is idempotent and must tolerate partial init — call it
         * unconditionally so a partially-acquired netlink socket / fd does
         * not leak across a systemd restart loop. */
        plat->teardown();
        return 1;
    }
    g_teardown = plat->teardown;
    if (plat->get_wlan_count() < 1) {
        LOG("platform reports zero WLAN interfaces — refusing to start");
        g_teardown();
        return 1;
    }

    /* Boot completes synchronously, so BOOTING is never observable from the
     * wire today — handle_login's "boot in progress" (0x0001) branch is kept
     * for the spec's boot-window semantics in case init ever becomes
     * asynchronous (D15). */
    st.boot_status = OPC_DEVICE_READY;
    LOG("starting on UDP :%u (idle=%us)", st.conf.udp_port, st.conf.login_idle_s);

    int udp_fd = open_udp_socket(st.conf.udp_port);
    if (udp_fd < 0) { g_teardown(); return 1; }
    st.udp_fd = udp_fd;

    int sig_fd   = open_signalfd();
    int timer_fd = open_timerfd_1s();
    int ep       = epoll_create1(EPOLL_CLOEXEC);
    if (sig_fd < 0 || timer_fd < 0 || ep < 0) {
        LOG("setup failed");
        close(udp_fd);
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

    uint8_t rx[OPC_FRAME_MAX], tx[OPC_FRAME_MAX];

    while (!st.should_exit && !st.should_reset) {
        struct epoll_event events[8];
        int n = epoll_wait(ep, events, 8, -1);
        if (n < 0) { if (errno == EINTR) continue; LOG("epoll_wait: %s", strerror(errno)); break; }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == sig_fd) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof si) == (ssize_t)sizeof si) { }
                LOG("signal received — exiting");
                st.should_exit = true;
            } else if (fd == timer_fd) {
                uint64_t expirations;
                while (read(timer_fd, &expirations, sizeof expirations) > 0) { }
                if (st.logged_in) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    if (ts.tv_sec >= st.idle_deadline) {
                        LOG("idle auto-logout (holder=0x%08X)", st.holder_ip);
                        opcd_session_logout(&st);
                    }
                }
                opcd_ind_tick(&st);
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
                    if ((size_t)rn > sizeof rx) {
                        /* D12: oversize datagram — only sizeof rx bytes
                         * landed in rx; the header prefix is intact for the
                         * NG echo. */
                        LOG("oversize frame (%zd B) rejected", rn);
                        opcd_reject_bad_length(&st, rx, sizeof rx, cip, cprt);
                        continue;
                    }
                    if (rn != OPC_FIXED_HEADER_SIZE &&
                        rn < (ssize_t)OPC_HEADER_SIZE) {
                        /* D13: 9..63 B can never be a valid frame (frame.c
                         * rejects it) and <8 B has no header to echo (the
                         * reject helper drops it). Exactly 8 B is NOT a bad
                         * length: it is the spec's empty request wire size
                         * (Logout/GetBasicInfo/GetDeviceInfo/Reset — A2:
                         * fixed header only, Length=0), accepted by
                         * frame_parse and handled by the dispatcher.
                         * Previously a silent drop for every source. */
                        opcd_reject_bad_length(&st, rx, (size_t)rn, cip, cprt);
                        continue;
                    }
                    ssize_t tx_len = 0;
                    int rc = opcd_dispatch(&st, rx, (size_t)rn, cip, cprt,
                                           tx, sizeof tx, &tx_len);
                    if (rc == 0) {
                        /* tx_len == 0 is a legitimate no-response: a deferred
                         * Set* ack awaiting its NVRAM completion, or a failed
                         * ack pack (emit_ack). */
                        if (tx_len > 0) {
                            ssize_t w = sendto(udp_fd, tx, (size_t)tx_len, 0,
                                               (struct sockaddr *)&src, srclen);
                            if (w != tx_len) LOG("sendto short: %zd/%zd", w, tx_len);
                        }
                    } else {
                        LOG("frame dropped (rc=%d rn=%zd)", rc, rn);
                    }
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
    close(sig_fd);
    close(timer_fd);
    close(ep);
    return 0;
}
