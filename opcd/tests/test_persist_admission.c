#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../handler.h"
#include "../opcd_state.h"
#include "../platform.h"
#include "../store.h"
#include "../store_async.h"
#include "../../protocol/codec.h"
#include "../../protocol/commands.h"
#include "../../protocol/ids.h"

extern int __real_fsync(int fd);
extern int stub_apply_radio_calls(void);
extern void stub_apply_radio_reset_calls(void);
extern void stub_apply_radio_set_fail(int fail);

static int failures;

#define CHECK(cond, label) do {                                               \
    if (cond) fprintf(stdout, "PASS %s\n", label);                          \
    else { fprintf(stderr, "FAIL %s\n", label); failures++; }               \
} while (0)

/* The real async worker reaches the real store implementation.  Only fsync,
 * the slow storage boundary, is held so queue saturation is deterministic.
 * A timed wait makes every failure path self-releasing instead of hanging the
 * test runner during opc_store_async_destroy(). */
static pthread_mutex_t fsync_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fsync_cv = PTHREAD_COND_INITIALIZER;
static bool fsync_armed;
static bool fsync_entered;
static bool fsync_release;
static bool fsync_guard_timed_out;

int __wrap_fsync(int fd)
{
    pthread_mutex_lock(&fsync_mu);
    if (fsync_armed) {
        fsync_armed = false;
        fsync_entered = true;
        pthread_cond_broadcast(&fsync_cv);

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;
        while (!fsync_release) {
            int rc = pthread_cond_timedwait(&fsync_cv, &fsync_mu, &deadline);
            if (rc == ETIMEDOUT) {
                fsync_guard_timed_out = true;
                break;
            }
        }
        fsync_release = false;
    }
    pthread_mutex_unlock(&fsync_mu);
    return __real_fsync(fd);
}

static void arm_fsync_block(void)
{
    pthread_mutex_lock(&fsync_mu);
    fsync_entered = false;
    fsync_release = false;
    fsync_armed = true;
    pthread_mutex_unlock(&fsync_mu);
}

