# 종합 코드 리뷰 리포트

- **대상:** 전체 코드베이스 — `protocol/{frame,codec,commands,indications}.c+.h`, `opcd/{opcd,handler,indication,store,opcd_state}.c+.h`, `vhlctl/vhlctl.c` (테스트 제외)
- **생성일:** 2026-06-02
- **감사 영역:** 아키텍처 · 보안 · 성능 · 스타일

## 실행 요약

원시 발견 24건을 병합 후 **20건의 고유 이슈**로 정규화 (4쌍이 동일 file:line/근본 원인으로 병합).

| 영역 | 발견 | critical | high | medium | low/info |
|------|-----:|---------:|-----:|-------:|---------:|
| 보안 | 5 | 0 | 1 | 1 | 3 |
| 아키텍처 | 6 | 0 | 2 | 3 | 1 |
| 성능 | 6 | 0 | 1 | 0 | 5 |
| 스타일 | 7 | 0 | 0 | 1 | 6 |
| **원시 합계** | **24** | **0** | **4** | **5** | **15** |
| **병합 후 고유** | **20** | **0** | **4** | **3** | **13** |

병합으로 정규화된 심각도 분포(고유 20건 기준): **critical 0 · high 4 · medium 3 · low 5 · info 8**.

**즉시 조치 Top 3:**
1. **[SEC-001]** UDP 소스 IP만으로 세션 인증 — 소스 IP 스푸핑 한 방으로 reset/set-password/set-radio 전권 탈취 (`opcd/handler.c:26`)
2. **[PERF-001]** 동기 NVRAM 쓰기(open+write+fsync+rename)가 단일 스레드 epoll 루프 안에서 실행 — Set* 명령 1건이 스펙 예산 120 s 동안 전 제어평면 정지 (`opcd/store.c:44`)
3. **[ARCH-001 + STYLE-001]** 명령별 ErrorCause가 명명되지 않은 hex 리터럴로 6곳 산재, `0x0010`이 세 가지 의미로 충돌 — 와이어 계약이 깨짐, VHL이 password-mismatch/slot-range/indication-violation을 구분 불가 (`opcd/handler.c:81`)

*감사 누락/실패 영역 없음 — 4개 영역 모두 정상 수신.*

## 발견 상세 (심각도 순)

### 🔴 Critical

해당 없음.

### 🟠 High

#### [SEC-001] 스푸핑 가능한 UDP 소스 IP에만 세션을 묶음; 시퀀스/논스 없음 — 소스 IP 스푸핑으로 전권 획득
- **영역:** 보안 (cross_ref: 아키텍처)
- **위치:** `opcd/handler.c:26` (`session_owns` / `check_login_required` / `opcd_dispatch`)
- **확신도:** high
- **문제:** Login 후 세션은 오직 `st->holder_ip == client_ip`로 식별된다(handler.c:26-29). `client_ip`는 비연결 UDP 소켓의 `recvfrom` 소스 주소(opcd.c:221)에서 온 값이고, UDP 소스 주소는 손쉽게 스푸핑된다. 요청별 시퀀스/논스 검증이 없다(`hdr.sequence_number`는 에코만, 검증 안 함 — handler.c:371). 공격: 정상 VHL이 로그인한 후, 공격자가 그 소스 IP로 위조한 단일 datagram에 `OPC_REQ_RESET`/`SET_PASSWORD`/`SET_RADIO_CONFIG`/`CHANGE_IP_ADDRESS`를 실어 보내면 `check_login_required`가 통과한다. 비밀번호 불필요, ack는 실제 holder에게 가므로 공격자는 받을 필요도 없다.
- **영향:** 인증/인가 우회. 소스 IP 스푸핑 가능한 원격 공격자가 디바이스 reset(DoS), 비밀번호 변경(락아웃/지속 침해), 무선/IP 재구성 전권 획득. 세션 소유권이 포트 무관(IP만 확인)이라 공격 창이 더 넓다.
- **권장:** UDP 소스 IP 단독에 의존하지 말 것. Login ack로 서버 발급 세션 토큰/논스를 반환하고 이후 모든 요청에서 검증, 그리고/또는 세션별 단조 증가 시퀀스 윈도우 강제. 최소한 링크 보호(IPsec/신뢰 L2) 요구를 문서화하고 시퀀스 미진행 요청을 거부.

