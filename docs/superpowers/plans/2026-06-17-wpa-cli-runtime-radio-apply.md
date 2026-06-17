# wpa_cli 런타임 무선설정 적용 (freq · essid) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SetRadioConfig 의 freq 와 ChangeIp 슬롯의 essid 를 conf 직접 rewrite 대신 `wpa_cli` 런타임 명령(set_network → save_config → reassociate)으로 변경·실시간 적용한다.

**Architecture:** OPC 적용 전용의 작은 셸 스크립트 `opc_wlan_apply.sh`(부모 `wlan-package`)를 신설하고, opcd(`wlan-opc` 서브모듈)의 nxp 백엔드가 기존 `wifi.sh freq`(conf awk-rewrite) 대신 이 스크립트를 호출한다. 명령의 책임은 "설정 변경 + reassociate 트리거"까지이고 재연결 성공 확인·롤백은 하지 않는다(결과는 기존 WlanStatusChange indication 이 통지). vtable 은 불변 — essid 는 이미 `opc_ipcfg_entry_t.essid` 로 `apply_ip_change` 까지 전달된다.

**Tech Stack:** C11 (opcd), POSIX sh + wpa_cli (스크립트), make (native/arm64, PLATFORM=stub|nxp).

**Repos / 작업 규칙:**
- 스크립트 = `wlan-package` repo: `dist/wlan/usr/local/scripts/opc_wlan_apply.sh`
- opcd 코드 = `wlan-opc` 서브모듈: `opcd/*`
- 두 repo 모두 **`master` 직접 커밋 금지** → 작업 브랜치(`feat/wpa-cli-runtime-apply`)에서. **커밋/푸시/PR 은 사용자 명시 승인 후** (프로젝트 규칙). 각 Task 의 commit 스텝은 그 전제로 실행.
- `platform_nxp.c` 는 `make check`(PLATFORM=stub)에서 **컴파일되지 않음** → nxp 변경은 `make arm64 PLATFORM=nxp`(cross 툴체인) 컴파일 + 실타깃으로 검증, handler-레벨 회귀는 stub 으로 검증.

**Spec:** `docs/superpowers/specs/2026-06-17-wpa-cli-runtime-radio-apply-design.md`

---

## File Structure

| 파일 | repo | 책임 | 변경 |
|---|---|---|---|
| `dist/wlan/usr/local/scripts/opc_wlan_apply.sh` | wlan-package | wpa_cli 로 freq/ssid 런타임 적용 + 영속 + reassociate | **신규** |
| `opcd/platform_nxp.c` | wlan-opc | 새 스크립트 상수 + 헬퍼, freq 경로 교체, essid 적용 추가 | 수정 |
| `opcd/platform_stub.c` | wlan-opc | apply_ip_change essid 관측 훅(테스트용) | 수정 |
| `opcd/tests/test_handler.c` | wlan-opc | essid 가 슬롯→apply_ip_change 로 전달되는지 검증 | 수정 |

대상 conf 전제(확인됨): `/etc/wpa_supplicant/wpa_supplicant-<iface>.conf` 에 `update_config=1`, `ctrl_interface=/var/run/wpa_supplicant`, 단일 `network={}` 블록(id 0).

---

## Task 1: 새 스크립트 `opc_wlan_apply.sh` (wlan-package)

**Files:**
- Create: `<wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh`

- [ ] **Step 1: 스크립트 작성**

`dist/wlan/usr/local/scripts/opc_wlan_apply.sh`:

