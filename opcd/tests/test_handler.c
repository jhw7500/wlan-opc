/*
 * Handler-level tests for the password-authentication policy (P0 security
 * hardening). Drives the real opcd_dispatch() path with the stub platform so
 * the login / set-password behaviour is exercised end-to-end through frame
 * pack → dispatch → ack unpack.
 *
 * Security property under test: an empty stored password must NEVER
 * authenticate, and set-password must refuse to set an empty new password
 * (which is how the empty-password state was reachable in the first place).
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../handler.h"
#include "../opcd_state.h"
#include "../platform.h"
#include "../store.h"
#include "../store_async.h"
#include "../../protocol/codec.h"
#include "../../protocol/commands.h"
#include "../../protocol/ids.h"
#include "../../protocol/proto.h"

static int failures = 0;

/* platform_stub accessors: observe the change-ip → platform apply wiring
 * (deferred until logout) from this handler test. */
extern unsigned stub_apply_ip_calls(void);
extern uint32_t stub_apply_ip_last_ip(void);
extern void     stub_apply_ip_reset(void);
extern void     stub_apply_ip_set_fail(int fail);

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static char g_pw_path[128];
static char g_iplist_path[128];
static char g_radio_path[128];

/* Bind a loopback UDP socket on an ephemeral port; returns the fd and the
 * chosen port. Used to observe the deferred Set* acks (PERF-001). */
static int bind_loopback_udp(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) { close(fd); return -1; }
    if (port_out) *port_out = ntohs(sa.sin_port);
    return fd;
}

static int wait_fd_readable(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r;
    do { r = poll(&pfd, 1, timeout_ms); } while (r < 0 && errno == EINTR);
    return (r == 1 && (pfd.revents & POLLIN)) ? 0 : -1;
}

static void init_state(opcd_state_t *st, const char *pw)
{
    memset(st, 0, sizeof *st);
    st->conf.login_idle_s         = 3600;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->paths.password = g_pw_path;
    st->paths.ip_list  = g_iplist_path;
    st->paths.radio    = g_radio_path;
    st->udp_fd      = -1;            /* indication send is a no-op when < 0 */
    st->boot_status = OPC_DEVICE_READY;
    st->radio.station_type = OPC_STATION_SINGLE;
    strncpy(st->password, pw, sizeof st->password - 1);
}

