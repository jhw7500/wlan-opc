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
extern unsigned     stub_apply_ip_calls(void);
extern uint32_t     stub_apply_ip_last_ip(void);
extern int          stub_apply_ip_last_iface(void);
extern void         stub_apply_ip_reset(void);
extern void         stub_apply_ip_set_fail(int fail);
extern const char  *stub_apply_ip_last_essid(void);
extern void     stub_apply_radio_set_fail(int fail);
extern void     stub_apply_radio_set_fail_once(int fail);
extern int      stub_apply_radio_calls(void);
extern void     stub_apply_radio_reset_calls(void);
extern int      stub_apply_radio_last_w1_freq(void);
extern int      stub_apply_radio_last_station(void);
/* device-info live-link injection (freq-source toggle test) */
extern void     stub_set_link(int idx, bool assoc, uint16_t freq, uint16_t ch);
extern void     stub_set_link_signal(int idx, bool rssi_valid, int8_t rssi, bool snr_valid, int8_t snr);
extern void     stub_reset_link(void);

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

static char g_pw_path[128];
static char g_iplist_path[128];
static char g_radio_path[128];

/* error_cause of the last ack seen by the matching do_* helper below */
static uint16_t g_last_ind_err, g_last_iplist_err, g_last_radio_err, g_last_chgip_err;
static uint16_t g_last_login_err, g_last_pw_err;

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
    g_last_login_err = ack.error_cause;
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
    g_last_pw_err = ack.error_cause;
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
/* Rev1.01 (#102): SetRadioConfig names a SCAN band + channel bitmap instead of
 * a FREQ/CH pair. The legacy (freq, ch) helper signature is kept for the many
 * "any valid config" callers: the band comes from the frequency (or, for
 * freq 0, from the CH field's band byte) and the CH number becomes a bit. */
static void legacy_to_scan(uint16_t freq, uint16_t ch, opc_wlan_radio_cfg_t *w)
{
    uint16_t band = OPC_SCAN_BAND_UNSET;
    if (freq >= 2400 && freq <= 2484)      band = OPC_SCAN_BAND_2_4GHZ;
    else if (freq >= 4900 && freq <= 5925) band = OPC_SCAN_BAND_5GHZ;
    else if (freq >= 5926 && freq <= 7125) band = OPC_SCAN_BAND_6GHZ;
    else if (freq != 0)                    band = 0x0003;             /* unknown band id */
    else if ((ch >> 8) != 0)               band = (uint16_t)(ch >> 8);
    w->scan_band = band;
    memset(w->scan_chlist, 0, sizeof w->scan_chlist);
    uint8_t chn = (uint8_t)(ch & 0xFF);
    if (chn != 0) opc_scan_list_set_channel(w->scan_chlist, band, chn);
}

/* Dispatch a SetRadioConfig request as-is; returns the Result, error in g_last_radio_err. */
static uint16_t do_set_radio_req(opcd_state_t *st, uint32_t cip, const opc_set_radio_config_req_t *req)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_set_radio_config_req_pack(frame, sizeof frame, 7, req);
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

static uint16_t do_set_radio(opcd_state_t *st, uint32_t cip,
                             uint16_t freq, uint16_t ch)
{
    opc_set_radio_config_req_t req;
    memset(&req, 0, sizeof req);
    req.station_type    = OPC_STATION_SINGLE;
    req.wlan1.mode      = OPC_WLAN_MODE_11AX;
    req.wlan1.bandwidth = OPC_BANDWIDTH_20;
    legacy_to_scan(freq, ch, &req.wlan1);
    req.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
    req.wlan2.mode = 0xFF; req.wlan2.bandwidth = 0xFF;   /* Single: invalid values */

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
    req.priority_ch     = 0x02FF;                 /* 5 GHz, band only */
    req.wlan1.mode      = OPC_WLAN_MODE_11AX;
    req.wlan1.bandwidth = OPC_BANDWIDTH_20;
    legacy_to_scan(f1, ch1, &req.wlan1);
    req.wlan2.mode      = OPC_WLAN_MODE_11AX;
    req.wlan2.bandwidth = OPC_BANDWIDTH_20;
    legacy_to_scan(f2, ch2, &req.wlan2);

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
    g_last_chgip_err = ack.error_cause;
    return ack.result;
}

/* Dispatch GetDeviceInfo; return 0 and fill WLAN#1 (and optionally WLAN#2)
 * freq/channel from the ack. w2 pointers may be NULL when only WLAN#1 matters. */
static int do_get_devinfo(opcd_state_t *st, uint32_t cip,
                          uint16_t *w1_freq, uint16_t *w1_ch,
                          uint16_t *w2_freq, uint16_t *w2_ch)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_get_device_info_req_pack(frame, sizeof frame, 1);
    if (fn <= 0) return -1;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return -1;

    opc_get_device_info_ack_t ack;
    if (opc_get_device_info_ack_unpack(resp, (size_t)rlen, &ack) != 0) return -1;
    if (w1_freq) *w1_freq = ack.wlan1.freq_mhz;
    if (w1_ch)   *w1_ch   = ack.wlan1.channel;
    if (w2_freq) *w2_freq = ack.wlan2.freq_mhz;
    if (w2_ch)   *w2_ch   = ack.wlan2.channel;
    return 0;
}

/* Dispatch GetDeviceInfo; return 0 and hand back the whole unpacked ack. */
static int do_get_devinfo_ack(opcd_state_t *st, uint32_t cip,
                              opc_get_device_info_ack_t *ack)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_get_device_info_req_pack(frame, sizeof frame, 1);
    if (fn <= 0) return -1;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return -1;
    return opc_get_device_info_ack_unpack(resp, (size_t)rlen, ack);
}

/* Dispatch GetDeviceInfo; return 0 and fill the management IP triple
 * (ip/netmask/gateway, host order) from the ack — for the device_ip_iface
 * source-selector tests. */
