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
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "../handler.h"
#include "../indication.h"
#include "../opcd_state.h"
#include "../platform.h"
#include "../store.h"
#include "../store_async.h"
#include "../../protocol/codec.h"
#include "../../protocol/commands.h"
#include "../../protocol/ids.h"
#include "../../protocol/indications.h"
#include "../../protocol/proto.h"

static int failures = 0;

/* platform_stub accessors: observe the change-ip → platform apply wiring
 * (deferred until logout) from this handler test. */
extern unsigned stub_apply_ip_calls(void);
extern uint32_t stub_apply_ip_last_ip(void);
extern void     stub_apply_ip_reset(void);
extern void     stub_apply_ip_set_fail(int fail);
extern void     stub_apply_radio_set_fail(int fail);
extern void     stub_apply_radio_set_fail_once(int fail);
extern int      stub_apply_radio_calls(void);
extern void     stub_apply_radio_reset_calls(void);
extern int      stub_apply_radio_last_w1_freq(void);
extern int      stub_apply_radio_last_station(void);

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static char g_pw_path[128];
static char g_iplist_path[128];
static char g_radio_path[128];

/* error_cause of the last ack seen by the matching do_* helper below */
static uint16_t g_last_ind_err, g_last_iplist_err, g_last_radio_err;

/* Bind a loopback UDP socket on an ephemeral port; returns the fd and the
 * chosen port. Used to observe the deferred Set* acks (PERF-001). */
static int bind_loopback_udp(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    /* Hang-proofing: a recv() after a missed deferred ack must fail after 5s
     * instead of blocking the whole test run forever (this hung `make check`
     * indefinitely on 2026-06-11 when a validation bug suppressed the ack). */
    struct timeval rcvto = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
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
    g_last_ind_err = ack.error_cause;
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
    g_last_iplist_err = ack.error_cause;
    return ack.result;
}

/* Pack one full SetIpConfigList entry as-is, dispatch, return ack result;
 * error cause lands in g_last_iplist_err. */
