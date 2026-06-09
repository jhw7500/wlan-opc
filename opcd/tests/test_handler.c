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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../handler.h"
#include "../opcd_state.h"
#include "../platform.h"
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

static void init_state(opcd_state_t *st, const char *pw)
{
    memset(st, 0, sizeof *st);
    st->conf.login_idle_s         = 3600;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->paths.password = g_pw_path;
    st->paths.ip_list  = g_iplist_path;
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
    unlink(g_pw_path);
    unlink(g_iplist_path);

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

    unlink(g_pw_path);
    unlink(g_iplist_path);

    if (failures == 0) {
        fprintf(stdout, "all handler tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
