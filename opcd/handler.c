#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "handler.h"
#include "indication.h"
#include "inventory.h"
#include "platform.h"
#include "snapshot.h"
#include "store.h"
#include "store_async.h"

/* ---- session helpers ---- */

static time_t mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void session_touch(opcd_state_t *st)
{
    st->idle_deadline = mono_now() + st->conf.login_idle_s;
}

static bool session_owns(const opcd_state_t *st, uint32_t client_ip)
{
    return st->logged_in && st->holder_ip == client_ip;
}

/* ARCH-005: single authority for the wire-visible station type, shared by
 * GetBasicInfo and GetDeviceInfo. The fallback is unreachable today —
 * state_init seeds radio.station_type with SINGLE before any frame is
 * handled (D15) — and is kept defensively for a zeroed radio config. */
static uint16_t effective_station_type(const opcd_state_t *st)
{
    return st->radio.station_type ? st->radio.station_type
                                  : st->conf.default_station_type;
}

static int check_login_required(const opcd_state_t *st, uint32_t client_ip,
                                uint16_t *result, uint16_t *err)
{
    if (!st->logged_in) {
        *result = OPC_RESULT_NG;
        *err    = OPC_ERR_LOGIN_VIOLATION;
        return -1;
    }
    if (!session_owns(st, client_ip)) {
        *result = OPC_RESULT_NG;
        *err    = OPC_ERR_LOGIN_CONDITION;
        return -1;
    }
    return 0;
}

/* Single owner of session teardown — used by explicit logout, dispatch idle
 * check, and the main-loop timer idle check (previously three divergent
 * copies). Emits the final LOGGED_OUT transition while indication is still
 * enabled so the notification goes out, then clears the session and stops
 * indications: the periodic indication stream (a potential UDP reflector,
 * SEC-002) must not outlive the authenticated session. */
void opcd_session_logout(opcd_state_t *st)
{
    opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_OUT);
    st->logged_in          = false;
    st->boot_status        = OPC_DEVICE_READY;
    st->indication_enabled = false;
    /* An open SetIpConfigList cycle dies with its session — otherwise the
     * next login could flush (or trip A17 on) a previous session's stale
     * staging buffer. */
    st->ip_list_staging_active = false;
    memset(&st->ip_list_staging, 0, sizeof st->ip_list_staging);
}

/* Reject indication recipients that are not plain unicast. 0.0.0.0, the
 * 224.0.0.0/4 multicast block, and the 255.255.255.255 broadcast address
 * would let SetIndicationConfig aim the periodic stream at a group/broadcast
 * (amplifier). Arbitrary unicast is allowed per spec (Indication IP Address).
 * `ip_host` is host byte order. */
static bool valid_unicast_ipv4(uint32_t ip_host)
{
    if (ip_host == 0u)                          return false;  /* 0.0.0.0 */
    if (ip_host == 0xFFFFFFFFu)                 return false;  /* 255.255.255.255 */
    if ((ip_host & 0xF0000000u) == 0xE0000000u) return false;  /* 224.0.0.0/4 */
    return true;
}

/* §3.3.6: a netmask must be contiguous ones from the MSB (/32 allowed) —
 * valid iff ~m == 2^k - 1, which the (~m & (~m + 1)) == 0 test checks. */
static bool valid_netmask(uint32_t m)
{
    return m != 0 && ((~m & (~m + 1u)) == 0);
}

/* §3.3.6 "impossible IP" for ipcfg fields: must be unicast and not loopback —
 * a field device cannot live at (nor route via / sync to) 127.0.0.0/8. This
 * is stricter than valid_unicast_ipv4, which serves indication *recipients*
 * where loopback is at least addressable. */
static bool valid_ipcfg_addr(uint32_t ip_host)
{
    return valid_unicast_ipv4(ip_host) && (ip_host >> 24) != 127u;
}

/* §3.3.6 per-entry value validation (D1). Returns 0 or the NG error cause.
 * Check order follows the spec's cause listing; the network/broadcast
 * re-check returns 0x0011 again but sits after the netmask check because
 * it needs a valid mask. default_gateway / ntp_server 0 = "unset" and is
 * accepted: the spec text lists 0.0.0.0 as invalid, but the fields are
 * operationally optional — recorded as a vendor inquiry (#35). */
static uint16_t ipcfg_entry_error(const opc_ipcfg_entry_t *e, bool essid_terminated)
{
    if (!valid_ipcfg_addr(e->ip_address))     return OPC_ERR_IPCFG_IP;        /* 0x0011 */
    if (!valid_netmask(e->subnet_mask))       return OPC_ERR_IPCFG_NETMASK;   /* 0x0012 */
    if (e->subnet_mask != 0xFFFFFFFFu) {
        /* the subnet's network / broadcast address is not a host IP */
        uint32_t net   = e->ip_address & e->subnet_mask;
        uint32_t bcast = net | ~e->subnet_mask;
        if (e->ip_address == net || e->ip_address == bcast)
            return OPC_ERR_IPCFG_IP;                                          /* 0x0011 */
    }
    if (e->default_gateway != 0 &&
        (!valid_ipcfg_addr(e->default_gateway) ||
         e->default_gateway == e->ip_address ||
         /* same-subnet rule does not apply to /32 — a point-to-point host
          * routinely uses a gateway from another block */
         (e->subnet_mask != 0xFFFFFFFFu &&
          (e->default_gateway & e->subnet_mask) != (e->ip_address & e->subnet_mask))))
        return OPC_ERR_IPCFG_GW;                                              /* 0x0013 */
    if (e->ntp_server != 0 && !valid_ipcfg_addr(e->ntp_server))
        return OPC_ERR_IPCFG_NTP;                                             /* 0x0014 */
    if (!essid_terminated)                    return OPC_ERR_IPCFG_ESSID_NUL; /* 0x0016 */
    return 0;
}

/* Store an ack-pack result as the response length. A negative pack result
 * (capacity / argument failure) means "no response": store 0 rather than a
 * negative length, which the main loop would otherwise have to special-case.
 * Centralises the check the handlers previously skipped (ARCH-003). */
static void emit_ack(ssize_t *rlen, ssize_t packed)
{
    *rlen = (packed < 0) ? 0 : packed;
}

/* ---- file persistence helpers ----
 *
 * PERF-001: with an async store attached, the NVRAM write (and its fsync
 * stall) runs on the store_async worker thread and the command's ack is
 * deferred until the worker reports the result — the wire ack must carry
 * OPC_ERR_NVRAM on persist failure, so it cannot be sent earlier. Without
 * one (unit tests, or the daemon when writer creation failed at startup)
 * the write happens synchronously in-line, exactly as before. */

