# wpa_cli 런타임 무선설정 적용 설계 (freq · essid)

- **작성일**: 2026-06-17
- **상태**: 설계 확정 (사용자 승인) → 구현 계획 대기
- **범위**: SetRadioConfig 의 freq, ChangeIp 슬롯의 essid 를 conf 직접 rewrite 대신 `wpa_cli` 런타임 명령으로 변경·실시간 적용
- **범위 밖**: mode/bw 실반영(드라이버 mlanutl, 별도), IP/netmask 적용 방식(현 `ip addr` 유지), 다중 network 블록, 재연결 성공 확인·롤백

---

## 1. 배경 / 문제

- 현재 SetRadioConfig(freq) 적용 경로 = `nxp_apply_radio_config` → `wifi.sh <iface> freq <mhz>` → conf 의 `freq_list`/`scan_freq` 를 **awk 로 rewrite**(persist-only). `reassociate` 를 의도적으로 하지 않아(`platform_nxp.c:800-802`) **다음 자연 reconnect 까지 무선에 미반영**.
- essid 는 IPConfigList(0x1002) 슬롯 필드로 `iplist.cfg` 에 저장되나 **apply 시 무시**(`nxp_apply_ip_change` 는 IP/netmask 만; 미구현 V3).
- 목표: conf 파일을 직접 편집하는 대신 **`wpa_cli` 로 런타임 변경 + 실시간 적용**, wpa_supplicant **프로세스를 종료하지 않고 끊김 최소화**.

## 2. 책임 경계 (핵심 결정)

- 무선설정변경 명령의 책임 = **설정을 변경하고 적용(reassociate)을 트리거**하는 것까지.
- **변경된 설정으로 재연결이 성공했는지 확인하는 것은 명령의 책임이 아니다.** (연결 성공/실패는 해당 채널의 AP 존재 등 환경에 의존)
- 따라서 **연결 성공 확인·실패 롤백을 하지 않는다.**
- `ack` 의 의미 = `set_network` + `save_config` 성공 여부 (= 설정이 wpa_supplicant 메모리와 conf 에 반영됨). **재연결 결과는 ack 에 싣지 않는다.**
- 재연결 성공/실패는 **이미 구현된 WlanStatusChange indication**(nl80211 producer, V1)이 별도로 통지한다.

## 3. 응답 예산 (사양)

- ① 참조계 / 비휘발 저장 불요 = **1초**
- ② 설정계 / 비휘발 저장 필요 = **2분(120초)**
- 무선설정변경은 비휘발 저장(save_config)을 동반하므로 사양상 ② **2분 클래스**.
- 단 `set_network` / `save_config` / `reassociate` 는 모두 wpa_cli 에 명령을 보내고 `OK` 수신까지만 동작(연결 완료를 기다리지 않음) → **전부 ms 급**. 따라서 2분 예산을 쓸 필요 없이 **동기 호출로 충분**(현 `wifi.sh freq` 900ms 와 동급). 단일 스레드 recv 루프의 블로킹은 무시 가능 수준이며, deferred-ack(store_async)·2분 timeout 불필요.

## 4. 새 스크립트 — `opc_wlan_apply.sh`

- **경로**: `/usr/local/scripts/opc_wlan_apply.sh` (1808줄 다목적 `wifi.sh` 와 분리된, OPC 적용 전용의 작고 집중된 스크립트)
- **전제**: 타깃 conf 에 `update_config=1`, `ctrl_interface=/var/run/wpa_supplicant` (확인됨)

### 인터페이스

```
opc_wlan_apply.sh <iface> [--netid N] [freq "<mhz ...>"] [ssid "<name>"]
  --netid N : 대상 network 블록 id (기본 0; 현재 단일 블록 전제, 다중은 후속 확장)
  freq/ssid : 둘 중 하나 이상 지정 (둘 다 지정 시 한 번의 reassociate 로 묶어 끊김 1회)
```

### 동작 시퀀스

```
1) freq 지정 시:
     wpa_cli -i <iface> set_network N freq_list "<f ...>"
     wpa_cli -i <iface> set_network N scan_freq "<f ...>"
   ssid 지정 시:
     wpa_cli -i <iface> set_network N ssid "\"<name>\""
2) wpa_cli -i <iface> save_config        # 비휘발 영속 (update_config=1; conf 재생성 — 주석/포맷 손실 허용)
3) wpa_cli -i <iface> reassociate        # 새 설정으로 즉시 재연결 트리거 (결과 미확인, fire-and-forget)
```

