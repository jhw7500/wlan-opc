# Handoff: opc_wlan_apply.sh 적용 방식 전환 (wlan-package → wlan-opc 후속 처리)

- 날짜: 2026-06-17
- 원천 변경: wlan-package PR **#49** (`feat/wpa-cli-reconfigure-apply`)
  https://github.com/jhw7500/wlan-package/pull/49
- 작성: wlan-package 세션 (peer 핸드오프)

## 무엇이 바뀌었나 (wlan-package 측, 완료·검증)

`opc_wlan_apply.sh`의 무선설정 적용 방식을 전환:

- **기존**: wpa_cli `set_network`(freq_list/scan_freq/ssid) → `save_config` → `reassociate`
- **변경**: wpa_supplicant conf **파일 직접편집(awk)** → `wpa_cli reconfigure`

이유:
- wpa_supplicant v2.10의 `save_config`가 `freq_list`를 직렬화하지 않아 영속 실패 + conf 주석/포맷 손실 (실타겟 검증)
- `reassociate`는 현재 BSS를 유지한 채 `freq_list`를 재평가하지 않아 freq 변경 미반영
- conf 직접편집으로 `freq_list` 영속(하드 밴드 락) + `reconfigure`로 freq 변경 확실 반영

`vhld.c`(wlan-package)도 동일하게 `set_network`+`reassociate` → `opc_wlan_apply.sh` 위임으로 전환됨.

**CLI 계약은 불변**: `opc_wlan_apply.sh <iface> [--netid N] [freq "<mhz ...>"] [ssid <name>]`.
`platform_nxp.c`의 `run_opc_wlan_apply` 호출(freq=MHz, exit 0/비-0)은 **코드 변경 없이 그대로 동작**.

---

## wlan-opc 측 후속 처리 필요 (이 세션에서)

### 1. [문서] `opcd/platform_nxp.c` `run_opc_wlan_apply` 헤더 주석 갱신 (≈L800-804)
현재: `"The script does set_network -> save_config -> reassociate (trigger only)."`
→ 변경: `"conf 파일 직접편집(ssid/scan_freq/freq_list) 후 wpa_cli reconfigure 로 적용+영속."`

### 2. [문서] `docs/implementation/implementation-status.md` 갱신
- 1.1 freq 적용 경로: "opc_wlan_apply.sh — set_network freq_list/scan_freq → save_config → reassociate"
  → "opc_wlan_apply.sh — conf 직접편집(freq_list/scan_freq) → wpa_cli reconfigure"
- 1.2 essid 적용: "opc_wlan_apply.sh ssid → save_config → reassociate"
  → "opc_wlan_apply.sh ssid(conf 편집) → reconfigure"
- "reassociate 로 즉시 적용 / conf-rewrite 시절 reconnect 지연 해소" 서술 →
  "reconfigure(전체 conf 재로드)로 적용 + 영속, 끊김 가능(실기 측정 필요)" 로 정정

### 3. [코드 검토] opcd essid 입력 검증 (defense-in-depth)
- essid가 `opc_wlan_apply.sh ssid` 인자로 전달될 때, SSID에 `"` 나 `\` 가 있으면 conf 라인 인젝션 위험.
- **1차 방어는 완료**: `opc_wlan_apply.sh`가 awk에서 `\`·`"` 를 wpa_supplicant C-escape 문법으로 이스케이프(검증 통과). 호출처가 무필터여도 conf는 안전.
- 권장: opcd essid 입력단(SET_WLAN_CONFIG/0x0016 류 핸들러)에서 SSID 길이/문자 검증이 있는지 확인. (참고: wlan-package `vhld.c`는 ASCII printable 검증 있으나 `"`·`\` 는 통과 — awk 이스케이프로 커버)

---

## 검증 상태 (wlan-package 측)
- `opc_wlan_apply.sh`: `sh -n`·shellcheck clean, mock 테스트 통과 — freq/ssid/멀티블록/주석보존/SSID 이스케이프(따옴표·백슬래시·혼합)/reconfigure 롤백/에러
- `vhld.c`: `-Werror -Wextra` 빌드 OK

## 실기(NXP 9098) 확인 권장
1. `reconfigure` 실제 끊김 시간 (크면 freq_list 런타임 전용 방식으로 후속 전환 검토)
2. 타겟 awk(busybox 시) `ENVIRON`/`function`/`[[:space:]]` 지원 — 단 기존 wifi.sh가 동일 패턴으로 동작 중
3. wpa_supplicant가 `ssid="...\"..."` C-escape를 올바르게 파싱하는지
