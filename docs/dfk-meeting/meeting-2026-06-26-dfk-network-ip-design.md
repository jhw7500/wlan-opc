# DFK 회의 — 네트워크 토폴로지·IP 설계 분석 (2026-06-26)

> **목적**: DFK 8p "운용 네트워크 토폴로지"를 우리 무선기판(BD) 방식에 맞춰 IP/게이트웨이/MAC을 어떻게 설계해야 하는지 정리.
> **전제**: 우리 무선 브릿지 방식(**3-address L2 MAC 스푸핑**)은 **변경 불가**. 이를 기준으로 네트워크를 맞춘다.
> **근거**: `wlan-bridge/wbridge/`(L2 forward·ARP 필터), `wifi_init.sh`/`wifi_init_conf.json`(mac_mode=dynamic, arp_ignore_always), `.network`(eth0/mlan0 IP), `opcd`(INADDR_ANY, eth0 관리 IP), 사용자 실기 검증 경험.
> **상태**: 실타깃 오프라인 — 코드/설정/경험 기반 결론. 일부 항목 실측 보강 예정(§12).

---

## 1. 한눈에 (TL;DR)

1. **우리 장치 = 3-addr L2 MAC 스푸핑 브릿지** — mlan0가 유선 기기(OPC Board) MAC(M)을 클론해 일반 AP에 단말 1대로 접속. 일반 AP 호환, WDS/4addr·Proxy ARP **미사용**.
2. **eth0 IP로는 무선 통신이 안 된다** — eth0는 OPC Board와의 **유선 1:1 전용**. `arp_ignore_always`가 eth0 IP의 무선 노출을 차단. **BD 자신의 무선 통신은 mlan0 IP로** 한다.
3. **무선에 노출되는 IP = OPC Board IP + mlan0 IP** (둘 다 클론 M). 이 둘만 무선 서브넷에서 유일·관리 대상. **eth0 IP는 무선과 무관**.
4. **MAC 충돌(한 MAC 다IP)은 IP 서브넷으로 못 푼다** — mlan0 MAC은 클론 M으로 고정(서브넷 무관). 단 **OPC Board IP와 mlan0 IP를 다른 서브넷에 두면 ARP 경합은 회피**된다(사용자 실기 검증).
5. **운용 권장(B 구성)**: OPC Board는 자기 GW로 MCP와, BD는 별도 서브넷+BD용 GW로 NTP/PC와 각각 통신. **MCP/NTP/PC는 코어망에 같이 둬도 됨**(게이트웨이가 라우팅).
6. **여러 BD 배치**: eth0 IP는 고정 가능(186.1 등, 무선 무관), **mlan0 IP·OPC Board IP만 기기별 유일**.

---

## 2. 우리 장치(BD)의 브릿지 방식 — 고정

| 항목 | 내용 | 근거 |
|---|---|---|
| 무선 모드 | 3-address station (host MLME) | `driver-options.md` `host_mlme=1` |
| MAC | **유선 peer(OPC Board) MAC(M) 동적 클론** | `wifi_init_conf.json` `mac_mode:dynamic`, `wifi_init.sh:526` resolve_mac |
| 데이터패스 | user-space wbridge(pcap), **L2 forward** | `bridge_packet_handler.c` |
| AP 요구 | 일반 AP (WDS/4addr 불필요) | `vlan-passthrough-prereq.notes.md` |
| Proxy ARP | **미사용** (자기 IP 대상 ARP는 drop·커널 처리) | `filter.c:150-205` |

→ 네트워크 관점: **OPC Board가 AP 너머에 직접 붙은 단말 1대처럼** 보이고, 우리 장치(BD)는 그 사이의 투명 어댑터. 무선 너머에서 보이는 주체는 OPC Board(MAC M)다.

---

## 3. 인터페이스·IP 역할 (★ 핵심)

```
OPC Board ──eth0(유선 1:1)── BD ──mlan0(무선, 클론 M)── AP ─── 무선 서브넷
            └ arp_ignore로 무선 노출 차단        └ 무선에 노출되는 쪽
```

