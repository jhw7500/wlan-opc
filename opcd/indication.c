#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "indication.h"
#include "opcd_log.h"
#include "../protocol/indications.h"

static int send_indication_frame(opcd_state_t *st, const uint8_t *frame, ssize_t frame_len)
{
    if (!st || !frame || frame_len <= 0) return -1;
    if (!st->indication_enabled || st->udp_fd < 0) return 0;
    if (st->indication_recipient_ip == 0 || st->indication_recipient_port == 0) return 0;

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(st->indication_recipient_port);
    dst.sin_addr.s_addr = htonl(st->indication_recipient_ip);
    ssize_t n = sendto(st->udp_fd, frame, (size_t)frame_len, 0,
                       (struct sockaddr *)&dst, sizeof dst);
    /* 감사 로그 — id/seq 는 packed 헤더에서 직접 읽는다(BE, @2/@4). 헤더
     * prefix 가 온전한 프레임(>=8B)에서만 읽어 오버리드를 배제한다(packed
     * indication 은 항상 >=64B 지만 방어적으로 가드). KeepAlive 는 주기
     * 발행이라 DEBUG 로 강등(local0.info 셀렉터가 필터). */
    if (frame_len >= 8) {
        uint16_t id  = (uint16_t)((frame[2] << 8) | frame[3]);
        uint16_t seq = (uint16_t)((frame[4] << 8) | frame[5]);
        char ipb[16];
        if (n != frame_len)
            OLOG_ERR("IND %s(0x%04x) seq=%u to=%s:%u send failed (%zd/%zd)",
                     opcd_ind_name(id), id, seq,
                     opcd_ip4str(st->indication_recipient_ip, ipb),
                     st->indication_recipient_port, n, frame_len);
        else if (id == OPC_IND_KEEP_ALIVE)
            OLOG_DEBUG("IND KeepAlive seq=%u to=%s:%u", seq,
                       opcd_ip4str(st->indication_recipient_ip, ipb),
                       st->indication_recipient_port);
        else
            OLOG_INFO("IND %s(0x%04x) seq=%u to=%s:%u len=%zd",
                      opcd_ind_name(id), id, seq,
                      opcd_ip4str(st->indication_recipient_ip, ipb),
                      st->indication_recipient_port, frame_len);
    }
    return (n == frame_len) ? 0 : -1;
}

static uint16_t next_seq(opcd_state_t *st)
{
    return st->indication_seq++;
}

int opcd_ind_init_complete(opcd_state_t *st, uint32_t status)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_INIT_COMPLETE)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_init_complete_t in = { .status = status };
    ssize_t n = opc_ind_init_complete_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

/* Immediate senders (bit-gating done by the public entry points below). Split
 * out so both the period-0 path and the period-tick flush share one packer. */
static int emit_wlan_status(opcd_state_t *st, uint16_t status, uint16_t ch)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_wlan_status_change_t in = { .wlan_status = status, .indication_ch = ch };
    ssize_t n = opc_ind_wlan_status_change_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

static int emit_roaming(opcd_state_t *st, int8_t snr, int8_t rssi,
                        const uint8_t ap_mac[6], uint16_t ch)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_roaming_t in = { .current_snr = snr, .current_rssi = rssi, .ch_number = ch };
    memcpy(in.connect_ap_mac, ap_mac, 6);
    ssize_t n = opc_ind_roaming_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

static int emit_ap_disconnect(opcd_state_t *st, uint16_t msg_id, uint16_t reason,
                              const uint8_t ap_mac[6])
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_ap_disconnect_t in = { .message_id = msg_id, .result_code = reason };
    memcpy(in.disconnect_ap_mac, ap_mac, 6);
    ssize_t n = opc_ind_ap_disconnect_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

/* §4.3.9 period coalescing (#105): with a reporting period > 0 a state change
 * is STAGED (latest wins) and flushed once at the period boundary by
 * opcd_ind_tick; with period 0 it is notified on arrival, as the spec requires
 * (p.37, unchanged since Rev1.00). Only idx 0 is staged under the mlan0-only
 * policy (opcd.c drops idx != 0). */
int opcd_ind_wlan_status(opcd_state_t *st, uint16_t status, uint16_t ch)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_WLAN_STATUS_CHANGE)) return 0;
    if (st->indication_period_s > 0) {
        struct opcd_ind_coalesce *c = &st->indication_coalesce[0];
        c->wlan_status = status; c->wlan_ch = ch; c->wlan_pending = true;
        return 0;
    }
    return emit_wlan_status(st, status, ch);
}