```sh
#!/bin/sh
# opc_wlan_apply.sh — OPC 무선설정 런타임 적용 (wpa_cli)
#
# 사용: opc_wlan_apply.sh <iface> [--netid N] [freq "<mhz ...>"] [ssid <name>]
#   --netid N : 대상 network 블록 id (기본 0; 단일 블록 전제, 다중은 후속 확장)
#   freq/ssid : 하나 이상 지정. 둘 다면 한 번의 reassociate 로 묶어 끊김 1회.
#
# 책임: 설정 변경 + 적용(reassociate) 트리거까지. 재연결 성공 확인/롤백은 안 함
#       (결과는 WlanStatusChange indication 이 통지). freq/ssid 는 save_config 로 영속.
#
# exit: 0=ok / 2=usage / 3=ctrl_interface 부재 / 4=set_network 실패 / 5=save_config 실패
set -u

IFACE="${1:-}"
[ -n "$IFACE" ] || { echo "usage: $0 <iface> [--netid N] [freq \"<mhz ...>\"] [ssid <name>]" >&2; exit 2; }
shift

NETID=0
FREQS=""
SSID=""
HAVE_SSID=0
while [ $# -gt 0 ]; do
    case "$1" in
        --netid) NETID="${2:-0}"; shift 2 ;;
        freq)    FREQS="${2:-}"; shift 2 ;;
        ssid)    SSID="${2:-}"; HAVE_SSID=1; shift 2 ;;
        *)       echo "opc_wlan_apply: unknown arg '$1'" >&2; exit 2 ;;
    esac
done
[ -n "$FREQS" ] || [ "$HAVE_SSID" = 1 ] || { echo "opc_wlan_apply: nothing to apply (need freq and/or ssid)" >&2; exit 2; }

wc() { wpa_cli -i "$IFACE" "$@"; }
wc_ok() { [ "$(wc "$@" 2>/dev/null)" = "OK" ]; }

# ctrl interface 가용 확인 (wpa_supplicant 미동작이면 3).
wc ping >/dev/null 2>&1 || { echo "opc_wlan_apply: wpa_cli ctrl unavailable for $IFACE" >&2; exit 3; }

if [ -n "$FREQS" ]; then
    wc_ok set_network "$NETID" freq_list "$FREQS" || { echo "opc_wlan_apply: set freq_list failed" >&2; exit 4; }
    wc_ok set_network "$NETID" scan_freq "$FREQS" || { echo "opc_wlan_apply: set scan_freq failed" >&2; exit 4; }
fi
if [ "$HAVE_SSID" = 1 ]; then
    wc_ok set_network "$NETID" ssid "\"$SSID\"" || { echo "opc_wlan_apply: set ssid failed" >&2; exit 4; }
fi

# 비휘발 영속 (update_config=1; conf 재생성 — 주석/포맷 손실 허용).
wc_ok save_config || { echo "opc_wlan_apply: save_config failed" >&2; exit 5; }

# 적용 트리거 (결과 미확인, 비치명). 끊김 1회.
wc reassociate >/dev/null 2>&1 || echo "opc_wlan_apply: warn reassociate command failed (non-fatal)" >&2

exit 0
```

- [ ] **Step 2: 실행 비트 + 문법 검사**

Run:
```bash
chmod +x <wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh
sh -n <wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh && echo SYNTAX_OK
command -v shellcheck >/dev/null && shellcheck <wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh || echo "shellcheck 없음 — 스킵"
```
Expected: `SYNTAX_OK` 출력, shellcheck 경고 없음(또는 스킵).

- [ ] **Step 3: 인자 파싱 스모크 (wpa_cli 없이 usage/3 경로)**

Run:
```bash
sh <wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh; echo "exit=$?"        # usage → 2
sh <wlan-package>/dist/wlan/usr/local/scripts/opc_wlan_apply.sh mlan0; echo "exit=$?"  # nothing → 2
```
Expected: 각각 `exit=2` (usage/nothing-to-apply). wpa_cli 가 PATH 에 있으면 freq 지정 시 ctrl 부재로 `exit=3` — 빌드 머신엔 wpa_cli 가 없을 수 있으니 2 경로만 확인.

- [ ] **Step 4: Commit (사용자 승인 후)**

```bash
# wlan-package repo, feat/wpa-cli-runtime-apply 브랜치
git add dist/wlan/usr/local/scripts/opc_wlan_apply.sh
git commit -m "feat(scripts): opc_wlan_apply.sh — wpa_cli 런타임 무선설정 적용(freq/essid)"
```

---

## Task 2: platform_nxp.c — 상수 + 공용 헬퍼 `run_opc_wlan_apply`

**Files:**
- Modify: `opcd/platform_nxp.c` (상수: `:57` 인근 / 헬퍼: `run_wifi_sh_freq` 위 `:797` 인근)

