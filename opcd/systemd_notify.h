#ifndef OPCD_SYSTEMD_NOTIFY_H
#define OPCD_SYSTEMD_NOTIFY_H

#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct opcd_systemd_notify {
    int fd;
    bool watchdog_enabled;
    struct sockaddr_un addr;
    socklen_t addr_len;
} opcd_systemd_notify_t;

/* Read systemd's notification environment and prepare a non-blocking
 * datagram sender. Missing NOTIFY_SOCKET is a supported standalone mode.
 * Returns 0 on success/standalone mode and -1 for an invalid socket address. */
int opcd_systemd_notify_init(opcd_systemd_notify_t *notify);

void opcd_systemd_notify_close(opcd_systemd_notify_t *notify);

/* Return 1 when the exact notification was queued, 0 when notifications (or
 * the watchdog for opcd_systemd_watchdog) are disabled, and -1 on send error. */
int opcd_systemd_ready(opcd_systemd_notify_t *notify);
int opcd_systemd_watchdog(opcd_systemd_notify_t *notify);

#endif
