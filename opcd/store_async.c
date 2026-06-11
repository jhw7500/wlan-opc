#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "store.h"
#include "store_async.h"

typedef enum {
    JOB_FREE = 0,   /* slot available for submit() */
    JOB_QUEUED,     /* waiting for / being processed by the worker */
    JOB_DONE,       /* finished, waiting for drain() */
} job_state_t;

typedef struct {
    job_state_t state;
    char        path[1024];
    uint8_t     data[OPC_STORE_ASYNC_DATA_MAX];
    size_t      len;
    mode_t      mode;
    uint64_t    token;
    int         result;
    int         saved_errno;
} job_t;

struct opc_store_async {
    pthread_t       worker;
    pthread_mutex_t mu;
    pthread_cond_t  cv;            /* signalled on submit and shutdown */
    int             event_fd;
    bool            shutdown;
    job_t           jobs[OPC_STORE_ASYNC_QUEUE_DEPTH];
    /* FIFO ring of indexes into jobs[] awaiting the worker. */
    int             q[OPC_STORE_ASYNC_QUEUE_DEPTH];
    int             q_head;
    int             q_count;
};

/* Bump the completion eventfd. Safe to call under sa->mu: the eventfd is
 * EFD_NONBLOCK (see create), so write() never blocks even with the mutex
 * held — a counter at UINT64_MAX-1 returns EAGAIN instead of sleeping.
 * (Removing EFD_NONBLOCK would reintroduce a deadlock here.) */
static void notify_completion(opc_store_async_t *sa)
{
    uint64_t one = 1;
    ssize_t  w;
    do {
        w = write(sa->event_fd, &one, sizeof one);
    } while (w < 0 && errno == EINTR);
    /* EAGAIN means the counter is already saturated — still readable,
     * which is all the poller needs. */
}

/* Worker: pop jobs FIFO, run the (blocking) atomic write outside the lock,
 * publish the result, kick the eventfd. On shutdown it first drains the
 * queue so submitted data always reaches NVRAM. The job's path/data are
 * safe to read unlocked: submit() only writes into JOB_FREE slots and the
 * state transition to JOB_QUEUED happens under the mutex. */
static void *worker_main(void *arg)
{
    opc_store_async_t *sa = arg;

    pthread_mutex_lock(&sa->mu);
    for (;;) {
        while (sa->q_count == 0 && !sa->shutdown)
            pthread_cond_wait(&sa->cv, &sa->mu);
        if (sa->q_count == 0 && sa->shutdown)
            break;

        int ji = sa->q[sa->q_head];
        sa->q_head  = (sa->q_head + 1) % OPC_STORE_ASYNC_QUEUE_DEPTH;
        sa->q_count--;
        job_t *j = &sa->jobs[ji];
        pthread_mutex_unlock(&sa->mu);

        int rc = opc_store_write_atomic(j->path, j->data, j->len, j->mode);
        int e  = (rc != 0) ? errno : 0;

        pthread_mutex_lock(&sa->mu);
        j->result      = rc;
        j->saved_errno = e;
        j->state       = JOB_DONE;
        notify_completion(sa);
    }
    pthread_mutex_unlock(&sa->mu);
    return NULL;
}

opc_store_async_t *opc_store_async_create(void)
{
    opc_store_async_t *sa = calloc(1, sizeof *sa);
    if (!sa) return NULL;

    sa->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (sa->event_fd < 0) {
        free(sa);
        return NULL;
    }
    if (pthread_mutex_init(&sa->mu, NULL) != 0) {
        close(sa->event_fd);
        free(sa);
        return NULL;
    }
    if (pthread_cond_init(&sa->cv, NULL) != 0) {
        pthread_mutex_destroy(&sa->mu);
        close(sa->event_fd);
        free(sa);
        return NULL;
    }
    if (pthread_create(&sa->worker, NULL, worker_main, sa) != 0) {
        pthread_cond_destroy(&sa->cv);
        pthread_mutex_destroy(&sa->mu);
        close(sa->event_fd);
        free(sa);
        return NULL;
    }
    return sa;
}