- [ ] **Step 1: 새 스크립트 상수 추가**

`opcd/platform_nxp.c` 의 `#define WIFI_SH ...`(:57) 아래에 추가:

```c
#define OPC_WLAN_APPLY_SH        "/usr/local/scripts/opc_wlan_apply.sh"
/* 모든 단계(set_network/save_config/reassociate)가 wpa_cli OK 수신까지만(ms급)이라
 * 현 freq 동기 호출과 동일 예산으로 충분. reassociate 는 트리거만(연결 완료 대기 X). */
#define OPC_WLAN_APPLY_TIMEOUT_MS 900
```

- [ ] **Step 2: 헬퍼 함수 추가**

`run_argv_bounded` 정의가 끝나는 `:796` 아래(= `run_wifi_sh_freq` 위)에 추가:

```c
/* Run "opc_wlan_apply.sh <iface> [freq <mhz>] [ssid <name>]" synchronously.
 * Builds argv from the non-empty fields; freq_mhz==0 omits freq, ssid==NULL/""
 * omits ssid. The script does set_network → save_config → reassociate (trigger
 * only). All steps are wpa_cli round-trips (ms), so the bounded sync call stays
 * within budget. essid_buf/freq_buf must outlive the call — both are caller
 * stack here via the argv the caller passes; this builder keeps its own. */
static int run_opc_wlan_apply(const char *iface, uint16_t freq_mhz,
                              const char *ssid, long timeout_ms)
{
    char freq_buf[8];
    const char *argv[8];
    int n = 0;
    argv[n++] = "opc_wlan_apply.sh";
    argv[n++] = iface;
    if (freq_mhz != 0) {
        snprintf(freq_buf, sizeof freq_buf, "%u", freq_mhz);
        argv[n++] = "freq";
        argv[n++] = freq_buf;
    }
    if (ssid && ssid[0] != '\0') {
        argv[n++] = "ssid";
        argv[n++] = ssid;
    }
    argv[n] = NULL;
    return run_argv_bounded("opc_wlan_apply", OPC_WLAN_APPLY_SH,
                            (char *const *)argv, timeout_ms);
}
```

- [ ] **Step 3: cross 컴파일 확인**

Run:
```bash
cd <wlan-opc>; make arm64 PLATFORM=nxp 2>&1 | tail -20
```
Expected: 컴파일 성공, 경고 없음. (cross 툴체인 부재 시: 이 Task 는 다음 Task 들과 함께 Task 4 끝에서 일괄 빌드 검증 — 단독으로는 미사용 함수 경고가 날 수 있으므로 Task 3 와 묶어 검증 권장.)

- [ ] **Step 4: Commit (사용자 승인 후)** — Task 3 와 함께 묶어 커밋 가능. 단독 시:

```bash
cd <wlan-opc>
git add opcd/platform_nxp.c
git commit -m "feat(opcd): opc_wlan_apply.sh 호출 헬퍼 + 상수 추가"
```

---

## Task 3: nxp_apply_radio_config — `wifi.sh freq` → 새 스크립트로 교체

**Files:**
- Modify: `opcd/platform_nxp.c` (`nxp_apply_radio_config` :844-867, `run_wifi_sh_freq` :803-812 제거)

- [ ] **Step 1: freq 적용 호출 교체**

`nxp_apply_radio_config`(:814) 안의 두 `run_wifi_sh_freq` 호출을 교체:

```c
    /* (mlan0) — 기존: run_wifi_sh_freq("mlan0", cfg->wlan1.freq_mhz, per_call_ms) */
    if (cfg->wlan1.freq_mhz != 0) {
        int rc = run_opc_wlan_apply("mlan0", cfg->wlan1.freq_mhz, NULL, per_call_ms);
        if (rc != 0) return rc;
    }
```
```c
    /* (mlan1, DUAL) — 기존: run_wifi_sh_freq("mlan1", cfg->wlan2.freq_mhz, per_call_ms) */
        int rc = run_opc_wlan_apply("mlan1", cfg->wlan2.freq_mhz, NULL, per_call_ms);
        if (rc != 0) return rc;
```

