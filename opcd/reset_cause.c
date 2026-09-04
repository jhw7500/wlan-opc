#define _GNU_SOURCE   /* pipe2 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "reset_cause.h"
#include "../protocol/ids.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint32_t opcd_reset_cause_read(const char *path)
{
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return 0;
    char *p = line;
    while (isspace((unsigned char)*p)) p++;
    /* "0x.." hex or plain decimal — chosen explicitly so a leading zero can
     * never be read as octal, and no sign is accepted (strtoull would wrap a
     * negative into a valid-looking id). */
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!isxdigit((unsigned char)*p) || (base == 10 && !isdigit((unsigned char)*p))) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, base);
    if (end == p || errno != 0) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;                    /* trailing junk */
    if (v == 0 || v > 0xFFFFFFFFull) return 0;
    return (uint32_t)v;
}

int opcd_system_stopping(const char *systemctl_path, int timeout_ms)
{
    if (!systemctl_path || timeout_ms < 0) return -1;
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return -1;     /* no leak into other exec'd children */
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        /* child: stdout → pipe, stdin/stderr → /dev/null. opcd blocks
         * SIGINT/SIGTERM for its signalfd and the mask survives exec — give
         * systemctl a clean mask so it is not signal-immune. */
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDERR_FILENO); }
        dup2(pfd[1], STDOUT_FILENO);               /* dup2 clears O_CLOEXEC on the copy */
        execl(systemctl_path, "systemctl", "is-system-running", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    /* One DEADLINE for the whole probe (read + exit), not a per-read timer:
     * a dribbling writer must not stretch the shutdown. */
    const uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    char out[64];
    size_t n = 0;
    int rc = -1;
    for (;;) {
        uint64_t t = now_ms();
        if (t >= deadline) break;                                    /* timeout */
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int r = poll(&p, 1, (int)(deadline - t));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;                                           /* timeout / error */
        ssize_t k = read(pfd[0], out + n, sizeof out - 1 - n);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) break;                                            /* read error: unknown */
        if (k == 0) { rc = 0; break; }                               /* EOF: answer complete */
        n += (size_t)k;
        if (n >= sizeof out - 1) { rc = 0; break; }
    }
    close(pfd[0]);
    if (rc < 0) kill(pid, SIGKILL);                                  /* timeout/error: do not linger */

    /* Reap within the same deadline (EOF only proves the write end closed).
     * A child that outlives the deadline is killed and reaped — never a
     * zombie, never an unbounded wait. */
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) { rc = -1; break; }
        if (now_ms() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            rc = -1;
            break;
        }
        struct timespec nap = { 0, 5 * 1000 * 1000 };                 /* 5 ms */
        nanosleep(&nap, NULL);
    }
    if (rc < 0) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;  /* exec failed */
    out[n] = '\0';
    char *s = out;
    while (isspace((unsigned char)*s)) s++;
    return strncmp(s, "stopping", 8) == 0 &&
           (s[8] == '\0' || isspace((unsigned char)s[8]));
}

uint32_t opcd_shutdown_reset_cause(int stopping, uint32_t file_cause)
{
    if (stopping != 1) return 0;
    return file_cause ? file_cause : OPC_RESET_CAUSE_SYSTEM;
}
