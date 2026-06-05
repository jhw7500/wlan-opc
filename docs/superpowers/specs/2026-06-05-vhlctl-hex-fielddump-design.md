# vhlctl `--hex` 필드별 hex 분해 모드 — 설계

- **일시**: 2026-06-05
- **대상**: `vhlctl` (VHL-side CLI)
- **목적**: 응답 프레임을 필드 단위로 `라벨 @절대오프셋 (N B): 원시hex = 디코딩값` 형태로 출력해 프로토콜/데이터소스 디버깅을 쉽게 한다.

## 동기

현재 vhlctl 출력은 두 가지뿐:
- `--dump`: 프레임 전체 raw hexdump (16B/줄, **필드 경계 없음**)
- 기본: 디코드된 값 (hex 아님, 오프셋 없음)

"각 필드가 어느 바이트인지 + 그 raw hex + 디코드값"을 한 번에 보는 모드가 없다. 이를 `--hex`로 추가한다.

## 범위

전역 `--hex` 플래그. 적용 대상:
- `basic-info` ack
- `device-info` ack
- `listen` 수신 indication 7종 (InitComplete/WlanStatusChange/Roaming/ApDisconnect/FaultDetect/ResetNotice/KeepAlive)

요청 프레임과 단순 ack(login/logout/set-*/reset = result+err 4B)는 대상 외 — raw `--dump`로 충분.

## 동작

- `--hex`가 켜지면 해당 명령은 기존 디코드 출력 **대신** 필드별 분해 출력 (hex+디코드값이 한 줄에 다 있어 정보 손실 없음).
- `--dump`(raw 전체)와 **독립** — 둘 다 켜면 둘 다 출력.
- NG ack(device-info 등 result≠OK)는 헤더 + result/error_cause 필드까지만 분해 (body가 0으로 차 있으므로).

## 출력 포맷

```
device-info (--hex):
  [hdr] protocol_ver  @00 ( 1B): 01                      = 0x01
  [hdr] command_type  @01 ( 1B): 02                      = ACK
  [hdr] req_id        @02 ( 2B): 00 02                   = 0x0002
  [hdr] sequence      @04 ( 2B): 00 01                   = 1
  [hdr] length        @06 ( 2B): 01 98                   = 408
  result          @64 ( 2B): 00 00                   = OK
  error_cause     @66 ( 2B): 00 00                   = 0x0000
  vendor_code     @68 ( 4B): 00 90 2c fb             = 0x00902cfb
  essid           @204 (32B): 6a 68 77 5f 77 6c 61 6e 00 …  = 'jhw_wlan'
  wlan1.rssi      @356 ( 1B): b9                      = -71
```
- 라인: `라벨 @절대오프셋 (크기B): 원시hex [요약시 …] = 디코딩값`
- hex는 최대 N바이트(기본 16)까지 표시, 초과분은 `…`
- 절대오프셋 = `OPC_HEADER_SIZE(64)` + body offset (헤더 필드는 0~7)

## 구조 (접근 A: 디스크립터 테이블 + 범용 포맷 함수)

새 모듈 `vhlctl/fielddump.{c,h}` (테스트 가능하도록 vhlctl.c에서 분리):

```c
typedef enum {
    FD_HEX,    /* raw만 (reserve 등) */
    FD_U8, FD_I8, FD_U16BE, FD_U32BE,
    FD_MAC,    /* xx:xx:xx:xx:xx:xx */
    FD_STR,    /* NULL-종단 ascii */
    FD_DATE    /* year(2)+month(1)+day(1) */
} fd_kind_t;

typedef struct { const char *label; size_t off; size_t len; fd_kind_t kind; } fd_field_t;

/* 한 필드를 사람이 읽는 한 줄 문자열로 렌더 (경계검사 포함). 테스트 진입점. */
void fd_render(char *out, size_t outcap,
               const uint8_t *frame, size_t frame_len, const fd_field_t *f);

/* 응답 타입별 전체 분해를 fp에 출력 (헤더 공통 + body 테이블). */
void fd_dump_basic_info (FILE *fp, const uint8_t *frame, size_t len);
void fd_dump_device_info(FILE *fp, const uint8_t *frame, size_t len);
void fd_dump_indication (FILE *fp, uint16_t ind_id, const uint8_t *frame, size_t len);
```