static_assert(sizeof(opcd_ip_list_t) <= OPC_STORE_ASYNC_DATA_MAX,
              "largest persisted blob must fit an async store job");
/* Intended invariant is equality (PENDING_ACK_MAX == QUEUE_DEPTH) so every
 * pending ack can always claim a job slot and persist_blob defers in one
 * pass. The assert only requires <=; if the two ever diverge, persist_blob
 * still terminates via its wait-and-retry loop, just not in a single pass. */
static_assert(OPCD_PENDING_ACK_MAX <= OPC_STORE_ASYNC_QUEUE_DEPTH,
              "every pending ack needs an async store job slot");

/* Job token for a queued write with no deferred ack attached — the wire ack
 * already went out carrying an earlier error. Completion is log-only. */
#define OPCD_STORE_TOKEN_NO_ACK UINT64_MAX

/* Upper bound on waiting for an in-flight NVRAM write when every slot is
 * busy. Only a client violating the spec response timer can fill the queue,
 * and a healthy fsync finishes orders of magnitude sooner; an unbounded
 * wait here would re-create the loop stall (and outlast SIGTERM, which is
 * blocked for signalfd and thus cannot interrupt poll).
 *
 * Side effect: when slot saturation does occur, the dispatch thread is
 * suspended inside wait_one_completion for up to this duration, pausing UDP
 * reception, indication ticks, and the idle-logout timer for that window.
 * This is a bounded residual of PERF-001's stall, reachable only by a
 * non-compliant client; the spec-compliant one-ack-at-a-time flow never
 * fills more than one slot. */
#define OPCD_PERSIST_WAIT_MS 2000

static int pending_ack_alloc(const opcd_state_t *st)
{
    for (int i = 0; i < OPCD_PENDING_ACK_MAX; i++)
        if (!st->pending_acks[i].in_use) return i;
    return -1;
}

/* Wait up to `ms` for one async-store completion and harvest it (sending
 * any deferred ack it carries). Returns 0 only when the eventfd was actually
 * readable (POLLIN). A timeout, or a wake from POLLERR/POLLHUP/POLLNVAL,
 * returns -1 — without the POLLIN guard the caller's retry loop would spin
 * at 100% CPU on a persistent error event. */
static int wait_one_completion(opcd_state_t *st, int ms)
{
    struct pollfd pfd = {
        .fd     = opc_store_async_event_fd(st->store_async),
        .events = POLLIN,
    };
    int r;
    do { r = poll(&pfd, 1, ms); } while (r < 0 && errno == EINTR);
    if (r != 1 || !(pfd.revents & POLLIN)) return -1;
    opcd_store_async_on_ready(st);
    return 0;
}

/* Persist `data` to `path`. Returns 0 with *deferred=true when the write was
 * queued (the ack is sent later by opcd_store_async_on_ready), 0/-1 with
 * *deferred=false for a synchronous write.
 *
 * In async mode the worker is the only writer of NVRAM paths. An in-line
 * write from this thread could interleave with an in-flight job for the
 * same path — both would use the same <path>.tmp.<pid> temp file (store.c)
 * and a stale queued snapshot could overtake the newer data — so a persist
 * that cannot be queued surfaces as OPC_ERR_NVRAM instead of falling back
 * to a synchronous write.
 *
 * Slot exhaustion means Set* commands arrived faster than NVRAM completes —
 * either a non-compliant client, or legitimate A19 retransmissions stacking
 * discarded response duties behind a slow write. Waiting is bounded by
 * OPCD_PERSIST_WAIT_MS either way. */