static int wait_fsync_entered(int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&fsync_mu);
    while (!fsync_entered) {
        int rc = pthread_cond_timedwait(&fsync_cv, &fsync_mu, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    int entered = fsync_entered;
    pthread_mutex_unlock(&fsync_mu);
    return entered ? 0 : -1;
}

static void release_fsync(void)
{
    pthread_mutex_lock(&fsync_mu);
    fsync_release = true;
    fsync_armed = false;
    pthread_cond_broadcast(&fsync_cv);
    pthread_mutex_unlock(&fsync_mu);
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L +
           (b->tv_nsec - a->tv_nsec) / 1000000L;
}

static int wait_readable(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    int rc;
    do { rc = select(fd + 1, &fds, NULL, NULL, &tv); } while (rc < 0 && errno == EINTR);
    return rc == 1 ? 0 : -1;
}

static int bind_loopback(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    if (port_out) *port_out = ntohs(addr.sin_port);
    return fd;
}

static void init_state(opcd_state_t *st, const char *pw_path,
                       const char *ip_path, const char *radio_path, int udp_fd,
                       opc_store_async_t *store, uint32_t client_ip,
                       uint16_t client_port)
{
    memset(st, 0, sizeof *st);
    st->conf.login_idle_s = 3600;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->paths.password = pw_path;
    st->paths.ip_list = ip_path;
    st->paths.radio = radio_path;
    st->udp_fd = udp_fd;
    st->store_async = store;
    st->boot_status = OPC_DEVICE_LOGGED_IN;
    st->logged_in = true;
    st->holder_ip = client_ip;
    st->holder_port = client_port;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    st->idle_deadline = now.tv_sec + 3600;
    strcpy(st->password, OPC_PASSWORD_DEFAULT);
    st->radio.station_type = OPC_STATION_SINGLE;
    st->radio.wlan1.scan_band = OPC_SCAN_BAND_UNSET;
    st->radio.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
}

static opc_set_radio_config_req_t radio_request(void)
{
    opc_set_radio_config_req_t req;
    memset(&req, 0, sizeof req);
    req.station_type = OPC_STATION_SINGLE;
    req.wlan1.mode = OPC_WLAN_MODE_11AX;
    req.wlan1.bandwidth = OPC_BANDWIDTH_20;
    req.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(req.wlan1.scan_chlist, OPC_SCAN_BAND_5GHZ, 36);
    req.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
    req.wlan2.mode = OPC_WLAN_MODE_UNSET;
    req.wlan2.bandwidth = OPC_BANDWIDTH_UNSET;
    return req;
}

static int file_equals(const char *path, const void *want, size_t want_len)
{
    uint8_t buf[256];
    ssize_t n = opc_store_read_all(path, buf, sizeof buf);
    return n == (ssize_t)want_len && memcmp(buf, want, want_len) == 0;
}

static size_t drain_jobs(opc_store_async_t *store, size_t want, int timeout_ms)
{
    size_t total = 0;
    struct timespec begin, now;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    while (total < want) {
        if (wait_readable(opc_store_async_event_fd(store), 100) == 0) {
            opc_store_async_done_t done[OPC_STORE_ASYNC_QUEUE_DEPTH];
            total += opc_store_async_drain(store, done,
                                           OPC_STORE_ASYNC_QUEUE_DEPTH);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_ms(&begin, &now) >= timeout_ms) break;
    }
    return total;
}

static int fill_all_but_one(opc_store_async_t *store, const char *path)
{
    arm_fsync_block();
    if (opc_store_async_submit(store, path, "0", 1, 0600, UINT64_MAX) != 0 ||
        wait_fsync_entered(1000) != 0)
        return -1;
    for (int i = 1; i < OPC_STORE_ASYNC_QUEUE_DEPTH - 1; i++) {
        char byte = (char)('a' + i);
        if (opc_store_async_submit(store, path, &byte, 1, 0600, UINT64_MAX) != 0)
            return -1;
    }
    return 0;
}

static ssize_t pump_deferred_ack(opcd_state_t *st, int client,
                                 uint8_t *wire, size_t cap, int timeout_ms)
{
    struct timespec begin, now;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (;;) {
        ssize_t n = recv(client, wire, cap, MSG_DONTWAIT);
        if (n >= 0) return n;
        if (wait_readable(opc_store_async_event_fd(st->store_async), 100) == 0)
            opcd_store_async_on_ready(st);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_ms(&begin, &now) >= timeout_ms) return -1;
    }
}

