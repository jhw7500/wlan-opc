#define _GNU_SOURCE
#include "../systemd_notify.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef OPCD_TEST_BIN
#define OPCD_TEST_BIN "../../build/native/opcd/opcd"
#endif

static int failures;

#define ASSERT(cond, msg) do {                                            \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL: %s (line %d, errno=%d)\n",               \
                (msg), __LINE__, errno);                                  \
        failures++;                                                       \
    }                                                                     \
} while (0)

static void clear_env(void)
{
    unsetenv("NOTIFY_SOCKET");
    unsetenv("WATCHDOG_USEC");
    unsetenv("WATCHDOG_PID");
}

static int recv_exact(int fd, const char *want)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) != 1)
        return -1;
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    return n == (ssize_t)strlen(want) && memcmp(buf, want, (size_t)n) == 0
        ? 0 : -1;
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int recv_exact_until(int fd, const char *want, int timeout_ms)
{
    long long deadline = monotonic_ms() + timeout_ms;
    while (1) {
        long long remaining = deadline - monotonic_ms();
        if (remaining <= 0)
            return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, remaining > 100 ? 100 : (int)remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) continue;
        char buf[64];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n == (ssize_t)strlen(want) &&
            memcmp(buf, want, (size_t)n) == 0)
            return 0;
    }
}

static int choose_udp_port(char *text, size_t cap)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return snprintf(text, cap, "%u", (unsigned)ntohs(sa.sin_port)) < (int)cap
        ? 0 : -1;
}

static pid_t spawn_udp_flooder(const char *port_text)
{
    pid_t pid = fork();
    if (pid != 0) return pid;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) _exit(2);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons((uint16_t)strtoul(port_text, NULL, 10));
    const uint8_t bad_frame = 0;
    long long deadline = monotonic_ms() + 4000;
    while (monotonic_ms() < deadline)
        (void)sendto(fd, &bad_frame, sizeof bad_frame, 0,
                     (struct sockaddr *)&dst, sizeof dst);
    close(fd);
    _exit(0);
}

static int bind_path_socket(char *path, size_t cap)
{
    char dir[] = "/tmp/opcd-notify-XXXXXX";
    if (!mkdtemp(dir))
        return -1;
    if (snprintf(path, cap, "%s/socket", dir) >= (int)cap) {
        rmdir(dir);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        rmdir(dir);
        return -1;
    }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        rmdir(dir);
        return -1;
    }
    return fd;
}

static void cleanup_path_socket(int fd, const char *path)
{
    close(fd);
    unlink(path);
    char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        rmdir(dir);
    }
}

static void test_standalone(void)
{
    clear_env();
    opcd_systemd_notify_t n;
    ASSERT(opcd_systemd_notify_init(&n) == 0, "missing NOTIFY_SOCKET is supported");
    ASSERT(n.fd == -1, "standalone mode owns no socket");
    ASSERT(!n.watchdog_enabled, "standalone mode has no watchdog");
    ASSERT(opcd_systemd_ready(&n) == 0, "READY is a no-op in standalone mode");
    ASSERT(opcd_systemd_watchdog(&n) == 0, "WATCHDOG is a no-op in standalone mode");
    opcd_systemd_notify_close(&n);
}

static void test_filesystem_socket_and_frames(void)
{
    clear_env();
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int receiver = bind_path_socket(path, sizeof path);
    ASSERT(receiver >= 0, "bind filesystem notification receiver");
    if (receiver < 0) return;

    setenv("NOTIFY_SOCKET", path, 1);
    setenv("WATCHDOG_USEC", "5000000", 1);
    opcd_systemd_notify_t n;
    ASSERT(opcd_systemd_notify_init(&n) == 0, "initialize filesystem notification sender");
    ASSERT(n.fd >= 0, "notification sender opened");
    ASSERT((fcntl(n.fd, F_GETFL) & O_NONBLOCK) != 0, "sender is non-blocking");
    ASSERT((fcntl(n.fd, F_GETFD) & FD_CLOEXEC) != 0, "sender is close-on-exec");
    ASSERT(n.watchdog_enabled, "positive WATCHDOG_USEC enables watchdog");
    ASSERT(opcd_systemd_ready(&n) == 1, "READY datagram queued");
    ASSERT(recv_exact(receiver, "READY=1") == 0, "READY frame is exact");
    ASSERT(opcd_systemd_watchdog(&n) == 1, "WATCHDOG datagram queued");
    ASSERT(recv_exact(receiver, "WATCHDOG=1") == 0, "WATCHDOG frame is exact");

    opcd_systemd_notify_close(&n);
    cleanup_path_socket(receiver, path);
}

