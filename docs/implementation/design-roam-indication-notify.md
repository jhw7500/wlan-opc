# Roaming Indication — 로밍 실행체 통지 방식 설계

> 상태: **설계(검토 대기)** · 2026-07-06 · 실타겟 cts-wlan 조사 기반
> 대상: opcd Roaming Indication(0x04) 발행을 **로밍을 실제 수행하는 호스트 데몬의 통지**로 구동한다.

## 1. 배경 / 문제

opcd의 Roaming Indication(0x04, SNR/RSSI/AP MAC/CH)은 nl80211 `NL80211_CMD_ROAM(47)`에만
반응한다(`opcd/nl80211_parse.c:93`, `opcd/opcd.c:95` `OPCD_PEVT_ROAMING`). 그러나 이 보드
(NXP 88W9098, mlan/moal, **userspace SME**)에서 로밍은:

- host-based(`wifi_roam.py` / `passive_roam.py`)로 `wpa_cli roam`을 호출 → 커널이 이를
  **`CMD_ASSOCIATE(38)` + `CMD_CONNECT(46)`** 로 보고(`CMD_ROAM` 아님, "Ignore connect event
  when using userspace SME"). ⇒ **opcd Roaming(0x04)이 구조적으로 발행되지 않음.**
- opcd가 nl80211 `CONNECT`에서 BSSID 변화로 로밍을 추정하는 방안은 **재연결(link loss →
  다른 AP 재접속)과 구분 불가**(실측: 로밍과 재연결이 동일하게 WlanStatusChange
  DISCONNECTED→CONNECTED만 냄). 즉 nl80211 계층엔 "로밍 의도"가 없다.

**권위 있는 로밍 정보는 로밍을 결정·실행하는 호스트 데몬만 갖는다** — 자기가 트리거했고,
페이로드(대상 AP MAC/RSSI/SNR/채널)도 이미 손에 쥐고 있으며, 재연결은 이 경로를 거치지 않는다.

## 2. 접근

로밍 실행체가 로밍 완료 시 **opcd에 로밍 통지**를 보내고, opcd는 이를 기존
`OPCD_PEVT_ROAMING` 이벤트로 합성해 **기존 `opcd_ind_roaming()`** 으로 Roaming(0x04)을 발행한다.
opcd 인디케이션 코드는 사실상 무수정 — **입력 소스만 추가**한다.

### opcd 측이 이미 갖춘 것 (조사 결과)
- `opcd/platform.h`: `OPCD_PEVT_ROAMING` + `opcd_platform_evt_t.u.roaming{snr,rssi,mac,channel,idx}`
- `opcd/indication.c`: `opcd_ind_roaming(snr,rssi,mac,ch)` (bit `OPC_IND_BIT_ROAMING=0x04` gate)
- `opcd/opcd.c`: **단일 epoll 루프**가 `udp_fd/sig_fd/timer_fd/evt_fd/store_fd`를 멀티플렉싱.
  `evt_fd`(nl80211)는 `platform_drain_events()`로 드레인. → **로밍 통지 fd를 epoll에 하나 더 추가**하면 됨.

## 3. 스코프

| 로밍 경로 | 실행체 | 통지 | 비고 |
|---|---|---|---|
| 자동(same-SSID) | `wifi_roam.py` `roam_to_bssid()` | ✅ 대상 | `wpa_cli roam` |
| 자동(cross-SSID) | `wifi_roam.py` `connect_to_ssid()` | ✅ 대상 | `wifi connect` |
| **수동/테스트** | `passive_roam.py` (via `wifi <if> roam [0\|N]`, `wifi.sh:917`) | ✅ 대상 | **테스트 자유도 위해 필수** |
| bgscan 자율 로밍 | wpa_supplicant 내부 | ⏸ **차기 과제** | 지금은 제외 |

> 수동 통지가 중요한 이유: `wifi <if> roam N` 으로 임의 로밍을 유발해 **Roaming(0x04) 발행을
> 자유롭게 재현·검증**할 수 있어야 한다(테스트 진입점).

## 4. 컴포넌트 설계

### 4.1 공용 통지 헬퍼 (신규, DRY)
세 실행체가 각자 통지 코드를 복제하지 않도록 **공용 헬퍼** 하나로 묶는다.
- 후보: `roam_notify.py`(파이썬 모듈/함수) 또는 소형 CLI. 세 실행체가 로밍 성공 직후 호출.
- 동작: **post-roam** `link.json`(`/var/log/cantops/json/<iface>/link.json`)을 읽어 페이로드를
  구성 → opcd로 IPC 전송.

### 4.2 IPC 계약
- **전송(권장)**: **unix-datagram 소켓**(예: `/run/opcd/roam.sock`) 또는 로컬 UDP
  `127.0.0.1:<port>`. opcd가 이미 epoll fd 멀티플렉싱을 하므로 소켓 fd 추가가 가장 자연스럽다.
- **메시지(권장)**: 소형 JSON 한 줄
  ```json
  {"iface":"mlan0","ap_mac":"04:ba:d6:ec:0b:08","rssi":-47,"snr":46,"channel":40,"band":"5G","from":"00:80:4c:c7:7d:dd"}
  ```
- opcd는 **로컬 전용**(소켓 권한/바인드 127.0.0.1)으로 열고, 필드 검증 후 수용.

### 4.3 opcd 측 변경 (~~`opcd/platform_nxp.c` 중심~~ → **§9에서 `opcd/opcd.c` core로 확정**)
1. 통지 소켓 open + `evt_fd`처럼 epoll에 등록(또는 `platform_drain_events` 소스 확장).
2. 메시지 수신 → 파싱 → `opcd_platform_evt_t{kind=OPCD_PEVT_ROAMING, u.roaming={snr,rssi,mac,ch,idx=0}}` 합성.
3. 기존 콜백 경로(`opcd/opcd.c:95` `case OPCD_PEVT_ROAMING`)가 `opcd_ind_roaming()` 호출 → **Roaming(0x04) 발행**.
   - `idx!=0`(mlan1) drop 정책은 기존 로직 재사용(단일-STA #35).

### 4.4 로밍 실행체 훅 지점 (조사 확정)
- `wifi_roam.py:roam_to_bssid()` — `result.returncode==0` 분기, `optimize_post_roam_connectivity()` 직후.
- `wifi_roam.py:connect_to_ssid()` — 동 성공 분기.
- `passive_roam.py` — 로밍 성공 지점(구현 시 내부 확인; `wpa_cli roam` 성공 후).
- 각 지점에서 `from_bssid`/`to_bssid` 보유. post-roam AP 정보는 `link.json` 재독으로 획득.

## 5. 페이로드 매핑 (Roaming 0x04 ← link.json)
`link.address`→AP MAC · `link.signal_avg`→RSSI · `channel_info.<freq>.noise`로 SNR(=rssi−noise) ·
`info.channel`/`info.freq`→CH(band|ch 인코딩). (device-info live 소스와 동일 매핑 — testing-guide §D)

## 6. 결정 사항 (2026-07-06)

### 확정
1. **WlanStatusChange 중복 → ✅ 허용(both)**: 로밍 시 Roaming(0x04) + WlanStatusChange(0x02)를
   **둘 다 발행**한다. 억제하지 않음(발행 경로·타이밍이 상이해 억제가 복잡, 소비측이 `req_id`로 구분 가능).
   VHL은 Roaming 수신=로밍, WlanStatus 수신=링크상태로 각각 처리.
4. **소스 위치 → ✅ 양 repo 둘 다**: opcd(C) 변경=wlan-opc(서브모듈), 공용 헬퍼+3 훅=wlan-package.
   조율 커밋(부모 wlan-package gitlink 갱신 포함).

### 확정 (2026-07-06 결정) — 트레이드오프 및 선택

**2. 전송 방식: unix-datagram 소켓 vs 로컬 UDP(127.0.0.1)**
| | unix-datagram (`/run/opcd/roam.sock`) | 로컬 UDP (`127.0.0.1:<port>`) |
|---|---|---|
| opcd 코드 | AF_UNIX 신규 경로(bind path, 소켓파일 unlink/권한/디렉토리 lifecycle) | **기존 `open_udp_socket()` 재사용**(AF_INET/SOCK_DGRAM), bind 주소만 127.0.0.1 |
| 접근제어 | **파일권한으로 제한**(root만 write) | 포트 오픈(로컬 전용이나 임의 로컬 프로세스 송신 가능→메시지 검증 의존) |
| 부가물 | 포트 불요, 소켓파일 관리 필요 | 포트 1개 할당/config, 소켓파일 없음 |
| Python 송신 | `AF_UNIX` sendto(path) | `AF_INET` sendto(127.0.0.1,port) |
> 이 보드는 전 프로세스 root라 접근제어 차이는 미미. **재사용/단순성=UDP, 격리 정도=unix-datagram.**
> ✅ **결정: 로컬 UDP** (`127.0.0.1:<port>`) — opcd 기존 `open_udp_socket()` 패턴 재사용, C 신규코드 최소. 포트는 `opc.conf`에 config화(기본값 지정).

**3. 헬퍼 형태: 파이썬 모듈 vs CLI vs 하이브리드**
- 실제 호출자 3곳(`wifi_roam.py`, 그 안 `connect_to_ssid`, `passive_roam.py`)이 **전부 파이썬**
  (wifi.sh는 passive_roam.py로 위임). → 순수 **파이썬 모듈**이면 import 직접 호출로 충분(서브프로세스 X).
- **하이브리드 권장**: `roam_notify.py`에 `notify_roam(iface, from_bssid, to_bssid)` 함수 + `__main__` CLI
  (`python3 roam_notify.py --iface .. --from .. --to ..`) 둘 다 제공 → 파이썬은 import, 미래 shell 경로는 CLI.
> ✅ **결정: 하이브리드** — `roam_notify.py`(`notify_roam(iface, from_bssid, to_bssid)` 함수 + `__main__` CLI). 3 훅은 import로 직접 호출, shell 경로는 CLI 여지 확보.

## 7. 테스트 계획
1. `vhlctl --hex listen --bind <VHL>:9999` (유선 제어 경로)
2. `vhlctl set-indication --bits 0x06 --to <VHL>:9999` (Roaming 0x04 + WlanStatus 0x02)
3. **`ssh target 'wifi mlan0 roam 0'`** (수동 로밍 유발) — 또는 자동 로밍 대기
4. 기대: 수신기에 **`req_id=0x0004`(Roaming)** + SNR/RSSI/AP MAC/CH 페이로드. (현재는 0x0004 미발행)
5. 회귀: 순수 재연결(`wpa_cli disconnect/reconnect`)은 **Roaming 미발행**(WlanStatus만) 확인 — 재연결/로밍 구분 입증.

## 8. 커버리지 / 한계 / bgscan 차기과제 스코핑 (2026-07-07 확정)

### 8.1 커버리지 현황 (소스 전수 확정)
- **프로그램적 로밍 실행체는 `wifi_roam.py`·`passive_roam.py` 2개뿐이며 same/cross-SSID 전 경로가
  3훅 `notify_roam`으로 수렴**(wifi_roam.py:1510/:1740, passive_roam.py:198/:201).
  `wifi_periodic_roam.sh`는 passive_roam.py 위임이라 커버.
- `wifi_bgscan.py`(커스텀 iw 스캐너)는 자율 로밍을 **유발하지 않음** — 외부 요청 스캔 결과는
  wpa_supplicant 네트워크 선택에서 명시 제외(hostap v2.10 `events.c:2171` external_scan).
- 무notify 잔여 경로(알려진 한계): 수동 `wpa_cli roam/select_network/reassociate`
  (wifi.sh:1139/:1223·opc_wlan_apply.sh:53 의 안내 메시지 경유 수동 실행 포함),
  `wifi connect`/`wifi radio`류 재결합(wifi.sh:1294/:1304/:1755-1775), **wifi_checker.sh 복구
  reassoc(:237/:266 — 링크품질 트리거의 현존 자동 경로, 다른 BSS 재결합 가능·경계 사례)**,
  opc_wlan_apply reconfigure, FW 주도 roam(wifi_event.sh:116 'roamed to' — opcd 휴면 ROAM arm 담당).
  ~~passive_roam.py subprocess timeout 미세갭~~ → **수리됨(2026-07-07)**: timeout 후 실 결합 BSS가
  from과 다르면 통지(get_associated_bssid 재사용, 조회실패/미변경 시 생략=오발행 없음).

### 8.2 wpa_supplicant 내부 bgscan 갭 — 스코핑 결론
- mlan0 실행 conf의 bgscan은 주석=비활성(`wpa_supplicant-mlan0.conf:11`). 단 타겟 바이너리에
  bgscan **simple 모듈이 이미 컴파일됨**(strings 실측; learn 미컴파일=스코프 제외) — 주석 1줄
  해제로 즉시 활성 가능. **갭은 잠재적이나 활성화 문턱이 매우 낮다.** 배포 conf 중 유일한 활성
  bgscan은 **mlan1 템플릿**(`wpa_supplicant-mlan1.conf:11`) — opcd는 mlan0-only(단일-STA #35:
  mlan1 이벤트 drop)라 즉시 갭 아님. **결정(2026-07-07)**: mlan1은 현재 미사용이며 추후 **DBDC
  구현 시 고려대상** → 템플릿 bgscan은 **유지(스코프 제외)**, DBDC로 mlan1이 활성화/opcd 스코프에
  편입되는 시점에 본 갭이 실존하게 되므로 그 시점을 Phase 1 트리거로 연동(§8.3 ⑤).
- bgscan은 스캔 스케줄러일 뿐, 로밍 결정·실행은 wpa_supplicant core(need_to_roam_within_ess).
  **활성화돼도 커널 관측은 여전히 CMD_CONNECT뿐**(PREV_BSSID는 요청 방향 attr, 이벤트엔 없음)
  → §1 기각 논리는 유지되나 로밍 '의도'가 3훅 밖(supplicant 내부)에 존재하게 됨.
- bgscan 외 자율 로밍 벡터(전부 3훅 우회): (V1) associated 중 `wpa_cli scan` 자기요청 스캔의
  within-ESS 평가(배포 스크립트엔 현재 없음 — 소스 전수 확인), (V2) 11v BTM(wnm_bss_tm_connect),
  (V3) 단절 후 재연결, (V4) FW roaming offload(→CMD_ROAM, 휴면 arm 담당).
  **bgscan 가드만으로 자율 로밍 0이 보장되지 않는다.**
- supplicant 자율 로밍의 유일한 catch-all 관측 지점 = **wpa_ctrl 소켓 ATTACH 상주 모니터**:
  v2.10에 `CTRL-EVENT-DO-ROAM cur_bssid=.. sel_bssid=..`(events.c:2003) 존재 +
  `CTRL-EVENT-CONNECTED` 재발행(새 BSSID 포함). `wpa_cli -a` 액션 스크립트는 dedup
  (wpa_cli.c:4260, 동일 network id면 미호출)으로 same-SSID 전이를 못 잡아 **확정 배제**.
  '성공 로밍엔 CTRL-EVENT-DISCONNECTED 미발행'은 이 타겟에서 **실측 확인됨(2026-07-07,
  수동 roam 2회 양방향 ch36↔ch40)** — ctrl 스트림은 `Trying to associate →
  CTRL-EVENT-SUBNET-STATUS-UPDATE → CTRL-EVENT-CONNECTED(새 bssid)`만 발행. 그래도 Phase 1
  상태머신은 방어적으로 **DO-ROAM latch + 짧은 창 내 BSSID 전이**(DISCONNECTED
  (locally_generated=1) 직후 CONNECTED 포함)로 설계한다(자율로밍 경로 미실측분 방어).

### 8.3 조건부 로드맵 (결정)
- **Phase 0 (현행 채택, 가드 구현됨 2026-07-07)**: 3훅 커버리지 유지 + '내부 bgscan 비활성'을
  운영 전제로 격상 + 가드 — (a) 부팅 conf lint: 비주석 `bgscan=` 검출 시 syslog 경고
  (**wifi_init.sh** extra_ssid sync 직후, mlan0 전용), (b) wifi_checker.sh 주기(300s)
  `get_network <id> bgscan` 조회(**전 network id 순회** — 모드 A extra_ssid 자동 블록 포함,
  mlan0 전용), (c) wpa.log(-d) 의 `bgscan: Initialized module`(hostap 2.10 bgscan.c:64) 감시
  (전역 set 간접 탐지). 탐지 패턴은 실타겟 conf로 검증됨(mlan0 무매치/mlan1 매치/미설정
  network는 FAIL 필터).
  **가드 한계(명시)**: 전역 `wpa_cli set bgscan`은 ctrl로 무탐지(hostap 2.10 전역 getter NULL,
  config.c:5072/:5096)이고 연결 중 즉시 적용됨(wpa_supplicant.c:7592-7595) → (c)가 유일한 보조
  탐지이며, SIGNAL_MONITOR probe 역이용은 CQM 등록 부작용으로 비채택. 대응은 경고-only
  (자동 해제는 operator 의도 충돌). opcd(C) 무변경·신규 발행 소스 없음 → 재연결 오발행 구조적
  0(§7 테스트5 유지).
- **Phase 1 (트리거 발생 시)**: wpa_ctrl ATTACH 상주 모니터(신규 `wifi_roam_monitor.py`+systemd
  유닛, ~500 LOC/2-repo PR 2건) + opcd UDP 수신부 **dedup 창**(ap_mac 키 ~3s, §8.5 FT 전략(b)와
  동일 자리라 선구현 시 재사용). 3훅은 권위 소스(트리거 시점 페이로드) 유지·모니터는 backstop.
  **dedup 소스 우선순위**: first-wins가 아니라 창 내 실행체 notify 도착 시 교체/우선(권위
  페이로드 폐기 방지).
  - 트리거: ① mlan0 내부 bgscan 활성 요구 ② 11v BTM AP 도입 ③ 운영 절차의 `wpa_cli scan` 사용
    ④ Phase 0 가드 경고 실관측 ⑤ **DBDC 구현으로 mlan1 활성화/opcd 스코프 편입**(템플릿 bgscan
    활성 실재라 편입 즉시 갭 실존 — DBDC 설계 단계에서 본 절 재평가 필수).
  - Phase 1 선행 온타겟 확인 — **2026-07-07 실측 반영**:
    (a) ROAM_SUPPORT 광고: **정황 없음**(wpa.log에 bss-selection/driver-based 문자열 무 +
    moal 모듈 roam 파라미터 무) — 최종 확정만 자율로밍 평가 시점 로그로 잔여.
    (b) 로밍 ctrl 시퀀스: **실측 완료**(위 §8.2) — DISCONNECTED 미발행 확인.
    **DO-ROAM 실발행만 잔여**(자율로밍에서만 관측 가능 — Phase 1 착수 시 bgscan 임시 활성으로 확인).
    (c) SIGNAL_POLL: **동작 확인**(RSSI/NOISE/FREQUENCY 반환).
    (d) FW roaming offload: **미사용 정황**(모듈 파라미터 부재).
    (e) 스펙 §3.4.3 'AP MAC' semantics(로밍 전 vs 후) 재점검 — 잔여(DFK 문의 채널).
- **기각 — opcd 내부 CONNECT BSSID-diff 합성**: 재연결 오발이 스펙 §3.4.3('임계값 하회' 트리거
  사건) 위반, §1 기각 실측 유효, FW 로밍은 휴면 ROAM arm(platform_nxp.c:1270-1288) 담당이라
  고유 이득 없음. DFK 스펙 semantics 재합의 없이는 채택 불가.

### 8.4 운영 전제 (위반 시 Roaming(0x04) 커버리지 파손)
1. mlan0 conf 내부 bgscan 비활성 유지(주석). 2. 런타임 `wpa_cli set/set_network … bgscan` 금지
(update_config=1 되쓰기 유의, **전역 set은 ctrl로 무탐지**). 3. associated 중 `wpa_cli scan` 운영
사용 금지(iw 스캔은 external_scan 제외라 안전). 4. 11v BTM AP 미사용. 5. FW roaming offload
미사용/ROAM_SUPPORT 미광고(온타겟 확인 필요). 6. opcd mlan0-only — mlan1 템플릿 bgscan은 유지
결정(스코프 제외), **DBDC 구현 시 재평가(Phase 1 트리거 ⑤)**. 7. FT(802.11r) 미사용(§8.5).
8. bgscan learn 미컴파일 유지.
9. 수동 `wpa_cli roam/select_network/reassociate`·`wifi connect/radio`류 재결합은 통지 대상 외.

### 8.5 FT(802.11r) 이중발행 (기존 유지)
FT 도입 시엔 커널이 CMD_ROAM을 내므로 기존 nl80211 경로로도 발행됨 → **단일 로밍에 Roaming(0x04)이 이중 발행될 수 있음**(nl80211 경로 + UDP notify). 현 보드는 host-based SME라 CMD_ROAM 미발행 = 이중발행 없음. **FT 활성화 시 전략**(도입 시점 확정 과제, claude 리뷰 지적): (a) 로밍 실행체의 UDP notify를 조건부 비활성화(FT면 커널이 권위 소스), 또는 (b) opcd에서 nl80211 ROAM 수신 후 짧은 창 동안 UDP notify를 억제해 한 소스만 채택 — **§8.3 Phase 1 의 dedup 창과 동일 지점이라 (b) 선구현 시 재사용됨**. 병행하여 소비측(VHL)이 동일 `req_id` 중복을 무해 dedup 처리하는지 오동작하는지 실측 확인.

## 9. 구현 확정 (2026-07-06 코드검증 반영)

> 6개 앵커 클러스터를 현 코드와 전수 대조한 결과. 결정 2건 추가확정: **opcd 통합=`opcd/opcd.c` core**, **포트=opc.conf 파서 신규(`freq_source.c` 패턴)**.

### 9.0 검증으로 정정된 설계 전제
- **§4.3 정정**: opcd 변경은 `platform_nxp.c`가 아니라 **`opcd/opcd.c` core**. 이유 (1) 결정#2가 재사용키로 한 `open_udp_socket()`이 opcd.c에 있음(`opcd/opcd.c:191`), (2) 플랫폼 vtable `event_fd()/drain_events()`는 **단일 fd만 지원**(`platform.h:239`) → nxp에 넣으면 내부 멀티플렉싱/vtable 수술 필요, (3) opcd.c core는 host 빌드/`make check` 가능(nxp는 cross 전용). opcd.c는 이미 5-fd epoll 멀티플렉싱 중(sig/timer/store/evt/udp, `opcd.c:354-496`) → 6번째 fd 추가가 자연스러움.
- **결정#2 config 정정**: `udp_port`는 실제로 opc.conf에서 **안 읽음**(컴파일 기본값 `OPC_DEFAULT_UDP_PORT`=50607 + `-p` CLI뿐, `opcd.c:254-255` 주석 명시). opc.conf 스칼라 파서 패턴은 `freq_source.c:14`가 유일 → 복제. **repo에 opc.conf 템플릿 파일 없음**(런타임 전용, `OPC_PATH_CONF`) → 키는 문서/테스트픽스처로만 노출.
- **기존 nl80211 ROAM 경로 이미 구현됨**(`platform_nxp.c:1270-1288` `nl_stage_evt` case `OPCD_NL_ROAM`). 이 보드에선 CMD_ROAM 미발행이라 **휴면**(§1 전제). UDP notify는 이 보드에서 0x04를 실제 띄우는 **병렬 소스** — 충돌 없음, FT 도입 시 기존 경로 부활(§8 병존).
- **채널 인코딩**: `u.roaming.channel`은 생 채널번호가 아니라 `opc_chan_field(freq_mhz, ch)`=`band<<8 | ch`(`chan_encode.h`). opcd측에서 인코딩(페이로드는 raw freq+ch 전달).
- **indication 시그니처**: `int opcd_ind_roaming(opcd_state_t *st, int8_t snr, int8_t rssi, const uint8_t ap_mac[6], uint16_t ch)` — **첫 인자 `st` 있음**(설계 §2 표기 누락). gate `OPC_IND_BIT_ROAMING=0x04`(`protocol/ids.h:37`).
- **struct 필드**(정확): `platform.h:98-104` `u.roaming{ uint8_t idx; int8_t snr; int8_t rssi; uint8_t mac[6]; uint16_t channel; }` (선언순 idx-first, 이름/타입 전부 일치).

### 9.1 opcd C (wlan-opc) — 구현 순서
1. **config 필드**: `opcd/opcd_state.h` `opcd_conf_t`(:23-28)에 `uint16_t roam_notify_port;` 추가 + `#define OPC_DEFAULT_ROAM_NOTIFY_PORT 50608`(:30 인근).
2. **default**: `opcd/opcd.c` `state_set_defaults()`(:122-140)에 `st->conf.roam_notify_port = OPC_DEFAULT_ROAM_NOTIFY_PORT;`.
3. **파서 모듈 신규**: `opcd/roam_notify_conf.{c,h}` — `uint16_t opcd_roam_notify_port_parse(const char *conf_path, uint16_t dflt)`. `freq_source.c` 스캔(`sscanf(line," %47[A-Za-z0-9_] = %63s",key,val)`, key=`roam_notify_port`, 1..65535 range, last-wins, over-long-line skip) 복제. `opcd/Makefile` SRCS 등록.
4. **startup 연결**: `opcd/opcd.c` main() `:257`(freq_source parse) 직후 `st.conf.roam_notify_port = opcd_roam_notify_port_parse(st.paths.conf, st.conf.roam_notify_port);`.
5. **리스너 소켓**: `open_roam_socket(uint16_t)` 추가(open_udp_socket `:191` body 복제, **bind=INADDR_LOOPBACK**). main() udp_fd 블록(:290-292) 직후 `int roam_fd = open_roam_socket(st.conf.roam_notify_port);`(루프 스코프 로컬).
6. **epoll 등록**: store_fd 등록 idiom(:338-348) 복제 — `EPOLL_CTL_ADD roam_fd EPOLLIN`, 실패 시 non-fatal(log+close+roam_fd=-1). store_fd 블록 뒤(:348), `rx[]` 선언(:350) 앞.
7. **드레인+이벤트 주입**: 디스패치 for-loop에 `else if (fd == roam_fd)` arm 추가. nonblocking recvfrom 루프(udp arm :395-408 미러) → datagram JSON을 `opc_json_*` 헬퍼로 파싱 → `opcd_platform_evt_t evt = { .kind=OPCD_PEVT_ROAMING, .u.roaming={ .idx=0, .snr, .rssi, .mac, .channel=opc_chan_field(freq,ch) } }` → `on_platform_event(&evt, &st)`(static `:80`, mlan0-only idx 가드 재사용).
8. **단위테스트**: `opcd/tests/test_roam_notify_conf.c`(`test_freq_source.c` 복제 — 파일없음→default / valid / 범위초과→default / last-wins). make check 배선.

### 9.2 helper + 3훅 (wlan-package)
9. **신규 `dist/wlan/usr/local/logger/roam_notify.py`**: `notify_roam(iface, from_bssid, to_bssid)` + `__main__` CLI(argparse `--iface/--from/--to/--port`). post-roam `link.json`(`/var/log/cantops/json/<iface>/link.json`) 재독 → 페이로드: `ap_mac`=`link.address`, `rssi`=int(`link.signal_avg` 문자열 `' dBm'` strip), `noise`=`channel_info[str(info.freq)].noise`, `snr`=rssi−noise, `channel`=`info.channel`, `freq`=`info.freq`, `band`(freq 파생), `from`=from_bssid. **파일 `{}`(미연결)면 skip**. UDP→`127.0.0.1:<port>`(기본 50608). **전 구간 try/except, 절대 raise 안 함**(로밍 return 경로 보호).
10. **훅1** `wifi_roam.py:roam_to_bssid()`(:1470, 시그니처 `(from_bssid, to_bssid)` — **iface 파라미터 없음, 전역 `IFACE` 사용**) — `:1507`(optimize) 직후 `:1509`(return True) 앞 `notify_roam(IFACE, from_bssid, to_bssid)`. import `from roam_notify import notify_roam`(:14 `sUTILS` import 인근).
11. **훅2** `wifi_roam.py:route_cross_ssid_transition()`(:1723 — cross-SSID **단일 choke point**, 유일 호출 :2055). 모드 A(`select_network_for_ssid`, `wpa_cli select_network`)·모드 B(`connect_to_ssid`, 외부 `wifi connect`) 두 분기 결과 `ok` 캡처 후 `if ok: notify_roam(iface, from_bssid, to_bssid)`. ⚠️ **실측 발견**: 설계 원안의 'connect_to_ssid 내부 훅'은 모드 A(`generate_network_blocks=true`, select_network 경유, 성공점 :1698)를 **놓침**. 출하 기본은 모드 B(`GENERATE_NETWORK_BLOCKS=False` :115, wifi_init_conf.json:288)라 원안으로도 커버되나, **route 레벨 훅이 모드-무관하게 안전** → 이쪽으로 상향.
12. **훅3** `passive_roam.py:roam_to_ap()`(bssid=`ap['bssid']` :159, `interface` 파라미터) — `:183`(ROAM_RESULT print) 직후 `:184`(return returncode) 앞 `if result.returncode==0: notify_roam(interface, read_current_bssid(interface), bssid)`. same/cross-SSID 공통 exit → returncode만 게이트. `import socket`+roam_notify 추가. wifi.sh(:917 `roam)`)는 **변경 불요**.
13. **부모 gitlink**: wlan-package가 wlan-opc 서브모듈 포인터 갱신(커밋 시점, 사용자 요청 후).

### 9.3 포트 커플링 주의
python 기본 포트(50608)와 opcd `OPC_DEFAULT_ROAM_NOTIFY_PORT`(50608)는 **반드시 일치**. opc.conf로 opcd 포트를 바꾸면 python은 opc.conf를 안 읽으므로 `--port`/env로 동일 override 필요(기본값 정렬로 무설정 케이스 커버).

### 9.4 착수 전 실측 (설계 §8 권장)
- **모든 로밍이 이 3경로만 거치는지**: 타깃 `wpa_supplicant.conf` `bgscan` 설정 유무 + 자율 로밍 유발 시 wifi_roam.py/passive_roam.py 로그 경유 관찰. bgscan 자율 로밍이 있으면 그 경로는 미통지(차기 과제).
- 포트 50608 미충돌(`ss -lun`).
- notify 성공판정: `wpa_cli roam`이 returncode 0이어도 stdout `FAIL` 가능 → 필요 시 stdout `OK` 확인 보강(1차는 returncode).

#### 실측 결과 (2026-07-06, **정적**; 타깃 192.168.0.100 offline이라 런타임 미완)
- **mlan0 로밍 실행체 = {wifi_roam.py, passive_roam.py} 뿐** (소스 전수):
  - `wifi_roam@mlan0`(wifi_roam.py): same-SSID `roam_to_bssid()`(:2065 호출) + cross-SSID `route_cross_ssid_transition()`(:2055 호출, 모드A `select_network_for_ssid`/모드B `connect_to_ssid`). → 훅1·훅2로 전부 커버.
  - `wifi_periodic_roam@mlan0`(wifi_periodic_roam.sh): 주기적으로 `passive_roam.py 0 --iface` 실행 → **passive_roam.py 경유**(훅3 커버). 수동 `wifi roam N`과 동일 코드.
  - `wifi_bgscan@mlan0`(wifi_bgscan.py): **로밍 트리거 안 함** — 스캔 후 `/tmp/wifi_roam_hint_<iface>` touch만(wifi_roam.py가 소비). 실행체 아님.
- **wpa_supplicant 내부 bgscan**: mlan0 conf(`wpa_supplicant-mlan0.conf:11`) **주석처리=비활성** → 우회 자율 로밍 없음. (mlan1은 활성이나 opcd mlan0-only라 무관.) ⚠️ **잔여 리스크**: 런타임에 operator가 mlan0 bgscan을 켜면 그 로밍은 커널 CMD_CONNECT만 내어 3훅 우회·미통지(설계 §1/§8의 '재연결 구분 불가' 케이스, 차기 과제).
- **런타임 미완(타깃 복귀 시)**: (a) 포트 50608 실기기 미충돌 `ss -lunp`, (b) 실제 로밍 1회 유발해 3경로만 경유하는지 로그 확인, (c) `wpa_cli roam` rc=0-but-FAIL 여부. — opcd `udp_port`=50607(test-env.json 확인)이라 50608 로컬 무충돌.

### 9.5 코드리뷰 반영 (PR #64, 2026-07-06)
Gemini(앱/워크플로) + Codex 리뷰 4건 수용·1건 기각, `opcd.c` 하드닝(와이어 포맷 불변):
- **F5(Codex P2) 포트 충돌 가드**: `roam_notify_port == udp_port`이면 SO_REUSEADDR로 루프백 roam bind와 wildcard 제어 bind가 충돌해 로컬 VHL 요청이 roam_fd로 흡수(→무응답 타임아웃)될 수 있음 → 동일 포트 시 roam-notify **비활성화**(경고 로그).
- **F1 channel/freq 필수 검증**: 누락/범위밖이면 datagram **drop**(`opc_chan_field(0,0)`=0x0000 발행 방지). `channel∈[1,255]`, `freq∈[2400,7300]`.
- **F2 iface→idx**: `idx=0` 강제 대신 iface로 idx 결정(mlan0=0/mlan1=1/그외 drop) → mlan1은 `on_platform_event` 단일-STA 가드가 정상 drop(오귀속 제거, 와이어계약 정합).
- **F3 로그 rate-limit**: malformed datagram 로그를 1/64로 제한(로컬 오작동 프로세스 로그 스팸 방지).
- **F4(기각)**: `parse_bssid`의 `v[i]>0xFF`는 `%2x`가 이미 2자리 제한이라 dead check(무해) — 변경 없음.

라운드2(claude 리뷰) 반영:
- **[MEDIUM] 512B 초과 datagram**: `recvfrom`에 `MSG_TRUNC`로 실제 길이 확인 → 512B 초과 시 drop(잘린 페이로드를 완전한 것으로 오파싱 방지, rate-limit 로그).
- **[LOW] rssi/snr 클램프**: `int8_t` 캐스트 전 [-128,127] 클램프(범위밖 값의 impl-defined 변환 회피).
- **[LOW 보류]** `parse_bssid`/`roam_datagram_to_evt` host 단위테스트 — static 함수 추출 리팩터 필요, 동작은 e2e로 커버됨, 후속 과제.
- 실기기 재검: valid→0x0004 발행 / channel누락·mlan1·**초과(813B)** datagram→drop 확인.

## 참조
- 실측·층위 분석: 세션 메모리 `on-target-indication-testing`
- opcd: `opcd/opcd.c`(epoll) · `opcd/platform.h`(evt) · `opcd/indication.c` · `opcd/nl80211_parse.c`
- 실행체: `wifi_roam.py`(`/usr/local/logger/`) · `passive_roam.py` · `wifi.sh`(`/usr/local/scripts/`, roam=L917)
