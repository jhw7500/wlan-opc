#ifndef WLAN_OPC_OPCD_STORE_ASYNC_H
#define WLAN_OPC_OPCD_STORE_ASYNC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Asynchronous NVRAM writer (PERF-001).
 *
 * opcd is a single-threaded epoll loop; opc_store_write_atomic() ends in an
 * fsync() that on eMMC/NAND can stall for hundreds of ms (the spec budgets
 * up to OPC_TIMER_NVRAM_WRITE_S = 120 s for an NVRAM commit). Running it in
 * the dispatch path freezes every other socket, the indication tick and the
 * idle-logout timer for the duration.
 *
 * This module moves the write to a single worker thread:
 *
 *   main thread                       worker thread
 *   ───────────                       ─────────────
 *   opc_store_async_submit()  ──FIFO──▶ opc_store_write_atomic()
 *        (copies data)                       │
 *   epoll on event_fd  ◀──eventfd──── completion (result + errno)
 *   opc_store_async_drain()
 *
 * Jobs complete in submission order (single worker, FIFO queue), so two
 * writes to the same path can never be reordered. Submitted data is copied
 * into the job slot — the caller's buffer may change immediately after
 * submit returns.
 *
 * Capacity is fixed (OPC_STORE_ASYNC_QUEUE_DEPTH in-flight jobs). submit()
 * never blocks: it fails with EAGAIN when every slot is in use (queued or
 * completed-but-not-drained). Callers gate submissions so this is reached
 * only under a non-spec-compliant client (see opcd_pending_ack_t).
 *
 * Shutdown: opc_store_async_destroy() completes every queued job before
 * joining the worker — pending data still reaches NVRAM; only the
 * completion notifications are dropped.
 */

#define OPC_STORE_ASYNC_QUEUE_DEPTH 4
#define OPC_STORE_ASYNC_DATA_MAX    8192

typedef struct opc_store_async opc_store_async_t;

typedef struct opc_store_async_done {
    uint64_t token;        /* caller-chosen id passed to submit() */
    int      result;       /* 0 = written, -1 = failed */
    int      saved_errno;  /* errno of the failure; 0 on success */
} opc_store_async_done_t;

/* Create the writer (worker thread + eventfd). Returns NULL on failure —
 * callers are expected to fall back to synchronous writes. */
opc_store_async_t *opc_store_async_create(void);

/* Complete all queued jobs, join the worker, release resources.
 * NULL-safe. Undrained completions are discarded. */
void opc_store_async_destroy(opc_store_async_t *sa);

/* Pollable fd (eventfd): readable whenever at least one completion is
 * waiting to be drained. Register with epoll/poll for level-triggered use. */
int opc_store_async_event_fd(const opc_store_async_t *sa);

/*
 * Queue an atomic write of `len` bytes to `path` with final mode `mode`
 * (same contract as opc_store_write_atomic). `data` is copied.
 *
 * Returns 0 on enqueue. -1 with errno EINVAL (bad args), ENAMETOOLONG
 * (path too long), E2BIG (len > OPC_STORE_ASYNC_DATA_MAX), or EAGAIN
 * (all job slots in flight — drain first).
 */
int opc_store_async_submit(opc_store_async_t *sa, const char *path,
                           const void *data, size_t len, mode_t mode,
                           uint64_t token);

/* Harvest finished jobs into `out[cap]`, freeing their slots. Clears the
 * eventfd; re-arms it when more completions remain than `cap` allowed.
 * Returns the number of entries written. */
size_t opc_store_async_drain(opc_store_async_t *sa,
                             opc_store_async_done_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_STORE_ASYNC_H */
