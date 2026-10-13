#define _GNU_SOURCE
#include "../systemd_notify.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

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

int main(void)
{
    test_standalone();
    test_filesystem_socket_and_frames();
    test_watchdog_environment();
    test_abstract_socket();
    test_invalid_socket_addresses();
    clear_env();
    if (failures) {
        fprintf(stderr, "%d systemd-notify test(s) failed\n", failures);
        return 1;
    }
    puts("all systemd-notify tests passed");
    return 0;
}