#### [PERF-001] 동기 NVRAM 쓰기(open+write+fsync+rename)가 명령 디스패치 안에서 단일 스레드 이벤트 루프를 블로킹
- **영역:** 성능 (cross_ref: 아키텍처)
- **위치:** `opcd/store.c:44` (`opc_store_write_atomic` — `handle_set_password`/`handle_set_radio_config`/`handle_set_ip_config_list`의 `save_*`에서 호출)
- **확신도:** high
- **문제:** opcd는 단일 스레드 epoll 루프(opcd.c:183-237)다. 그 안에서 `opcd_dispatch -> handle_set_* -> save_* -> opc_store_write_atomic`가 open()+write() 루프+fsync()(store.c:44)+close()+rename()을 호출 스레드에서 전부 동기 블로킹으로 실행한다. 실행 중 `epoll_wait`에 재진입하지 못해 다른 UDP 패킷 수신·idle-logout 체크·indication tick이 모두 멈춘다. 스펙 자체가 `OPC_TIMER_NVRAM_WRITE_S = 120`(proto.h:48)으로 NVRAM 커밋이 최대 120 s 걸릴 수 있음을 가정한다. i.MX8MM eMMC/NAND의 fsync는 wear-leveling/GC로 수백 ms~수 초 지연 가능.
- **영향:** 응답성/처리량. 단일 SetPassword/SetRadioConfig/SetIpConfigList가 fsync 지연(스펙 예산 120 s) 동안 데몬 전체를 정지. 그 사이 모든 클라이언트 타임아웃, KeepAlive/indication tick 누락, idle-logout 지연. 바쁜 링크에서 전 제어평면 head-of-line 블록.
- **권장:** NVRAM 커밋을 핫패스 밖으로: (a) 비동기 쓰기(워커 스레드/epoll에 추가한 전용 fd, Set*를 즉시 ack하고 eventfd로 완료 통지) 또는 (b) 최소한 per-write fsync를 통합/주기 flush로 변경. 동기 의미가 필수면 루프 정지를 문서화하고 write timeout으로 한정.

#### [ARCH-001 + STYLE-001] 와이어 ErrorCause 코드가 명명되지 않은 매직 넘버로 핸들러 전반에 산재; `0x0010`이 세 가지 의미로 충돌
- **영역:** 아키텍처(ARCH-001, high) **+ 스타일(STYLE-001, medium)** — 동일 근본 원인 병합, 높은 쪽(high) 채택
- **위치:** `opcd/handler.c:81` (`handle_login`/`handle_set_password`/`handle_set_ip_config_list`/`handle_change_ip_address`/`handle_set_radio_config`)
- **확신도:** high
- **문제:** 명령별 ErrorCause가 6곳에서 맨 리터럴로 작성됨: `0x0010`(비밀번호 불일치 handler.c:81,196 / 슬롯 범위초과 :226 / 라디오 station-type 무효 :294), `0x0011`(슬롯 비어있음 :269/270), `0x0012`(IP 변경 충돌 :266). protocol 계층은 공통 cause(proto.h:40-44 `OPC_ERR_NONE`..`OPC_ERR_NVRAM`)만 정의하고 0x0010+ 범위는 데몬에 리터럴로 방치. 게다가 명명 상수 `OPC_ERR_INDICATION_SETTING_VIOLATION 0x0010`(ids.h:81)이 무관한 'password mismatch'/'slot range' 리터럴과 수치 충돌 — 같은 0x0010이 명령에 따라 세 가지를 의미. vhlctl이 이를 시인: `"0x0010 (indication-setting/pw-mismatch/slot-range)"`(vhlctl.c:58).
- **영향:** 오류 계약이 protocol 계층 소유가 아니라 producer(opcd)·consumer(vhlctl)가 의미를 합의 못 함. 와이어상 0x0010이 진짜로 모호 — VHL이 password-mismatch/config-range/indication-violation을 구분 불가하여 진단·재시도 로직이 오도될 수 있음. 명명 규칙('모든 와이어 상수는 OPC_* 매크로') 위반이고, 새 cause 추가 시 silent 충돌 유발.
- **권장:** 0x0010+ 명령별 ErrorCause를 `protocol/ids.h`(또는 전용 errors.h)에 의미당 하나씩 명명 상수로 정의(`OPC_ERR_PASSWORD_MISMATCH`, `OPC_ERR_SLOT_RANGE`, `OPC_ERR_RADIO_PARAM`, `OPC_ERR_SLOT_EMPTY=0x0011`, `OPC_ERR_IP_CHANGE_CONFLICT=0x0012`). handler.c와 vhlctl.c가 함께 참조하고 `err_str()`도 갱신. 각 명령에 고유 코드를 주어 0x0010 과부하 해소.

