# DFK opcd 프로토콜 협의 — 회의 자료 (2026-06-26)

> **목적**: DFK(고객사)에 전달한 문의(`tmp/DFK_opcd_protocol_QA.md`) 21문항 + 당사 추가 제기 6건에 대해, 회의에서 **(질문 배경 → 우리 입장/근거 → 구현 현황 → 받아야 할 결정)** 순으로 설명·확정하기 위한 발표 자료.
> **기준 사양서**: 無線基板共通制御通信仕様書 Rev1.00 (2026-05-25, 落合庸央) = `docs/spec/opc_vhl_protocol_Rev1.00_KO.md`
> **교차 근거**: 발송 레터 `docs/spec-inquiry/spec-inquiry-letter.md`(Q1~Q20) · 마스터 분석 `docs/spec-inquiry/spec-inquiry.md` · 구현현황 `docs/implementation/implementation-status.md` · 적합성 `docs/spec-inquiry/spec-conformance.md`(A#/D#/V#) · `docs/spec-inquiry/proto-todo.md`(T#) · 실타깃 검증 `tmp/device_test_*.md`
> **회의 우선순위**: 토폴로지 > 무선 설정 > Indication > 에러 코드 > 기타
> **인용 검증**: 본 자료의 §절·에러코드(0x…)·코드 파일:라인은 실제 사양서·소스로 교차 검증 완료(코드 라인 drift 보정 반영).

---

## 0. 한눈에 보기 — 전 27항목 결정 요청표

> 우선순위: 〔상〕 블로킹 · 〔중〕 진행 가능하나 확정 필요 · 〔하〕 확인 권장

| 항목 | 우리 입장(발언 포인트) | DFK 결정 요청 |
|---|---|---|
| **T1 설정 IP=유선/무선** 〔상〕 | 설정·조회 IP = **유선 eth0 관리 IP**, 무선은 별도 서브넷, 데이터는 투명 L2 브릿지 | 토폴로지 모델 일치 확인 + 인터페이스 귀속 명문화 |
| **T2 게이트웨이 불요** 〔중〕 | 브릿지라 GW **검증·에코만, 미적용** | GW 필요한 운용 시나리오 존재 여부 |
| **T3 GW/NTP 미지정 허용** 〔중〕 | GW=0·NTP=0 **미설정 수용**(NTP=0은 사양 일탈) | §3.3.6 NTP 0.0.0.0 금지 완화 동의 |
| **T4 NTP 망 위치** 〔중〕 | 다른 서브넷 NTP 등록 **인지함**, cross-subnet 라우팅은 장치 책임 외 | NTP 망 위치·라우팅 책임 주체 |
| **T5 실 라인 구성도 요청** 〔상〕 | T2~T4 일괄 확정의 **전제 산출물** | 각 장치 IP/mask/GW/NTP + AP/컨트롤러 토폴로지 제공 |
| **R1 듀얼스테이션 미동작** 〔중〕 | Single만 유효, Dual은 파싱만·미발행 | **Dual 납품 범위 제외** 확정 |
| **R2 SetRadio OK 의미** 〔상〕 | OK = **설정 기록+적용(reconfigure)**, 재접속 대기 안 함 | OK 판정 시점 (a)적용 vs (b)재접속 확정 |
| **R3 ESSID 위치** 〔중〕 | 현재 IP리스트 구현, **무선설정 이관 제안** | IP리스트에 묶은 의도 + 이관 가부 |
| **R4 미접속 시 필드값** 〔중〕 | 미접속 시 FREQ/CH/Mode/BW=**설정값**, RSSI 등=0 | 미접속 필드 규정 확정 |
| **R5 11r/k/v/ai 의미** 〔중〕 | **칩 정적 지원여부**(88W9098: r/k/v 지원, ai 미지원) | 지원여부 vs 링크상태 확정 |
| **I1 임계·단위** 〔상〕 | 임계 80%(가변)·CPU/Disk=%·Net=Mbps·Memory 미발행(swapless) | 임계/단위/Memory 미발행 확정 |
| **I2 Indication 적용 범위** 〔중〕 | mlan0(주채널)만 발행, 단일 unicast 수신처 | 싱글 적용·wlan_id 부재 비범위 합의 |
| **I3 주기 내 다중 사상** 〔중〕 | 이벤트성=**에지 트리거 즉시·개별**(seq 가산), 주기는 폴링만 | 에지 모델 vs coalesce 스냅샷 |
| **E1 무효문자 정의** 〔상〕 | NUL종단만 구현, 문자집합 미정(0x0015 예약) | ASCII printable 한정 기준 확정 |
| **E2 0x0002/0x0013 구분** 〔상〕 | 0x0001=미로그인, 0x0013=타IP, **0x0002 미사용** | 두 코드 역할 확정 |
| **E3 start 없는 boundary** 〔상〕 | 비정상은 NG, **0x0018 신설 제안** | NG 처리 + 0x0018 채택 가부 |
| **M1 List Boundary 상충** 〔중〕 | **개시0x0000/계속0x0001/완료0x0002**(24p 기준) | 값 확정 + 22p↔24p 정정 |
| **M2 헤더 64=8+56** 〔하〕 | 고정 8 + Reserve 56(@8~63), body@64, Len=전체−8 | 구성 확인 |
| **M3 Reset Ack Length** 〔중〕 | 도면 0은 오타, **Length=60** 채택 | Length=60 확정 + 도면 정정 |
| **M4 자동로그아웃 IP변경** 〔중〕 | 명시적 Logout 없으면 **대기 IP변경 폐기** | 폐기 동작 확인 |
| **M5 Device Status 기동중** 〔중〕 | 0x00000000 = **pre-READY 부팅구간**, 패닉은 별도 통지 | 의미·지속시간·VHL 대기정책 |
| **+G1 기본 UDP 포트** 〔상〕 | 임시 기본 **50607** + Config override | 표준 기본 포트 확정 |
| **+G2 Protocol Version 협상** 〔하〕 | best-effort(버전 거부 안 함) | 불일치 시 거부 vs 하위호환 |
| **+G3 자율리셋 트리거** 〔상〕 | 배관 완비, **트리거·cause 미정**으로 producer 보류 | 트리거 조건 + 신규 cause 값 |
| **+G9 KeepAlive Timestamp** 〔하〕 | ISO-8601 UTC·시스템클럭(NTP 의존) | 형식·동기원 확정 |
| **+G10 그림2-2 2124MHz** 〔하〕 | 2412MHz 오기 추정 | 오타 확인·문서 정정 |
| **+G11 device-info FREQ 출처** 〔중〕 | 설정값 기본(스펙 엄수), **토글 보유**(config/live/auto) | 표준 의미 설정값 vs 접속값 |

> **블로킹(상) 우선 처리 순서**: **T5(구성도) → T1 → R2 → E1·E2·E3 → I1 → G1·G3**. 특히 **T5 네트워크 구성도**는 T1~T4 토폴로지 일괄 확정의 전제이므로 회의 초반에 요청.

---

## 1. 네트워크 토폴로지 〔우선순위 1위〕

### T1. 설정·조회 IP = 유선(eth0) 관리 IP인가 〔상〕
- **DFK 문의**: 사양이 설정/조회하는 IP가 유선용인지 무선용인지, 그리고 우리 토폴로지 모델(유선=VHL 1:1 동일서브넷·무선=별도서브넷/IP없음·데이터=투명 L2 브릿지·동일서브넷 시 ARP 경합)이 사양 의도와 맞는지.
- **사양 근거(§3.3.4 / §3.3.6 / §3.1.1)**: §3.3.4는 **Ethernet MAC만 "유선 LAN MAC"**으로 적시하고, 같은 블록의 IP/SubnetMask/Gateway/NTP·§3.3.6 IP 리스트가 유선/무선 중 무엇인지 **미정의**. §3.1.1 "VHL과 무선 기판은 기본적으로 유선 LAN(Ethernet)으로 통신". 투명 L2 브릿지·MAC 클로닝·ARP 경합 모델 자체가 사양에 없음.
- **질문 배경**: 설정 IP를 데이터 경로로 오해하면 토폴로지 전체가 틀어짐. 데이터 전달이 L2 브릿지인지 L3 라우팅인지 사양에 모델이 없어 설정 IP 역할이 모호.
- **우리 입장(발언 포인트)**: 이미 **"OPC 설정 IP(SetIPConfigList/ChangeIpAddress) 및 GetDeviceInfo의 IP/Netmask/Gateway = 유선 eth0 관리 IP에 적용·조회"**로 구현·확정. 무선(mlan0)은 별도 서브넷 관리 IP를 갖거나 IP 없음. 사용자 데이터는 투명 L2 브릿지(유선측 MAC 동적 클로닝)로 전달, L3 라우팅 안 함 → 관리 IP와 무관하게 end-to-end 성립. 유·무선 관리 IP가 동일 서브넷이면 ARP 경합 → 서로 다른 서브넷.
- **구현 현황**: GetDeviceInfo가 eth0에서 조회 `handler.c:541-545`(get_ntp_server/get_eth_mac/get_eth_ipv4_host/get_eth_netmask_host/get_eth_gateway_host). ChangeIp는 eth0에만 `ip addr` 적용 `platform_nxp.c:915-978`(nxp_apply_ip_change). 브릿지·MAC 클로닝 자체는 opcd 밖(보드 OS `wifi_init.sh`).
- **실측**: `tmp/device_test_192.168.214.5_20260616_reverify_sweep.md:3` — 타깃 eth0=관리망(214.5)/mlan0=WLAN, VHL=214.3이 동일 214.x에서 1:1 동작 확인.
- **받을 결정**: 위 모델이 사양 의도와 일치하는지 확인 → 일치 시 사양/부속서에 **인터페이스 귀속 명문화** 요청.
- *내부 참조*: spec-inquiry 10·letter Q7 · 브릿지 #27(확정)

### T2. 기본 게이트웨이가 필요한 경우가 있는가 〔중〕
- **DFK 문의**: 브릿지라 장치는 L3 라우터가 아니므로(포워딩 off, default route 없음) 게이트웨이 불필요 — 실제 필요한 경우가 있는지.
- **사양 근거(§3.3.4 / §3.3.6)**: §3.3.6 NG 조건은 `default Gateway IP 지정 이상(0x0013): IP와 다른 세그먼트의 GW 지정`뿐. GW를 **필수로 두거나 라우팅에 쓴다는 규정 없음**, 장치가 L3 라우터로 동작한다는 규정도 없음.
- **질문 배경**: 사양이 GW 용도(라우팅 경로 vs 메타데이터)를 정의하지 않아 필수처럼 비침. 우리 장치는 브릿지라 GW가 동작에 관여 안 함.
- **우리 입장**: 이미 **"GW는 검증(D1: 엔트리 서브넷 내)·에코만, 적용 안 함"**으로 구현·확정(#27, 2026-06-12). 회의에선 미적용을 설명하고, GW가 실제로 필요한 운용 시나리오가 있는지 **역으로 확인**받음.
- **구현 현황**: 검증만(다른 세그먼트면 0x0013 NG) `handler.c:135-142`. 미적용 주석 `platform_nxp.c:910-913`("gateway is a non-goal … validated … echoed but never applied").
- **실측**: 동 sweep — 게이트웨이 없이 제어통신 정상(적용 경로 자체가 코드에 없음).
- **받을 결정**: "브릿지에선 GW 검증·에코만·미적용"이 맞는지 + GW가 필요한 구성이 있으면 시나리오·라우팅 책임 명시.
- *내부 참조*: spec-inquiry 11·G6·letter Q8 · conformance D1 · #27

### T3. Gateway·NTP 미지정(0.0.0.0) 허용 〔중〕
- **DFK 문의**: GW·NTP 없는 망을 위해 미지정 허용 검토.
- **사양 근거(§3.3.6)**: **GW(0x0013)은 0.0.0.0 금지 조항 없음** vs **NTP(0x0014)는 "0xFFFFFFFF/0.0.0.0 등 지정 불가"로 명시 금지** → 같은 §3.3.6 내 선택 필드 취급이 **상충**.
- **질문 배경**: GW·NTP 둘 다 없는 망이 현실에 존재하는데 NTP 0.0.0.0 금지는 운영현실과 정면 충돌.
- **우리 입장**: 이미 **GW=0·NTP=0 모두 미설정 수용**. 단 근거가 다름 — GW=0은 **사양 적합**, NTP=0은 0x0014가 금지하므로 **의도적 사양 일탈(deliberate deviation)**. 주장: GW·NTP는 운용상 선택 필드 → "0=미설정 허용"을 명문화하고 **NTP 0.0.0.0 금지 조항 완화** 요청.
- **구현 현황**: `handler.c:135`(GW≠0 가드)·`handler.c:143`(NTP≠0 가드), 의도 차이 주석 `handler.c:115-123`("GW=0 is spec-compliant; … accepting NTP=0 is a deliberate deviation"). 상수 `ids.h:138`(GW=0x0013)·`ids.h:139`(NTP=0x0014).
- **실측**: `tmp/device_test_192.168.0.100_20260604.md:51` — NTP 읽기 검증. NTP=0 수용 자체 스윕은 미실측.
- **받을 결정**: GW·NTP 미지정(0.0.0.0) 허용 확정 + §3.3.6 NTP 0.0.0.0 금지 완화 동의.
- *내부 참조*: spec-inquiry G6·letter Q8 · conformance D1 · #35

### T4. NTP 서버의 망 위치·cross-subnet 책임 〔중〕
- **DFK 문의**: NTP를 무선망 측에 두면 VHL 서브넷과 다른 서브넷 NTP를 등록해야 하는데 이를 인지하는지.
- **사양 근거(§3.3.4 / §3.3.6)**: NTP Server IP 필드만 정의, **NTP 위치(유선/무선/관리망)와 서브넷 간 동기 라우팅 책임 미정의**.
- **질문 배경**: 유선(eth0)·무선(mlan0)이 다른 서브넷이고 장치는 L3 라우팅 안 함 → NTP를 무선망에 두면 다른 서브넷 NTP 등록 필요, 도달은 장치 책임 밖일 수 있음.
- **우리 입장**: 다른 서브넷 NTP 등록 필요성을 **인지함**. 다만 장치는 투명 L2 브릿지·L3 미수행이므로 **서브넷 간 NTP 도달(라우팅)은 장치 책임 외**라고 이해.
- **구현 현황**: NTP **읽기 전용**만 구현 `nxp_get_ntp_server(platform_nxp.c:621)`, **쓰기/적용 미구현** `platform_nxp.c:914`(주석 "essid/ntp remain V3 on-target work") — implementation-status §1.2(timesyncd.conf write+재시작 on-target 검증 대기).
- **받을 결정**: "NTP 망 위치 가정 + cross-subnet 라우팅은 장치 책임 외"가 맞는지 + 실제 NTP 망 위치를 구성도로 명시.
- *내부 참조*: spec-inquiry 12·letter Q9 · implementation-status §1.2(V3)

### T5. 실제 운용 라인 네트워크 구성도 요청 〔상〕
- **DFK 문의**: 각 장치 IP/mask/GW/NTP + AP·컨트롤러 구성을 포함한 실 라인 토폴로지.
- **사양 근거(§2)**: §2 그림 2-1/2-2는 `[VHL]—Ethernet—[무선기판]~주파수`의 **추상 1-링크 도면뿐**, 실 주소 배치는 미정의.
- **질문 배경**: T1~T4 확정은 모두 실제 주소 배치에 의존 → 구성도 없이는 토폴로지 모델을 사양으로 못박을 수 없음.
- **우리 입장**: **이 항목이 토폴로지 그룹 일괄 확정의 전제**. 구성도 한 장을 받으면 우리 브릿지 모델과 대조해 T1~T4 구현을 즉시 고정.
- **현황**: 우리 측 실타깃(eth0 214.5/mlan0 WLAN/VHL 214.3)은 확보, **DFK 실 라인 전체 구성도는 미보유 → 요청**.
- **받을 결정**: 실 운용 라인 구성도(장치별 IP/mask/GW/NTP·유무선 서브넷 분리·AP·컨트롤러) 제공.
- *내부 참조*: spec-inquiry 18·letter Q10(T1~T4 확정 전제)

---

## 2. 무선 설정 〔우선순위 2위〕

### R1. 듀얼 스테이션 미동작 — 싱글만 유효 〔중〕
- **DFK 문의**: 사양 프로토콜은 따르되 Dual 기능은 미동작, Single만 유효.
- **사양 근거(§2.2 / §3.3.8\*1 / §3.4)**: §2.2 Dual 정의, §3.3.8(\*1) "Station Type이 Dual일 때 유효", §3.4 Indication 포맷에 **WLAN#1/#2 식별 필드(wlan_id) 부재**.
- **질문 배경**: Dual에선 두 무선기 동시 동작이나 indication에 wlan_id가 없어 출처 식별 불가. 우리 납품 타깃(88W9098, mlan0 단일)은 Dual 실HW 없어 검증 불가.
- **우리 입장**: 'Dual 미동작·Single만 유효' 확정 환영(letter Q15 답). 사양 준수로 Station Type SINGLE/DUAL 모두 파싱·검증하되 Dual 전용 필드는 SINGLE에서 무시, indication은 주 WLAN(mlan0)만 발행. **Dual 정식 제외 확정 시 wlan_id 부재(G4) 보완도 비범위**. 향후 Dual 필요 시 Indication 식별 필드 추가가 선결.
- **구현 현황**: station_type 검증(미지원 종별 `0x0010`=OPC_ERR_STATION_TYPE, `ids.h:90`) `handler.c:879-880`; 기본 SINGLE `handler.c:45-48`; DUAL 분기 `platform_nxp.c:832-888`(partial 실패 시 rc 반환); DUAL Indication wlan_id 부재로 mlan1 드롭 `opcd.c:90·97`. Dual 실HW 미보유로 V6/V16 미검증.
- **받을 결정**: **Dual을 납품 범위에서 제외, Single만 유효** 확정.
- *내부 참조*: spec-inquiry 1·Q15·G4 · conformance V6/V16

### R2. SetRadio Result=OK의 의미 〔상〕
- **DFK 문의**: OK가 (a)설정 기록·적용 완료인지 (b)AP 재접속까지인지. (불휘발 2분 예산·재접속 시간 산정에 영향)
- **사양 근거(§3.3.8)**: §3.3.8 "장치 리셋 없이 변경, 불휘발 메모리 저장", 응답 `Result: OK(0x0000)/NG(0x0001)` — **OK 성공 판정 시점 미규정**.
- **질문 배경**: 재접속까지 OK로 묶으면 association 소요(AP 거리/채널/밴드스티어링 의존)가 §3.1.3.1 응답 예산(참조계 1초/불휘발계 2분)을 비결정적으로 초과 가능.
- **우리 입장**: 이미 **(a)로 구현·확정**. OK = 설정값 불휘발 기록 + 무선기 적용(wpa_supplicant conf 편집 후 `wpa_cli reconfigure` 발행)까지, **AP 재접속(association)은 대기 안 함**. reconfigure는 ms 내 OK 반환·재association은 비동기. 실접속 FREQ/CH 결과는 §3.4.2 WlanStatusChange로 별도 통지.
- **구현 현황**: apply+persist 성공 시 OK `handler.c:907-940`(재접속 대기 코드 없음). apply 실패 시 `0x0050`=OPC_ERR_RADIO_APPLY NG `handler.c:929` + last-good 롤백 arm(`handler.c:925-929`, drain `:949-967`). ※**0x0050은 사양 미정의 제안값**, 발주처 확인 대기(`ids.h:131`, conformance D9). 비동기 주석 `platform_nxp.c:60-62`("does not await association").
- **실측**: freq 적용+영속 reconfigure 실타깃 검증(implementation-status §1.1, wlan-package PR #49).
- **받을 결정**: "OK = 설정 기록+적용(reconfigure) 시점, AP 재접속 미포함"(=(a))이 맞는지 확정. (덤: apply 실패 전용코드 0x0050 채택 가부 → E 그룹과 연계)
- *내부 참조*: spec-inquiry 17·letter Q5 · conformance D9 · implementation-status §1.1

### R3. ESSID 항목 위치(설계 제안) 〔중〕
- **DFK 문의**: ESSID를 IP리스트에 묶은 의도 + 무선설정 변경으로 이관 가능 여부.
- **사양 근거(§3.3.6 / §3.3.7 / §3.3.8)**: §3.3.6·§3.3.7에 ESSID 포함, **§3.3.8 SetRadio 요구 포맷엔 ESSID 필드 없음**(Station/Priority CH/FREQ/CH/Mode/BW만).
- **질문 배경**: ESSID는 무선 파라미터 성격이 강한데 IP리스트에 묶임. 채널/주파수와 함께 변경하는 게 운용상 자연스러움.
- **우리 입장**: 설계 제안(🔵). 현재는 사양 준수로 ESSID를 IP리스트/IP변경 경로에 구현 완료(set-ip-list 저장, change-ip는 conf 편집+reconfigure 비휘발 적용, 실타깃 검증). DFK가 (1)IP리스트에 묶은 의도와 (2)§3.3.8 이관 가부 회신 요청. **이관 확정 시 SetRadio 페이로드에 ESSID 32B 추가**(와이어 포맷 변경) 동반.
- **구현 현황**: set-ip-list essid → iplist.cfg(`handler.c:124-146`, NUL종단 0x0016 `:145`); change-ip essid → `platform_nxp.c:966-973`(비휘발 reconfigure); device-info essid 읽기 `handler.c:548`. SetRadio 코덱 `commands.c:543-587`엔 essid 필드 자체 없음.
- **실측**: change-ip essid 비휘발 적용 검증(impl §1.2); essid 읽기 `FXE3000_JHW`(conformance V4).
- **받을 결정**: IP리스트에 묶은 설계 의도 + §3.3.8 이관 가부.
- *내부 참조*: spec-inquiry 9·letter Q6(🔵) · implementation-status §1.2

### R4. 무선 미접속 시 필드값(Mode/BW/FREQ/CH) 〔중〕
- **DFK 문의**: 기판 정보 취득 시 무선 미접속이면 Mode/Band Width/FREQ/CH 값이 미정의.
- **사양 근거(§3.3.4)**: FREQ=`설정 주파수(MHz)`·CH=`설정 CH 번호`·Status=`미접속 0x0000/접속 0x0001`. Status는 미접속 정의하나 **Mode/BW/FREQ/CH의 미접속 반환값은 미정의**.
- **질문 배경**: FREQ/CH는 사양이 '설정값'이라 미접속에도 설정 캐시가 자연스럽지만, Mode/BW는 본래 live 협상 결과라 미접속 시 의미 모호.
- **우리 입장**: **미접속 시 Status=0x0000으로 명시, FREQ/CH/Mode/BW=설정 캐시값, RSSI/SNR/접속 AP MAC=0**. Mode/BW는 associated일 때만 live로 덮고 미접속/legacy(mode=0)는 설정값 fallback. (G11 표준화와 연계 — live 모드 채택 시 미접속이면 FREQ/CH도 0/0)
- **구현 현황**: ack memset(0) `handler.c:505`; live readback은 associated+mode≠0/bw유효일 때만 `:556-578`; Status `:567`; select_devinfo_freq_ch `:479-496`; 적용 `:608-618`. snapshot.c 동일.
- **실측**: 설정 5180/CH36 vs 실접속 5240/CH48 → device-info는 설정값 반환(design-device-info-freq-source.md §1). 미접속 Status=0x0000은 WlanStatusChange e2e 캡처.
- **받을 결정**: "미접속 시 FREQ/CH/Mode/BW=설정값, RSSI 등=0"이 맞는지, 아니면 미접속 시 0(무효) 강제인지. (G11과 함께 회신)
- *내부 참조*: spec-inquiry G11·letter Q20 · conformance V2/V12

### R5. 802.11r/k/v/ai = 칩 지원여부 vs 링크상태 〔중〕
- **DFK 문의**: 11r/k/v/ai 필드가 칩 지원 여부인지 링크 상태 조회인지.
- **사양 근거(§3.3.4)**: 각 "…지원. 없음(0x00)/있음(0x01)", 11k/11v는 "있음 ON(0x01)" 표현.
- **질문 배경**: '지원'이라 칩 capability로 읽히나 'ON' 표현 때문에 '링크 협상 활성'으로도 해석 여지 → 모호.
- **우리 입장**: '지원 없음/있음'이므로 **칩(silicon) 정적 capability로 해석·구현**('ON' 표현이 오인 소지라 확인 요청). 88W9098 실측+NXP Features 문서로 **11r/11k/11v=지원, 11ai(FILS)=미지원** 확정, device_info.json 정정 완료(ieee_11ai 1→0).
- **구현 현황**: ack에 정적값 그대로 `handler.c:600-603`(live 협상 조회 경로 없음); device_info.json 로드 `inventory.c:164-167`. 출하 config `etc/device_info.json:9-12`(11r=1/11ai=0/11k=1/11v=1).
- **받을 결정**: '칩 정적 지원여부'가 맞는지 + 88W9098 실측 보고값(r/k/v 지원, ai 미지원)이 라인 운용 의도와 정합인지.
- *내부 참조*: spec-inquiry 3·G5 · conformance V5 · proto-todo T5

---

## 3. Indication 〔우선순위 3위〕

### I1. 자원 임계값·판정·Current Val 단위 〔상〕
- **DFK 문의**: CPU/Memory/Disk/Network 임계값·판정 방식·Current Val 단위(CPU/Disk=%, Network=Mbps) 규정.
- **사양 근거(§3.4.5)**: Congestion ID 4종(CPU 0x0001/Memory 0x0002/Disk I/O 0x0003/Network I/O 0x0004)만 열거, **임계·단위·지속·재통지 미정의**. Memory(0x0002) 정의는 "메모리 부족→디스크 스왑 페이징 증대".
- **질문 배경**: 임계·단위 전무 + **본 타깃은 swap 없음(SwapTotal 0)** → Memory 폭주 정의(페이징)가 원리상 성립 불가.
- **우리 입장**: 잠정 정책 — (1)전 자원 공통 임계 **80%**(opc.conf `congestion_threshold_pct` 1~100 가변), (2)보고 주기마다 1샘플, (3)지속 시 매 주기 재통지, (4)단위 **CPU/Disk=%·Network=Mbps**(uint16 65535 포화). **Memory(0x0002)는 swapless라 미발행, Disk I/O(0x0003)로 일원화**.
- **구현 현황**: 임계 평가 `fault_probe.c:101-132`(net_over는 link 용량 대비 %), Mbps 포화 `:128`; 기본값 `fault_probe.h:34-35`(THRESHOLD_PCT 80/NET_CAPACITY 1000); 주기·Memory 미발행 `indication.c:103-146`("Memory (0x0002) is deliberately not produced — swapless target"); conf 키 `fault_probe.c:158`. 상수 `indications.h:108-111`.
- **실측**: swapless 실측 확인(SwapTotal 0). 단 **FaultDetect indication 자체의 와이어 캡처는 아직 없음**(현재 캡처는 WlanStatusChange e2e만).
- **받을 결정**: 임계 80%(가변)·1샘플/주기·매 주기 재통지·CPU/Disk=%·Net=Mbps 확정 + Memory **미발행(ⓑ)** 허용인지 RAM 사용률 재정의(ⓐ)인지.
- *내부 참조*: letter Q12 · proto-todo T6 · conformance V1·D7

### I2. Indication 적용 범위 — 싱글/우선 채널 〔중〕
- **DFK 문의**: Indication이 싱글 스테이션(우선 통신 채널)에만 적용되는지(= wlan_id 식별 필드 부재 문제).
- **사양 근거(§3.4.2/3/4, §3.1.3.2)**: 모든 통지 포맷에 **wlan_id 미정의**, §3.1.3.2가 단일 무선기 전제로 기술.
- **질문 배경**: Dual에선 출처 무선기 식별 불가. DFK가 '싱글만 유효'로 확정한 바와 indication 적용 범위가 정합하는지.
- **우리 입장**: '싱글만 유효' 전제에 맞춰 **indication은 주 WLAN(mlan0, idx==0)에서만 발행, mlan1 이벤트는 drop**(#35 항목6). 수신처도 **단일 unicast만 허용**(0.0.0.0/멀티/브로드캐스트 거부). 즉 'Indication = 싱글 스테이션(우선 채널 mlan0)에만 적용'. 현 납품 범위(싱글)에선 wlan_id 부재가 문제 안 됨.
- **구현 현황**: mlan0-only 정책 `opcd.c:51-62`("no wlan_id field … emit idx==0 events only"); 분기 drop `opcd.c:80-120`; unicast 검증 `valid_unicast_ipv4(handler.c:86-97)`, 적용·NG `handler.c:983`; 단일 수신처 발송 `indication.c:12-25`.
- **실측**: e2e에서 SINGLE·mlan0만 사용, 단일 수신처(214.3:50699)로 발행 확인.
- **받을 결정**: Indication을 싱글(주채널 mlan0)·단일 unicast로 발행하는 이해 확인 + **Dual 미납품 전제이므로 wlan_id 부재가 현재 비범위**임을 합의.
- *내부 참조*: letter Q15 · G4 · conformance #결정·한계 bullet · #35 항목6

### I3. 한 주기 내 다중 사상 처리 〔중〕
- **DFK 문의**: Indication Period>0일 때 한 주기 내 사상(상태변화/로밍/절단)이 여러 번이면 처리 미정의.
- **사양 근거(§3.3.9, 그림3-3, §3.2.1)**: §3.3.9 "주기마다 사상 변화가 있으면 통지"(레벨/스냅샷 시사), 그림3-3 "통지마다 Sequence Number 갱신", §3.2.1 "통지마다 seq 가산, 수신측이 중복 판단". **한 주기 내 복수 사상(모두/최신/합치기) 처리는 미정의**.
- **질문 배경**: 한 주기 안 disconnect→reconnect→roam 시 (a)모두 통지 (b)최신 1회 (c)합쳐 통지 중 미정.
- **우리 입장**: 이벤트성 3종은 **에지 트리거 — 이벤트 도착 즉시 개별 통지, 각 통지마다 seq 가산** → 한 주기 안 여러 사상도 손실/합쳐짐 없이 순서대로 전달. Indication Period는 **KeepAlive·FaultDetect 폴링에만** 적용(레벨 샘플 1회/주기), 이벤트성은 주기 무관 즉시. 즉 "이벤트성=에지 즉시·개별, 폭주/생존=주기 샘플".
- **구현 현황**: 즉시 발행 `opcd.c:80-120`(배칭/대기 없음); 주기 gating은 KeepAlive/FaultDetect만 `indication.c:103-114`; 통지별 seq `indication.c:27-30`. coalesce/dedupe 없음(의도적).
- **실측**: period=120s인데 disconnect/reconnect 두 사상이 주기 대기 없이 즉시 각각 발행([0]seq=0 DISCONNECTED, [1]seq=1 CONNECTED, Length=60) — `tmp/device_test_192.168.214.5_20260616_v1_wlanstatus_e2e.md`.
- **받을 결정**: 에지 트리거 즉시·개별(seq로 VHL이 중복/순서 판단) 모델이 맞는지, 아니면 '주기마다 최신 상태 1회 coalesce'를 원하는지.
- *내부 참조*: DFK QA 직접 제기(letter 미수록) · conformance V1·D7 · 그림3-3·§3.2.1

---

## 4. 에러 코드 〔우선순위 4위〕

### E1. 패스워드·ESSID 무효 문자 정의 〔상〕
- **DFK 문의**: 어떤 문자를 '무효 문자'로 판정할지 기준 요청.
- **사양 근거(§3.3.1/§3.3.5/§3.3.6)**: 패스워드 0x0011/0x0013·ESSID 0x0015(`문자열로 지정할 수 없는 값`) 에러코드는 있으나 **허용 문자집합 미정의**. ESSID는 IEEE 802.11 임의 바이트열 vs 사양 "최대 31자+NULL"(§3.3.6) 충돌.
- **질문 배경**: 기준 없으면 동일 입력이 펌웨어별 OK/NG로 갈려 상호운용 깨짐.
- **우리 입장**: 형식 규칙(NUL종단)만 구현, 문자집합 판정은 **발주처 정의 대기로 보류**(0x0015 UNUSED 예약). 제안: 패스워드·ESSID 모두 **ASCII printable 0x20~0x7E 한정**(제어문자·멀티바이트 무효), ESSID는 IEEE 임의 바이트열보다 사양 "31자+NULL" 우선. 회의 목표 = '무효문자 = 비-ASCII-printable'로 좁혀 확정.
- **구현 현황**: 문자집합 검증 미구현(형식만). Login NUL종단(0x0012) `handler.c:367-372`; SetPassword `handler.c:645-650`; ESSID NUL종단(0x0016) `handler.c:145`. `OPC_ERR_IPCFG_ESSID_CHAR=0x0015`는 `ids.h:140-142` UNUSED("reserved pending the A5 inquiry").
- **실측**: 무효문자 경로 미구현 → 해당 없음.
- **받을 결정**: 허용 문자 범위 확정 + ESSID에 사양 "31자+NULL" 우선 적용 가부 + 0x0015 구체 기준.
- *내부 참조*: spec-inquiry 4·letter Q1 · conformance A5·D4·D5 · #35

### E2. SetIndication 로그인 위반 0x0002 / 0x0013 구분 〔상〕
- **DFK 문의**: §3.3.9에서 '로그인 조건 위반'이 0x0002와 0x0013로 중복 기재 — 역할 구분.
- **사양 근거(§3.3.9)**: Error Cause에 `Login 조건 위반(0x0002)`와 `Login 조건 위반(0x0013): 다른 IP에서 발행`이 **동일 명칭 중복**. (Logout §3.3.2의 0x0001/0x0002는 일치하여 제외)
- **질문 배경**: 미로그인 발행과 '로그인했으나 타 IP 발행'을 어느 코드로 보고할지 미결.
- **우리 입장**: **0x0001=미로그인 발행, 0x0013=타 IP 발행, 0x0002는 SetIndication에서 미사용(중복)**으로 구현·확정. 회의에서 이 매핑 확인, 또는 0x0002/0x0013에 별도 의미를 둘지 확정.
- **구현 현황**: 공통 `check_login_required(handler.c:51)`가 미로그인→`0x0001`(proto.h:58)·타IP→`0x0002`(proto.h:59) 매핑 후, SetIndication `handler.c:979`에서 호출하고 `handler.c:1004-1009` else 분기가 타IP의 0x0002를 **0x0013으로 override**("override the common 0x0002 mapping"). 상수 `ids.h:113`(OPC_ERR_IND_OTHER_IP=0x0013, "overlap with 0x0002 — vendor inquiry"). 결과적으로 0x0002 미발행.
- **실측**: Login의 0x0002(타 IP 2중 Login 배타)는 실타깃 검증(reverify_sweep.md:24, D_implemented_features.md:37). SetIndication 0x0013 직접 실측은 별도 기록 없음.
- **받을 결정**: 0x0002/0x0013 역할 확정 — 우리 매핑(0x0002 미사용·0x0013=타IP)이 맞는지, 0x0002에 별도 의미 부여 필요한지.
- *내부 참조*: spec-inquiry 5·letter Q2 · conformance A14 · #35

### E3. start 없는 비정상 boundary 시퀀스 응답 〔상〕
- **DFK 문의**: start 없이 continue/end만 수신한 비정상 시퀀스의 Result/Error Cause 미정의.
- **사양 근거(§3.3.6)**: Error Cause는 0x0010~0x0017까지만 정의. Boundary Flag는 정의되나 **START 없는 CONTINUE/END의 응답코드 미정의**. (덤: '어느 값이 START인가'도 22p↔24p 상충 → M1 참조)
- **질문 배경**: START 없이 CONTINUE/END 도착 시 staging 미개시 상태. cause 목록에 거절 코드 없으면 일부만 커밋되는 위험(silent skip).
- **우리 입장**: 비정상은 **NG로 응답**, 미사용 코드 **0x0018 신설 제안**해 구현 — START 없는 CONTINUE/END는 엔트리 미기록·NVM 미반영으로 NG(0x0018) 반환. **0x0018은 제안값, 발주처 확정 필요**.
- **구현 현황**: `handler.c:709-724` — START면 staging seed, else !staging_active이면 NG `0x0018`(`OPC_ERR_LIST_SEQUENCE`, `ids.h:115` "FIXME: wire value unconfirmed — vendor-answer proposal"). boundary 상수 `commands.h:274-276`(START 0x0000/CONTINUE 0x0001/END 0x0002).
- **실측**: 정상 시퀀스(START→END 커밋)는 실타깃 검증(D_implemented_features.md:120). 비정상 0x0018 경로는 미실측.
- **받을 결정**: 비정상 boundary를 NG로 처리하는 방침 + Error Cause 값 0x0018 채택 가부.
- *내부 참조*: spec-inquiry 7(값상충은 13/M1) · letter Q3 · conformance A17 · #35

---

## 5. 기타

### M1. List Boundary Flag 값 상충 〔중〕
- **DFK 문의**: List Boundary Flag 값이 사양서 내 상충 — 정확한 정의.
- **사양 근거(§3.3.6)**: **22p 본문**(KO md L572) `개시0x0001/계속0x0000/완료0x0002/개시+완료0x0003` vs **24p 필드 설명**(L634) `개시0x0000/계속0x0001/종료0x0002` — **정반대**. (누락 본문 L576-578 "시작0x0000/계속0x0001"도 발견 → 2/3이 개시=0x0000 지지)
- **우리 입장**: **24p 기준 = 개시 0x0000/계속 0x0001/완료 0x0002** 채택·확정(근거: 24p가 필드 정의로 권위·벤더 구두 확인·2/3 일치). **'개시+완료 0x0003' 단일프레임 커밋은 미지원**(드롭) → START→END 2프레임만 지원.
- **구현 현황**: `commands.h:274-276`(page-24 채택), staging→atomic commit `handler.c:709-737`, pack/unpack `commands.c:410·423`. 0x0003 제거.
- **실측**: slot10 `--flag start`(미커밋, sha 불변)→slot11 `--flag end`(commit, atomic rename, sha 변경) — D_implemented_features.md:61-63.
- **받을 결정**: 개시0x0000/계속0x0001/완료0x0002 확정 + 22p↔24p 정정 + '개시+완료 0x0003' 존재 여부.
- *내부 참조*: letter Q11 · spec-inquiry 13(연계 7) · conformance D6·A17 · proto-todo T2

### M2. 공통 헤더 64B = 8 + Reserve 56 〔하〕
- **DFK 문의**: 헤더 64B = 고정 8B + Reserve 56B(@8~63)가 맞는지.
- **사양 근거(§3.2 / §3.2.1)**: §3.2.1 본문엔 "Reserve=56B" 명시 문구 없음, **도면(바이트맵)에서만** Reserve(8~63)·body(@64) 드러남(예 §3.3.3 응답 도면).
- **우리 입장**: **64B = 고정 8B(@0..7) + Reserve 56B(@8..63, 전송 시 0)**, body @64부터, **Length = 전체 프레임 − 8**. 빈요구(Logout/GetBasicInfo/GetDeviceInfo/Reset 요구)는 Reserve 없이 8B만 → Length=0.
- **구현 현황**: `proto.h:30-42`(FIXED 8/HEADER 64/PAYLOAD 1360/FRAME 1424, static_assert 64+1360==1424 및 8<64).
- **실측**: 실프레임 — basic-info Reserve 56B 전부 00, body@64, Length=전체−8(72=80−8, 408=416−8, 88=96−8) — h64_redeploy_e2e.md:77-100.
- **받을 결정**: 64B=8+56·body@64·Length=전체−8 + 빈요구 Reserve 생략(Length=0) 확인.
- *내부 참조*: letter Q17 · spec-inquiry 2 · conformance A8 · proto-todo T3·T10

### M3. Reset Ack Length 〔중〕
- **DFK 문의**: §3.3.10 리셋 응답 도면이 Length=0인데 Reserve+Result/Error Cause를 포함(자기모순).
- **사양 근거(§3.3.10)**: 응답 도면이 **Length=0**으로 적혔으나 Reserve(8~63)+Result/Error Cause(64~67)를 모두 포함 → 도면 자체 모순. 다른 단순 응답은 Length=60.
- **우리 입장**: 일관성 위해 **Length=60** 채택·확정(body 4 + Reserve 56). 도면의 0은 리셋 '요구'(진짜 빈 요구)에서 복사된 오타로 판단. §3.4.6 리셋통지도 Length=60으로 일관. (사내 T15 RESOLVED이나 상호운용에서 즉시 드러나므로 회의 확정 권장)
- **구현 현황**: `commands.h:426`(OPC_RESET_ACK_LENGTH 60, "spec '0'은 Reset Req 복붙 오타"), pack `commands.c:673-679`, handle `handler.c:1016-1029`.
- **실측**: reset→OK→ResetNotice(cause=USER)→재기동, Length=60 ack 정상 파싱 — D_implemented_features.md:70-81.
- **받을 결정**: Reset Ack Length=60 확정 + 도면 Length=0 오타 정정.
- *내부 참조*: letter Q18 · spec-inquiry G8 · proto-todo T11·T15(RESOLVED). *(레터는 〔하〕이나 와이어 불일치 리스크로 〔중〕 격상)*

### M4. 자동 로그아웃 시 IP 변경 처리 〔중〕
- **DFK 문의**: IP 변경 후 5분 자동 로그아웃(timeout) 시 대기 IP변경을 적용 안 하고 폐기하는지.
- **사양 근거(§3.3.7 / §3.3.1)**: §3.3.7 "IP 변경은 Logout 응답 송신 후 적용, 불휘발 저장 안 함", §3.3.1 "5분 무수신 시 자동 Logout". **자동 로그아웃 시 대기 IP변경의 적용/폐기 미규정**.
- **우리 입장**: 자동 로그아웃엔 명시적 Logout이 없으므로 **대기 IP 변경을 적용 안 하고 폐기**. 명시적 Logout만이 deferred ChangeIp의 commit 신호이고, idle 로그아웃은 commit을 arm 안 하고 teardown → pending 자연 폐기. 다음 fresh login 시 이전 미완료 ChangeIp도 cross-session 가드로 drop.
- **구현 현황**: idle teardown `handler.c:73-77`(commit arm 안 함); explicit Logout만 arm `handler.c:432-447`; fresh login 가드 `handler.c:386-397`; pending set `handler.c:795`. conformance A12와 정합.
- **실측**: idle 타이머 주입(`-i 3`)으로 "idle auto-logout" 로그 확인 — D_implemented_features.md:44-51.
- **받을 결정**: 자동 로그아웃 시 대기 IP 변경 폐기가 맞는지 확인.
- *내부 참조*: letter Q13 · spec-inquiry 8 · conformance A12

### M5. Device Status 기동 중(0x00000000) 정의 〔중〕
- **DFK 문의**: 기동 중 상태 0x00000000이 부팅/초기화/무선랜 모듈 패닉 중 무엇인지.
- **사양 근거(§3.3.3 / §3.4.1)**: §3.3.3 "0x00000000 = Login·설정 처리 불가 상태", §3.4.1 InitComplete "초기 개시(리셋·Power ON) = 0x00000000". **부팅/초기화 구분·지속시간·패닉 포함 여부 미정의**.
- **우리 입장**: opcd는 프로세스 기동 즉시 READY(0x00000001)로 천이하므로, 와이어상 0x00000000은 실질적으로 **opcd가 UDP 소켓 bind 전(커널/드라이버/wpa_supplicant bringup) 구간** = 어떤 UDP 요구에도 응답 불가. 즉 **0x00000000 = pre-READY 부팅·초기화 구간**. 런타임 무선랜 모듈 **패닉은 Device Status 복귀가 아니라 별도 FaultDetect(0x0010)/ResetNotice(0x0020)로 통지**.
- **구현 현황**: 상수 `ids.h:69-71`(BOOTING 0/READY 1/LOGGED_IN 2); BOOTING→READY `opcd.c:138→287`; booting Login 방어분기(도달불가) `handler.c:363-364`; ack 반영 `handler.c:467·605`.
- **실측**: basic-info ack device_status=0x00000001(pre-login READY)→login 후 0x00000002. 0x00000000은 실측 미관측(opcd가 항상 READY로 응답) — h64_redeploy_e2e.md:108·160.
- **받을 결정**: 0x00000000 의미(부팅/초기화만인지, 패닉 포함 여부)·예상 지속시간·VHL 대기/재시도 정책 + 패닉은 별도 통지하는 우리 해석 확인.
- *내부 참조*: letter Q14 · spec-inquiry 15 · conformance D11·D15

---

## 6. 추가 제기 권장 〔DFK 미포함 — 당사 발신〕

> DFK 문서에는 없으나 사양 미정의·TBD로 남아 회의에서 함께 확정하면 좋은 항목.

### +G1. 기본 UDP 제어 포트 확정 〔상〕
- **사양(§3.1.2)**: "제어 포트는 Config로 지정 가능. **기본 포트는 (TBD) 향후 결정**".
- **우리 입장**: 사내 임시 기본 **50607**(private 49152~65535) + opc.conf override. Indication 수신포트는 SetIndication 와이어로 매번 도착하므로 별도 기본 불요. DFK에 '50607 표준 채택 가부' 확인.
- **구현**: `opcd_state.h:30`(OPC_DEFAULT_UDP_PORT 50607), `opcd.c:125/252/288`. proto-todo T1 RESOLVED.
- **받을 결정**: 기본 UDP 포트 50607 확정 가부(또는 DFK 지정 포트).

### +G2. Protocol Version 불일치 처리 〔하〕
- **사양(§3.2.1, §3.3.3)**: 버전 채움 규칙(요구=최고지원/응답=구현버전)은 정의, **불일치 시 거부/best-effort 미정의**.
- **우리 입장**: 현재 단일 버전 0x01, opcd는 버전 검증·거부 안 함(**best-effort**). 응답에 구현버전 반환, 요구측이 하위호환 판단 권장. (단일 버전이라 〔하〕)
- **구현**: `proto.h:28`(0x01), codec read/write `codec.c:37·59`, 응답 항상 구현버전 `frame.c:24·46` — 거부 로직 없음.
- **받을 결정**: 향후 버전 분기 시 (a)거부 vs (b)best-effort 방침.

### +G3. ResetNotice 자율리셋 트리거·cause 〔상〕
- **사양(§3.4.6)**: "자율리셋 전 통지" + "Reset Cause = 리셋 요인 고유 ID"만 규정, **트리거 조건·신규 cause 값 미정의**.
- **우리 입장**: 통지 배관(emitter/event/consumer) 완비, 명시적 Reset(cause=USER=0x00000001)은 동작. **자율리셋 트리거 정책이 사양에 없어 자율 producer만 보류**. 트리거 조건+cause 체계 확정 시 producer만 추가하면 완성.
- **구현**: pack `indications.c:177`, emitter `indication.c:81`, 명시적 reset만 USER `handler.c:1025`(`ids.h:153`=0x00000001), consumer 준비됐으나 `platform_nxp.c:1305` nxp_drain_events no-op(PEVT_RESET_NOTICE 미생산). proto-todo T9 DEFERRED, #35-5.
- **받을 결정**: (a)자율리셋 트리거 조건(자원 폭주 지속/watchdog 등) + (b)USER 외 신규 reset cause ID 체계.

### +G9. KeepAlive Timestamp 형식·동기원 〔하〕
- **사양(§3.4.7)**: Timestamp "최대 31자 문자열, 초 단위, ex 2026-02-16T15:47:00Z". **정확한 형식·시각 동기원 미정의**.
- **우리 입장**: 예시가 ISO-8601 UTC(Z)이므로 그 형식 채택 구현, 시각은 시스템 클럭(gmtime)→**NTP 동기 의존**(NTP 미설정 시 부정확 가능, Q8 연계).
- **구현**: pack `indications.c:203`, `indication.c:139-140`(gmtime_r→strftime "%Y-%m-%dT%H:%M:%SZ").
- **받을 결정**: ISO-8601 UTC 형식 확정 + 시각 동기원 NTP 필수 여부.

### +G10. 그림 2-2 '2124MHz' 오기 〔하〕
- **사양(§2.2 그림 2-2)**: Dual "5180MHz + 2124MHz". **2124MHz는 2.4GHz 유효 주파수 아님**(ch1=2412MHz부터). §3.3.4 예시도 2412MHz.
- **우리 입장**: **2412MHz 자리바꿈 오기 추정** — 도면 표기 오류, 기능 영향 없음, 문서 정정만 요청.
- **구현**: 코드 영향 없음(주파수 인코딩은 §3.3.4 표대로 정상).
- **받을 결정**: 2124MHz가 2412MHz 오기인지 확인·문서 정정.

### +G11. device-info FREQ/Channel = 설정값 vs 접속값 〔중〕
- **사양(§3.3.4)**: FREQ/Channel을 "**설정** 주파수/CH"로 정의. 그러나 운영 현장은 실접속값(밴드스티어링 시 설정≠접속)을 기대.
- **우리 입장**: 사양 문언대로 **설정값 기본(출하 기본=config, 스펙 엄수)**. 양쪽 대응 위해 토글 `device_info_freq_source`(config[기본]/live/auto) **이미 신설·배선 완료** → DFK가 접속값을 표준으로 정하면 **opc.conf 한 줄로 재빌드 없이 전환**. (실시간 FREQ/CH는 §3.4.2/§3.4.3 indication으로도 통지)
- **구현**: `freq_source.c:6·14`, 기본 CONFIG `opcd.c:128`, 파싱 `opcd.c:257`, select `handler.c:479-496`, 호출 `handler.c:608·615`. design-device-info-freq-source.md.
- **실측**: 설정 5180/ch36 vs 실접속 5240/ch48 불일치 실측, config 모드는 설정값 반환 — design §1, v1_wlanstatus_e2e.md:9·34.
- **받을 결정**: 표준 의미 (a)설정값[현 기본·사양 문언] vs (b)접속값 → 회신 시 출하 기본을 코드 변경 없이 확정.

---

## 7. 회의 진행 가이드

1. **먼저 T5(네트워크 구성도) 요청** — T1~T4 토폴로지 일괄 확정의 전제. 구성도를 받아야 우리 브릿지 모델 정합을 못박을 수 있음.
2. **블로킹(상) 집중**: T1(설정 IP=eth0) → R2(SetRadio OK 의미) → E1·E2·E3(에러코드 체계) → I1(임계·단위) → G1(포트)·G3(자율리셋 트리거).
3. **'확인'톤 항목**(우리가 이미 구현·확정): T1·T2·M1·M2·M3·M4 — "이렇게 구현했는데 맞는지 확인" 형태로 빠르게 통과.
4. **설계 제안 항목**: R3(ESSID 이관)·T3(NTP=0 완화)·G11(FREQ 출처) — DFK 결정에 따라 와이어 포맷/기본값 조정.
5. **연계 처리**: R2(OK 의미)↔E3/0x0050(apply 실패코드), R4(미접속 필드)↔G11(FREQ 출처), R1(Dual 제외)↔I2(wlan_id 비범위), T3·T4(NTP)↔T5(구성도).
6. **회신 우선순위 부탁**: 블로킹 상 항목 우선 회신 시 해당 구현 즉시 확정.

> 본 자료는 발송 레터(`docs/spec-inquiry/spec-inquiry-letter.md`)와 입장이 일치하며, 모든 §절·에러코드·코드 라인은 실파일 교차 검증(라인 drift 보정 포함) 완료.