/* Pack a Login request, dispatch it, return the ack result code. */
static uint16_t do_login(opcd_state_t *st, uint32_t cip, const char *password)
{
    opc_login_req_t req;
    memset(&req, 0, sizeof req);
    strncpy(req.password, password, sizeof req.password - 1);

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_login_req_pack(frame, sizeof frame, 1, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_login_ack_t ack;
    if (opc_login_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

/* Pack a SetPassword request, dispatch it, return the ack result code. */
static uint16_t do_set_password(opcd_state_t *st, uint32_t cip,
                                const char *old_pw, const char *new_pw)
{
    opc_set_password_req_t req;
    memset(&req, 0, sizeof req);
    strncpy(req.old_password, old_pw, sizeof req.old_password - 1);
    strncpy(req.new_password, new_pw, sizeof req.new_password - 1);

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_password_req_pack(frame, sizeof frame, 2, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_password_ack_t ack;
    if (opc_set_password_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

/* Pack a Logout request, dispatch it, return the ack result code. */
static uint16_t do_logout(opcd_state_t *st, uint32_t cip)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_logout_req_pack(frame, sizeof frame, 4);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_logout_ack_t ack;
    if (opc_logout_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

/* Pack a SetIndicationConfig request, dispatch it, return the ack result code.
 * recipient_ip is host byte order. */
static uint16_t do_set_indication(opcd_state_t *st, uint32_t cip,
                                  uint32_t recipient_ip_host, uint16_t port,
                                  uint8_t info_bits, uint8_t period)
{
    opc_set_indication_config_req_t req;
    memset(&req, 0, sizeof req);
    req.recipient_ip   = recipient_ip_host;
    req.recipient_port = port;
    req.info_bits      = info_bits;
    req.period_seconds = period;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_indication_config_req_pack(frame, sizeof frame, 3, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_indication_config_ack_t ack;
    if (opc_set_indication_config_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

/* Pack one SetIpConfigList entry (ip_host, /24), dispatch, return ack result. */
static uint16_t do_set_ip_list(opcd_state_t *st, uint32_t cip, uint16_t slot,
                               uint16_t flag, uint32_t ip_host)
{
    opc_set_ip_config_list_req_t req;
    memset(&req, 0, sizeof req);
    req.entry_count              = 1;
    req.entries[0].boundary_flag = flag;
    req.entries[0].list_number   = slot;
    req.entries[0].ip_address    = ip_host;
    req.entries[0].subnet_mask   = 0xFFFFFF00u;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_ip_config_list_req_pack(frame, sizeof frame, 5, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_ip_config_list_ack_t ack;
    if (opc_set_ip_config_list_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

/* Pack a ChangeIpAddress request, dispatch, return ack result. */
static uint16_t do_change_ip(opcd_state_t *st, uint32_t cip, uint16_t slot)
{
    opc_change_ip_address_req_t req;
    memset(&req, 0, sizeof req);
    req.list_number = slot;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_change_ip_address_req_pack(frame, sizeof frame, 6, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_change_ip_address_ack_t ack;
    if (opc_change_ip_address_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    return ack.result;
}

int main(void)
{
    const uint32_t CIP = 0x7f000001;   /* 127.0.0.1, host order */
    opcd_platform_register();          /* link-time stub backend (needed for change-ip apply hook) */
    /* CWD-relative, not /tmp: avoids the predictable-shared-path symlink class
     * (CWE-377). `make check` runs this in opcd/tests/, a build-owned dir. */
    snprintf(g_pw_path, sizeof g_pw_path, "test_handler_pw_%d.tmp", (int)getpid());
    snprintf(g_iplist_path, sizeof g_iplist_path, "test_handler_iplist_%d.tmp", (int)getpid());
    snprintf(g_radio_path, sizeof g_radio_path, "test_handler_radio_%d.tmp", (int)getpid());
    unlink(g_pw_path);
    unlink(g_iplist_path);
    unlink(g_radio_path);

    opcd_state_t st;

    /* 1. Empty stored password must never authenticate (the live-device hole). */
    init_state(&st, "");
    uint16_t r = do_login(&st, CIP, "");
    ASSERT(r == OPC_RESULT_NG, "empty stored password: login rejected");
    ASSERT(!st.logged_in,      "empty stored password: session not opened");

    /* 2. A non-empty stored password still authenticates (bootstrap intact). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    r = do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    ASSERT(r == OPC_RESULT_OK, "default password: login succeeds");
    ASSERT(st.logged_in,       "default password: session opened");

    /* 3. A wrong password is still rejected (no regression). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    r = do_login(&st, CIP, "wrong");
    ASSERT(r == OPC_RESULT_NG, "wrong password: login rejected");

    /* 4. set-password must refuse an empty new password, leaving state intact. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_password(&st, CIP, OPC_PASSWORD_DEFAULT, "");
    ASSERT(r == OPC_RESULT_NG, "set-password: empty new password rejected");
    ASSERT(strcmp(st.password, OPC_PASSWORD_DEFAULT) == 0,
           "set-password: password unchanged after empty reject");

    /* 5. set-password with a real new password still works (no regression). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_password(&st, CIP, OPC_PASSWORD_DEFAULT, "NewSecret123");
    ASSERT(r == OPC_RESULT_OK, "set-password: valid new password accepted");
    ASSERT(strcmp(st.password, "NewSecret123") == 0,
           "set-password: password updated");

    /* ---- P1: SEC-002 indication session-lifetime + recipient validation ---- */

    /* 6. logout stops indication (reflector window bounded to the session). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP, 0xC0A80063 /*192.168.0.99*/, 6000,
                          OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_OK && st.indication_enabled,
           "set-indication unicast: enabled");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "logout ok");
    ASSERT(!st.indication_enabled, "logout stops indication");

    /* 7. dispatch idle auto-logout also stops indication. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    (void)do_set_indication(&st, CIP, 0xC0A80063, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    st.idle_deadline = 1;   /* force the deadline into the past */
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT); /* dispatch idle check fires first */
    ASSERT(!st.indication_enabled, "idle-logout stops indication");

    /* 8-10. set-indication rejects non-unicast recipients. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP, 0xE0000001 /*224.0.0.1*/, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && !st.indication_enabled, "set-indication rejects multicast");
    r = do_set_indication(&st, CIP, 0xFFFFFFFF, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && !st.indication_enabled, "set-indication rejects broadcast");
    r = do_set_indication(&st, CIP, 0x00000000, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && !st.indication_enabled, "set-indication rejects 0.0.0.0");

    /* 11. set-indication accepts an arbitrary unicast recipient (spec line 751). */
    r = do_set_indication(&st, CIP, 0x0A0A0A0A /*10.10.10.10*/, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_OK && st.indication_enabled, "set-indication accepts unicast");

    /* ---- P1: ARCH-003 pack return value checked ---- */

    /* 12. A failed ack pack (resp buffer too small) yields rlen 0, not -1. */
    {
        opc_login_req_t lr;
        memset(&lr, 0, sizeof lr);
        strncpy(lr.password, OPC_PASSWORD_DEFAULT, sizeof lr.password - 1);
        uint8_t lf[OPC_FRAME_MAX];
        ssize_t lfn = opc_login_req_pack(lf, sizeof lf, 1, &lr);
        uint8_t tiny[4];
        ssize_t rlen = 999;
        (void)opcd_dispatch(&st, lf, (size_t)lfn, CIP, 5000, tiny, sizeof tiny, &rlen);
        ASSERT(rlen == 0, "emit_ack: failed ack pack yields rlen 0");
    }

    /* ---- change-ip → platform apply wiring (deferred until logout) ---- */

    /* 13. change-ip commits a slot, defers application until logout, then drives
     *     the platform apply_ip_change hook with the committed slot's IP. This is
     *     the handler→platform wiring that the 1st-stage scaffold left as a stub. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    r = do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165 /*192.168.1.101*/);
    ASSERT(r == OPC_RESULT_OK, "change-ip: set-ip-list start ok");
    r = do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    ASSERT(r == OPC_RESULT_OK, "change-ip: set-ip-list commit ok");
    r = do_change_ip(&st, CIP, 1);
    ASSERT(r == OPC_RESULT_OK, "change-ip: accepted");
    ASSERT(stub_apply_ip_calls() == 0, "change-ip: apply deferred (not before logout)");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "change-ip: logout ok");
    opcd_apply_pending_ip_change(&st);   /* main loop applies after logout response */
    ASSERT(stub_apply_ip_calls() == 1, "change-ip: platform apply_ip_change called on logout");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "change-ip: apply gets committed slot ip");

    /* 14. A failed platform apply must NOT clear indication — the IP did not
     *     actually move, so the existing indication session stays valid. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    (void)do_set_indication(&st, CIP, 0x0A0A0A0A, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(st.indication_enabled, "change-ip fail: indication enabled precondition");
    stub_apply_ip_reset();
    stub_apply_ip_set_fail(1);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    opcd_apply_pending_ip_change(&st);   /* platform apply fails */
    ASSERT(stub_apply_ip_calls() == 1, "change-ip fail: platform apply attempted");
    ASSERT(st.indication_enabled, "change-ip fail: indication kept (IP unchanged)");
    stub_apply_ip_set_fail(0);

    /* ---- PERF-001: async NVRAM persist → deferred Set* ack ---- */

    {
        uint16_t cli_port = 0;
        int srv = bind_loopback_udp(NULL);        /* daemon side: acks sent from here */
        int cli = bind_loopback_udp(&cli_port);   /* VHL side: acks received here */
        opc_store_async_t *sa = opc_store_async_create();
        ASSERT(srv >= 0 && cli >= 0 && sa != NULL, "async: test rig up");

        const uint32_t LOOP = 0x7F000001;   /* acks must reach 127.0.0.1:cli_port */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        st.udp_fd      = srv;
        st.store_async = sa;
        (void)do_login(&st, LOOP, OPC_PASSWORD_DEFAULT);

        uint8_t frame[OPC_FRAME_MAX], resp[OPC_FRAME_MAX], rx_buf[OPC_FRAME_MAX];

        /* 15. set-password defers its ack until the NVRAM write completes;
         *     the deferred ack is OK, echoes the request seq, and the file
         *     lands on disk. Dispatch itself must produce no response. */
        opc_set_password_req_t preq;
        memset(&preq, 0, sizeof preq);
        strncpy(preq.old_password, OPC_PASSWORD_DEFAULT, sizeof preq.old_password - 1);
        strncpy(preq.new_password, "AsyncSecret1", sizeof preq.new_password - 1);
        ssize_t fn = opc_set_password_req_pack(frame, sizeof frame, 77, &preq);
        ssize_t rlen = -1;
        int drc = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                                resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "async set-password: ack deferred");
        ASSERT(strcmp(st.password, "AsyncSecret1") == 0,
               "async set-password: in-memory password updated");

        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "async set-password: completion signalled");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0, "async set-password: ack arrived");
        ssize_t rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        opc_header_t ahdr;
        ASSERT(rn > 0 && opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 77,
               "async set-password: deferred ack echoes seq");
        opc_set_password_ack_t pack_ack;
        ASSERT(rn > 0 && opc_set_password_ack_unpack(rx_buf, (size_t)rn, &pack_ack) == 0 &&
               pack_ack.result == OPC_RESULT_OK,
               "async set-password: deferred ack OK");
        char pwbuf[129] = {0};
        ASSERT(opc_store_read_all(g_pw_path, pwbuf, sizeof pwbuf - 1) ==
                   (ssize_t)strlen("AsyncSecret1") &&
               strcmp(pwbuf, "AsyncSecret1") == 0,
               "async set-password: NVRAM file written");

        /* 16. a failing NVRAM write surfaces as a deferred NG/OPC_ERR_NVRAM
         *     ack — the wire contract the deferral exists to preserve. */
        char bad_path[160];
        snprintf(bad_path, sizeof bad_path, "no_such_dir_%d/pw", (int)getpid());
        st.paths.password = bad_path;
        memset(&preq, 0, sizeof preq);
        strncpy(preq.old_password, "AsyncSecret1", sizeof preq.old_password - 1);
        strncpy(preq.new_password, "Another1", sizeof preq.new_password - 1);
        fn   = opc_set_password_req_pack(frame, sizeof frame, 78, &preq);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "async NVRAM-fail: ack deferred");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "async NVRAM-fail: completion signalled");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0, "async NVRAM-fail: ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        ASSERT(rn > 0 && opc_set_password_ack_unpack(rx_buf, (size_t)rn, &pack_ack) == 0 &&
               pack_ack.result == OPC_RESULT_NG &&
               pack_ack.error_cause == OPC_ERR_NVRAM,
               "async NVRAM-fail: deferred NG/NVRAM ack");
        st.paths.password = g_pw_path;

        /* 17. set-ip-list commits then defers one persist for the request;
         *     deferred ack is OK and the committed list reaches disk. */
        opc_set_ip_config_list_req_t lreq;
        memset(&lreq, 0, sizeof lreq);
        lreq.entry_count              = 2;
        lreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START;
        lreq.entries[0].list_number   = 1;
        lreq.entries[0].ip_address    = 0xC0A80165;          /* 192.168.1.101 */
        lreq.entries[0].subnet_mask   = 0xFFFFFF00u;
        lreq.entries[1].boundary_flag = OPC_LIST_BOUNDARY_END;
        lreq.entries[1].list_number   = 2;
        lreq.entries[1].ip_address    = 0xC0A80166;          /* 192.168.1.102 */
        lreq.entries[1].subnet_mask   = 0xFFFFFF00u;
        fn   = opc_set_ip_config_list_req_pack(frame, sizeof frame, 79, &lreq);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "async set-ip-list: ack deferred");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "async set-ip-list: completion signalled");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0, "async set-ip-list: ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        opc_set_ip_config_list_ack_t lack;
        ASSERT(rn > 0 &&
               opc_set_ip_config_list_ack_unpack(rx_buf, (size_t)rn, &lack) == 0 &&
               lack.result == OPC_RESULT_OK,
               "async set-ip-list: deferred ack OK");
        static opcd_ip_list_t disk_list;
        ASSERT(opc_store_read_all(g_iplist_path, &disk_list, sizeof disk_list) ==
                   (ssize_t)sizeof disk_list &&
               disk_list.present[0] == 1 && disk_list.present[1] == 1 &&
               disk_list.slots[1].ip_address == 0xC0A80166,
               "async set-ip-list: committed list on disk");

        /* 18. NG-after-commit: a frame that commits (START..END) and then
         *     fails (slot out of range) acks NG immediately, while the
         *     committed list still reaches NVRAM through the queue (no-ack
         *     job) — never via an in-line write that could interleave with
         *     an in-flight worker job on the same temp file. */
        memset(&lreq, 0, sizeof lreq);
        lreq.entry_count              = 3;
        lreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START;
        lreq.entries[0].list_number   = 1;
        lreq.entries[0].ip_address    = 0xC0A80170;          /* 192.168.1.112 */
        lreq.entries[0].subnet_mask   = 0xFFFFFF00u;
        lreq.entries[1].boundary_flag = OPC_LIST_BOUNDARY_END;
        lreq.entries[1].list_number   = 2;
        lreq.entries[1].ip_address    = 0xC0A80171;
        lreq.entries[1].subnet_mask   = 0xFFFFFF00u;
        lreq.entries[2].list_number   = 999;                 /* > MAX_SLOTS → NG */
        fn   = opc_set_ip_config_list_req_pack(frame, sizeof frame, 80, &lreq);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen > 0, "NG-after-commit: immediate ack");
        ASSERT(opc_set_ip_config_list_ack_unpack(resp, (size_t)rlen, &lack) == 0 &&
               lack.result == OPC_RESULT_NG,
               "NG-after-commit: ack carries the entry error");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "NG-after-commit: queued no-ack write completed");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 300) != 0,
               "NG-after-commit: no duplicate deferred ack sent");
        memset(&disk_list, 0, sizeof disk_list);
        ASSERT(opc_store_read_all(g_iplist_path, &disk_list, sizeof disk_list) ==
                   (ssize_t)sizeof disk_list &&
               disk_list.present[0] == 1 &&
               disk_list.slots[0].ip_address == 0xC0A80170,
               "NG-after-commit: committed list reached NVRAM via queue");

        /* 19. burst pressure: with every async-store job slot pre-filled by
         *     no-ack writes, a deferred Set* must still queue — persist_blob
         *     blocks on a bounded wait until a slot frees — and produce
         *     exactly one OK ack. Re-init to a known password so this case
         *     does not depend on the 15-16 chain (test 16 leaves memory at
         *     "Another1" with disk unchanged — the no-rollback semantics). */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        st.udp_fd      = srv;
        st.store_async = sa;
        (void)do_login(&st, LOOP, OPC_PASSWORD_DEFAULT);

        /* Fill all job slots; the single worker drains them FIFO with no
         * temp-file race (it is the sole writer of this path). */
        for (int b = 0; b < OPC_STORE_ASYNC_QUEUE_DEPTH; b++)
            (void)opc_store_async_submit(sa, g_iplist_path, &st.ip_list,
                                         sizeof st.ip_list, 0644,
                                         UINT64_MAX /* no-ack token */);
        memset(&preq, 0, sizeof preq);
        strncpy(preq.old_password, OPC_PASSWORD_DEFAULT, sizeof preq.old_password - 1);
        strncpy(preq.new_password, "BurstSecret1", sizeof preq.new_password - 1);
        fn   = opc_set_password_req_pack(frame, sizeof frame, 81, &preq);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "burst: ack deferred despite full queue");
        /* All slots were pre-filled with no-ack jobs, so on_ready must drain
         * those completions (freeing a slot) before the password write can be
         * submitted and its deferred ack reach the client. Pump until the ack
         * lands. */
        for (int tries = 0; tries < 10 && wait_fd_readable(cli, 1000) != 0; tries++) {
            if (wait_fd_readable(opc_store_async_event_fd(sa), 1000) == 0)
                opcd_store_async_on_ready(&st);
        }
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        ASSERT(rn > 0 && opc_set_password_ack_unpack(rx_buf, (size_t)rn, &pack_ack) == 0 &&
               pack_ack.result == OPC_RESULT_OK,
               "burst: deferred ack OK after queue drained");
        ASSERT(strcmp(st.password, "BurstSecret1") == 0,
               "burst: in-memory password updated");

        /* Drain any no-ack filler completions the burst loop left queued so
         * test 20 starts with an empty job queue, then re-init to a clean
         * logged-in session. Without this, an undrained JOB_DONE slot can hold
         * the queue full and the radio persist would hit the bounded wait. */
        while (wait_fd_readable(opc_store_async_event_fd(sa), 200) == 0)
            opcd_store_async_on_ready(&st);
        init_state(&st, OPC_PASSWORD_DEFAULT);
        st.udp_fd      = srv;
        st.store_async = sa;
        (void)do_login(&st, LOOP, OPC_PASSWORD_DEFAULT);

        /* 20. set-radio-config deferred ack. Structurally mirrors test 15 but
         *     exercises handle_set_radio_config, whose session_touch sits
         *     inside the deferred `else if` branch rather than unconditionally
         *     before the ack — verify both the deferred OK ack and that the
         *     session was touched on that path (no regression). */
        opc_set_radio_config_req_t rreq;
        memset(&rreq, 0, sizeof rreq);
        rreq.station_type     = OPC_STATION_SINGLE;
        rreq.wlan1.freq_mhz   = 5180;
        rreq.wlan1.channel    = 36;
        rreq.wlan1.mode       = OPC_WLAN_MODE_11A;
        rreq.wlan1.bandwidth  = OPC_BANDWIDTH_20;
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 82, &rreq);
        /* Pull the deadline back to a still-future value (not 0 — that would
         * read as expired and trip dispatch's idle auto-logout before the
         * handler runs). session_touch on the deferred path must push it
         * forward again, which the post-dispatch assert checks. */
        time_t radio_deadline_before = st.idle_deadline - 1000;
        st.idle_deadline = radio_deadline_before;
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "async set-radio: ack deferred");
        ASSERT(st.idle_deadline > radio_deadline_before,
               "async set-radio: session touched on deferred path");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "async set-radio: completion signalled");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0, "async set-radio: ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        opc_set_radio_config_ack_t rack;
        ASSERT(rn > 0 &&
               opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 82,
               "async set-radio: deferred ack echoes seq");
        ASSERT(rn > 0 &&
               opc_set_radio_config_ack_unpack(rx_buf, (size_t)rn, &rack) == 0 &&
               rack.result == OPC_RESULT_OK,
               "async set-radio: deferred ack OK");
        static opc_set_radio_config_req_t disk_radio;
        ASSERT(opc_store_read_all(g_radio_path, &disk_radio, sizeof disk_radio) ==
                   (ssize_t)sizeof disk_radio &&
               disk_radio.wlan1.channel == 36 &&
               disk_radio.wlan1.mode == OPC_WLAN_MODE_11A,
               "async set-radio: NVRAM file written");

        st.store_async = NULL;
        opc_store_async_destroy(sa);
        close(srv);
        close(cli);
    }

    unlink(g_pw_path);
    unlink(g_iplist_path);
    unlink(g_radio_path);

    if (failures == 0) {
        fprintf(stdout, "all handler tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
