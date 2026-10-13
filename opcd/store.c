#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "store.h"

int opc_store_write_atomic_ex(const char *path, const void *data, size_t len,
                              mode_t mode, opc_store_outcome_t *outcome)
{
    if (outcome) *outcome = OPC_STORE_UNCHANGED;
    if (!path || (len > 0 && !data)) { errno = EINVAL; return -1; }
    if (!mode) mode = 0600;

    char tmp_path[1024], parent[1024];
    int n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp_path) { errno = ENAMETOOLONG; return -1; }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        strcpy(parent, ".");
    } else if (slash == path) {
        strcpy(parent, "/");
    } else {
        size_t plen = (size_t)(slash - path);
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    /* Establish the directory handle before changing anything; a missing or
     * inaccessible parent cannot leave a freshly renamed unconfirmed target. */
    int dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return -1;
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        int e = errno;
        close(dirfd);
        errno = e;
        return -1;
    }
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, (const char *)data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            goto unchanged_failure;
        }
        written += (size_t)w;
    }
    if (fsync(fd) != 0) goto unchanged_failure;
    /* Linux releases the descriptor even if close reports an error. Never
     * close it again on that failure: another thread may have reused it. */
    int close_rc = close(fd);
    fd = -1;
    if (close_rc != 0) goto unchanged_failure;
    if (rename(tmp_path, path) != 0) goto unchanged_failure;

    if (outcome) *outcome = OPC_STORE_REPLACED_UNSYNCED;
    if (fsync(dirfd) != 0) {
        int e = errno;
        close(dirfd);
        errno = e;
        return -1;
    }
    if (close(dirfd) != 0) return -1;
    if (outcome) *outcome = OPC_STORE_DURABLE;
    return 0;

unchanged_failure: {
        int e = errno;
        if (fd >= 0) close(fd);
        unlink(tmp_path);
        close(dirfd);
        errno = e;
        return -1;
    }
}

int opc_store_write_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
    return opc_store_write_atomic_ex(path, data, len, mode, NULL);
}

ssize_t opc_store_read_all(const char *path, void *buf, size_t cap)
{
    if (!path || (cap > 0 && !buf)) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < cap) {
        ssize_t r = read(fd, (char *)buf + total, cap - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            int save = errno;
            close(fd);
            errno = save;
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    close(fd);
    return (ssize_t)total;
}
