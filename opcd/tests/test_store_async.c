/*
 * Unit tests for the async NVRAM writer (store_async.c, PERF-001).
 *
 * Covers the contract the deferred-ack path depends on: completion
 * notification via the eventfd, token round-trip, FIFO ordering for
 * same-path writes, error reporting (result + saved_errno), argument
 * validation, and queue-drain-on-destroy durability.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../store.h"
#include "../store_async.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

/* Wait for the writer's eventfd to become readable. */
static int wait_ready(opc_store_async_t *sa, int timeout_ms)
{
    struct pollfd pfd = {
        .fd     = opc_store_async_event_fd(sa),
        .events = POLLIN,
    };
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return (r == 1 && (pfd.revents & POLLIN)) ? 0 : -1;
}

/* Drain until `want` completions arrive (or a poll times out). */
static size_t drain_n(opc_store_async_t *sa, opc_store_async_done_t *out,
                      size_t want)
{
    size_t got = 0;
    while (got < want) {
        if (wait_ready(sa, 5000) != 0) break;
        got += opc_store_async_drain(sa, out + got,
                                     OPC_STORE_ASYNC_QUEUE_DEPTH - got);
    }
    return got;
}

static ssize_t read_file(const char *path, char *buf, size_t cap)
{
    return opc_store_read_all(path, buf, cap);
}

int main(void)
{
    /* CWD-relative paths (`make check` runs with CWD opcd/tests/), avoiding
     * the predictable-/tmp-path symlink class (CWE-377). Cleaned by unlink
     * below on a passing run. */
    char path_a[128], path_bad[160];
    snprintf(path_a, sizeof path_a, "test_store_async_a_%d.tmp", (int)getpid());
    snprintf(path_bad, sizeof path_bad, "no_such_dir_%d/file.tmp", (int)getpid());
    unlink(path_a);

    opc_store_async_t *sa = opc_store_async_create();
    ASSERT(sa != NULL, "create succeeds");
    if (!sa) return 1;
    ASSERT(opc_store_async_event_fd(sa) >= 0, "event fd valid");

    /* 1. Single write completes, token round-trips, file content lands. */
    ASSERT(opc_store_async_submit(sa, path_a, "hello", 5, 0644, 42u) == 0,
           "submit ok");
    opc_store_async_done_t done[OPC_STORE_ASYNC_QUEUE_DEPTH];
    size_t n = drain_n(sa, done, 1);
    ASSERT(n == 1, "one completion drained");
    ASSERT(n == 1 && done[0].token == 42u, "token round-trip");
    ASSERT(n == 1 && done[0].result == 0, "write succeeded");
    char buf[64] = {0};
    ASSERT(read_file(path_a, buf, sizeof buf - 1) == 5 && strcmp(buf, "hello") == 0,
           "file content written");

    /* 2. Submitted data is copied — mutating the source after submit must
     *    not affect the write. */
    char volatile_buf[16];
    strcpy(volatile_buf, "first");
    ASSERT(opc_store_async_submit(sa, path_a, volatile_buf, 5, 0644, 1u) == 0,
           "submit (copy semantics) ok");
    strcpy(volatile_buf, "XXXXX");
    n = drain_n(sa, done, 1);
    memset(buf, 0, sizeof buf);
    ASSERT(n == 1 && read_file(path_a, buf, sizeof buf - 1) == 5 &&
           strcmp(buf, "first") == 0,
           "data copied at submit time");

    /* 3. FIFO ordering — same path, multiple writes: the last submit wins. */
    ASSERT(opc_store_async_submit(sa, path_a, "one", 3, 0644, 1u) == 0 &&
           opc_store_async_submit(sa, path_a, "two", 3, 0644, 2u) == 0 &&
           opc_store_async_submit(sa, path_a, "three", 5, 0644, 3u) == 0,
           "three same-path submits ok");
    n = drain_n(sa, done, 3);
    ASSERT(n == 3, "three completions drained");
    memset(buf, 0, sizeof buf);
    ASSERT(read_file(path_a, buf, sizeof buf - 1) == 5 && strcmp(buf, "three") == 0,
           "FIFO: last submit is the final file content");

    /* 4. A failing write reports result -1 + errno (missing directory). */
    ASSERT(opc_store_async_submit(sa, path_bad, "x", 1, 0644, 7u) == 0,
           "submit to bad path accepted (fails at write time)");
    n = drain_n(sa, done, 1);
    ASSERT(n == 1 && done[0].token == 7u && done[0].result == -1,
           "bad path reports failure");
    ASSERT(n == 1 && done[0].saved_errno == ENOENT, "bad path errno ENOENT");

    /* 5. Argument validation fails fast (no completion involved). */
    ASSERT(opc_store_async_submit(sa, NULL, "x", 1, 0644, 0) == -1 &&
           errno == EINVAL, "NULL path rejected EINVAL");
    static uint8_t big[OPC_STORE_ASYNC_DATA_MAX + 1];
    ASSERT(opc_store_async_submit(sa, path_a, big, sizeof big, 0644, 0) == -1 &&
           errno == E2BIG, "oversized blob rejected E2BIG");

    /* 6. destroy() completes queued jobs before joining (durability). */
    ASSERT(opc_store_async_submit(sa, path_a, "final", 5, 0644, 9u) == 0,
           "submit before destroy ok");
    opc_store_async_destroy(sa);
    memset(buf, 0, sizeof buf);
    ASSERT(read_file(path_a, buf, sizeof buf - 1) == 5 && strcmp(buf, "final") == 0,
           "destroy drains the queue to disk");

    /* 7. NULL-safety of the accessors. */
    ASSERT(opc_store_async_event_fd(NULL) == -1, "event_fd(NULL) is -1");
    opc_store_async_destroy(NULL);   /* must not crash */
    ASSERT(1, "destroy(NULL) is a no-op");

    unlink(path_a);

    if (failures == 0) {
        fprintf(stdout, "all store_async tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
