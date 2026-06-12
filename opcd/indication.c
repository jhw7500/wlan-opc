#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "indication.h"
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

int opcd_ind_wlan_status(opcd_state_t *st, uint16_t status, uint16_t ch)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_WLAN_STATUS_CHANGE)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_wlan_status_change_t in = { .wlan_status = status, .indication_ch = ch };
    ssize_t n = opc_ind_wlan_status_change_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

int opcd_ind_roaming(opcd_state_t *st, int8_t snr, int8_t rssi,
                     const uint8_t ap_mac[6], uint16_t ch)
{
    if (!(st->indication_info_bits & OPC_IND_BIT_ROAMING)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_roaming_t in = { .current_snr = snr, .current_rssi = rssi, .ch_number = ch };
    memcpy(in.connect_ap_mac, ap_mac, 6);
    ssize_t n = opc_ind_roaming_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
}

int opcd_ind_ap_disconnect(opcd_state_t *st, uint16_t msg_id, uint16_t reason,
                           const uint8_t ap_mac[6])
{
    if (!(st->indication_info_bits & OPC_IND_BIT_AP_DISCONNECT)) return 0;
    uint8_t frame[OPC_FRAME_MAX];
    opc_ind_ap_disconnect_t in = { .message_id = msg_id, .result_code = reason };
    memcpy(in.disconnect_ap_mac, ap_mac, 6);
    ssize_t n = opc_ind_ap_disconnect_pack(frame, sizeof frame, next_seq(st), &in);
    return send_indication_frame(st, frame, n);
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
          (OPC_IND_BIT_KEEP_ALIVE | OPC_IND_BIT_FAULT_DETECT))) return;

    st->indication_tick_counter++;
    if (st->indication_tick_counter < (int32_t)st->indication_period_s) return;
    st->indication_tick_counter = 0;

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
                (void)opcd_ind_fault_detect(st, OPC_CONGESTION_NETWORK, rep.net_mbps);
        }
    }

    /* Emit KeepAlive with ISO-8601 timestamp — its own bit-gate sits inside
     * the emitter, so this is a no-op when only FaultDetect is enabled. */
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