static void test_watchdog_environment(void)
{
    static const char *invalid_usec[] = { "", "0", "-1", "5x", "18446744073709551616" };
    for (size_t i = 0; i < sizeof invalid_usec / sizeof invalid_usec[0]; i++) {
        clear_env();
        char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
        int receiver = bind_path_socket(path, sizeof path);
        ASSERT(receiver >= 0, "bind invalid-usec receiver");
        if (receiver < 0) continue;
        setenv("NOTIFY_SOCKET", path, 1);
        setenv("WATCHDOG_USEC", invalid_usec[i], 1);
        opcd_systemd_notify_t n;
        ASSERT(opcd_systemd_notify_init(&n) == 0, "invalid WATCHDOG_USEC does not break READY");
        ASSERT(!n.watchdog_enabled, "invalid WATCHDOG_USEC disables watchdog");
        ASSERT(opcd_systemd_ready(&n) == 1, "READY remains enabled without watchdog");
        ASSERT(recv_exact(receiver, "READY=1") == 0, "READY received with watchdog disabled");
        ASSERT(opcd_systemd_watchdog(&n) == 0, "disabled watchdog sends nothing");
        opcd_systemd_notify_close(&n);
        cleanup_path_socket(receiver, path);
    }

    clear_env();
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int receiver = bind_path_socket(path, sizeof path);
    ASSERT(receiver >= 0, "bind pid-filter receiver");
    if (receiver < 0) return;
    setenv("NOTIFY_SOCKET", path, 1);
    setenv("WATCHDOG_USEC", "5000000", 1);
    char pid[32];
    snprintf(pid, sizeof pid, "%ld", (long)getpid() + 1);
    setenv("WATCHDOG_PID", pid, 1);
    opcd_systemd_notify_t n;
    ASSERT(opcd_systemd_notify_init(&n) == 0, "foreign WATCHDOG_PID accepted as non-watchdog mode");
    ASSERT(!n.watchdog_enabled, "foreign WATCHDOG_PID disables watchdog");
    opcd_systemd_notify_close(&n);

    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    setenv("WATCHDOG_PID", pid, 1);
    ASSERT(opcd_systemd_notify_init(&n) == 0, "current WATCHDOG_PID accepted");
    ASSERT(n.watchdog_enabled, "current WATCHDOG_PID enables watchdog");
    opcd_systemd_notify_close(&n);
    cleanup_path_socket(receiver, path);
}

static void test_abstract_socket(void)
{
    clear_env();
    char name[80];
    snprintf(name, sizeof name, "opcd-notify-%ld", (long)getpid());
    int receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    ASSERT(receiver >= 0, "open abstract notification receiver");
    if (receiver < 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    sa.sun_path[0] = '\0';
    memcpy(sa.sun_path + 1, name, strlen(name));
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    if (bind(receiver, (struct sockaddr *)&sa, len) != 0) {
        ASSERT(0, "bind abstract notification receiver");
        close(receiver);
        return;
    }

    char env[sizeof name + 1];
    snprintf(env, sizeof env, "@%s", name);
    setenv("NOTIFY_SOCKET", env, 1);
    opcd_systemd_notify_t n;
    ASSERT(opcd_systemd_notify_init(&n) == 0, "initialize abstract notification sender");
    ASSERT(opcd_systemd_ready(&n) == 1, "queue READY to abstract socket");
    ASSERT(recv_exact(receiver, "READY=1") == 0, "abstract READY frame is exact");
    opcd_systemd_notify_close(&n);
    close(receiver);
}

static void test_invalid_socket_addresses(void)
{
    clear_env();
    opcd_systemd_notify_t n;
    setenv("NOTIFY_SOCKET", "relative", 1);
    ASSERT(opcd_systemd_notify_init(&n) == -1, "relative socket address rejected");
    ASSERT(n.fd == -1, "invalid address leaves no open socket");

    char long_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 8];
    memset(long_path, 'x', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    setenv("NOTIFY_SOCKET", long_path, 1);
    ASSERT(opcd_systemd_notify_init(&n) == -1, "oversized socket address rejected");
    ASSERT(n.fd == -1, "oversized address leaves no open socket");
}

static void test_watchdog_survives_continuous_udp_ingress(void)
{
    clear_env();
    char notify_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int receiver = bind_path_socket(notify_path, sizeof notify_path);
    ASSERT(receiver >= 0, "bind watchdog fairness receiver");
    if (receiver < 0) return;

    char port[16];
    int port_rc = choose_udp_port(port, sizeof port);
    ASSERT(port_rc == 0, "choose control UDP port");
    if (port_rc != 0) {
        cleanup_path_socket(receiver, notify_path);
        return;
    }

    setenv("NOTIFY_SOCKET", notify_path, 1);
    setenv("WATCHDOG_USEC", "2000000", 1);
    pid_t daemon_pid = fork();
    ASSERT(daemon_pid >= 0, "fork opcd fairness daemon");
    if (daemon_pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execl(OPCD_TEST_BIN, OPCD_TEST_BIN, "-p", port,
              "-c", "/nonexistent/opcd-fairness.conf", (char *)NULL);
        _exit(127);
    }
    clear_env();
    if (daemon_pid < 0) {
        cleanup_path_socket(receiver, notify_path);
        return;
    }

    int ready = recv_exact_until(receiver, "READY=1", 3000);
    ASSERT(ready == 0, "opcd emits READY before ingress flood");

    enum { FLOODER_COUNT = 32 };
    pid_t flooders[FLOODER_COUNT];
    size_t flooder_count = 0;
    if (ready == 0) {
        for (size_t i = 0; i < FLOODER_COUNT; i++) {
            pid_t pid = spawn_udp_flooder(port);
            ASSERT(pid >= 0, "fork UDP ingress flooder");
            if (pid > 0) flooders[flooder_count++] = pid;
        }
        ASSERT(recv_exact_until(receiver, "WATCHDOG=1", 2500) == 0,
               "main loop services watchdog during continuous UDP ingress");
        ASSERT(kill(daemon_pid, 0) == 0,
               "opcd remains alive while servicing continuous UDP ingress");
    }

    for (size_t i = 0; i < flooder_count; i++) {
        kill(flooders[i], SIGTERM);
        (void)waitpid(flooders[i], NULL, 0);
    }
    kill(daemon_pid, SIGTERM);
    (void)waitpid(daemon_pid, NULL, 0);
    cleanup_path_socket(receiver, notify_path);
}

int main(void)
{
    test_standalone();
    test_filesystem_socket_and_frames();
    test_watchdog_environment();
    test_abstract_socket();
    test_invalid_socket_addresses();
    test_watchdog_survives_continuous_udp_ingress();
    clear_env();
    if (failures) {
        fprintf(stderr, "%d systemd-notify test(s) failed\n", failures);
        return 1;
    }
    puts("all systemd-notify tests passed");
    return 0;
}