- [ ] **Step 2: 죽은 `run_wifi_sh_freq` 제거**

`run_wifi_sh_freq`(:798 주석 포함 ~ :812) 함수를 삭제. `WIFI_SH` 상수가 이 파일 내 다른 사용처가 없으면 함께 제거:

Run (사용처 확인):
```bash
cd <wlan-opc>; rg -n 'run_wifi_sh_freq|WIFI_SH\b' opcd/platform_nxp.c
```
Expected: 호출/참조 0건이면 `run_wifi_sh_freq` 와 `WIFI_SH`/`WIFI_SH_TIMEOUT_MS`/`WIFI_SH_POLL_MS` 중 미사용 항목 제거. (`WIFI_SH_POLL_MS`/`WIFI_SH_TIMEOUT_MS` 가 `run_argv_bounded` 등에서 쓰이면 유지 — rg 로 확인 후 결정.)

- [ ] **Step 3: cross 컴파일 + 회귀(stub) 확인**

Run:
```bash
cd <wlan-opc>; make arm64 PLATFORM=nxp 2>&1 | tail -20 && make check 2>&1 | tail -15
```
Expected: nxp 빌드 무경고, `make check` FAIL 0 (freq 의 handler-레벨 동작은 apply_radio_config 호출로 동일 — 회귀 없음).

- [ ] **Step 4: Commit (사용자 승인 후)**

```bash
cd <wlan-opc>
git add opcd/platform_nxp.c
git commit -m "feat(opcd): SetRadioConfig freq 적용을 wifi.sh→opc_wlan_apply.sh(wpa_cli 런타임)로 전환"
```

---

## Task 4: nxp_apply_ip_change — essid 적용 추가 (비휘발)

**Files:**
- Modify: `opcd/platform_nxp.c` (`nxp_apply_ip_change` :943-945 `return ret;` 직전)

- [ ] **Step 1: essid 적용 블록 추가**

`nxp_apply_ip_change`(:897) 의 IP 적용 직후, `return ret;`(:945) 바로 위에 추가:

```c
    /* essid (비휘발): IP/netmask(휘발) 와 독립. 슬롯에 essid 가 있으면 wpa_cli 로
     * 런타임 변경 + save_config 영속. DUAL 은 mlan0(관리)을 대상으로 한다.
     * best-effort: essid apply 실패는 로그만 — IP 적용 결과(ret)를 반환한다.
     * (spec §7: essid 실패 전용 에러코드는 미정. 재연결 결과는 WlanStatusChange
     *  indication 이 통지하므로 silent 아님.) slot->essid 는 0x0016 으로 NUL 종단
     * 검증됨이나 방어적으로 bound copy 한다. */
    if (slot->essid[0] != '\0') {
        char essid_buf[OPC_ESSID_FIELD_LEN + 1];
        snprintf(essid_buf, sizeof essid_buf, "%.*s",
                 (int)sizeof slot->essid, slot->essid);
        int erc = run_opc_wlan_apply("mlan0", 0, essid_buf, OPC_WLAN_APPLY_TIMEOUT_MS);
        fprintf(stderr, "opcd: nxp_apply_ip_change: essid='%s' apply%s\n",
                essid_buf, erc == 0 ? " (wpa_cli)" : " FAILED");
    }
    return ret;
```

`OPC_ESSID_FIELD_LEN` 는 `protocol/commands.h:177`(=32). `platform_nxp.c` 가 이미 `protocol` 헤더를 포함(`platform.h`→commands.h 경유)하는지 확인하고, 아니면 include 추가:

Run:
```bash
cd <wlan-opc>; rg -n 'OPC_ESSID_FIELD_LEN|commands\.h' opcd/platform_nxp.c opcd/platform.h
```
Expected: `OPC_ESSID_FIELD_LEN` 가 컴파일 시점에 가시(미가시면 `#include "../protocol/commands.h"` 추가).

- [ ] **Step 2: cross 컴파일 확인**

Run:
```bash
cd <wlan-opc>; make arm64 PLATFORM=nxp 2>&1 | tail -20
```
Expected: 무경고 컴파일.