#### [ARCH-002] Idle 자동 로그아웃 로직이 두 상태 권위에 중복 (cross_ref ↔ PERF-006: 성능 무관 판정 — 상충/미해결 참조)
- **영역:** 아키텍처
- **위치:** `opcd/handler.c:374` (`opcd_dispatch` vs `opcd.c` 타이머 분기)
- **확신도:** high
- **문제:** 로그인 세션을 두 독립 경로가 복붙되었으나 비동일한 로직으로 변경한다. handler.c:374-378의 dispatch 내부 idle 체크와 opcd.c:198-207의 타이머 분기가 같은 teardown을 하되, 후자는 holder를 추가로 LOG하고 `mono_now()` 대신 `clock_gettime`을 재호출한다. `handle_logout`(handler.c:109-111)에 세 번째 teardown 사본이 또 있다. 세션 상태(logged_in, holder_ip, boot_status, idle_deadline)의 단일 소유자가 없다.
- **영향:** logout 의미(holder_ip 클리어, indication 타겟 리셋, 다른 indication 발행 등)를 바꾸려면 3곳을 복제해야 하며 안 하면 silent 발산. 두 idle 체크 사본이 이미 부수효과(로깅·클럭 소스)에서 다름 — 동기화 버그 온상.
- **권장:** `session_logout(opcd_state_t*, reason)` 단일 함수를 두어 전체 teardown+indication 발행을 소유하게 하고 dispatch idle 체크·타이머 idle 체크·handle_logout에서 호출. opcd.c 타이머는 술어를 중복하지 말 것.

### 🟡 Medium

#### [ARCH-003 + STYLE-005] codec `pack()` 반환값을 체계적으로 폐기; `-1`이 응답 길이로 저장됨
- **영역:** 아키텍처(ARCH-003, medium) **+ 스타일(STYLE-005, low, cross_ref 보안)** — 병합, 높은 쪽(medium) 채택
- **위치:** `opcd/handler.c:92` (모든 `handle_*` 함수)
- **확신도:** high
- **문제:** 모든 핸들러가 `*rlen = opc_*_ack_pack(resp, rcap, seq, &ack);`로 끝난다(handler.c:92,114,134,180,208,250,278,304,334,351). pack 함수는 `ssize_t`를 반환하고 실패 시 -1(null 인자/용량 부족, frame.h:28-31)인데 체크 없이 `*resp_len`에 직접 대입. -1이면 호출자 opcd.c:226의 `if (rc == 0 && tx_len > 0)`가 음수 tx_len 전송을 막아 우연히 안전하지만 오류는 silent 삼켜지고 'success=0' 계약이 무의미해진다. vhlctl도 unchecked pack 결과를 `(size_t)tn`으로 캐스팅(vhlctl.c:150-151) — 여기선 -1이 거대한 size_t가 되어 **실제로 위험**(STYLE-005 보안 cross_ref).
- **영향:** codec 용량/인자 실패가 경계에서 '응답 없음'과 구분 불가; protocol 계층이 유지하는 -1 규약이 버려짐. 용량 회귀가 silent해지고 향후 클라이언트 오류 보고가 복잡해진다. vhlctl 측은 잠재적 메모리 안전 위험.
- **권장:** 각 pack 결과 체크: 핸들러는 음수 시 `*rlen=0`(응답 없음) 후 로그/ NG ack 합성, opcd.c가 로깅할 별도 코드 반환. 작은 `emit_ack()` 래퍼로 체크 중앙화. vhlctl은 `tn < 0` 시 size_t 캐스팅 전 중단.

