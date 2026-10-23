#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../handler.h"
#include "../platform.h"
#include "../store.h"
#include "../store_async.h"
#include "../../protocol/frame.h"

extern int __real_fsync(int fd);
extern int __real_rename(const char *oldpath, const char *newpath);
extern void stub_apply_radio_reset_calls(void);
extern void stub_apply_radio_set_fail(int fail);
extern int stub_apply_radio_calls(void);
extern int stub_apply_radio_last_w1_freq(void);
extern int stub_apply_radio_last_station(void);
extern void stub_apply_ip_reset(void);
extern unsigned stub_apply_ip_calls(void);
extern uint32_t stub_apply_ip_last_ip(void);
extern int stub_apply_ip_last_iface(void);

static int failures;
#define CHECK(c, label) do { \
    if (c) fprintf(stdout, "PASS %s\n", label); \
    else { fprintf(stderr, "FAIL %s\n", label); failures++; } \
} while (0)

enum fault_mode {
    FAULT_NONE = 0,
    FAULT_BLOCK_FILE_FSYNC,
    FAULT_RENAME,
    FAULT_DIR_FSYNC,
};

static pthread_mutex_t fault_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fault_cv = PTHREAD_COND_INITIALIZER;
static enum fault_mode fault_mode;
static bool fault_entered;
static bool fault_release;
static unsigned fault_fsync_count;

static void fault_arm(enum fault_mode mode)
{
    pthread_mutex_lock(&fault_mu);
    fault_mode = mode;
    fault_entered = false;
    fault_release = false;
    fault_fsync_count = 0;
    pthread_mutex_unlock(&fault_mu);
}

static void fault_clear(void)
{
    pthread_mutex_lock(&fault_mu);
    fault_mode = FAULT_NONE;
    fault_release = true;
    pthread_cond_broadcast(&fault_cv);
    pthread_mutex_unlock(&fault_mu);
}

static int wait_fault_entered(int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&fault_mu);
    while (!fault_entered) {
        int rc = pthread_cond_timedwait(&fault_cv, &fault_mu, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    bool entered = fault_entered;
    pthread_mutex_unlock(&fault_mu);
    return entered ? 0 : -1;
}

int __wrap_fsync(int fd)
{
    pthread_mutex_lock(&fault_mu);
    unsigned call = ++fault_fsync_count;
    enum fault_mode mode = fault_mode;
    if (mode == FAULT_BLOCK_FILE_FSYNC && call == 1) {
        fault_entered = true;
        pthread_cond_broadcast(&fault_cv);
        while (!fault_release)
            pthread_cond_wait(&fault_cv, &fault_mu);
        fault_mode = FAULT_NONE;
    }
    if (mode == FAULT_DIR_FSYNC && call == 2) {
        fault_mode = FAULT_NONE;
        pthread_mutex_unlock(&fault_mu);
        errno = EIO;
        return -1;
    }
    pthread_mutex_unlock(&fault_mu);
    return __real_fsync(fd);
}

int __wrap_rename(const char *oldpath, const char *newpath)
{
    pthread_mutex_lock(&fault_mu);
    bool fail = fault_mode == FAULT_RENAME;
    if (fail) fault_mode = FAULT_NONE;
    pthread_mutex_unlock(&fault_mu);
    if (fail) {
        errno = EIO;
        return -1;
    }
    return __real_rename(oldpath, newpath);
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
    socklen_t n = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &n) != 0) {
        close(fd);
        return -1;
    }
    if (port_out) *port_out = ntohs(addr.sin_port);
    return fd;
}

static int file_equals(const char *path, const void *want, size_t want_len)
{
    uint8_t buf[8192];
    ssize_t n = opc_store_read_all(path, buf, sizeof buf);
    return n == (ssize_t)want_len && memcmp(buf, want, want_len) == 0;
}

static void init_state(opcd_state_t *st, const char *pw_path, const char *ip_path,
                       const char *radio_path, int server, opc_store_async_t *store,
                       uint16_t holder_port)
{
    memset(st, 0, sizeof *st);
    st->conf.login_idle_s = 3600;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->paths.password = pw_path;
    st->paths.ip_list = ip_path;
    st->paths.radio = radio_path;
    st->udp_fd = server;
    st->store_async = store;
    st->boot_status = OPC_DEVICE_LOGGED_IN;
    st->logged_in = true;
    st->holder_ip = 0x7f000001u;
    st->holder_port = holder_port;
    st->idle_deadline = time(NULL) + 3600;
    strcpy(st->password, OPC_PASSWORD_DEFAULT);
    st->radio.station_type = OPC_STATION_SINGLE;
    st->radio.wlan1.scan_band = OPC_SCAN_BAND_UNSET;
    st->radio.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
}

