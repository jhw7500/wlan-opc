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
#include "../../protocol/codec.h"
#include "../../protocol/commands.h"
#include "../../protocol/ids.h"
#include "../../protocol/proto.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static char g_pw_path[128];

static void init_state(opcd_state_t *st, const char *pw)
{
    memset(st, 0, sizeof *st);
    st->conf.login_idle_s         = 3600;
    st->conf.default_station_type = OPC_STATION_SINGLE;
    st->paths.password = g_pw_path;
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

int main(void)
{
    const uint32_t CIP = 0x7f000001;   /* 127.0.0.1, host order */
    /* CWD-relative, not /tmp: avoids the predictable-shared-path symlink class
     * (CWE-377). `make check` runs this in opcd/tests/, a build-owned dir. */
    snprintf(g_pw_path, sizeof g_pw_path, "test_handler_pw_%d.tmp", (int)getpid());
    unlink(g_pw_path);

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

    unlink(g_pw_path);

    if (failures == 0) {
        fprintf(stdout, "all handler tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
