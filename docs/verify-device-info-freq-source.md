# device_info_freq_source 토글 실타겟 검증 절차

> 대상: PR #60 (`opc.conf`의 `device_info_freq_source = config|live|auto`).
> 설계: `docs/design-device-info-freq-source.md` · 일반 테스트법: `docs/testing-guide.md`.
> 플레이스홀더: `<OPC_IP>` = 무선보드(opcd 상주), `<VHL_IP>` = pim-camera(vhlctl 실행).
> opcd는 UDP :50607 systemd 상주, opc.conf = `/usr/local/opc/etc/opc.conf`.

## 실측 기준값
| 출처 | freq | ch(와이어) | 근거 |
|---|---|---|---|
| 설정(radio.conf) | 5180 | `0x0224` | 5GHz ch36 → `0x02<<8 \| 0x24` |
| 접속(live, AP FXE3000_JHW) | 5240 | `0x0230` | `opc_chan_field(5240,48)` = `0x02<<8 \| 48` |
| 미접속(live) | 0 | `0x0000` | associated=false → 0/0 |

동작(`handler.c` `select_devinfo_freq_ch`): CONFIG → radio.conf 캐시(5180/0x0224) / LIVE·AUTO+접속 → `freq=live` + `ch=opc_chan_field()`(0x0230) / LIVE+미접속 → 0/0 / AUTO+미접속 → 설정값.

## 1. 베이스라인 (VHL 호스트=pim-camera)
```bash
cd <repo-root>/wlan-package/wlan-opc
make native
export VHL="./build/native/vhlctl/vhlctl --host <OPC_IP> --port 50607 --timeout 2000"
vhl(){ for t in $(seq 1 8); do $VHL "$@" && return 0; echo "  ..retry $t"; done; return 1; }
vhl login --password MyPassword; vhl device-info | grep -i wlan; vhl logout   # 출발 상태 기록
```

## 2. AP 접속 확인 (OPC 보드)
```bash
ssh wlan-target
wpa_cli -i mlan0 status | grep -E "wpa_state|freq|ssid"   # COMPLETED / FXE3000_JHW / 5240
cat /usr/local/opc/etc/radio.conf 2>/dev/null            # set-radio 캐시(5180/0x0224)
exit
```

## 3. config 모드 (출하 기본 — 항상 설정값)
```bash
ssh wlan-target
cp /usr/local/opc/etc/opc.conf /usr/local/opc/etc/opc.conf.bak   # ★ 최초 1회 백업
sed -i '/^[[:space:]]*device_info_freq_source[[:space:]]*=/d' /usr/local/opc/etc/opc.conf
printf 'device_info_freq_source = config\n' >> /usr/local/opc/etc/opc.conf
systemctl restart opcd && systemctl is-active opcd               # ★ 파서는 부팅 1회 → restart 필수
exit
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout
```
기대: `freq=5180 ch=0x0224` (접속 중이어도 설정값).

## 4. live 모드 (항상 접속값)
```bash
ssh wlan-target -- "sed -i '/^[[:space:]]*device_info_freq_source[[:space:]]*=/d' /usr/local/opc/etc/opc.conf; printf 'device_info_freq_source = live\n' >> /usr/local/opc/etc/opc.conf; systemctl restart opcd"
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout
```
기대(접속 중): `freq=5240 ch=0x0230`.

## 5. auto 모드 (접속 시 live)
```bash
ssh wlan-target -- "sed -i '/^[[:space:]]*device_info_freq_source[[:space:]]*=/d' /usr/local/opc/etc/opc.conf; printf 'device_info_freq_source = auto\n' >> /usr/local/opc/etc/opc.conf; systemctl restart opcd"
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout
```
기대(접속 중): live와 동일 `freq=5240 ch=0x0230`.

## 6. 와이어 hex 확인 (`--hex`, live 모드에서)
```bash
$VHL login --password MyPassword
$VHL --hex device-info | grep -E "wlan1.freq_mhz|wlan1.channel"
$VHL logout
```
기대:
```
wlan1.freq_mhz   @292 ( 2B): 14 78  = 5240
wlan1.channel    @294 ( 2B): 02 30  = 0x0230   (band 0x02=5GHz | ch 0x30=48)
```
(config 모드면 `14 3c`=5180 / `02 24`=0x0224.)

## 7. 엣지: 미접속 (절단은 wpa_cli만 — iw 불가)
```bash
ssh wlan-target -- "wpa_cli -i mlan0 disconnect; wpa_cli -i mlan0 status | grep wpa_state"   # DISCONNECTED/SCANNING

# 7a. live + 미접속 (모드는 §4에서 live 유지)
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout   # 기대 freq=0 ch=0x0000

# 7b. auto + 미접속
ssh wlan-target -- "sed -i '/^[[:space:]]*device_info_freq_source[[:space:]]*=/d' /usr/local/opc/etc/opc.conf; printf 'device_info_freq_source = auto\n' >> /usr/local/opc/etc/opc.conf; systemctl restart opcd"
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout   # 기대 freq=5180 ch=0x0224 (설정값 폴백)

# 7c. 접속 복원 (필수)
ssh wlan-target -- "wpa_cli -i mlan0 reconnect; sleep 3; wpa_cli -i mlan0 status | grep -E 'wpa_state|freq'"   # COMPLETED / 5240
```

## 8. 원복 (★ 필수)
```bash
ssh wlan-target -- "cp /usr/local/opc/etc/opc.conf.bak /usr/local/opc/etc/opc.conf; systemctl restart opcd; systemctl is-active opcd"
vhl login --password MyPassword; vhl device-info | grep -i "WLAN#1"; vhl logout   # §1 베이스라인과 동일해야 함
```

## 기대결과 요약
| # | 모드 | 접속 | freq | ch(와이어) |
|---|---|---|---|---|
| 3 | config | 접속 | 5180 | `0x0224` |
| 4 | live | 접속 | 5240 | `0x0230` |
| 5 | auto | 접속 | 5240 | `0x0230` |
| 6 | live `--hex` | 접속 | `14 78` | `02 30` |
| 7a | live | 미접속 | 0 | `0x0000` |
| 7b | auto | 미접속 | 5180 | `0x0224` |
| 8 | 백업복원 | 접속 | §1과 동일 | — |

## 주의점
1. **파서 부팅 1회 로드** — opc.conf 편집 후 반드시 `systemctl restart opcd`(런타임 반영 아님).
2. **세션 = 소스 IP** — restart마다 세션 초기화 → 각 단계 재login(절차에 포함).
3. **indication ON 중 device-info는 NG 0x0010** — 검증 중 `set-indication` 켜두지 말 것.
4. **AP 절단은 `wpa_cli -i mlan0 disconnect`만** — `iw … disconnect`는 wpa_supplicant SME 소유라 "Operation not permitted". 종료 시 `reconnect` 복구 필수.
5. **판독** — ch는 항상 `band<<8|ch`(0x0224=5GHz/ch36, 0x0230=5GHz/ch48), freq는 raw MHz. DUAL wlan2 예: 5745/ch149 → `0x0295`.
6. **이 토글은 freq/ch만 좌우** — mode/bw/essid/RSSI는 무관(항상 live 우선).
7. **원복 후 §1 재확인 필수** — 출하 기본(config) 동작 무변화 입증.