static ssize_t dispatch_password(opcd_state_t *st, uint16_t port, uint16_t seq,
                                 const char *old_pw, const char *new_pw,
                                 uint8_t resp[OPC_FRAME_MAX])
{
    opc_set_password_req_t req;
    uint8_t frame[OPC_FRAME_MAX];
    memset(&req, 0, sizeof req);
    snprintf(req.old_password, sizeof req.old_password, "%s", old_pw);
    snprintf(req.new_password, sizeof req.new_password, "%s", new_pw);
    ssize_t flen = opc_set_password_req_pack(frame, sizeof frame, seq, &req);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static ssize_t recv_ack(int fd, uint8_t wire[OPC_FRAME_MAX], int timeout_ms)
{
    if (wait_readable(fd, timeout_ms) != 0) return -1;
    return recv(fd, wire, OPC_FRAME_MAX, 0);
}

static void check_password_ack(const uint8_t *wire, ssize_t n, uint16_t seq,
                               uint16_t result, const char *label)
{
    opc_header_t hdr;
    opc_set_password_ack_t ack;
    CHECK(n > 0 && opc_frame_parse(wire, (size_t)n, &hdr, NULL, NULL) == 0 &&
          hdr.sequence_number == seq &&
          opc_set_password_ack_unpack(wire, (size_t)n, &ack) == 0 &&
          ack.result == result &&
          ack.error_cause == (result == OPC_RESULT_OK ? OPC_ERR_NONE : OPC_ERR_NVRAM),
          label);
}

static opc_ipcfg_entry_t ip_entry(uint16_t number, uint16_t boundary,
                                  uint32_t address, const char *essid)
{
    opc_ipcfg_entry_t e;
    memset(&e, 0, sizeof e);
    e.list_number = number;
    e.boundary_flag = boundary;
    e.ip_address = address;
    e.subnet_mask = 0xffffff00u;
    snprintf(e.essid, sizeof e.essid, "%s", essid);
    return e;
}

static ssize_t dispatch_ip_list(opcd_state_t *st, uint16_t port, uint16_t seq,
                                const opc_ipcfg_entry_t *entries, size_t count,
                                uint8_t resp[OPC_FRAME_MAX])
{
    opc_set_ip_config_list_req_t req;
    uint8_t frame[OPC_FRAME_MAX];
    memset(&req, 0, sizeof req);
    req.entry_count = count;
    for (size_t i = 0; i < count; i++) {
        req.entries[i] = entries[i];
        req.essid_terminated_mask |= (uint32_t)1u << i;
    }
    ssize_t flen = opc_set_ip_config_list_req_pack(frame, sizeof frame, seq, &req);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static uint16_t unpack_ip_result(const uint8_t *wire, ssize_t n, uint16_t *err)
{
    opc_set_ip_config_list_ack_t ack = {0};
    if (n <= 0 || opc_set_ip_config_list_ack_unpack(wire, (size_t)n, &ack) != 0)
        return UINT16_MAX;
    if (err) *err = ack.error_cause;
    return ack.result;
}

static ssize_t dispatch_change_ip(opcd_state_t *st, uint16_t port, uint16_t seq,
                                  uint16_t number, uint8_t resp[OPC_FRAME_MAX])
{
    opc_change_ip_address_req_t req = { .list_number = number };
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t flen = opc_change_ip_address_req_pack(frame, sizeof frame, seq, &req);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static ssize_t dispatch_logout(opcd_state_t *st, uint16_t port, uint16_t seq,
                               uint8_t resp[OPC_FRAME_MAX])
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t flen = opc_logout_req_pack(frame, sizeof frame, seq);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static ssize_t dispatch_login(opcd_state_t *st, uint16_t port, uint16_t seq,
                              const char *password,
                              uint8_t resp[OPC_FRAME_MAX])
{
    opc_login_req_t req;
    uint8_t frame[OPC_FRAME_MAX];
    memset(&req, 0, sizeof req);
    snprintf(req.password, sizeof req.password, "%s", password);
    ssize_t flen = opc_login_req_pack(frame, sizeof frame, seq, &req);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static opc_set_radio_config_req_t radio_config(uint16_t station, uint16_t ch1,
                                                uint16_t ch2)
{
    opc_set_radio_config_req_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = station;
    cfg.priority_ch = (uint16_t)((OPC_BAND_5GHZ << 8) | ch1);
    cfg.wlan1.mode = OPC_WLAN_MODE_11AX;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_20;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(cfg.wlan1.scan_chlist, OPC_SCAN_BAND_5GHZ,
                              (uint8_t)ch1);
    cfg.wlan2.mode = station == OPC_STATION_DUAL ? OPC_WLAN_MODE_11AX
                                                  : OPC_WLAN_MODE_UNSET;
    cfg.wlan2.bandwidth = station == OPC_STATION_DUAL ? OPC_BANDWIDTH_20
                                                       : OPC_BANDWIDTH_UNSET;
    cfg.wlan2.scan_band = station == OPC_STATION_DUAL ? OPC_SCAN_BAND_5GHZ
                                                       : OPC_SCAN_BAND_UNSET;
    if (station == OPC_STATION_DUAL)
        opc_scan_list_set_channel(cfg.wlan2.scan_chlist, OPC_SCAN_BAND_5GHZ,
                                  (uint8_t)ch2);
    return cfg;
}

static ssize_t dispatch_radio(opcd_state_t *st, uint16_t port, uint16_t seq,
                              const opc_set_radio_config_req_t *cfg,
                              uint8_t resp[OPC_FRAME_MAX])
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t flen = opc_set_radio_config_req_pack(frame, sizeof frame, seq, cfg);
    ssize_t rlen = -1;
    if (flen < 0 || opcd_dispatch(st, frame, (size_t)flen, 0x7f000001u, port,
                                  resp, OPC_FRAME_MAX, &rlen) != 0)
        return -2;
    return rlen;
}

static void test_password_resource_gate_and_outcomes(const char *pw_path,
                                                     const char *ip_path,
                                                     const char *radio_path)
{
    uint16_t port_a = 0, port_b = 0;
    int server = bind_loopback(NULL);
    int client_a = bind_loopback(&port_a);
    int client_b = bind_loopback(&port_b);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client_a >= 0 && client_b >= 0 && store != NULL,
          "password: async handler rig created");
    if (server < 0 || client_a < 0 || client_b < 0 || !store) return;

    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port_a);
    CHECK(opc_store_write_atomic(pw_path, OPC_PASSWORD_DEFAULT,
                                 strlen(OPC_PASSWORD_DEFAULT), 0600) == 0,
          "password: old durable baseline written");

    uint8_t resp[OPC_FRAME_MAX], wire[OPC_FRAME_MAX];
    fault_arm(FAULT_BLOCK_FILE_FSYNC);
    CHECK(dispatch_password(&st, port_a, 100, OPC_PASSWORD_DEFAULT,
                            "FirstSecret1", resp) == 0,
          "password: original request defers its ack");
    CHECK(wait_fault_entered(1000) == 0, "password: original write held in flight");
    CHECK(strcmp(st.password, OPC_PASSWORD_DEFAULT) == 0,
          "password: candidate is not published before persistence completes");

    CHECK(dispatch_password(&st, port_b, 101, OPC_PASSWORD_DEFAULT,
                            "PeerSecret2", resp) == 0,
          "password: same-resource request from a second port waits by timeout");
    CHECK(dispatch_password(&st, port_a, 102, OPC_PASSWORD_DEFAULT,
                            "SamePeerSecret3", resp) == 0,
          "password: same-peer retry preserves the original transaction");

    uint8_t basic_req[OPC_FRAME_MAX];
    ssize_t basic_len = opc_get_basic_info_req_pack(basic_req, sizeof basic_req, 103);
    ssize_t basic_rlen = -1;
    CHECK(opcd_dispatch(&st, basic_req, (size_t)basic_len, 0x7f000001u, port_b,
                        resp, sizeof resp, &basic_rlen) == 0 && basic_rlen > 0,
          "password: unrelated request continues while resource is busy");

    fault_clear();
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "password: original write completes");
    CHECK(strcmp(st.password, OPC_PASSWORD_DEFAULT) == 0,
          "password: completion is unpublished until main-loop reconciliation");
    opcd_store_async_on_ready(&st);
    ssize_t n = recv_ack(client_a, wire, 1000);
    check_password_ack(wire, n, 100, OPC_RESULT_OK,
                       "password: original endpoint and SN receive the sole durable ack");
    CHECK(recv(client_a, wire, sizeof wire, MSG_DONTWAIT) < 0 &&
          recv(client_b, wire, sizeof wire, MSG_DONTWAIT) < 0,
          "password: peer retries do not create extra acks");
    CHECK(strcmp(st.password, "FirstSecret1") == 0 &&
          file_equals(pw_path, "FirstSecret1", strlen("FirstSecret1")),
          "password: durable completion publishes exactly the original candidate");

    fault_arm(FAULT_RENAME);
    CHECK(dispatch_password(&st, port_a, 110, "FirstSecret1",
                            "RenameFails2", resp) == 0,
          "password unchanged: request accepted");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "password unchanged: failed write completes");
    CHECK(strcmp(st.password, "FirstSecret1") == 0,
          "password unchanged: failed candidate remains unpublished");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client_a, wire, 1000);
    check_password_ack(wire, n, 110, OPC_RESULT_NG,
                       "password unchanged: original request receives NVRAM NG");
    CHECK(strcmp(st.password, "FirstSecret1") == 0 &&
          file_equals(pw_path, "FirstSecret1", strlen("FirstSecret1")),
          "password unchanged: memory and target retain the old value");

    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_password(&st, port_a, 120, "FirstSecret1",
                            "VisibleUnsynced3", resp) == 0,
          "password replaced-unsynced: request accepted");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "password replaced-unsynced: failed directory sync completes");
    CHECK(strcmp(st.password, "FirstSecret1") == 0,
          "password replaced-unsynced: publish still waits for reconciliation");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client_a, wire, 1000);
    check_password_ack(wire, n, 120, OPC_RESULT_NG,
                       "password replaced-unsynced: durability failure returns NG");
    CHECK(strcmp(st.password, "VisibleUnsynced3") == 0 &&
          file_equals(pw_path, "VisibleUnsynced3", strlen("VisibleUnsynced3")),
          "password replaced-unsynced: visible target and published state agree");

    fault_clear();
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client_a);
    close(client_b);
}

