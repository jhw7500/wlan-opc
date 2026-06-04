# P0 보안 하드닝 설계

- **작성일:** 2026-06-04
- **근거:** `wlan-package/docs/review-report.md`(2026-06-02 종합 리뷰) 심층 분석 → P0 우선순위 + 라이브 장치(192.168.0.100) 테스트에서 확인된 빈 비밀번호 로그인
- **브랜치:** `harden/p0-password-errorcode`
- **PR:** 단일 PR (3개 항목 묶음)

## 배경 / 문제

1. **빈 비밀번호 로그인** — 라이브 장치에서 `vhlctl login`(빈 PW)이 OK 반환. 근본 원인: `set-password`가 빈 새 비밀번호를 허용 → 저장 비밀번호가 빈 문자열 → `handle_login`의 `strncmp("", "", n) == 0`으로 인증 통과. 사실상 무인증 제어.
2. **ErrorCause 와이어 코드 다의성** — `0x0010`이 와이어상 4가지 의미(indication-violation / password-mismatch / slot-range / station-type-invalid)로 충돌. `handler.c`에 맨 hex 리터럴 12곳 산재.
3. **위협모델 미문서화** — OPC는 사양상 암호화 없는 평문 LAN 프로토콜. 신뢰 네트워크 배포 전제가 코드/문서에 명시 안 됨.

## 제약 (확정된 결정)

- **와이어 ErrorCause 값은 사양 고정 — 변경 불가.** 값은 유지하고 명명상수로 가독성만 개선.
- **비밀번호 정책은 "빈 비밀번호 차단" 최소안.** 부트스트랩(기본값 `"MyPassword"` 로그인)은 보존. 강제 변경 흐름·프로토콜 변경 없음.

## 설계

### P0-1 빈 비밀번호 차단 (`opcd/handler.c`)

- **수정 ① `handle_login`**: 비밀번호 비교 분기에 선행 조건 추가 — 저장 비밀번호가 비어있으면(`st->password[0] == '\0'`) 무조건 NG (미프로비저닝 = 인증 거부, fail-closed). `error_cause = OPC_ERR_PASSWORD_MISMATCH`(=0x0010).
- **수정 ② `handle_set_password`**: `req.new_password`의 `strnlen == 0`이면 NG, 저장하지 않음. `error_cause = OPC_ERR_PASSWORD_MISMATCH`(=0x0010, password-domain NG 재사용).
- 기본값 `"MyPassword"`는 비어있지 않으므로 부트스트랩 정상.

### P0-2 ErrorCause 명명 리팩터 (`protocol/ids.h`, `opcd/handler.c`, `vhlctl/vhlctl.c`)

- `protocol/ids.h`에 명령별 ErrorCause 명명상수 추가(**와이어 값 불변**, 동일 값 별칭 허용):

  | 명명상수 | 값 | 의미 |
  |---|---|---|
  | `OPC_ERR_PASSWORD_MISMATCH` | 0x0010 | 비밀번호 불일치 / 빈 비밀번호 |
  | `OPC_ERR_SLOT_RANGE` | 0x0010 | IP 슬롯 번호 범위초과 |
  | `OPC_ERR_STATION_TYPE` | 0x0010 | radio station-type 무효 |
  | `OPC_ERR_SLOT_EMPTY` | 0x0011 | IP 슬롯 비어있음 |
  | `OPC_ERR_IP_CHANGE_CONFLICT` | 0x0012 | 리스트 갱신 중 IP 변경 충돌 |
  | `OPC_ERR_RADIO_MODE` | 0x0013 | WLAN mode 무효 |
  | `OPC_ERR_RADIO_BW` | 0x0014 | WLAN bandwidth 무효 |
  | `OPC_ERR_RADIO_APPLY` | 0x0050 | 플랫폼이 radio 적용 거부 |
  | `OPC_RESET_CAUSE_USER` | 0x00000001 | reset 사유(사용자 요청) |

- `handler.c`의 hex 리터럴 12곳을 위 상수로 치환.
- `vhlctl/vhlctl.c`의 `err_str()`도 동일 헤더 참조.
- **0x0010 4중 다의성은 사양 한계로 보존** — `ids.h`에 주석으로 "복수 의미가 같은 와이어 값에 매핑됨(사양 고정)"을 명시.

### P0-3 보안 문서화 (`docs/SECURITY.md` 신규)

- OPC = 평문 LAN 제어 프로토콜 → **신뢰 L2 / 격리망 배포 전제** 명문화.
- 알려진 한계 + 완화:
  - SEC-001: UDP 소스 IP-only 세션 (스푸핑 가능) → 신뢰망 전제
  - 기본 비밀번호 `"MyPassword"` → 최초 운영 시 변경 권고 (+빈 비밀번호는 차단됨)
  - 0x0010 ErrorCause 다의성 → 사양 한계, 명령 컨텍스트로 해석

## 검증

- **단위 테스트** (`opcd/tests/`): 
  - 빈 저장 비밀번호 → 로그인 거부
  - 기본/정상 비밀번호 → 로그인 성공
  - 빈 새 비밀번호 set-password → 거부
  - 정상 set-password → 성공
  - (테스트 가능성: `handle_*`는 static이므로, 호스트 테스트가 가능한 진입점/구조를 먼저 확인. 불가 시 인증 비교 로직을 작은 testable 헬퍼로 추출.)
- **회귀**: 기존 `make check`(protocol codec + opcd 단위) 통과.
- **실장치**: 재빌드·재배포 후 `vhlctl login`(빈 PW) → NG, `login --password MyPassword` → OK 확인.

## 비범위 (YAGNI)

- 강제 비밀번호 변경 흐름, 비밀번호 복잡도 정책, 해시 저장 — 별도 검토.
- SEC-002/ARCH-002/ARCH-003 등 P1 항목 — 후속 PR.
- ErrorCause 와이어 값 재할당 — 사양 고정으로 제외.