- [ ] **Step 3: Commit (사용자 승인 후)**

```bash
cd <wlan-opc>
git add opcd/platform_nxp.c
git commit -m "feat(opcd): ChangeIp essid 적용 추가 — opc_wlan_apply.sh ssid(비휘발), IP는 휘발 유지"
```

---

## Task 5: essid 전달 검증 (stub 관측 훅 + handler 테스트, TDD)

**Files:**
- Modify: `opcd/tests/test_handler.c` (change-ip 테스트 인근 :431-446, extern 선언 :41-44)
- Modify: `opcd/platform_stub.c` (`stub_apply_ip_change` :185-196)

- [ ] **Step 1: 실패 테스트 작성**

`opcd/tests/test_handler.c` 의 extern 블록(:41-44 인근)에 추가:

```c
extern const char *stub_apply_ip_last_essid(void);
```

기존 change-ip → logout → apply 검증 테스트(`stub_apply_ip_last_ip()==...` 가 있는 블록, :445-446 인근) 바로 뒤에 essid 검증 assert 추가. 해당 테스트가 슬롯에 essid 를 싣도록(SetIPConfigList 단계에서 essid 필드 설정) 픽스처를 보강하고:

```c
    /* essid 도 같은 슬롯으로 apply_ip_change 까지 전달되어야 한다(비휘발 적용 대상). */
    ASSERT(strcmp(stub_apply_ip_last_essid(), "cantops-x") == 0,
           "change-ip: apply gets committed slot essid");
```

(슬롯 essid 설정 방법: 해당 테스트가 SetIPConfigList 요청을 만드는 헬퍼에 `essid="cantops-x"` 를 넣는다 — 기존 IP 설정과 동일 슬롯/요청에 essid 필드를 채운다. `#include <string.h>` 가 이미 있으면 strcmp 사용 가능, 없으면 추가.)

- [ ] **Step 2: 테스트 빌드 실패 확인**

Run:
```bash
cd <wlan-opc>; make check 2>&1 | tail -20
```
Expected: 링크 실패 — `undefined reference to 'stub_apply_ip_last_essid'` (accessor 미정의).

- [ ] **Step 3: stub 관측 훅 구현**

`opcd/platform_stub.c` 의 essid 정적 + 기록/accessor 추가. `s_apply_ip_last_ip`(:183) 옆:

```c
static char     s_apply_ip_last_essid[OPC_ESSID_FIELD_LEN + 1] = {0};
```

`stub_apply_ip_change`(:185) 본문에 기록 추가:

```c
static int stub_apply_ip_change(const opc_ipcfg_entry_t *slot)
{
    s_apply_ip_calls++;
    s_apply_ip_last_ip = slot->ip_address;
    snprintf(s_apply_ip_last_essid, sizeof s_apply_ip_last_essid, "%.*s",
             (int)sizeof slot->essid, slot->essid);
    return s_apply_ip_fail ? -1 : 0;
}
```

accessor + reset 갱신(:193-195 인근):

```c
const char *stub_apply_ip_last_essid(void) { return s_apply_ip_last_essid; }
```
`stub_apply_ip_reset` 에 `s_apply_ip_last_essid[0] = '\0';` 추가:

```c
void stub_apply_ip_reset(void) { s_apply_ip_calls = 0; s_apply_ip_last_ip = 0;
                                 s_apply_ip_last_essid[0] = '\0'; s_apply_ip_fail = 0; }
```

`platform_stub.c` 에 `OPC_ESSID_FIELD_LEN`/`snprintf` 가시 확인(commands.h, `<stdio.h>`). 미가시면 include 추가.

- [ ] **Step 4: 테스트 통과 확인**

Run:
```bash
cd <wlan-opc>; make check 2>&1 | tail -20
```
Expected: PASS — "change-ip: apply gets committed slot essid" 포함 전체 통과, FAIL 0.

- [ ] **Step 5: Commit (사용자 승인 후)**

```bash
cd <wlan-opc>
git add opcd/platform_stub.c opcd/tests/test_handler.c
git commit -m "test(opcd): ChangeIp essid가 슬롯→apply_ip_change로 전달되는지 검증 + stub essid 관측 훅"
```

