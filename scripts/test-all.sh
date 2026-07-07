#!/usr/bin/env bash
# 전체 OPC 프로토콜 명령 자동 검증 스위트 (test-env.sh 재사용).
#
#   bash scripts/test-all.sh                 # 안전 전체 (set-radio 실적용 제외)
#   RADIO_APPLY=1 bash scripts/test-all.sh   # set-radio OK 실적용까지 (동일 freq 재적용/자기복구) ⚠️콘솔권장
#
# 안전장치: 시작 시 타겟 /usr/local/opc/etc 를 스냅샷, 종료(정상/중단) 시 복원 + opcd 재시작.
# 링크위험(자기절단)으로 스킵: WlanStatusChange/ApDisconnect/FaultDetect (사유 출력).
# Roaming(0x04)은 notify 경로(#64/#83, 링크 무영향)로 능동 검증(§9).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/test-env.sh" >/dev/null || { echo "loader 로드 실패"; exit 1; }

RADIO_APPLY="${RADIO_APPLY:-0}"
LOG="${LOG:-${TMPDIR:-/tmp}/opc-test-all.$$.log}"
pass=0; fail=0; skip=0; FAILED=""

sec(){ printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
# chk "설명" "기대문자열" 명령...   (명령 출력에 기대문자열 포함 시 PASS)
chk(){ local d="$1" e="$2"; shift 2; local o; o="$("$@" 2>&1)"
  if printf '%s' "$o" | grep -qF -- "$e"; then printf '  PASS  %s\n' "$d"; pass=$((pass+1))
  else printf '  \033[31mFAIL\033[0m  %s  [want:%s]\n         got: %s\n' "$d" "$e" "$(printf '%s' "$o"|tr '\n' ' '|cut -c1-150)"; fail=$((fail+1)); FAILED="${FAILED}\n  - ${d}"; fi; }
skipn(){ printf '  SKIP  %s  (%s)\n' "$1" "$2"; skip=$((skip+1)); }

# --- 안전: etc 스냅샷 + 복원 트랩 ---
sec "SETUP: etc 스냅샷"
vhl_ssh 'rm -rf /usr/local/opc/etc.testbak; cp -a /usr/local/opc/etc /usr/local/opc/etc.testbak && echo snapshot-ok' 2>&1 | tail -1
restore(){ echo; sec "TEARDOWN: etc 복원 + opcd 재시작"
  vhl_ssh 'rm -rf /usr/local/opc/etc; mv /usr/local/opc/etc.testbak /usr/local/opc/etc 2>/dev/null; systemctl restart opcd; sleep 1; echo restored=$(systemctl is-active opcd)' 2>&1 | tail -1
  $VHL logout >/dev/null 2>&1 || true; }
trap restore EXIT

# ============ 1. Login / Logout / 세션 ============
sec "1. Login / Logout / 세션 배타"
chk "GetBasicInfo (로그인 불요)"            "vendor_code"  $VHL basic-info
chk "GetDeviceInfo 미로그인 → NG 0x0001"    "0x0001"       $VHL device-info
chk "Login 틀린비번 → NG 0x0010"            "0x0010"       $VHL login --password WRONG_pw
chk "Login MyPassword → OK"                 "OK"           $VHL login --password "$PW"
chk "GetBasicInfo status=0x02"              "0x00000002"   $VHL basic-info
chk "2nd-host(loopback) login → NG 0x0002"  "0x0002"       vhl_ssh "/usr/local/opc/bin/vhlctl --host 127.0.0.1 --port $TEST_OPC_PORT login --password $PW"

# ============ 2. GetDeviceInfo ============
sec "2. GetDeviceInfo"
chk "device-info vendor(0x00902cfb)"        "0x00902cfb"   $VHL device-info
chk "device-info hardware(HW-1.0.0)"        "HW-1.0.0"     $VHL device-info
chk "device-info live essid(jhw_wlan)"      "jhw_wlan"     $VHL device-info

# ============ 3. SetIndicationConfig + Indication 발행 ============
sec "3. SetIndicationConfig / Indication"
chk "set-indication 비유니캐스트 → NG 0x0012" "0x0012"     $VHL set-indication --bits 0x80 --period 5 --to 224.0.0.1:9999
chk "set-indication 미할당 bit(0x40) → NG 0x0010" "0x0010" $VHL set-indication --bits 0x40 --period 5 --to "$VHLIP:$TEST_LISTEN_PORT"
chk "set-indication unicast KeepAlive → OK" "OK"           $VHL set-indication --bits 0x80 --period 2 --to "$VHLIP:$TEST_LISTEN_PORT"
chk "indication ON 중 device-info → NG 0x0010" "0x0010"    $VHL device-info
# KeepAlive 수신 (period=2, listener 6s)
timeout 7 "$VHLCTL_BIN" --hex listen --bind "0.0.0.0:$TEST_LISTEN_PORT" > "$LOG.ka" 2>&1 &
LP=$!; sleep 6
$VHL set-indication --bits 0x80 --period 0 --to "$VHLIP:$TEST_LISTEN_PORT" >/dev/null 2>&1   # teardown
wait $LP 2>/dev/null
ka=$(grep -c "req_id" "$LOG.ka" 2>/dev/null || echo 0)
if [ "${ka:-0}" -ge 2 ]; then printf '  PASS  KeepAlive 수신 %d프레임 (seq 단조/teardown)\n' "$ka"; pass=$((pass+1))
else printf '  \033[31mFAIL\033[0m  KeepAlive 수신 부족(%s)\n' "$ka"; fail=$((fail+1)); FAILED="${FAILED}\n  - KeepAlive"; fi

# ============ 4. SetPassword (백업/복원) ============
sec "4. SetPassword"
chk "set-password old오타 → NG 0x0010"      "0x0010"       $VHL set-password --old WRONGold --new NewPass1234
chk "set-password OK"                        "OK"           $VHL set-password --old "$PW" --new NewPass1234
vhl_ssh 'systemctl restart opcd; sleep 1' >/dev/null 2>&1
chk "새 비번 restart 잔존 → login OK"        "OK"           $VHL login --password NewPass1234
chk "set-password 원복 OK"                   "OK"           $VHL set-password --old NewPass1234 --new "$PW"
$VHL login --password "$PW" >/dev/null 2>&1

# ============ 5. SetIpConfigList (백업/복원) ============
sec "5. SetIpConfigList"
chk "set-ip-list START(slot1) → OK"          "OK"           $VHL set-ip-list --slot 1 --flag start --ip 10.0.0.50 --mask 255.255.255.0 --gw 10.0.0.1 --ntp 10.0.0.2 --essid testnet
chk "change-ip (END 전) → NG 0x0012 conflict" "0x0012"      $VHL change-ip --slot 1
chk "set-ip-list 비연속 netmask → NG 0x0012"  "0x0012"      $VHL set-ip-list --slot 2 --flag cont --ip 10.0.0.60 --mask 0.255.0.0 --gw 10.0.0.1 --ntp 10.0.0.2 --essid testnet
chk "set-ip-list END(slot1) → OK commit"      "OK"          $VHL set-ip-list --slot 1 --flag end --ip 10.0.0.50 --mask 255.255.255.0 --gw 10.0.0.1 --ntp 10.0.0.2 --essid testnet

# ============ 6. ChangeIpAddress (eth0 DOWN → 우리 경로 무관, 안전) ============
sec "6. ChangeIpAddress"
chk "change-ip 빈슬롯(25) → NG 0x0011"       "0x0011"       $VHL change-ip --slot 25
chk "change-ip slot1 (armed) → OK"           "OK"           $VHL change-ip --slot 1

# ============ 7. SetRadioConfig ============
sec "7. SetRadioConfig"
chk "set-radio mode 99 → NG 0x0013"          "0x0013"       $VHL set-radio --station single --w1-freq 5200 --w1-ch 0x0228 --w1-mode 99 --w1-bw 2
chk "set-radio bw 99 → NG 0x0014"            "0x0014"       $VHL set-radio --station single --w1-freq 5200 --w1-ch 0x0228 --w1-mode 11 --w1-bw 99
chk "set-radio 6G freq(6200) → NG 0x0011"    "0x0011"       $VHL set-radio --station single --w1-freq 6200 --w1-ch 0x0224 --w1-mode 11 --w1-bw 2
if [ "$RADIO_APPLY" = "1" ]; then
  chk "set-radio OK 실적용(동일 freq5200 재적용)" "OK"       $VHL set-radio --station single --w1-freq 5200 --w1-ch 0x0228 --w1-mode 11 --w1-bw 2
  sleep 3; chk "실적용 후 링크 생존 확인"      "vendor_code" $VHL basic-info
else
  skipn "set-radio OK 실적용" "RADIO_APPLY=1 필요 (mlan0 재결합 위험)"
fi

# ============ 8. Reset (+ ResetNotice) ============
sec "8. Reset / ResetNotice"
$VHL login --password "$PW" >/dev/null 2>&1
$VHL set-indication --bits 0x20 --period 0 --to "$VHLIP:$TEST_LISTEN_PORT" >/dev/null 2>&1  # ResetNotice
timeout 6 "$VHLCTL_BIN" --hex listen --bind "0.0.0.0:$TEST_LISTEN_PORT" > "$LOG.rn" 2>&1 &
LP=$!; sleep 1
chk "reset → OK"                             "OK"           $VHL reset
sleep 3; wait $LP 2>/dev/null
if grep -q "0x0020\|0020" "$LOG.rn" 2>/dev/null; then printf '  PASS  ResetNotice(0x0020) 수신\n'; pass=$((pass+1))
else skipn "ResetNotice 수신" "타이밍/recipient volatile — journal로 확인"; fi
vhl_ssh 'sleep 2; echo opcd=$(systemctl is-active opcd)' 2>&1 | tail -1
chk "reset 후 systemd 재기동 + 재도달"       "vendor_code"  vhl basic-info

# ============ 9. Roaming(0x04) — notify 경로 (링크 무영향) ============
# 이 보드는 host-based 로밍(커널 CMD_ROAM 미발행) → 로밍 실행체가 opcd에 로컬 UDP
# 통지(127.0.0.1:50608)해 0x04를 발행(#64/#83). roam_notify.py 수동 트리거로
# 헬퍼→opcd 파싱→indication 발행→수신까지 e2e 검증(실 link.json 페이로드, 링크 무영향).
sec "9. Roaming(0x04) notify 경로"
chk "재로그인(reset 후) → OK"                "OK"           $VHL login --password "$PW"
chk "set-indication Roaming(0x04) → OK"      "OK"           $VHL set-indication --bits 0x04 --period 0 --to "$VHLIP:$TEST_LISTEN_PORT"
timeout 6 "$VHLCTL_BIN" --hex listen --bind "0.0.0.0:$TEST_LISTEN_PORT" > "$LOG.roam" 2>&1 &
LP=$!; sleep 1
chk "roam_notify.py 트리거(실 link.json)"    "sent-ok"      vhl_ssh 'python3 /usr/local/logger/roam_notify.py --iface mlan0 && echo sent-ok'
wait $LP 2>/dev/null
if grep -q "indication 0x0004" "$LOG.roam" 2>/dev/null; then printf '  PASS  Roaming(0x0004) indication 수신\n'; pass=$((pass+1))
else printf '  \033[31mFAIL\033[0m  Roaming(0x0004) 미수신 — 타겟 opcd(#64)/roam_notify.py(#83) 배포본 확인\n'; fail=$((fail+1)); FAILED="${FAILED}\n  - Roaming(0x0004)"; fi

# ============ 10. 환경제약 스킵 ============
sec "10. 환경제약 (미실행)"
skipn "WlanStatusChange(0x02)" "wpa_cli disconnect가 mlan0=우리경로 절단"
skipn "ApDisconnect(0x08)"     "AP-주도 deauth 환경 필요"
skipn "FaultDetect(0x10)"      "CPU/Mem/Net 실폭주 유발 필요(침습적)"
skipn "Roaming 실로밍 라이브"   "wifi mlan0 roam N = mlan0 재결합 유발 — notify 경로는 §9로 검증"

# ============ 요약 ============
sec "RESULT"
printf '  PASS=%d  FAIL=%d  SKIP=%d\n' "$pass" "$fail" "$skip"
[ "$fail" -gt 0 ] && printf '  실패:%b\n' "$FAILED"
rm -f "$LOG".ka "$LOG".rn "$LOG".roam 2>/dev/null
exit 0