### 종료 코드 (exit)

| code | 의미 | opcd 처리 |
|---|---|---|
| 0 | 정상 (set_network + save_config 성공) | OK ack |
| 2 | usage/인자 오류 | NG (내부 오류) |
| 3 | ctrl_interface/환경 부재 (wpa_supplicant 미동작 등) | NG (apply 실패) |
| 4 | set_network 실패 | NG (apply 실패) |
| 5 | save_config 실패 | NG (apply 실패) |

- **reassociate(3단계) 실패는 비치명**: 설정은 이미 반영·영속됐으므로 ack 는 OK. reassociate 명령 전송 실패는 경고 로그만 남기고, 재연결 상태는 WlanStatusChange indication 이 통지(§2).

## 5. opcd 배선

- **freq** → `nxp_apply_radio_config` (`platform_nxp.c`): 기존 `run_wifi_sh_freq`(wifi.sh freq, conf awk-rewrite) 호출을 **새 스크립트 freq 호출로 대체**. DUAL 은 mlan0/mlan1 각각, freq=0 은 적용 스킵(현행 의미 유지).
- **essid** → `nxp_apply_ip_change` (`platform_nxp.c`): IP/netmask 는 현행 `ip addr`(휘발) 유지하고, **슬롯의 essid 가 비어있지 않으면 새 스크립트 ssid 호출(비휘발)** 을 추가. IP 적용과 essid 적용은 독립(한쪽 실패가 다른쪽을 막지 않도록 순서·에러처리 정의).
- 새 스크립트 실행은 기존 `run_argv_bounded` 패턴 재사용(bounded timeout, SIGKILL 가드, exit code 판독).

## 6. 영속성

| 항목 | 명령 | 영속성 | 메커니즘 |
|---|---|---|---|
| freq | SetRadioConfig 0x1004 | **비휘발(영속)** | set_network + `save_config` |
| essid | ChangeIp 0x1003 (슬롯 필드) | **비휘발(영속)** | set_network + `save_config` |
| IP / netmask | ChangeIp 0x1003 | **휘발** (사양: 전원 재투입 시 컨피그 값 복귀) | `ip addr` (현행 불변) |

## 7. 에러 / D9 정합

- 새 스크립트 exit(3/4/5)는 apply 런타임 실패 → 현행 D9 의 `OPC_ERR_RADIO_APPLY=0x0050`(발주처 확인 대기, #35) 경로와 매핑 정리. freq apply 실패 시 D9 의 deferred revert(last-good 재적용) 정책과 충돌하지 않도록 조정.
- essid apply 실패의 에러코드(별도 필요 여부)는 ChangeIp 핸들러 에러 모델에 맞춰 정의.

## 8. 결정 사항 / 유의점

- **(a) save_config 주석·포맷 손실 허용** — wpa_supplicant 가 conf 를 재생성하므로 수동 주석/정렬이 사라질 수 있음. "conf 직접 수정 대신 wpa_cli" 방침상 **허용**(사용자 확정).
- **(b) network id** — `--netid` 로 지정 가능, **기본 0**. 현재 conf 는 단일 `network={}` 블록이므로 0 고정으로 충분. **다중 블록 대상 선정 규칙은 후속 확장**으로 미룸(YAGNI).

## 9. 테스트

- stub 플랫폼: 기존 `stub_apply_radio_config` 관측 훅 + essid apply 관측 훅 추가(호출 인자/횟수 기록).
- 핸들러 테스트: SetRadioConfig freq → 새 스크립트 인자 검증, ChangeIp essid → ssid 호출 검증, IP 휘발/essid 비휘발 분리 검증.
- exit code → ack(OK/NG) 매핑 테스트(2/3/4/5 각 경로, reassociate 실패 비치명 확인).
- 빌드: `make check` FAIL 0, arm64 PLATFORM=nxp 무경고.

## 10. 범위 밖 (YAGNI)

- mode / bandwidth 실반영 (드라이버 `mlanutl htcapinfo/vhtcfg` + reassociate; wifi.sh radio-apply 에 기구현, opcd 미연결 — 별도 작업).
- 재연결 성공 확인 · 실패 롤백 (§2 책임 경계).
- 다중 network 블록 대상 선정.
- IP/netmask 적용 방식 변경 (현 `ip addr` 유지).
