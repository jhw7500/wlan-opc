#define _POSIX_C_SOURCE 200809L
#include "systemd_notify.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool parse_positive_decimal(const char *text, uint64_t *value)
{
    if (!text || !*text)
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < '0' || *p > '9')
            return false;

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || parsed == 0)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool watchdog_pid_matches(void)
{
    const char *text = getenv("WATCHDOG_PID");
    if (!text)
        return true;

    uint64_t pid;
    return parse_positive_decimal(text, &pid) && pid == (uint64_t)getpid();
}

int opcd_systemd_notify_init(opcd_systemd_notify_t *notify)
{
    if (!notify) {
        errno = EINVAL;
        return -1;
    }
    memset(notify, 0, sizeof *notify);
    notify->fd = -1;

    const char *path = getenv("NOTIFY_SOCKET");
    if (!path)
        return 0;

    size_t path_len = strlen(path);
    if (path_len == 0 || (path[0] != '/' && path[0] != '@')) {
        errno = EINVAL;
        return -1;
    }
    if (path_len >= sizeof notify->addr.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    notify->addr.sun_family = AF_UNIX;
    if (path[0] == '@') {
        notify->addr.sun_path[0] = '\0';
        memcpy(notify->addr.sun_path + 1, path + 1, path_len - 1);
        notify->addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len);
    } else {
        memcpy(notify->addr.sun_path, path, path_len + 1);
        notify->addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    }

    notify->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (notify->fd < 0)
        return -1;

    uint64_t watchdog_usec;
    notify->watchdog_enabled =
        parse_positive_decimal(getenv("WATCHDOG_USEC"), &watchdog_usec) &&
        watchdog_pid_matches();
    return 0;
}

void opcd_systemd_notify_close(opcd_systemd_notify_t *notify)
{
    if (!notify)
        return;
    if (notify->fd >= 0)
        close(notify->fd);
    notify->fd = -1;
    notify->watchdog_enabled = false;
}

static int send_state(opcd_systemd_notify_t *notify, const char *state)
{
    if (!notify || notify->fd < 0)
        return 0;
    size_t len = strlen(state);
    ssize_t sent = sendto(notify->fd, state, len, MSG_NOSIGNAL,
                          (const struct sockaddr *)&notify->addr,
                          notify->addr_len);
    if (sent < 0)
        return -1;
    if ((size_t)sent != len) {
        errno = EIO;
        return -1;
    }
    return 1;
}

int opcd_systemd_ready(opcd_systemd_notify_t *notify)
{
    return send_state(notify, "READY=1");
}

int opcd_systemd_watchdog(opcd_systemd_notify_t *notify)
{
    if (!notify || !notify->watchdog_enabled)
        return 0;
    return send_state(notify, "WATCHDOG=1");
}
