# bd_provision.sh — 무선기판 현장 프로비저닝 번들

DFK 배정값(관리 IP/GW/NTP/무선 자격증명)을 기기 1대에 원자적으로 적용하는 **1회성 운영자 도구**.
배경: opcd 프로토콜로는 wlan0 IP·GW·NTP를 설정할 수 없고(코드 확정), 반영 지점이 파일 4곳+재부팅으로
흩어져 있어 부분 적용 시 고장 상태가 됨 — `docs/dfk-meeting/dfk-consolidated-questions-20260708.md` §4.
**NTP는 이 스크립트의 `--ntp`가 공식 반영 경로다** (opcd 프로토콜 NTP 필드는 의도적 미적용 —
결정 2026-09-02 #91·2안, 근거는 `docs/spec-inquiry/spec-conformance.md` V3).

## 설계 원칙 (사용자 지시, 2026-09-02)

- **부팅 시 자동 감지·자동 교정 없음.** systemd 유닛·부팅 훅·디스패처를 설치하지 않는다.
  (충분한 검증 후 필요 시 별도 결정으로만 도입)
- `verify`는 **읽기 전용** — 불일치를 보고만 하고 절대 수정하지 않는다(FAIL 시 exit 1).
- 모든 쓰기는 tmp 생성→검증→원자 mv. 변경이 없으면 백업도 만들지 않는다(rollback 이력 보호).

## 사용

```sh
# 1) 미리보기 (시스템 무변경)
./bd_provision.sh plan  --ip 172.22.130.7/17 --gw 172.22.129.254 --ntp 172.22.2.1 \
                        --ssid SITE_SSID --psk 'passphrase' --profile b
# 2) 적용 (백업 → 반영 → set-ntp true → 재부팅 안내; --reboot 시 실제 재부팅)
./bd_provision.sh apply <동일 인자> [--reboot] [--force]
# 3) 재부팅 후 점검 (읽기 전용; --psk 불필요)
./bd_provision.sh verify --ip 172.22.130.7/17 --gw 172.22.129.254 --ntp 172.22.2.1 --ssid SITE_SSID --profile b
# 4) 문제 시 복원 (적용 직전 백업 tar; 자동 재부팅 안 함)
./bd_provision.sh rollback [--backup FILE.tar]
```

- `--profile b` = Option B 3종 토글(peer_route=on·ip_discovery=on·arp_ignore_always=off) 적용.
  `--profile a`(기본) = 토글 불변, 현재값 보고만.
- 대상 파일: `20-mlan0.network`(Address·Gateway) / `timesyncd.conf`(NTP=) /
  `wpa_supplicant-mlan0.conf`(첫 network 블록 ssid·psk만) / `wifi_init_conf.json`(profile b).
- 무선(mlan0) 경유 SSH에서 apply 시 자기절단 가드 — 유선 접속 또는 `--force`.

## 테스트

`PROVISION_ROOT=<fakeroot>`로 파일 루트를 옮기면 타겟 가드·라이브 명령을 건너뛰고 파일 변환만
수행한다. 스모크 테스트(2026-09-02, 18항목 전부 통과): plan 무변경성 / 네거티브 인자 거부 /
apply 단언(첫 wpa 블록만·무관 JSON 키 보존) / 멱등성·무변경 시 백업 미생성 / verify 양성·음성
(훼손 시 FAIL) / rollback 초기 원본 복원 / --gw 미지정 시 기존 Gateway 보존.

**실기 리허설 완료(2026-09-02, cts-wlan)**: plan→apply→재부팅→verify 전체 PASS→rollback→재부팅→원상 복원까지 전 사이클 통과(무흔적 종료). 리허설이 잡은 결함 2건(BASH_REMATCH 덮어쓰기로 192.168.x 오거부 / BusyBox tar 비호환 — fail-safe는 정상 동작) 수정 반영됨. 상세: wlan-opc#88 코멘트. **wpa 자격증명 경로도 2차 리허설로 검증 완료**(첫 블록만 교체·extra_ssid 블록 보존·600 권한 보존·rollback 복원 — #88 2차 코멘트).

### verify 운영 주의 (2차 리허설 발견)
- networkd가 carrier 대기(`ConfigureWithoutCarrier` 미설정)라 **무선 미접속 동안 static IP/route도 미부여** — verify는 무선 안정화(wpa_state=COMPLETED) 후 실행. wifi 다운이면 IP/route/GW가 연쇄 FAIL하므로 진단은 wpa_state부터.
- 잘못된 SSID 적용 시 다른 network 블록이 있어도 접속이 수렴하지 않을 수 있음(실기 관찰) — 자격증명 오적용의 안전망은 rollback뿐.
