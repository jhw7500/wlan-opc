# wlan-opc 테스트 가이드

opcd / vhlctl / protocol을 직접 테스트하는 방법. 3가지 레벨(단위테스트 → 로컬 e2e → 실장치 e2e)과
device-info 데이터 소스 확인법을 다룬다.

> 시점별 검증 결과(실측값)는 `tmp/device_test_*.md` 리포트를, 1차 인수 체크리스트는
> `docs/testing/manual-runthrough.md`를 참조. 이 문서는 **방법**만 담는다.

작업 디렉토리: `<repo-root>/wlan-package/wlan-opc`

---

## 0. 빌드

산출물은 arch별로 `build/native/`·`build/arm64/`에 **분리 보관**된다 (서로 덮어쓰지 않음 — 전환 시 clean 불필요).

| 목적 | 명령 | 산출물 |
|---|---|---|
| ARM64 크로스(실장치 배포용) | `make` 또는 `make arm64 PLATFORM=nxp` | `build/arm64/**` (aarch64 ELF) |
| native(로컬 테스트용, stub) | `make native` (= `make native PLATFORM=stub`) | `build/native/**` (x86-64 ELF) |
| native(로컬에서 nxp 백엔드) | `make native PLATFORM=nxp` | `build/native/**` (x86-64 ELF) |
| 양쪽 동시 | `make both` | `build/native` + `build/arm64` |

> arch는 **타겟 별칭**으로 고른다: `make`/`make arm64` = 크로스(aarch64), `make native` = 호스트(cc).
> 둘이 `build/<arch>/`로 공존하므로 더 이상 전환 시 `make clean`이 필요 없다 (예전의 "환경변수 vs 인자"
> 빌드 함정도 사라짐). `file build/native/opcd/opcd`로 `x86-64`, `file build/arm64/opcd/opcd`로 `aarch64` 확인.
> `vhlctl`은 클라이언트라 PLATFORM과 무관(stub/nxp 동일 바이너리).

`PLATFORM` 차이(arch와 직교, opcd에만 영향):
- `stub` — 플랫폼 호출이 canned(0/빈값). 프로토콜·세션·보안 로직 테스트용.
- `nxp` — `/var/log/cantops/json/**`, `/etc/systemd/timesyncd.conf`, `dpkg-query`에서 실제 데이터를 읽음(§4 참조).

---

## A. Host 단위테스트 (가장 간단)

```bash
make check        # native(cc)로 codec round-trip + opcd 단위테스트 (build/native/ 에 빌드)
```
→ `all tests passed` (protocol 19 + opcd 41 = 60개). 중간의 `cannot read .../No such file`류 stderr는
음성 경로를 일부러 트리거하는 테스트의 의도된 출력.

---

## B. 로컬 e2e (하드웨어 없이, stub)

opcd를 native stub으로 띄우고 vhlctl로 UDP 프로토콜을 왕복시킨다.

```bash
make native        # = make native PLATFORM=stub → build/native/

# 터미널 1 — opcd
./build/native/opcd/opcd -p 55555 -i 120
#   -p 포트(기본 50607)  -i idle 자동로그아웃 초(기본 300)
#   "mkdir /usr/local/opc ... Permission denied" 경고는 무시 가능(read-only 명령엔 영향 없음)
#   "starting on UDP :55555" 나오면 정상. 비밀번호 파일이 없으면 기본 비번 MyPassword.

# 터미널 2 — vhlctl
V="./build/native/vhlctl/vhlctl --host 127.0.0.1 --port 55555"
$V basic-info
$V login --password ''            # P0: NG 0x0010 (빈 비번 거부)
$V login --password MyPassword    # OK
$V device-info                    # stub 더미 데이터
$V set-indication --bits 0x80 --period 5 --to 0.0.0.0:9999    # P1: NG (비유니캐스트 거부)
$V set-indication --bits 0x80 --period 5 --to 127.0.0.1:9999  # OK
$V device-info                    # indication ON 중: NG 0x0010
$V logout
$V --dump basic-info             # raw TX/RX 프레임 hexdump
```
종료: 터미널 1에서 `Ctrl+C`.

> `set-password`/`set-ip-list`/`set-radio`는 `/usr/local/opc/etc`에 저장하므로 권한이 없으면
> `NG(NVRAM)`가 난다. 끝까지 보려면 `sudo mkdir -p /usr/local/opc/etc/temp` 후 `sudo ./build/native/opcd/opcd -p 55555`.

---

## C. VHL ↔ OPC 실장치 e2e

이 호스트를 **VHL**, 실장치(`192.168.0.100`)의 opcd를 **OPC**로 두고 네트워크 너머로 제어한다.
실장치 opcd는 systemd로 이미 active(UDP 50607). vhlctl은 클라이언트이므로 **호스트에서 native로 빌드**해
쓴다(`make native`).

```bash
cd <repo-root>/wlan-package/wlan-opc
export VHL="./build/native/vhlctl/vhlctl --host 192.168.0.100 --port 50607 --timeout 2000"

# 요청/응답
$VHL basic-info
$VHL login --password MyPassword
$VHL device-info                 # 실장치 live 데이터
$VHL logout
```

### indication end-to-end (2개 터미널)
```bash
# 터미널 A — VHL 수신기 (★ --to 에는 이 호스트 IP를 넣는다. 127.0.0.1 아님)
./build/native/vhlctl/vhlctl listen --bind 0.0.0.0:9999

# 터미널 B
$VHL login --password MyPassword
$VHL set-indication --bits 0x81 --period 5 --to 192.168.0.2:9999   # 0x81 = InitComplete+KeepAlive
$VHL logout                                                        # → KeepAlive 계속 수신(#160: 통지 설정은 세션과 무관, 리셋·IP 변경·Info=0으로만 해제)

# #105 coalesce: 상태변화(WlanStatus 0x02 / Roaming 0x04 / ApDisconnect 0x08)는
#   period>0이면 즉시가 아니라 주기 경계에서 마지막 1건만 통지. 예) period 5로
#   set-indication(--bits 0x02) 후 5초 안에 wpa_cli disconnect;reconnect 연타 →
#   WlanStatusChange는 경계에서 1건(마지막=CONNECTED)만 도착. period=0이면 즉시 송신.
#   경계 catch-up: 데몬이 수 초 스톨해도 timerfd 만료 합산으로 주기가 밀리지 않고
#   한 번에 경계를 통과(스톨 후 flush는 1회). 송신 실패 시 그 주기 마지막 상태는
#   pending 유지 → 다음 경계에서 재시도(유실 방지).
```

### 무선 간헐 대비 재시도 래퍼
```bash
vhl(){ for t in $(seq 1 8); do $VHL "$@" && return 0; echo "  ..retry $t"; done; }
vhl device-info
```

### ssh 접속 / 장치 내부 실행
```bash
sshpass -p '' ssh root@192.168.0.100        # 빈 비번
# 장치 내부에서 (무선 끊겨도 안정):
/usr/local/opc/bin/vhlctl --host 127.0.0.1 --port 50607 device-info
```
> 무선이 불안정할 때 긴 검증은 스크립트를 장치에 올려 `setsid bash script > log 2>&1 < /dev/null &`로
> 내부 백그라운드 완주시키고 결과 로그만 회수하는 방식이 안정적.

---

## D. device-info 데이터 소스 확인

`platform_nxp.c`는 ioctl/netlink를 안 쓰고 **cantops logger가 만든 JSON 파일을 매 요청마다 읽는다.**
필드별 소스:

