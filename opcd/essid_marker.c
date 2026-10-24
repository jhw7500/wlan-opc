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

/* Marker body: "essid=<orig>\nslot=N\ntime=T\n", optionally "target=<hex of
 * the requested SSID bytes>\n" (arm) and "applied=<token>\n" (commit/settle).
 * essid= must be the first line; the others are single lines in any order. */
typedef struct {
    char essid[TOKEN_MAX];
    char applied[TOKEN_MAX];        /* "" until commit/settle */
    char target_hex[TOKEN_MAX];     /* "" when arm had no requested SSID */
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
    if (line_value(body, "applied=", m->applied, sizeof m->applied) == -EINVAL) return -EINVAL;
    if (line_value(body, "target=", m->target_hex, sizeof m->target_hex) == -EINVAL) return -EINVAL;
    char num[32];
    if (line_value(body, "slot=", num, sizeof num) == 0) m->slot = (unsigned)strtoul(num, NULL, 10);
    if (line_value(body, "time=", num, sizeof num) == 0) m->time = strtoll(num, NULL, 10);
    return 0;
}

static int marker_write(const char *path, const marker_t *m)
{
    char body[3 * TOKEN_MAX + 96];
    int bl = snprintf(body, sizeof body, "essid=%s\nslot=%u\ntime=%lld\n%s%s%s%s%s%s",
                      m->essid, m->slot, m->time,
                      m->target_hex[0] ? "target=" : "", m->target_hex, m->target_hex[0] ? "\n" : "",
                      m->applied[0] ? "applied=" : "", m->applied, m->applied[0] ? "\n" : "");
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

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Does the conf token denote exactly the SSID bytes in target_hex? The helper
 * writes "<ssid>" when the SSID is safe to quote and bare hex otherwise
 * (wifi_ssid_to_conf_value); wpa_supplicant reads a quoted value literally.
 * P"..." (printf-escaped) is not produced by the helper and never matches. */
static int token_is_ssid(const char *tok, const char *target_hex)
{
    size_t tl = strlen(tok), hl = strlen(target_hex);
    if (hl == 0 || hl % 2) return 0;
    if (tl >= 2 && tok[0] == '"' && tok[tl - 1] == '"') {
        if (tl - 2 != hl / 2) return 0;
        for (size_t i = 0; i < hl / 2; i++) {
            int hi = hexval(target_hex[2 * i]), lo = hexval(target_hex[2 * i + 1]);
            if (hi < 0 || lo < 0 || (unsigned char)tok[1 + i] != (unsigned)(hi * 16 + lo)) return 0;
        }
        return 1;
    }
    if (tl != hl) return 0;
    for (size_t i = 0; i < hl; i++) {
        int a = hexval(tok[i]), b = hexval(target_hex[i]);
        if (a < 0 || b < 0 || a != b) return 0;
    }
    return 1;
}

int opc_wpa_conf_ssid(const char *conf_path, char *out, size_t cap)
{
    if (!out || cap == 0) return -EINVAL;
    out[0] = '\0';
    int err = 0;
    char *conf = slurp(conf_path, &err);
    if (!conf) return -err;
    const char *vs = NULL, *ve = NULL;
    size_t tl = find_ssid(conf, &vs, &ve) > 0 ? (size_t)(ve - vs) : 0;
    if (tl >= 2 && vs[0] == '"' && vs[tl - 1] == '"') {
        if (tl - 2 < cap) {
            memcpy(out, vs + 1, tl - 2);
            out[tl - 2] = '\0';
        }
    } else if (tl > 0 && tl % 2 == 0 && tl / 2 < cap) {
        size_t n = tl / 2, i;
        for (i = 0; i < n; i++) {
            int hi = hexval(vs[2 * i]), lo = hexval(vs[2 * i + 1]);
            if (hi < 0 || lo < 0 || hi * 16 + lo == 0) break;
            out[i] = (char)(hi * 16 + lo);
        }
        out[i == n ? n : 0] = '\0';
    }
    free(conf);
    return 0;
}

static int load_marker(const char *marker_path, marker_t *m)
{
    int err = 0;
    char *body = slurp(marker_path, &err);
    if (!body) return -err;
    int rc = marker_parse(body, m);
    free(body);
    return rc;
}

int opc_essid_marker_arm(const char *marker_path, const char *conf_path,
                         unsigned slot_no, time_t now, const char *requested_ssid)
{
    char cur[TOKEN_MAX];
    int rc = conf_token(conf_path, cur, sizeof cur);
    if (rc != 0) return rc;

    /* Keep the first original while the conf holds a value this chain wrote:
     * the committed token, or the SSID it last requested (an apply whose
     * commit never ran — crash or write error between the helper's exit and
     * commit/settle). The same ownership predicate restore() uses. Anything
     * else is another writer's value and starts a new chain from it. */
    marker_t m, old;
    memset(&m, 0, sizeof m);
    if (load_marker(marker_path, &old) == 0 &&
        ((old.applied[0] && strcmp(old.applied, cur) == 0) ||
         (old.target_hex[0] && token_is_ssid(cur, old.target_hex)))) {
        snprintf(m.essid, sizeof m.essid, "%s", old.essid);
        snprintf(m.applied, sizeof m.applied, "%s", cur);   /* a late commit of what landed */
    } else {
        snprintf(m.essid, sizeof m.essid, "%s", cur);
    }
    m.slot = slot_no;
    m.time = (long long)now;
    if (requested_ssid && requested_ssid[0]) {
        size_t n = strlen(requested_ssid);
        if (2 * n >= sizeof m.target_hex) return -ENAMETOOLONG;
        for (size_t i = 0; i < n; i++)
            snprintf(m.target_hex + 2 * i, 3, "%02x", (unsigned char)requested_ssid[i]);
    }
    return marker_write(marker_path, &m);
}

int opc_essid_marker_commit(const char *marker_path, const char *conf_path)
{
    marker_t m;
    int rc = load_marker(marker_path, &m);
    if (rc != 0) return rc;
    rc = conf_token(conf_path, m.applied, sizeof m.applied);
    if (rc != 0) return rc;
    return marker_write(marker_path, &m);
}

int opc_essid_marker_disarm(const char *marker_path)
{
    marker_t m;
    int rc = load_marker(marker_path, &m);
    if (rc == -ENOENT) return 0;
    if (rc != 0) return rc;                         /* unusable: left for -R to report */
    if (m.applied[0]) return 0;                     /* earlier successful apply stays */
    return marker_unlink(marker_path);
}

int opc_essid_marker_settle(const char *marker_path, const char *conf_path)
{
    marker_t m;
    int rc = load_marker(marker_path, &m);
    if (rc == -ENOENT) return 0;
    if (rc != 0) return rc;
    char cur[TOKEN_MAX];
    rc = conf_token(conf_path, cur, sizeof cur);
    if (rc != 0) return rc;
    const char *pre = m.applied[0] ? m.applied : m.essid;
    if (strcmp(cur, pre) == 0) {                    /* the helper changed nothing */
        if (!m.applied[0]) return marker_unlink(marker_path);
        /* Committed chain: the failed request's target identifies nothing
         * this chain wrote any more — drop it, or a later writer storing that
         * SSID would be reverted at boot. */
        m.target_hex[0] = '\0';
        return marker_write(marker_path, &m);
    }
    if (m.target_hex[0] && token_is_ssid(cur, m.target_hex)) {
        /* The helper installed the requested SSID before failing (rollback
         * failure, TERM at the timeout): keep it restorable. */
        snprintf(m.applied, sizeof m.applied, "%s", cur);
        rc = marker_write(marker_path, &m);
        return rc != 0 ? rc : 1;
    }
    /* Someone else wrote the conf while the apply was blocked or failing
     * (the helper only ever writes the requested SSID): not ours. An
     * uncommitted marker goes; a committed chain stays without the target,
     * so -R drops it as a foreign change instead of reverting it. */
    if (!m.applied[0]) {
        rc = marker_unlink(marker_path);
        return rc != 0 ? rc : 2;
    }
    m.target_hex[0] = '\0';
    rc = marker_write(marker_path, &m);
    return rc != 0 ? rc : 2;
}

int opc_essid_marker_restore(const char *marker_path, const char *conf_path)
{
    marker_t m;
    int rc = load_marker(marker_path, &m);
    if (rc == -ENOENT) return 0;
    if (rc != 0) return rc;

    int err = 0;
    char *conf = slurp(conf_path, &err);
    if (!conf) return -err;
    const char *vs = NULL, *ve = NULL;
    if (find_ssid(conf, &vs, &ve) != 1) { free(conf); return -EINVAL; }
    size_t cl = (size_t)(ve - vs);
    if (cl >= TOKEN_MAX) { free(conf); return -EINVAL; }
    char cur[TOKEN_MAX];
    memcpy(cur, vs, cl);
    cur[cl] = '\0';

    if (strcmp(cur, m.essid) == 0) {                /* already the original */
        free(conf);
        return marker_unlink(marker_path);
    }
    /* Revert only a value this chain wrote: the committed token, or (an apply
     * interrupted between the conf install and commit) the requested SSID. */
    int ours = (m.applied[0] && strcmp(cur, m.applied) == 0) ||
               (m.target_hex[0] && token_is_ssid(cur, m.target_hex));
    if (!ours) {
        free(conf);
        rc = marker_unlink(marker_path);
        return rc != 0 ? rc : 2;
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
