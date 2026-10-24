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
    char path_a[128], path_reserved[128], path_unsubmitted[128], path_bad[160];
    snprintf(path_a, sizeof path_a, "test_store_async_a_%d.tmp", (int)getpid());
    snprintf(path_reserved, sizeof path_reserved,
             "test_store_async_reserved_%d.tmp", (int)getpid());
    snprintf(path_unsubmitted, sizeof path_unsubmitted,
             "test_store_async_unsubmitted_%d.tmp", (int)getpid());
    snprintf(path_bad, sizeof path_bad, "no_such_dir_%d/file.tmp", (int)getpid());
    unlink(path_a);
    unlink(path_reserved);
    unlink(path_unsubmitted);

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

    /* 5b. Saturation contract (#140). A slot stays occupied from submit until
     *     drain (JOB_QUEUED and JOB_DONE both hold it), so submitting exactly
     *     OPC_STORE_ASYNC_QUEUE_DEPTH times without draining must be accepted
     *     in full and the next submit must fail EAGAIN — the EAGAIN edge the
     *     handler's bounded wait exists to absorb, previously untested.
     *
     *     The depth floor is asserted as a literal on purpose: every other
     *     reference uses the symbol and would re-scale silently, so a revert
     *     to the pre-#140 depth of 4 would go unnoticed. Depth is what keeps a
     *     saturating-but-spec-compliant client (store_async.h) from driving
     *     opcd's dispatch loop into the wait in handler.c persist_blob. */
    ASSERT(OPC_STORE_ASYNC_QUEUE_DEPTH >= 16,
           "queue depth floor is 16 in-flight writes");
    {
        int accepted = 0;
        for (int i = 0; i < OPC_STORE_ASYNC_QUEUE_DEPTH; i++)
            if (opc_store_async_submit(sa, path_a, "sat", 3, 0644,
                                       (uint64_t)(100 + i)) == 0)
                accepted++;
        ASSERT(accepted == OPC_STORE_ASYNC_QUEUE_DEPTH,
               "every slot accepts a submit before any drain");
        ASSERT(opc_store_async_submit(sa, path_a, "sat", 3, 0644, 999u) == -1 &&
               errno == EAGAIN,
               "submit past the last slot fails EAGAIN");
        size_t sat = drain_n(sa, done, OPC_STORE_ASYNC_QUEUE_DEPTH);
        ASSERT(sat == (size_t)OPC_STORE_ASYNC_QUEUE_DEPTH,
               "saturated queue drains every completion");
    }

    /* 5c. Two-phase admission reserves real capacity before the handler
     *     mutates live state. A reservation neither reaches the worker nor
     *     emits a completion; cancel releases exactly that capacity. */
    {
        int reservation = opc_store_async_reserve(sa, path_reserved, 5);
        ASSERT(reservation >= 0, "reserve validates path/length and claims a slot");
        int accepted = 0;
        for (int i = 0; i < OPC_STORE_ASYNC_QUEUE_DEPTH - 1; i++)
            if (opc_store_async_submit(sa, path_a, "held", 4, 0644,
                                       (uint64_t)(200 + i)) == 0)
                accepted++;
        ASSERT(accepted == OPC_STORE_ASYNC_QUEUE_DEPTH - 1,
               "reservation counts against queue capacity");
        ASSERT(opc_store_async_submit(sa, path_a, "full", 4, 0644, 999u) == -1 &&
               errno == EAGAIN,
               "normal submit sees reserved slot as occupied");
        opc_store_async_cancel(sa, reservation);
        ASSERT(opc_store_async_submit(sa, path_a, "freed", 5, 0644, 299u) == 0,
               "cancel makes the reserved capacity reusable");
        n = drain_n(sa, done, OPC_STORE_ASYNC_QUEUE_DEPTH);
        ASSERT(n == OPC_STORE_ASYNC_QUEUE_DEPTH,
               "cancelled-reservation saturation drains submitted jobs only");
        ASSERT(access(path_reserved, F_OK) != 0,
               "cancelled reservation never reaches persistent storage");
    }

    /* 5d. submit_reserved accepts up to its captured length (#194: the IP
     *     list text is sized only after the mutation), consumes the handle
     *     exactly once, and retains the reservation after a rejected length
     *     so the owner can still submit or cancel it deliberately. */
    {
        int reservation = opc_store_async_reserve(sa, path_reserved, 5);
        ASSERT(reservation >= 0, "second reservation succeeds");
        ASSERT(opc_store_async_submit_reserved(sa, reservation,
                                               "toolong", 7, 0644, 300u) == -1 &&
               errno == EINVAL,
               "reserved submit rejects a length beyond the admission");
        ASSERT(opc_store_async_submit_reserved(sa, reservation,
                                               "right", 5, 0644, 301u) == 0,
               "reservation remains usable after rejected submit");
        ASSERT(opc_store_async_submit_reserved(sa, reservation,
                                               "again", 5, 0644, 302u) == -1 &&
               errno == EINVAL,
               "submitted reservation cannot be reused");
        n = drain_n(sa, done, 1);
        memset(buf, 0, sizeof buf);
        ASSERT(n == 1 && done[0].token == 301u &&
               read_file(path_reserved, buf, sizeof buf - 1) == 5 &&
               strcmp(buf, "right") == 0,
               "reserved submit preserves token and writes captured path");
    }

    /* 5d'. A shorter submission than reserved writes exactly its own bytes. */
    {
        int reservation = opc_store_async_reserve(sa, path_reserved, 24);
        ASSERT(reservation >= 0 &&
               opc_store_async_submit_reserved(sa, reservation, "ok", 2, 0644, 303u) == 0,
               "reserved submit accepts a length below the admission");
        n = drain_n(sa, done, 1);
        memset(buf, 0, sizeof buf);
        ASSERT(n == 1 && done[0].token == 303u &&
               read_file(path_reserved, buf, sizeof buf - 1) == 2 && strcmp(buf, "ok") == 0,
               "a shorter reserved submit writes only its own length");
    }

    /* Reservations do not impose worker order.  Jobs enter FIFO when they are
     * submitted, even when their slot indexes were reserved in the opposite
     * order. */
    {
        int first_reserved = opc_store_async_reserve(sa, path_reserved, 5);
        int second_reserved = opc_store_async_reserve(sa, path_reserved, 6);
        ASSERT(first_reserved >= 0 && second_reserved >= 0,
               "two ordered reservations succeed");
        ASSERT(opc_store_async_submit_reserved(sa, second_reserved,
                                               "second", 6, 0644, 310u) == 0 &&
               opc_store_async_submit_reserved(sa, first_reserved,
                                               "first", 5, 0644, 311u) == 0,
               "reserved jobs submit opposite reservation order");
        n = drain_n(sa, done, 2);
        memset(buf, 0, sizeof buf);
        ASSERT(n == 2 && read_file(path_reserved, buf, sizeof buf - 1) == 5 &&
               strcmp(buf, "first") == 0,
               "reserved jobs run in submit order, not slot order");
    }

    /* 5e. Reserve performs predictable validation before caller mutation. */
    {
        static char too_long[1025];
        memset(too_long, 'x', sizeof too_long - 1);
        too_long[sizeof too_long - 1] = '\0';
        ASSERT(opc_store_async_reserve(NULL, path_a, 1) == -1 && errno == EINVAL,
               "reserve rejects NULL store");
        ASSERT(opc_store_async_reserve(sa, NULL, 1) == -1 && errno == EINVAL,
               "reserve rejects NULL path");
        ASSERT(opc_store_async_reserve(sa, path_a,
                                       OPC_STORE_ASYNC_DATA_MAX + 1) == -1 &&
               errno == E2BIG,
               "reserve rejects oversized blob");
        ASSERT(opc_store_async_reserve(sa, too_long, 1) == -1 &&
               errno == ENAMETOOLONG,
               "reserve rejects overlong path");
        opc_store_async_cancel(NULL, -1);
        ASSERT(1, "cancel is NULL/invalid-handle safe");
    }

    /* 6. destroy() completes queued jobs before joining (durability). */
    ASSERT(opc_store_async_submit(sa, path_a, "final", 5, 0644, 9u) == 0,
           "submit before destroy ok");
    opc_store_async_destroy(sa);
    memset(buf, 0, sizeof buf);
    ASSERT(read_file(path_a, buf, sizeof buf - 1) == 5 && strcmp(buf, "final") == 0,
           "destroy drains the queue to disk");

    /* Shutdown drains submitted work but does not turn a reservation into a
     * write or wait for its owner (dispatch reservations never cross turns). */
    opc_store_async_t *shutdown_sa = opc_store_async_create();
    ASSERT(shutdown_sa != NULL, "shutdown reservation rig created");
    if (shutdown_sa) {
        int reservation = opc_store_async_reserve(shutdown_sa,
                                                  path_unsubmitted, 8);
        ASSERT(reservation >= 0, "unsubmitted shutdown reservation created");
        ASSERT(opc_store_async_submit(shutdown_sa, path_a,
                                      "shutdown", 8, 0644, 400u) == 0,
               "queued shutdown write accepted beside reservation");
        opc_store_async_finish(shutdown_sa);
        ASSERT(opc_store_async_event_fd(shutdown_sa) >= 0,
               "finish retains completion channel for reconciliation");
        n = drain_n(shutdown_sa, done, 1);
        ASSERT(n == 1 && done[0].token == 400u && done[0].result == 0 &&
               done[0].outcome == OPC_STORE_DURABLE,
               "finish retains submitted result and durability outcome");
        ASSERT(opc_store_async_submit(shutdown_sa, path_a, "late", 4, 0644, 401u) == -1 &&
               errno == ECANCELED,
               "finished writer refuses new admission");
        opc_store_async_finish(shutdown_sa);
        opc_store_async_destroy(shutdown_sa);
        memset(buf, 0, sizeof buf);
        ASSERT(read_file(path_a, buf, sizeof buf - 1) == 8 &&
               strcmp(buf, "shutdown") == 0,
               "destroy drains queued work beside reservation");
        ASSERT(access(path_unsubmitted, F_OK) != 0,
               "destroy discards unsubmitted reservation without writing");
    }

    /* 7. NULL-safety of the accessors. */
    ASSERT(opc_store_async_event_fd(NULL) == -1, "event_fd(NULL) is -1");
    opc_store_async_destroy(NULL);   /* must not crash */
    ASSERT(1, "destroy(NULL) is a no-op");

    unlink(path_a);
    unlink(path_reserved);
    unlink(path_unsubmitted);

    if (failures == 0) {
        fprintf(stdout, "all store_async tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
