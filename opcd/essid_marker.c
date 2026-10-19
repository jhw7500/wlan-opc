#define _POSIX_C_SOURCE 200809L
#include "essid_marker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_MAX_BYTES  (64 * 1024)   /* a wpa conf / marker is far smaller */
#define TOKEN_MAX       256           /* 32-byte SSID: quoted, escaped, or 64 hex */

static char *slurp(const char *path, int *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *err = errno; return NULL; }
    char *buf = malloc(FILE_MAX_BYTES + 1);
    if (!buf) { close(fd); *err = ENOMEM; return NULL; }
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, FILE_MAX_BYTES - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { *err = errno; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
        if (len == FILE_MAX_BYTES) { *err = EFBIG; free(buf); close(fd); return NULL; }
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

/* Tmp file in the destination directory, fsync, rename, fsync the directory:
 * the destination is either the old or the complete new content. */
static int write_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -ENAMETOOLONG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -errno;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int e = errno; close(fd); unlink(tmp); return -e; }
        off += (size_t)n;
    }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0) { int e = errno; close(fd); unlink(tmp); return -e; }
    if (close(fd) != 0) { int e = errno; unlink(tmp); return -e; }
    if (rename(tmp, path) != 0) { int e = errno; unlink(tmp); return -e; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *(slash == dir ? slash + 1 : slash) = '\0';
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    }
    return 0;
}

/* Locate the `ssid=` assignments (line start, optional blanks; `#ssid=`,
 * `scan_ssid=`, `bssid=` do not match). Returns how many there are; for the
 * first one reports the value span [*vs, *ve) (trailing CR/blanks excluded). */
static int find_ssid(const char *conf, const char **vs, const char **ve)
{
    int count = 0;
    for (const char *ln = conf; *ln; ) {
        const char *eol = strchr(ln, '\n');
        const char *end = eol ? eol : ln + strlen(ln);
        const char *p = ln;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (end - p >= 5 && strncmp(p, "ssid=", 5) == 0) {
            if (count == 0) {
                const char *e = end;
                while (e > p + 5 && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
                *vs = p + 5;
                *ve = e;
            }
            count++;
        }
        ln = eol ? eol + 1 : end;
    }
    return count;
}

/* Marker body: "essid=<orig>\nslot=N\ntime=T\n" and, once commit ran,
 * "applied=<token>\n". essid= must be the first line; the others are
 * single lines in any order. */
typedef struct {
    char essid[TOKEN_MAX];
    char applied[TOKEN_MAX];        /* "" until commit */
    unsigned slot;
    long long time;
} marker_t;

/* Copy the value of the line that starts with `key` (line start, no blanks).
 * 0 = found, -ENOENT = absent, -EINVAL = present but truncated/oversized. */
static int line_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *ln = body; *ln; ) {
        const char *eol = strchr(ln, '\n');
        if (strncmp(ln, key, kl) == 0) {
            if (!eol) return -EINVAL;
            size_t n = (size_t)(eol - (ln + kl));
            if (n == 0 || n >= cap) return -EINVAL;
            memcpy(out, ln + kl, n);
            out[n] = '\0';
            return 0;
        }
        if (!eol) break;
        ln = eol + 1;
    }
    return -ENOENT;
}

static int marker_parse(const char *body, marker_t *m)
{
    memset(m, 0, sizeof *m);
    if (strncmp(body, "essid=", 6) != 0) return -EINVAL;   /* not ours */
    if (line_value(body, "essid=", m->essid, sizeof m->essid) != 0) return -EINVAL;
    int rc = line_value(body, "applied=", m->applied, sizeof m->applied);
    if (rc == -EINVAL) return -EINVAL;
    char num[32];
    if (line_value(body, "slot=", num, sizeof num) == 0) m->slot = (unsigned)strtoul(num, NULL, 10);
    if (line_value(body, "time=", num, sizeof num) == 0) m->time = strtoll(num, NULL, 10);
    return 0;
}

static int marker_write(const char *path, const char *essid, unsigned slot_no,
                        long long now, const char *applied)
{
    char body[2 * TOKEN_MAX + 96];
    int bl = (applied && applied[0])
        ? snprintf(body, sizeof body, "essid=%s\nslot=%u\ntime=%lld\napplied=%s\n",
                   essid, slot_no, now, applied)
        : snprintf(body, sizeof body, "essid=%s\nslot=%u\ntime=%lld\n",
                   essid, slot_no, now);
    if (bl < 0 || (size_t)bl >= sizeof body) return -ENAMETOOLONG;
    return write_atomic(path, body, (size_t)bl, 0600);
}