static void test_password_sync_fallback_outcomes(const char *pw_path,
                                                 const char *ip_path,
                                                 const char *radio_path)
{
    opcd_state_t st;
    uint8_t resp[OPC_FRAME_MAX];
    init_state(&st, pw_path, ip_path, radio_path, -1, NULL, 41001);
    CHECK(opc_store_write_atomic(pw_path, OPC_PASSWORD_DEFAULT,
                                 strlen(OPC_PASSWORD_DEFAULT), 0600) == 0,
          "password sync: old durable baseline written");

    fault_arm(FAULT_RENAME);
    ssize_t n = dispatch_password(&st, 41001, 130, OPC_PASSWORD_DEFAULT,
                                  "SyncRenameFails1", resp);
    check_password_ack(resp, n, 130, OPC_RESULT_NG,
                       "password sync unchanged: inline durability NG");
    CHECK(strcmp(st.password, OPC_PASSWORD_DEFAULT) == 0 &&
          file_equals(pw_path, OPC_PASSWORD_DEFAULT, strlen(OPC_PASSWORD_DEFAULT)),
          "password sync unchanged: old published and target values remain");

    fault_arm(FAULT_DIR_FSYNC);
    n = dispatch_password(&st, 41001, 131, OPC_PASSWORD_DEFAULT,
                          "SyncVisibleUnsynced2", resp);
    check_password_ack(resp, n, 131, OPC_RESULT_NG,
                       "password sync replaced-unsynced: inline durability NG");
    CHECK(strcmp(st.password, "SyncVisibleUnsynced2") == 0 &&
          file_equals(pw_path, "SyncVisibleUnsynced2",
                      strlen("SyncVisibleUnsynced2")),
          "password sync replaced-unsynced: visible candidate is published");
    fault_clear();
}

