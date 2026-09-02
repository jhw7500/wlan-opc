# device_ip_iface — 관리 IP 인터페이스 선택자 (#89, 2026-09-02)

> 접근 리뷰(critic, 2026-09-02 REVISE→반영) 통과본. 선례: `design-device-info-freq-source.md`(G11 토글)와 동형.

## 문제

GetDeviceInfo Ack의 IP/netmask/GW가 eth0 스냅샷(`/var/log/cantops/json/eth0/link.json`) 고정이라, 관리 IP를 mlan0에 두는 운용(옵션 X/mlan0-IP 토폴로지)에서 VHL이 받는 장치 IP가 실제 관리 접점과 불일치한다(#89, hairpin 갭검토 G1).

## 접근

`opc.conf::device_ip_iface = eth0|mlan0` (출하 기본 **eth0 = 현행 동작 0 변화**). `freq_source` 선례를 미러링한 독립 모듈 `ip_iface.{h,c}`(호스트 단위테스트).

**선택자는 읽기·쓰기 양면을 결속한다** (리뷰 C1 반영 — 핵심):
- 읽기: GetDeviceInfo IP 3필드 소스 = 선택 인터페이스의 link.json
- 쓰기: ChangeIp의 runtime `ip addr` 적용 대상 = **같은** 선택 인터페이스

읽기만 전환하면 VHL의 사양 루프(§3.3.6 설정→§3.3.7 변경→§3.3.4 재조회)가 발산한다: 변경은 eth0에 조용히 적용(OK 응답)되는데 재조회는 mlan0을 보므로 VHL이 무한 재시도하며 매회 유선 IP를 무단 재할당. 이를 막기 위해 vtable `apply_ip_change(slot, iface)`로 인터페이스를 명시 전달한다.

구현 구조: `get_eth_ipv4/netmask/gateway_host` 3-getter를 **단일 `get_dev_ipv4(iface, ip, mask, gw)`로 치환** — 1회 slurp라 로거 동시 재작성에 대한 찢어진 삼중값(torn read)도 함께 해소(리뷰 minor 반영). `ethernet_mac`은 **항상 eth0**(mlan0 MAC=클론 peer MAC → 장치 오식별).

## 정책 확정 (리뷰 반영)

1. **미접속 시 응답 = 0.0.0.0 (정직 보고)** — `mlan0` 선택 + 무선 미접속이면 networkd carrier 대기로 mlan0에 IP 자체가 없어 3필드 모두 0. 이는 관리평면의 실상태다: 이 상황에서 VHL 조회는 유선(OPC Board 1:1) 경유로만 도달 가능하고, 그때 "무선 관리평면 다운"을 0으로 알리는 것이 eth0의 무관한 값보다 진단적으로 옳다.
2. **3치 `auto`(리뷰 M1 권고안) 기각** — 읽기엔 자연스러우나 **쓰기(ChangeIp 적용 대상)가 비결정**이 된다(미접속 시 eth0으로 적용? 접속 복귀 후 불일치). 자동 판단 배제 원칙(2026-09-02 사용자 지시와 동일 계열)에 따라 2치 결정론 유지. 필요해지면 읽기 전용 auto를 별도 키로 분리 검토.
3. **제3안 `IP_PKTINFO`(수신 인터페이스 기준 응답) 기각** — ①루프백 조회 시 127.0.0.1 보고 ②snapshot 발행물이 요청자별로 달라짐 ③사양의 "장치 속성" 모델과 충돌(경로별 상이 응답) ④"출하 기본=무변화" opt-in 지점 부재.
4. **MAC/IP 쌍 비정합은 명시적 트레이드오프** (리뷰 M2) — `mlan0` 선택 시 Ack는 (eth0 MAC, mlan0 IP) 조합. VHL이 쌍으로 ARP/식별 매핑하면 오매핑 가능 → **G12로 발주처 확인 등재**. MAC 동시 전환 대안은 클론 MAC 오보고라 기각.
5. **GW=0 상시 노출은 잠정 사내 정책 위** (리뷰 M3 정정) — mlan0 gateway는 link.json null 통례라 0 보고. GW=0 허용은 **발주처 미확인**(G6, `spec-conformance.md` D1) — "확인됨" 아님. G12에 ③으로 병기.
6. **DUAL(mlan1) 범위 밖** — mlan1/DBDC 비고려 결정(2026-07-17). 키 값 도메인에 의도적으로 불포함.

## 변경 파일

| 파일 | 내용 |
|---|---|
| `opcd/ip_iface.{h,c}` | 신규 — enum/token/parse + `opcd_ip_iface_idx()` 단일 매핑점 |
| `opcd/platform.h` | 3-getter → `get_dev_ipv4(iface,...)`; `apply_ip_change(slot, iface)` |
| `opcd/platform_nxp.c` | 단일 slurp 리더(eth0/mlan0 link.json — `opc_json_string` 전역 스캐너라 양 레이아웃 공통, `json_util.h` 주석 정정); apply `dev %s` |
| `opcd/platform_stub.c` | eth0=0 삼중값(기존 기대 보존) / mlan0=TEST-NET-1 고정값; apply iface 기록 |
| `opcd/opcd_state.h` / `opcd.c` / `handler.c` | conf 필드·부팅 파싱·분기 2곳 |
| tests | `test_ip_iface.c`(파서 21케이스) + `test_handler.c` 26블록(소스 전환 read + apply 결속) — **되돌림 시 FAIL 확인**(read 분기 제거 → `ip-iface mlan0` 케이스 적색) |
| docs | `testing-guide.md`·`spec-conformance.md` V3·`spec-inquiry.md` **G12 신설** |

## 운영 노트

- 키 노출 경로: opc.conf는 리포 템플릿이 없는 런타임 전용 파일 — 본 문서와 `testing-guide.md`가 노출 경로. 옵션 X 프로비저닝 시 `bd_provision.sh` 번들과의 역할: .network(영속 IP)는 스크립트, `device_ip_iface`는 opc.conf — **두 값이 같은 토폴로지를 가리켜야 한다**(불일치 시 device-info가 0 또는 무관값 보고).
- ChangeIp의 mlan0 적용도 eth0과 동일하게 **runtime-only**(재부팅 시 `20-mlan0.network` 복원)이며, wbridge IP 필터는 부팅 스냅샷이라 재기동 전 stale(#88 README와 동일 제약).
- 실기 검증 절차는 freq-source 선례(`verify-device-info-freq-source.md`) 동형으로 온타겟 시 작성 예정: 토글 off/on × 유선/무선 조회 × ChangeIp 적용 대상 확인(`ip addr` 관찰).