int opcd_ind_roaming(opcd_state_t *st, int8_t snr, int8_t rssi,
                     const uint8_t ap_mac[6], uint16_t ch)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_ROAMING)) return 0;
    if (st->indication_period_s > 0) {
        struct opcd_ind_coalesce *c = &st->indication_coalesce[0];
        c->roam_snr = snr; c->roam_rssi = rssi; c->roam_ch = ch;
        memcpy(c->roam_mac, ap_mac, 6); c->roam_pending = true;
        return 0;
    }
    return emit_roaming(st, snr, rssi, ap_mac, ch);
}

int opcd_ind_ap_disconnect(opcd_state_t *st, uint16_t msg_id, uint16_t reason,
                           const uint8_t ap_mac[6])
{
    if (!(st->indication_info_bits & OPC_IND_BIT_AP_DISCONNECT)) return 0;
    if (st->indication_period_s > 0) {
        struct opcd_ind_coalesce *c = &st->indication_coalesce[0];
        c->apd_msg_id = msg_id; c->apd_reason = reason;
        memcpy(c->apd_mac, ap_mac, 6); c->apd_pending = true;
        return 0;
    }
    return emit_ap_disconnect(st, msg_id, reason, ap_mac);
}

void opcd_ind_coalesce_reset(opcd_state_t *st)
{
    for (size_t i = 0; i < sizeof st->indication_coalesce / sizeof st->indication_coalesce[0]; i++)
        st->indication_coalesce[i] = (struct opcd_ind_coalesce){0};
    /* A new recipient/config (or a new session) has no knowledge of an
     * ONGOING congestion — forget the entry latch so the next probe sample
     * reports it as a fresh entry (#121). */
    opcd_fault_probe_reset_latch(&st->fault_probe);
}

static int emit_fault_detect(opcd_state_t *st, uint16_t cong_id, uint16_t val)
{
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_fault_detect_t in = { .congestion_id = cong_id, .current_val = val };
    ssize_t n = opc_ind_fault_detect_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

/* Coalesce slot index for a congestion id (OPC_CONGESTION_* 1..4 → 0..3);
 * -1 for an id outside the spec table. */
static int fault_slot(uint16_t cong_id)
{
    return (cong_id >= OPC_CONGESTION_CPU && cong_id <= OPC_CONGESTION_NETWORK)
               ? (int)cong_id - 1 : -1;
}

/* §4.3.9 (#121): FaultDetect is a state-change notification like the WLAN
 * status kinds — period 0 notifies on arrival, period > 0 stages the LAST
 * value per resource for the period-end flush. Callers pass congestion
 * ENTRIES only (fault_probe entry latch); a persisting congestion is not
 * re-notified. */
int opcd_ind_fault_detect(opcd_state_t *st, uint16_t cong_id, uint16_t val)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_FAULT_DETECT)) return 0;
    if (st->indication_period_s > 0) {
        int slot = fault_slot(cong_id);
        if (slot < 0) return -1;   /* unknown id: never off-boundary under period>0 */
        struct opcd_ind_coalesce *c = &st->indication_coalesce[0];
        c->fault_val[slot]     = val;
        c->fault_pending[slot] = true;
        return 0;
    }
    return emit_fault_detect(st, cong_id, val);
}

/* Device-internal congestion watch (#121): runs on its own interval
 * regardless of the Indication Period (period 0 included — the stream is
 * off but state changes are still notified on arrival). Only ENTRIES are
 * notified; a clear only drops the latch (inquiry Q6 decides whether the
 * clear itself is a notifiable state change). Memory (0x0002) is
 * deliberately not produced — swapless target, see fault_probe.h. */
static void fault_probe_tick(opcd_state_t *st, uint32_t elapsed_s)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_FAULT_DETECT)) return;
    if (!opcd_fault_probe_due(&st->fault_probe, elapsed_s)) return;
    opcd_fault_report_t rep;
    if (opcd_fault_probe_sample(&st->fault_probe, &rep) != 0) return;
    if (rep.cpu_entered)
        (void)opcd_ind_fault_detect(st, OPC_CONGESTION_CPU, rep.cpu_pct);
    if (rep.disk_entered)
        (void)opcd_ind_fault_detect(st, OPC_CONGESTION_DISK_IO, rep.disk_pct);
    if (rep.net_entered)
        (void)opcd_ind_fault_detect(st, OPC_CONGESTION_NETWORK, rep.net_pct);
    /* rep.*_cleared: congestion-clear hook — intentionally no notification
     * until the vendor answers Q6 (notify as a state change, or not at all). */
}