---

## Task 6: 통합 빌드 검증 + spec/문서 동기화

**Files:**
- (검증 전용) + Modify: `docs/spec-conformance.md` 또는 `docs/implementation-status.md` 의 1.1/1.2 항목 상태 갱신

- [ ] **Step 1: 전체 빌드/테스트**

Run:
```bash
cd <wlan-opc>
make check 2>&1 | tail -15          # stub 회귀: FAIL 0
make arm64 PLATFORM=nxp 2>&1 | tail -20   # nxp cross: 무경고
```
Expected: `make check` FAIL 0, nxp 빌드 무경고.

- [ ] **Step 2: 추적 문서 갱신**

`docs/implementation-status.md` 의 1.2(ChangeIp essid apply) 를 "구현(wpa_cli 런타임, 비휘발)"으로, 1.1 의 freq 적용 경로를 "opc_wlan_apply.sh(wpa_cli)로 전환, mode/bw 는 여전히 미반영"으로 갱신. (gitignore 인 `proto-todo.md` 가 아니라 tracked 문서에 기록 — 프로젝트 규칙.)

- [ ] **Step 3: Commit (사용자 승인 후)**

```bash
cd <wlan-opc>
git add docs/implementation-status.md
git commit -m "docs(opcd): wpa_cli 런타임 freq/essid 적용 반영 — impl-status 1.1/1.2 갱신"
```

---

## 실타깃 검증 (수기, on-target)

`make check`/cross 빌드로는 wpa_cli 실동작을 못 본다. 실타깃에서:
1. 배포 후 `wpa_cli -i mlan0 ping` 로 ctrl 가용 확인.
2. SetRadioConfig(freq) 발행 → `wpa_cli -i mlan0 get_network 0 freq_list` 가 갱신됐는지 + `/etc/wpa_supplicant/wpa_supplicant-mlan0.conf` 에 save_config 반영(영속) 확인 + reassociate 로 재연결(WlanStatusChange indication 수신).
3. ChangeIp(essid 포함 슬롯) → Logout → `get_network 0 ssid` 갱신 + conf 영속 + IP 는 휘발(reboot 시 복귀) 확인.
4. 잘못된 freq(해당 AP 없음) → ack 는 OK(설정 반영), 재연결 실패는 WlanStatusChange 로 통지됨을 확인(명령 책임 경계 검증).

---

## Self-Review

**Spec coverage:**
- §2 책임경계(연결확인/롤백 없음) → Task 1 스크립트(reassociate 비치명) + Task 4 best-effort. ✓
- §3 예산(ms급 동기) → Task 2 헬퍼 동기 호출, 900ms. ✓
- §4 스크립트 인터페이스/exit → Task 1. ✓
- §5 배선(freq/essid) → Task 3 / Task 4. ✓
- §6 영속성(freq·essid save_config / IP 휘발) → Task 1 save_config + Task 4 IP 불변. ✓
- §7 에러/D9 → Task 1 exit 매핑 + Task 4 best-effort(에러코드 미정 명시). ✓
- §8 (a)주석손실 허용 / (b)netid 기본0·단일블록 → Task 1 `--netid` 기본 0. ✓
- §9 테스트 → Task 5(stub TDD) + Task 6(빌드) + 실타깃 절. ✓

**Placeholder scan:** 코드 스텝 모두 실제 코드 포함. 단 (i) Task 5 의 SetIPConfigList essid 픽스처 보강은 기존 테스트 헬퍼에 의존 — 실행 시 해당 헬퍼 시그니처 확인 후 essid 필드 설정, (ii) cross 툴체인 부재 시 nxp 빌드 검증은 실타깃으로 이연. 두 가지는 환경 의존이라 의도적으로 표기.

**Type consistency:** `run_opc_wlan_apply(const char*, uint16_t, const char*, long)` — Task 2 정의, Task 3/4 호출 일치. `stub_apply_ip_last_essid(void)→const char*` — Task 5 정의/사용 일치. `OPC_ESSID_FIELD_LEN`(=32) — commands.h, stub/nxp 동일 사용. ✓
