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
 * opcd_ind_tick; with period 0 it is notified on arrival (spec branch pending a
 * 발주처 answer on period-0 semantics). Only idx 0 is staged under the
 * mlan0-only policy (opcd.c drops idx != 0). */
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
}

int opcd_ind_fault_detect(opcd_state_t *st, uint16_t cong_id, uint16_t val)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_FAULT_DETECT)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_fault_detect_t in = { .congestion_id = cong_id, .current_val = val };
    ssize_t n = opc_ind_fault_detect_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
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

void opcd_ind_tick(opcd_state_t *st)
{
    if (!st->indication_enabled)             return;
    if (st->indication_period_s == 0)        return;   /* spec: 0 disables the stream */
    /* The reporting period drives both KeepAlive and the congestion probe
     * (T6 interim: 판별은 보고 주기에 맞게) — advance the counter when
     * either is enabled. */
    if (!(st->indication_info_bits &
          (OPC_IND_BIT_KEEP_ALIVE | OPC_IND_BIT_FAULT_DETECT |
           OPC_IND_BIT_WLAN_STATUS_CHANGE | OPC_IND_BIT_ROAMING |
           OPC_IND_BIT_AP_DISCONNECT))) return;

    st->indication_tick_counter++;
    if (st->indication_tick_counter < (int32_t)st->indication_period_s) return;
    st->indication_tick_counter = 0;

    /* §4.3.9 (#105): flush this period's LAST staged state change of each kind
     * (per link idx). State changes lead, then FaultDetect, then KeepAlive. */
    for (size_t i = 0; i < sizeof st->indication_coalesce / sizeof st->indication_coalesce[0]; i++) {
        struct opcd_ind_coalesce *c = &st->indication_coalesce[i];
        if (c->wlan_pending) {
            (void)emit_wlan_status(st, c->wlan_status, c->wlan_ch);
            c->wlan_pending = false;
        }
        if (c->roam_pending) {
            (void)emit_roaming(st, c->roam_snr, c->roam_rssi, c->roam_mac, c->roam_ch);
            c->roam_pending = false;
        }
        if (c->apd_pending) {
            (void)emit_ap_disconnect(st, c->apd_msg_id, c->apd_reason, c->apd_mac);
            c->apd_pending = false;
        }
    }

    /* T6 interim congestion probe (decision 2026-06-12, inquiry #35): one
     * sample per reporting period, re-notified every period while the
     * congestion persists. Memory (0x0002) is deliberately not produced —
     * swapless target, see fault_probe.h. */
    if (st->indication_info_bits & OPC_IND_BIT_FAULT_DETECT) {
        opcd_fault_report_t rep;
        if (opcd_fault_probe_sample(&st->fault_probe, &rep) == 0) {
            if (rep.cpu_over)
                (void)opcd_ind_fault_detect(st, OPC_CONGESTION_CPU, rep.cpu_pct);
            if (rep.disk_over)
                (void)opcd_ind_fault_detect(st, OPC_CONGESTION_DISK_IO, rep.disk_pct);
            if (rep.net_over)
                (void)opcd_ind_fault_detect(st, OPC_CONGESTION_NETWORK, rep.net_pct);
        }
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