/* The conf's single `ssid=` token. -EINVAL when there is not exactly one. */
static int conf_token(const char *conf_path, char *out, size_t cap)
{
    int err = 0;
    char *conf = slurp(conf_path, &err);
    if (!conf) return -err;
    const char *vs = NULL, *ve = NULL;
    int n = find_ssid(conf, &vs, &ve);
    size_t len = (n == 1) ? (size_t)(ve - vs) : 0;
    if (n != 1 || len == 0 || len >= cap) { free(conf); return -EINVAL; }
    memcpy(out, vs, len);
    out[len] = '\0';
    free(conf);
    return 0;
}

static int marker_unlink(const char *marker_path)
{
    if (unlink(marker_path) != 0 && errno != ENOENT) return -errno;
    return 0;
}

int opc_essid_marker_arm(const char *marker_path, const char *conf_path,
                         unsigned slot_no, time_t now)
{
    char cur[TOKEN_MAX];
    int rc = conf_token(conf_path, cur, sizeof cur);
    if (rc != 0) return rc;

    /* Keep the first original only while the conf still holds the value this
     * chain committed. Anything else (uncommitted marker, foreign change)
     * starts a new chain from the current conf value. */
    int err = 0, keep = 0;
    marker_t m;
    char *old = slurp(marker_path, &err);
    if (old) {
        if (marker_parse(old, &m) == 0 && m.applied[0] && strcmp(m.applied, cur) == 0) keep = 1;
        free(old);
    }
    if (keep) return marker_write(marker_path, m.essid, slot_no, (long long)now, m.applied);
    return marker_write(marker_path, cur, slot_no, (long long)now, NULL);
}

int opc_essid_marker_commit(const char *marker_path, const char *conf_path)
{
    int err = 0;
    char *old = slurp(marker_path, &err);
    if (!old) return -err;
    marker_t m;
    int rc = marker_parse(old, &m);
    free(old);
    if (rc != 0) return rc;
    char cur[TOKEN_MAX];
    rc = conf_token(conf_path, cur, sizeof cur);
    if (rc != 0) return rc;
    return marker_write(marker_path, m.essid, m.slot, m.time, cur);
}

int opc_essid_marker_disarm(const char *marker_path)
{
    int err = 0;
    char *old = slurp(marker_path, &err);
    if (!old) return err == ENOENT ? 0 : -err;
    marker_t m;
    int rc = marker_parse(old, &m);
    free(old);
    if (rc != 0) return rc;                         /* unusable: left for -R to report */
    if (m.applied[0]) return 0;                     /* earlier successful apply stays */
    return marker_unlink(marker_path);
}

int opc_essid_marker_restore(const char *marker_path, const char *conf_path)
{
    int err = 0;
    char *mk = slurp(marker_path, &err);
    if (!mk) return err == ENOENT ? 0 : -err;
    marker_t m;
    int rc = marker_parse(mk, &m);
    free(mk);
    if (rc != 0) return rc;

    char *conf = slurp(conf_path, &err);
    if (!conf) return -err;
    const char *vs = NULL, *ve = NULL;
    if (find_ssid(conf, &vs, &ve) != 1) { free(conf); return -EINVAL; }

    /* The conf must hold exactly what this chain applied (or, uncommitted,
     * still the original). Otherwise someone else wrote it, or the apply
     * never reached commit: never revert a change this marker did not make. */
    const char *expect = m.applied[0] ? m.applied : m.essid;
    size_t cl = (size_t)(ve - vs);
    if (cl != strlen(expect) || memcmp(vs, expect, cl) != 0) {
        free(conf);
        rc = marker_unlink(marker_path);
        return rc != 0 ? rc : 2;
    }
    if (!m.applied[0] || strcmp(m.applied, m.essid) == 0) {   /* nothing to put back */
        free(conf);
        return marker_unlink(marker_path);
    }

    size_t head = (size_t)(vs - conf), tail = strlen(ve);
    size_t tl = strlen(m.essid);
    char *out = malloc(head + tl + tail + 1);
    if (!out) { free(conf); return -ENOMEM; }
    memcpy(out, conf, head);
    memcpy(out + head, m.essid, tl);
    memcpy(out + head + tl, ve, tail);
    size_t outlen = head + tl + tail;
    free(conf);

    struct stat st;
    mode_t mode = (stat(conf_path, &st) == 0) ? (st.st_mode & 07777) : 0600;
    rc = write_atomic(conf_path, out, outlen, mode);
    free(out);
    if (rc != 0) return rc;                         /* marker kept: retry next boot */
    rc = marker_unlink(marker_path);
    return rc != 0 ? rc : 1;
}
