# device-info FREQ/CH 출처 토글 구현 계획

> 설계: `docs/design-device-info-freq-source.md` · 문의: `docs/spec-inquiry.md` G11
> 브랜치: `feat/opcd-device-info-freq-source`

**Goal:** device-info의 WLAN FREQ/CH를 opc.conf 토글(`device_info_freq_source = config|live|auto`)로 선택. 출하 기본 `config`(기존 동작 무변화).

**Architecture:** `opcd_conf_t`에 enum 추가 → opcd 시작 시 opc.conf 파싱 → `handle_get_device_info`가 모드에 따라 설정값(`st->radio`) 또는 live(`get_link()`) freq/ch 선택. live channel은 `opc_chan_field()`로 band 인코딩.

**Tech Stack:** C11, 기존 opcd 모듈 + protocol `opc_chan_field`, host 단위테스트(`make check`).

## Global Constraints
- 와이어 포맷·vhlctl·protocol 무변경. 기본값 `config`로 회귀 0.
- live channel은 raw 채널번호 → 반드시 `opc_chan_field(freq, ch)` 인코딩.
- `make check` green 유지(protocol 19 + opcd 41 + 신규).

---

### Task 1: conf enum + 기본값

**Files:** Modify `opcd/opcd_state.h` (opcd_conf_t), `opcd/opcd.c:122-128`(state_set_defaults), `opcd/opcd.c:255`(main, conf parse)

- enum 추가 (`opcd_state.h`):
```c
typedef enum {
    OPC_FREQ_SRC_CONFIG = 0,  /* 항상 set-radio 설정값 (spec §3.3.4 "설정 주파수") */
    OPC_FREQ_SRC_LIVE,        /* 항상 live 접속값 (미접속이면 0/0) */
    OPC_FREQ_SRC_AUTO,        /* 접속 시 live, 미접속 시 설정값 */
} opcd_freq_source_t;
```
  `opcd_conf_t`에 `opcd_freq_source_t device_info_freq_source;` 추가.
- 기본값 (`state_set_defaults`): `st->conf.device_info_freq_source = OPC_FREQ_SRC_CONFIG;`
- 파서 (`opcd.c`, fault_probe conf와 동일 line-scan):
```c
static opcd_freq_source_t parse_devinfo_freq_source(const char *conf_path)
{
    opcd_freq_source_t src = OPC_FREQ_SRC_CONFIG;
    FILE *f = fopen(conf_path, "r");
    if (!f) return src;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char key[48], val[64];
        if (sscanf(line, " %47[A-Za-z0-9_] = %63s", key, val) != 2) continue;
        if (strcmp(key, "device_info_freq_source") != 0) continue;
        if      (strcmp(val, "live") == 0) src = OPC_FREQ_SRC_LIVE;
        else if (strcmp(val, "auto") == 0) src = OPC_FREQ_SRC_AUTO;
        else                                src = OPC_FREQ_SRC_CONFIG;
    }
    fclose(f);
    return src;
}
```
  main에서 `opcd_fault_probe_conf(...)` 다음 줄: `st.conf.device_info_freq_source = parse_devinfo_freq_source(st.paths.conf);`

### Task 2: handler 모드 분기 (핵심)

**Files:** Modify `opcd/handler.c` (include `chan_encode.h`; capture live at get_link; 573-579 분기)

