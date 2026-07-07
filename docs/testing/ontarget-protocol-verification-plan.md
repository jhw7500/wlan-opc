# opcd 데몬 타겟 통신프로토콜 검증 방안

> 대상: `opc_vhl_protocol_Rev1.00_KO` 기반 **opcd / vhlctl** 을 실장치
> (**OPC = 192.168.0.100, UDP 50607, Big-Endian**, NXP88W9098 + i.MX8MM ARM64)에서
> 통신프로토콜이 사양과 와이어 레벨로 일치하는지 **증거기반·재현가능**하게 검증하는 계획.
>
> 토폴로지: 이 호스트 = **VHL**(제어측) / 실장치 = **OPC**(무선기판). `vhlctl`은 클라이언트이므로
> 호스트에서 `make native`로 빌드한다(stub/nxp 동일 바이너리).

## 본 문서의 위치 (기존 문서와의 관계)

| 문서 | 역할 |
|---|---|
| [`testing-guide.md`](testing-guide.md) | **방법**(빌드 / 단위 / 로컬 e2e / 실장치 e2e / device-info 소스). "어떻게 돌리나" |
| [`manual-runthrough.md`](manual-runthrough.md) | **1차 인수 체크리스트**(Phase 5 sign-off). "출하 전 무엇을 ☑ 하나" |
| `tmp/device_test_*.md` | **실측 결과 리포트**(시점별 PASS/FAIL + hex dump) |
| **본 문서** | **검증 방안(계획)** — 계층모델(L0~L6) + 명령/Indication 매트릭스 + 와이어 독립검증 + 증거 자동화 + 커버리지 갭. 위 세 문서를 묶는 상위 전략 |

> 모든 명령 / 플래그 / 포트 / 경로 / ErrorCause hex 는 소스(`protocol/ids.h`, `protocol/commands.h`,
> `opcd/opcd.c`, `opcd/handler.c`, `vhlctl/vhlctl.c`) · 스펙 · 실타깃 리포트에서 **검증된 실제값만** 사용했다.

**검증 대상** = 10 Request/Query 명령(Req+Ack 쌍) + 7 Indication + `listen` 수신기.
`vhlctl` 서브커맨드 11개: `login logout basic-info device-info set-password set-ip-list change-ip set-radio set-indication reset listen`.

---

## 공통 셋업 (이하 모든 절에서 재사용)

```bash
cd <repo-root>/wlan-package/wlan-opc
make native                       # build/native/{opcd,vhlctl}  (호스트 VHL용)
export VHL="./build/native/vhlctl/vhlctl --host 192.168.0.100 --port 50607 --timeout 2000"
PW=MyPassword                     # opcd 기본 비밀번호(비번 파일 없을 때)
VHLIP=192.168.0.2                 # 이 호스트(VHL)의 IP — indication --to 에 사용
# 무선 간헐 대비 재시도 래퍼
vhl(){ for t in $(seq 1 8); do $VHL "$@" && return 0; echo "  ..retry $t"; done; return 1; }
```

---

## 0. 검증 목표와 전제 (타겟 사전조건 체크리스트)

**목표**: "구현이 스펙 Rev1.00과 와이어 레벨로 일치하고, 거부/세션/안정성 경로가 명세대로 동작함"을
증거기반·재현가능하게 입증한다. **자기검증(vhlctl 디코드)과 독립검증(tcpdump 와이어)을 분리**한다.

사전조건(한 줄씩 실행 → 모두 통과해야 L0 진입):

