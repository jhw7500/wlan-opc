/* opcd_log — 구현. 설계 배경은 opcd_log.h 헤더 주석 참조. */
#define _DEFAULT_SOURCE   /* vsyslog (glibc) */
#include <stdarg.h>
#include <stdio.h>

#include "opcd_log.h"
#include "../protocol/ids.h"
#include "../protocol/proto.h"

static int g_log_ready;   /* opcd_log_init() 전에는 전 로그 no-op (테스트 무음) */

void opcd_log_init(void)
{
    /* LOG_PID 미사용 — logger.log 관례(sUTILS)와 동일하게 태그를 "opcd"로
     * 깨끗하게 유지한다(rsyslog myformat 이 %syslogtag%[%severity%] 렌더). */
    openlog("opcd", 0, LOG_LOCAL0);
    g_log_ready = 1;
}

void opcd_logf(int priority, const char *fmt, ...)
{
    if (!g_log_ready) return;
    va_list ap;
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
}

const char *opcd_ip4str(uint32_t ip, char buf[16])
{
    if (!buf) return "?";   /* 방어: 호출부 printf 에 안전한 리터럴 폴백 */
    snprintf(buf, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

const char *opcd_req_name(uint16_t req_id)
{
    switch (req_id) {
    case OPC_REQ_LOGIN:                 return "Login";
    case OPC_REQ_LOGOUT:                return "Logout";
    case OPC_REQ_GET_BASIC_INFO:        return "GetBasicInfo";
    case OPC_REQ_GET_DEVICE_INFO:       return "GetDeviceInfo";
    case OPC_REQ_SET_PASSWORD:          return "SetPassword";
    case OPC_REQ_SET_IP_CONFIG_LIST:    return "SetIpConfigList";
    case OPC_REQ_CHANGE_IP_ADDRESS:     return "ChangeIpAddress";
    case OPC_REQ_SET_RADIO_CONFIG:      return "SetRadioConfig";
    case OPC_REQ_SET_INDICATION_CONFIG: return "SetIndicationConfig";
    case OPC_REQ_RESET:                 return "Reset";
    default:                            return "Unknown";
    }
}

const char *opcd_ind_name(uint16_t ind_id)
{
    switch (ind_id) {
    case OPC_IND_INIT_COMPLETE:      return "InitComplete";
    case OPC_IND_WLAN_STATUS_CHANGE: return "WlanStatusChange";
    case OPC_IND_ROAMING:            return "Roaming";
    case OPC_IND_AP_DISCONNECT:      return "ApDisconnect";
    case OPC_IND_FAULT_DETECT:       return "FaultDetect";
    case OPC_IND_RESET_NOTICE:       return "ResetNotice";
    case OPC_IND_KEEP_ALIVE:         return "KeepAlive";
    default:                         return "Unknown";
    }
}

int opcd_ack_result_peek(const uint8_t *frame, size_t frame_len,
                         uint16_t *result, uint16_t *error_cause)
{
    /* Result(2B)@64 + ErrorCause(2B)@66 가 payload 선두인 ack 만 대상:
     *   - 단순 ack (frame 68B = 64B 헤더 + 4B)
     *   - GetDeviceInfo ack (frame 416B — result/error_cause 선두, NG 응답 포함)
     * GetBasicInfo ack(80B)는 result 필드가 없어(vendor_code 선두) 제외.
     * Length 필드까지 요구하진 않는다 — 호출자는 이미 pack 된 자국 프레임을
     * 넘기므로 길이+타입+id 검사로 충분하다. */
    if (!frame || frame_len < 68) return 0;
    if (frame[1] != OPC_CMD_ACK) return 0;
    uint16_t id = (uint16_t)((frame[2] << 8) | frame[3]);
    if (frame_len != 68 && id != OPC_REQ_GET_DEVICE_INFO) return 0;
    if (result)      *result      = (uint16_t)((frame[64] << 8) | frame[65]);
    if (error_cause) *error_cause = (uint16_t)((frame[66] << 8) | frame[67]);
    return 1;
}