static uint16_t do_set_ip_entry(opcd_state_t *st, uint32_t cip,
                                const opc_ipcfg_entry_t *e)
{
    opc_set_ip_config_list_req_t req;
    memset(&req, 0, sizeof req);
    req.entry_count = 1;
    req.entries[0]  = *e;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_ip_config_list_req_pack(frame, sizeof frame, 8, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_ip_config_list_ack_t ack;
    if (opc_set_ip_config_list_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    g_last_iplist_err = ack.error_cause;
    return ack.result;
}

/* Pack a SetRadioConfig request (SINGLE, 11AX/20MHz, given WLAN#1 freq/ch),
 * dispatch, return ack result; error cause lands in g_last_radio_err. */
static uint16_t do_set_radio(opcd_state_t *st, uint32_t cip,
                             uint16_t freq, uint16_t ch)
{
    opc_set_radio_config_req_t req;
    memset(&req, 0, sizeof req);
    req.station_type    = OPC_STATION_SINGLE;
    req.wlan1.mode      = OPC_WLAN_MODE_11AX;
    req.wlan1.bandwidth = OPC_BANDWIDTH_20;
    req.wlan1.freq_mhz  = freq;
    req.wlan1.channel   = ch;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_radio_config_req_pack(frame, sizeof frame, 7, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_radio_config_ack_t ack;
    if (opc_set_radio_config_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    g_last_radio_err = ack.error_cause;
    return ack.result;
}

/* DUAL variant: pack a SetRadioConfig with station_type=DUAL and valid WLAN#1
 * + WLAN#2 configs (both 11AX/20MHz), so the apply-failure revert's full-DUAL
 * config hand-off is verifiable (D9 M2, test 24e). */
static uint16_t do_set_radio_dual(opcd_state_t *st, uint32_t cip,
                                  uint16_t f1, uint16_t ch1,
                                  uint16_t f2, uint16_t ch2)
{
    opc_set_radio_config_req_t req;
    memset(&req, 0, sizeof req);
    req.station_type    = OPC_STATION_DUAL;
    req.wlan1.mode      = OPC_WLAN_MODE_11AX;
    req.wlan1.bandwidth = OPC_BANDWIDTH_20;
    req.wlan1.freq_mhz  = f1;
    req.wlan1.channel   = ch1;
    req.wlan2.mode      = OPC_WLAN_MODE_11AX;
    req.wlan2.bandwidth = OPC_BANDWIDTH_20;
    req.wlan2.freq_mhz  = f2;
    req.wlan2.channel   = ch2;

    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_radio_config_req_pack(frame, sizeof frame, 7, &req);
    if (fn <= 0) return 0xFFFF;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return 0xFFFE;

    opc_set_radio_config_ack_t ack;
    if (opc_set_radio_config_ack_unpack(resp, (size_t)rlen, &ack) != 0) return 0xFFFD;
    g_last_radio_err = ack.error_cause;
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

    /* 13b. #43 regression: the deferred ChangeIp must NOT apply on a non-logout
     *      wakeup. opcd_apply_pending_ip_change runs every epoll iteration
     *      (opcd.c) — before the arm-flag fix it fired on ANY mid-session event
     *      (timer tick, async-NVRAM completion, stray datagram) and migrated the
     *      live session's IP out from under it. Only an explicit Logout arms it. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    r = do_change_ip(&st, CIP, 1);
    ASSERT(r == OPC_RESULT_OK, "#43: change-ip accepted");
    opcd_apply_pending_ip_change(&st);   /* stray mid-session wakeup */
    ASSERT(stub_apply_ip_calls() == 0, "#43: no apply on a non-logout wakeup (live session)");
    opcd_apply_pending_ip_change(&st);   /* repeated wakeups stay inert */
    ASSERT(stub_apply_ip_calls() == 0, "#43: still inert on repeated mid-session wakeups");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43: explicit logout ok");
    opcd_apply_pending_ip_change(&st);   /* main loop applies after the Logout ack */
    ASSERT(stub_apply_ip_calls() == 1, "#43: apply fires only after an explicit Logout");

    /* 13c. #43 STRICT: a non-explicit teardown (idle auto-logout / abandon) must
     *      NOT commit a pending change — the device keeps its current IP so an
     *      abandoned client can still reach it. Both idle paths funnel through
     *      opcd_session_logout(), which never arms the commit. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    opcd_session_logout(&st);            /* models idle auto-logout (no arm) */
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 0, "#43: idle/abandon logout does not commit IP change");

    /* 13d. #43: a change staged by client A and abandoned (idle) must not be
     *      committed by a later session's explicit Logout — a fresh Login clears
     *      the inherited staging (cross-session contamination guard). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);     /* A stages a change */
    st.idle_deadline = 1;                /* A goes idle */
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);  /* dispatch idle-logs-out A, then B logs in */
    ASSERT(!st.ip_change_pending, "#43: fresh Login clears an inherited pending change");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43: client B logout ok");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 0, "#43: a later session's Logout does not commit A's change");

    /* 13e. #43 (Codex P2): an explicit Logout that armed a commit must survive a
     *      Login read in the same UDP drain before the loop-tail apply pass — the
     *      armed migration is NOT undone by a racing/re-login. B (the intervening
     *      login) is then severed and must reconnect on the new IP; that is the
     *      expected A12 consequence, not an error. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2: explicit logout arms commit");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);   /* Login arrives before the apply pass */
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2: armed commit survives an intervening Login");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "#43 P2: applies the armed slot's IP");

    /* 13f. #43 (Codex re-review): an armed commit is IMMUTABLE until applied — a
     *      later session's ChangeIp must NOT cancel it (only Logout controls
     *      commits, and the arm is write-once). A logs out arming slot 1; B logs
     *      in and issues its OWN ChangeIp(slot 2); apply still commits A's armed
     *      slot 1, and B's un-Logged-out change is ignored. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2b: A logout arms slot 1");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);     /* B logs in, same drain */
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_START, 0xC0A80166);
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_END, 0xC0A80166);
    (void)do_change_ip(&st, CIP, 2);                    /* B's ChangeIp must NOT cancel A's commit */
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2b: A's armed commit applies despite B's ChangeIp");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "#43 P2b: commits A's snapshot slot 1, not B's slot 2");

    /* 13g. #43 (Codex re-review): a same-holder re-login / Login retransmission
     *      (same IP, session still active) must NOT drop the session's own
     *      still-pending ChangeIp. Only a genuinely fresh login after teardown
     *      clears inherited staging. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    ASSERT(st.ip_change_pending, "#43 P2c: change staged");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);    /* same-holder re-login (UDP retransmit) */
    ASSERT(st.ip_change_pending, "#43 P2c: same-session re-login keeps the pending change");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2c: logout arms");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2c: change applies after Logout despite re-login");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "#43 P2c: applies the staged slot");

    /* 13h. #43 (Claude review): successive ChangeIp within ONE session — the
     *      second restage (slot 2) overrides the first (slot 1), and the Logout
     *      commits slot 2, never the stale slot 1 (slot confusion was part of the
     *      original bug, so guard it explicitly). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_START, 0xC0A80166);
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_END, 0xC0A80166);
    (void)do_change_ip(&st, CIP, 1);                   /* stage slot 1 */
    (void)do_change_ip(&st, CIP, 2);                   /* restage slot 2 — overrides slot 1 */
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2d: logout arms the latest stage");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2d: applies once on logout");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80166, "#43 P2d: commits slot 2 (latest), not slot 1");

    /* 13i. #43 (Codex re-review): the armed commit snapshots the resolved entry
     *      at Logout, so a later session rewriting that slot via SetIpConfigList
     *      before the apply pass cannot change what A's armed commit applies. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165 /*.101*/);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);                   /* A targets slot 1 = .101 */
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2e: A logout arms + snapshots slot 1");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);    /* B logs in, arm preserved */
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A801C8 /*.200*/);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A801C8);  /* B rewrites slot 1 */
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2e: A's armed commit still applies");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165,
           "#43 P2e: applies A's snapshot (.101), not B's rewrite (.200)");

    /* 13j. #43: same session ChangeIp(slot1) then SetIpConfigList(slot1=new) then
     *      Logout — the commit snapshots slot 1 AT Logout, applying the latest
     *      configured value the client set before committing. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A801C8);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A801C8);
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2f: logout arms (snapshots latest slot1)");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_last_ip() == 0xC0A801C8,
           "#43 P2f: applies slot 1 value as of the committing Logout (.200)");

    /* 13k. #43 (Codex re-review): once armed, the snapshot is immutable until
     *      applied. A later session that rewrites the slot AND logs out (its own
     *      full cycle, no ChangeIp) must NOT re-arm/re-snapshot the original
     *      Logout's commit — A's armed entry survives B's cycle. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165 /*.101*/);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);                   /* A targets slot 1 = .101 */
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2g: A logout arms snapshot .101");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);    /* B logs in, arm preserved */
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A801C8 /*.200*/);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A801C8);  /* B rewrites slot 1 */
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2g: B logout must NOT re-arm");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2g: applies once");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165,
           "#43 P2g: applies A's original snapshot (.101), not B's rewrite (.200)");

    /* 13L. #43 (Codex re-review): while a prior session's Logout has an armed
     *      commit pending apply, a new ChangeIp is REJECTED (NG) rather than
     *      accepted-then-silently-dropped by the apply's clear — so a returned
     *      OK always means the change will commit on that session's Logout. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_START, 0xC0A80166);
    (void)do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_END, 0xC0A80166);
    (void)do_change_ip(&st, CIP, 1);
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 P2h: A logout arms commit");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);    /* B logs in, armed pending */
    r = do_change_ip(&st, CIP, 2);
    ASSERT(r == OPC_RESULT_NG, "#43 P2h: ChangeIp rejected while a commit is armed");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 P2h: A's armed commit still applies");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "#43 P2h: applies A's slot 1");

    /* 13m. #43 (Claude review): a second apply after a successful commit is a
     *      no-op — the armed flag is cleared, so the platform is not re-invoked. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    stub_apply_ip_reset();
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    (void)do_change_ip(&st, CIP, 1);
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "#43 idemp: logout arms");
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 idemp: first apply commits");
    opcd_apply_pending_ip_change(&st);
    opcd_apply_pending_ip_change(&st);
    ASSERT(stub_apply_ip_calls() == 1, "#43 idemp: further apply calls are no-ops");

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
    st.ip_change_armed_entry  = st.ip_list.slots[0];  /* snapshot, as a real arm would */
    st.ip_change_commit_armed = true;    /* arm the commit gate directly: a real
                                          * Logout would pre-clear indication, so
                                          * exercise the apply unit in isolation (#43) */
    opcd_apply_pending_ip_change(&st);   /* platform apply fails */
    ASSERT(stub_apply_ip_calls() == 1, "change-ip fail: platform apply attempted");
    ASSERT(st.indication_enabled, "change-ip fail: indication kept (IP unchanged)");
    stub_apply_ip_set_fail(0);

    /* 14b. SetIPConfigList merges into the committed list (spec §3.3.6 "갱신"):
     *      a later START..END cycle naming only slot 2 must not erase slot 1. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165 /*192.168.1.101*/);
    ASSERT(r == OPC_RESULT_OK, "merge: cycle A start (slot 1) ok");
    r = do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_END, 0xC0A80166 /*192.168.1.102*/);
    ASSERT(r == OPC_RESULT_OK, "merge: cycle A commit (slot 2) ok");
    ASSERT(st.ip_list.present[0] == 1 && st.ip_list.present[1] == 1,
           "merge: cycle A committed slots 1+2");
    r = do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_START, 0xC0A80199 /*192.168.1.153*/);
    ASSERT(r == OPC_RESULT_OK, "merge: cycle B start (slot 2) ok");
    r = do_set_ip_list(&st, CIP, 2, OPC_LIST_BOUNDARY_END, 0xC0A80199);
    ASSERT(r == OPC_RESULT_OK, "merge: cycle B commit (slot 2) ok");
    ASSERT(st.ip_list.present[0] == 1 &&
           st.ip_list.slots[0].ip_address == 0xC0A80165,
           "merge: slot 1 survives a cycle that does not name it");
    ASSERT(st.ip_list.present[1] == 1 &&
           st.ip_list.slots[1].ip_address == 0xC0A80199,
           "merge: slot 2 updated by cycle B");

    /* 14c. A17: CONTINUE/END without a prior START must NG with 0x0018 and
     *      record nothing; a following normal cycle is unaffected. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_ip_list(&st, CIP, 3, OPC_LIST_BOUNDARY_END, 0xC0A80170);
    ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_LIST_SEQUENCE,
           "A17: lone END → NG 0x0018");
    r = do_set_ip_list(&st, CIP, 3, OPC_LIST_BOUNDARY_CONTINUE, 0xC0A80170);
    ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_LIST_SEQUENCE,
           "A17: lone CONTINUE → NG 0x0018");
    ASSERT(st.ip_list.present[2] == 0, "A17: nothing committed by lone entries");
    r = do_set_ip_list(&st, CIP, 3, OPC_LIST_BOUNDARY_START, 0xC0A80170);
    ASSERT(r == OPC_RESULT_OK, "A17: START after NG still opens a cycle");
    r = do_set_ip_list(&st, CIP, 3, OPC_LIST_BOUNDARY_END, 0xC0A80170);
    ASSERT(r == OPC_RESULT_OK && st.ip_list.present[2] == 1,
           "A17: normal START..END cycle unaffected");

    /* 14c-2. An open cycle dies with its session: START → logout → login →
     *        END must NG (stale staging cleared on logout, A17 review fix). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_ip_list(&st, CIP, 4, OPC_LIST_BOUNDARY_START, 0xC0A80171);
    ASSERT(r == OPC_RESULT_OK, "A17: cycle open before logout");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "A17: logout mid-cycle ok");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_ip_list(&st, CIP, 4, OPC_LIST_BOUNDARY_END, 0xC0A80171);
    ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_LIST_SEQUENCE &&
           st.ip_list.present[3] == 0,
           "A17: stale staging cleared on logout → END after re-login NGs");

    /* 14d. A14: SetIndication from a non-login IP → 0x0013 (was the common
     *      0x0002); the not-logged-in case stays 0x0001. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP, 0x0A0A0A0A, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_LOGIN_VIOLATION,
           "A14: not logged in → 0x0001 unchanged");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP + 1, 0x0A0A0A0A, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_IND_OTHER_IP,
           "A14: other IP while logged in → 0x0013");

    /* 14e. D10: non-unicast indication recipient → 0x0012 (spec "IP 주소 이상"). */
    r = do_set_indication(&st, CIP, 0xFFFFFFFF, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_IND_RECIPIENT_IP,
           "D10: broadcast recipient → 0x0012");

    /* 14f. D9: platform apply refusal → dedicated apply-failure 0x0050
     *      (re-decided 2026-06-16; was 0x0011, but a runtime fault must not be
     *      reported as a bad-frequency input). The revert is DEFERRED: dispatch
     *      does ONE apply and arms the revert; the main loop (here: an explicit
     *      drain) runs it after the ack. */
    stub_apply_radio_set_fail(1);
    stub_apply_radio_reset_calls();
    r = do_set_radio(&st, CIP, 0, 0);
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_APPLY,
           "D9: apply refusal → NG 0x0050");
    ASSERT(stub_apply_radio_calls() == 1,
           "D9: dispatch does a single apply (revert is deferred, not synchronous)");
    ASSERT(st.radio_revert_pending,
           "D9: apply failure arms the deferred last-good revert");
    /* Drain as the main loop would; full-fail makes the revert fail too →
     * exercises the deferred double-failure log path (the one intentional case). */
    opcd_radio_revert_drain(&st);
    ASSERT(stub_apply_radio_calls() == 2 && !st.radio_revert_pending,
           "D9: main-loop drain runs the revert and clears the pending flag");
    stub_apply_radio_set_fail(0);
    r = do_set_radio(&st, CIP, 0, 0);
    ASSERT(r == OPC_RESULT_OK, "D9: apply ok once fail toggle cleared");

    /* 14g. D8/A21: SetRadio frequency / channel-band validation (§3.3.8). */
    r = do_set_radio(&st, CIP, 6000, 0);          /* 6 GHz frequency */
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "D8: unsupported frequency → 0x0011");
    r = do_set_radio(&st, CIP, 2490, 0);          /* above ch14 (2484 MHz) */
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "D8: 2.4G frequency above ch14 → 0x0011");
    r = do_set_radio(&st, CIP, 3000, 0);          /* inter-band gap */
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "D8: inter-band gap frequency → 0x0011");
    r = do_set_radio(&st, CIP, 0, (uint16_t)((OPC_BAND_6GHZ << 8) | 37));
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
           "A21: 6 GHz band channel → 0x0012");
    r = do_set_radio(&st, CIP, 0, (uint16_t)(OPC_BAND_2_4GHZ << 8));
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
           "D8: band specified with CH 0 → 0x0012");
    r = do_set_radio(&st, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));
    ASSERT(r == OPC_RESULT_OK, "D8: valid 2.4G freq+CH accepted");

    /* 14g-2. A21 DUAL: wlan2.channel / priority_ch band validation. */
    {
        opc_set_radio_config_req_t rreq;
        memset(&rreq, 0, sizeof rreq);
        rreq.station_type    = OPC_STATION_DUAL;
        rreq.wlan1.mode      = OPC_WLAN_MODE_11AX;
        rreq.wlan1.bandwidth = OPC_BANDWIDTH_20;
        rreq.wlan2.mode      = OPC_WLAN_MODE_11AX;
        rreq.wlan2.bandwidth = OPC_BANDWIDTH_20;
        rreq.wlan2.channel   = (uint16_t)((OPC_BAND_6GHZ << 8) | 37);
        uint8_t rf[OPC_FRAME_MAX], rresp[OPC_FRAME_MAX];
        ssize_t rfn = opc_set_radio_config_req_pack(rf, sizeof rf, 98, &rreq);
        ssize_t rrl = 0;
        (void)opcd_dispatch(&st, rf, (size_t)rfn, CIP, 5000, rresp, sizeof rresp, &rrl);
        opc_set_radio_config_ack_t rack;
        ASSERT(opc_set_radio_config_ack_unpack(rresp, (size_t)rrl, &rack) == 0 &&
               rack.result == OPC_RESULT_NG && rack.error_cause == OPC_ERR_RADIO_CH,
               "A21: DUAL wlan2 6 GHz channel → 0x0012");
        rreq.wlan2.channel = 0;
        rreq.priority_ch   = (uint16_t)((OPC_BAND_6GHZ << 8) | 1);
        rfn = opc_set_radio_config_req_pack(rf, sizeof rf, 99, &rreq);
        rrl = 0;
        (void)opcd_dispatch(&st, rf, (size_t)rfn, CIP, 5000, rresp, sizeof rresp, &rrl);
        ASSERT(opc_set_radio_config_ack_unpack(rresp, (size_t)rrl, &rack) == 0 &&
               rack.result == OPC_RESULT_NG && rack.error_cause == OPC_ERR_RADIO_CH,
               "A21: DUAL priority_ch 6 GHz → 0x0012");
    }

    /* 14h. SetIndication info-bit validation (§3.3.9 0x0010): 0x40 is the
     *      only unassigned bit. */
    r = do_set_indication(&st, CIP, 0x0A0A0A0A, 6000, 0x40, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_IND_BITS,
           "ind-bits: unassigned 0x40 → 0x0010");

    /* 14i. D1: SetIPConfigList per-entry value validation (§3.3.6). */
    {
        opc_ipcfg_entry_t ent;
        memset(&ent, 0, sizeof ent);
        ent.boundary_flag = OPC_LIST_BOUNDARY_START;
        ent.list_number   = 1;
        ent.subnet_mask   = 0xFFFFFF00u;

        ent.ip_address = 0xFFFFFFFFu;                 /* broadcast */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_IP,
               "D1: broadcast entry IP → 0x0011");

        ent.ip_address  = 0xC0A80165;                 /* 192.168.1.101 */
        ent.subnet_mask = 0xFF00FF00u;                /* non-contiguous */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_NETMASK,
               "D1: non-contiguous netmask → 0x0012");

        ent.subnet_mask     = 0xFFFFFF00u;
        ent.default_gateway = 0x0A000001u;            /* 10.0.0.1 — other segment */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_GW,
               "D1: off-subnet gateway → 0x0013");

        ent.default_gateway = 0xC0A801FEu;            /* 192.168.1.254 — same segment */
        ent.ntp_server      = 0xE0000001u;            /* multicast */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_NTP,
               "D1: multicast NTP → 0x0014");

        ent.ntp_server = 0;                           /* unset NTP/GW=valid: lenient */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_OK, "D1: valid entry (gw set, ntp unset) accepted");

        ent.default_gateway = 0;
        ent.ip_address      = 0xC0A80100u;            /* 192.168.1.0 — network addr */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_IP,
               "D1: subnet network address as host IP → 0x0011");

        ent.ip_address = 0xC0A801FFu;                 /* 192.168.1.255 — broadcast */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_IP,
               "D1: subnet broadcast address as host IP → 0x0011");

        ent.ip_address      = 0xC0A80165u;
        ent.default_gateway = 0xC0A80165u;            /* gw == host IP */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_GW,
               "D1: gateway equal to host IP → 0x0013");

        ent.ip_address      = 0x7F000001u;            /* 127.0.0.1 — loopback */
        ent.default_gateway = 0;
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_IPCFG_IP,
               "D1: loopback host IP → 0x0011");

        ent.ip_address      = 0xC0A80165u;            /* /32 P2P: off-block GW ok */
        ent.subnet_mask     = 0xFFFFFFFFu;
        ent.default_gateway = 0x0A000001u;            /* 10.0.0.1 */
        r = do_set_ip_entry(&st, CIP, &ent);
        ASSERT(r == OPC_RESULT_OK,
               "D1: /32 host with off-block gateway accepted (P2P)");
    }

    /* 14j. D3: unterminated ESSID wire field → 0x0016. Packers always
     *      NUL-pad, so poke the wire bytes (entry 0 ESSID @ frame+64+20). */
    {
        opc_set_ip_config_list_req_t vreq;
        memset(&vreq, 0, sizeof vreq);
        vreq.entry_count = 1;
        vreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START;
        vreq.entries[0].list_number   = 1;
        vreq.entries[0].ip_address    = 0xC0A80165;
        vreq.entries[0].subnet_mask   = 0xFFFFFF00u;
        uint8_t vf[OPC_FRAME_MAX], vresp[OPC_FRAME_MAX];
        ssize_t vfn = opc_set_ip_config_list_req_pack(vf, sizeof vf, 91, &vreq);
        /* entry 0 starts at the common-header end (proto.h OPC_HEADER_SIZE);
         * ESSID sits at entry offset +20 (commands.c pack_ipcfg_entry). */
        memset(vf + OPC_HEADER_SIZE + 20, 'A', 32);   /* ESSID: no NUL anywhere */
        ssize_t vrl = 0;
        (void)opcd_dispatch(&st, vf, (size_t)vfn, CIP, 5000, vresp, sizeof vresp, &vrl);
        opc_set_ip_config_list_ack_t vack;
        ASSERT(opc_set_ip_config_list_ack_unpack(vresp, (size_t)vrl, &vack) == 0 &&
               vack.result == OPC_RESULT_NG &&
               vack.error_cause == OPC_ERR_IPCFG_ESSID_NUL,
               "D3: unterminated ESSID → 0x0016");

        /* 14j-2. D1: body not 64×n → 0x0017 (list-size violation). */
        uint8_t body63[63];
        memset(body63, 0, sizeof body63);
        /* header Length = reserve(56) + payload bytes (spec A1 rule) */
        vfn = opc_frame_build(vf, sizeof vf, OPC_CMD_REQUEST,
                              OPC_REQ_SET_IP_CONFIG_LIST, 92,
                              (uint16_t)(56 + 63), body63, sizeof body63);
        vrl = 0;
        (void)opcd_dispatch(&st, vf, (size_t)vfn, CIP, 5000, vresp, sizeof vresp, &vrl);
        ASSERT(opc_set_ip_config_list_ack_unpack(vresp, (size_t)vrl, &vack) == 0 &&
               vack.result == OPC_RESULT_NG &&
               vack.error_cause == OPC_ERR_IPCFG_LIST_SIZE,
               "D1: body not 64*n → 0x0017");
    }

    /* 14k. D5/D4: unterminated password fields. Packers NUL-pad, so poke the
     *      wire bytes (Login pw @ frame+64; SetPassword old @ +64, new @ +192). */
    {
        init_state(&st, OPC_PASSWORD_DEFAULT);
        opc_login_req_t lreq2;
        memset(&lreq2, 0, sizeof lreq2);
        strncpy(lreq2.password, OPC_PASSWORD_DEFAULT, sizeof lreq2.password - 1);
        uint8_t kf[OPC_FRAME_MAX], kresp[OPC_FRAME_MAX];
        ssize_t kfn = opc_login_req_pack(kf, sizeof kf, 95, &lreq2);
        /* password field starts at the common-header end (OPC_HEADER_SIZE) */
        memset(kf + OPC_HEADER_SIZE, 'A', OPC_LOGIN_REQ_BODY_LEN);
        ssize_t krl = 0;
        (void)opcd_dispatch(&st, kf, (size_t)kfn, CIP, 5000, kresp, sizeof kresp, &krl);
        opc_login_ack_t kack;
        ASSERT(opc_login_ack_unpack(kresp, (size_t)krl, &kack) == 0 &&
               kack.result == OPC_RESULT_NG && kack.error_cause == OPC_ERR_PW_NUL,
               "D5: unterminated login password → 0x0012");

        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        opc_set_password_req_t preq;
        memset(&preq, 0, sizeof preq);
        strncpy(preq.old_password, OPC_PASSWORD_DEFAULT, sizeof preq.old_password - 1);
        strncpy(preq.new_password, "NewPassword1", sizeof preq.new_password - 1);
        kfn = opc_set_password_req_pack(kf, sizeof kf, 96, &preq);
        memset(kf + OPC_HEADER_SIZE, 'A', 128);       /* old pw: no NUL */
        krl = 0;
        (void)opcd_dispatch(&st, kf, (size_t)kfn, CIP, 5000, kresp, sizeof kresp, &krl);
        opc_set_password_ack_t pack2;
        ASSERT(opc_set_password_ack_unpack(kresp, (size_t)krl, &pack2) == 0 &&
               pack2.result == OPC_RESULT_NG && pack2.error_cause == OPC_ERR_PW_NUL,
               "D4: unterminated old password → 0x0012");

        kfn = opc_set_password_req_pack(kf, sizeof kf, 97, &preq);
        memset(kf + OPC_HEADER_SIZE + 128, 'A', 128); /* new pw: no NUL */
        krl = 0;
        (void)opcd_dispatch(&st, kf, (size_t)kfn, CIP, 5000, kresp, sizeof kresp, &krl);
        ASSERT(opc_set_password_ack_unpack(kresp, (size_t)krl, &pack2) == 0 &&
               pack2.result == OPC_RESULT_NG && pack2.error_cause == OPC_ERR_NEW_PW_NUL,
               "D4: unterminated new password → 0x0014");
        ASSERT(do_login(&st, CIP, OPC_PASSWORD_DEFAULT) == OPC_RESULT_OK,
               "D4: stored password unchanged after NUL violations");
    }

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

        /* 21. A19: a same-command retransmission (new SN) while the previous
         *     NVRAM write is still in flight supersedes the older response
         *     duty — exactly one ack comes back, carrying the newest SN. */
        opc_set_radio_config_req_t a19r;
        memset(&a19r, 0, sizeof a19r);
        a19r.station_type    = OPC_STATION_SINGLE;
        a19r.wlan1.mode      = OPC_WLAN_MODE_11A;
        a19r.wlan1.bandwidth = OPC_BANDWIDTH_20;
        a19r.wlan1.channel   = 36;
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 90, &a19r);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19: first request deferred");
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 91, &a19r);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19: retransmission (new SN) deferred too");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "A19: completion signalled");
        opcd_store_async_on_ready(&st);
        if (wait_fd_readable(opc_store_async_event_fd(sa), 1000) == 0)
            opcd_store_async_on_ready(&st);   /* second job may drain separately */
        ASSERT(wait_fd_readable(cli, 5000) == 0, "A19: an ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        ASSERT(rn > 0 &&
               opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 91 &&
               opc_set_radio_config_ack_unpack(rx_buf, (size_t)rn, &rack) == 0 &&
               rack.result == OPC_RESULT_OK,
               "A19: the single ack carries the retransmission SN and OK");
        ASSERT(wait_fd_readable(cli, 300) != 0,
               "A19: no second ack for the superseded SN");

        /* 22. D12/D13: a bad-length datagram is NG'd (0x0003) only toward
         *     the logged-in session's IP; any other source stays silent. */
        uint8_t badf[30];
        memset(badf, 0, sizeof badf);
        {
            opc_header_t bh = { .protocol_version  = OPC_PROTOCOL_VERSION,
                                .command_type      = OPC_CMD_REQUEST,
                                .req_indication_id = OPC_REQ_SET_PASSWORD,
                                .sequence_number   = 77,
                                .length            = 22 };
            /* fixed-header pack writes only 8 B — opc_header_pack would
             * memset the full 64 B common header into this 30 B buffer */
            (void)opc_fixed_header_pack(badf, &bh);
        }
        opcd_reject_bad_length(&st, badf, sizeof badf, LOOP, cli_port);
        ASSERT(wait_fd_readable(cli, 1000) == 0,
               "D13: NG sent to the session holder");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        opc_set_password_ack_t bad_ack;
        ASSERT(rn > 0 &&
               opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 77 &&
               opc_set_password_ack_unpack(rx_buf, (size_t)rn, &bad_ack) == 0 &&
               bad_ack.result == OPC_RESULT_NG &&
               bad_ack.error_cause == OPC_ERR_PACKET_SIZE,
               "D13: NG echoes req/seq with 0x0003");
        opcd_reject_bad_length(&st, badf, sizeof badf, LOOP + 1, cli_port);
        ASSERT(wait_fd_readable(cli, 300) != 0,
               "D13: non-session source stays silent");
        opcd_reject_bad_length(&st, badf, 4 /* <8 B: no header to echo */,
                               LOOP, cli_port);
        ASSERT(wait_fd_readable(cli, 300) != 0,
               "D13: sub-header runt stays silent even for the holder");

        /* 23. T6 interim: the congestion probe fires FaultDetect on the
         *     reporting period and re-notifies while the congestion
         *     persists. Synthetic /proc/stat source; disk/net sources are
         *     left dead so only the CPU resource can flag. */
        {
            char fpd[64], fpstat[128];
            snprintf(fpd, sizeof fpd, "test_handler_fp_%d", (int)getpid());
            mkdir(fpd, 0755);
            snprintf(fpstat, sizeof fpstat, "%s/stat", fpd);
            FILE *ff = fopen(fpstat, "w");
            ASSERT(ff != NULL, "T6: fixture write (prime)");
            if (ff) { fputs("cpu  0 0 0 1000 0 0 0 0\n", ff); fclose(ff); }

            opcd_fault_probe_init(&st.fault_probe);
            snprintf(st.fault_probe.path_proc_stat,
                     sizeof st.fault_probe.path_proc_stat, "%s", fpstat);
            snprintf(st.fault_probe.path_diskstats,
                     sizeof st.fault_probe.path_diskstats, "%s/none", fpd);
            snprintf(st.fault_probe.net_dir,
                     sizeof st.fault_probe.net_dir, "%s/none", fpd);

            r = do_set_indication(&st, LOOP, 0x7F000001, cli_port,
                                  OPC_IND_BIT_FAULT_DETECT, 1);
            ASSERT(r == OPC_RESULT_OK, "T6: FaultDetect-only indication enabled");
            opcd_ind_tick(&st);              /* period 1 → fires; primes probe */
            ASSERT(wait_fd_readable(cli, 200) != 0,
                   "T6: priming tick emits nothing");

            ff = fopen(fpstat, "w");         /* +900 busy / +1000 total = 90% */
            ASSERT(ff != NULL, "T6: fixture write (90%)");
            if (ff) { fputs("cpu  900 0 0 1100 0 0 0 0\n", ff); fclose(ff); }
            st.fault_probe.mono_ms -= 1000;  /* pretend one second elapsed */
            opcd_ind_tick(&st);
            ASSERT(wait_fd_readable(cli, 1000) == 0, "T6: congestion frame sent");
            rn = recv(cli, rx_buf, sizeof rx_buf, 0);
            opc_ind_fault_detect_t fdi;
            ASSERT(rn > 0 &&
                   opc_ind_fault_detect_unpack(rx_buf, (size_t)rn, &fdi) == 0 &&
                   fdi.congestion_id == OPC_CONGESTION_CPU &&
                   fdi.current_val >= 80,
                   "T6: CPU congestion id and value reported");

            ff = fopen(fpstat, "w");         /* still 90% over the next period */
            ASSERT(ff != NULL, "T6: fixture write (repeat)");
            if (ff) { fputs("cpu  1800 0 0 1200 0 0 0 0\n", ff); fclose(ff); }
            st.fault_probe.mono_ms -= 1000;
            opcd_ind_tick(&st);
            ASSERT(wait_fd_readable(cli, 1000) == 0,
                   "T6: persistent congestion re-notified");
            rn = recv(cli, rx_buf, sizeof rx_buf, 0);
            ASSERT(rn > 0 &&
                   opc_ind_fault_detect_unpack(rx_buf, (size_t)rn, &fdi) == 0 &&
                   fdi.congestion_id == OPC_CONGESTION_CPU,
                   "T6: repeat frame is FaultDetect CPU");
            unlink(fpstat);
            rmdir(fpd);
        }

        st.store_async = NULL;
        opc_store_async_destroy(sa);
        close(srv);
        close(cli);
    }

    /* ---- Issue #12: error-path coverage for SetRadioConfig apply failure ----
     *
     * These tests drive the handler dispatch path using env-var fault injection
     * (OPCD_STUB_APPLY_RADIO_RC) so that platform_nxp.c is never touched.
     * The nxp fork/execl/timeout code path and journal output are excluded from
     * this change set (#13 covers those). */

    /* 24a. apply failure (-EPROTO) → Result=NG + error_cause=0x0050
     *      (OPC_ERR_RADIO_APPLY). handler.c maps every non-zero
     *      apply_radio_config return to the dedicated apply-failure code (D9,
     *      re-decided 2026-06-16). */
    {
        opcd_state_t st24;
        init_state(&st24, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24, CIP, OPC_PASSWORD_DEFAULT);
        setenv("OPCD_STUB_APPLY_RADIO_RC", "-71", 1);  /* -EPROTO */
        uint16_t r24 = do_set_radio(&st24, CIP, 2412,
                                    (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));
        unsetenv("OPCD_STUB_APPLY_RADIO_RC");
        ASSERT(r24 == OPC_RESULT_NG,
               "issue#12-24a: apply -EPROTO → Result NG");
        ASSERT(g_last_radio_err == OPC_ERR_RADIO_APPLY,
               "issue#12-24a: apply -EPROTO → error_cause 0x0050");
    }

    /* 24b. apply failure (-ETIMEDOUT) → same NG + 0x0050 mapping. */
    {
        opcd_state_t st24b;
        init_state(&st24b, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24b, CIP, OPC_PASSWORD_DEFAULT);
        setenv("OPCD_STUB_APPLY_RADIO_RC", "-110", 1);  /* -ETIMEDOUT */
        uint16_t r24b = do_set_radio(&st24b, CIP, 5180, 36);
        unsetenv("OPCD_STUB_APPLY_RADIO_RC");
        ASSERT(r24b == OPC_RESULT_NG,
               "issue#12-24b: apply -ETIMEDOUT → Result NG");
        ASSERT(g_last_radio_err == OPC_ERR_RADIO_APPLY,
               "issue#12-24b: apply -ETIMEDOUT → error_cause 0x0050");
    }

    /* 24c. state preservation + DEFERRED revert: after an apply failure the
     *      in-memory radio config retains the previous settings, and the revert
     *      is ARMED (run later by the main loop), NOT performed synchronously —
     *      so dispatch does a single apply and the NG ack is not delayed. */
    {
        opcd_state_t st24c;
        init_state(&st24c, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24c, CIP, OPC_PASSWORD_DEFAULT);

        /* Prime with a known good config. */
        (void)do_set_radio(&st24c, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));
        uint16_t saved_freq = st24c.radio.wlan1.freq_mhz;
        uint16_t saved_ch   = st24c.radio.wlan1.channel;

        /* Inject a single apply failure and submit a different config. Reset the
         * counter after priming so it measures only the failing Set-Radio. */
        stub_apply_radio_reset_calls();
        stub_apply_radio_set_fail_once(1);
        (void)do_set_radio(&st24c, CIP, 5180, 36);

        ASSERT(st24c.radio.wlan1.freq_mhz == saved_freq,
               "issue#12-24c: apply failure preserves previous freq_mhz");
        ASSERT(st24c.radio.wlan1.channel == saved_ch,
               "issue#12-24c: apply failure preserves previous channel");
        /* Dispatch did exactly ONE apply (the failing one) — the revert is
         * deferred, so the NG response is never delayed by a second apply. */
        ASSERT(stub_apply_radio_calls() == 1,
               "issue#12-24c: dispatch does a single apply (revert deferred)");
        ASSERT(st24c.radio_revert_pending,
               "issue#12-24c: apply failure arms the deferred last-good revert");
        ASSERT(st24c.radio_revert_cfg.wlan1.freq_mhz == saved_freq,
               "issue#12-24c: armed revert carries the previous config (2412)");

        /* Main loop drains the revert AFTER the ack: re-applies the last-good
         * config (2412), then clears the pending flag. */
        opcd_radio_revert_drain(&st24c);
        ASSERT(stub_apply_radio_calls() == 2,
               "issue#12-24c: drain runs the deferred revert (2nd apply)");
        ASSERT(stub_apply_radio_last_w1_freq() == (int)saved_freq,
               "issue#12-24c: deferred revert re-applies the previous config (2412)");
        ASSERT(!st24c.radio_revert_pending,
               "issue#12-24c: drain clears the pending flag");
    }

    /* 24d. success-path regression (env var cleared): apply succeeds, Result=OK. */
    {
        opcd_state_t st24d;
        init_state(&st24d, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24d, CIP, OPC_PASSWORD_DEFAULT);
        /* Ensure env var is absent. */
        unsetenv("OPCD_STUB_APPLY_RADIO_RC");
        uint16_t r24d = do_set_radio(&st24d, CIP, 5180, 36);
        ASSERT(r24d == OPC_RESULT_OK,
               "issue#12-24d: no injection → apply succeeds → Result OK");
        ASSERT(st24d.radio.wlan1.freq_mhz == 5180,
               "issue#12-24d: success updates in-memory radio state");
    }

    /* 24e. M2: DUAL deferred revert hands the FULL DUAL last-good config back to
     *      the platform — the headline "DUAL partial-apply reconverges" claim,
     *      proven at the handler/stub boundary (the per-interface nxp reconverge
     *      itself is cross-only / on-target). */
    {
        opcd_state_t st24e;
        init_state(&st24e, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24e, CIP, OPC_PASSWORD_DEFAULT);
        /* Prime a DUAL good config (both interfaces). */
        uint16_t pr = do_set_radio_dual(&st24e, CIP, 2412, 1, 5180, 36);
        ASSERT(pr == OPC_RESULT_OK, "issue#12-24e: DUAL prime succeeds");
        uint16_t saved_f1 = st24e.radio.wlan1.freq_mhz;

        /* Inject a single apply failure; the deferred revert then succeeds. */
        stub_apply_radio_reset_calls();
        stub_apply_radio_set_fail_once(1);
        uint16_t r = do_set_radio_dual(&st24e, CIP, 2437, 6, 5200, 40);

        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_APPLY,
               "issue#12-24e: DUAL apply failure → NG 0x0050");
        ASSERT(stub_apply_radio_calls() == 1,
               "issue#12-24e: dispatch does a single apply (revert deferred)");
        ASSERT(st24e.radio_revert_pending &&
               st24e.radio_revert_cfg.station_type == OPC_STATION_DUAL,
               "issue#12-24e: armed revert carries the full DUAL config (station_type)");

        opcd_radio_revert_drain(&st24e);
        ASSERT(stub_apply_radio_calls() == 2,
               "issue#12-24e: drain runs the deferred DUAL revert");
        ASSERT(stub_apply_radio_last_station() == OPC_STATION_DUAL,
               "issue#12-24e: deferred revert hands back DUAL station_type");
        ASSERT(stub_apply_radio_last_w1_freq() == (int)saved_f1,
               "issue#12-24e: deferred revert re-applies previous DUAL wlan1 freq (2412)");
        ASSERT(!st24e.radio_revert_pending,
               "issue#12-24e: drain clears the pending flag");
    }

    /* 24f. L2: a *successful* deferred revert still yields NG 0x0050 — guards
     *      against a future reorder letting the revert's OK overwrite the NG. */
    {
        opcd_state_t st24f;
        init_state(&st24f, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24f, CIP, OPC_PASSWORD_DEFAULT);
        (void)do_set_radio(&st24f, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));

        stub_apply_radio_reset_calls();
        stub_apply_radio_set_fail_once(1);   /* real apply fails, deferred revert succeeds */
        uint16_t r = do_set_radio(&st24f, CIP, 5180, 36);

        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_APPLY,
               "issue#12-24f: apply failure → NG 0x0050 (response set before any revert)");
        ASSERT(st24f.radio_revert_pending && stub_apply_radio_calls() == 1,
               "issue#12-24f: dispatch armed the revert with a single apply");
        opcd_radio_revert_drain(&st24f);
        ASSERT(stub_apply_radio_calls() == 2 && !st24f.radio_revert_pending,
               "issue#12-24f: deferred revert ran (succeeded) and cleared — response stays NG");
    }

    /* 24g. should_revert guard (Gemini review): when the failing request equals
     *      the committed config there is nothing to undo, so no revert is armed —
     *      avoids a pointless re-apply and a misleading "revert failed" log. */
    {
        opcd_state_t st24g;
        init_state(&st24g, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24g, CIP, OPC_PASSWORD_DEFAULT);
        (void)do_set_radio(&st24g, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));

        stub_apply_radio_reset_calls();
        stub_apply_radio_set_fail_once(1);
        /* Resubmit the identical config; apply fails but nothing changed. */
        uint16_t r = do_set_radio(&st24g, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));

        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_APPLY,
               "issue#12-24g: identical-config apply failure still → NG 0x0050");
        ASSERT(!st24g.radio_revert_pending,
               "issue#12-24g: identical config → no revert armed (nothing to undo)");
        ASSERT(stub_apply_radio_calls() == 1,
               "issue#12-24g: identical config → no second apply scheduled");
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