- 헤더 공통 필드 테이블 1개 + 응답별 body 테이블(절대오프셋으로 기재).
- 오프셋 출처: device-info는 `commands.h` 주석, indication은 `indications.c`, 헤더는 `codec.h`+`--dump` 실측.
- 경계검사: `off+len > frame_len`이면 `(truncated)` 표시, over-read 없음.

## 테스트 (`vhlctl/tests/`, `make check` 통합)

신설 `vhlctl/tests/test_fielddump.c` + `Makefile`. 루트 `Makefile`의 `check`에 추가.

핵심 전략 — **pack을 ground truth로**:
1. 알려진 값으로 ack 구조체를 채우고 `opc_get_device_info_ack_pack` / `opc_get_basic_info_ack_pack` / `opc_ind_*_pack`으로 프레임 생성.
2. `fd_render`/`fd_dump_*`로 분해.
3. 각 필드의 디코드값이 원래 넣은 값과 일치하는지 검증.
→ 디스크립터 오프셋이 commands.c/indications.c와 어긋나면 테스트가 깨진다(자동 정합).

추가 케이스: 경계검사(짧은 프레임 → truncated), MAC/STR/DATE/I8(음수 RSSI) 렌더, hex truncation.

## 영향 파일

- 신규: `vhlctl/fielddump.h`, `vhlctl/fielddump.c`, `vhlctl/tests/test_fielddump.c`, `vhlctl/tests/Makefile`
- 수정: `vhlctl/Makefile`(fielddump.o 링크), 루트 `Makefile`(check에 vhlctl/tests), `vhlctl.c`(`--hex` 플래그 + 3개 cmd 분기)
- 문서: `docs/testing-guide.md`에 `--hex` 사용법 1단락 추가

## 구현 변경 — 누적(cumulative) offset 채택 (2026-06-05)

프로토콜 일부 필드의 **길이/바이트수가 변경될 예정**이라, 절대 offset 하드코딩은 변경 비용이 크다(한 필드 길이가 바뀌면 그 뒤 모든 행의 offset을 수동 수정). 그래서 구조를 바꿨다:

- `fd_field_t`에서 `off`를 제거하고 **`{label, len, kind}`** 만 둔다. dump 워커가 헤더 끝(`OPC_HEADER_SIZE`)부터 offset을 **누적 계산**한다. → 필드 길이가 바뀌면 **그 행의 `len` 한 곳만** 수정하면 뒤 필드가 자동 시프트.
- reserve/pad gap은 **명시 `FD_HEX` 행**으로 둬서 누적 offset이 codec과 어긋나지 않게 한다.
- `fd_render(out, cap, frame, frame_len, off, f)` — off를 인자로 받는다.
- **전 필드 테스트**(`test_dump_device_info_all_fields` + `line_has`): device-info의 모든 디코드 필드를 distinct 값으로 채워, 각 값이 자기 라벨 줄에 있는지 검증. 어느 오프셋이 밀려도 `make check`가 잡는다.

> codec(`commands.c`/`indications.c`)과는 여전히 별도 정의이지만, 테스트가 `opc_*_ack_pack`을 ground truth로 대조하므로 드리프트는 silent하지 않고 `make check` 실패로 드러난다. 완전 SSOT(매크로 공유)는 codec 재설계가 필요해 현 단계 비범위.

## 비범위 (YAGNI)

- 요청 프레임/단순 ack 분해 (raw `--dump`로 충분)
- result/err 외 enum 전체 이름 테이블 (핵심 값만 라벨)
- 바이트 편집/주입 기능
