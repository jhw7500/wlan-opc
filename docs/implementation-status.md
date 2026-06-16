# wlan-opc 미구현 항목 현황 (Implementation Status)

**조사일**: 2026-06-16 · **방법**: 코드 마커 grep(no-op/V3/pending/canned/TODO/deferred) + `spec-conformance.md` + proto-todo 전수 교차 (16건 발견 → 묶음 정리)
**분류**: ① 자율 구현 가능(외부 대기 없음) · ② 발주처/벤더/spec 대기 · ③ 구현됨·실측만(미구현 아님)
**항목번호**: `spec-conformance.md`의 V#/D#/③-B, proto-todo의 T# 참조

---

## ① 자율 구현 가능 (외부 대기 없음)

### 1.1 SetRadio mode/bw/channel 실드라이버 미반영 — V2/V12 · partial-impl · **L**
- **현황**: `nxp_apply_radio_config`가 **freq만** `run_wifi_sh_freq`로 wpa_supplicant에 반영. mode(11n/ac/ax)·bandwidth(20/40/80/160)·channel은 **stderr 로깅만** 하고 드라이버 미반영. wpa_supplicant 재시작도 안 해 다음 reconnect까지 채널 전환 지연.
- **파일**: `platform_nxp.c:811-864` (주석 839-840 "Mode / bandwidth mapping is deferred to a follow-up PR"), `:795-809`
- **막힘**: `wifi.sh`가 mode/bw 서브커맨드 미지원 → 드라이버 매핑 + wifi.sh 확장 + reconnect 트리거 설계 필요
- **부수**: DUAL에서 mlan0 성공·mlan1 실패 시 멱등 복구 없음 (운영자 재발행 의존, `:845-862`)

### 1.2 ChangeIp ESSID/NTP apply 미구현 — V3 · essid **M** / ntp **S**
- **현황**: `nxp_apply_ip_change`는 IP/netmask만 `ip addr`로 적용. **essid 적용 로직 전무**(code-absent), ntp는 읽기(`nxp_get_ntp_server`)만 있고 write/재시작 apply 없음.
- **파일**: `platform_nxp.c:875-940` (주석 886-890 "essid/ntp remain V3 on-target work (wifi_init.sh routing / wpa_supplicant)")
- **막힘**: essid = wpa_supplicant conf 재구성 시 활성 링크 드롭 정책 미정 / ntp = timesyncd.conf write+재시작 on-target 검증 대기
- **비고**: essid **읽기** fallback(device-info용, nl80211 GET_INTERFACE)은 2026-06-16 구현됨(V4 별개). 여기는 **쓰기**(change-ip가 SSID를 실제로 바꾸는 것). set-ip-list의 essid 필드는 iplist.cfg에 저장되나 apply 시 무시됨.

### 1.3 Protocol Version 협상 + EEPROM identity — V7 · code-absent · **M**
- **현황**: codec이 protocol version을 read/write만, **버전 검증/협상 전무**. static identity(vendor/product/serial/hw)는 `device_info.json` 로드뿐 — **EEPROM/hostcmd 직접 읽기 미구현**.
- **파일**: `protocol/codec.c:37,59`, `platform_nxp.c:449-450`(주석 "canned" stale — 실로직은 inventory.c JSON), `inventory.c:119`
- **막힘**: 멀티버전 상호운용 요구 미확정 / EEPROM·hostcmd 인터페이스 미설계(현재 JSON config로 대체 운용)

### 1.4 AP_DISCONNECT 구분 / EHT 매핑 / 스텔스 빈SSID — code-absent · **M/S**
- **(a) Disassoc/Deauth 구분 불가**: nl80211 CMD_DISCONNECT에서 raw 802.11 프레임 미디코드 → 항상 Deauthentication(0x000C)으로 보고. `platform_nxp.c:1147-1169`
- **(b) EHT(802.11be) 모드 매핑 부재**: `parse_bitrate_to_mode`가 EHT-MCS tx_bitrate에 -EINVAL, OPC enum 없음(11ax까지만 매핑). `:138-152`
- **(c) 스텔스 빈SSID NULL 처리 미작성**(V4): 빈 SSID 분기 없음 (essid 읽기 fallback과 별개)

---

## ② 발주처/벤더/spec 대기 (자율 진행 불가)

| 항목 | 미구현 내용 | blocker | 파일 |
|---|---|---|---|
| **T9** 자율리셋 producer | 배관(emitter `opcd_ind_reset_notice` / consumer / `OPCD_PEVT_RESET_NOTICE` enum)은 완비, **자율 트리거 코드·cause ID 부재**(user reset만 존재) | #35 — 트리거 조건(watchdog/fault)·자율 cause 값 미정 | `platform_nxp.c:1218`, `indication.c:81`, `opcd.c:112`, `ids.h:139` |
| **A5** 무효문자 검증 | SetPassword/Login/ESSID 모두 NUL종단만 검사, **문자집합(무효문자) 검증 없음**(0x0015 UNUSED) | #35 — '무효문자' 정의가 사양에 부재 | `handler.c:591-632`(SetPassword), `:350-404`(Login) |
| **wlan_id** Dual 구분 | indication에 wlan_id 필드 없어 **mlan1(idx≠0) 이벤트 드롭** → Dual WLAN#1/#2 구분 불가(잠정 mlan0) | #35 — dual 발행 정책 + 실 dual-radio HW | `opcd.c` on_platform_event |
| Memory(0x0002) | swapless 타깃이라 사양 정의(페이징) 성립 불가 → **의도적 미발행**(Disk I/O로 일원화) | 확정(코드 변경 불요) | `fault_probe.c` |
| capability 비트 | `device_info.json` 정적값(전부 1=추정) vs silicon 실광고 미검증 | #35 — 고객사 문의 | `inventory.c` |
| 0x0018 / gateway | 0x0018(비정상 boundary NG) 벤더 확정 대기 / gateway는 #27로 **의도적 미적용**(브릿지) | #35(A17) / #27(확정) | `handler.c`, `platform_nxp.c:886-889` |

---

## ③ 참고 — 구현은 됨, "실측만" 남음 (미구현 아님)
- **V1** Roaming/ApDisconnect (#47 — WlanStatusChange는 2026-06-16 와이어 캡처 검증)
- **V10** NVRAM 120s fsync 예산(i.MX8MM eMMC) · **V11 power-cycle IP 복원 (2026-06-16 reboot로 검증됨)**
- **V13~V17** (규제 NG 실동작 / ESSID 길이 경계 / mlan0 라우팅 소유 / DUAL 검출 / idle 경계)
- `nxp_prepare_reset` no-op은 vtable 계약상 허용(필요성 미확정) · **V8** IP-only 세션 식별은 운영정책(SEC-001, 코드 범위 밖)

---

## 우선순위 요약
- **임팩트 순(자율)**: ① SetRadio 실반영(L) > ② ChangeIp essid/ntp(M/S) > ③ Version/EEPROM(M) > ④ AP구분/EHT/스텔스(M/S)
- **②는 #35 발주처 답변** 의존 — 답변 수신 시 T9 producer·A5 검증·wlan_id는 배관 완비라 소규모 추가로 마무리 가능
- 코드 스냅샷 기준 라인은 후속 커밋으로 drift 가능 — 착수 시 재확인