| # | 점검 | 명령 | 합격 |
|---|---|---|---|
| C1 | 네트워크 도달성 | `ping -c3 192.168.0.100` | 0% loss |
| C2 | ssh 접속(빈 비번) | `sshpass -p '' ssh root@192.168.0.100 'uname -m'` | `aarch64` |
| C3 | 배포본 존재 | `ssh root@192.168.0.100 'ls -l /usr/local/opc/bin/{opcd,vhlctl}'` | 둘 다 존재 |
| C4 | systemd active | `ssh root@192.168.0.100 'systemctl is-active opcd'` | `active` |
| C5 | (운영전제) enabled | `ssh root@192.168.0.100 'systemctl is-enabled opcd'` | `enabled` |
| C6 | 포트 LISTEN | `ssh root@192.168.0.100 'ss -ulnp \| grep 50607'` | `0.0.0.0:50607 …opcd` |
| C7 | 배포본 sha = 호스트 빌드 | `ssh root@192.168.0.100 'sha256sum /usr/local/opc/bin/opcd'` ↔ `sha256sum build/arm64/opcd/opcd` | 일치(코드-실행본 동일성 보증) |
| C8 | 무선 링크 | `ssh root@192.168.0.100 'cat /var/log/cantops/json/mlan0/link.json'` | `link.address`(AP MAC) 존재 |

opcd 런타임 파라미터(검증 환경 통제용): `-p <port>`(기본 50607) · `-i <idle_s>`(기본 300,
`OPC_LOGIN_IDLE_S`) · 서비스 `RestartSec=2`.
**증거 산출물**: `evidence/00_precond.txt` (C1~C8 출력 + 타임스탬프 + 배포 sha).

---

## 1. 검증 계층 모델 (L0 → L6)

각 계층은 하위가 통과해야 의미가 있다. L0/L1 실패는 상위 모든 결과를 무효화한다.

| 계층 | 목적 | 핵심 절차 | 합격 기준 | 증거 |
|---|---|---|---|---|
| **L0** 소켓 도달 | UDP 50607 OTA 왕복 | `$VHL basic-info` + `$VHL --dump basic-info` + tcpdump 병행 | timeout 없이 Vendor/Status 디코드 | `L0.pcap`, `--dump` 콘솔 |
| **L1** 프레임/헤더 | 64B헤더(8+Reserve56) · **Length=프레임−8** · payload@64 · BE · 빈요구=8B만(Len0) | `$VHL --hex basic-info` / `--hex device-info` | 아래 Length 실측표 일치 | `--hex` 필드덤프 |
| **L2** 명령 Req/Ack | 10명령 정상 왕복 + Result/Length 적합 | §2 매트릭스 "정상기대" 순차 | Ack `req_id` 일치, Result=OK(0x0000), Length 명세값 | `L2_<cmd>.txt` |
| **L3** Indication | InitComplete·KeepAlive·이벤트 통지 도달 | §3 `listen` + `set-indication` 2터미널 | period 간격, seq 단조, logout시 teardown | `listen` 로그 + `udp 9999` pcap |
| **L4** 네거티브/견고성/보안 | 거부 매트릭스·세션배타·idle·프레임공격 | §4 | ErrorCause hex 일치 + opcd 미crash(C4 재확인) | `L4.txt` + `journalctl -u opcd` |
| **L5** 소크/안정성 | 장시간/연속 부하 세션·메모리 안정 | §1 L5 절차(임시 opcd `-i`) | idle 초과시 NG 0x0001, KeepAlive seq 무손실, RSS 안정 | `opcd_idle.log` |
| **L6** 와이어 독립 교차검증 | vhlctl 불신, 원시바이트 ↔ 스펙 literal | §5 | 바이트 동치 + 스펙 literal 일치 | `.pcap` + 대조표 |

**L1 Length 실측표** (소스 + 실타깃 리포트 검증):

| Ack | payload | frame | Length 실측 | 공식 total−8 |
|---|---|---|---|---|
| basic-info | 16 | 80 | **72**(0x48) | 80−8 = 72 ✅ |
| device-info | 352 | 416 | **408**(0x198) | 416−8 = 408 ✅ |
| KeepAlive ind | 32 | 96 | **88**(0x58) | 96−8 = 88 ✅ |

`Command Type`(헤더 byte1): Request=0x01 / Ack=0x02 / Indication=0x03. `Protocol Version`=0x01.
body 첫 필드가 정확히 @064. (스펙 §3.2 / `protocol/proto.h`)

### L5 소크 — idle 경계 격리 검증 예