void opc_store_async_destroy(opc_store_async_t *sa)
{
    if (!sa) return;
    pthread_mutex_lock(&sa->mu);
    sa->shutdown = true;
    pthread_cond_broadcast(&sa->cv);
    pthread_mutex_unlock(&sa->mu);
    pthread_join(sa->worker, NULL);   /* worker drains the queue first */
    close(sa->event_fd);
    pthread_cond_destroy(&sa->cv);
    pthread_mutex_destroy(&sa->mu);
    free(sa);
}

int opc_store_async_event_fd(const opc_store_async_t *sa)
{
    return sa ? sa->event_fd : -1;
}

int opc_store_async_submit(opc_store_async_t *sa, const char *path,
                           const void *data, size_t len, mode_t mode,
                           uint64_t token)
{
    if (!sa || !path || (len > 0 && !data)) {
        errno = EINVAL;
        return -1;
    }
    if (len > OPC_STORE_ASYNC_DATA_MAX) {
        errno = E2BIG;
        return -1;
    }
    size_t plen = strlen(path);
    if (plen >= sizeof sa->jobs[0].path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    pthread_mutex_lock(&sa->mu);
    if (sa->shutdown) {
        pthread_mutex_unlock(&sa->mu);
        errno = ECANCELED;
        return -1;
    }
    int ji = -1;
    for (int i = 0; i < OPC_STORE_ASYNC_QUEUE_DEPTH; i++) {
        if (sa->jobs[i].state == JOB_FREE) { ji = i; break; }
    }
    if (ji < 0) {
        /* Never block here: the main thread is also the drainer, so waiting
         * for a slot inside submit() would deadlock against itself. */
        pthread_mutex_unlock(&sa->mu);
        errno = EAGAIN;
        return -1;
    }

    job_t *j = &sa->jobs[ji];
    memcpy(j->path, path, plen + 1);
    if (len > 0) memcpy(j->data, data, len);
    j->len         = len;
    j->mode        = mode;
    j->token       = token;
    j->result      = 0;
    j->saved_errno = 0;
    j->state       = JOB_QUEUED;

    sa->q[(sa->q_head + sa->q_count) % OPC_STORE_ASYNC_QUEUE_DEPTH] = ji;
    sa->q_count++;
    pthread_cond_broadcast(&sa->cv);
    pthread_mutex_unlock(&sa->mu);
    return 0;
}

size_t opc_store_async_drain(opc_store_async_t *sa,
                             opc_store_async_done_t *out, size_t cap)
{
    if (!sa || !out || cap == 0) return 0;

    /* Clear the eventfd counter. Its value is intentionally unused: the
     * counter only signals "≥1 completion pending"; the actual harvest scans
     * job slots for JOB_DONE below, and drain() re-arms the fd if it could
     * not take them all this pass. */
    uint64_t cnt;
    ssize_t  r;
    do {
        r = read(sa->event_fd, &cnt, sizeof cnt);
    } while (r < 0 && errno == EINTR);
    (void)cnt;

    size_t n = 0;
    bool   leftover = false;
    pthread_mutex_lock(&sa->mu);
    for (int i = 0; i < OPC_STORE_ASYNC_QUEUE_DEPTH; i++) {
        if (sa->jobs[i].state != JOB_DONE) continue;
        if (n < cap) {
            out[n].token       = sa->jobs[i].token;
            out[n].result      = sa->jobs[i].result;
            out[n].saved_errno = sa->jobs[i].saved_errno;
            sa->jobs[i].state  = JOB_FREE;
            n++;
        } else {
            leftover = true;
        }
    }
    if (leftover) notify_completion(sa);   /* re-arm for the rest */
    pthread_mutex_unlock(&sa->mu);
    return n;
}