| Ack 필드 | 소스 (파일/명령) | 계층 | 갱신 |
|---|---|---|---|
| vendor/product/subcode, hardware, serial, 802.11, 날짜 | `/usr/local/opc/etc/device_info.json` | static | **부팅 시 1회** |
| firmware | `dpkg-query -W -f='${Version}' wlan-proc` | dynamic | 첫 요청 1회(캐시) |
| ntp_server | `/run/systemd/timesyncd.conf.d/opc-ntp.conf`(ChangeIp가 쓴 휘발 drop-in)가 있으면 그 `NTP=`, 없으면 `/etc/systemd/timesyncd.conf` 의 `NTP=` | dynamic | 매 요청 |
| ethernet_mac | `/var/log/cantops/json/eth0/link.json` (항상 eth0 — mlan0 MAC은 클론이라 오보고) | live | 매 요청 |
| ip/netmask/gateway | `opc.conf::device_ip_iface` 토글에 따라 `eth0/link.json`(기본) 또는 `mlan0/link.json` — 읽기와 change-ip 적용 대상이 **같은 선택자**를 따름 | live | 매 요청 |
| essid, WLAN mac, AP mac, RSSI, SNR, mode, bw | `/var/log/cantops/json/mlan0/link.json` (mlan1=WLAN#2) | live | 매 요청 |
| WLAN 개수(1/2) | `/var/log/cantops/json/mlan1/link.json` 존재 여부 | live | — |
| freq/channel, station_type, priority_ch | `/usr/local/opc/etc/radio.conf` (set-radio 캐시: SCAN band+채널 목록). freq/channel은 `opc.conf::device_info_freq_source` 토글에 따라 `live`(기본, Rev1.01)=접속 시 live·미접속 0xFFFF / `auto`=접속 시 live·미접속 시 설정 파생값 / `config`=설정 파생값(Rev1.00 해석). priority_ch는 Single이면 0xFFFF | live(기본)/config | set-radio·매 요청 |
| device_status | opcd 런타임 세션 | runtime | 실시간 |

mlan0/link.json 키 매핑: `info.ssid`→essid, `info.address`→WLAN mac, `link.address`→connect_ap_mac(=associated),
`link.signal_avg`→RSSI, `channel_info.<freq>.noise`→SNR(`rssi−noise`), `link.tx_bitrate`(HE-/VHT-/MCS)→mode,
`info.width`→bw. freq/channel은 **`device_info_freq_source`(opc.conf) 토글**: `live`(기본, Rev1.01 §4.3.4)=접속 시 live값(미접속·주파수 없는 링크→0xFFFF/0xFFFF) / `auto`=접속 시 live값, 미접속 시 설정 파생값(없으면 0xFFFF) / `config`=설정 파생값(Rev1.00 해석). mode/bw는 항상 live가 있으면 우선, 설정한 적 없으면 0xFF. 미접속 시 SNR/RSSI −128·AP MAC 0·status 0, Single이면 WLAN#2 블록 전체 무효값(0xFF/0xFFFF/−128) — Rev1.01 §4.2.2. (실타겟 검증: `docs/implementation/verify-device-info-freq-source.md`)

### live 필드는 재시작 없이 즉시 반영 — 파일을 바꿔 입증
```bash
# 실장치에서 essid가 link.json에서 온다는 것 입증
cp /var/log/cantops/json/mlan0/link.json /tmp/bak.json
sed -i 's/"jhw_wlan"/"TEST_SSID"/' /var/log/cantops/json/mlan0/link.json
$VHL login --password MyPassword
$VHL device-info | grep essid          # → essid='TEST_SSID'
cp /tmp/bak.json /var/log/cantops/json/mlan0/link.json    # 원복
```
> logger가 link.json을 주기적으로 덮어쓴다(파일 내 `"date"`가 갱신 시각). 편집 직후 빠르게 요청하거나
> logger를 잠시 멈춘다. **static 필드(device_info.json)는 부팅 시 1회 로드**라 수정 후 `systemctl restart opcd` 필요.

### 로컬에서 파일→Ack 매핑 재현 (nxp 백엔드)
```bash
make native PLATFORM=nxp     # 경로는 코드에 #define으로 하드코딩됨 (arch/PLATFORM 전환은 clean 불필요)
sudo mkdir -p /var/log/cantops/json/eth0 /var/log/cantops/json/mlan0
# eth0/link.json, mlan0/link.json 을 실장치 샘플 형식으로 작성
sudo ./build/native/opcd/opcd -p 55555 -i 120
./build/native/vhlctl/vhlctl --host 127.0.0.1 --port 55555 login --password MyPassword
./build/native/vhlctl/vhlctl --host 127.0.0.1 --port 55555 device-info   # 내가 만든 파일 값이 그대로 나옴
```

---

## vhlctl 명령 / 기대 결과 치트시트

```
vhlctl [--host HOST] [--port PORT] [--timeout MS] [--dump] [--hex] SUBCOMMAND
  login [--password PW] | logout | basic-info | device-info
  set-password --old PW --new PW
  set-ip-list  --slot N --flag start|cont|end|start_end --ip .. --mask .. --gw .. --ntp .. --essid NAME
  change-ip    --slot N
  set-radio    --station single|dual --w1-freq F --w1-ch CH --w1-mode N --w1-bw N [--w2-.. --priority HEX]
  set-indication --bits HEX --period S --to A.B.C.D:PORT
  reset | listen --bind HOST:PORT
```

| 명령 | 기대 |
|---|---|
| 미로그인 `device-info` | `NG login-violation(0x0001)` |
| `login --password ''` / 틀린 비번 | `NG 0x0010` (P0) |
| `login --password MyPassword` | `OK` |
| `set-indication --to 0.0.0.0 / 224.x / 255.255.255.255` | `NG 0x0010` (P1) |
| `set-indication --to <장치가 게이트웨이 없이 닿는 유니캐스트>` (연결 서브넷·host route·loopback) | `OK` |
| `set-indication --to <다른 세그먼트 / 서브넷 브로드캐스트 / 경로 없음>` | `NG 0x0012` (#159) |
| indication ON 중 `device-info` | `NG 0x0010` |
| `logout` 후 재로그인 → `device-info` | `NG 0x0010` (#160: 통지 설정은 세션 종료 뒤에도 유지) |
| 같은 세션(idle 만료 전)에서 `set-indication --bits 0x00 --period 0 --to A.B.C.D:PORT` 후 `device-info` | `OK` (통지 해제 후 조회) |

### SetRadioConfig의 NXP mode/BW 적용 범위

NXP backend는 주파수 목록과 mode/BW를 실제 driver helper에 함께 전달한다. mode는
기존 Wi-Fi CLI와 같은 최대 규격 상한, BW는 20/40/80MHz 상한이다. 따라서 낮은 규격의
AP에 연결되면 `device-info`가 반환하는 실제 협상 규격/폭은 요청보다 낮을 수 있다.
mlan1의 ax와 b/g의 5GHz 전용 조합은 변경 전에 0x0013, 현재 적용 helper가 구현하지
않는 160/80+80/320MHz는 0x0014를 반환한다. 일반 enum을 수용하는 host stub 결과를
NXP 하드웨어 지원 증거로 사용하지 않는다. Dual `priority_ch`는 실적용 정책 미결이다.

AP 재접속 완료는 설정 ACK 조건이 아니다. supplicant conf와 mode/BW의 부팅 JSON을
저장한 뒤 연결 결과를 기존 indication으로 관찰한다. 로컬 회귀 테스트는 실제 helper의
명령/파일/복구 경로 및 NXP의 argv·선검증·process-group timeout을 실행하지만, 실제
타겟의 성공 시간, 협상 결과, 재부팅 복원을 증명하지 않는다. helper는 Single과 Dual
모두 인터페이스별 900ms로 제한한다. SetRadioConfig는 저장 완료 후 응답하므로
Rev1.01 §4.1.3.1의 저장 명령 타이머(120초)를 적용한다. 저장 없는 조회/설정의
1초 타이머와 구분한다. Dual 두 helper를 450ms씩 제한하던 경로는 실제 첫 호출이
시간 초과되어 수정했으며, 로컬 회귀에서 각 500ms helper 두 번의 성공을 확인한다.
타겟 검증 때는 적용 전 설정/드라이버 상태를 보존하고 성공·NG·재부팅을 각각 확인한다.
일반 실패와 TERM에서 snapshot 복구를 시도하고, hard KILL 이후에는 완전 복구를
보장하지 않는다. handler의 사후 last-good 재적용도 best-effort다.

2026-10-13에 기존 검증 환경을 정정했다. 임시 복구 타이머에 `OnBootSec=25min`과
`OnActiveSec=25min`이 함께 들어 있었고, 부팅 후 이미 25분 이상 지난 타겟에서
타이머를 활성화하자 복구 service가 즉시 실행됐다. 최초 mlan0 `DISCONNECT`는
그 복구 실행과 겹쳤으며, 제품의 mlan1 기동 문제로 분류했던 해석을 철회한다.
[systemd 공식 매뉴얼](https://github.com/systemd/systemd/blob/db11bab38ccf1ed257f310d29070843d4c58ea01/man/systemd.timer.xml#L167)은
이미 지난 OnBootSec 만료 시각의 즉시 발화를 설명한다. 이전 예비 시험 기록은 보존하되
동시 복구가 개입하지 않은 통제 시험으로 사용하지 않는다.

재검증은 `OnActiveSec=25min` 단독과 `AccuracySec=1s`를 사용했다. setup 전후,
매 관측과 원복 직전에 `LastTriggerUSecMonotonic=0`을 확인하고, 시험 후 전용 복구
service의 journal 시작 기록이 0건인지 대조했다. 타이머 활성화 기준 시각은 enabled
timer가 다음 부팅에서 다시 활성화될 때도 새로 계산된다. 복구 service 발화가 복구
성공 자체를 보장하는 것은 아니므로 파일/드라이버/서비스/네트워크 복구도 별도로 확인했다.

같은 날 cts-wlan에서 mlan1은 비활성 상태로 두고 mlan0를 `jhw_wlan`/5200MHz
(채널 40)에 연결했다. 독립 UDP 클라이언트로 다음 9조합의 ACK를 측정하고 실제
`bandcfg`/`htcapinfo`/`vhtcfg`, JSON 저장값 및 `radio.conf` 존재를 대조했다.
각 조합에서 `ping -I mlan0 -c 2 -W 1 192.168.0.1` 2/2 성공과 `mlanutl getdatarate`의
TX 규격/폭을 확인했으며, `GetDeviceInfo`의 mode/BW/freq/status가 일치했다.

| Request | Actual TX | OPC mode/BW | ACK (ms) | Ping |
|---|---|---|---:|---|
| n / 20 | HT20 | n / 20 | 538.679 | 2/2 |
| n / 40 | HT20 | n / 20 | 474.368 | 2/2 |
| n / 80 | HT20 | n / 20 | 480.598 | 2/2 |
| ac / 20 | VHT20 | ac / 20 | 481.373 | 2/2 |
| ac / 40 | VHT20 | ac / 20 | 497.184 | 2/2 |
| ac / 80 | VHT20 | ac / 20 | 473.244 | 2/2 |
| ax / 20 | VHT20 | ac / 20 | 496.591 | 2/2 |
| ax / 40 | VHT20 | ac / 20 | 462.666 | 2/2 |
| ax / 80 | VHT20 | ac / 20 | 478.715 | 2/2 |

이 AP는 VHT IE를 광고하지만 현재 HT operation은 20MHz이고 VHT operation의
width는 0이며, HE IE는 관측되지 않았다. 따라서 n/ac20 실제 링크와 상한 이하의
하향 협상 증거이며 실제 ax·40/80MHz 링크 협상을 검증한 것은 아니다.

160/80+80/320MHz 3건은 0x0014, b/g+5GHz 2건과 Dual mlan1 ax 1건은 0x0013으로
사전 거절됐다. 총 6건에서 mlan0 드라이버 상한과 JSON/WPA/radio.conf 해시가 유지됐다.
로그인/로그아웃 ACK도 OK였다. 앞선 AC/40 부팅·Dual 세 조합·mlan1 서비스 중단
복구 자료는 보존하지만 이번 타이머 미발화 재검증을 대체하지 않는다. 이번에는
재부팅 복원이나 hard KILL·저장장치 장애 복구를 새로 시험하지 않았다.

수정한 타이머 환경에서 mlan1의 SSID/freq만 `jhw_wlan`/5200MHz로 임시 변경하고
mlan0 `jhw_wlan_`/5180MHz와의 동시 연결을 다시 시도했다. 기존 WPA 인증 필드는
유지했다. n/20 + n/20의 SetRadioConfig ACK는 OK(1057.843ms)였고 양 인터페이스
드라이버 상한 및 JSON 저장값이 n/20이었다. 그러나 45초 관측에서 mlan0만 연결되고
mlan1은 SCANNING에 머물렀다. 후속 Dual mode 조합과 60초 동시 유지 시험은 실행하지
않았다. 이전 시험에서 mlan0가 먼저 끊겼던 현상을 이 재검증 결과와 혼동하지 않는다.

타겟은 동일 SDIO 카드의 `02DF:914D`/`02DF:914E` 함수이며,
[NXP 소스의 장치 ID 매핑](https://github.com/nxp-imx/mwifiex/blob/5ad19e194f49ed9447bee7864eb562618ccaf9b1/mlinux/moal_sdio_mmc.c#L99)에
따르면 88W9098 Function 1/2다. 실제 드라이버/펌웨어 문자열은
`SD9098----17.92.1.p149.115--MM6X17543.p18-GPL-(FP92)`였다.
[NXP RN00104 Rev.20.0 §3.1.4](https://www.nxp.com/docs/en/release-note/RN00104.pdf#page=21)는
CDW STA+STA 조합을 5GHz+2.4GHz로 설명하며,
[NXP 기술지원의 2026-02-23 답변](https://community.nxp.com/t5/Other-NXP-Products/88W9098-Dual-STA-Concurrency-Issue-2-4GHz-5GHz-Works-5GHz-5GHz/m-p/2321440)은
PCIe의 5GHz+5GHz 실패를 이 제약으로 설명한다. 현재 SDIO 타겟에서 발생한 모든
끊김의 원인을 확정한 것은 아니지만, 공식 조합의 성공 시험에는 2.4GHz AP가 필요하다.
DRCS/DMCS 모듈 설정 변경이나 드라이버 재적재는 이 시험에서 수행하지 않았다.

타이머 수정 후 원래 `cantops` 프로필을 유지한 mlan1으로 허용된 2.4GHz 채널
1~13의 passive scan-only 요청을 실행했다. 스캔 완료 이벤트와 AP 36개를 관측했으나
`cantops`, `jhw_wlan`, `jhw_wlan_`은 없었다. mlan1 기동 및 스캔 중 mlan0의 기존
`jhw_wlan_`/5180MHz 연결은 유지됐다. Dual 성공 시험을 이어가려면 사용할 2.4GHz
AP의 SSID·채널과 기존 인증 정보 재사용 여부가 필요하다. 주변 다른 AP에 접속하지 않았다.

Dual 두 AP 동시 연결·`priority_ch` 데이터 경로, 실제 ax·40/80MHz 링크, 재부팅 복원과
장시간/다른 보드 운용은 이번 재검증의 잔여 범위다. 초기 단일 링크 프로그램의
`Type:`/`BW:` 파싱 오류와 첫 스캔 프로그램의 `phy1` 이름 가정도 예비 기록에 남겼다.
현재 스캔은 sysfs에서 실제 이름 `mwiphy1`을 얻어 사용한다. 검증 프로그램의 오류를
제품 실패로 집계하지 않는다. 모든 시도 후 파일 SHA-256·소유권·모드와 드라이버·서비스·
MAC·IP·IPv4/IPv6 경로를 원복했고, 원래 mlan0 AP에 재연결됐다. mlan1에 생긴
IPv6 multicast 경로는 기존 상태에 없었음을 대조한 뒤 정확히 그 경로만 정리했다.
이번 여섯 전용 디렉터리의 자격증명 백업·원시 trace·임시 파일과 복구 타이머를 제거했고
primary supplicant에 남은 tracer가 없음을 확인했다. 외부 Release는 보류 상태다.

2026-10-13 후속 시험은 사용자 결정에 따라 Single에 집중했다. 원래 AP
`jhw_wlan_`/5180MHz에서 ac/40 저장 후 재부팅 1회로 boot ID 변경과 JSON/radio.conf
해시 유지, OPCd를 시작하기 전 드라이버 상한 및 무선 ping을 확인했다. 부팅 시험의
후보는 신호 수정 전 `66fcf9b…`이며 기존 `wifi_init.sh` 부팅 경로는 변경하지 않았다.

중단 시험에서 OPCd의 `open_signalfd()`가 차단한 SIGINT/SIGTERM 마스크를 helper가
상속하여 TERM cleanup을 실행하지 못하는 결함을 확인했다. `run_argv_bounded()`의
자식 exec 전에 마스크를 초기화했다. 회귀 테스트는 실제 OPCd와 같은 부모 마스크에서
수정 전 실패, 수정 후 TERM 정리·자식 회수·부모 마스크 보존 및 TERM 무시 시 KILL을
검증했다. 전체 `make -C wlan-opc check`와 ARM64 NXP 빌드가 통과했다.

수정 후보 `0ce37bc8…`의 기준은 Single ac/40·채널 36/40이었다. n/20·채널 40만
요청하여 드라이버와 WPA 주파수 목록을 함께 변경하고 reconfigure에서 one-shot 오류를
주입했다. 각 사례의 JSON/WPA/radio.conf 해시와 드라이버 값은 기준과 일치했고 원래
AP 연결 및 `ping -I mlan0` 2/2 성공을 확인했다.

| Injection | ACK | ACK (ms) | TERM | KILL | State/data recovery |
|---|---|---:|---|---|---|
| FAIL once | NG / 0x0050 | 761.192 | yes | no | PASS |
| Delay, default TERM | NG / 0x0050 | 882.045 | yes | no | PASS |
| Delay, ignore TERM | NG / 0x0050 | 902.307 | yes | yes | PASS |

helper의 `/proc/self/status` SIGTERM 차단 비트는 0, OPCd 부모의 차단 비트는 유지됐다.
TERM 지연 시험은 helper trap 정리가 관측됐고, TERM 무시 시험은 수신 기록과 900ms
KILL을 함께 확인했다. FAIL 사례도 TERM 시각에 도달했으므로 순수 즉시 오류 경로나
모든 helper trap 실행을 입증한 것으로 취급하지 않는다. KILL 이후 남은 transaction
backup은 일반 제품 동작이 자동 삭제한 결과가 아니다. 원복 후 검증 도구가 기록된
inode/해시와 활성 프로세스 부재를 확인해 새 파일만 제거했다.

예비 시험의 잘못된 wpa_cli 경로, Python 중계 비용, 연속 기준 요청 및 내부 로그 표식
판정 오류와 신호 기록 경합은 원본 기록에 남겼다. 신호 마스크 결함의 수정 전/후 결과를
구분한다. 모든 시험에서 복구 타이머 미발화를 확인했고, 최종 점검으로 원본 파일 권한·
소유권·SHA-256, 드라이버·MAC·IP·IPv4/IPv6 경로 및 서비스 활성/enable 상태를 대조했다.
전용 원격 백업 7개와 타이머·override를 제거했다. 증거는 부모 저장소의
`.omx/plans/opcd-single-recovery-20261013/qa-summary.json`에 있다.

Dual/priority_ch는 이번 0.7.0의 후속 범위다. 최종 설치 패키지의 설치·업그레이드·재부팅,
실제 ax·40/80MHz 링크, 저장장치 장애와 다른 보드/AP·장시간 운용은 이 시험에서
검증하지 않았다. 강제 KILL 후 완전 복구는 보장하지 않으며 외부 Release는 보류한다.

### Single 고장 부위별 통지·복구 추가 검증 (2026-10-13)

사용자 방침은 자원 사용률 폭주에서는 FaultDetect만 송신하고, 실제 운용 불가 장애에서
ResetNotice 송신을 시도한 뒤 기존 정책으로 리셋하는 것이다. 아래 표는 **검증 계획**이다.
R1 및 통제된 R2의 실행 결과는 뒤의 실기 결과에 별도로 기록하며, 표의 모든 고장 유형이
통과했다는 뜻이 아니다. 무선 장애만으로 유선 통지도 불가능하다고 판정하지 않는다.

현재 제어 UDP 소켓은 wildcard bind(`opcd.c::open_udp_socket`)이고 ResetNotice는
일반 `sendto`(`indication.c::send_indication_frame`)로 기존 수신 IP/port에 보낸다.
따라서 송신 경로는 커널 라우팅에 달려 있다. 유선으로 직접 접근 가능한 수신기를 두고
`ip route get <recipient-ip>`의 dev/src 및 정책 라우팅을 확인한 뒤, 고장 전/후의
실제 수신 패킷과 타겟 송신 기록을 대조한다. 수신기가 무선망 뒤에만 있거나 route/src가
실패한 무선을 계속 가리키면, eth0 링크가 살아 있어도 대체 경로가 마련된 것은 아니다.
현재 코드에 별도의 인터페이스 자동 전환이나 보조 수신기 송신 기능은 없다.

| Case | Fault | Expected notification | Recovery/check |
|---|---|---|---|
| R1 | AP disconnect only | WlanStatusChange when enabled | Reconnect; no new immediate board-reset rule |
| R2 | WLAN firmware/driver unusable | ResetNotice via reachable eth0 when OPCd/kernel work | Existing recovery policy; reset only on escalation |
| R3 | Supplicant or monitoring process exits | Verify enabled event delivery; no assumed reset notice | Verify existing service restart and link restoration |
| R4 | OPCd exits | No pre-reset notice from the dead process | Verify service restart and receiver reconfiguration |
| R5 | OPCd stays alive but hangs | No notice guarantee | Main-loop heartbeat; 30 s watchdog; restart OPCd only |
| R6 | CPU/Disk/Network load congestion | FaultDetect entry when enabled | No automatic board reset from congestion alone |
| R7 | Reset cause/log storage write fails | Verify cause handling and bounded notification attempt | Recovery must not depend on successful log/cause writes |
| R8 | Kernel fully hangs or all receiver paths fail | No pre-reset notice guarantee | Verify watchdog configuration/recovery separately |

`opcd.service`는 초기화 완료를 `READY=1`로 알린 뒤 메인 루프의 1초 timerfd에서
`WATCHDOG=1`을 보낸다. `WatchdogSec=30s`가 만료되면 SIGKILL로 OPCd를 종료하고
`Restart=always`/`RestartSec=2`로 서비스만 다시 시작한다. 별도 저장 worker는
heartbeat를 보내지 않으며 보드 리셋 조건을 연결하지 않았다. 시작/종료 제한은 각각
30초/10초다. R4/R5에서는 재기동 뒤 메모리 내 로그인/Indication 설정이 유지된다고
가정하지 않고 수신기 재설정 절차를 확인한다. R7은 일반 사용률 폭주와 별개인 저장 실패 경로다.

우선 검증은 R1/R2의 유선 수신 경로와 정상 종료 중 ResetNotice 송신 순서다. 이어
checker/fw_watch 서비스 종료에 reboot policy 자식이 함께 종료되어 강제 리셋 fallback이
사라지는지, OPCd보다 supplicant/네트워크가 먼저 종료되는지 확인한다. 서비스 종료 순서와
강제 리셋 fallback의 생존성은 기존 원인 파일·통지 판정 단위 테스트만으로 입증하지 않는다.
후속 시험에서 확인한 서비스 종료 중 생존성과 실제 강제 리셋은 아래에서 구분한다.

#### R1/R2 유선 통지 실기 결과 (2026-10-13)

cts-wlan의 분리 관리 IP 구성(`eth0_ip`)에서 시험 후보 `09bbc0ec…`를 일시 기동했다.
타겟 수신기 경로는 `eth0`/`192.168.1.1`, 유선 수신기는 `192.168.1.2:50791`이었다.
OPC 로그인·Indication 설정은 자격증명 반출 없이 타겟 loopback에서 수행하고,
ResetNotice의 실제 UDP 수신은 외부 유선 호스트에서 관측했다. 타겟의 저장 비밀번호
파일은 없었으며 공개 제품 기본값을 타겟 내부에서만 사용했다. Login 및 SetIndication은
OK였고, 통지 활성 중 GetDeviceInfo의 NG/0x0010은 현재 로그인·통지 규약의 정상 응답이다.

| Case | Controlled condition | Observed result |
|---|---|---|
| R1 | AP disconnected; mlan0 still administratively UP | Wired WlanStatusChange + KeepAlive; unchanged boot ID; original AP reconnected |
| R2 | mlan0 administratively DOWN; supplicant INTERFACE_DISABLED | Wired KeepAlive; one 68-byte ResetNotice with cause 0x12 before graceful reboot |
| Restore | Reboot completed | Original files/driver/MAC/IP/routes/services restored; radio ping 2/2 |

R2에서는 실제 펌웨어 크래시나 드라이버 wedge를 유발하지 않았다. 무선 인터페이스를
내린 상태에서 기존 `wlan_reboot_policy.sh`를 호출하고, `wlan_fw_watch`라는 정책 source를
수동 지정했다. 따라서 무선 중단 중 유선 통지 경로·원인 파일 연동·정상 재부팅은 검증했지만
실제 고장 자동 검출이나 강제 reboot/sysrq fallback까지 통과한 것으로 확대하지 않는다.

수신기는 158프레임의 송신원·version/type·선언 길이를 대조했고, ResetNotice는 seq 157,
Length 60, 전체 68바이트, cause `0x00000012`로 1회 수신했다. boot ID는
`4667e1c0-fbac-4fab-97ff-7c27e7510f86`에서 `af6f7263-e589-4291-8137-d92eb3d6f4e2`로
변경됐다. 타겟과 수신기의 wall clock은 달랐으므로 수신기 monotonic 기록과 boot ID를
기준으로 구분한다. 이전 부팅의 표준 journal은 persistent 저장이 없어 조회 결과가 비었으며,
전체 종료 순서를 journal로 독립 입증한 것은 아니다.

원복 보고서와 별도의 fresh audit에서 파일 SHA-256·소유권·권한, 양 인터페이스 드라이버
상한, MAC·IP·경로, 기록된 모든 서비스의 enabled/active 상태가 기준과 일치했다.
원본 OPCd(`803f28e5…`)는 다시 inactive이고 `jhw_wlan_`/5180MHz에 재연결하여
`ping -I mlan0` 2/2를 확인했다. 재부팅·MAC 원복 구간의 SSH 시간 초과는 재연결 후
최종 상태로 대조했다. 복구 타이머의 LastTrigger는 0이었다.

원본 증거는 부모 저장소 `.omx/plans/opcd-reset-notice-20261013-v6jp9i/qa-summary.json`에
있다. 수신 파서 경계 테스트 7건과 ARM64 NXP 빌드가 통과했다. 이어 수행한 서비스 종료 중
fallback 생존성과 OPCd 재시작 시험은 아래에 기록한다. R3/R6/R7/R8, 실제 펌웨어 고장,
다른 양산 IP 프로파일 및 최종 설치 패키지는 이 시험의 통과 범위가 아니다.

#### 재부팅 fallback 생존 / R4·R5 OPCd 재시작 결과 (2026-10-13)

타겟의 systemd 254에서 기존 정책의 `do_reboot`·로그·sync·journal 경계만
시험용 표식/no-op으로 바꾼 사본을 실행했다. 정책 인자 처리·제한·lock·원인 파일은
그대로 사용하고 저장 경로만 전용 시험 디렉터리로 격리했다. 호출 서비스가 정지될 때
기존 자식도 종료돼 다음 단계가 실행되지 않는 결함을 재현했다. 수정본은 전체 정책을
`DefaultDependencies=no`인 독립 transient service에서 실행하므로 호출 서비스가
정지돼도 다음 두 표식이 순서대로 실행됐다. 재부팅 예산은 한 번만 적용됐고, 최종 실패
127 이후 원인 파일과 lock도 정리됐다. 실행기 시작 실패 시 호출 서비스 안에서 재부팅을
대신 실행하지 않는다. shutdown 기본 의존성과 transient service의 독립 실행은
[systemd v254 service](https://raw.githubusercontent.com/systemd/systemd/v254/man/systemd.service.xml)와
[systemd-run](https://raw.githubusercontent.com/systemd/systemd/v254/man/systemd-run.xml)의 계약을 따른다.
실제 `reboot -f`/sysrq 쓰기나 최종 shutdown의 전체 프로세스 종료는 이 시험 범위가 아니다.

OPCd 시험은 실제 타겟의 systemd와 제품 unit 설정을 사용하되 별도 ARM64 **stub**
후보(`d9c4a8ff…`)·UDP 55173/loopback 55174·전용 데이터/임시 영역에서 수행했다.
실제 NXP 무선 드라이버 장애를 주입하지 않았다. watchdog은 실제 OPCd 메인 루프에서
송신되며 별도 worker/thread heartbeat로 정지를 감추지 않는다.

| Check | Result |
|---|---|
| Caller service stops | Independent policy reaches forced/sysrq marker stages |
| Healthy OPCd for 32 s | Same PID; zero restarts; KeepAlive received |
| R4: QA PID receives SIGKILL | New PID and UDP response in 2.582 s |
| R5: QA PID receives SIGSTOP | Stopped state and UDP timeout; watchdog restart in 31.233 s |
| Session after each restart | Login required; receiver silent until reconfigured; KeepAlive resumes |
| Final restoration | Original files/services/network/AP/boot ID match; wired and WLAN ping 2/2 |

R5 원인은 SIGSTOP 직전 journal cursor 이후 PID 1의 구체적인 watchdog timeout과
`UNIT_RESULT=watchdog`로 확인했다. 재시작한 OPCd는 이전 로그인/Indication 수신기
설정을 기억하지 않으므로 고객 클라이언트가 재로그인·SetIndicationConfig를 다시 해야 한다.
OPCd 자체가 멈추거나 SIGKILL되는 경우에는 사전 ResetNotice를 보장하지 않는다.

첫 시험에서는 `TemporaryFileSystem=/dev/shm`과 `ReadWritePaths=/dev/shm`을 함께
지정해 호스트 임시 영역이 노출됐고 stub NG 응답 snapshot이 남아 원복 검사가 실패했다.
별도 harmless mount probe로 원인을 확인하고 중복 쓰기 경로를 제거했다. 이후 시험은
ExecStartPre와 실제 daemon mount의 device/inode/config hash 검사로 격리를 확인했고,
호스트 snapshot 없이 완료됐다. 첫 결과와 검증된 시험 파일의 정리 증거는 보존했다.
시험 중 유선 SSH가 일시 끊겨 컨트롤러가 180초 제한에 도달했으나, 복구 후 같은 boot ID에서
이미 완료된 두 번째 결과와 정리 상태를 수집했다. 연결 중단의 원인은 확정하지 않았다.

정책/호출자 관련 pytest 42건과 소스 manifest 2건, 단독 전체 `make -C wlan-opc check`,
ARM64 NXP 빌드가 통과했다. 로컬 staging의 NXP 바이너리와 unit도 함께 갱신했다.
타겟의 원본 OPCd 바이너리와 unit은 복원된 상태이며 새 제품 watchdog을 상시 배포한 것은 아니다.
증거는 부모 저장소 `.omx/plans/opcd-recovery-watchdog-20261013-0s04gpcz/`에 있다.
R6/R7의 후속 결과는 아래에 기록한다. 최종 NXP 설치 패키지 운용·R3/R8·실제 강제 리셋은
남은 범위다.

#### R6 FaultDetect / R7 저장·로그 실패 추가 결과 (2026-10-13)

R6은 같은 타겟에서 ARM64 stub 후보 `d9c4a8ff…`와 제품 unit을 격리해 실행했다.
전용 UDP 55175/loopback 55176, OPC 데이터와 `/dev/shm`, `/proc/stat`·`/proc/diskstats`·
`/sys/class/net/eth0` 입력을 분리하고 시작 전·실제 PID의 mount device/inode를 확인했다.
호스트 CPU·디스크·망에 실제 부하를 걸지 않고 누적 카운터를 주입했다. 진입 80%, 해제
70%, 측정/통지 주기 각 1초이며 FaultDetect·ResetNotice·KeepAlive를 모두 설정했다.

| Phase | FaultDetect count | Resource IDs | ResetNotice |
|---|---:|---|---:|
| Normal | 0 | - | 0 |
| Hot entry | 3 | 1, 3, 4 | 0 |
| Hot persists | 0 | - | 0 |
| Clear | 0 | - | 0 |
| Hot reentry | 3 | 1, 3, 4 | 0 |

CPU(id 1)는 94%, Disk I/O(id 3)와 Network(id 4)는 100%로 통지됐다. 두 자원의 입력은
210% 상당이며 100으로 제한된다. 각 구간 GetBasicInfo 응답, 동일 PID·재시작 0회·동일
boot ID를 확인했다. 수신 프레임은 실제 길이·declared length·자원 ID·백분율 범위를
검사했다. 이 결과는 공통 producer와 실제 ARM64 UDP 통지 경로의 결정적 시험이며,
실제 자원 포화나 NXP 드라이버 고장을 주입한 결과는 아니다.

R7은 최종 정책 `da2fa2e5…` 사본을 타겟 Bash/GNU timeout 9.4에서 18개 시나리오로
실행했다. 인자 처리·거부·예산·원인 파일·종료 코드 흐름을 유지하고 경로와 외부 명령만
전용 시험 영역으로 바꿨다. `--executor`로 실행하여 독립 service handoff는 앞 시험의
검증에 의존한다. reboot/sysrq는 표식·파일 쓰기, logger/sync/snapshot과 설정 파일은
전용 fixture이며 생성한 스크립트에 실제 변경 경로가 남으면 실행 전에 거부한다.

| Case | Result | Elapsed (s) |
|---|---|---:|
| Logger ignores TERM | Reboot marker reached | 2.148 |
| Sync ignores TERM | Reboot marker reached | 2.155 |
| Snapshot ignores TERM | Reboot marker reached | 10.145 |
| State / cause return ENOSPC | Reboot marker reached | 0.148 |
| State stat/open/read blocks | Reboot marker reached | 2.119 |
| Full stderr plus logger error | Reboot marker reached | 2.144 |
| Cause cleanup ignores TERM | Original rc 127 preserved | 1.167 |
| MFG read blocks | Refused with rc 13 | 1.036 |
| Private real escalation | Reboot, reboot -f, sysrq 1/b; rc 127 | 0.223 |

추가로 logger/sync 오류, cause FIFO, 정상/MFG/잘못된 제조 모드, `--force` 예외,
uptime 거부(10)와 예산 거부(11)를 확인했다. 정상 MFG 거부는 12이며 판정 실패/시간 초과는
스크립트 종료 코드 13으로 안전하게 거부한다. 기존 `--force`, 과열, `wifi_logger_temp`,
`wifi_init` 예외는 제조 모드 읽기 전에 통과한다. 저장 예산 stat/open/read·쓰기와 원인
파일 쓰기/삭제, logger/kmsg/stderr는 각 1초, snapshot은 10초, sync는 2초 제한이다.

처음에는 `timeout -k 1`에서 wrapper가 TERM으로 먼저 끝나면 TERM을 무시한 손자가
남는 결함이 마지막 정리 검사에서 발견됐다. 실제 private jq와 sleep의 PID·그룹·session·
스크립트 해시를 확인해 정리하고 I/O helper를 `timeout -s KILL`로 수정했다. 첫 제한 시간에
별도 프로세스 그룹을 종료하도록 한 동작은
[GNU coreutils v9.4 timeout 소스](https://raw.githubusercontent.com/coreutils/coreutils/v9.4/src/timeout.c)를
확인했다. 회귀는 변경 전 실패·변경 후 통과했고 최종 실기는 잔존 시험 프로세스가 없었다.
초기 timer의 MainPID 검사 오류와 시험 경로 문자열 오탐 결과도 보존했으며 최종 정리 상태는
별도 fresh audit으로 확인했다.

이 초기 시험의 상태 파일 읽기 실패/손상 시 예산 기본값 처리는 릴리스 검토에서 수정했다.
일반 자동 요청은 예산 디렉터리 생성, 기존 기록 파싱/읽기, 새 횟수 영속화가 실패하거나
시간 초과하면 종료 코드 14로 거부하며 원인 파일과 재부팅을 실행하지 않는다. 기록이 없으면
최초 요청으로 초기화한다. 새 기록은 같은 디렉터리의 임시 파일을 sync한 뒤 rename하고
디렉터리도 sync한다. `--force`는 기존 긴급 복구 예외를 유지한다. 로컬 회귀에서 malformed/
빈 기록, 읽기 stall, 실제 EACCES와 sync stall을 거부하고 중단된 쓰기가 이전 기록을 보존함을
확인했다. 이 최종 수정은 보드의 실제 재부팅 시험에 적용하지 않았다.
원인 쓰기/정리 실패 시 이전 유효 원인이 남을
수 있으며, 파일이 없거나 잘못된 경우에만 OPCd가 SYSTEM(0x0002)으로 처리한다.
SIGKILL로도 끝나지 않는 커널 D state·고장 난 저장장치·커널 완전 정지와 실제 강제 리셋은
이 시험에 포함하지 않았다. 일반 제조 모드 판정 실패는 승인 전 안전 거부 분기다.

최종 관련 pytest 98건, 수신/fixture 경계 10건, native fault_probe·handler·reset_cause가
통과했다. 타겟 원본 파일·credential 메타데이터·MAC/IP/경로/AP·service enable/active·
boot ID를 대조하고 유선/무선 ping 각각 2/2 및 모든 시험 프로세스·unit·폴더 정리를 확인했다.
제품 변경은 로컬 staging에 있으며 타겟 원본 정책/OPCd를 상시 갱신하지 않았다.
증거는 부모 저장소 `.omx/plans/opcd-fault-storage-20261013-hfkzjoyo/qa-summary.json`에 있다.
외부 Release는 보류하며 R3/R8와 패키지 후보의 후속 결과는 아래에 기록한다.
최종 설치 경로, 실제 자원 포화·강제 리셋은 남은 범위다.

#### R3 서비스 재시작 / R8 Ethernet 경로 복구 결과 (2026-10-13)

이번 시험은 Single 타겟에서 새 0.7.0 DEB를 추출한 ARM64 **NXP** OPCd
`c969f0d8…`와 제품 unit을 전용 서비스·UDP 55181/55182(R3), 55183/55184(R8)에
격리해 실행했다. `/usr/local/opc/etc`와 `/dev/shm`의 실제 daemon mount를 검사했고,
타겟에서 생성한 임의 시험 자격증명으로 로그인했다. 원본 OPC 비밀번호를 반출하지 않았다.
타겟의 상시 OPCd와 checker/fw_watch/policy 파일은 새 패키지로 설치하지 않았다.
따라서 아래 R3은 **현재 설치된 서비스**의 재시작과 새 NXP OPCd의 응답을 검증한 결과다.

| R3 service | SIGKILL restart (s) | AP check after restart (s) | KeepAlive count |
|---|---:|---:|---:|
| wpa_supplicant@mlan0 | 3.395 | 0.531 | 2 |
| wifi_checker@mlan0 | 5.196 | 0.017 | 3 |
| wlan_fw_watch | 5.196 | 0.017 | 3 |

각 서비스의 실제 MainPID를 한 번씩 SIGKILL했고 새 PID·NRestarts 증가를 확인했다.
fw_watch는 재시작 후 초기 60초 대기가 끝나 5초 감시 sleep으로 들어가기까지 추가
60.279초를 관측했다. 원래 `jhw_wlan_`/5180MHz의 COMPLETED·AP ping과 NXP
GetBasicInfo 응답이 돌아왔고, QA OPCd 재시작·ResetNotice·보드 재부팅은 없었다.
Period 0의 이벤트 수집은 비었으며, 같은 세션을 Period 1로 바꿔 생존성을 확인한 구간에
위 KeepAlive와 supplicant의 WlanStatusChange 2건을 받았다. supplicant 종료는 드라이버
연결을 그대로 남길 수 있으므로 AP 주도 ApDisconnect를 발생시킨 시험은 아니다.

R8에서는 유선 외부 수신기 `192.168.1.2:55183`을 준비하고 무선 SSH로 eth0를 약
7초 DOWN했다. DOWN 직전 독립 UP 복구 타이머의 활성 상태와 잔여 시간을 확인했다.
수신기로의 route는 ENETUNREACH가 됐지만 무선 관리와 loopback GetBasicInfo 10회는
응답했다. DOWN 전 외부 KeepAlive 4건을 받았고 DOWN/UP 뒤에는 추가 수신이 없었다.

| R8 check | Result |
|---|---|
| Ethernet DOWN: wired recipient route | ENETUNREACH |
| Ethernet DOWN: local OPCd queries | 10 ACKs; same QA PID |
| Ethernet UP: carrier / IP / routes | Returned |
| Ethernet UP: data path within 20 s | FAIL; ARP / ping / wired SSH did not recover |
| PHY renegotiation with ethtool -r | FAIL; data path remained unavailable |
| Graceful recovery reboot | Wired data path returned with new boot MAC |
| Restore pre-test runtime MAC while UP | FAIL; wired data path failed again |
| Undo MAC change to boot MAC | Bidirectional wired ping returned |

첫 실행기의 `target_preserved=true`는 파일·망 설정 문자열의 비교일 뿐 실제 도달성 성공이
아니다. 해당 실패 결과와 종료 코드는 보존했다. 이후 관측용 `tcpdump -p`가 동작하는
동안 유선 패킷이 오갔으나 종료 뒤 장애가 다시 나타났으며, 이 상관관계의 원인은 미확정이다.
추가 정상 재부팅은 수동 복구이며 Ethernet 자동 복구 통과로 처리하지 않는다.

재부팅 전 원본 파일·credential 메타데이터·서비스·IP/경로/AP를 대조하고 모든 R3/R8
시험 unit·timer·폴더를 제거했다. 정상 재부팅 1회로 boot ID가
`af6f7263-e589-4291-8137-d92eb3d6f4e2`에서 `e2d706a9-9a05-499a-be4a-a83ff72e8e13`로
바뀌었다. eth0의 부팅 MAC은 `b2:fc:2b:23:63:6b`였고, 원래 런타임 MAC
`ee:4f:f8:cd:83:d6`으로 복원하면 유선 통신이 실패하여 부팅 MAC으로 되돌렸다.
영구 MAC 설정이나 출하 IP 설정은 변경하지 않았다. 최종 운영 복구 판정과 원래 런타임
MAC의 차이는 `operational-recovery-summary.json`에 따로 기록한다.

읽기 전용 sibling 드라이버 소스 `wlan-driver-v2/mlinux/moal_bridge.c`에는 초기
peer_mac 캐시만 있고 NETDEV_CHANGEADDR 갱신이 없다. 캐시와 다른 MAC으로 받은
ARP REPLY는 eth0 소유 IP를 향해도 WLAN 전달 분기로 갈 수 있어 MAC 변경 후 장애의
후보다. 현재 부팅 journal의 masked peer_mac은 부팅 MAC과 일치하는 부분을 보이며
`eth2wlan=rx_handler`를 확인했다. 설치 모듈 `543.p18`/srcversion
`69D92F25B44159057AE055A`와 해당 소스의 동일성은 확인하지 않았다. 최초 DOWN/UP은
MAC이 같았으므로 이 후보만으로 최초 장애나 tcpdump 효과까지 설명하지 않는다.

별도 HW watchdog은 `watchdog.service`가 `/dev/watchdog`을 열고 있으며 부팅 로그에서
실제 timeout 30초 설정을 확인했다. PID 1의 RuntimeWatchdog는 0이다. 커널의
CONFIG_WATCHDOG_NOWAYOUT=y만으로 해당 vendor driver의 runtime nowayout을
확정하지 않았다. 장치 open/ioctl·feeder 정지·커널 정지·강제 리셋은 수행하지 않았으며,
실제 HW expiry 시험은 별도 승인과 물리 복구 수단이 필요한 후속 범위다.

#### 진단 timeout 보완 / 최종 패키지 후보 결과 (2026-10-13)

진단 helper의 TERM 처리 중 wrapper만 종료되고 손자가 남는 결함을 로컬에서 재현했다.
`setsid`로 별도 session을 만들고 TERM trap에서 그 그룹을 KILL하도록 수정했으며,
비동기 실행에서 사라지는 stdin은 전용 FD로 전달한다. 정상 종료 7/137과 timeout 124를
구분한다. 실제 checker/fw_watch를 실행하는 회귀 4건으로 snapshot 10초·sync 2초
제한과 후속 복구 정책 진입을 확인했고 helper 회귀 15건도 통과했다. 모든 진단 I/O나
SIGKILL로 끝나지 않는 D state까지 제한한 것은 아니다. 타겟의 timeout 9.4와
util-linux setsid/flock 2.39.2를 확인했으나 최소 외부 이미지의 도구 공급은 별도 확인한다.

격리된 전체 소스 사본에서 `./build.sh --release`를 실행하여 logger 823건, scripts 349건,
pcap 20건, FTP QA 147건의 pytest **1,339건과 subtest 144건**, shell/schema/native
검사·ARM64 NXP 컴파일·DEB gate·source TAR fixture/실물 gate가 모두 통과했다.
소스 TAR 안전 검사에는 manifest가 이미 허용한 FTP 문서·QA의 정확한 경로 5개를 추가했다.
처음 실패한 git metadata 누락·sandbox loopback 거부·소스 경로 gate 결과도 보존했다.

| Candidate | Version / arch | SHA-256 |
|---|---|---|
| wlan-proc-0.7.0.deb | 0.7.0 / arm64 | 8532bfcd76761bb45839cde41f6c575e86cab0ffa2f86a3f60501535b742066c |
| wlan-package.tar | source | af38ac7deb7d1718790c256286cc3a54b46e3935634bd44ad8c0ac0956adb3b6 |

DEB를 설치 없이 추출하여 버전·아키텍처·AArch64 ELF·NXP symbol과 주요 payload의
바이트·권한을 검증했다. 타겟에서 새 monitor/policy를 상시 설치한 결과나 설치·업그레이드·
postinst의 전체 검증은 아니다. Yocto/BitBake 태스크는 실행하지 않았다. 후보 아카이브와
원본 증거는 부모 저장소 `.omx/plans/opcd-r3-r8-package-20261013-omaneagb/`에 보존한다.
외부 Release는 보류하며 Ethernet 복구 결함 분석·설치 경로·실제 HW watchdog expiry가
남은 검증 범위다.

### `--dump` vs `--hex` (출력 모드)
- `--dump` — 프레임 전체 raw hexdump(16B/줄, 필드 경계 없음). TX/RX/listen 수신(IND)에 적용.
- `--hex` — **필드 단위 분해**: `라벨 @절대오프셋 (크기): raw hex = 디코딩값`. `basic-info`/`device-info` ack와 `listen` 수신 indication에 적용. 켜면 그 명령의 디코드 출력을 대체하며, `--dump`와 독립(둘 다 켜면 둘 다 출력).
```
$VHL --hex device-info       # 헤더 5필드(@000~) + body 전 필드(@064~) 분해
  vendor_code        @068 ( 4B): 00 90 2c fb       = 0x00902cfb
  essid              @204 (32B): 6a 68 77 5f 77 …  = 'jhw_wlan'
  wlan1.rssi         @299 ( 1B): b9                = -71
./build/native/vhlctl/vhlctl --hex listen --bind 0.0.0.0:9999   # 수신 indication을 필드별 hex로
```

### indication bit (set-indication --bits, ids.h)
`0x01` InitComplete · `0x02` WlanStatusChange · `0x04` Roaming · `0x08` ApDisconnect ·
`0x10` FaultDetect · `0x20` ResetNotice · `0x80` KeepAlive

> **이벤트성 producer 구현됨**(2026-06-15): FaultDetect(0x10) 폴링 폭주 프로브(`fault_probe.c`) · WlanStatusChange(0x02)/Roaming(0x04)/ApDisconnect(0x08) nl80211 연동(PR #46, `nxp_drain_events`). 무선 이벤트 트리거는 **`wpa_cli -i mlan0 disconnect/reconnect`** 사용(`iw … disconnect`는 wpa_supplicant가 SME 소유라 "Operation not permitted"). ApDisconnect는 **AP-주도 deauth**(`NL80211_ATTR_DISCONNECTED_BY_AP`)일 때만 발행 — 로컬 disconnect는 WlanStatusChange만 냄. 실타깃 검증(2026-06-15, 214.5): WlanStatusChange ✓ / Roaming·ApDisconnect는 트리거 환경 잔여(#47). 상세 절차는 `docs/testing/manual-runthrough.md` §4 / 세션 메모리 참조.
> 항상 발생: InitComplete(0x01, enable/login/logout 시), KeepAlive(0x80, period마다), ResetNotice(0x20, reset 시).

### 장치 리셋 통지(0x0020) 자율 리셋 producer — Reset Cause 표와 `/run/opc/reset_cause` (#47 T9)

사양 §3.4.6/§4.4.6의 Reset Cause는 "장치별 리셋 요인 ID"라 당사 정의(문의 Q10 참고 항목). 값은 `protocol/ids.h` `OPC_RESET_CAUSE_*`:

| 값 | 의미 | 발원 |
|---|---|---|
| 0x0000_0001 | 운용자 Reset 요구 | opcd `handle_reset` |
| 0x0000_0002 | 시스템 재부팅/종료, 원인 미상 | opcd SIGTERM(시스템 종료 중) — cause 파일 없음 |
| 0x0000_0010 / 0x11 / 0x12 | 무선 IF 소실·FW 크래시 / station dump 장애 지속 / 드라이버 wedge | wifi_checker / wlan_fw_watch |
| 0x0000_0020 | 과열 복구 재부팅 | wifi_logger_temp |
| 0x0000_0030 | wifi_init 실패 | wlan_emergency_reboot |
| 0x0000_0040 | 공장 초기화 후 재부팅 | factory_reset |

동작: WLAN 재부팅 정책은 승인된 요청의 `--source/--reason`을 Reset Cause로 기록한 뒤
재부팅을 시도한다. opcd는 SIGTERM에서 `systemctl is-system-running`(3초 제한)이
`stopping`이면 `/run/opc/reset_cause`를 읽고, 파일이 없으면 0x0002로 ResetNotice를
한 번 시도한다. `After=network-online.target`만으로 supplicant/라우팅의 생존이나
상대 수신을 보장하지 않는다. 통지 설정과 실제 송신 경로가 살아 있어야 한다.
단순 `systemctl stop opcd`나 종료 상태 판정 불가에서는 통지하지 않는다.
HW watchdog·강제 리셋·커널 완전 정지에서는 사전 통지를 보장할 수 없다.

실기 확인: `vhlctl set-indication --bits 0x20`(RESET_NOTICE) 후 리스너를 영속 파일로 캡처(`vhlctl --hex listen --bind 127.0.0.1:50699 > /root/reset_notice.log`)한 채 `systemctl reboot` → 재부팅 후 로그에 `reset_cause` 프레임 확인. 음성 대조군: `systemctl stop`(또는 `kill -TERM`)만으로는 프레임이 없어야 한다. 호스트는 `opcd/tests/test_reset_cause.c`(가짜 systemctl 스크립트·타임아웃·파일 파싱·판정).

### 관리 IP 토폴로지 — `wifi_init_conf.json` peer_route / opc.conf `management_topology` (#122)

| 소스 | 값 | 의미 |
|---|---|---|
| `/usr/local/etc/wifi_init_conf.json` `.wbridge.peer_route.enabled` | true/false | **운영 기준**. true면 관리 IP 1개를 mlan0(primary)+eth0 /32 미러가 공유하는 peer_route 토폴로지. wifi_init.sh와 같은 판정표: 파일 없음→eth0_ip(degraded), 파일 있고 키 없음/무효→**peer_route(공장 기본)** |
| `opc.conf` `management_topology` | `auto`(기본)·`eth0_ip`·`peer_route` | 테스트/벤치용 override. `auto`는 JSON을 따름 |

peer_route에서는 device-info IP 삼중항과 ChangeIp 적용이 `device_ip_iface`와 무관하게 mlan0를 따르고(기동 시 경고 1줄), ChangeIp 성공 후 eth0 /32 미러(첫 주소로 재배치)·table 100 link route·eth0 경로의 `src`(peer host route, eth_fallback metric 200)가 새 IP로 갱신된다(`nxp_peer_route_refresh`, 실패 시 로그만). 호스트에서는 `unshare -rn` + dummy eth0/mlan0로 시퀀스를 드라이런할 수 있다 — 픽스처는 **mlan0에 NEW만, eth0에 강등 /24 + OLD/32**(apply 후 상태)로 만들어야 커널 prefsrc flush가 재현된다(PR #122 검증 절차). 호스트 stub는 `stub_peer_route_refresh_*`로 호출을 관측한다(`test_handler.c` 28). 실기 검증은 `wifi 0 br profile peer-route apply` + 재부팅 후 ChangeIp → `ip -4 addr show eth0`(새 IP /32)·`ip route show table 100`·`ip route show dev eth0`(src) 확인.

### FaultDetect(0x0010) 폭주 probe — opc.conf `congestion_*` 키

| 키 | 기본 | 의미 |
|---|---|---|
| `congestion_probe_interval_s` | 10 (1..3600) | 자원 사용률 측정 주기(장치 고유, Indication Period와 무관 — #121). Period 0에서도 측정·즉시 통지 |
| `congestion_threshold_pct` | 80 | CPU/Memory/Disk/Net 공통 **진입** 임계(%) |
| `congestion_clear_pct` | 진입 임계 − min(10, 진입 임계/2) (1..100) | **해제** 임계(%) — 사용률이 이 값 아래로 내려가야 래치가 풀린다(#141). **미지정이면 설정된 진입 임계에서 파생**되므로 `congestion_threshold_pct`만 낮춰도 밴드가 사라지지 않는다. 폭은 진입 임계의 **절반을 넘지 않는다** — 낮은 임계에서 밴드가 범위 전체를 삼켜 오히려 둔감해지는 것을 막는다(80→70, 20→10, 10→5, 5→3, 1→1). 진입 임계를 넘는 값·`0`·숫자가 아닌 값은 **거부/클램프되고 stderr에 기록**된다. 히스테리시스를 끄려면 해제 임계를 **진입 임계와 같게** 설정한다 |
| — (참고) | | 밴드 안(해제~진입)에 머무는 자원이 `SetIndicationConfig` 후 재통지될 때 `current_val`은 **진입 임계보다 낮을 수 있다**(해제 임계까지). 밴드 규칙상 여전히 폭주 중이므로 모순이 아니다 |
| `congestion_net_if` / `congestion_net_capacity_mbps` | eth0 / 1000 | 네트워크 사용률 소스·링크 속도 폴백 |
| `congestion_disk_dev` | mmcblk0 | `/proc/diskstats` 장치명 |

통지는 자원별 **폭주 진입 시 1회**(임계 미만→초과 전이). 지속 중 재통지 없음, 해소 통지는 발주처 Q6 회신 대기. 로컬 재현은 `opcd/tests/test_handler.c` 23 블록(합성 `/proc/stat`) 참조.

### ErrorCause (protocol/ids.h) — `0x0010`은 다의(ARCH-001)
`0x0001` login-violation · `0x0002` login-condition · `0x0010` indication-violation/pw-mismatch/slot-range/station-type ·
`0x0011` slot-empty · `0x0012` ip-change-conflict · `0x0013` radio-mode · `0x0014` radio-bw · `0x0050` radio-apply
→ 같은 와이어값은 **명령 컨텍스트로 해석**(spec 고정값).

---

## 주의사항

- **세션 = 소스 IP로 식별** → 같은 호스트의 연속 vhlctl 호출은 로그인 유지. **idle(기본 300s)** 초과 시 자동 로그아웃.
- `device-info`의 `ip_address`는 기본 **eth0 기준**(무선 IP와 별개), `opc.conf::device_ip_iface = mlan0`이면 **mlan0 기준**(옵션 X/mlan0-IP 토폴로지용 — change-ip 적용 대상도 함께 전환). 선택된 인터페이스가 down/IP 미보유(무선 미접속 포함)이면 `0.0.0.0` — 관리평면의 실상태를 그대로 보고.
- **상태변경 명령 주의**: `set-password`/`set-ip-list`/`set-radio`는 `/usr/local/opc/etc`에 영구 저장
  (set-radio는 wpa_supplicant conf의 freq를 실제 수정). `change-ip`는 nxp 백엔드에서 eth0 관리 IP를
  `ip addr`로 **실제 변경**(런타임만 — reboot 시 22-eth0.network로 복원되는 휘발성, wifi_init.sh의 mlan /32는 보존).
  리스트의 Gateway(0.0.0.0 제외)는 관리 IF의 기본 경로(`ip route replace default`), NTP(0.0.0.0 제외)는
  `/run` timesyncd drop-in로 역시 휘발 적용한다(#162). ESSID는 wpa conf 편집이라 비휘발이므로 편집 직전에
  `/usr/local/opc/etc/changeip_essid.restore` 표식(원래 `ssid=`·슬롯·시각·요청 SSID `target=` hex)을 먼저 쓰고,
  적용 성공 뒤 conf의 현재 `ssid=` 토큰을 `applied=`로 커밋한다. 실패·타임아웃이면 conf를 다시 읽어 요청 SSID로
  디코드되면 커밋, 다른 값(다른 작성자)이면 커밋하지 않고(미커밋 표식 삭제·커밋 체인은 target 제거), 안 바뀌었으면
  미커밋 표식 삭제·커밋 체인은 target 제거(`settle`). SIGKILL 뒤 자식 rename(2) 한 번의 창은 잔여. 부팅 시 `opcd -R`이 conf가 커밋값이거나 `target=`으로 디코드될 때만
  원래 값으로 되돌리고(#161; 커밋 전 전원 차단도 복원), 그 밖의 값이면(다른 작성자) 표식만 지우고 conf는 손대지
  않는다(rc=2 로그). 주소는 `ip addr replace` 뒤 global scope 존재를 확인한 다음에만 다른 주소를 지우고(같은 IP 재선택 시 EEXIST 없이
  GW·NTP 적용, host-scope /32 미러와 주소·프리픽스가 같은 /32 슬롯이면 실패), /32 슬롯의 게이트웨이는
  `onlink`. drop-in은 `NTP=`(빈 값)로 /etc 목록을 먼저 비운 뒤 서버를 넣는다. 호스트 검증: `test_essid_marker`(표식
  arm/commit/settle/disarm/restore·외부 변경·전원 차단·hex 토큰 모사)와 `test_platform_nxp_radio`(실제
  `platform_nxp.c`, execv 가로채기로 호출 순서·인자·실패/설치 후 실패 시 표식 상태·onlink 확인). **실기에서만 확인
  가능**: 변경→reconfigure 후 ESSID 유지→재부팅 후 IP·ESSID 원복, 기본 경로·timesyncd drop-in 실동작, `opcd -R`이
  wifi_init.sh의 conf 변환과 wpa_supplicant보다 먼저 도는 유닛 순서(wlan-package#362; -R은 conf writer lock을 잡지 않음).
  `reset`(#164)은 Ack → ResetNotice → `wlan_reboot_policy.sh --source opc_reset --reason opc_reset_command --force`(20 s 상한)로
  보드를 재부팅한다. 스크립트는 재부팅을 transient unit(`systemd-run --wait`)에 넘기므로 정상 경로에서 0으로 끝나지 않는다:
  재부팅의 stop job이 opcd 쪽 클라이언트를 SIGTERM으로 끊거나(SIGTERM 종료만 그렇게 분류하고, 다른 시그널 종료는 hand-off의 증거가
  아니라 "미확인"→재시작 폴백) 20 s 상한이 먼저 지나며, 둘 다 "재부팅 진행 중"으로
  분류돼 opcd는 exit 0으로 끝난다. exit 0은 dedupe 락 충돌뿐이다(다른 요청이 락을 쥐고 있음 — 그 요청이 자기 게이트에서 거부할
  수도 있어 "미확인"으로 적는다). exit≠0 뒤 최대 5 s 안에 opcd 자신의 stop SIGTERM이 오면(opcd는 TERM을 막아 두므로
  대기 중으로 남는다) "재부팅 진행 중"으로 분류한다(#179: 실기에서 셧다운이 D-Bus를 멈추자 클라이언트가 exit 1로 끝났고 약 10 ms 뒤
  opcd.service의 stop job TERM이 왔다). 그 TERM이 오지 않은 exit≠0이면 재부팅 없이 opcd만 재시작된다(systemd RestartSec 2s, 감사 로그 "reset hook started
  no reboot"): `--force` 아래서는 정책 게이트(10-14)가 적용되지 않으므로 exit≠0은 systemd-run이 실행기를 못 띄웠거나 실행기가 중단된
  경우이고, 126/127은 실행 불가(스크립트의 소진 코드 127은 상한 안에 도달할 수 없다)다. fork/setpgid 실패(-errno)도 재부팅 없이
  끝난다. SIGTERM이 아닌 시그널 종료는 재부팅 여부를 알 수 없는 "미확인"이다 — 클라이언트가 transient unit 생성 전에 죽었을 수도,
  hand-off 뒤에 죽어 재부팅이 이어질 수도 있다 — 그래서 별도 반환값(`OPC_RESET_UNCONFIRMED`)과 감사 로그 "reset hook could not
  confirm a reboot"로 구분하고, opcd는 같은 재시작 폴백으로 끝난다(보드가 살아 있으면 재기동). exit 0 뒤 락을 쥔 다른 요청이
  거부하는 경우는 opcd에서 알 수 없다. 상한 뒤에
  재부팅 경로가 모두 소진되는 경우는 opcd에서 구분할 수 없다(그때는 보드가 그대로 살아 있고 opcd는 이미 재시작됨). `--force`는
  MFG/uptime/cooldown/loop 게이트를 건너뛰지만 dedupe 락은 건너뛰지 않는다. 실기 확인 항목: Ack 뒤 ResetNotice 수신 순서,
  재부팅 발생, 정상 리셋의 경로, 스크립트 부재 시 재시작 폴백. **근거는 판정 가능한 출처로**: 순서는 `--hex listen` 리스너가 Ack를
  받지 않으므로(별도 소켓) `/var/log/cantops/logger.log`에서 `TX Reset ack seq=` 줄이 `IND ResetNotice(0x0020)` 줄보다 앞인지로,
  통지 1회·cause 0x01은 리스너 캡처로, 정상 리셋 경로는 `journalctl -b -1 -u opcd`의 "policy client terminated by signal" 또는 "then opcd received its stop signal"(#179)로 본다
  (reboot 뒤에 찍히므로 정책 스크립트의 journald 스냅샷에는 없고 영속 journal이 필요; 아니면 시리얼 콘솔). 감사 로그의 "reboot under
  way or held by another request"는 rc 0의 모든 경우에 같아 경로를 구분하지 못한다. **`test-all.sh` §8은 보드를 재부팅시키고
  boot_id 변경·opcd·basic-info 재도달을 기다린 뒤 이 두 근거를 확인한다**(SETUP은 이전 실행의 `etc.testbak`이 남아 있으면 중단, TEARDOWN 복원 실패는 FAIL).
- 실장치 배포 시 **반드시 크로스빌드**(`make arm64 PLATFORM=nxp` → `build/arm64/`), 배포 전 바이너리 백업 권장.