| 인터페이스/IP | 통신 범위 | 무선 노출 | 비고 |
|---|---|---|---|
| **eth0 IP** | OPC Board측 **유선 1:1만** | ❌ 안 됨 | `arp_ignore_always`가 차단 → **eth0 IP로 무선 통신 불가** |
| **mlan0 IP** | BD 자신의 **무선 통신**(NTP/PC) | ✅ 노출 | 클론 M MAC. BD가 무선 너머와 통신하는 IP |
| **OPC Board IP** | 클론 M으로 브릿지 → **무선 너머**(MCP) | ✅ 노출 | 반송제어. BD가 IP를 안 바꾸고 투명 전달 |

- **opcd는 `INADDR_ANY`로 listen**(`opcd.c:199`)이라 OPC 제어 요청은 eth0/mlan0 어디로 와도 수신. 단 device-info/ChangeIp가 다루는 "장치 IP"는 **eth0** 기준(`handler.c:543`).
- 출하 `.network` 예: eth0=`192.168.1.1/24`, mlan0=`192.168.0.100/24`(타서브넷) + `arp_ignore_always=true`. → **eth0-IP 토폴로지**.

---

## 4. MAC 충돌 — 원리와 해법

### 왜 IP 서브넷으로 못 푸나
- mlan0의 MAC은 **클론 M(OPC Board MAC)으로 고정**되며, MAC은 L2 속성이라 IP 서브넷(L3)을 바꿔도 그대로 M이다.
- 따라서 mlan0를 어느 서브넷(/17·/20·/22)에 두든 OPC Board(M)와 **같은 MAC**이고, "한 MAC에 여러 IP"는 그대로 남는다.

### 무엇이 실제 문제이고, 무엇이 해법인가
- **"한 MAC에 여러 IP"는 표준상 정상**(멀티홈). 진짜 문제는 *"같은 서브넷에 같은 MAC의 여러 IP → ARP 경합"*이다.
- **OPC Board IP와 mlan0 IP를 서로 다른 서브넷에 두면 ARP 경합이 사라진다** → 동작. (사용자 실기 검증 §5)
- 다만 일부 엔터프라이즈 AP/WLC가 **DAI(Dynamic ARP Inspection)·port-security·DHCP snooping**으로 "1 MAC = 1 IP"를 강제하면 막힐 수 있음 → **DFK 인프라 확인 필요**(§11).

### mlan0의 서브넷은 우리가 임의로 못 정한다
- mlan0는 AP에 무선 접속하므로, **그 서브넷은 AP가 서빙하는 BSS 서브넷에 종속**된다(AP-HUB 백홀망 같은 곳에 임의로 못 넣음).
- 단 AP가 순수 L2 브릿지이면, **mlan0와 통신 상대가 같은 L2 세그먼트 + 같은 서브넷이면 AP IP와 무관하게 통신**된다(사용자 검증).

---

## 5. 검증된 구성 — 게이트웨이 없는 단순 L2 (사용자 실기 경험)

```
[하나의 L2 세그먼트, 게이트웨이 없음, 서브넷만 분리]
 OPC망 192.168.0.x/24 :  OPC Board(0.x, M) ← MCP(0.x) 가 접속        [OPC 브릿지 ✅]
 mlan0망 192.168.1.x/24:  BD/mlan0(1.x, M)  ← PC(1.x)  가 접속        [BD 무선 ✅]
```

- 같은 클론 MAC M이지만 **서브넷(0.x/1.x) 분리** → ARP 경합 없이 **OPC 브릿지 + BD 무선이 동시 동작**(실기 확인).
- 전제: MCP·PC가 BD와 **같은 L2 세그먼트**(같은 AP)에서 **같은 서브넷 직통**. 게이트웨이 불필요.

→ 이 구성은 이미 검증됨. **8p가 이런 단순 L2로 운용 가능하면 그대로 적용 가능**(§11 확인 ①).

---

## 6. 운용 권장 — B 구성 (게이트웨이/코어망)

8p는 MCP/NTP/PC가 **게이트웨이 너머 코어망(172.22.0.0/22, CORHUB 너머)**에 있다 → 검증 구성과 달리 **L3 라우팅**이 필요하다.

```
OPC Board (172.22.131.x / /17) ──GW 172.22.129.254──┐
                                                    ├── 코어망 MCP·NTP·PC (/22)
BD/mlan0 (별도 서브넷)          ──GW(BD용)───────────┘
   ↑                                ↑
 OPC Board 측이 설정              우리 opcd가 설정
```