```bash
# 임시 opcd 를 별도 포트/짧은 idle 로 띄워 운영 데몬과 격리
ssh root@192.168.0.100 'setsid /usr/local/opc/bin/opcd -p 55556 -i 3 >/tmp/opcd_idle.log 2>&1 </dev/null &'
./build/native/vhlctl/vhlctl --host 192.168.0.100 --port 55556 login --password $PW
sleep 5
./build/native/vhlctl/vhlctl --host 192.168.0.100 --port 55556 device-info   # → NG 0x0001 (auto-logout)
ssh root@192.168.0.100 'pkill -f "opcd -p 55556"'                            # 정리
```
합격: `NG 0x0001` + opcd 로그 `idle auto-logout (holder=…)`. (KeepAlive 장주기 연속 시 sequence 무손실 확인)

---

## 2. 명령별 검증 매트릭스 (10 Request/Query)

ID/Length 는 `protocol/ids.h`·`protocol/commands.h`·스펙 §3.3 검증값. ErrorCause 는
**명령 컨텍스트로 해석**(같은 와이어값 다의 — ARCH-001).

| 명령 | CmdID | vhlctl | Login | Req Len | Ack Len | 정상기대 | 대표 네거티브 → ErrorCause | 근거 |
|---|---|---|---|---|---|---|---|---|
| Login | `0xF001` | `login --password PW` | — | 184 | 60 | Result=OK, status→0x02 | 빈/오타 비번→**0x0010** ; 2중(타 IP)→**0x0002** ; 기동중→0x0001 | §3.3.1 |
| Logout | `0xF002` | `logout` | — | 0 | 60 | OK, teardown | 미로그인→0x0001 ; 타 IP→0x0002 | §3.3.2 |
| GetBasicInfo | `0x0001` | `basic-info` | 불요 | 0 | 72 | Vendor=0x00902cfb, Station=SINGLE(0x0001) | (발행조건 없음, 항상 응답) | §3.3.3 |
| GetDeviceInfo | `0x0002` | `device-info` | 요 | 0 | 408 | 전 필드 디코드 | 미로그인→**0x0001** ; indication ON 중→**0x0010** | §3.3.4 |
| SetPassword | `0x1001` | `set-password --old --new` | 요 | 312 | 60 | OK + restart 후 잔존 | 구비번 오타→0x0010 ; 신비번 문자/NUL→0x0013/0x0014 ; NVRAM→0x0004 | §3.3.5 |
| SetIpConfigList | `0x1002` | `set-ip-list --slot --flag start\|cont\|end --ip --mask --gw --ntp --essid` | 요 | 56+64·n | 60 | END 수신 시에만 commit | slot→0x0010 ; IP→0x0011 ; netmask→0x0012 ; GW→0x0013 ; NTP→0x0014 ; ESSID 문자/NUL→0x0015/0x0016 ; Len≠56+64n→0x0017 ; seq위반→0x0018 | §3.3.6 |
| ChangeIpAddress | `0x1003` | `change-ip --slot N` | 요 | 60 | 60 | OK(armed)→logout 후 적용 | slot→0x0010 ; 빈 슬롯→**0x0011** ; 리스트변경 중 경합→**0x0012** | §3.3.7 |
| SetRadioConfig | `0x1004` | `set-radio --station --w1-freq --w1-ch --w1-mode --w1-bw [--w2-* --priority]` | 요 | 76 | 60 | OK + reboot 잔존(wpa freq 실수정) | station→0x0010 ; freq(6G)→**0x0011** ; CH→0x0012 ; mode→**0x0013** ; bw→**0x0014** ; 적용실패→0x0050 | §3.3.8 |
| SetIndicationConfig | `0x1005` | `set-indication --bits HEX --period S --to A.B.C.D:PORT` | 요 | 64 | 60 | unicast OK | 미할당 bit→0x0010 ; 비유니캐스트(0.0.0.0/mcast/bcast)→**0x0012** | §3.3.9 |
| Reset | `0x2001` | `reset` | 요 | 0 | **60** ¹ | OK → opcd 재시작 | 미로그인→0x0001 ; 타 IP→0x0002 | §3.3.10 |