int opcd_ind_reset_notice(opcd_state_t *st, uint32_t cause)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_RESET_NOTICE)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_reset_notice_t in = { .reset_cause = cause };
    ssize_t n = opc_ind_reset_notice_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

int opcd_ind_keep_alive(opcd_state_t *st, const char *timestamp)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_KEEP_ALIVE)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_keep_alive_t in;
    memset(&in, 0, sizeof in);
    if (timestamp) {
        strncpy(in.timestamp, timestamp, sizeof in.timestamp - 1);
    }
    ssize_t n = opc_ind_keep_alive_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

void opcd_ind_tick(opcd_state_t *st) { opcd_ind_tick_elapsed(st, 1); }

void opcd_ind_tick_elapsed(opcd_state_t *st, uint32_t elapsed_s)
{
    if (!st->indication_enabled)             return;
    /* The congestion watch has its own interval (#121) and must run before
     * the period-0 return below: with period 0 an entry is notified at once,
     * with period > 0 it is staged and flushed further down in this call. */
    fault_probe_tick(st, elapsed_s);
    if (st->indication_period_s == 0)        return;   /* spec: 0 disables the stream */
    /* The reporting period drives KeepAlive and the coalesce flush — advance
     * the counter when any of them is enabled. */
    if (!(st->indication_info_bits &
          (OPC_IND_BIT_KEEP_ALIVE | OPC_IND_BIT_FAULT_DETECT |
           OPC_IND_BIT_WLAN_STATUS_CHANGE | OPC_IND_BIT_ROAMING |
           OPC_IND_BIT_AP_DISCONNECT))) return;

    /* Advance by the elapsed seconds (timerfd expiration count from the main
     * loop). Clamp to one period so a long stall crosses the boundary exactly
     * once — flush once, not a burst of KeepAlives/FaultDetects. */
    if (elapsed_s > (uint32_t)st->indication_period_s)
        elapsed_s = (uint32_t)st->indication_period_s;
    st->indication_tick_counter += (int32_t)elapsed_s;
    if (st->indication_tick_counter < (int32_t)st->indication_period_s) return;
    st->indication_tick_counter = 0;

    /* §4.3.9 (#105): flush this period's LAST staged state change of each kind
     * (per link idx). State changes lead, then FaultDetect, then KeepAlive. */
    for (size_t i = 0; i < sizeof st->indication_coalesce / sizeof st->indication_coalesce[0]; i++) {
        struct opcd_ind_coalesce *c = &st->indication_coalesce[i];
        /* Clear pending only on a successful emit: a transient send failure
         * (e.g. ENETUNREACH) keeps the LAST state staged so the next period
         * retries it, instead of dropping the state change entirely (OpenCode,
         * PR #116). A newer change before the retry overwrites it — latest
         * still wins. */
        if (c->wlan_pending && emit_wlan_status(st, c->wlan_status, c->wlan_ch) == 0)
            c->wlan_pending = false;
        if (c->roam_pending &&
            emit_roaming(st, c->roam_snr, c->roam_rssi, c->roam_mac, c->roam_ch) == 0)
            c->roam_pending = false;
        if (c->apd_pending &&
            emit_ap_disconnect(st, c->apd_msg_id, c->apd_reason, c->apd_mac) == 0)
            c->apd_pending = false;
        /* FaultDetect congestion entries staged by fault_probe_tick (#121) */
        for (int k = 0; k < 4; k++)   /* idx = OPC_CONGESTION_* - 1 */
            if (c->fault_pending[k] &&
                emit_fault_detect(st, (uint16_t)(k + 1), c->fault_val[k]) == 0)
                c->fault_pending[k] = false;
    }

    /* Emit KeepAlive with ISO-8601 timestamp — skipped entirely when only
     * FaultDetect is enabled. */
    if (st->indication_info_bits & OPC_IND_BIT_KEEP_ALIVE) {
        time_t now = time(NULL);
        struct tm tm_buf;
        char ts[32] = {0};
        if (gmtime_r(&now, &tm_buf)) {
            strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        } else {
            snprintf(ts, sizeof ts, "%lld", (long long)now);
        }
        opcd_ind_keep_alive(st, ts);
    }
}