#### [SEC-002] Indication 타겟 IP/포트가 공격자 완전 제어 — 데몬을 UDP 리플렉터/증폭기로 악용
- **영역:** 보안 (cross_ref: 아키텍처)
- **위치:** `opcd/handler.c:319` (`handle_set_indication_config` / `send_indication_frame`)
- **확신도:** high
- **문제:** SetIndicationConfig가 요청의 수신 IP/포트를 검증 없이 `st->indication_recipient_ip/_port`로 복사(handler.c:319-324)하고, `send_indication_frame`(indication.c:18-24)이 그 임의 host:port로 프레임을 전송한다. SEC-001(스푸핑 로그인) 또는 정상 로그인 1회와 결합해, 공격자가 victim_ip:victim_port와 `info_bits=KeepAlive|InitComplete`, `period_seconds=1`인 SetIndicationConfig 하나만 보내면 데몬이 이후 1초마다 88바이트 KeepAlive를 무기한, rate-limit·역경로 없이 victim에게 발사한다.
- **영향:** Reflection/amplification DoS. 작은 요청이 지속적 주기 출력을 유발. 정보/스캔 oracle로도 사용 가능.
- **권장:** indication 수신자를 로그인 holder IP(또는 명시적 allow-list 관리 서브넷)로 제한, holder_ip와 다른 수신자 거부, `period_seconds` 하한 캡, logout/idle 시 indication 중지. 로그인 시 관측한 holder 소스 포트에 바인딩 고려.

#### [SEC-004 + ARCH-005] GetBasicInformation: 인증 없이 + 파싱 실패에도 응답 / GetDeviceInfo와 read-path 권위 불일치
- **영역:** 보안(SEC-004, low) **+ 아키텍처(ARCH-005, medium)** — 동일 핸들러 정책 불일치, 높은 쪽(medium) 채택
- **위치:** `opcd/handler.c:118` (`handle_get_basic_info` vs `handle_get_device_info`)
- **확신도:** medium
- **문제:** `handle_get_basic_info`(118-136)는 login 불요, malformed 요청 무시('best effort' :123-125), station_type을 인라인 fallback(`st->radio.station_type ? ... : st->conf.default_station_type` :131-132)으로 계산하고 `check_login_required`를 호출하지 않는다. 임의 미인증 원격 호스트가 GetBasicInfo(0x0001) 한 번으로 vendor/product/subcode/device_status/station_type을 받는다 — 프레임 well-formedness와 무관하게 응답하므로 liveness/fingerprint oracle(OUI, product, boot/ready/logged-in 상태)이 되어 SEC-001 표적 선정·타이밍을 돕는다. 반면 `handle_get_device_info`(138-182)는 login 요구, indication_enabled 시 거부, station_type을 fallback 없이 `st->radio`에서 직독(:164). 'effective station type' 규칙이 한 핸들러에만 존재.
- **영향:** Pre-auth 정보 노출(디바이스 식별 + login-state oracle). 동시에 파생 필드(effective station type, device status)의 권위가 중앙화되지 않아 두 질의 응답이 불일치 가능.
- **권장:** discovery 의도는 인정하되 (1) 성공적으로 파싱된 요청에만 응답, (2) `device_status==LOGGED_IN` 등 live login-state를 미인증 피어에 노출하지 않음, (3) 미인증 응답 rate-limit. 파생 상태는 `effective_station_type(st)`/`device_status(st)` accessor로 두 핸들러가 공유.

### ⚪ Low / Info

