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

int opc_store_write_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
    if (!path || (len > 0 && !data)) {
        errno = EINVAL;
        return -1;
    }
    if (!mode) mode = 0600;

    char tmp_path[1024];
    int n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp_path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, (const char *)data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            int save = errno;
            close(fd);
            unlink(tmp_path);
            errno = save;
            return -1;
        }
        written += (size_t)w;
    }
    if (fsync(fd) != 0) {
        int save = errno;
        close(fd);
        unlink(tmp_path);
        errno = save;
        return -1;
    }
    if (close(fd) != 0) {
        int save = errno;
        unlink(tmp_path);
        errno = save;
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        int save = errno;
        unlink(tmp_path);
        errno = save;
        return -1;
    }
    return 0;
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