| 대상 | IP / 마스크 / GW | 통신 | 설정 주체 |
|---|---|---|---|
| **OPC Board** | 172.22.131.x / /17 / **GW 129.254** | → MCP | OPC Board 측(DFK) |
| **BD(mlan0)** | **OPC와 다른 별도 서브넷** / **BD용 GW** | → NTP/PC | 우리 opcd |
| MCP·NTP·PC | 172.22.0.0/22 (그대로) | — | — |

- **MCP와 NTP/PC의 망분리는 불필요** — 코어망에 같이 둬도 게이트웨이가 라우팅한다. (검증 구성에서 MCP/PC가 나뉘었던 건 게이트웨이가 없어 같은 서브넷 직통만 가능했기 때문.)
- **OPC Board와 BD는 각각 GW가 필요**하다 — 둘 다 다른 서브넷의 코어망과 통신하므로. (이는 우리 T2 입장을 "브릿지라 GW 불요" → "장치 자신의 cross-subnet 통신엔 GW 필요"로 정정한 것과 일치.)
- OPC Board와 BD를 **다른 서브넷**에 두므로, 같은 클론 M MAC이어도 ARP 경합이 없다(§4).

**DFK 요청(B 구성)**: **BD(무선기판)용 서브넷 1개 + 그 서브넷의 게이트웨이 1개**를 배정해 달라. (OPC Board용 GW 129.254는 8p에 이미 있음.)

---

## 7. 여러 BD(브릿지 장치)를 배치할 때 — IP 관리 규칙

| IP | 여러 BD 배치 시 | 이유 |
|---|---|---|
| **eth0 IP** (예 172.22.186.1) | **고정/중복 가능** | 무선에 노출 안 됨. 각 BD–OPC Board 전용 케이블(분리된 L2)에만 유효 |
| **mlan0 IP** | **BD마다 유일** | 무선 서브넷에 노출됨 |
| **OPC Board IP** | **기기마다 유일** | 클론 M으로 무선에 노출됨 |

- eth0 IP를 모든 BD가 186.1로 고정해도 **무선 통신에 지장 없음**(eth0 IP는 무선에 안 나감). 단 무선에 노출되는 mlan0 IP·OPC Board IP만 중복 없이 배정하면 된다.
- /17은 약 3만 IP라 여유 충분 — **중복 배정만 금지**.

---

## 8. 같은 IP를 가진 무선 장치가 있어도 안전한가

**시나리오**: BD eth0 = 172.22.186.1, 그런데 무선 서브넷에도 172.22.186.1(장치 X)이 존재.

```
OPC Board ─186.1 ARP→ eth0(BD,186.1) 응답 → OPC Board ↔ BD 통신
                       └ wbridge: 186.1 ARP는 무선으로 forward 안 함(drop)
무선 장치 X(186.1) ── 무선 세그먼트에서 자기끼리만 (OPC Board와 격리)
```

- **OPC Board가 186.1과 통신 → eth0(BD)로만 간다.** 무선 장치 X로는 가지 않는다.
- 격리 메커니즘: ① wbridge ARP 필터(`filter_arp_is_for_bridge`)가 **자기 IP(186.1) 대상 ARP를 무선으로 forward 안 함**, ② `arp_ignore_always`로 mlan0가 eth0 IP ARP에 응답 안 함.
- 결과: **유선의 186.1=BD eth0**, **무선의 186.1=장치 X**로 분리. ARP 충돌이 밖으로 새지 않는다.
- 유일한 제약: OPC Board가 *무선 장치 X(186.1)를 부르려 해도* BD가 가로채 X에는 못 간다(같은 IP라). 보통 OPC Board는 자기 BD와 통신하므로 정상.
- 전제: wbridge IP 필터 활성(`enable_ip_filter=true`).

---

## 9. DFK에 확인/요청할 사항

1. **PC/NTP의 위치** — BD와 **같은 L2 세그먼트(같은 AP)**인가, **코어망(CORHUB 너머)**인가?
   - 같은 세그먼트 → §5 검증 구성 그대로 적용 가능(게이트웨이 불필요).
   - 코어망 → §6 B 구성(BD용 GW 필요).
