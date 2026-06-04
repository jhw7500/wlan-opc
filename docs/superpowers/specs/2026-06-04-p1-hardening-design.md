# P1 하드닝 설계 (SEC-002 / ARCH-002 / ARCH-003)

- **작성일:** 2026-06-04
- **근거:** `wlan-package/docs/review-report.md` P1 항목 + P0 심층 분석
- **브랜치:** `harden/p1-indication-session-pack`
- **PR:** 단일 PR (3개 항목, SEC-002와 ARCH-002가 session teardown에서 결합)

## 제약 (확정된 결정)

- **사양 충돌 회피**: 사양(Rev1.00 KO line 751 "Indication IP Address: 통지처 IP 주소 지정")이 **임의 통지처 IP를 명시적으로 허용**한다. 따라서 리뷰의 원래 권고인 "indication 수신자를 holder IP로 제한"은 **채택하지 않는다**(별도 모니터링 호스트 사용을 막아 사양 위반).
- **와이어 값 불변**: 무효 recipient 거부는 기존 코드 `OPC_ERR_INDICATION_SETTING_VIOLATION`(0x0010) 재사용.

## 설계

### A. 세션 teardown 단일화 (ARCH-002) + indication 세션-수명 연동 (SEC-002)

신규 공개 함수 `void opcd_session_logout(opcd_state_t *st)` (`handler.c`/`handler.h`):
1. `opcd_ind_init_complete(st, OPC_INIT_STATE_LOGGED_OUT)` **먼저** 발행 — indication이 아직 enabled인 동안 마지막 LOGGED_OUT 통지를 내보낸다.
2. `st->logged_in = false; st->boot_status = OPC_DEVICE_READY;`
3. `st->indication_enabled = false;` — **SEC-002**: 세션이 끝나면 주기 indication(리플렉터 출력)을 멈춘다.

치환 대상 3곳(현재 복붙·비동일):
- `opcd_dispatch` 내 idle 체크 (handler.c)
- `opcd.c` timer 분기 idle 체크 (holder 로그는 유지)
- `handle_logout`

호출자별 컨텍스트 로깅은 호출자가 담당(timer의 "idle auto-logout" 로그 유지). teardown 부수효과는 단일 함수가 소유.

### B. recipient 검증 (SEC-002) — `handle_set_indication_config`

`indication_enabled`로 설정하기 전, `recipient_ip`(host order)가 다음이면 NG(`OPC_ERR_INDICATION_SETTING_VIOLATION`):
- `0.0.0.0` (== 0)
- 멀티캐스트: `224.0.0.0/4` (host order `0xE0000000`–`0xEFFFFFFF`)
- 브로드캐스트: `255.255.255.255` (`0xFFFFFFFF`)

임의 **유니캐스트** 통지처는 사양대로 허용.

### C. pack 반환값 검사 (ARCH-003)

- `handler.c`: `static void emit_ack(ssize_t *rlen, ssize_t packed)` — `*rlen = (packed < 0) ? 0 : packed;`. 모든 `*rlen = opc_*_ack_pack(...)` (10곳)을 `emit_ack(rlen, opc_*_ack_pack(...))`로 치환. 음수(용량/인자 실패) → 응답 없음으로 명시 (opcd.c는 이미 `tx_len > 0` 가드).
- `vhlctl/vhlctl.c`: req pack 결과를 `(size_t)`로 캐스팅하기 전에 `< 0` 검사 후 중단 (STYLE-005 메모리 안전).

## 테스트 (TDD, `opcd/tests/test_handler.c` 확장)

- `logout이 indication을 멈춘다`: 로그인 + indication enable → logout → `indication_enabled == false`.
- `idle-logout이 indication을 멈춘다`: dispatch idle 경로 (idle=0 또는 deadline 과거) → 다음 요청에서 teardown.
- `set-indication 무효 recipient 거부`: 멀티캐스트/브로드캐스트/0.0.0.0 → NG, `indication_enabled == false`.
- `set-indication 유니캐스트 허용`: 정상 IP → OK, `indication_enabled == true`.
- `emit_ack 음수 처리`: 너무 작은 resp 버퍼로 dispatch → `*rlen == 0`.

## 비범위

- holder IP 제한(사양 이탈), period_seconds 하한(사양은 1 이상 허용), 강제 비밀번호 변경.
- SEC-004 GetBasicInfo login-state 비노출 — 별도.

## 검증

- `make check` 전체 PASS + 신규 테스트.
- 호스트 빌드(stub) + aarch64(nxp) cross-build clean.
- 실장치: set-indication 멀티캐스트 거부 / 유니캐스트 OK / logout 후 indication 중지 확인.