static void test_ip_publication_and_no_ack(const char *pw_path,
                                           const char *ip_path,
                                           const char *radio_path)
{
    uint16_t port = 0;
    int server = bind_loopback(NULL);
    int client = bind_loopback(&port);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "ip: async handler rig created");
    if (server < 0 || client < 0 || !store) return;
    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    st.conf.management_topology = OPC_TOPOLOGY_PEER_ROUTE;

    uint8_t resp[OPC_FRAME_MAX], wire[OPC_FRAME_MAX];
    opc_ipcfg_entry_t e = ip_entry(1, OPC_LIST_BOUNDARY_START_END,
                                   0xc633640au, "jhw_wlan");
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_ip_list(&st, port, 200, &e, 1, resp) == 0,
          "ip replaced-unsynced: request defers its ack");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip replaced-unsynced: write completes");
    CHECK(!st.ip_list.present[0],
          "ip replaced-unsynced: candidate is unpublished before reconciliation");

    ssize_t n = dispatch_change_ip(&st, port, 201, 1, resp);
    opc_change_ip_address_ack_t change_ack;
    CHECK(n > 0 && opc_change_ip_address_ack_unpack(resp, (size_t)n, &change_ack) == 0 &&
          change_ack.result == OPC_RESULT_NG && change_ack.error_cause == OPC_ERR_SLOT_EMPTY,
          "ip replaced-unsynced: ChangeIp cannot consume an unpublished slot");

    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    uint16_t ip_err = 0;
    CHECK(unpack_ip_result(wire, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_NVRAM,
          "ip replaced-unsynced: original request receives NVRAM NG");
    CHECK(st.ip_list.present[0] && st.ip_list.slots[0].ip_address == e.ip_address &&
          file_equals(ip_path, &st.ip_list, sizeof st.ip_list),
          "ip replaced-unsynced: visible target and published list agree");

    n = dispatch_change_ip(&st, port, 202, 1, resp);
    CHECK(n > 0 && opc_change_ip_address_ack_unpack(resp, (size_t)n, &change_ack) == 0 &&
          change_ack.result == OPC_RESULT_OK && st.ip_change_pending,
          "ip replaced-unsynced: ChangeIp sees the reconciled candidate");
    CHECK(dispatch_logout(&st, port, 203, resp) > 0 && st.ip_change_commit_armed,
          "ip replaced-unsynced: Logout arms the reconciled candidate");
    stub_apply_ip_reset();
    opcd_apply_pending_ip_change(&st);
    CHECK(stub_apply_ip_calls() == 1 && stub_apply_ip_last_ip() == e.ip_address &&
          stub_apply_ip_last_iface() == 1,
          "ip replaced-unsynced: peer_route Logout applies candidate on mlan0");

    /* Re-open the session for a partial commit whose later entry is invalid.
     * Its immediate SLOT_RANGE NG is authoritative; completion publishes the
     * valid prefix without sending a second ack. */
    st.logged_in = true;
    st.holder_ip = 0x7f000001u;
    st.holder_port = port;
    st.boot_status = OPC_DEVICE_LOGGED_IN;
    st.idle_deadline = time(NULL) + 3600;
    opc_ipcfg_entry_t partial[2];
    partial[0] = ip_entry(2, OPC_LIST_BOUNDARY_START_END,
                          0xc6336414u, "jhw_wlan_");
    partial[1] = ip_entry(0, OPC_LIST_BOUNDARY_START_END,
                          0xc633641eu, "bad_slot");
    fault_arm(FAULT_BLOCK_FILE_FSYNC);
    n = dispatch_ip_list(&st, port, 210, partial, 2, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_SLOT_RANGE,
          "ip partial: later validation error is returned immediately");
    CHECK(wait_fault_entered(1000) == 0 && !st.ip_list.present[1],
          "ip partial: committed prefix waits for persistence publication");
    fault_clear();
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip partial: no-ack write completes");
    opcd_store_async_on_ready(&st);
    CHECK(st.ip_list.present[1] &&
          st.ip_list.slots[1].ip_address == partial[0].ip_address,
          "ip partial: durable no-ack completion publishes the valid prefix");
    CHECK(recv(client, wire, sizeof wire, MSG_DONTWAIT) < 0,
          "ip partial: completion does not send a second ack");

    fault_clear();
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);
}