static int do_get_devinfo_ip(opcd_state_t *st, uint32_t cip,
                             uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_get_device_info_req_pack(frame, sizeof frame, 1);
    if (fn <= 0) return -1;

    uint8_t resp[OPC_FRAME_MAX];
    ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0)
        return -1;

    opc_get_device_info_ack_t ack;
    if (opc_get_device_info_ack_unpack(resp, (size_t)rlen, &ack) != 0) return -1;
    if (ip)   *ip   = ack.ip_address;
    if (mask) *mask = ack.subnet_mask;
    if (gw)   *gw   = ack.default_gateway;
    return 0;
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

    /* 6. E1 (DFK 2026-06-29 written answer): password / ESSID character whitelist
     *    = A-Z a-z 0-9 . - _ + / : = ~ @ (the source's lone backtick is excluded
     *    as a typo). proto-todo §DFK-2026-06-18 (에러1). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    r = do_login(&st, CIP, "bad pass!");            /* space and '!' disallowed */
    ASSERT(r == OPC_RESULT_NG && g_last_login_err == OPC_ERR_LOGIN_PW_CHAR,
           "E1: Login invalid password char → 0x0011");
    r = do_login(&st, CIP, "tick`bad");             /* backtick disallowed (the typo char) */
    ASSERT(r == OPC_RESULT_NG && g_last_login_err == OPC_ERR_LOGIN_PW_CHAR,
           "E1: Login backtick → 0x0011");
    { char ctl[8]; ctl[0] = 'a'; ctl[1] = (char)0x01; ctl[2] = 'b'; ctl[3] = '\0';
      r = do_login(&st, CIP, ctl);                  /* control char disallowed */
      ASSERT(r == OPC_RESULT_NG && g_last_login_err == OPC_ERR_LOGIN_PW_CHAR,
             "E1: Login control char → 0x0011"); }
    init_state(&st, "Good.Pass-1_+/:=~@");          /* every allowed special char */
    r = do_login(&st, CIP, "Good.Pass-1_+/:=~@");
    ASSERT(r == OPC_RESULT_OK, "E1: Login full allowed charset → OK");

    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_password(&st, CIP, OPC_PASSWORD_DEFAULT, "newbad`tick");  /* backtick disallowed */
    ASSERT(r == OPC_RESULT_NG && g_last_pw_err == OPC_ERR_NEW_PW_CHAR,
           "E1: SetPassword new password backtick → 0x0013");
    r = do_set_password(&st, CIP, "old bad!", "GoodNew1");
    ASSERT(r == OPC_RESULT_NG && g_last_pw_err == OPC_ERR_OLD_PW_CHAR,
           "E1: SetPassword old password invalid char → 0x0011");

    /* ESSID invalid char → 0x0015 (do_set_ip_list takes no essid arg → inline). */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    {
        opc_set_ip_config_list_req_t ipreq;
        memset(&ipreq, 0, sizeof ipreq);
        ipreq.entry_count = 1;
        ipreq.entries[0].boundary_flag = OPC_LIST_BOUNDARY_START;
        ipreq.entries[0].list_number   = 1;
        ipreq.entries[0].ip_address    = 0xC0A80165u;
        ipreq.entries[0].subnet_mask   = 0xFFFFFF00u;
        strncpy(ipreq.entries[0].essid, "bad essid!",
                sizeof ipreq.entries[0].essid - 1);
        uint8_t f[OPC_FRAME_MAX]; ssize_t fn =
            opc_set_ip_config_list_req_pack(f, sizeof f, 5, &ipreq);
        uint8_t rp[OPC_FRAME_MAX]; ssize_t rl = 0;
        (void)opcd_dispatch(&st, f, (size_t)fn, CIP, 5000, rp, sizeof rp, &rl);
        opc_set_ip_config_list_ack_t eack;
        (void)opc_set_ip_config_list_ack_unpack(rp, (size_t)rl, &eack);
        ASSERT(eack.result == OPC_RESULT_NG &&
               eack.error_cause == OPC_ERR_IPCFG_ESSID_CHAR,
               "E1: ESSID invalid char → 0x0015");
    }

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
    {
        opc_ipcfg_entry_t e13;
        memset(&e13, 0, sizeof e13);
        e13.boundary_flag = OPC_LIST_BOUNDARY_START;
        e13.list_number   = 1;
        e13.ip_address    = 0xC0A80165; /* 192.168.1.101 */
        e13.subnet_mask   = 0xFFFFFF00u;
        strncpy(e13.essid, "cantops-x", sizeof e13.essid - 1);
        r = do_set_ip_entry(&st, CIP, &e13);
    }
    ASSERT(r == OPC_RESULT_OK, "change-ip: set-ip-list start ok");
    {
        opc_ipcfg_entry_t e13end;
        memset(&e13end, 0, sizeof e13end);
        e13end.boundary_flag = OPC_LIST_BOUNDARY_END;
        e13end.list_number   = 1;
        e13end.ip_address    = 0xC0A80165;
        e13end.subnet_mask   = 0xFFFFFF00u;
        strncpy(e13end.essid, "cantops-x", sizeof e13end.essid - 1);
        r = do_set_ip_entry(&st, CIP, &e13end);
    }
    ASSERT(r == OPC_RESULT_OK, "change-ip: set-ip-list commit ok");
    r = do_change_ip(&st, CIP, 1);
    ASSERT(r == OPC_RESULT_OK, "change-ip: accepted");
    ASSERT(stub_apply_ip_calls() == 0, "change-ip: apply deferred (not before logout)");
    ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "change-ip: logout ok");
    opcd_apply_pending_ip_change(&st);   /* main loop applies after logout response */
    ASSERT(stub_apply_ip_calls() == 1, "change-ip: platform apply_ip_change called on logout");
    ASSERT(stub_apply_ip_last_ip() == 0xC0A80165, "change-ip: apply gets committed slot ip");
    ASSERT(stub_apply_ip_last_iface() == 0, "change-ip: default device_ip_iface applies to eth0");
    ASSERT(strcmp(stub_apply_ip_last_essid(), "cantops-x") == 0,
           "change-ip: apply gets committed slot essid");

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

    /* 14d. #107 Rev1.01 §4.3.6: List Boundary 0x0003 (시작 및 완료) commits in a
     *      SINGLE frame — seed from the committed list (merge), apply the entry,
     *      commit + persist to NVRAM, all in one entry. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165 /*.1.101*/);
    r = do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
    ASSERT(r == OPC_RESULT_OK, "0x0003: baseline slot 1 committed via normal cycle");
    r = do_set_ip_list(&st, CIP, 5, OPC_LIST_BOUNDARY_START_END, 0xC0A80205 /*.2.5*/);
    ASSERT(r == OPC_RESULT_OK, "0x0003: single-frame commit OK");
    ASSERT(!st.ip_list_staging_active, "0x0003: staging closed after single-frame commit");
    ASSERT(st.ip_list.present[4] == 1 && st.ip_list.slots[4].ip_address == 0xC0A80205,
           "0x0003: slot 5 committed in one frame");
    ASSERT(st.ip_list.present[0] == 1 && st.ip_list.slots[0].ip_address == 0xC0A80165,
           "0x0003: prior slot 1 survives (seed/merge from committed list)");
    {
        static opcd_ip_list_t disk_list;
        ASSERT(opc_store_read_all(g_iplist_path, &disk_list, sizeof disk_list) == (ssize_t)sizeof disk_list &&
               disk_list.present[4] == 1 && disk_list.slots[4].ip_address == 0xC0A80205 &&
               disk_list.present[0] == 1,
               "0x0003: single-frame commit persisted to NVRAM (비휘발 기록)");
    }

    /* 14e. #107: 0x0003 is self-contained. A CONTINUE/END that FOLLOWS it has no
     *      open cycle → START-less sequence → NG 0x0018, exactly like a lone one,
     *      and commits nothing. */
    r = do_set_ip_list(&st, CIP, 6, OPC_LIST_BOUNDARY_CONTINUE, 0xC0A80206);
    ASSERT(r == OPC_RESULT_NG && g_last_iplist_err == OPC_ERR_LIST_SEQUENCE,
           "0x0003: a CONTINUE after a completed single-frame commit → NG 0x0018");
    ASSERT(st.ip_list.present[5] == 0, "0x0003: the stray CONTINUE committed nothing");

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

    /* 14d. A14 (DFK 2026-06-29 written answer: "0x0013 is a typo, will be deleted
     *      on spec update"): SetIndication from a logged-in *other* IP → 0x0002
     *      (the common login-condition); the not-logged-in case stays 0x0001. */
    init_state(&st, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP, 0x0A0A0A0A, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_LOGIN_VIOLATION,
           "A14: not logged in → 0x0001 unchanged");
    (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
    r = do_set_indication(&st, CIP + 1, 0x0A0A0A0A, 6000, OPC_IND_BIT_KEEP_ALIVE, 5);
    ASSERT(r == OPC_RESULT_NG && g_last_ind_err == OPC_ERR_LOGIN_CONDITION,
           "A14: other IP while logged in → 0x0002 (0x0013 deleted per DFK)");

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

    /* 14g (Rev1.01 §4.3.8, #102): SCAN Frequency Band / Channel List validation. */
    r = do_set_radio(&st, CIP, 6000, 0);          /* 6 GHz band */
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "A21: 6 GHz band → 0x0011");
    r = do_set_radio(&st, CIP, 3000, 0);          /* unknown band id */
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "unknown band id → 0x0011");
    r = do_set_radio(&st, CIP, 0, (uint16_t)((OPC_BAND_6GHZ << 8) | 37));
    ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
           "A21: 6 GHz band via CH band byte → 0x0011");
    {
        opc_set_radio_config_req_t bad;
        memset(&bad, 0, sizeof bad);
        bad.station_type    = OPC_STATION_SINGLE;
        bad.wlan1.mode      = OPC_WLAN_MODE_11AX;
        bad.wlan1.bandwidth = OPC_BANDWIDTH_20;
        bad.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
        bad.wlan1.scan_chlist[0] = 0x02;              /* row A bit25: unassigned in 5 GHz */
        bad.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
        r = do_set_radio_req(&st, CIP, &bad);
        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
               "5 GHz list bit25 (unassigned) → 0x0012");
        bad.wlan1.scan_band = OPC_SCAN_BAND_UNSET;   /* unset band but channels listed */
        bad.wlan1.scan_chlist[0] = 0;
        bad.wlan1.scan_chlist[3] = 0x01;
        r = do_set_radio_req(&st, CIP, &bad);
        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
               "unset band with a channel list → 0x0012");
        memset(bad.wlan1.scan_chlist, 0, sizeof bad.wlan1.scan_chlist);
        r = do_set_radio_req(&st, CIP, &bad);
        ASSERT(r == OPC_RESULT_OK, "unset band + empty list (no band lock) → OK");
        memset(bad.wlan1.scan_chlist, 0, sizeof bad.wlan1.scan_chlist);
        bad.wlan1.scan_band = OPC_SCAN_BAND_2_4GHZ;  /* list carried in row B (bytes 4..7) */
        bad.wlan1.scan_chlist[6] = 0x04;
        bad.wlan1.scan_chlist[7] = 0x21;
        r = do_set_radio_req(&st, CIP, &bad);
        ASSERT(r == OPC_RESULT_OK, "2.4 GHz list in row B tolerated (lenient row order) → OK");
    }
    r = do_set_radio(&st, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));
    ASSERT(r == OPC_RESULT_OK, "valid 2.4 GHz band + ch1 accepted");
    /* An identical re-send must not reconfigure wpa_supplicant (link drop):
     * OK without an apply call. */
    stub_apply_radio_reset_calls();
    r = do_set_radio(&st, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));
    ASSERT(r == OPC_RESULT_OK && stub_apply_radio_calls() == 0,
           "identical config re-sent → OK, apply skipped");

    /* 14g-2. A21 DUAL: wlan2 band / priority_ch validation (Rev1.01). */
    {
        opc_set_radio_config_req_t rreq;
        memset(&rreq, 0, sizeof rreq);
        rreq.station_type    = OPC_STATION_DUAL;
        rreq.priority_ch     = 0x02FF;               /* 5 GHz, band only */
        rreq.wlan1.mode      = OPC_WLAN_MODE_11AX;
        rreq.wlan1.bandwidth = OPC_BANDWIDTH_20;
        rreq.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
        rreq.wlan2.mode      = OPC_WLAN_MODE_11AX;
        rreq.wlan2.bandwidth = OPC_BANDWIDTH_20;
        rreq.wlan2.scan_band = OPC_SCAN_BAND_6GHZ;
        uint16_t rr = do_set_radio_req(&st, CIP, &rreq);
        ASSERT(rr == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_FREQ,
               "A21: DUAL wlan2 6 GHz band → 0x0011");
        rreq.wlan2.scan_band = OPC_SCAN_BAND_2_4GHZ;
        rreq.priority_ch     = (uint16_t)((OPC_BAND_6GHZ << 8) | 1);
        rr = do_set_radio_req(&st, CIP, &rreq);
        ASSERT(rr == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
               "A21: DUAL priority_ch 6 GHz → 0x0012");
        rreq.priority_ch = (uint16_t)((OPC_BAND_5GHZ << 8) | 38);   /* not in the 5 GHz table */
        rr = do_set_radio_req(&st, CIP, &rreq);
        ASSERT(rr == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_RADIO_CH,
               "DUAL priority_ch 5 GHz ch38 → 0x0012");
        rreq.priority_ch = (uint16_t)((OPC_BAND_5GHZ << 8) | 36);
        rr = do_set_radio_req(&st, CIP, &rreq);
        ASSERT(rr == OPC_RESULT_OK, "DUAL 5 GHz/2.4 GHz bands, priority ch36 → OK");
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
        legacy_to_scan(5180, 36, &rreq.wlan1);   /* 5 GHz, ch36 bit */
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
        ASSERT(st.radio_committed, "async set-radio: committed once the write completed");
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
               disk_radio.wlan1.scan_band == OPC_SCAN_BAND_5GHZ &&
               disk_radio.wlan1.scan_chlist[3] == 0x01 &&
               disk_radio.wlan1.mode == OPC_WLAN_MODE_11A,
               "async set-radio: NVRAM file written");

        /* 21. A19 / Rev1.01 그림 4-2: a byte-identical retransmission (new SN)
         *     arriving while the ORIGINAL request's NVRAM write is still in
         *     flight is DISCARDED — it is not re-applied and starts no second
         *     write — and the single ack carries the ORIGINAL request's SN, not
         *     the retransmission's. (#104 inverts Rev1.00, which answered the
         *     newest SN.) */
        opc_set_radio_config_req_t a19r;
        memset(&a19r, 0, sizeof a19r);
        a19r.station_type    = OPC_STATION_SINGLE;
        a19r.wlan1.mode      = OPC_WLAN_MODE_11A;
        a19r.wlan1.bandwidth = OPC_BANDWIDTH_20;
        /* Must differ from test 20's committed config so the original takes the
         * deferred (apply+persist) path, not the "nothing to apply" shortcut
         * (#102). The retransmission below is byte-identical to it. */
        legacy_to_scan(5200, 40, &a19r.wlan1);
        stub_apply_radio_reset_calls();
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 90, &a19r);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19: original request (SN=90) deferred");
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 91, &a19r);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19: retransmission (SN=91) gives no immediate response");
        ASSERT(stub_apply_radio_calls() == 1,
               "A19: retransmission is NOT re-applied (apply called once, for the original)");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0,
               "A19: completion signalled");
        opcd_store_async_on_ready(&st);
        if (wait_fd_readable(opc_store_async_event_fd(sa), 1000) == 0)
            opcd_store_async_on_ready(&st);   /* only the original enqueued a write */
        ASSERT(wait_fd_readable(cli, 5000) == 0, "A19: an ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        ASSERT(rn > 0 &&
               opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 90 &&
               opc_set_radio_config_ack_unpack(rx_buf, (size_t)rn, &rack) == 0 &&
               rack.result == OPC_RESULT_OK,
               "A19: the single ack carries the ORIGINAL SN (90), not the retransmission's");
        ASSERT(wait_fd_readable(cli, 300) != 0,
               "A19: no second ack for the discarded retransmission");

        /* 21b. Rev1.01 그림 4-2 엇갈림(cross): a retransmission arriving AFTER
         *      the original response was already sent is a fresh request,
         *      answered with its OWN SN. {5200/40} is committed now (21 drained),
         *      so an identical SN=92 takes the shortcut and is answered at once —
         *      two acks total across 21+21b, each carrying its own SN. */
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 92, &a19r);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port,
                             resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen > 0, "A19 cross: post-response retransmission answered immediately");
        ASSERT(opc_frame_parse(resp, (size_t)rlen, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 92 &&
               opc_set_radio_config_ack_unpack(resp, (size_t)rlen, &rack) == 0 &&
               rack.result == OPC_RESULT_OK,
               "A19 cross: crossed retransmission answered with its own SN (92)");

        /* 21c. Retry classification must compare the WHOLE payload, not the
         *      apply-relevant fields. radio_cfg_differs() ignores priority_ch /
         *      WLAN#2 for SINGLE, but a request that differs there is a DISTINCT
         *      wire frame, not a retransmission — dropping it would answer the
         *      original SN and time the client out (Codex, PR #113). Original
         *      A(SN=93) in flight, then B(SN=94): same WLAN#1, different
         *      priority_ch → B must be processed and answered with ITS OWN SN. */
        stub_apply_radio_reset_calls();
        opc_set_radio_config_req_t rqA = a19r;   /* SINGLE, {5200/40} */
        legacy_to_scan(5220, 44, &rqA.wlan1);    /* differ from committed → deferred */
        rqA.priority_ch = 0x0000;
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 93, &rqA);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port, resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19 payload: original A(SN=93) deferred");
        opc_set_radio_config_req_t rqB = rqA;    /* same WLAN#1 ... */
        rqB.priority_ch = 0x1234;                /* ... but a different wire byte */
        fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 94, &rqB);
        rlen = -1;
        drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port, resp, sizeof resp, &rlen);
        ASSERT(drc == 0 && rlen == 0, "A19 payload: distinct B(SN=94) not answered inline");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0, "A19 payload: completion signalled");
        opcd_store_async_on_ready(&st);
        if (wait_fd_readable(opc_store_async_event_fd(sa), 1000) == 0)
            opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0, "A19 payload: an ack arrived");
        rn = recv(cli, rx_buf, sizeof rx_buf, 0);
        ASSERT(rn > 0 &&
               opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
               ahdr.sequence_number == 94 &&
               opc_set_radio_config_ack_unpack(rx_buf, (size_t)rn, &rack) == 0 &&
               rack.result == OPC_RESULT_OK,
               "A19 payload: distinct request answered with its OWN SN (94), not dropped as a retry");
        ASSERT(wait_fd_readable(cli, 300) != 0, "A19 payload: exactly one ack");

        /* 21d. A retry must be matched against THIS port's pending request, not
         *      the global st->radio. Session ownership is IP-scoped but pending
         *      slots are (ip,port)-scoped: a DISTINCT request Y from a second
         *      source port overwrites st->radio while port P1's original X is
         *      still in flight. Comparing X's retransmission against st->radio(=Y)
         *      would misjudge it as a new request and answer P1 with the
         *      retransmission SN — X's ORIGINAL SN must win (Codex, PR #113 r2). */
        {
            uint16_t cli2_port = 0;
            int cli2 = bind_loopback_udp(&cli2_port);
            ASSERT(cli2 >= 0, "A19 multiport: second client socket");
            stub_apply_radio_reset_calls();
            opc_set_radio_config_req_t rqX = a19r;           /* SINGLE */
            legacy_to_scan(5180, 36, &rqX.wlan1);            /* differ from committed → deferred */
            rqX.priority_ch = 0x0000;
            fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 95, &rqX);
            rlen = -1;
            drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port, resp, sizeof resp, &rlen);
            ASSERT(drc == 0 && rlen == 0, "A19 multiport: X(SN=95) from P1 deferred");
            opc_set_radio_config_req_t rqY = a19r;
            legacy_to_scan(5745, 149, &rqY.wlan1);           /* distinct payload */
            rqY.priority_ch = 0x0000;
            fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 96, &rqY);
            rlen = -1;
            drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli2_port, resp, sizeof resp, &rlen);
            ASSERT(drc == 0 && rlen == 0, "A19 multiport: distinct Y(SN=96) from P2 deferred");
            /* X retransmission from P1 (new SN, identical payload to X) */
            fn   = opc_set_radio_config_req_pack(frame, sizeof frame, 97, &rqX);
            rlen = -1;
            drc  = opcd_dispatch(&st, frame, (size_t)fn, LOOP, cli_port, resp, sizeof resp, &rlen);
            ASSERT(drc == 0 && rlen == 0, "A19 multiport: X retransmission(SN=97) from P1 no inline resp");
            for (int d = 0; d < 4; d++) {
                if (wait_fd_readable(opc_store_async_event_fd(sa), 2000) != 0) break;
                opcd_store_async_on_ready(&st);
            }
            ASSERT(wait_fd_readable(cli, 5000) == 0, "A19 multiport: P1 ack arrived");
            rn = recv(cli, rx_buf, sizeof rx_buf, 0);
            ASSERT(rn > 0 && opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
                   ahdr.sequence_number == 95,
                   "A19 multiport: P1 answered with X's ORIGINAL SN (95), not the retransmission SN");
            ASSERT(wait_fd_readable(cli, 300) != 0, "A19 multiport: P1 exactly one ack");
            ASSERT(wait_fd_readable(cli2, 5000) == 0, "A19 multiport: P2 ack arrived");
            rn = recv(cli2, rx_buf, sizeof rx_buf, 0);
            ASSERT(rn > 0 && opc_frame_parse(rx_buf, (size_t)rn, &ahdr, NULL, NULL) == 0 &&
                   ahdr.sequence_number == 96,
                   "A19 multiport: P2 answered with Y's SN (96)");
            close(cli2);
        }

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

        /* 22b. D12/D13 lenient receive-length model (opcd_intake_frame_len,
         *      2026-06-16): trust the declared Length → dispatch 8+Length and
         *      ignore trailing wire bytes; runt / 9..63 / over-max / truncated
         *      return 0 (→ reject path). Pure function, no socket. */
        {
            uint8_t f[OPC_FRAME_MAX];
            opc_header_t h = { .protocol_version  = OPC_PROTOCOL_VERSION,
                               .command_type      = OPC_CMD_REQUEST,
                               .req_indication_id = OPC_REQ_LOGOUT,
                               .sequence_number   = 1, .length = 0 };
            memset(f, 0, sizeof f);

            h.length = 0; (void)opc_fixed_header_pack(f, &h);
            ASSERT(opcd_intake_frame_len(f, OPC_FIXED_HEADER_SIZE) == OPC_FIXED_HEADER_SIZE,
                   "intake: empty frame (Length=0) → 8 B");
            ASSERT(opcd_intake_frame_len(f, 30) == OPC_FIXED_HEADER_SIZE,
                   "intake: empty frame + trailing → 8 B (trailing ignored)");

            h.length = OPC_HEADER_SIZE - OPC_FIXED_HEADER_SIZE;  /* 56 → want 64 */
            (void)opc_fixed_header_pack(f, &h);
            ASSERT(opcd_intake_frame_len(f, OPC_HEADER_SIZE) == OPC_HEADER_SIZE,
                   "intake: full-header frame exact → 64 B");
            ASSERT(opcd_intake_frame_len(f, 200) == OPC_HEADER_SIZE,
                   "intake: valid Length + longer datagram → 8+Length (trailing ignored)");

            h.length = 68; (void)opc_fixed_header_pack(f, &h);   /* want 76 */
            ASSERT(opcd_intake_frame_len(f, OPC_FRAME_MAX) == 76,
                   "intake: MSG_TRUNC overflow, valid small Length → 8+Length (D12 win)");

            h.length = (uint16_t)(OPC_FRAME_MAX - OPC_FIXED_HEADER_SIZE + 1);  /* 1417 */
            (void)opc_fixed_header_pack(f, &h);
            ASSERT(opcd_intake_frame_len(f, OPC_FRAME_MAX) == 0,
                   "intake: declared Length > max → 0 (bad length)");

            h.length = 22; (void)opc_fixed_header_pack(f, &h);   /* want 30 (9..63) */
            ASSERT(opcd_intake_frame_len(f, 30) == 0,
                   "intake: 9..63 B extent → 0 (malformed)");
            h.length = 1;  (void)opc_fixed_header_pack(f, &h);   /* want 9 — first of 9..63 */
            ASSERT(opcd_intake_frame_len(f, 9) == 0,
                   "intake: want=9 (first of 9..63) → 0 (boundary)");
            h.length = 55; (void)opc_fixed_header_pack(f, &h);   /* want 63 — last of 9..63 */
            ASSERT(opcd_intake_frame_len(f, 63) == 0,
                   "intake: want=63 (last of 9..63) → 0 (boundary)");

            h.length = 200; (void)opc_fixed_header_pack(f, &h);  /* want 208 */
            ASSERT(opcd_intake_frame_len(f, 100) == 0,
                   "intake: datagram shorter than declared frame → 0 (truncated)");

            ASSERT(opcd_intake_frame_len(f, 4) == 0,
                   "intake: sub-header runt → 0");
            ASSERT(opcd_intake_frame_len(NULL, OPC_HEADER_SIZE) == 0,
                   "intake: NULL frame → 0");
        }

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
        opc_wlan_radio_cfg_t saved_w1 = st24c.radio.wlan1;
        const uint16_t saved_freq = 2412;   /* derived from the primed 2.4 GHz ch1 list */

        /* Inject a single apply failure and submit a different config. Reset the
         * counter after priming so it measures only the failing Set-Radio. */
        stub_apply_radio_reset_calls();
        stub_apply_radio_set_fail_once(1);
        (void)do_set_radio(&st24c, CIP, 5180, 36);

        ASSERT(st24c.radio.wlan1.scan_band == saved_w1.scan_band,
               "issue#12-24c: apply failure preserves previous freq_mhz");
        ASSERT(memcmp(st24c.radio.wlan1.scan_chlist, saved_w1.scan_chlist, OPC_SCAN_CHLIST_LEN) == 0,
               "issue#12-24c: apply failure preserves previous channel");
        /* Dispatch did exactly ONE apply (the failing one) — the revert is
         * deferred, so the NG response is never delayed by a second apply. */
        ASSERT(stub_apply_radio_calls() == 1,
               "issue#12-24c: dispatch does a single apply (revert deferred)");
        ASSERT(st24c.radio_revert_pending,
               "issue#12-24c: apply failure arms the deferred last-good revert");
        ASSERT(st24c.radio_revert_cfg.wlan1.scan_band == saved_w1.scan_band &&
               memcmp(st24c.radio_revert_cfg.wlan1.scan_chlist, saved_w1.scan_chlist, OPC_SCAN_CHLIST_LEN) == 0,
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
        ASSERT(st24d.radio.wlan1.scan_band == OPC_SCAN_BAND_5GHZ && st24d.radio.wlan1.scan_chlist[3] == 0x01,
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
        const uint16_t saved_f1 = 2412;   /* derived from the primed 2.4 GHz ch1 list */

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

    /* 24g. Identical config re-sent (#102 supersedes the Gemini-review guard):
     *      nothing to apply, so the platform is not called at all — no link
     *      drop, no revert to arm, and a still-armed apply failure cannot fire. */
    {
        opcd_state_t st24g;
        init_state(&st24g, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st24g, CIP, OPC_PASSWORD_DEFAULT);
        (void)do_set_radio(&st24g, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));

        stub_apply_radio_reset_calls();
        /* Resubmit the identical config. */
        uint16_t r = do_set_radio(&st24g, CIP, 2412, (uint16_t)((OPC_BAND_2_4GHZ << 8) | 1));

        ASSERT(r == OPC_RESULT_OK, "issue#12-24g: identical config → OK");
        ASSERT(stub_apply_radio_calls() == 0,
               "issue#12-24g: identical config → apply not called");
        ASSERT(!st24g.radio_revert_pending,
               "issue#12-24g: identical config → no revert armed (nothing to undo)");
    }

    /* 25. device-info FREQ/CH source toggle (device_info_freq_source).
     *     set-radio stores 5180/ch36(0x0224) as the config; the live link is
     *     5240/ch48 → opc_chan_field(5240,48)=0x0230. Verify 3 modes × assoc. */
    {
        const uint16_t CFG_FREQ = 5180, CFG_CH = (uint16_t)((OPC_BAND_5GHZ << 8) | 36); /* 0x0224 */
        const uint16_t LIVE_FREQ = 5240, LIVE_CH_RAW = 48;
        const uint16_t LIVE_CH_ENC = (uint16_t)((OPC_BAND_5GHZ << 8) | 48);              /* 0x0230 */
        uint16_t f = 0, c = 0;

        /* config: always the set-radio value, regardless of association */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        (void)do_set_radio(&st, CIP, CFG_FREQ, CFG_CH);
        st.conf.device_info_freq_source = OPC_FREQ_SRC_CONFIG;
        stub_set_link(0, true, LIVE_FREQ, LIVE_CH_RAW);
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == CFG_FREQ && c == CFG_CH,
               "freq-src config + assoc -> config value");
        stub_reset_link();
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == CFG_FREQ && c == CFG_CH,
               "freq-src config + not-assoc -> config value");

        /* live: associated -> live (band-encoded), not-assoc -> 0/0 */
        st.conf.device_info_freq_source = OPC_FREQ_SRC_LIVE;
        stub_set_link(0, true, LIVE_FREQ, LIVE_CH_RAW);
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == LIVE_FREQ && c == LIVE_CH_ENC,
               "freq-src live + assoc -> live value (band-encoded)");
        stub_reset_link();
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == 0xFFFF && c == 0xFFFF,
               "freq-src live + not-assoc -> 0xFFFF/0xFFFF (Rev1.01 unset)");

        /* auto: associated -> live, not-assoc -> config */
        st.conf.device_info_freq_source = OPC_FREQ_SRC_AUTO;
        stub_set_link(0, true, LIVE_FREQ, LIVE_CH_RAW);
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == LIVE_FREQ && c == LIVE_CH_ENC,
               "freq-src auto + assoc -> live value");
        stub_reset_link();
        ASSERT(do_get_devinfo(&st, CIP, &f, &c, NULL, NULL) == 0 && f == CFG_FREQ && c == CFG_CH,
               "freq-src auto + not-assoc -> config value");

        /* DUAL: exercises the WLAN#2 branch + the stub idx=1 injection
         * (Gemini review). auto + both associated → wlan1 & wlan2 live values.
         * wlan2 live 5745/ch149 → opc_chan_field(5745,149)=0x0295. */
        {
            const uint16_t W2_FREQ = 5745, W2_CH_RAW = 149;
            const uint16_t W2_CH_ENC = (uint16_t)((OPC_BAND_5GHZ << 8) | 149); /* 0x0295 */
            uint16_t f2 = 0, c2 = 0;
            init_state(&st, OPC_PASSWORD_DEFAULT);
            (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
            (void)do_set_radio_dual(&st, CIP, CFG_FREQ, CFG_CH, CFG_FREQ, CFG_CH);
            st.conf.device_info_freq_source = OPC_FREQ_SRC_AUTO;
            stub_set_link(0, true, LIVE_FREQ, LIVE_CH_RAW);
            stub_set_link(1, true, W2_FREQ, W2_CH_RAW);
            ASSERT(do_get_devinfo(&st, CIP, &f, &c, &f2, &c2) == 0 &&
                   f == LIVE_FREQ && c == LIVE_CH_ENC &&
                   f2 == W2_FREQ && c2 == W2_CH_ENC,
                   "freq-src auto DUAL + assoc -> wlan1 & wlan2 live values");
            stub_reset_link();
        }
    }

    /* 26(#101/#102). Rev1.01 SCAN Frequency Band / SCAN Channel List elements
     *     echo the committed SetRadioConfig; a never-configured WLAN reads as
     *     unset (0xFFFF) + empty list, never 0x0000. Single: WLAN#2 unset. */
    {
        opc_get_device_info_ack_t ack;
        static const uint8_t zero8[OPC_SCAN_CHLIST_LEN] = {0};
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "devinfo ack before any set-radio");
        ASSERT(ack.wlan1.scan_band == OPC_SCAN_BAND_UNSET, "unconfigured wlan1 scan band = unset 0xFFFF");
        ASSERT(memcmp(ack.wlan1.scan_chlist, zero8, sizeof zero8) == 0, "unconfigured wlan1 list = ALL 0");
        (void)do_set_radio(&st, CIP, 5180, (uint16_t)((OPC_BAND_5GHZ << 8) | 36));
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "devinfo ack after set-radio");
        ASSERT(ack.wlan1.scan_band == OPC_SCAN_BAND_5GHZ, "wlan1 scan band echoes 5 GHz");
        ASSERT(ack.wlan1.scan_chlist[3] == 0x01 && ack.wlan1.scan_chlist[0] == 0 && ack.wlan1.scan_chlist[7] == 0,
               "wlan1 scan chlist echoes ch36 = row A bit0");
        ASSERT(ack.wlan2.scan_band == OPC_SCAN_BAND_UNSET, "single: wlan2 scan band = unset 0xFFFF");
        ASSERT(memcmp(ack.wlan2.scan_chlist, zero8, sizeof zero8) == 0, "single: wlan2 scan chlist = ALL 0");
    }

    /* 26b(#102). radio.conf decoder: current layout as-is, Rev1.00 16-byte
     *     layout converted (band from FREQ, channel bit from CH), other → -1. */
    {
        opc_set_radio_config_req_t cur, out;
        memset(&cur, 0, sizeof cur);
        cur.station_type = OPC_STATION_DUAL;
        cur.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
        cur.wlan1.scan_chlist[3] = 0x01;
        ASSERT(opcd_radio_conf_decode(&cur, sizeof cur, &out) == 0 && memcmp(&out, &cur, sizeof cur) == 0,
               "decode: current layout copied as-is");
        uint8_t legacy[OPCD_RADIO_CONF_LEGACY_LEN];
        memset(legacy, 0, sizeof legacy);
        uint16_t v;
        v = OPC_STATION_SINGLE; memcpy(&legacy[0], &v, 2);     /* station */
        v = 5180;               memcpy(&legacy[4], &v, 2);     /* w1 freq */
        v = 0x0224;             memcpy(&legacy[6], &v, 2);     /* w1 ch (5 GHz, 36) */
        legacy[8] = OPC_WLAN_MODE_11AX;                        /* w1 mode */
        legacy[9] = OPC_BANDWIDTH_80;                          /* w1 bw */
        ASSERT(opcd_radio_conf_decode(legacy, sizeof legacy, &out) == 1, "decode: legacy layout converted");
        ASSERT(out.station_type == OPC_STATION_SINGLE && out.wlan1.mode == OPC_WLAN_MODE_11AX &&
               out.wlan1.bandwidth == OPC_BANDWIDTH_80, "decode: legacy scalars kept");
        ASSERT(out.wlan1.scan_band == OPC_SCAN_BAND_5GHZ && out.wlan1.scan_chlist[3] == 0x01,
               "decode: legacy 5180/ch36 → 5 GHz, row A bit0");
        ASSERT(out.wlan2.scan_band == OPC_SCAN_BAND_UNSET && opc_scan_list_empty(out.wlan2.scan_chlist),
               "decode: legacy wlan2 (freq 0) → unset band, empty list");
        ASSERT(opcd_radio_conf_decode(legacy, 20, &out) == -1, "decode: unknown size → -1");
    }

    /* 26c(#102). The identical-request shortcut applies only to a COMMITTED
     *     config. A matching but uncommitted state (never applied) must still
     *     reach the platform; once committed, the same request is skipped. */
    {
        opc_set_radio_config_req_t req;
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        memset(&req, 0, sizeof req);
        req.station_type    = OPC_STATION_SINGLE;
        req.wlan1.mode      = OPC_WLAN_MODE_11AX;
        req.wlan1.bandwidth = OPC_BANDWIDTH_20;
        req.wlan1.scan_band = OPC_SCAN_BAND_UNSET;
        req.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
        req.wlan2.mode = 0xFF; req.wlan2.bandwidth = 0xFF;
        st.radio = req;                       /* matches, but never applied */
        st.radio_committed = false;
        stub_apply_radio_reset_calls();
        ASSERT(do_set_radio_req(&st, CIP, &req) == OPC_RESULT_OK && stub_apply_radio_calls() == 1,
               "matching but uncommitted config → applied, not skipped");
        ASSERT(st.radio_committed, "sync persist → committed");
        ASSERT(do_set_radio_req(&st, CIP, &req) == OPC_RESULT_OK && stub_apply_radio_calls() == 1,
               "matching committed config → skipped");
    }

    /* 26d(#102, Codex P1). A request whose NVRAM write fails is applied but NOT
     *     committed: the identical retry must run apply + persist again instead
     *     of taking the shortcut (which would drop the config on reboot). */
    {
        char bad_path[160];
        snprintf(bad_path, sizeof bad_path, "no_such_dir_%d/radio", (int)getpid());
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        st.paths.radio = bad_path;
        stub_apply_radio_reset_calls();
        uint16_t r = do_set_radio(&st, CIP, 5180, 36);
        ASSERT(r == OPC_RESULT_NG && g_last_radio_err == OPC_ERR_NVRAM, "persist failure → NG 0x0004");
        ASSERT(!st.radio_committed && stub_apply_radio_calls() == 1, "applied once, uncommitted");
        st.paths.radio = g_radio_path;
        unlink(g_radio_path);
        r = do_set_radio(&st, CIP, 5180, 36);
        ASSERT(r == OPC_RESULT_OK && stub_apply_radio_calls() == 2,
               "identical retry after NVRAM failure → re-applied and persisted");
        ASSERT(st.radio_committed, "retry → committed");
        opc_set_radio_config_req_t disk;
        ASSERT(opc_store_read_all(g_radio_path, &disk, sizeof disk) == (ssize_t)sizeof disk &&
               disk.wlan1.scan_band == OPC_SCAN_BAND_5GHZ, "retry wrote radio.conf");
    }

    /* 26e(#102, round 4). A deferred radio write commits only if it is still the
     *     CURRENT generation when it completes. Simulate a newer request having
     *     replaced st->radio (generation bump) before the older write drains:
     *     that completion must NOT commit; a current-generation write must. */
    {
        uint16_t cport = 0;
        int srv = bind_loopback_udp(NULL);
        int cli = bind_loopback_udp(&cport);
        opc_store_async_t *sa = opc_store_async_create();
        ASSERT(srv >= 0 && cli >= 0 && sa != NULL, "26e: rig up");
        const uint32_t LOOP = 0x7F000001;
        init_state(&st, OPC_PASSWORD_DEFAULT);
        st.udp_fd      = srv;
        st.store_async = sa;
        (void)do_login(&st, LOOP, OPC_PASSWORD_DEFAULT);

        opc_set_radio_config_req_t rq;
        memset(&rq, 0, sizeof rq);
        rq.station_type    = OPC_STATION_SINGLE;
        rq.wlan1.mode      = OPC_WLAN_MODE_11AX;
        rq.wlan1.bandwidth = OPC_BANDWIDTH_20;
        legacy_to_scan(5180, 36, &rq.wlan1);
        rq.wlan2.scan_band = OPC_SCAN_BAND_UNSET;
        uint8_t fr[OPC_FRAME_MAX], rs[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
        ssize_t rl = -1;
        ssize_t fl = opc_set_radio_config_req_pack(fr, sizeof fr, 61, &rq);
        ASSERT(opcd_dispatch(&st, fr, (size_t)fl, LOOP, cport, rs, sizeof rs, &rl) == 0 && rl == 0,
               "26e: write A deferred");
        st.radio_gen++;                       /* a newer request replaced st->radio meanwhile */
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0, "26e: A completed");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0 && recv(cli, rx, sizeof rx, 0) > 0, "26e: A ack arrived");
        ASSERT(!st.radio_committed, "26e: older-generation completion does not commit");

        /* Control arm: a write of the current generation commits. */
        legacy_to_scan(5200, 40, &rq.wlan1);
        fl = opc_set_radio_config_req_pack(fr, sizeof fr, 62, &rq);
        rl = -1;
        ASSERT(opcd_dispatch(&st, fr, (size_t)fl, LOOP, cport, rs, sizeof rs, &rl) == 0 && rl == 0,
               "26e: write B deferred");
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0, "26e: B completed");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0 && recv(cli, rx, sizeof rx, 0) > 0, "26e: B ack arrived");
        ASSERT(st.radio_committed, "26e: current-generation completion commits");

        /* Stale completion after a current-generation commit: the drain may
         * hand back an older-generation write AFTER the current one (job-slot
         * order ≠ completion order). It must be ignored, not clear the commit.
         * Simulate: queue write C, then pretend a newer write already committed
         * (gen bump + committed), then let C's completion drain. */
        legacy_to_scan(5220, 44, &rq.wlan1);
        fl = opc_set_radio_config_req_pack(fr, sizeof fr, 63, &rq);
        rl = -1;
        ASSERT(opcd_dispatch(&st, fr, (size_t)fl, LOOP, cport, rs, sizeof rs, &rl) == 0 && rl == 0,
               "26e: write C deferred");
        st.radio_gen++;
        st.radio_committed = true;            /* the (simulated) current write landed */
        ASSERT(wait_fd_readable(opc_store_async_event_fd(sa), 5000) == 0, "26e: C completed");
        opcd_store_async_on_ready(&st);
        ASSERT(wait_fd_readable(cli, 5000) == 0 && recv(cli, rx, sizeof rx, 0) > 0, "26e: C ack arrived");
        ASSERT(st.radio_committed, "26e: stale completion leaves the commit untouched");

        st.store_async = NULL;
        opc_store_async_destroy(sa);
        close(srv);
        close(cli);
    }

    /* 28(#103). Rev1.01 unset / invalid value conventions in GetDeviceInfo:
     *     not associated → FREQ/CH 0xFFFF (live is the default source),
     *     SNR/RSSI -128, Status 0x0000, AP MAC all-zero; never-configured
     *     Mode/BW → 0xFF; Single station → Priority CH 0xFFFF and the whole
     *     WLAN#2 block invalid (Mode/BW 0xFF, FREQ/CH 0xFFFF, Status 0xFFFF,
     *     SNR/RSSI -128, MACs all-zero, SCAN band 0xFFFF, list all-zero). */
    {
        opc_get_device_info_ack_t ack;
        static const uint8_t zero6[6] = {0};
        static const uint8_t zero8[OPC_SCAN_CHLIST_LEN] = {0};
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        stub_reset_link();
        /* a) never configured, not associated */
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (unconfigured, not assoc)");
        ASSERT(ack.wlan1.freq_mhz == 0xFFFF && ack.wlan1.channel == 0xFFFF, "28: not-assoc FREQ/CH = 0xFFFF");
        ASSERT(ack.wlan1.snr == -128 && ack.wlan1.rssi == -128, "28: not-assoc SNR/RSSI = -128");
        ASSERT(ack.wlan1.status == 0x0000, "28: not-assoc Status = 0x0000 (SCAN)");
        ASSERT(memcmp(ack.wlan1.connect_ap_mac, zero6, 6) == 0, "28: not-assoc AP MAC = all-zero");
        ASSERT(ack.wlan1.mode == 0xFF && ack.wlan1.bandwidth == 0xFF, "28: unconfigured Mode/BW = 0xFF");
        ASSERT(ack.priority_ch == 0xFFFF, "28: single station Priority CH = 0xFFFF");
        ASSERT(ack.wlan2.mode == 0xFF && ack.wlan2.bandwidth == 0xFF, "28: single WLAN#2 Mode/BW = 0xFF");
        ASSERT(ack.wlan2.freq_mhz == 0xFFFF && ack.wlan2.channel == 0xFFFF, "28: single WLAN#2 FREQ/CH = 0xFFFF");
        ASSERT(ack.wlan2.status == 0xFFFF, "28: single WLAN#2 Status = 0xFFFF");
        ASSERT(ack.wlan2.snr == -128 && ack.wlan2.rssi == -128, "28: single WLAN#2 SNR/RSSI = -128");
        ASSERT(memcmp(ack.wlan2.mac, zero6, 6) == 0 && memcmp(ack.wlan2.connect_ap_mac, zero6, 6) == 0,
               "28: single WLAN#2 MACs = all-zero");
        ASSERT(ack.wlan2.scan_band == OPC_SCAN_BAND_UNSET && memcmp(ack.wlan2.scan_chlist, zero8, sizeof zero8) == 0,
               "28: single WLAN#2 SCAN = unset band, empty list");
        /* b) configured (5 GHz ch36, 11AX/20MHz) but still not associated:
         *    Mode/BW come from the config, FREQ/CH stay 0xFFFF (live default) */
        (void)do_set_radio(&st, CIP, 5180, (uint16_t)((OPC_BAND_5GHZ << 8) | 36));
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (configured, not assoc)");
        ASSERT(ack.wlan1.mode == OPC_WLAN_MODE_11AX && ack.wlan1.bandwidth == OPC_BANDWIDTH_20,
               "28: configured Mode/BW reported");
        ASSERT(ack.wlan1.freq_mhz == 0xFFFF && ack.wlan1.channel == 0xFFFF,
               "28: configured but not assoc → FREQ/CH 0xFFFF (live default)");
        /* c) associated → live FREQ/CH and Status, signal from the link */
        stub_set_link(0, true, 5240, 48);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc)");
        ASSERT(ack.wlan1.freq_mhz == 5240 && ack.wlan1.channel == (uint16_t)((OPC_BAND_5GHZ << 8) | 48),
               "28: assoc FREQ/CH = live");
        ASSERT(ack.wlan1.status == 0x0001, "28: assoc Status = 0x0001");
        ASSERT(ack.wlan1.rssi == -55 && ack.wlan1.snr == 40, "28: assoc SNR/RSSI copied from the link, not the -128 marker");
        /* d) associated but the link carries no frequency (link.json without
         *    info.freq/channel): the live source must report unset, never 0/0
         *    or a bandless raw channel (Codex, PR #112). */
        stub_set_link(0, true, 0, 0);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc, no freq)");
        ASSERT(ack.wlan1.freq_mhz == 0xFFFF && ack.wlan1.channel == 0xFFFF,
               "28: assoc without live freq → FREQ/CH 0xFFFF");
        ASSERT(ack.wlan1.status == 0x0001, "28: assoc without live freq still Status 0x0001");
        /* e) associated with a frequency but no channel (link.json with
         *    info.freq only): FREQ is real, CH must be the unset marker — never
         *    the ambiguous bare 0x0000 that opc_chan_field() yields for ch 0
         *    (Claude, PR #112). */
        stub_set_link(0, true, 5220, 0);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc, freq only)");
        ASSERT(ack.wlan1.freq_mhz == 5220, "28: assoc freq-only → FREQ live");
        ASSERT(ack.wlan1.channel == 0xFFFF, "28: assoc freq-only → CH 0xFFFF, not 0x0000");
        /* f) channel present but the frequency maps to no OPC band: a bandless
         *    (band byte 0) CH is not a valid Rev1.01 value either → unset. */
        stub_set_link(0, true, 4000, 44);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc, bandless)");
        ASSERT(ack.wlan1.channel == 0xFFFF, "28: assoc bandless freq → CH 0xFFFF, not 0x002C");
        /* g) associated, RSSI read but channel_info noise absent: SNR keeps
         *    the -128 invalid marker while RSSI is real (Codex, PR #112). */
        stub_set_link(0, true, 5240, 48);
        stub_set_link_signal(0, true, -61, false, 0);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc, no noise)");
        ASSERT(ack.wlan1.rssi == -61, "28: assoc RSSI read → real value");
        ASSERT(ack.wlan1.snr == -128, "28: assoc without noise → SNR -128, not 0");
        /* h) associated but link.signal_avg absent/malformed: neither metric
         *    was measured → both markers stay, Status still associated. */
        stub_set_link(0, true, 5240, 48);
        stub_set_link_signal(0, false, 0, false, 0);
        ASSERT(do_get_devinfo_ack(&st, CIP, &ack) == 0, "28: devinfo (assoc, no signal)");
        ASSERT(ack.wlan1.snr == -128 && ack.wlan1.rssi == -128, "28: assoc without signal → SNR/RSSI -128, not 0/0");
        ASSERT(ack.wlan1.status == 0x0001, "28: assoc without signal still Status 0x0001");
        stub_reset_link();
    }

    /* 27(#90). ChangeIp 반대편 서브넷 겹침 가드 (OPC_ERR_IP_CHANGE_CLASH 0x0050).
     *     Stub other-iface(mlan0) 라이브 서브넷 = 192.0.2.0/24 (get_dev_ipv4).
     *     겹치는 슬롯의 change-ip는 스테이징 전 NG, 비겹침·상대-무IP는 통과. */
    {
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);

        /* a) /24 겹침 (192.0.2.50/24 vs mlan0 192.0.2.0/24) → NG 0x0050 */
        opc_ipcfg_entry_t g;
        memset(&g, 0, sizeof g);
        g.boundary_flag = OPC_LIST_BOUNDARY_START;
        g.list_number   = 1;
        g.ip_address    = 0xC0000232u;  /* 192.0.2.50 */
        g.subnet_mask   = 0xFFFFFF00u;
        strncpy(g.essid, "clash-a", sizeof g.essid - 1);
        (void)do_set_ip_entry(&st, CIP, &g);
        g.boundary_flag = OPC_LIST_BOUNDARY_END;
        (void)do_set_ip_entry(&st, CIP, &g);
        r = do_change_ip(&st, CIP, 1);
        ASSERT(r == OPC_RESULT_NG && g_last_chgip_err == OPC_ERR_IP_CHANGE_CLASH,
               "clash-guard: /24 overlap with other-iface subnet -> NG 0x0050");
        ASSERT(!st.ip_change_pending, "clash-guard: rejected change not staged");

        /* b) 인접 비겹침 (192.0.3.1/24) → OK */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        g.boundary_flag = OPC_LIST_BOUNDARY_START;
        g.ip_address    = 0xC0000301u;  /* 192.0.3.1 */
        (void)do_set_ip_entry(&st, CIP, &g);
        g.boundary_flag = OPC_LIST_BOUNDARY_END;
        (void)do_set_ip_entry(&st, CIP, &g);
        ASSERT(do_change_ip(&st, CIP, 1) == OPC_RESULT_OK,
               "clash-guard: adjacent /24 (no overlap) -> OK");

        /* c) 짧은 prefix 포함 겹침 (192.0.0.0/16 ⊃ 192.0.2.x) → NG */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        g.boundary_flag = OPC_LIST_BOUNDARY_START;
        g.ip_address    = 0xC0000101u;  /* 192.0.1.1/16 — 상대 /24를 포함 */
        g.subnet_mask   = 0xFFFF0000u;
        (void)do_set_ip_entry(&st, CIP, &g);
        g.boundary_flag = OPC_LIST_BOUNDARY_END;
        (void)do_set_ip_entry(&st, CIP, &g);
        r = do_change_ip(&st, CIP, 1);
        ASSERT(r == OPC_RESULT_NG && g_last_chgip_err == OPC_ERR_IP_CHANGE_CLASH,
               "clash-guard: /16 superset overlap -> NG 0x0050");

        /* d) 선택자=mlan0 → 반대편(eth0)은 stub에서 무IP(0/0) → 무제약 통과.
         *    (a)와 같은 값이라도 방향이 바뀌면 정당 — 가드가 '반대편' 기준임을 고정 */
        init_state(&st, OPC_PASSWORD_DEFAULT);
        st.conf.device_ip_iface = OPC_IP_IFACE_MLAN0;
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);
        g.boundary_flag = OPC_LIST_BOUNDARY_START;
        g.ip_address    = 0xC0000232u;  /* 192.0.2.50/24 — mlan0 자신 대역 */
        g.subnet_mask   = 0xFFFFFF00u;
        (void)do_set_ip_entry(&st, CIP, &g);
        g.boundary_flag = OPC_LIST_BOUNDARY_END;
        (void)do_set_ip_entry(&st, CIP, &g);
        ASSERT(do_change_ip(&st, CIP, 1) == OPC_RESULT_OK,
               "clash-guard: target=mlan0, other(eth0) has no IP -> no constraint");
    }

    /* 26. device-info IP triple source selector (device_ip_iface) + ChangeIp
     *     apply-target coupling. Stub get_dev_ipv4: eth0 → 0/0/0 (legacy),
     *     mlan0 → TEST-NET-1 192.0.2.100 / 255.255.255.0 / gw 0. The read
     *     source and the apply target MUST follow the same selector (#89
     *     review C1 — a split makes the VHL set→change→re-read loop diverge). */
    {
        uint32_t ip = 1, mask = 1, gw = 1;
        init_state(&st, OPC_PASSWORD_DEFAULT);
        (void)do_login(&st, CIP, OPC_PASSWORD_DEFAULT);

        /* default (eth0): legacy all-zero stub triple — behavior unchanged */
        ASSERT(do_get_devinfo_ip(&st, CIP, &ip, &mask, &gw) == 0 &&
               ip == 0 && mask == 0 && gw == 0,
               "ip-iface default eth0 -> stub eth zeros");

        /* mlan0: the wireless-side triple, gateway stays 0 (link.json null) */
        st.conf.device_ip_iface = OPC_IP_IFACE_MLAN0;
        ASSERT(do_get_devinfo_ip(&st, CIP, &ip, &mask, &gw) == 0 &&
               ip == 0xC0000264u && mask == 0xFFFFFF00u && gw == 0,
               "ip-iface mlan0 -> stub mlan triple (192.0.2.100/24, gw 0)");

        /* apply coupling: with the selector on mlan0, the deferred ChangeIp
         * must reach the platform with iface=1 (same plane as the read). */
        stub_apply_ip_reset();
        (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_START, 0xC0A80165);
        (void)do_set_ip_list(&st, CIP, 1, OPC_LIST_BOUNDARY_END, 0xC0A80165);
        ASSERT(do_change_ip(&st, CIP, 1) == OPC_RESULT_OK, "ip-iface mlan0: change-ip accepted");
        ASSERT(do_logout(&st, CIP) == OPC_RESULT_OK, "ip-iface mlan0: logout ok");
        opcd_apply_pending_ip_change(&st);
        ASSERT(stub_apply_ip_calls() == 1 && stub_apply_ip_last_iface() == 1,
               "ip-iface mlan0: apply_ip_change targets iface=1 (mlan0)");
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