2. **BD(무선기판)용 서브넷 + 게이트웨이 1개** 배정 가능 여부 (B 구성 전제).
3. **AP/WLC가 한 station MAC에 복수 IP를 허용**하는가 (OPC Board IP + mlan0 IP가 같은 클론 M을 공유). DAI/port-security/DHCP snooping 정책 확인.
4. 무선 서브넷의 **IP 주소 계획** — mlan0 IP·OPC Board IP가 기기별로 중복되지 않도록.

---

## 10. 실측 검증 필요 항목 (실타깃 재가동 시)

- [ ] 한 클론 MAC(M)에 두 IP(OPC Board + mlan0)가 실 AP/WLC를 통과하는지
- [ ] BD 자신의 무선 통신(mlan0 IP)으로 NTP/PC 도달 동작
- [ ] eth0 IP가 무선에 노출되지 않음(arp_ignore_always 동작) — 같은 IP 무선 장치와 격리
- [ ] wbridge ARP 필터가 자기 IP 대상 ARP를 무선으로 차단
- [ ] `proxy_arp=0` (Proxy ARP 미사용)

---

## 11. 최종 DFK 요청안 + 산출물 (2026-06-29)

> §1~§10 분석을 바탕으로 DFK 전달용 **요청 문구 + 토폴로지 그림(PPTX/PNG)** 확정.

### 11.1 핵심 결론
- **wlan0(무선 관리 IP)**: OPC Board와 **다른 별도 IP 서브넷**(빈 대역 /24~/29, 예 `172.22.112.0/24`) — ARP 경합(flux) 회피. **IP만 분리, L2 세그먼트는 OPC와 동일**(단일 클론 MAC STA라 VLAN 분리 불가).
- **wlan0 전용 GW**: 기존 OPC GW `172.22.129.254`와 **동일 L2 세그먼트·동일 장치에 secondary**로 추가(예 `172.22.112.254`). wlan0가 직접 ARP 해석해야 하므로 AP-HUB 백홀(/20)·CORHUB(2-hop)에는 불가.
  - `.129.254` 호스팅 장치 = **AP-HUB(client측) 추정** (근거: AP-HUB가 자기 /20 default GW `.97.254` 보유=L3 라우팅 서명, /17·/20 멀티홈 등재, 문서 "AP-HUB·CORHUB가 L3 라우팅"). 단 CORHUB(collapsed-core) 가능성도 열려 있어 **DFK 확인 필요** — 어느 쪽이든 "`.129.254`와 동일 자리"가 불변 요건.
- **도달성(return route)**: wlan0 서브넷이 services망(/22)에서 라우팅 도달 가능하도록 **경로 등록** 필요 → 8p 메모 "PC→STA(wlan0) Access" 실현(양방향).
- **eth0(유선)**: OPC Board와 **단독 폐쇄망**(유선 1:1·격리·무선 미노출 `arp_ignore`). IP `172.22.186.1/17`(**OPC와 동일 서브넷** → 유선 관리 직접 통신 가능)로 **고정·전 BD 중복 가능**. 데이터(반송제어)는 L2 투명 브릿지라 eth0 IP 무관. → **DFK 코어망 주소 배정 불요**.
- **OPC Board**: `172.22.131.x`, GW `172.22.129.254` **유지**(반송제어 MCP 도달).

### 11.2 산출물 (동일 폴더)
| 파일 | 내용 |
|---|---|
| `dfk-wlan0-subnet-request-topology.pptx` | ⭐ **최종 산출물** — slide1=방식 비교·강점 & 변경 안(OPC GW + wlan0 ①②③) / slide2=토폴로지 그림 + DFK 확인 2건 (편집 가능, 네이티브 도형) |
| `dfk-wlan0-subnet-request-topology-p1.png` · `-p2.png` | 슬라이드 1·2 이미지 (현재본, PPTX에서 export) |