static void test_ip_failed_commit_drops_dependent_staging(const char *pw_path,
                                                          const char *ip_path,
                                                          const char *radio_path)
{
    uint8_t resp[OPC_FRAME_MAX], wire[OPC_FRAME_MAX];
    uint16_t ip_err = 0;
    opc_ipcfg_entry_t chained[2] = {
        ip_entry(1, OPC_LIST_BOUNDARY_START_END, 0xc6336428u, "chain_a"),
        ip_entry(2, OPC_LIST_BOUNDARY_START, 0xc6336429u, "chain_b"),
    };
    opc_ipcfg_entry_t end =
        ip_entry(3, OPC_LIST_BOUNDARY_END, 0xc633642au, "chain_c");

    /* Async unchanged: the open cycle after a failed commit depends on the
     * unpublished candidate and must not survive reconciliation. */
    uint16_t port = 0;
    int server = bind_loopback(NULL);
    int client = bind_loopback(&port);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "ip chain async: handler rig created");
    if (server < 0 || client < 0 || !store) return;
    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    opcd_ip_list_t empty = {0};
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "ip chain async: old durable baseline written");
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_ip_list(&st, port, 220, chained, 2, resp) == 0,
          "ip chain async: commit plus trailing START defers");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip chain async: failed commit completes");
    opcd_store_async_on_ready(&st);
    ssize_t n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_NVRAM,
          "ip chain async: unchanged outcome returns NVRAM NG");
    CHECK(!st.ip_list.present[0] && !st.ip_list_staging_active,
          "ip chain async: failed candidate and dependent staging stay unpublished");
    n = dispatch_ip_list(&st, port, 221, &end, 1, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_LIST_SEQUENCE && !st.ip_list.present[0],
          "ip chain async: later END cannot resurrect the failed prefix");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* A visible completion in the same session publishes both the committed
     * prefix and the trailing cycle, but only at reconciliation time. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "ip chain durable: handler rig created");
    if (server < 0 || client < 0 || !store) return;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "ip chain durable: old durable baseline written");
    fault_arm(FAULT_BLOCK_FILE_FSYNC);
    CHECK(dispatch_ip_list(&st, port, 245, chained, 2, resp) == 0 &&
          wait_fault_entered(1000) == 0 && !st.ip_list_staging_active,
          "ip chain durable: prefix and trailing cycle remain unpublished in flight");
    fault_clear();
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip chain durable: commit completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &ip_err) == OPC_RESULT_OK &&
          st.ip_list.present[0] && st.ip_list_staging_active &&
          st.ip_list_staging.present[0] && st.ip_list_staging.present[1],
          "ip chain durable: same-session completion publishes dependent staging");
    CHECK(dispatch_ip_list(&st, port, 246, &end, 1, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip chain durable: later END commits the published trailing cycle");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &ip_err) == OPC_RESULT_OK &&
          st.ip_list.present[0] && st.ip_list.present[1] &&
          st.ip_list.present[2] && !st.ip_list_staging_active,
          "ip chain durable: dependent cycle completes normally");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* Even a durable prefix must not carry its volatile trailing cycle across
     * Logout and a fresh Login while the worker is still finishing. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "ip chain session: handler rig created");
    if (server < 0 || client < 0 || !store) return;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "ip chain session: old durable baseline written");
    fault_arm(FAULT_BLOCK_FILE_FSYNC);
    CHECK(dispatch_ip_list(&st, port, 250, chained, 2, resp) == 0 &&
          wait_fault_entered(1000) == 0,
          "ip chain session: committed prefix remains in flight");
    CHECK(dispatch_logout(&st, port, 251, resp) > 0 && !st.logged_in,
          "ip chain session: Logout clears the submitting session");
    CHECK(dispatch_login(&st, port, 252, OPC_PASSWORD_DEFAULT, resp) > 0 &&
          st.logged_in,
          "ip chain session: fresh Login starts before persistence completion");
    fault_clear();
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip chain session: durable commit completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &ip_err) == OPC_RESULT_OK &&
          st.ip_list.present[0] && !st.ip_list_staging_active,
          "ip chain session: prefix publishes without reviving prior-session staging");
    n = dispatch_ip_list(&st, port, 253, &end, 1, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_LIST_SEQUENCE,
          "ip chain session: new session cannot END the prior trailing cycle");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* Synchronous fallback must use the same unchanged publication rule. */
    init_state(&st, pw_path, ip_path, radio_path, -1, NULL, 42001);
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "ip chain sync: old durable baseline written");
    fault_arm(FAULT_RENAME);
    n = dispatch_ip_list(&st, 42001, 230, chained, 2, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_NVRAM && !st.ip_list_staging_active,
          "ip chain sync: unchanged commit drops dependent trailing staging");
    n = dispatch_ip_list(&st, 42001, 231, &end, 1, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_LIST_SEQUENCE && !st.ip_list.present[0],
          "ip chain sync: later END cannot resurrect the failed prefix");

    /* A later validation error makes the ack immediate, but the queued valid
     * prefix and its dependent open cycle still follow the store outcome. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "ip chain partial: handler rig created");
    if (server < 0 || client < 0 || !store) return;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "ip chain partial: old durable baseline written");
    opc_ipcfg_entry_t partial[3] = { chained[0], chained[1],
        ip_entry(0, OPC_LIST_BOUNDARY_START_END, 0xc633642bu, "bad_slot") };
    fault_arm(FAULT_RENAME);
    n = dispatch_ip_list(&st, port, 240, partial, 3, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_SLOT_RANGE,
          "ip chain partial: later validation error is immediate");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "ip chain partial: no-ack failed commit completes");
    opcd_store_async_on_ready(&st);
    CHECK(!st.ip_list.present[0] && !st.ip_list_staging_active &&
          recv(client, wire, sizeof wire, MSG_DONTWAIT) < 0,
          "ip chain partial: unchanged no-ack completion publishes no dependent state");
    n = dispatch_ip_list(&st, port, 241, &end, 1, resp);
    CHECK(n > 0 && unpack_ip_result(resp, n, &ip_err) == OPC_RESULT_NG &&
          ip_err == OPC_ERR_LIST_SEQUENCE && !st.ip_list.present[0],
          "ip chain partial: later END cannot resurrect the valid prefix");
    fault_clear();
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);
}

static void test_radio_reconciliation(const char *pw_path,
                                      const char *ip_path,
                                      const char *radio_path)
{
    uint16_t port = 0;
    int server = bind_loopback(NULL);
    int client = bind_loopback(&port);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "radio: async handler rig created");
    if (server < 0 || client < 0 || !store) return;
    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    opc_set_radio_config_req_t baseline = radio_config(OPC_STATION_DUAL, 36, 40);
    opc_set_radio_config_req_t candidate = radio_config(OPC_STATION_DUAL, 44, 48);
    st.radio = baseline;
    st.radio_committed = true;
    st.radio_applied_known = true;
    CHECK(opc_store_write_atomic(radio_path, &baseline, sizeof baseline, 0644) == 0,
          "radio: known DUAL baseline written");

    uint8_t resp[OPC_FRAME_MAX], wire[OPC_FRAME_MAX];
    stub_apply_radio_reset_calls();
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_radio(&st, port, 300, &candidate, resp) == 0,
          "radio unchanged: candidate apply defers its ack");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0 &&
          stub_apply_radio_calls() == 1,
          "radio unchanged: persist failure completes after one candidate apply");
    opcd_store_async_on_ready(&st);
    ssize_t n = recv_ack(client, wire, 1000);
    opc_set_radio_config_ack_t radio_ack;
    CHECK(n > 0 && opc_set_radio_config_ack_unpack(wire, (size_t)n, &radio_ack) == 0 &&
          radio_ack.result == OPC_RESULT_NG && radio_ack.error_cause == OPC_ERR_NVRAM,
          "radio unchanged: original request receives NVRAM NG");
    CHECK(stub_apply_radio_calls() == 2 &&
          stub_apply_radio_last_w1_freq() == 5180 &&
          stub_apply_radio_last_station() == OPC_STATION_DUAL,
          "radio unchanged: completion immediately restores the known DUAL baseline");
    CHECK(memcmp(&st.radio, &baseline, sizeof baseline) == 0 &&
          st.radio_committed && st.radio_applied_known &&
          st.radio_recovery_state == OPCD_RADIO_RECOVERY_HEALTHY,
          "radio unchanged: state records the successful restore truthfully");

    /* Graceful shutdown must join the worker, harvest its completion and run
     * the same recovery while the platform is still alive. */
    stub_apply_radio_reset_calls();
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_radio(&st, port, 301, &candidate, resp) == 0,
          "radio shutdown: candidate accepted");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    CHECK(stub_apply_radio_calls() == 2 &&
          memcmp(&st.radio, &baseline, sizeof baseline) == 0 &&
          st.radio_recovery_state == OPCD_RADIO_RECOVERY_HEALTHY,
          "radio shutdown: finish/harvest restores before teardown");
    n = recv_ack(client, wire, 1000);
    CHECK(n > 0 && opc_set_radio_config_ack_unpack(wire, (size_t)n, &radio_ack) == 0 &&
          radio_ack.result == OPC_RESULT_NG,
          "radio shutdown: completion remains a real NVRAM NG");

    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* Missing live baseline: a failed persist cannot honestly restore. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    opc_set_radio_config_req_t single = radio_config(OPC_STATION_SINGLE, 36, 0);
    stub_apply_radio_reset_calls();
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_radio(&st, port, 310, &single, resp) == 0,
          "radio degraded: no-baseline candidate accepted");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    CHECK(stub_apply_radio_calls() == 1 && !st.radio_applied_known &&
          !st.radio_committed &&
          st.radio_recovery_state == OPCD_RADIO_RECOVERY_DEGRADED_UNKNOWN_BASELINE,
          "radio degraded: missing baseline is recorded without a fake restore");
    (void)recv_ack(client, wire, 1000);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* A known SINGLE baseline still cannot restore WLAN#2 after a DUAL apply. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    st.radio = single;
    st.radio_committed = true;
    st.radio_applied_known = true;
    stub_apply_radio_reset_calls();
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_radio(&st, port, 320, &candidate, resp) == 0,
          "radio degraded: SINGLE-to-DUAL candidate accepted");
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    CHECK(stub_apply_radio_calls() == 1 && !st.radio_applied_known &&
          st.radio_recovery_state == OPCD_RADIO_RECOVERY_DEGRADED_UNKNOWN_BASELINE,
          "radio degraded: SINGLE baseline does not pretend to restore WLAN#2");
    (void)recv_ack(client, wire, 1000);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* A valid baseline whose recovery apply fails is a distinct degraded state. */
    server = bind_loopback(NULL);
    client = bind_loopback(&port);
    store = opc_store_async_create();
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    st.radio = baseline;
    st.radio_committed = true;
    st.radio_applied_known = true;
    stub_apply_radio_reset_calls();
    fault_arm(FAULT_RENAME);
    CHECK(dispatch_radio(&st, port, 330, &candidate, resp) == 0,
          "radio degraded: restore-failure candidate accepted");
    CHECK(wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "radio degraded: restore-failure persist completes");
    stub_apply_radio_set_fail(1);
    opcd_store_async_on_ready(&st);
    stub_apply_radio_set_fail(0);
    CHECK(stub_apply_radio_calls() == 2 && !st.radio_applied_known &&
          !st.radio_committed &&
          st.radio_recovery_state == OPCD_RADIO_RECOVERY_DEGRADED_RESTORE_FAILED,
          "radio degraded: failed recovery apply remains explicitly degraded");
    (void)recv_ack(client, wire, 1000);
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);
}