static int persist_blob(opcd_state_t *st, const char *path,
                        const void *data, size_t len, mode_t mode,
                        uint16_t req_id, uint32_t ip, uint16_t port,
                        uint16_t seq, bool *deferred)
{
    /* T7 (proto-todo): captured before any slot wait so the served-in log
     * on the deferred ack covers the full request→response interval. */
    struct timespec rx_ts;
    clock_gettime(CLOCK_MONOTONIC, &rx_ts);

    *deferred = false;
    if (!st->store_async)
        return opc_store_write_atomic(path, data, len, mode);

    for (;;) {
        int slot = pending_ack_alloc(st);
        if (slot >= 0) {
            if (opc_store_async_submit(st->store_async, path, data, len, mode,
                                       (uint64_t)slot) == 0) {
                st->pending_acks[slot] = (opcd_pending_ack_t){
                    .in_use      = true,
                    .req_id      = req_id,
                    .seq         = seq,
                    .client_ip   = ip,
                    .client_port = port,
                    .rx_ts       = rx_ts,
                };
                /* A19 (§3.1.3, 그림 3-2): a retransmission of the same
                 * command arrives carrying a NEW sequence number while the
                 * previous write is still in flight — the earlier response
                 * duty is discarded and only the newest SN is answered.
                 * Marked only now, after the replacement is definitely
                 * queued: discarding up front could drop the original ack
                 * with no replacement when the queue is saturated and this
                 * request then fails. The discarded slot stays allocated
                 * until its job drains, so a live job's token is never
                 * handed to a new request. If the old job completed while
                 * we waited for a slot, its ack already went out — that is
                 * the spec's "reply crossed the retransmission" two-ack
                 * case. */
                for (int i = 0; i < OPCD_PENDING_ACK_MAX; i++) {
                    opcd_pending_ack_t *pa = &st->pending_acks[i];
                    if (i != slot && pa->in_use && !pa->discarded &&
                        pa->req_id == req_id && pa->client_ip == ip &&
                        pa->client_port == port)
                        pa->discarded = true;
                }
                *deferred = true;
                return 0;
            }
            if (errno != EAGAIN) {
                /* EINVAL/E2BIG/ENAMETOOLONG are ruled out by the fixed paths
                 * and the static_asserts above; ECANCELED only during
                 * shutdown. Report as a persist failure either way. */
                fprintf(stderr, "opcd: async store submit failed: %s\n",
                        strerror(errno));
                return -1;
            }
            /* EAGAIN: a no-ack job holds the last queue slot — fall through
             * and wait for a completion to free it. */
        }
        if (wait_one_completion(st, OPCD_PERSIST_WAIT_MS) != 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

/* Persist with no deferred ack — the wire ack already carries an earlier
 * error, but a commit that reached memory still has to reach NVRAM. Queued
 * with the no-ack token so completion is log-only; written in-line only
 * when no worker exists. A drop (queue wedged) leaves memory ahead of disk
 * until the next successful persist — logged, same exposure as a failed
 * write. */
static void persist_no_ack(opcd_state_t *st, const char *path,
                           const void *data, size_t len, mode_t mode)
{
    if (!st->store_async) {
        if (opc_store_write_atomic(path, data, len, mode) != 0)
            fprintf(stderr, "opcd: NVRAM write failed (%s): %s\n",
                    path, strerror(errno));
        return;
    }
    for (;;) {
        if (opc_store_async_submit(st->store_async, path, data, len, mode,
                                   OPCD_STORE_TOKEN_NO_ACK) == 0)
            return;
        if (errno != EAGAIN ||
            wait_one_completion(st, OPCD_PERSIST_WAIT_MS) != 0) {
            fprintf(stderr, "opcd: NVRAM write dropped (%s): %s\n",
                    path, strerror(errno));
            return;
        }
    }
}

static int persist_password(opcd_state_t *st, uint32_t ip, uint16_t port,
                            uint16_t seq, bool *deferred)
{
    return persist_blob(st, st->paths.password, st->password,
                        strnlen(st->password, sizeof st->password), 0600,
                        OPC_REQ_SET_PASSWORD, ip, port, seq, deferred);
}

static int persist_radio(opcd_state_t *st, uint32_t ip, uint16_t port,
                         uint16_t seq, bool *deferred)
{
    return persist_blob(st, st->paths.radio, &st->radio, sizeof st->radio, 0644,
                        OPC_REQ_SET_RADIO_CONFIG, ip, port, seq, deferred);
}

static int persist_ip_list(opcd_state_t *st, uint32_t ip, uint16_t port,
                           uint16_t seq, bool *deferred)
{
    return persist_blob(st, st->paths.ip_list, &st->ip_list, sizeof st->ip_list, 0644,
                        OPC_REQ_SET_IP_CONFIG_LIST, ip, port, seq, deferred);
}

/* ---- handlers ---- */

static int handle_login(opcd_state_t *st, const uint8_t *frame, size_t flen,
                        uint32_t ip, uint16_t port,
                        uint8_t *resp, size_t rcap, ssize_t *rlen, uint16_t seq)
{
    opc_login_req_t req;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;

    if (opc_login_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (st->boot_status == OPC_DEVICE_BOOTING) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_VIOLATION;
    } else if (st->logged_in && st->holder_ip != ip) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_CONDITION;
    } else if (!req.password_terminated) {
        /* §3.3.1 0x0012: password field not NUL-terminated (D5). Deliberately
         * checked after the boot / exclusive-session state checks: the spec
         * lists 0x0001/0x0002 ahead of the field-format causes, and exclusive
         * control is Login's primary semantic. */
        result = OPC_RESULT_NG; err = OPC_ERR_PW_NUL;
    } else if (st->password[0] == '\0') {
        /* Empty stored password is "not provisioned" — it must never
         * authenticate. Without this, strncmp("", "") == 0 lets an empty
         * login in (the live-device hole). Fail closed. */
        result = OPC_RESULT_NG; err = OPC_ERR_PASSWORD_MISMATCH;
    } else if (strncmp(req.password, st->password, sizeof st->password - 1) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PASSWORD_MISMATCH;
    } else {
        bool was_active = st->logged_in;
        st->logged_in   = true;
        st->holder_ip   = ip;
        st->holder_port = port;
        st->boot_status = OPC_DEVICE_LOGGED_IN;
        /* Drop a prior session's ABANDONED ChangeIp (idle-logged-out, never
         * armed) so this fresh session's eventual Logout cannot commit it (#43
         * cross-session guard). Gated on two conditions:
         *  - !was_active: only a genuinely fresh login (no session was held)
         *    clears. A same-holder re-login / Login retransmission continues the
         *    SAME session and must keep its own still-pending change (#43).
         *  - !armed: an explicit Logout earlier in the same UDP drain armed a
         *    commit that must survive to the apply pass. */
        if (!was_active && !st->ip_change_commit_armed) {
            st->ip_change_pending      = false;
            st->ip_change_list_no      = 0;
            st->ip_change_commit_armed = false;  /* already false per the guard;
                                                  * kept so the invariant survives
                                                  * a future relaxation of it */
        }
        session_touch(st);
        opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_IN);
    }

    opc_login_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_login_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_logout(opcd_state_t *st, const uint8_t *frame, size_t flen,
                         uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                         ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    if (opc_logout_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (!st->logged_in) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_VIOLATION;
    } else if (!session_owns(st, ip)) {
        result = OPC_RESULT_NG; err = OPC_ERR_LOGIN_CONDITION;
    } else {
        /* The explicit Logout is the SOLE commit signal for a deferred ChangeIp
         * (#43): snapshot the resolved target entry and arm before teardown so
         * the main-loop apply — which runs after this ack is sent — performs the
         * switch against an immutable copy (a later session cannot rewrite the
         * slot out from under it). The `!armed` guard makes that snapshot
         * write-once: once a Logout has armed a commit, a later session's own
         * Logout (in the same UDP drain, before apply) must not re-snapshot it
         * with the rewritten slot. Idle auto-logout reaches opcd_session_logout()
         * without arming and therefore never commits. */
        if (st->ip_change_pending && !st->ip_change_commit_armed) {
            uint16_t n = st->ip_change_list_no;
            if (n >= 1 && n <= OPC_IPCFG_LIST_MAX_SLOTS) {
                st->ip_change_armed_entry  = st->ip_list.slots[n - 1];
                st->ip_change_armed_no     = n;
                st->ip_change_commit_armed = true;
            } else {
                /* Defensive dead path: handle_change_ip_address validates the
                 * slot before staging, so n is always in range here. Log if the
                 * invariant is ever violated so a future regression is visible
                 * rather than a silently dropped commit (Claude review). */
                fprintf(stderr,
                        "opcd: logout: pending IP change has invalid slot %u — not armed\n", n);
            }
        }
        opcd_session_logout(st);
    }
    opc_logout_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_logout_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_get_basic_info(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                 uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                 ssize_t *rlen, uint16_t seq)
{
    (void)ip; (void)port;
    if (opc_get_basic_info_req_unpack(frame, flen) != 0) {
        /* Spec says this must always answer — best effort with current state. */
    }
    const opcd_inventory_t *inv = opcd_inventory();
    opc_get_basic_info_ack_t ack = {
        .vendor_code     = inv->vendor_code,
        .product_code    = inv->product_code,
        .product_subcode = inv->product_subcode,
        .device_status   = st->boot_status,
        .station_type    = effective_station_type(st),
    };
    emit_ack(rlen, opc_get_basic_info_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_get_device_info(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                  uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                  ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_get_device_info_ack_t ack;
    memset(&ack, 0, sizeof ack);

    if (opc_get_device_info_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (st->indication_enabled) {
            result = OPC_RESULT_NG; err = OPC_ERR_INDICATION_SETTING_VIOLATION;
        } else {
            /* Static identity (vendor/product/hardware/serial/dates/caps)
             * comes from /usr/local/opc/etc/device_info.json via inventory.h.
             * Live-queried fields (firmware version, ntp server, MAC/IP, link
             * state) come from the platform vtable. ack was memset(0) above,
             * so any failed lookup leaves an empty/zero field rather than
             * failing the whole Ack — GetDeviceInfo on a partly-readable
             * device is more useful than NG. */
            const opcd_inventory_t   *inv  = opcd_inventory();
            const opcd_platform_ops_t *plat = opcd_platform();
            /* platform.h: opcd_platform() may return NULL before registration;
             * the dispatch path requires it to be non-NULL. Surface a missing
             * register call as abort() instead of NULL deref. Explicit check
             * (not assert) survives -DNDEBUG release builds. Logged before
             * abort() so a crash dump has triage context. */
            if (!plat) {
                fprintf(stderr, "opcd: BUG: opcd_platform() returned NULL in dispatch\n");
                abort();
            }
            ack.vendor_code     = inv->vendor_code;
            ack.product_code    = inv->product_code;
            ack.product_subcode = inv->product_subcode;
            ack.manufacture     = inv->manufacture_date;
            ack.shipment        = inv->shipment_date;
            memcpy(ack.hardware_version, inv->hardware_version,
                   sizeof ack.hardware_version);
            memcpy(ack.serial_number,    inv->serial_number,
                   sizeof ack.serial_number);
            (void)plat->get_firmware_version(ack.firmware_version, sizeof ack.firmware_version);
            (void)plat->get_ntp_server(&ack.ntp_server);
            (void)plat->get_eth_mac(ack.ethernet_mac);
            (void)plat->get_eth_ipv4_host(&ack.ip_address);
            (void)plat->get_eth_netmask_host(&ack.subnet_mask);
            (void)plat->get_eth_gateway_host(&ack.default_gateway);
            (void)plat->get_wlan_mac(0, ack.wlan1.mac);
            /* Ack carries a single essid field — DUAL always reports mlan0 */
            (void)plat->get_essid(0, ack.essid, sizeof ack.essid);
            /* Runtime link readback — overwrites the radio-state portion of
             * wlan{1,2}. mode/bandwidth are taken from the live link when
             * available; freq/channel still come from the set-radio cache
             * (set below). mode and bandwidth are tracked separately because
             * legacy associations (11a/b/g) report mode=0 (no HE-/VHT-/MCS
             * prefix in the bitrate), and bandwidth uses a valid flag because
             * BANDWIDTH_20 == 0 cannot be distinguished from a missing field. */
            opcd_platform_link_t link = {0};
            bool w1_mode_live = false, w1_bw_live = false;
            bool w2_mode_live = false, w2_bw_live = false;
            if (plat->get_link(0, &link) == 0) {
                memcpy(ack.wlan1.connect_ap_mac, link.bssid, 6);
                ack.wlan1.snr    = link.snr;
                ack.wlan1.rssi   = link.rssi;
                ack.wlan1.status = link.associated ? 0x0001 : 0x0000;
                if (link.associated && link.mode != 0) {
                    ack.wlan1.mode = link.mode;
                    w1_mode_live = true;
                }
                if (link.associated && link.bandwidth_valid) {
                    ack.wlan1.bandwidth = link.bandwidth;
                    w1_bw_live = true;
                }
            }
            if (st->radio.station_type == OPC_STATION_DUAL) {
                (void)plat->get_wlan_mac(1, ack.wlan2.mac);
                if (plat->get_link(1, &link) == 0) {
                    memcpy(ack.wlan2.connect_ap_mac, link.bssid, 6);
                    ack.wlan2.snr    = link.snr;
                    ack.wlan2.rssi   = link.rssi;
                    ack.wlan2.status = link.associated ? 0x0001 : 0x0000;
                    if (link.associated && link.mode != 0) {
                        ack.wlan2.mode = link.mode;
                        w2_mode_live = true;
                    }
                    if (link.associated && link.bandwidth_valid) {
                        ack.wlan2.bandwidth = link.bandwidth;
                        w2_bw_live = true;
                    }
                }
            }
            ack.ieee_11r  = inv->ieee_11r;
            ack.ieee_11ai = inv->ieee_11ai;
            ack.ieee_11k  = inv->ieee_11k;
            ack.ieee_11v  = inv->ieee_11v;
            session_touch(st);
            ack.device_status = st->boot_status;
            ack.station_type  = effective_station_type(st);
            ack.priority_ch   = st->radio.priority_ch;
            ack.wlan1.freq_mhz  = st->radio.wlan1.freq_mhz;
            ack.wlan1.channel   = st->radio.wlan1.channel;
            if (!w1_mode_live) ack.wlan1.mode      = st->radio.wlan1.mode;
            if (!w1_bw_live)   ack.wlan1.bandwidth = st->radio.wlan1.bandwidth;
            if (ack.station_type == OPC_STATION_DUAL) {
                ack.wlan2.freq_mhz  = st->radio.wlan2.freq_mhz;
                ack.wlan2.channel   = st->radio.wlan2.channel;
                if (!w2_mode_live) ack.wlan2.mode      = st->radio.wlan2.mode;
                if (!w2_bw_live)   ack.wlan2.bandwidth = st->radio.wlan2.bandwidth;
            }
        }
    }
    ack.result      = result;
    ack.error_cause = err;
    emit_ack(rlen, opc_get_device_info_ack_pack(resp, rcap, seq, &ack));
    /* Side-channel snapshot for external monitoring. Best-effort: a failed
     * write does not change the wire response we just packed. */
    (void)opcd_snapshot_publish(&ack, OPCD_SNAPSHOT_PATH);
    return 0;
}

static int handle_set_password(opcd_state_t *st, const uint8_t *frame, size_t flen,
                               uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                               ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_password_req_t req;

    if (opc_set_password_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        size_t newlen = strnlen(req.new_password, sizeof st->password - 1);
        if (!req.old_password_terminated) {
            /* §3.3.5 0x0012: old password not NUL-terminated (D4) */
            result = OPC_RESULT_NG; err = OPC_ERR_PW_NUL;
        } else if (!req.new_password_terminated) {
            /* §3.3.5 0x0014: new password not NUL-terminated (D4) */
            result = OPC_RESULT_NG; err = OPC_ERR_NEW_PW_NUL;
        } else if (strncmp(req.old_password, st->password, sizeof st->password - 1) != 0) {
            result = OPC_RESULT_NG; err = OPC_ERR_PASSWORD_MISMATCH;
        } else if (newlen == 0) {
            /* Refuse an empty new password — that is how the unauthenticated
             * empty-login state became reachable. Reject at the source. */
            result = OPC_RESULT_NG; err = OPC_ERR_PASSWORD_MISMATCH;
        } else {
            memset(st->password, 0, sizeof st->password);
            memcpy(st->password, req.new_password, newlen);
            bool deferred = false;
            if (persist_password(st, ip, port, seq, &deferred) != 0) {
                result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
            }
            session_touch(st);
            if (deferred) {
                *rlen = 0;   /* ack follows the NVRAM completion */
                return 0;
            }
        }
    }
    opc_set_password_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_set_password_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_set_ip_config_list(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                     uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                     ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_ip_config_list_req_t req;

    /* Frame-shape checks (0x0017 / 0x0003) deliberately precede the login
     * check: they are protocol-level properties of the datagram itself,
     * independent of any session, mirroring every handler's unpack-first
     * ordering. */
    int urc = opc_set_ip_config_list_req_unpack(frame, flen, &req);
    if (urc == OPC_UNPACK_ERR_LIST_SIZE) {
        /* §3.3.6 0x0017: body is not 64×n (n=1..20) — list-size violation (D1) */
        result = OPC_RESULT_NG; err = OPC_ERR_IPCFG_LIST_SIZE;
    } else if (urc != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        bool committed = false;
        for (size_t i = 0; i < req.entry_count; i++) {
            const opc_ipcfg_entry_t *e = &req.entries[i];
            if (e->list_number < 1 || e->list_number > OPC_IPCFG_LIST_MAX_SLOTS) {
                result = OPC_RESULT_NG; err = OPC_ERR_SLOT_RANGE;
                break;
            }
            uint16_t verr = ipcfg_entry_error(e, (req.essid_terminated_mask >> i) & 1u);
            if (verr != 0) {
                /* §3.3.6 value validation (D1) — entry rejected before any
                 * staging mutation. */
                result = OPC_RESULT_NG; err = verr;
                break;
            }
            uint16_t flag = e->boundary_flag;
            if (flag == OPC_LIST_BOUNDARY_START) {
                /* Fresh sequence: seed staging from the committed list so the
                 * END commit only updates the slots named in this cycle (spec
                 * §3.3.6: "指定された番号のリストを更新する" — merge, not
                 * wholesale replace). This also drops any stale staging from
                 * a prior incomplete cycle. */
                st->ip_list_staging = st->ip_list;
                st->ip_list_staging_active = true;
            } else if (!st->ip_list_staging_active) {
                /* A17: CONTINUE/END with no open cycle — NG with 0x0018 per
                 * the vendor answer (numeric value pending confirmation).
                 * The entry is not recorded and nothing reaches NVM. */
                result = OPC_RESULT_NG; err = OPC_ERR_LIST_SEQUENCE;
                break;
            }

            uint16_t slot = (uint16_t)(e->list_number - 1);
            st->ip_list_staging.slots[slot] = *e;
            st->ip_list_staging.present[slot] = 1;

            if (flag == OPC_LIST_BOUNDARY_END) {
                /* staging is guaranteed active here — a lone CONTINUE/END
                 * already NG'd out above (A17). Commit atomically (in memory). */
                st->ip_list = st->ip_list_staging;
                committed = true;
                st->ip_list_staging_active = false;
                memset(&st->ip_list_staging, 0, sizeof st->ip_list_staging);
            }
        }
        session_touch(st);
        if (committed) {
            /* One NVRAM write per request, after the last commit — the final
             * in-memory list is what reaches disk either way (this also trims
             * the per-commit write churn noted in PERF-004). Only an all-OK
             * request takes the deferred-ack path: when an entry already
             * failed, the ack must carry that error now, so the committed
             * portion is written in-line (malformed-frame corner). */
            if (result == OPC_RESULT_OK) {
                bool deferred = false;
                if (persist_ip_list(st, ip, port, seq, &deferred) != 0) {
                    result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
                } else if (deferred) {
                    *rlen = 0;   /* ack follows the NVRAM completion */
                    return 0;
                }
            } else {
                persist_no_ack(st, st->paths.ip_list, &st->ip_list,
                               sizeof st->ip_list, 0644);
            }
        }
    }
    opc_set_ip_config_list_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_set_ip_config_list_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_change_ip_address(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                    uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                    ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_change_ip_address_req_t req;

    if (opc_change_ip_address_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (st->ip_change_commit_armed) {
            /* A prior session's Logout armed a commit that the main loop will
             * apply imminently; refuse to stage a new change so a returned OK is
             * never silently dropped by that apply's clear (#43, Codex review).
             * Only reachable in a same-drain race — the armed commit is about to
             * switch the device IP regardless. This also keeps the armed window
             * exclusive: no live pending state exists for the apply to discard. */
            result = OPC_RESULT_NG; err = OPC_ERR_IP_CHANGE_CONFLICT;
        } else if (st->ip_list_staging_active) {
            result = OPC_RESULT_NG; err = OPC_ERR_IP_CHANGE_CONFLICT;
        } else if (req.list_number < 1 || req.list_number > OPC_IPCFG_LIST_MAX_SLOTS) {
            result = OPC_RESULT_NG; err = OPC_ERR_SLOT_RANGE;
        } else if (!st->ip_list.present[req.list_number - 1]) {
            result = OPC_RESULT_NG; err = OPC_ERR_SLOT_EMPTY;
        } else {
            /* Reachable only when no commit is armed (rejected above), so this
             * fresh stage never collides with an armed snapshot. It commits when
             * THIS session explicitly Logs out, which snapshots + arms it. */
            st->ip_change_pending = true;
            st->ip_change_list_no = req.list_number;
            session_touch(st);
        }
    }
    opc_change_ip_address_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_change_ip_address_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static bool valid_wlan_mode(uint8_t m)
{
    return m == OPC_WLAN_MODE_11A || m == OPC_WLAN_MODE_11B ||
           m == OPC_WLAN_MODE_11G || m == OPC_WLAN_MODE_11N ||
           m == OPC_WLAN_MODE_11AC || m == OPC_WLAN_MODE_11AX;
}

static bool valid_wlan_bw(uint8_t b)
{
    return b == OPC_BANDWIDTH_20 || b == OPC_BANDWIDTH_40 ||
           b == OPC_BANDWIDTH_80 || b == OPC_BANDWIDTH_160 ||
           b == OPC_BANDWIDTH_80_80 || b == OPC_BANDWIDTH_320;
}

/* §3.3.8 (D8): supported bands are 2.4/5 GHz — 6 GHz rejected (A21). 0 means
 * "not specified". Exact per-band channel lists need device confirmation (V2),
 * so only band-level validation is done here. */
static bool valid_radio_freq(uint16_t mhz)
{
    /* 2.4 GHz tops out at ch14 = 2484 MHz; 5 GHz from the Japanese 4.9 GHz
     * public-safety band up to the 5/6 GHz boundary.
     * TODO(#35): tighten to the device's confirmed channel set (V2) — e.g.
     * 2412 lower bound, 5850 UNII-3 upper bound — once the vendor answers. */
    return mhz == 0 || (mhz >= 2400 && mhz <= 2484) ||
           (mhz >= 4900 && mhz <= 5925);
}

static bool valid_radio_channel(uint16_t ch)
{
    uint8_t band = (uint8_t)(ch >> 8);   /* upper byte band, lower byte CH */
    /* Reject only a present-but-unsupported band (A21: 6 GHz refused; unknown
     * band ids likewise). band 0x00 — a bare CH number — is tolerated: the
     * exact CH-list / encoding enforcement is pending device confirmation
     * (V2/V12), and the legacy encoding is in operational use. A specified
     * band with CH 0 is meaningless and rejected. */
    if (band != 0 && (uint8_t)(ch & 0xFF) == 0) return false;
    return band == 0 || band == OPC_BAND_2_4GHZ || band == OPC_BAND_5GHZ;
}

static int handle_set_radio_config(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                   uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                   ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_radio_config_req_t req;

    if (opc_set_radio_config_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (req.station_type != OPC_STATION_SINGLE && req.station_type != OPC_STATION_DUAL) {
            result = OPC_RESULT_NG; err = OPC_ERR_STATION_TYPE;
        } else if (!valid_wlan_mode(req.wlan1.mode)) {
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_MODE;
        } else if (!valid_wlan_bw(req.wlan1.bandwidth)) {
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_BW;
        } else if (req.station_type == OPC_STATION_DUAL && !valid_wlan_mode(req.wlan2.mode)) {
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_MODE;
        } else if (req.station_type == OPC_STATION_DUAL && !valid_wlan_bw(req.wlan2.bandwidth)) {
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_BW;
        } else if (!valid_radio_freq(req.wlan1.freq_mhz) ||
                   (req.station_type == OPC_STATION_DUAL &&
                    !valid_radio_freq(req.wlan2.freq_mhz))) {
            /* §3.3.8 0x0011: unsupported frequency (D8) */
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_FREQ;
        } else if (!valid_radio_channel(req.wlan1.channel) ||
                   (req.station_type == OPC_STATION_DUAL &&
                    (!valid_radio_channel(req.wlan2.channel) ||
                     !valid_radio_channel(req.priority_ch)))) {
            /* §3.3.8 0x0012: unsupported CH/band — includes 6 GHz (A21).
             * priority_ch is Dual-only (A16) and checked only there. */
            result = OPC_RESULT_NG; err = OPC_ERR_RADIO_CH;
        } else {
            const opcd_platform_ops_t *plat = opcd_platform();
            if (!plat) {
                fprintf(stderr, "opcd: BUG: opcd_platform() returned NULL in handle_set_radio_config\n");
                abort();
            }
            if (plat->apply_radio_config(&req) != 0) {
                /* Platform refused/failed the kernel change. Best-effort revert
                 * to the last-good config so a partial apply (e.g. DUAL: mlan0
                 * applied, mlan1 failed) cannot leave the device's wpa_supplicant
                 * confs diverged from committed state — "apply fails ⇒ no net
                 * change" (user decision 2026-06-16). st->radio is still the
                 * committed config here (only the success branch below writes
                 * it), i.e. it IS the pre-change setting to restore. The revert
                 * is best-effort: its own failure cannot improve the response,
                 * which is NG regardless. Caveat: before the first *successful*
                 * Set, st->radio is the zero/default config (freq 0), which the
                 * platform treats as "no association — skip", so the revert is a
                 * no-op; acceptable, since a fresh device has no prior committed
                 * radio conf to diverge from.
                 * Error cause: dedicated apply-failure 0x0050, NOT 0x0011 — the
                 * frequency value was valid (it passed the D8 check above); this
                 * is a runtime fault, so the operator must not be told to change
                 * it (D9 re-decided 2026-06-16; 0x0050 pending 발주처 confirm). */
                if (plat->apply_radio_config(&st->radio) != 0) {
                    /* Revert itself failed — the device may be left in a partial
                     * apply state; surface it for triage. Response stays NG. */
                    fprintf(stderr, "opcd: set_radio: best-effort revert to "
                                    "last-good config ALSO failed — device may "
                                    "be in a partial apply state\n");
                }
                result = OPC_RESULT_NG; err = OPC_ERR_RADIO_APPLY;
            } else {
                st->radio = req;
                bool deferred = false;
                if (persist_radio(st, ip, port, seq, &deferred) != 0) {
                    result = OPC_RESULT_NG; err = OPC_ERR_NVRAM;
                } else if (deferred) {
                    session_touch(st);
                    *rlen = 0;   /* ack follows the NVRAM completion */
                    return 0;
                }
            }
            session_touch(st);
        }
    }
    opc_set_radio_config_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_set_radio_config_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_set_indication_config(opcd_state_t *st, const uint8_t *frame, size_t flen,
                                        uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                                        ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    opc_set_indication_config_req_t req;

    if (opc_set_indication_config_req_unpack(frame, flen, &req) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        if (req.info_bits & OPC_IND_BITS_RESERVED) {
            /* §3.3.9 0x0010: unassigned notification bit set (D1 family) */
            result = OPC_RESULT_NG; err = OPC_ERR_IND_BITS;
        } else if (req.info_bits != 0 && !valid_unicast_ipv4(req.recipient_ip)) {
            /* SEC-002: only a unicast recipient may receive indications.
             * Reject 0.0.0.0 / multicast / broadcast so the daemon cannot be
             * pointed at a group/broadcast as an amplifier. State unchanged.
             * Spec error cause: "IP 주소 이상" 0x0012 (D10). */
            result = OPC_RESULT_NG; err = OPC_ERR_IND_RECIPIENT_IP;
        } else {
            st->indication_enabled         = (req.info_bits != 0);
            st->indication_recipient_ip    = req.recipient_ip;
            st->indication_recipient_port  = req.recipient_port;
            st->indication_info_bits       = req.info_bits;
            st->indication_period_s        = req.period_seconds;
            st->indication_tick_counter    = 0;
            session_touch(st);
            if (st->indication_enabled) {
                /* Spec: on enable, emit InitComplete state sequence. */
                opcd_ind_init_complete(st, OPC_INIT_STATE_READY);
                opcd_ind_init_complete(st, OPC_INIT_STATE_RADIO_UP);
                if (st->logged_in) opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_IN);
            }
        }
    } else {
        /* A14 (spec §3.3.9): "issued from a non-login IP" carries its own
         * code 0x0013 for this command — override the common 0x0002 mapping
         * check_login_required just stored. The not-logged-in case stays
         * 0x0001. */
        if (err == OPC_ERR_LOGIN_CONDITION) err = OPC_ERR_IND_OTHER_IP;
    }
    opc_set_indication_config_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_set_indication_config_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

static int handle_reset(opcd_state_t *st, const uint8_t *frame, size_t flen,
                        uint32_t ip, uint16_t port, uint8_t *resp, size_t rcap,
                        ssize_t *rlen, uint16_t seq)
{
    (void)port;
    uint16_t result = OPC_RESULT_OK, err = OPC_ERR_NONE;
    if (opc_reset_req_unpack(frame, flen) != 0) {
        result = OPC_RESULT_NG; err = OPC_ERR_PACKET_SIZE;
    } else if (check_login_required(st, ip, &result, &err) == 0) {
        opcd_ind_reset_notice(st, OPC_RESET_CAUSE_USER);
        st->should_reset = true;     /* main loop will exit(0) after sending ack */
    }
    opc_reset_ack_t ack = { .result = result, .error_cause = err };
    emit_ack(rlen, opc_reset_ack_pack(resp, rcap, seq, &ack));
    return 0;
}

/* ---- dispatch ---- */

/* Static command table (ARCH-004). Every handle_* shares one signature, so a
 * new command is a single table row plus its handler — there is no parallel
 * switch case to keep in sync. Lookup is a linear scan over a fixed, small
 * table; the RX hot path is the zero-copy recv loop in opcd.c, not this lookup,
 * so the table does not regress dispatch performance (PERF-005). */
typedef int (*opcd_handler_fn)(opcd_state_t *st,
                               const uint8_t *frame, size_t frame_len,
                               uint32_t client_ip, uint16_t client_port,
                               uint8_t *resp, size_t resp_cap, ssize_t *resp_len,
                               uint16_t seq);

static const struct {
    uint16_t        req_id;
    opcd_handler_fn fn;
} opcd_dispatch_table[] = {
    { OPC_REQ_LOGIN,                 handle_login },
    { OPC_REQ_LOGOUT,                handle_logout },
    { OPC_REQ_GET_BASIC_INFO,        handle_get_basic_info },
    { OPC_REQ_GET_DEVICE_INFO,       handle_get_device_info },
    { OPC_REQ_SET_PASSWORD,          handle_set_password },
    { OPC_REQ_SET_IP_CONFIG_LIST,    handle_set_ip_config_list },
    { OPC_REQ_CHANGE_IP_ADDRESS,     handle_change_ip_address },
    { OPC_REQ_SET_RADIO_CONFIG,      handle_set_radio_config },
    { OPC_REQ_SET_INDICATION_CONFIG, handle_set_indication_config },
    { OPC_REQ_RESET,                 handle_reset },
};

int opcd_dispatch(opcd_state_t *st,
                  const uint8_t *frame, size_t frame_len,
                  uint32_t client_ip, uint16_t client_port,
                  uint8_t *resp, size_t resp_cap, ssize_t *resp_len)
{
    opc_header_t hdr;
    if (opc_frame_parse(frame, frame_len, &hdr, NULL, NULL) != 0) {
        *resp_len = 0;
        return -1;
    }
    if (hdr.command_type != OPC_CMD_REQUEST) {
        *resp_len = 0;
        return -1;
    }
    uint16_t seq = hdr.sequence_number;

    /* Idle auto-logout — check before serving any Login-requiring command. */
    if (st->logged_in && mono_now() >= st->idle_deadline) {
        opcd_session_logout(st);
    }

    for (size_t i = 0; i < sizeof opcd_dispatch_table / sizeof opcd_dispatch_table[0]; i++) {
        if (opcd_dispatch_table[i].req_id == hdr.req_indication_id &&
            opcd_dispatch_table[i].fn != NULL) {
            return opcd_dispatch_table[i].fn(st, frame, frame_len, client_ip,
                                             client_port, resp, resp_cap, resp_len, seq);
        }
    }
    *resp_len = 0;   /* unknown request id, or a table row with no handler */
    return -1;
}

void opcd_apply_pending_ip_change(opcd_state_t *st)
{
    /* Commit only when an explicit Logout armed it (#43). The main loop calls
     * this every iteration; gating on the arm flag (not merely ip_change_pending)
     * keeps a staged-but-not-logged-out change deferred, and makes idle/abandon
     * teardown a no-op so the device never migrates its IP unattended. The target
     * is a snapshot taken at arm time, so the commit depends on no mutable shared
     * state — a later session rewriting the slot cannot change what is applied. */
    if (!st->ip_change_commit_armed) return;
    const opc_ipcfg_entry_t *e = &st->ip_change_armed_entry;
    fprintf(stderr, "opcd: apply pending IP change → slot %u ip=0x%08X essid=%s\n",
            st->ip_change_armed_no, e->ip_address, e->essid);

    /* Hand off to the platform backend to rewrite the active IP (stub no-ops;
     * nxp reconfigures eth0's management IP directly via ip addr). Clear the
     * indication target only on success — a failed apply means the IP did not
     * move, so the existing indication session stays valid. Per platform.h the
     * vtable is fully populated after init(), so only the registration guard
     * (opcd_platform() can be NULL before register) is needed. */
    const opcd_platform_ops_t *plat = opcd_platform();
    int rc = plat ? plat->apply_ip_change(e) : -1;
    if (rc != 0)
        fprintf(stderr, "opcd: apply pending IP change: platform apply_ip_change failed\n");
    else
        st->indication_enabled = false;   /* IP changed → indication target invalid */

    /* Fully reset the deferred-commit state. The arm gate already prevents a
     * stale snapshot from being reused, but zeroing the snapshot too means a
     * future relaxation of that gate cannot resurrect this entry (Claude review).
     * ip_change_list_no is staging state the apply no longer reads — cleared
     * here as belt-and-suspenders. */
    st->ip_change_pending      = false;
    st->ip_change_commit_armed = false;
    st->ip_change_list_no      = 0;
    st->ip_change_armed_no     = 0;
    memset(&st->ip_change_armed_entry, 0, sizeof st->ip_change_armed_entry);
}

/* D12/D13 (decision 2026-06-11): a bad-length datagram earns the spec's
 * 0x0003 NG only when it originates from the logged-in session's IP — any
 * other source is dropped silently, preserving the SEC-003 no-reflection
 * stance. The first OPC_FIXED_HEADER_SIZE bytes of `frame` must hold the
 * (possibly truncated) datagram's start; req_id and seq are echoed from
 * them so the client can correlate the NG. */
void opcd_reject_bad_length(opcd_state_t *st, const uint8_t *frame,
                            size_t valid_len, uint32_t cip, uint16_t cport)
{
    if (!st || !st->logged_in || st->holder_ip != cip) return;   /* drop */
    opc_header_t hdr;
    /* fixed-header unpack (8 B): a bad-length frame is by definition shorter
     * than the 64 B common header, which opc_header_unpack would insist on. */
    if (!frame || valid_len < OPC_FIXED_HEADER_SIZE ||
        opc_fixed_header_unpack(frame, valid_len, &hdr) != 0) return;
    uint8_t resp[OPC_FRAME_MAX];
    ssize_t n = opc_simple_ack_pack(resp, sizeof resp, hdr.req_indication_id,
                                    hdr.sequence_number, OPC_SIMPLE_ACK_LENGTH,
                                    OPC_RESULT_NG, OPC_ERR_PACKET_SIZE);
    if (n <= 0 || st->udp_fd < 0) return;
    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(cport);
    dst.sin_addr.s_addr = htonl(cip);
    (void)sendto(st->udp_fd, resp, (size_t)n, 0,
                 (struct sockaddr *)&dst, sizeof dst);
}

void opcd_store_async_on_ready(opcd_state_t *st)
{
    if (!st || !st->store_async) return;

    opc_store_async_done_t done[OPC_STORE_ASYNC_QUEUE_DEPTH];
    size_t n = opc_store_async_drain(st->store_async, done,
                                     OPC_STORE_ASYNC_QUEUE_DEPTH);
    for (size_t i = 0; i < n; i++) {
        /* No-ack check first, on purpose: OPCD_STORE_TOKEN_NO_ACK is UINT64_MAX
         * and would also trip the >= OPCD_PENDING_ACK_MAX guard below, so this
         * branch must run first to log no-ack failures rather than drop them
         * silently. Do not reorder. */
        if (done[i].token == OPCD_STORE_TOKEN_NO_ACK) {
            if (done[i].result != 0)
                fprintf(stderr, "opcd: NVRAM write failed (no-ack commit): %s\n",
                        strerror(done[i].saved_errno));
            continue;
        }
        if (done[i].token >= OPCD_PENDING_ACK_MAX) continue;
        opcd_pending_ack_t *pa = &st->pending_acks[done[i].token];
        if (!pa->in_use) continue;
        if (pa->discarded) {
            /* A19: superseded by a retransmission — drop the response duty.
             * The slot is freed only now that its job has drained. */
            if (done[i].result != 0)
                fprintf(stderr,
                        "opcd: NVRAM write failed (req 0x%04X, superseded ack): %s\n",
                        pa->req_id, strerror(done[i].saved_errno));
            *pa = (opcd_pending_ack_t){0};
            continue;
        }

        uint16_t result = (done[i].result == 0) ? OPC_RESULT_OK : OPC_RESULT_NG;
        uint16_t err    = (done[i].result == 0) ? OPC_ERR_NONE  : OPC_ERR_NVRAM;
        if (done[i].result != 0)
            fprintf(stderr, "opcd: NVRAM write failed (req 0x%04X): %s\n",
                    pa->req_id, strerror(done[i].saved_errno));

        uint8_t resp[OPC_FRAME_MAX];
        ssize_t rlen = 0;
        switch (pa->req_id) {
        case OPC_REQ_SET_PASSWORD: {
            opc_set_password_ack_t ack = { .result = result, .error_cause = err };
            emit_ack(&rlen, opc_set_password_ack_pack(resp, sizeof resp, pa->seq, &ack));
            break;
        }
        case OPC_REQ_SET_IP_CONFIG_LIST: {
            opc_set_ip_config_list_ack_t ack = { .result = result, .error_cause = err };
            emit_ack(&rlen, opc_set_ip_config_list_ack_pack(resp, sizeof resp, pa->seq, &ack));
            break;
        }
        case OPC_REQ_SET_RADIO_CONFIG: {
            opc_set_radio_config_ack_t ack = { .result = result, .error_cause = err };
            emit_ack(&rlen, opc_set_radio_config_ack_pack(resp, sizeof resp, pa->seq, &ack));
            break;
        }
        default:
            break;
        }

        if (rlen > 0 && st->udp_fd >= 0) {
            struct sockaddr_in dst = {0};
            dst.sin_family      = AF_INET;
            dst.sin_port        = htons(pa->client_port);
            dst.sin_addr.s_addr = htonl(pa->client_ip);
            ssize_t w = sendto(st->udp_fd, resp, (size_t)rlen, 0,
                               (struct sockaddr *)&dst, sizeof dst);
            if (w != rlen) {
                fprintf(stderr, "opcd: deferred ack send failed (req 0x%04X): %s\n",
                        pa->req_id, strerror(errno));
            } else {
                /* T7 (proto-todo): actual service time vs the spec budget
                 * (2 min for NVRAM-persisting commands). */
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long long us = (now.tv_sec - pa->rx_ts.tv_sec) * 1000000LL +
                               (now.tv_nsec - pa->rx_ts.tv_nsec) / 1000;
                fprintf(stderr,
                        "opcd: req 0x%04X seq=%u served in %lld.%03lld ms (deferred)\n",
                        pa->req_id, pa->seq, us / 1000, us % 1000);
            }
        }
        pa->in_use = false;
    }
}