### 11.3 DFK 요청 문구 (한국어 — 번역 업체 전달용)
```
【무선기판(wlan0) 네트워크 배정 요청】

■ 배경 / 목적
· 귀사 운용 네트워크 토폴로지(8p) "PC로부터 STA에 Access 필요" 실현 +
  반송제어(MCP ↔ OPC Board) 정상 동작.
· 무선기판은 OPC Board와 동일 MAC으로 단일 무선 단말(STA)로 접속 →
  같은 서브넷이면 ARP 충돌 → IP 서브넷 분리 필요.

■ 게이트웨이 설정 개요
[OPC Board]   172.22.131.x (/17) / GW 172.22.129.254 (기존 유지) → MCP 반송제어
[무선기판 wlan0] 신규 별도 서브넷(/24~/29) / GW 신규(=.129.254와 동일 L2 세그먼트 secondary) → NTP·PC
[eth0(유선)]  172.22.186.1/17 (OPC와 동일 서브넷, 폐쇄망 고정) — DFK 배정 불요

■ 요청
1. 서브넷: OPC(172.22.128.0/17)·AP-HUB(172.22.96.0/20)·services(172.22.0.0/22)와
   겹치지 않는 빈 대역에서 소규모 서브넷 1개(/24 또는 /29) 배정.
2. 게이트웨이: 위 서브넷 GW를 기존 OPC GW(172.22.129.254)와 동일한 L2 세그먼트
   (무선기판 접속 AP의 동일 구간)에 secondary(추가 IP)로 설정.
   · ARP 직접 해석 필요 → AP-HUB 백홀망(/20)·코어망(CORHUB)에는 둘 수 없음.
   · 당사 이해상 172.22.129.254 보유 장치(AP-HUB 추정)의 client측 인터페이스,
     단 실제 보유 장치에 맞추어 추가.
3. L2 세그먼트 동일: 무선기판·OPC는 단일 STA → 별도 VLAN/세그먼트 분리 불가.
   세그먼트는 동일, IP 서브넷만 분리.
4. 도달성: 본 서브넷이 services망(172.22.0.0/22)에서 라우팅 도달 가능하도록
   경로 등록(필요 시 정적 경로)까지. (PC ↔ wlan0 양방향)

■ 확인 요청
· 172.22.129.254 보유 장치명 + 해당 장치/세그먼트에 secondary GW 추가 가능 여부.
· AP/무선 컨트롤러가 단일 STA MAC에 복수 IP(OPC IP + 무선기판 IP) 허용 여부
  (DAI / port-security / DHCP snooping 정책).
```

### 11.4 이번 세션 확정/정정
- GW 입장: "브릿지라 GW 불요" → **장치 자신의 cross-subnet 통신엔 GW 필요**(secondary). (부록과 일치)
- eth0 IP: 폐쇄망이라 사설(192.168.x) 검토했으나 **OPC 직접 관리엔 동일 서브넷 필요** → `172.22.186.1/17` 확정(데이터 경로는 L2 투명이라 무관).
- 게이트웨이 호스팅: `.129.254`=AP-HUB 추정(확정은 DFK 회신) / services GW `.1.254`=CORHUB(services 세그먼트 직속, AP-HUB는 그 세그먼트에 인터페이스 없음).

### 11.5 DFK 확인 ⓑ(동일 MAC 복수 IP) 불허 시 대응 방안
> 클론 MAC(M)에 OPC IP + wlan0 IP **2개**를 얹는 설계가 AP/WLC의 **"1 MAC = 1 IP" 정책**(DAI/IPSG 등)에 막힐 경우.
> **주의**: 이는 *"IP 주소 충돌"*이 아니라 *"MAC당 IP 개수"* 정책 문제 — **서로 다른 서브넷이어도 막힐 수 있음**(서브넷 분리는 ARP flux만 해결, §4 참조).

| 우선 | 방안 | 내용 | 비용 |
|---|---|---|---|
| **1순위 (권장)** | DFK 인프라 **정적 바인딩 예외** | 클론 MAC M에 OPC IP + wlan0 IP **2개를 정적 바인딩 등록**, 또는 해당 포트/VLAN의 DAI·IPSG **완화** | DFK 설정 추가 1회 — **재설계 불필요, 당사 설계 그대로 유지** |
| **2순위** | wlan0 무선 관리 IP 포기 → **유선(eth0) 관리 전환** | opcd 제어·NTP를 eth0 관리망 경유. 무선엔 OPC IP만(1 MAC=1 IP) → DAI 통과 | **"PC→STA 무선 직접 접근" 포기**(유선 관리망 경유만), NTP 유선측 필요 |
| **3순위 (개발)** | 무선 관리용 **별도 MAC (dual-STA)** | 클론 M(OPC 브릿지) + BD 자체 MAC(관리)을 2 STA로 분리 → 각 MAC당 IP 1개 | **현재 Dual station 미지원**(사양 R1, 88W9098 단일 mlan0) → FW/드라이버 개발, 단기 비현실적 |

