/*
 * opcd — OPC-side UDP/IP control daemon for the VHL ↔ wireless-board protocol.
 *
 * Single-threaded epoll loop driving:
 *   - UDP socket on /usr/local/opc/etc/opc.conf::udp_port  (default 50607)
 *   - signalfd  for SIGINT / SIGTERM      → graceful shutdown
 *   - timerfd   1 s tick                  → indication period & idle check
 *
 * State persists in /usr/local/opc/etc/{password, iplist.cfg, radio.conf}
 * via atomic temp+rename writes (see store.c).
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
#include "opcd_state.h"
#include "platform.h"
#include "store.h"

#define LOG(fmt, ...) fprintf(stderr, "opcd: " fmt "\n", ##__VA_ARGS__)

/* Cached teardown function pointer for AS-safe signal-handler access — see
 * platform.h. Although opcd uses signalfd (not raw handlers) today, this
 * keeps the contract honest for future hot paths. */
static void (*g_teardown)(void);

static void state_set_defaults(opcd_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->conf.udp_port             = OPC_DEFAULT_UDP_PORT;
    st->conf.vendor_code          = 0x00902CFB;
    st->conf.product_code         = 0xFE03;
    st->conf.product_subcode      = 0x0001;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->conf.login_idle_s         = OPC_LOGIN_IDLE_S;
    st->paths.conf      = OPC_PATH_CONF;
    st->paths.password  = OPC_PATH_PASSWORD;
    st->paths.ip_list   = OPC_PATH_IPLIST;
    st->paths.radio     = OPC_PATH_RADIO;
    st->paths.temp_dir  = OPC_PATH_TEMP;
    strncpy(st->password, OPC_PASSWORD_DEFAULT, sizeof st->password - 1);
    st->radio.station_type = OPC_STATION_SINGLE;
    st->udp_fd     = -1;
    st->boot_status = OPC_DEVICE_BOOTING;
}

static void state_load_from_disk(opcd_state_t *st)
{
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

    ensure_dirs(&st);
    state_load_from_disk(&st);

    opcd_platform_stub_register();
    const opcd_platform_ops_t *plat = opcd_platform();
    if (!plat || plat->init() != 0) {
        LOG("platform init failed");
        return 1;
    }
    g_teardown = plat->teardown;
    if (plat->get_wlan_count() < 1) {
        LOG("platform reports zero WLAN interfaces — refusing to start");
        g_teardown();
        return 1;
    }

    st.boot_status = OPC_DEVICE_READY;
    LOG("starting on UDP :%u (idle=%us)", st.conf.udp_port, st.conf.login_idle_s);

    int udp_fd = open_udp_socket(st.conf.udp_port);
    if (udp_fd < 0) return 1;
    st.udp_fd = udp_fd;

    int sig_fd   = open_signalfd();
    int timer_fd = open_timerfd_1s();
    int ep       = epoll_create1(EPOLL_CLOEXEC);
    if (sig_fd < 0 || timer_fd < 0 || ep < 0) { LOG("setup failed"); return 1; }

    struct epoll_event ev_udp   = { .events = EPOLLIN, .data.fd = udp_fd };
    struct epoll_event ev_sig   = { .events = EPOLLIN, .data.fd = sig_fd };
    struct epoll_event ev_timer = { .events = EPOLLIN, .data.fd = timer_fd };
    epoll_ctl(ep, EPOLL_CTL_ADD, udp_fd,   &ev_udp);
    epoll_ctl(ep, EPOLL_CTL_ADD, sig_fd,   &ev_sig);
    epoll_ctl(ep, EPOLL_CTL_ADD, timer_fd, &ev_timer);
    /* TODO: platform event_fd / drain_events epoll wiring lands in the next
     * PR alongside indication.h idx fields. Stub returns event_fd()==-1 so
     * no events arrive today; the contract documented in platform.h is
     * temporarily under-fulfilled. */

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
                        st.logged_in   = false;
                        st.boot_status = OPC_DEVICE_READY;
                        opcd_ind_init_complete(&st, OPC_INIT_STATE_LOGGED_OUT);
                    }
                }
                opcd_ind_tick(&st);
            } else if (fd == udp_fd) {
                while (1) {
                    struct sockaddr_in src;
                    socklen_t srclen = sizeof src;
                    ssize_t rn = recvfrom(udp_fd, rx, sizeof rx, 0,
                                          (struct sockaddr *)&src, &srclen);
                    if (rn < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG("recvfrom: %s", strerror(errno));
                        break;
                    }
                    uint32_t cip  = ntohl(src.sin_addr.s_addr);
                    uint16_t cprt = ntohs(src.sin_port);
                    ssize_t tx_len = 0;
                    int rc = opcd_dispatch(&st, rx, (size_t)rn, cip, cprt,
                                           tx, sizeof tx, &tx_len);
                    if (rc == 0 && tx_len > 0) {
                        ssize_t w = sendto(udp_fd, tx, (size_t)tx_len, 0,
                                           (struct sockaddr *)&src, srclen);
                        if (w != tx_len) LOG("sendto short: %zd/%zd", w, tx_len);
                    } else {
                        LOG("frame dropped (rc=%d rn=%zd)", rc, rn);
                    }
                }
            }
        }
        opcd_apply_pending_ip_change(&st);
    }

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