static uint16_t unpack_password_result(const uint8_t *wire, ssize_t n, uint16_t *err)
{
    opc_set_password_ack_t ack = {0};
    if (n <= 0 || opc_set_password_ack_unpack(wire, (size_t)n, &ack) != 0)
        return UINT16_MAX;
    if (err) *err = ack.error_cause;
    return ack.result;
}

static uint16_t unpack_login_result(const uint8_t *wire, ssize_t n, uint16_t *err)
{
    opc_login_ack_t ack = {0};
    if (n <= 0 || opc_login_ack_unpack(wire, (size_t)n, &ack) != 0)
        return UINT16_MAX;
    if (err) *err = ack.error_cause;
    return ack.result;
}

static uint16_t unpack_radio_result(const uint8_t *wire, ssize_t n, uint16_t *err)
{
    opc_set_radio_config_ack_t ack = {0};
    if (n <= 0 || opc_set_radio_config_ack_unpack(wire, (size_t)n, &ack) != 0)
        return UINT16_MAX;
    if (err) *err = ack.error_cause;
    return ack.result;
}

/* #184: a write whose rename landed but whose directory sync failed answers
 * NVRAM NG while the new value is already live. The client re-sends the same
 * request (a split IP list from its START); every resource must converge to OK
 * on that re-send and leave the new value in its file. */