¹ **검증 정정**: 스펙 도면은 Reset Ack `Length=0`이나 코드는 **60**을 사용한다
(`protocol/commands.h:432` `OPC_RESET_ACK_LENGTH = 60`, "T11 resolved: spec '0' is a copy-paste
typo from Reset Req; 60 matches every other simple Ack"). 검증 기준 = **60**.
(과거 일부 리포트의 `Length=0`은 stale 코덱 주석 기준)

**공통 ErrorCause**(전 명령, `protocol/ids.h`): `0x0000` 정상 · `0x0001` login-violation ·
`0x0002` login-condition · `0x0003` packet-size · `0x0004` NVRAM.

> **0x0010~0x0014 는 스펙이 고정한 다의 오버로드(ARCH-001)** — 와이어값만으로 판정 금지,
> 반드시 **명령 컨텍스트**로 해석한다. 예: `0x0010` = indication-violation(GetDeviceInfo) /
> pw-mismatch(Login·SetPassword) / slot-range(IpList·ChangeIp) / station-type(SetRadio).
> `0x0012` = ip-change-conflict(ChangeIp) / ind-recipient-ip(SetIndication) / netmask(IpList) /
> radio-CH(SetRadio) / pw-NUL(Login).

---

## 3. Indication 검증

7개 Indication ID 와 검증값(스펙 §3.4 / `protocol/ids.h` · `protocol/indications.h`):

| Indication | ID(=bit) | Len | payload 핵심 | 트리거/발생 | 현재 상태 |
|---|---|---|---|---|---|
| InitComplete | `0x0001` | 60 | Status | enable/login/logout, 부팅 | ✅ 동작 |
| WlanStatusChange | `0x0002` | 60 | WLAN Status(접속0x0001/절단0x0002)+CH | `wpa_cli -i mlan0 disconnect` | ✅ 실타깃 입증(#46) |
| Roaming | `0x0004` | 68 | SNR/RSSI/AP MAC/CH | 로밍 실행체 notify(UDP 127.0.0.1:50608)→opcd 합성(#64/#83) | ✅ 실타깃 입증(roam_notify 수동 + 2AP 라이브 로밍 ch36↔ch40) |
| ApDisconnect | `0x0008` | 68 | Message ID(Disassoc 0x000a/Deauth 0x000c)+ReasonCode+AP MAC | **AP-주도 deauth만** | ⚠ 미입증(#47) |
| FaultDetect | `0x0010` | 60 | Congestion ID(CPU 0x1/Mem 0x2/Disk 0x3/Net 0x4)+Val | 리소스 폭주 | ⚠ 실폭주 미입증 |
| ResetNotice | `0x0020` | 60 | Reset Cause | reset 직전 | ✅ `reset_cause=0x00000001` |
| KeepAlive | `0x0080` | 88 | Timestamp(문자열) | period 마다(>0) | ✅ 동작 |

### 3-1. InitComplete 부팅 시퀀스
Status 전이: `0x01(ready) → 0x02(radio_up) → 0x03(login)` (`OPC_INIT_STATE_*`).
```bash
# 터미널 A — --to 에는 반드시 VHL 호스트 IP(127.0.0.1 아님)
./build/native/vhlctl/vhlctl --hex listen --bind 0.0.0.0:9999
# 터미널 B
$VHL login --password $PW
$VHL set-indication --bits 0x01 --period 0 --to $VHLIP:9999   # InitComplete만
ssh root@192.168.0.100 'systemctl restart opcd'              # 부팅 시퀀스 관측
```
합격: 수신기에 `req_id=0x0001` Status 단조 전이.

### 3-2. KeepAlive 주기 + teardown
```bash
$VHL set-indication --bits 0x80 --period 5 --to $VHLIP:9999   # 5s 주기
# … 수신기에서 sequence n→n+1 약 5s 간격 …
$VHL set-indication --bits 0x80 --period 0 --to $VHLIP:9999   # period=0 → 통지 안 함
$VHL logout                                                  # → KeepAlive 즉시 중단(teardown)
```
합격: period=5 → ~5s 간격 sequence 증가, **period=0 → 수신 0건**, logout → 즉시 중단.

### 3-3. 이벤트성 트리거 (wpa_cli)
```bash
ssh root@192.168.0.100 'wpa_cli -i mlan0 disconnect'   # WlanStatusChange(0x0002) 절단
ssh root@192.168.0.100 'wpa_cli -i mlan0 reconnect'    # WlanStatusChange 접속
# ⚠ iw … disconnect 는 wpa_supplicant 가 SME 소유 → "Operation not permitted"
ssh root@192.168.0.100 'python3 /usr/local/logger/roam_notify.py --iface mlan0'  # Roaming(0x0004) — 링크 무영향
```
ApDisconnect 는 **AP-주도 deauth**(`NL80211_ATTR_DISCONNECTED_BY_AP`)일 때만 발행 —
로컬 disconnect 는 WlanStatusChange 만. 단일-STA 정책(#35): mlan1 이벤트는 drop(interim).

**Roaming(0x0004)**: 이 보드는 host-based 로밍이라 커널이 `CMD_ROAM`을 내지 않음 → 로밍
실행체(wifi_roam.py/passive_roam.py 훅, #83)가 opcd에 로컬 UDP(127.0.0.1:50608, #64) 통지해
발행한다. 능동 검증 = `roam_notify.py` 수동 트리거(위, 링크 무영향, test-all.sh §9).
실로밍 트리거는 `wifi mlan0 roam N`(passive_roam 훅 경유) — mlan0 재결합 유발 ⚠.

---

## 4. 네거티브 / 보안 / 견고성 검증

### 4-1. 거부 매트릭스 (실타깃 PASS 검증값)

| 요청 | 기대 ErrorCause | 비고 |
|---|---|---|
| `login --password ''` / 오타 | NG **0x0010** | 빈/오타 비번 |
| `set-password --new ''` | NG 0x0010 + **파일 sha 불변** | 잠김위험 0 |
| 2nd-host `login` | NG **0x0002** | 세션 배타 제어 |
| indication ON 중 `device-info` | NG **0x0010** | §3.3.4 |
| `set-indication --to 0.0.0.0 / 224.0.0.1 / 255.255.255.255` | NG **0x0012** | **#34 정정**: testing-guide 치트시트는 0x0010 표기 → 현재 코드 0x0012 (`opcd/handler.c` `valid_unicast_ipv4`, `OPC_ERR_IND_RECIPIENT_IP=0x0012`) |
| `set-indication --to <unicast>` | OK | |
| `change-ip --slot 25` (END 전) | NG **0x0012** conflict | |
| netmask `0.255.0.0` (비연속) | NG **0x0012** | #36 |
| `set-radio --w1-mode 99` / `--w1-bw 99` / `--w1-freq 6200`(6G) | NG **0x0013 / 0x0014 / 0x0011** | #36/A21 6G 거부 |
| `-i 3` 후 5s 대기 `device-info` | NG **0x0001** | idle auto-logout |

대표 실행:
```bash
$VHL login --password '' ; $VHL login --password ThisIsWrong          # 0x0010
$VHL login --password $PW
$VHL set-indication --bits 0x80 --period 5 --to 224.0.0.1:9999        # 0x0012
$VHL set-radio --station single --w1-freq 6200 --w1-ch 0x0224 --w1-mode 11 --w1-bw 2   # 0x0011
```
**보안 불변식**: P0 전후 비번 파일 sha256 동일(`sha256sum /usr/local/opc/etc/password*`).

### 4-2. 프레임 공격면 (frame fuzz, L4)
opcd 수신은 관용 길이 모델: 선언 Length 신뢰, runt(<8B)/9..63B/선언 Length 가 데이터그램 초과/
oversize(>1424) → 에러응답(헤더 prefix=req/seq 에코) 또는 drop.
**핵심 합격기준: 어떤 입력에도 opcd 미crash**.
```bash
# 절단/과대/잘못된 버전 프레임 주입(nc/python). 주입 후 C4 재확인
ssh root@192.168.0.100 'systemctl is-active opcd'   # 매 주입 후 active 유지
```
세션 배타 · idle 300s(운영 기본) · 5분 무수신 자동 logout(§3.3.1) 동시 점검.

---

## 5. 와이어레벨 독립 검증 (vhlctl ↔ tcpdump ↔ 스펙 literal)

자기검증(vhlctl 디코드)이 아니라 **원시 바이트**로 삼각검증한다:
```bash
# 1) 호스트(VHL)에서 캡처 시작
sudo tcpdump -i any -n -X 'udp port 50607' -w evidence/L6_reqack.pcap &
# 2) 동시에 vhlctl --dump (소프트웨어가 본 바이트)
$VHL --dump basic-info
$VHL --dump --hex device-info
sudo pkill tcpdump
# 3) pcap 원시 바이트 추출
tcpdump -r evidence/L6_reqack.pcap -n -X | less
```
**대조 기준(스펙 literal 바이트)**:

| 프레임 | literal 바이트(offset 0~) | 의미 |
|---|---|---|
| basic-info Req | `01 01 00 01 00 01 00 00` | ver=1, type=1(Req), id=0001, seq=0001, **Length=0** |
| basic-info Ack | `01 02 00 01 .. .. 00 48` + @64 `00 90 2c fb` | type=2(Ack), **Length=0x48=72**, vendor=0x00902cfb |
| device-info Ack | `… 01 98 …` | **Length=0x0198=408** |
| KeepAlive Ind | `… 03 00 80 … 00 58` + @64 ASCII ts | type=3(Ind), id=0x0080, **Length=0x58=88** |

합격: tcpdump `-X` hex 가 vhlctl `--dump` 와 **바이트 동일**하고, 헤더 8B·Length·offset 64 body 가
스펙 literal 과 일치. Indication 교차검증은 캡처 필터를 `udp port 9999` 로.

---

## 6. 자동화 / 증거수집

**리포 동봉 스위트**: `scripts/test-all.sh` (호스트=VHL에서 실행, `test-env.json` 환경 로드,
etc 스냅샷/복원 트랩·PASS/FAIL 집계·실패 시 비0 종료 내장):
```bash
cp test-env.json.example test-env.json   # 실제 값 기입 (비추적)
bash scripts/test-all.sh                 # 전체 스윕 — exit 0 = 전부 PASS
```
무선 OTA 불안정 대비 — **장치 내부에서 setsid 완주** 후 로그만 회수(testing-guide 권장 패턴,
`<onboard-script>.sh`는 자리표시자 — 검증 항목을 담은 임의 스크립트로 대체, 리포 미포함):
```bash
scp <onboard-script>.sh root@192.168.0.100:/tmp/
ssh root@192.168.0.100 'cd /tmp && setsid bash <onboard-script>.sh > /tmp/verify.log 2>&1 < /dev/null &'
scp root@192.168.0.100:/tmp/verify.log evidence/
scp root@192.168.0.100:/tmp/*.pcap     evidence/
# 장치 내부 vhlctl(무선 무관, 안정):
#   /usr/local/opc/bin/vhlctl --host 127.0.0.1 --port 50607 device-info
```
**PASS/FAIL 집계 골격** (test-all.sh의 `chk` 패턴):
```bash
pass=0; fail=0
chk(){ desc="$1"; exp="$2"; shift 2; out=$("$@" 2>&1)
  if echo "$out" | grep -q "$exp"; then echo "PASS  $desc"; pass=$((pass+1));
  else echo "FAIL  $desc (want $exp): $out"; fail=$((fail+1)); fi; }
chk "L0 basic-info"        "vendor"  $VHL basic-info
chk "L4 empty-pw 0x0010"   "0x0010"  $VHL login --password ''
chk "L2 login OK"          "OK"      $VHL login --password $PW
chk "L4 6G reject 0x0011"  "0x0011"  $VHL set-radio --station single --w1-freq 6200 --w1-ch 0x0224 --w1-mode 11 --w1-bw 2
echo "== RESULT: $pass PASS / $fail FAIL =="
```
**보관 규약**: `evidence/<date>_<target>/` 하위에 `*.log`(콘솔) + `*.pcap`(와이어) +
`precond.txt`(C1~C8, 배포 sha) + `summary.md`(PASS/FAIL 표). 리포트 파일명 컨벤션:
`tmp/device_test_<ip>_<yyyymmdd>_<scope>.md`.

**CI / 회귀 연계**: `make check`(protocol + opcd 단위테스트)를 PR 게이트로 실행. 와이어 포맷 변경
(64B/−8 같은) PR 에서는 실타깃 e2e 필수 — **신 vhlctl ↔ 구 opcd timeout**이 회귀 신호. 배포 전
`build/arm64` sha 를 리포트에 기록해 코드-실행본 동일성을 사후 추적.

---

## 7. 롤백 · 안전 주의 ⚠

상태변경 명령은 **영구/휘발/실링크변경**을 정확히 구분해야 안전하다:

| 명령 | 부작용 | 영속성 | 원복 / 주의 |
|---|---|---|---|
| `set-password` | `/usr/local/opc/etc` 비번 갱신 | 영구(restart 잔존) | **사전 sha 백업** 후 검증, NG 경로 우선. 잠기면 복구 곤란 |
| `set-ip-list` | `iplist.cfg` END 시 commit | 영구(NVRAM) | `cp iplist.cfg{,.bak}` → 검증 → 복원 → sha 대조 |
| `set-radio` | **wpa_supplicant conf의 freq 실제 수정** | 영구(reboot 잔존) | OK 경로는 링크 끊김 가능 → **NG 경로 우선**(apply 전 거부, 링크 무영향) |
| `change-ip` | nxp 백엔드가 **eth0 관리 IP 를 `ip addr` 로 실변경** | 휘발(reboot→`22-eth0.network` 복원; mlan /32 보존) | **OK(armed)는 logout 시 ssh 경로 IP 변경→끊김** → 콘솔 필요. 원격은 **conflict(0x0012) 거부 경로만** |
| `reset` | opcd 재시작(`RestartSec=2`) | — | systemd 자동 복구. 사전 enabled 확인 |

**황금 규칙**
1. 실타깃 배포 전 바이너리 백업(`opcd.bak.<date>`, `vhlctl.bak.<date>`) + sha 검증.
2. config 쓰기 검증은 **NG/conflict 경로 우선**, OK+persist 는 별도 통제 세션(콘솔 가능 시).
3. `change-ip` OK · `set-radio` OK 는 ssh 경로/무선 링크를 끊을 수 있으므로 **시리얼 콘솔 확보 후**에만.
4. idle 테스트는 **임시 opcd**(별도 포트 55556, `-i N`)로 격리 후 `pkill`.
5. 세션 종료 시 반드시 `logout`, 백업/임시파일 정리, 최종 `systemctl is-active opcd` = active.

---

## 8. 현재 커버리지 vs 갭

### 이미 PASS (실타깃 입증 완료)
L0/L1 와이어(64B 헤더 / Length=프레임−8 / offset 64, `--dump`·`--hex` 실측) · basic-info ·
login/logout · device-info(408) · **P0** 빈/오타 비번 0x0010 + 비번파일 sha 불변 ·
**세션 배타** 2nd-host 0x0002 · indication 중 device-info 0x0010 ·
**P1** 비유니캐스트 거부 0x0012(#34 정정) · **idle 자동로그아웃** 0x0001 ·
KeepAlive 주기+teardown · ResetNotice 0x0020 · reset+systemd 재기동 ·
set-ip-list START/END+conflict 0x0012 · netmask·6G freq 거부 · set-radio mode/bw/freq NG ·
WlanStatusChange(#46) · **Roaming(0x0004) notify 경로**(#64/#83 — roam_notify 수동 트리거
wire 실증 + 2AP 라이브 로밍 ch36↔ch40 payload 정확성 실측).

### 남은 갭 (우선순위)

| 우선 | 갭 | 차단 요인 | 검증 방법 |
|---|---|---|---|
| **P1** | `change-ip` 실 IP 적용 + power-cycle 복원 | OK 시 ssh 끊김 | **시리얼 콘솔**에서 logout 후 `ip addr` 확인, 재부팅 후 복원 |
| **P1** | `set-radio` OK+persist & mode/bw/ch **실드라이버 반영** | wpa freq 실수정 | 콘솔에서 OK 후 reboot 잔존 + `iw dev mlan0 link` 대조 |
| **P2** | ApDisconnect MAC(#47) | AP-주도 deauth 환경 필요 | AP 에서 강제 deauth 유발(테스트 AP) |
| **P2** | NVRAM 2분 응답 예산 | 장시간 | set-password/set-ip-list/set-radio 응답시간 120s 이내 계측 |
| **P3** | Roaming 자연발화 — 3훅 외 로밍 경로(bgscan 등) 미커버 가능성 | notify 는 wifi_roam·passive_roam 훅에서만 발행(#83) | 임계하회 자율 로밍 유발 후 0x0004 수신 확인(차기) |
| **P3** | FaultDetect 실폭주 | CPU/Mem/Net 폭주 유발 | stress-ng 등으로 Congestion 유발 |
| **P3** | 스텔스 ESSID NULL / Dual WLAN#2 | mlan1 DOWN, 단일-STA 정책 | DFK 확정상 **단일 스테이션만 유효** — Dual 은 스펙 형식만 검증 |
| **P3** | Protocol Version 협상 · 0x0050 radio-apply 실펌웨어 | 미확정 와이어값 | 발주처 확정 후 |
| **P3** | .deb 재빌드 + dpkg lifecycle(manual-runthrough §6) · §7 sign-off | 패키지 단계 | dpkg -i/--purge + 경로 잔존 0 확인 |

### 문서 드리프트 / 스펙 미결 (검증 중 유의)
- **`testing-guide.md` 치트시트 drift**: `set-indication` 비유니캐스트를 `NG 0x0010` 으로 표기 →
  **현재 펌웨어는 0x0012**(#34). 검증 시 0x0012 기준, 문서 갱신 권장.
- **ErrorCause 0x0010~0x0014 다의 오버로드**(ARCH-001): 와이어값만으로 판정 금지 — 명령 컨텍스트 필수.
- **DFK 미확정**(`docs/spec-inquiry/spec-QA.md` · `docs/dfk-meeting/meeting-2026-06-26-dfk-opcd-qa.md`):
  Dual-Station 비동작(Single 만 유효) · GW/NTP 미지정 허용 검토 · `set-radio` Result=OK 의미
  (기록 완료 vs AP 재접속 완료) · List Boundary Flag 스펙 상충 · Reset Ack Length(0 vs 60, **60 채택**) ·
  0x0013 indication 조건위반 **삭제 확정**(DFK 답변).

---

## 참조

- 스펙: [`../spec/opc_vhl_protocol_Rev1.00_KO.md`](../spec/opc_vhl_protocol_Rev1.00_KO.md)
- 테스트 방법: [`testing-guide.md`](testing-guide.md)
- 인수 체크리스트: [`manual-runthrough.md`](manual-runthrough.md)
- 코드 리뷰: [`review-report.md`](review-report.md)
- ErrorCause/ID 정의: `../../protocol/ids.h` · `../../protocol/commands.h`
- vhlctl CLI: `../../vhlctl/vhlctl.c` · opcd: `../../opcd/opcd.c` · `../../opcd/handler.c`
- 스펙 미결 추적: [`../spec-inquiry/proto-todo.md`](../spec-inquiry/proto-todo.md) · [`../spec-inquiry/spec-QA.md`](../spec-inquiry/spec-QA.md)