**권장 흐름**: 방안 1 먼저 요청 → 정책상 불가 시 방안 2(유선 관리 후퇴) → 방안 3은 차기 개발 과제.

### 11.6 용어 — L2 보안 기능 (MAC↔IP 바인딩)
- **DHCP Snooping**: DHCP 교환을 엿들어 "포트·MAC·IP" 바인딩 장부 작성 (DAI/IPSG의 기반).
- **DAI (Dynamic ARP Inspection)**: ARP의 MAC↔IP가 장부와 불일치 시 드롭 (ARP 스푸핑 방지).
- **IPSG (IP Source Guard)**: 패킷 출발지 IP(+MAC)가 장부와 불일치 시 드롭 (IP 스푸핑 방지).
- **port-security**: 포트당 MAC 개수 제한 (한 MAC에 IP 2개인 우리 케이스엔 영향 적음).
- → **우리 영향**: 클론 MAC M이 장부에 IP 1개로 등록되면 2번째 IP(wlan0)의 ARP/트래픽이 드롭. **정적 바인딩으로 2 IP 등록 시 통과**(=방안 1).

### 11.7 relayd 상세 & 당사 방식 진화 이력
**relayd(일반·다중 클라) 동작 모델**
- 무선 STA는 **mlan0(자기) MAC 1개**로 AP에 등록.
- 클라 프레임을 L3에서 가로채 **src MAC을 mlan0 MAC으로 치환**(IP는 보존), **IP별 host route + Proxy ARP**로 demux.
- **서브넷을 바꾸지 않음** → 클라가 upstream과 같은 서브넷처럼 보이는 **L3 릴레이(pseudo-bridge)** (진짜 라우터=서브넷 분리와 다름).
- 결과: 무선엔 **"자기 MAC 1개 + 클라 IP 여러 개"** → **relayd도 strict DAI/IPSG엔 동일하게 걸릴 수 있음**(우리 방식만의 약점 아님). "1 MAC = 多 IP"를 피하는 건 **WDS(4-addr)**뿐.

**당사 방식(③)과의 차이**: relayd = 자기 MAC + Proxy ARP로 IP 릴레이 / 당사 = **OPC MAC 클론**으로 L2 직접 연장(Proxy ARP 불요).

**진화 이력**
- 초기: 고객 요건 *"L2 단일 클라 브릿지"* 충족 위해 **MAC 스푸핑 + relayd**로 **(당사 환경)** 데모 진행. ※ **DFK 환경 검증 아님.**
- 현재: 단일-클라 L2 전용으로 다듬어 **전용 wbridge(3-addr L2 clone)** 재구현 → relayd의 L3 릴레이·Proxy ARP 오버헤드 제거, 순수 L2 forward로 최적화. (즉 ③은 "relayd+스푸핑 데모"의 진화형)
- 시사점: ① 데모는 당사 환경(보안기능 미적용 추정)이라 **DFK DAI/IPSG는 §11.5 ⓑ로 별도 확인 필요**. ② 데모는 OPC IP 1개였을 가능성 → **wlan0 2번째 IP는 신규 변수**(데모 성공이 2-IP 보장 아님).

---

## 부록 — 이 분석에서 정정된 사항 (혼선 방지)

| 초기 서술 | 정정된 결론 |
|---|---|
| "기본 토폴로지는 mlan0-IP" | 실제 출하는 **eth0-IP**(`arp_ignore_always=true`). mlan0는 타서브넷 무선용 |
| "eth0이 무선 서브넷에 노출되어 여러 186.1이 충돌" | **eth0 IP는 무선에 노출 안 됨**(arp_ignore). 무선 노출은 mlan0 IP·OPC Board IP |
| "mlan0에 IP가 없다" | mlan0에 IP 있음(무선 통신용, 타서브넷) |
| "서브넷을 바꿔도 MAC 충돌 못 푼다" | MAC은 고정이나, **다른 서브넷이면 ARP 경합은 회피**됨(사용자 검증) |
| "브릿지라 GW 불요" | 장치 자신의 cross-subnet 통신엔 **GW 필요** |