static void test_unsynced_retry_converges(const char *pw_path,
                                          const char *ip_path,
                                          const char *radio_path)
{
    uint16_t port = 0;
    int server = bind_loopback(NULL);
    int client = bind_loopback(&port);
    opc_store_async_t *store = opc_store_async_create();
    CHECK(server >= 0 && client >= 0 && store != NULL,
          "unsynced retry: async handler rig created");
    if (server < 0 || client < 0 || !store) return;
    opcd_state_t st;
    init_state(&st, pw_path, ip_path, radio_path, server, store, port);
    CHECK(opc_store_write_atomic(pw_path, OPC_PASSWORD_DEFAULT,
                                 strlen(OPC_PASSWORD_DEFAULT), 0600) == 0,
          "unsynced retry password: old durable baseline written");

    uint8_t resp[OPC_FRAME_MAX], wire[OPC_FRAME_MAX];
    uint16_t err = 0;
    const char *next = "RetrySecret1";

    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_password(&st, port, 400, OPC_PASSWORD_DEFAULT, next, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry password: original write completes");
    opcd_store_async_on_ready(&st);
    ssize_t n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_NVRAM && strcmp(st.password, next) == 0,
          "unsynced retry password: original answers NVRAM NG with the new value live");

    n = dispatch_password(&st, port, 401, "WrongOld9", next, resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_PASSWORD_MISMATCH,
          "unsynced retry password: a wrong old password is still a mismatch");

    n = dispatch_password(&st, port, 408, OPC_PASSWORD_DEFAULT, "OtherNew3", resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_PASSWORD_MISMATCH && strcmp(st.password, next) == 0,
          "unsynced retry password: the replaced password cannot set another new one");

    fault_arm(FAULT_DIR_FSYNC);
    n = dispatch_password(&st, port, 402, OPC_PASSWORD_DEFAULT, next, resp);
    CHECK(n == 0 && wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry password: the same request is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_NVRAM,
          "unsynced retry password: a re-send that is still unsynced answers NVRAM NG");

    fault_arm(FAULT_RENAME);
    n = dispatch_password(&st, port, 409, OPC_PASSWORD_DEFAULT, next, resp);
    CHECK(n == 0 && wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry password: a re-send failing before rename completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_NVRAM && strcmp(st.password, next) == 0,
          "unsynced retry password: a re-send failing before rename answers NVRAM NG");

    n = dispatch_password(&st, port, 403, OPC_PASSWORD_DEFAULT, next, resp);
    CHECK(n == 0 && wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry password: a later re-send is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_OK &&
          strcmp(st.password, next) == 0 &&
          file_equals(pw_path, next, strlen(next)),
          "unsynced retry password: the durable re-send answers OK");

    n = dispatch_password(&st, port, 404, OPC_PASSWORD_DEFAULT, next, resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_PASSWORD_MISMATCH,
          "unsynced retry password: after a durable write the old password no longer matches");

    CHECK(dispatch_logout(&st, port, 405, resp) > 0, "unsynced retry password: Logout");
    n = dispatch_login(&st, port, 406, OPC_PASSWORD_DEFAULT, resp);
    CHECK(unpack_login_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_PASSWORD_MISMATCH,
          "unsynced retry password: Login rejects the old password");
    n = dispatch_login(&st, port, 407, next, resp);
    CHECK(unpack_login_result(resp, n, &err) == OPC_RESULT_OK,
          "unsynced retry password: Login accepts the new password");

    /* Two unsynced changes in a row: the re-send of the second one names the
     * first one's value as its old password. */
    const char *second = "SecondSecret4", *third = "ThirdSecret5";
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_password(&st, port, 410, next, second, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry chain: first change write completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_password(&st, port, 411, second, third, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry chain: second change write completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_NVRAM && strcmp(st.password, third) == 0,
          "unsynced retry chain: second change answers NVRAM NG with its value live");
    n = dispatch_password(&st, port, 412, next, second, resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_PASSWORD_MISMATCH,
          "unsynced retry chain: a re-send of the superseded change is a mismatch");
    n = dispatch_password(&st, port, 413, second, third, resp);
    CHECK(n == 0 && wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry chain: the re-send of the second change is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_password_result(wire, n, &err) == OPC_RESULT_OK &&
          file_equals(pw_path, third, strlen(third)),
          "unsynced retry chain: the re-send of the second change answers OK");

    fault_clear();
    opcd_ip_list_t empty = {0};
    CHECK(opc_store_write_atomic(ip_path, &empty, sizeof empty, 0644) == 0,
          "unsynced retry ip: old durable baseline written");
    opc_ipcfg_entry_t one = ip_entry(3, OPC_LIST_BOUNDARY_START_END,
                                     0xc6336432u, "retry_one");
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_ip_list(&st, port, 420, &one, 1, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry ip: single-frame write completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &err) == OPC_RESULT_NG && err == OPC_ERR_NVRAM,
          "unsynced retry ip: single-frame original answers NVRAM NG");
    CHECK(dispatch_ip_list(&st, port, 421, &one, 1, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry ip: single-frame re-send is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &err) == OPC_RESULT_OK &&
          st.ip_list.present[2] && st.ip_list.slots[2].ip_address == one.ip_address &&
          file_equals(ip_path, &st.ip_list, sizeof st.ip_list),
          "unsynced retry ip: single-frame re-send answers OK and the file keeps the slot");

    opc_ipcfg_entry_t start = ip_entry(4, OPC_LIST_BOUNDARY_START,
                                       0xc633643cu, "retry_start");
    opc_ipcfg_entry_t end = ip_entry(5, OPC_LIST_BOUNDARY_END,
                                     0xc6336446u, "retry_end");
    n = dispatch_ip_list(&st, port, 430, &start, 1, resp);
    CHECK(unpack_ip_result(resp, n, &err) == OPC_RESULT_OK,
          "unsynced retry ip: split START is staged");
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_ip_list(&st, port, 431, &end, 1, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry ip: split END write completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &err) == OPC_RESULT_NG && err == OPC_ERR_NVRAM,
          "unsynced retry ip: split END answers NVRAM NG");
    n = dispatch_ip_list(&st, port, 432, &start, 1, resp);
    CHECK(unpack_ip_result(resp, n, &err) == OPC_RESULT_OK,
          "unsynced retry ip: re-sent START opens a new cycle");
    CHECK(dispatch_ip_list(&st, port, 433, &end, 1, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry ip: re-sent END is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_ip_result(wire, n, &err) == OPC_RESULT_OK &&
          st.ip_list.present[3] && st.ip_list.present[4] &&
          file_equals(ip_path, &st.ip_list, sizeof st.ip_list),
          "unsynced retry ip: re-sent cycle answers OK and the file keeps both slots");

    opc_set_radio_config_req_t base = radio_config(OPC_STATION_SINGLE, 36, 0);
    opc_set_radio_config_req_t cand = radio_config(OPC_STATION_SINGLE, 44, 0);
    fault_clear();
    st.radio = base;
    st.radio_committed = true;
    st.radio_applied_known = true;
    CHECK(opc_store_write_atomic(radio_path, &base, sizeof base, 0644) == 0,
          "unsynced retry radio: old durable baseline written");
    fault_arm(FAULT_DIR_FSYNC);
    CHECK(dispatch_radio(&st, port, 440, &cand, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry radio: original write completes");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_radio_result(wire, n, &err) == OPC_RESULT_NG && err == OPC_ERR_NVRAM &&
          !st.radio_committed && memcmp(&st.radio, &cand, sizeof cand) == 0,
          "unsynced retry radio: original answers NVRAM NG with the candidate live");
    CHECK(dispatch_radio(&st, port, 441, &cand, resp) == 0 &&
          wait_readable(opc_store_async_event_fd(store), 3000) == 0,
          "unsynced retry radio: re-send is written again");
    opcd_store_async_on_ready(&st);
    n = recv_ack(client, wire, 1000);
    CHECK(unpack_radio_result(wire, n, &err) == OPC_RESULT_OK && st.radio_committed &&
          file_equals(radio_path, &cand, sizeof cand),
          "unsynced retry radio: re-send answers OK and commits the candidate");

    fault_clear();
    opc_store_async_finish(store);
    opcd_store_async_on_ready(&st);
    st.store_async = NULL;
    opc_store_async_destroy(store);
    close(server);
    close(client);

    /* The synchronous fallback (no writer thread) follows the same rule. */
    fault_clear();
    opcd_state_t sync_st;
    init_state(&sync_st, pw_path, ip_path, radio_path, -1, NULL, 41002);
    CHECK(opc_store_write_atomic(pw_path, OPC_PASSWORD_DEFAULT,
                                 strlen(OPC_PASSWORD_DEFAULT), 0600) == 0,
          "unsynced retry sync: old durable baseline written");
    fault_arm(FAULT_DIR_FSYNC);
    n = dispatch_password(&sync_st, 41002, 450, OPC_PASSWORD_DEFAULT,
                          "SyncRetry2", resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_NG &&
          err == OPC_ERR_NVRAM && strcmp(sync_st.password, "SyncRetry2") == 0,
          "unsynced retry sync: original answers NVRAM NG with the new value live");
    n = dispatch_password(&sync_st, 41002, 451, OPC_PASSWORD_DEFAULT,
                          "SyncRetry2", resp);
    CHECK(unpack_password_result(resp, n, &err) == OPC_RESULT_OK &&
          file_equals(pw_path, "SyncRetry2", strlen("SyncRetry2")),
          "unsynced retry sync: the re-send answers OK");
    fault_clear();
}

int main(void)
{
    opcd_platform_register();
    char dir[] = "/tmp/opcd-persist-failure-XXXXXX";
    char *tmp = mkdtemp(dir);
    CHECK(tmp != NULL, "temporary test directory created");
    if (!tmp) return 1;

    char pw_path[256], ip_path[256], radio_path[256];
    snprintf(pw_path, sizeof pw_path, "%s/password", tmp);
    snprintf(ip_path, sizeof ip_path, "%s/iplist.cfg", tmp);
    snprintf(radio_path, sizeof radio_path, "%s/radio.conf", tmp);
    test_password_resource_gate_and_outcomes(pw_path, ip_path, radio_path);
    test_password_sync_fallback_outcomes(pw_path, ip_path, radio_path);
    test_ip_publication_and_no_ack(pw_path, ip_path, radio_path);
    test_ip_failed_commit_drops_dependent_staging(pw_path, ip_path, radio_path);
    test_radio_reconciliation(pw_path, ip_path, radio_path);
    test_unsynced_retry_converges(pw_path, ip_path, radio_path);

    unlink(pw_path);
    unlink(ip_path);
    unlink(radio_path);
    rmdir(tmp);
    if (failures == 0) fprintf(stdout, "all persistence failure tests passed\n");
    return failures ? 1 : 0;
}
