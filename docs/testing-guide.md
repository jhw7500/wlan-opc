# wlan-opc 테스트 가이드

opcd / vhlctl / protocol을 직접 테스트하는 방법. 3가지 레벨(단위테스트 → 로컬 e2e → 실장치 e2e)과
device-info 데이터 소스 확인법을 다룬다.

> 시점별 검증 결과(실측값)는 `tmp/device_test_*.md` 리포트를, 1차 인수 체크리스트는
> `docs/manual-runthrough.md`를 참조. 이 문서는 **방법**만 담는다.

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
$VHL logout                                                        # → KeepAlive 중단 관측(teardown)
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
| ntp_server | `/etc/systemd/timesyncd.conf` 의 `NTP=` | dynamic | 매 요청 |
| ethernet_mac, ip/netmask/gateway | `/var/log/cantops/json/eth0/link.json` | live | 매 요청 |
| essid, WLAN mac, AP mac, RSSI, SNR, mode, bw | `/var/log/cantops/json/mlan0/link.json` (mlan1=WLAN#2) | live | 매 요청 |
| WLAN 개수(1/2) | `/var/log/cantops/json/mlan1/link.json` 존재 여부 | live | — |
| freq/channel, station_type, priority_ch | `/usr/local/opc/etc/radio.conf` (set-radio 캐시) | config | set-radio 시 |
| device_status | opcd 런타임 세션 | runtime | 실시간 |

mlan0/link.json 키 매핑: `info.ssid`→essid, `info.address`→WLAN mac, `link.address`→connect_ap_mac(=associated),
`link.signal_avg`→RSSI, `channel_info.<freq>.noise`→SNR(`rssi−noise`), `link.tx_bitrate`(HE-/VHT-/MCS)→mode,
`info.width`→bw. freq/channel은 radio.conf 캐시, mode/bw는 live가 있으면 우선.

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
  set-ip-list  --slot N --flag start|cont|end --ip .. --mask .. --gw .. --ntp .. --essid NAME
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
| `set-indication --to <유니캐스트>` | `OK` |
| indication ON 중 `device-info` | `NG 0x0010` |
| `logout` 후 재로그인 → `device-info` | `OK` (teardown) |

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

> **이벤트성 producer 구현됨**(2026-06-15): FaultDetect(0x10) 폴링 폭주 프로브(`fault_probe.c`) · WlanStatusChange(0x02)/Roaming(0x04)/ApDisconnect(0x08) nl80211 연동(PR #46, `nxp_drain_events`). 무선 이벤트 트리거는 **`wpa_cli -i mlan0 disconnect/reconnect`** 사용(`iw … disconnect`는 wpa_supplicant가 SME 소유라 "Operation not permitted"). ApDisconnect는 **AP-주도 deauth**(`NL80211_ATTR_DISCONNECTED_BY_AP`)일 때만 발행 — 로컬 disconnect는 WlanStatusChange만 냄. 실타깃 검증(2026-06-15, 214.5): WlanStatusChange ✓ / Roaming·ApDisconnect는 트리거 환경 잔여(#47). 상세 절차는 `docs/manual-runthrough.md` §4 / 세션 메모리 참조.
> 항상 발생: InitComplete(0x01, enable/login/logout 시), KeepAlive(0x80, period마다), ResetNotice(0x20, reset 시).

### ErrorCause (protocol/ids.h) — `0x0010`은 다의(ARCH-001)
`0x0001` login-violation · `0x0002` login-condition · `0x0010` indication-violation/pw-mismatch/slot-range/station-type ·
`0x0011` slot-empty · `0x0012` ip-change-conflict · `0x0013` radio-mode · `0x0014` radio-bw · `0x0050` radio-apply
→ 같은 와이어값은 **명령 컨텍스트로 해석**(spec 고정값).

---

## 주의사항

- **세션 = 소스 IP로 식별** → 같은 호스트의 연속 vhlctl 호출은 로그인 유지. **idle(기본 300s)** 초과 시 자동 로그아웃.
- `device-info`의 `ip_address`는 **eth0 기준**(무선 IP와 별개). eth0 down이면 `0.0.0.0`.
- **상태변경 명령 주의**: `set-password`/`set-ip-list`/`set-radio`는 `/usr/local/opc/etc`에 영구 저장
  (set-radio는 wpa_supplicant conf의 freq를 실제 수정). `change-ip`는 nxp 백엔드에서 eth0 관리 IP를
  `ip addr`로 **실제 변경**(런타임만 — reboot 시 22-eth0.network로 복원되는 휘발성, wifi_init.sh의 mlan /32는 보존).
  `reset`은 opcd 재시작(systemd RestartSec 2s).
- 실장치 배포 시 **반드시 크로스빌드**(`make arm64 PLATFORM=nxp` → `build/arm64/`), 배포 전 바이너리 백업 권장.