| id | 영역 | 위치 | 제목 | 심각도 |
|----|------|------|------|--------|
| SEC-003 | 보안 (xref 아키텍처) | `protocol/frame.c:52` | `header.length`를 실제 datagram 길이와 교차검증 안 함 — `OPC_ERR_PACKET_SIZE` 미발행 (입력검증 완전성 갭, 메모리 안전엔 무해) | low |
| SEC-005 | 보안 | `opcd/opcd_state.h:26` | 하드코딩 기본 비밀번호 `"MyPassword"` + 평문 디스크 저장, strncmp 비교 — factory 유닛 자명 침해 + 평문 노출 | low |
| ARCH-006 | 아키텍처 | `opcd/opcd_state.h:51` | `opcd_state_t`가 데몬 전체 내부를 단일 flat mutable struct로 노출 (서브모듈 소유권 없음); 단일 스레드라 영향 제한 | low |
| PERF-002 | 성능 | `opcd/indication.c:35` | 7개 indication 헬퍼가 매번 1424 B `frame[OPC_FRAME_MAX]`를 스택에 할당 (실제 body는 <128 B) — 제약 ARM64 스택 압박 | low |
| PERF-003 | 성능 | `opcd/handler.c:218` | 1048 B SetIpConfigList req struct를 entry 수 무관하게 스택에 + 전체 memset (commands.c:448) | low |
| PERF-004 | 성능 (xref 아키텍처) | `opcd/handler.c:239` | SetIpConfigList 커밋마다 6.6 KB ip_list struct 복사+memset(13.5 KB) + 6.6 KB 디스크 쓰기 (PERF-001 fsync 정지 가중) | low |
| STYLE-002 | 스타일 | `protocol/commands.c:71` | early-return guard brace 스타일이 파일 단위로 분기(braced multiline vs single-line); .clang-format 권장 | low |
| STYLE-003 | 스타일 | `protocol/indications.c:196` | `bounded_strnlen` 헬퍼가 commands.c:9-16와 indications.c:196-201에 중복 정의 — 공유 헤더로 승격 | low |
| STYLE-004 | 스타일 | `protocol/commands.c:352` | 명명 length 매크로 존재하나 password/ipcfg essid 경로는 raw 128/32 리터럴 사용 — `OPC_PASSWORD_FIELD_LEN` 추가 권장 | low |
| PERF-005 | 성능 | `opcd/handler.c:380` | (확인) dispatch는 이미 상수 시간 jump table, RX는 zero-copy — 변경 불요 (ARCH-004와 상충, 아래 참조) | info |
| PERF-006 | 성능 | `opcd/handler.c:374` | (확인) per-packet `mono_now()`는 ARM64 vDSO로 무시 가능 — 성능 조치 불요 (ARCH-002와 상충, 아래 참조) | info |
| STYLE-006 | 스타일 | `opcd/handler.c:347` | reset cause `0x00000001` 등 매직 리터럴 미명명 — `OPC_RESET_CAUSE_*` 매크로 권장 | info |
| STYLE-007 | 스타일 | `protocol/indications.h:148` | 정의-미사용 documentary 매크로(`OPC_TIMESTAMP_MAX_LEN`, `OPC_LOGIN_PASSWORD_MAX` 등) — STYLE-004와 연계 | info |

## 상충/미해결

### 1. 명령 디스패치 구조 — ARCH-004(medium) ↔ PERF-005(info)
- **위치:** `opcd/handler.c:380` (`opcd_dispatch` switch)
- **아키텍처(ARCH-004, medium):** 명령/indication 디스패치가 5-6개 파일에 걸친 병렬 switch/if-체인으로 수동 유지됨. 명령 1개 추가 시 ids.h/commands.h/commands.c/handler.c(handle_* + case)/vhlctl.c를 모두 손대야 하며 구조적으로 누락 방지가 안 됨. **정적 테이블(id→handler fn) 도입 권장.**
- **성능(PERF-005, info):** 같은 switch가 컴파일러에 의해 jump table/균형 비교 트리로 컴파일되어 사실상 O(1) — **성능상 변경 불요, switch 유지 권장.**
- **판정:** 상충 아님 — 서로 다른 축. 성능은 손대지 말라 하고 아키텍처는 유지보수성을 위해 테이블화하라 함. **결정 권장:** 성능 회귀 없이(테이블도 O(1)) 유지보수성 개선이 가능하므로 ARCH-004의 테이블화를 채택하되, 핫패스 zero-copy/단일 tx 빌드 구조(PERF-005가 확인)는 보존할 것. 추가 검증 불요.

### 2. Idle-logout 코드 경로 — ARCH-002(high) ↔ PERF-006(info)
- **위치:** `opcd/handler.c:374` (dispatch idle 체크) + `opcd.c:198-207` (타이머)
- **아키텍처(ARCH-002, high):** dispatch와 타이머의 idle-logout 로직이 중복·비동일 — **`session_logout()` 단일화로 두 술어 중 하나 제거 권장.**
- **성능(PERF-006, info):** 두 경로의 `clock_gettime`/`mono_now` 호출은 vDSO로 무시 가능 — **성능상 dispatch 체크를 굳이 제거할 필요 없음(정확성에 도움).**
- **판정:** 상충 아님 — 성능은 비용이 없다 하고, 아키텍처는 중복 자체(부수효과 발산: 로깅·클럭 소스 차이)를 문제 삼음. **결정 권장:** ARCH-002의 중복 제거(teardown 로직 단일 소유)를 채택하되 PERF-006이 지적한 대로 eager dispatch 체크의 정확성 이점은 단일 `session_logout()` 호출로 보존(클럭 호출 비용은 무시 가능하므로 성능 제약 아님). 추가 검증 불요.

> 영역 간 진짜 모순(한쪽 critical, 한쪽 무관)은 없음. 위 두 건은 동일 코드에 대한 보완적 관점이며, 권장 통합 방향을 제시함.
