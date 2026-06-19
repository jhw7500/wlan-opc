# 설계: device-info FREQ/CH 출처 토글 (`device_info_freq_source`)

> 작성일: 2026-06-19 · 작성: wlan-opc 팀
> 상태: **설계 승인 → 구현 계획 대기**
> 관련: `docs/opc_vhl_protocol_Rev1.00_KO.md` §3.3.4(:470/:745), `docs/spec-inquiry.md` G11,
> `docs/spec-conformance.md`(#17 SetRadio Result 인접), `opcd/handler.c`, `opcd/platform.h`

## 1. 배경 / 문제

실타겟(cts-wlan) 검증 중, device-info가 보고하는 WLAN 주파수/채널이 실제 접속과 불일치:

- `iw mlan0 link` → **freq 5240 / ch48** (AP `FXE3000_JHW`)
- device-info → **freq=5180 / ch=0x0224(ch36)**

근본원인(코드 확정):
- device-info의 freq/ch는 **set-radio 설정 캐시**(`st->radio`, `radio.conf` 16B 바이너리)에서 무조건 옴 — `handler.c:573-574`, 주석 `:524-526`("freq/channel still come from the set-radio cache").
- 반면 mode/bw/rssi/snr/ap_mac 등은 **live**(`get_link()`) 우선.
- `radio.conf`(`xxd`)에 `01 00 | 00 00 | 3c 14 | 24 02 | 0b | 02 | …` = station SINGLE / freq 5180 / ch 0x0224 / mode 11(11AX) / bw 2(80MHz)가 영구 저장돼 있어 그 설정값이 출력된 것.

스펙은 이 필드를 **"설정 주파수/설정 CH"**(§3.3.4, :470/:745)로 정의하므로 **현 동작은 스펙 준수(버그 아님)**. 다만 운영상 "현재 접속 주파수"가 더 유용하다는 요구가 있고, 추후 고객사가 설정값/접속값 중 무엇을 표준으로 할지 미확정.

## 2. 결정

**A안 채택**: opc.conf 토글 `device_info_freq_source`로 출처를 선택. 고객사 회신 전까지 **출하 기본 = `config`(스펙 엄수, 동작 무변화)**, 테스트는 `auto` 사용.

- 토글이라 고객사가 어느 쪽을 원하든 **설정 한 줄로 대응**(재빌드/재배포 없이).
- `auto`는 freq/ch를 mode/bw의 기존 하이브리드 동작과 **일관**되게 만듦.

## 3. 설정 (opc.conf)

```
device_info_freq_source = config   # 기본: 항상 설정값(스펙 §3.3.4 준수)
                        | live      # 항상 접속(live)값, 미접속이면 0/0
                        | auto       # 접속 시 접속값, 미접속 시 설정값
```

- opcd 시작 시 opc.conf에서 1회 파싱 → `opcd_conf_t`의 enum 필드에 저장.
- 미설정 / 인식불가 토큰 → **`config`로 폴백**(안전 기본).
- 파싱은 기존 opc.conf 리더(`opcd_fault_probe_conf`, `congestion_*` line-scan, `fault_probe.c:148`)와 동일한 `key = value` 라인 스캔 패턴을 opcd 시작부에 추가.

## 4. 동작 (wlan1·wlan2 동일 적용)

| 모드 | associated일 때 freq/ch | 미접속일 때 freq/ch |
|---|---|---|
| `config` | 설정값 `st->radio.wlanN.{freq_mhz,channel}` | 설정값 |
| `live`   | 접속값 `get_link()` | `0 / 0` (스펙 "no association", `platform.h:47`) |
| `auto`   | 접속값 `get_link()` | 설정값 |

- **연결 판정** = `link.associated` (이미 `wlanN.status` 계산에 쓰는 값, `handler.c:538`).
- **freq(live)** = `link.freq_mhz` (`get_link`이 link.json `info.freq`로 이미 채움, `platform_nxp.c` nxp_get_link).
- **channel(live)** = **`opc_chan_field(link.freq_mhz, link.channel)`로 OPC 포맷(상위=band, 하위=ch)으로 인코딩**.
  link.json `info.channel`은 raw 채널번호(예 48)라 band 상위바이트가 없음 → 반드시 인코딩(`opcd/chan_encode.h`, indication 경로와 동일 헬퍼). 설정값 `st->radio.channel`은 이미 인코딩돼 있으므로 그대로 사용.
- 구현은 **mode/bw의 기존 `w1_*_live` 패턴 미러링**(`handler.c:539-546`, `575-581`).

## 5. 영향 범위

| 파일 | 변경 |
|---|---|
| `opcd/opcd_state.h` | `opcd_conf_t`에 `device_info_freq_source` enum 필드 추가 |
| `opcd/opcd.c` | 시작 시 opc.conf 키 파싱 → `st.conf`에 적재(기본 `config`) |
| `opcd/handler.c` | device-info freq/ch 대입부(`573-579`)를 모드 분기로 교체 |
| `opcd/platform.h` / `platform_nxp.c` | **무변경** — link 구조체에 freq/channel/associated 이미 존재·채워짐 |
| `opcd/snapshot.c` | **무변경** — ack 그대로 publish하므로 자동 일관 |
| `vhlctl/`, `protocol/` | **무변경** — 와이어 포맷 동일 |

## 6. 기본값 & 안전성

- 출하 기본 `config` → **기존 동작과 100% 동일** → 고객사 회신 전 머지/배포해도 회귀 없음.
- 테스트/검증 시 opc.conf에 `device_info_freq_source = auto` 후 `systemctl restart opcd`.
- `auto`/`live`에서 freq 필드 의미가 상황에 따라 달라지므로, VHL은 `wlanN.status`(associated 비트)로 "이 값이 접속값인지 설정값인지"를 구분 가능.

## 7. 테스트 (`make check` host 단위테스트)

- stub `get_link`이 (a) associated + freq/ch 보유, (b) 미접속 두 케이스를 반환하도록 하여
  **3모드 × 2상태**의 device-info freq/ch 필드를 검증.
- live channel의 **band 상위바이트 인코딩**(`opc_chan_field`) 정확성 확인.
- `config` 모드는 기존 회귀(설정값 그대로) 유지 확인.

## 8. 범위 외 (YAGNI)

- `radio.conf` 텍스트화(현재 바이너리 구조체 덤프) — 별건 리팩토링.
- live freq를 커널 nl80211(`nxp_get_iface_freq`, 권위 소스, 주석 `platform_nxp.c:443`)에서 직접 취득 —
  현재는 link.json `info.freq` 사용(cantops logger 갱신 주기에 의존하는 정확도 한계 존재). 향후 정확도 옵션으로 분리.

## 9. 미해결 (고객사 확인 대기)

- device-info FREQ/CH의 표준 의미가 **설정값**인지 **접속값**인지 발주처 확정 필요 → `docs/spec-inquiry.md` **G11**.
  회신에 따라 출하 기본값을 `config`↔`auto`로 전환(코드 변경 불필요, 설정만).