- include 추가: `#include "chan_encode.h"`
- 선택 헬퍼 (handle_get_device_info 위에):
```c
static void select_devinfo_freq_ch(opcd_freq_source_t src, bool assoc,
                                   uint16_t live_freq, uint16_t live_ch,
                                   uint16_t cfg_freq, uint16_t cfg_ch,
                                   uint16_t *out_freq, uint16_t *out_ch)
{
    bool use_live = (src == OPC_FREQ_SRC_LIVE) ||
                    (src == OPC_FREQ_SRC_AUTO && assoc);
    if (use_live && assoc) {
        *out_freq = live_freq;
        *out_ch   = opc_chan_field(live_freq, live_ch);
    } else if (use_live) {          /* LIVE + 미접속 → 0/0 */
        *out_freq = 0; *out_ch = 0;
    } else {                         /* CONFIG, 또는 AUTO+미접속 */
        *out_freq = cfg_freq; *out_ch = cfg_ch;
    }
}
```
- get_link(0) 블록에서 live 캡처 locals 추가: `bool w1_assoc=false; uint16_t w1_lfreq=0, w1_lch=0;` → 블록 안 `w1_assoc=link.associated; w1_lfreq=link.freq_mhz; w1_lch=link.channel;` (wlan2 동일: w2_*).
- 573-574 교체:
```c
select_devinfo_freq_ch(st->conf.device_info_freq_source, w1_assoc,
                       w1_lfreq, w1_lch,
                       st->radio.wlan1.freq_mhz, st->radio.wlan1.channel,
                       &ack.wlan1.freq_mhz, &ack.wlan1.channel);
```
  578-579 (DUAL) 동일하게 w2_*/wlan2.

### Task 3: stub link 주입 API (테스트용)

**Files:** Modify `opcd/platform_stub.c`

```c
static bool s_link_set[2];
static opcd_platform_link_t s_link[2];
void stub_set_link(int idx, bool assoc, uint16_t freq, uint16_t ch) {
    if (idx < 0 || idx > 1) return;
    memset(&s_link[idx], 0, sizeof s_link[idx]);
    s_link[idx].associated = assoc;
    s_link[idx].freq_mhz   = freq;
    s_link[idx].channel    = ch;
    s_link_set[idx] = true;
}
void stub_reset_link(void) { s_link_set[0] = s_link_set[1] = false; }
```
  `stub_get_link`: `if (idx < 2 && s_link_set[idx]) { *out = s_link[idx]; return 0; }` (기존 미접속 폴백 유지)

### Task 4: 테스트 (TDD)

**Files:** Modify `opcd/tests/test_handler.c`

- extern 선언: `extern void stub_set_link(int,bool,uint16_t,uint16_t); extern void stub_reset_link(void);`
- 헬퍼:
```c
static int do_get_devinfo(opcd_state_t *st, uint32_t cip,
                          uint16_t *w1_freq, uint16_t *w1_ch) {
    uint8_t frame[OPC_FRAME_MAX];
    ssize_t fn = opc_get_device_info_req_pack(frame, sizeof frame, 1);
    if (fn <= 0) return -1;
    uint8_t resp[OPC_FRAME_MAX]; ssize_t rlen = 0;
    if (opcd_dispatch(st, frame, (size_t)fn, cip, 5000, resp, sizeof resp, &rlen) != 0) return -1;
    opc_get_device_info_ack_t ack;
    if (opc_get_device_info_ack_unpack(resp, (size_t)rlen, &ack) != 0) return -1;
    *w1_freq = ack.wlan1.freq_mhz; *w1_ch = ack.wlan1.channel;
    return 0;
}
```
- 검증 (set-radio로 설정값 5180/0x0224 주입; live = 5240/ch48 → `opc_chan_field`=0x0230):
  - config + associated → freq 5180, ch 0x0224 (회귀)
  - config + not-assoc → 5180/0x0224
  - live + associated → 5240/0x0230
  - live + not-assoc → 0/0
  - auto + associated → 5240/0x0230
  - auto + not-assoc → 5180/0x0224
  각 케이스: login → do_set_radio(5180,0x0224) → `st.conf.device_info_freq_source=...` → stub_set_link/stub_reset_link → do_get_devinfo → ASSERT.

### Task 5: 검증 + 빌드
- `make check` → all tests passed.
- `make arm64 PLATFORM=nxp` → 크로스 컴파일 무경고.

### Task 6: 커밋·푸시·PR + 3-리뷰어 모니터링
- 커밋, push, PR 생성, Codex·Gemini·Claude 리뷰 완주까지 모니터링.
