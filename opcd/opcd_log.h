/* opcd_log — 프로토콜 감사 로그 (syslog local0 → /var/log/cantops/logger.log).
 *
 * 보드의 다른 컴포넌트(sUTILS Logger, shell logger(1))와 같은 채널/포맷 관례를
 * 따른다: rsyslog가 local0.info 를 logger.log 로 라우팅하고, 한 줄 골격은
 *   <timestamp> opcd[<sev>] [<file>:<line>] <message>
 * 이다. 기록 대상 = RX 요청 / TX 응답(OK·NG+ErrorCause, 지연 ack 포함) /
 * 실행(IP apply·reset·roam-notify) / 에러(malformed·drop·송신실패) /
 * indication 발행. KeepAlive indication 은 DEBUG 로 내려 local0.info 셀렉터가
 * 자연 필터한다(logger.log 스팸 방지).
 *
 * stderr(journald) 로그와는 별개 채널: 여기는 "무슨 커맨드가 오가고 무엇이
 * 실행됐나"의 운영 감사용이고, 기존 stderr 는 개발/디버그 상세용으로 유지한다.
 *
 * opcd_logf()는 opcd_log_init() 호출 전에는 no-op — 단위테스트(test_handler 등)가
 * handler.o 를 링크해도 호스트 syslog 를 오염시키지 않는다.
 */
#ifndef OPCD_LOG_H
#define OPCD_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

/* openlog("opcd", 0, LOG_LOCAL0). 데몬 main()에서 1회 호출. */
void opcd_log_init(void);

/* vsyslog 래퍼 — init 전에는 no-op. */
void opcd_logf(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 호스트 바이트오더 IPv4 → dotted-quad. buf 는 최소 16B, 반환값 = buf. */
const char *opcd_ip4str(uint32_t ip_host_order, char buf[16]);

/* Request/Indication id → 사람이 읽는 이름. 미지 id 는 "Unknown". */
const char *opcd_req_name(uint16_t req_id);
const char *opcd_ind_name(uint16_t ind_id);

/* 단순 Ack(frame 68B = 64B 헤더 + Result u16 + ErrorCause u16)에서 결과를
 * 추출한다. 성공 시 1 을 반환하고 result·error_cause 출력 인자를 채운다.
 * basic-info/device-info 처럼 Result 필드가 없는 데이터 ack 는 0. */
int opcd_ack_result_peek(const uint8_t *frame, size_t frame_len,
                         uint16_t *result, uint16_t *error_cause);

#define OLOG_INFO(fmt, ...)  opcd_logf(LOG_INFO,    "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define OLOG_WARN(fmt, ...)  opcd_logf(LOG_WARNING, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define OLOG_ERR(fmt, ...)   opcd_logf(LOG_ERR,     "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define OLOG_DEBUG(fmt, ...) opcd_logf(LOG_DEBUG,   "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#endif /* OPCD_LOG_H */