int main(void)
{
    const uint32_t client_ip = 0x7f000001u;
    opcd_platform_register();
    char dir[] = "/tmp/opcd-admission-XXXXXX";
    char *tmp = mkdtemp(dir);
    CHECK(tmp != NULL, "test directory created");
    if (!tmp) return 1;

    char pw_path[256], ip_path[256], radio_path[256], fill_path[256];
    snprintf(pw_path, sizeof pw_path, "%s/password", tmp);
    snprintf(ip_path, sizeof ip_path, "%s/iplist.cfg", tmp);
    snprintf(radio_path, sizeof radio_path, "%s/radio.conf", tmp);
    snprintf(fill_path, sizeof fill_path, "%s/fill", tmp);

    uint16_t client_port = 0, other_port = 0;
    int server = bind_loopback(NULL);
    int client = bind_loopback(&client_port);
    int other = bind_loopback(&other_port);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && other >= 0 && store != NULL,
          "real handler/async-store rig created");
    if (server < 0 || client < 0 || other < 0 || !store) {
        release_fsync();
        if (store) opc_store_async_destroy(store);
        if (server >= 0) close(server);
        if (client >= 0) close(client);
        if (other >= 0) close(other);
        rmdir(tmp);
        return 1;
    }

    static const char old_pw[] = OPC_PASSWORD_DEFAULT;
    static const char old_radio[] = "old-radio";
    CHECK(opc_store_write_atomic(pw_path, old_pw, strlen(old_pw), 0600) == 0 &&
          opc_store_write_atomic(radio_path, old_radio, strlen(old_radio), 0644) == 0,
          "baseline persistent files written");

    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store,
               client_ip, client_port);

    /* Occupy all 16 real job slots: one worker job blocked in fsync plus 15
     * queued jobs.  A seventeenth submit proves the public boundary reports
     * EAGAIN before any handler request is sent. */
    arm_fsync_block();
    CHECK(opc_store_async_submit(store, fill_path, "0", 1, 0600, UINT64_MAX) == 0,
          "first filler submitted");
    CHECK(wait_fsync_entered(1000) == 0, "worker blocked inside real atomic write");
    int fill_ok = 1;
    for (int i = 1; i < OPC_STORE_ASYNC_QUEUE_DEPTH; i++) {
        char byte = (char)('a' + i);
        if (opc_store_async_submit(store, fill_path, &byte, 1, 0600, UINT64_MAX) != 0)
            fill_ok = 0;
    }
    CHECK(fill_ok, "all 16 async-store slots occupied");
    errno = 0;
    CHECK(opc_store_async_submit(store, fill_path, "x", 1, 0600, UINT64_MAX) == -1 &&
          errno == EAGAIN, "queue-full boundary confirmed");

    uint8_t frame[OPC_FRAME_MAX], resp[OPC_FRAME_MAX];
    ssize_t rlen, flen;
    struct timespec t0, t1;

    opc_set_radio_config_req_t radio = radio_request();
    stub_apply_radio_reset_calls();
    flen = opc_set_radio_config_req_pack(frame, sizeof frame, 200, &radio);
    rlen = -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                            resp, sizeof resp, &rlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(drc == 0 && elapsed_ms(&t0, &t1) < 100,
          "queue-full SetRadio dispatch returns in under 100 ms");
    CHECK(rlen == 0, "queue-full SetRadio is dropped for client timeout/retry");
    CHECK(stub_apply_radio_calls() == 0, "queue-full SetRadio has no live apply");
    CHECK(st.radio_gen == 0 && st.radio.wlan1.mode == 0 && !st.radio_committed,
          "queue-full SetRadio leaves daemon state unchanged");
    CHECK(file_equals(radio_path, old_radio, strlen(old_radio)),
          "queue-full SetRadio leaves persistent radio file unchanged");

    /* A query must remain serviceable while persistence admission is closed. */
    flen = opc_get_basic_info_req_pack(frame, sizeof frame, 300);
    rlen = -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    opc_get_basic_info_ack_t basic;
    CHECK(drc == 0 && elapsed_ms(&t0, &t1) < 100 && rlen > 0 &&
          opc_get_basic_info_ack_unpack(resp, (size_t)rlen, &basic) == 0,
          "unrelated GetBasicInfo is processed while persistence is full");

    opc_set_password_req_t pw;
    memset(&pw, 0, sizeof pw);
    strcpy(pw.old_password, OPC_PASSWORD_DEFAULT);
    strcpy(pw.new_password, "AdmissionSecret1");
    flen = opc_set_password_req_pack(frame, sizeof frame, 301, &pw);
    rlen = -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(drc == 0 && elapsed_ms(&t0, &t1) < 100 && rlen == 0,
          "queue-full SetPassword is dropped immediately without NG");
    CHECK(strcmp(st.password, OPC_PASSWORD_DEFAULT) == 0 &&
          file_equals(pw_path, old_pw, strlen(old_pw)),
          "queue-full SetPassword leaves memory and disk unchanged");

    /* This request would commit entry 1, then discover invalid entry 2.  The
     * full-store admission decision must precede even that partial mutation. */
    opc_set_ip_config_list_req_t ipreq;
    memset(&ipreq, 0, sizeof ipreq);
    ipreq.entry_count = 2;
    ipreq.essid_terminated_mask = 3;
    ipreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START_END;
    ipreq.entries[0].list_number = 1;
    ipreq.entries[0].ip_address = 0xc000020au;
    ipreq.entries[0].subnet_mask = 0xffffff00u;
    ipreq.entries[1].boundary_flag = OPC_LIST_BOUNDARY_START_END;
    ipreq.entries[1].list_number = 0; /* fails only after entry 1 committed */
    ipreq.entries[1].ip_address = 0xc0000214u;
    ipreq.entries[1].subnet_mask = 0xffffff00u;
    flen = opc_set_ip_config_list_req_pack(frame, sizeof frame, 302, &ipreq);
    rlen = -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(drc == 0 && elapsed_ms(&t0, &t1) < 100 && rlen == 0,
          "queue-full partial SetIpConfigList is dropped before an NG");
    CHECK(!st.ip_list.present[0] && !st.ip_list_staging_active &&
          access(ip_path, F_OK) != 0,
          "queue-full partial SetIpConfigList makes no memory/disk mutation");

    release_fsync();
    CHECK(drain_jobs(store, OPC_STORE_ASYNC_QUEUE_DEPTH, 5000) ==
              OPC_STORE_ASYNC_QUEUE_DEPTH,
          "all filler jobs released and drained");

    /* Retry after capacity returns: one real apply, one queued write, duplicate
     * discard, then exactly one OK ack to the original endpoint and SN. */
    stub_apply_radio_reset_calls();
    arm_fsync_block();
    flen = opc_set_radio_config_req_pack(frame, sizeof frame, 201, &radio);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    CHECK(drc == 0 && rlen == 0 && stub_apply_radio_calls() == 1,
          "retry with capacity applies once and defers ack");
    CHECK(wait_fsync_entered(1000) == 0, "retry persist held in flight");

    flen = opc_set_radio_config_req_pack(frame, sizeof frame, 202, &radio);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    CHECK(drc == 0 && rlen == 0 && stub_apply_radio_calls() == 1,
          "in-flight duplicate is discarded without a second apply");

    release_fsync();
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "retry persistence completed");
    opcd_store_async_on_ready(&st);

    uint8_t wire[OPC_FRAME_MAX];
    ssize_t n = recv(client, wire, sizeof wire, MSG_DONTWAIT);
    opc_header_t hdr;
    opc_set_radio_config_ack_t ack;
    CHECK(n > 0 && opc_frame_parse(wire, (size_t)n, &hdr, NULL, NULL) == 0 &&
          hdr.sequence_number == 201 &&
          opc_set_radio_config_ack_unpack(wire, (size_t)n, &ack) == 0 &&
          ack.result == OPC_RESULT_OK,
          "deferred ack uses original SN and reports real completion");
    CHECK(recv(client, wire, sizeof wire, MSG_DONTWAIT) < 0 &&
          recv(other, wire, sizeof wire, MSG_DONTWAIT) < 0,
          "exactly one ack reaches only the original endpoint");
    CHECK(st.radio_committed && file_equals(radio_path, &radio, sizeof radio),
          "successful retry commits the requested radio config");

    /* With exactly one store slot free, a radio apply failure must return its
     * admission reservation.  The following password write can then claim the
     * same last slot and receive its normal completion ack. */
    strcpy(st.password, OPC_PASSWORD_DEFAULT);
    CHECK(opc_store_write_atomic(pw_path, old_pw, strlen(old_pw), 0600) == 0,
          "password reset for reservation-cancel checks");
    CHECK(fill_all_but_one(store, fill_path) == 0,
          "15 slots occupied for apply-failure cancellation check");
    opc_set_radio_config_req_t failed_radio = radio;
    memset(failed_radio.wlan1.scan_chlist, 0,
           sizeof failed_radio.wlan1.scan_chlist);
    opc_scan_list_set_channel(failed_radio.wlan1.scan_chlist,
                              OPC_SCAN_BAND_5GHZ, 40);
    stub_apply_radio_set_fail(1);
    flen = opc_set_radio_config_req_pack(frame, sizeof frame, 400, &failed_radio);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    opc_set_radio_config_ack_t fail_ack;
    CHECK(drc == 0 && rlen > 0 &&
          opc_set_radio_config_ack_unpack(resp, (size_t)rlen, &fail_ack) == 0 &&
          fail_ack.result == OPC_RESULT_NG &&
          fail_ack.error_cause == OPC_ERR_RADIO_APPLY,
          "radio apply failure returns its reserved store slot");
    stub_apply_radio_set_fail(0);

    memset(&pw, 0, sizeof pw);
    strcpy(pw.old_password, OPC_PASSWORD_DEFAULT);
    strcpy(pw.new_password, "CancelSecret1");
    flen = opc_set_password_req_pack(frame, sizeof frame, 401, &pw);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    CHECK(drc == 0 && rlen == 0,
          "password claims slot released by radio apply failure");
    release_fsync();
    n = pump_deferred_ack(&st, client, wire, sizeof wire, 5000);
    opc_set_password_ack_t pw_ack;
    CHECK(n > 0 && opc_frame_parse(wire, (size_t)n, &hdr, NULL, NULL) == 0 &&
          hdr.sequence_number == 401 &&
          opc_set_password_ack_unpack(wire, (size_t)n, &pw_ack) == 0 &&
          pw_ack.result == OPC_RESULT_OK,
          "slot released after apply failure remains fully usable");

    /* A START-only IP-list frame mutates staging but schedules no write.  Its
     * speculative admission reservation must likewise be cancelled. */
    CHECK(fill_all_but_one(store, fill_path) == 0,
          "15 slots occupied for no-commit cancellation check");
    memset(&ipreq, 0, sizeof ipreq);
    ipreq.entry_count = 1;
    ipreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START;
    ipreq.entries[0].list_number = 2;
    ipreq.entries[0].ip_address = 0xc000021eu;
    ipreq.entries[0].subnet_mask = 0xffffff00u;
    flen = opc_set_ip_config_list_req_pack(frame, sizeof frame, 402, &ipreq);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    opc_set_ip_config_list_ack_t ip_ack;
    CHECK(drc == 0 && rlen > 0 &&
          opc_set_ip_config_list_ack_unpack(resp, (size_t)rlen, &ip_ack) == 0 &&
          ip_ack.result == OPC_RESULT_OK && st.ip_list_staging_active,
          "IP-list frame with no commit returns its reserved store slot");

    memset(&pw, 0, sizeof pw);
    strcpy(pw.old_password, "CancelSecret1");
    strcpy(pw.new_password, "CancelSecret2");
    flen = opc_set_password_req_pack(frame, sizeof frame, 403, &pw);
    rlen = -1;
    drc = opcd_dispatch(&st, frame, (size_t)flen, client_ip, client_port,
                        resp, sizeof resp, &rlen);
    CHECK(drc == 0 && rlen == 0,
          "password claims slot released by no-commit IP request");
    release_fsync();
    n = pump_deferred_ack(&st, client, wire, sizeof wire, 5000);
    CHECK(n > 0 && opc_frame_parse(wire, (size_t)n, &hdr, NULL, NULL) == 0 &&
          hdr.sequence_number == 403 &&
          opc_set_password_ack_unpack(wire, (size_t)n, &pw_ack) == 0 &&
          pw_ack.result == OPC_RESULT_OK,
          "slot released after no-commit request remains fully usable");

    release_fsync();
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);
    close(other);
    unlink(pw_path);
    unlink(ip_path);
    unlink(radio_path);
    unlink(fill_path);
    char fill_tmp[300];
    snprintf(fill_tmp, sizeof fill_tmp, "%s.tmp.%d", fill_path, (int)getpid());
    unlink(fill_tmp);
    rmdir(tmp);

    CHECK(!fsync_guard_timed_out, "fsync guard never needed its emergency timeout");
    if (failures == 0) fprintf(stdout, "all persistence admission tests passed\n");
    return failures ? 1 : 0;
}
