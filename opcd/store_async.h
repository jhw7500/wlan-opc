#ifndef WLAN_OPC_OPCD_STORE_ASYNC_H
#define WLAN_OPC_OPCD_STORE_ASYNC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "store.h"

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
 * never blocks: it fails with EAGAIN when every slot is in use (reserved, queued or
 * completed-but-not-drained).
 *
 * Saturation is NOT limited to a non-compliant client (the earlier claim here
 * was wrong, #140). §4.1.3.1 only makes the VHL wait for a RESPONSE before
 * sending the next request — it says nothing about how many jobs a request
 * leaves behind. A SetIpConfigList whose tail entry fails after an earlier
 * one committed answers NG immediately and still queues a no-ack write (the
 * no_ack argument of handler.c persist_ip_list). Back-to-back frames like
 * that used to fill every slot; since #144 handler.c admits a persisted
 * resource (password, IP list, radio) only while it has no outstanding job
 * (persist_resource_busy), so they no longer pile up. An IP list completion
 * also queues a log-only copy to the customer file (#194).
 *
 * Shutdown: opc_store_async_destroy() completes every queued job before
 * joining the worker — pending data still reaches NVRAM; only the
 * completion notifications are dropped.
 */

/* Accepted trade-off of the 4 -> 16 raise (#140, reviewer B): depth is also
 * the abrupt-termination loss window. opc_store_write_atomic has no
 * write-ahead log, so a job's data exists only in its slot until that job's
 * own write+fsync+rename lands, and the queue is drained before exit ONLY on
 * the graceful path (the signalfd SIGTERM/SIGINT branch only ends the main
 * loop; opcd.c then finishes, reconciles and destroys the queue at the end of
 * main). On SIGKILL or power loss every still
 * queued job is lost silently, with no on-disk trace and no reconciliation at
 * next boot. The raise widens that worst case from 4 writes to 16 (bounded by
 * OPC_STORE_ASYNC_DATA_MAX: 32 KiB -> 128 KiB; the largest blob actually
 * persisted is sizeof(opcd_ip_list_t) = 6784 B, so ~106 KiB in practice).
 * #194 raised DATA_MAX to 24 KiB for the text IP list (at most 21,604 B):
 * 16 x 24 KiB = 384 KiB of job buffers. Since #144 (above) the jobs actually
 * outstanding are at most one per persisted resource plus customer-file
 * copies, so that is the loss window, not 16.
 *
 * Accepted rather than capped: reaching it needs an abrupt kill DURING a
 * saturating burst, the lost writes are config the VHL can re-send, and the
 * alternative at the time — keeping depth at 4 — preserved the dispatch
 * stall. Issue #144 now reserves capacity before mutation and relies on
 * client timeout/retransmission when full, with no dispatch wait. #178
 * concluded that no write-ahead log is needed; the one remaining cross-file
 * window is documented in docs/testing/persistence-outcomes.md ("Radio
 * cross-file window"). */
#define OPC_STORE_ASYNC_QUEUE_DEPTH 16
#define OPC_STORE_ASYNC_DATA_MAX    24576

typedef struct opc_store_async opc_store_async_t;

typedef struct opc_store_async_done {
    uint64_t token;        /* caller-chosen id passed to submit() */
    int      result;       /* 0 = written, -1 = failed */
    int      saved_errno;  /* errno of the failure; 0 on success */
    opc_store_outcome_t outcome; /* replacement vs durability, even on failure */
} opc_store_async_done_t;

/* Create the writer (worker thread + eventfd). Returns NULL on failure —
 * callers are expected to fall back to synchronous writes. */
opc_store_async_t *opc_store_async_create(void);

/* Stop admission and join the worker after its queued writes finish, but
 * retain results/eventfd so the single lifecycle owner can harvest and
 * reconcile before destroy. NULL-safe and repeatable by that owner; lifecycle
 * calls must not run concurrently with one another. */
void opc_store_async_finish(opc_store_async_t *sa);

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

/* submit(), then — once the write replaced `path` (rename done, even if the
 * directory sync failed) — the same bytes to `echo_path` with the same mode.
 * The echo is a record of what `path` now holds (#194: the IP list text opcd
 * last put in the customer file); its failure is only logged and does not
 * change the job's result. */
int opc_store_async_submit_echo(opc_store_async_t *sa, const char *path,
                                const char *echo_path, const void *data,
                                size_t len, mode_t mode, uint64_t token);

/* Reserve capacity BEFORE a caller changes settings. Validates and captures
 * path/len; returns a slot >=0, or -1 with the same errors as submit().
 * `len` is the most the caller will submit: submit_reserved takes any length
 * up to it (a text whose final size is known only after the mutation).
 * Reserved slots count toward the fixed capacity but never reach the worker.
 * A caller owns its handle until exactly one submit_reserved or cancel.
 * The handler consumes reservations within one dispatch; it never retains
 * unsubmitted work across epoll turns or shutdown. */
int opc_store_async_reserve(opc_store_async_t *sa, const char *path, size_t len);
int opc_store_async_submit_reserved(opc_store_async_t *sa, int reservation,
                                    const void *data, size_t len, mode_t mode,
                                    uint64_t token);
/* Release an unsubmitted reservation. Does not cancel queued/running writes.
 * Invalid indexes and NULL are harmless. Handles must not be reused after
 * submission/cancellation; a freed index may belong to a subsequent owner. */
void opc_store_async_cancel(opc_store_async_t *sa, int reservation);

/* Harvest finished jobs into `out[cap]`, freeing their slots. Clears the
 * eventfd; re-arms it when more completions remain than `cap` allowed.
 * Returns the number of entries written. */
size_t opc_store_async_drain(opc_store_async_t *sa,
                             opc_store_async_done_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_STORE_ASYNC_H */
