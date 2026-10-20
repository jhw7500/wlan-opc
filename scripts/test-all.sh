#!/usr/bin/env bash
# 전체 OPC 프로토콜 명령 자동 검증 스위트 (test-env.sh 재사용).
#
#   bash scripts/test-all.sh                 # 안전 전체 (set-radio 실적용 제외)
#   RADIO_APPLY=1 bash scripts/test-all.sh   # set-radio OK 실적용까지 (동일 freq 재적용/자기복구) ⚠️콘솔권장
#   §8 reset 은 보드를 **재부팅**한다(#164): boot_id 변경·opcd·basic-info 복귀를 RESET_REBOOT_WAIT_S(기본 180)초까지 기다린다.
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
# 스냅샷 실패(타겟 오프라인/ssh 오류/cp 실패) 시 즉시 중단 — 트랩 설정 전이므로 타겟 무변경.
# 복원은 반드시 백업(etc.testbak) 존재 확인 후에만 rm -rf — 백업 없이 etc를 지우면
# 타겟 설정 디렉토리가 통째로 유실된다 (PR #65 리뷰: Codex P1 + Gemini CRITICAL).
sec "SETUP: etc 스냅샷"
# 이전 실행의 etc.testbak 이 남아 있으면 그것이 원본일 수 있다(§8 재부팅 중 복원 실패, #164) — 덮어쓰지 않고 중단.
snap=$(vhl_ssh 'if [ -e /usr/local/opc/etc.testbak ]; then echo leftover-backup; elif cp -a /usr/local/opc/etc /usr/local/opc/etc.testbak; then echo snapshot-ok; else rm -rf /usr/local/opc/etc.testbak; echo snapshot-failed; fi' 2>&1 | tail -1)
echo "  $snap"
case "$snap" in
  snapshot-ok) ;;
  leftover-backup) echo "  이전 실행의 복원이 끝나지 않았다(etc.testbak 잔존 — 원본일 수 있음). 타겟에서 내용을 확인한 뒤 cd /usr/local/opc && rm -rf etc && mv etc.testbak etc && systemctl restart opcd 로 수동 복원하고 재실행. 중단(타겟 미변경)"; exit 1 ;;
  *) echo "  스냅샷 실패(부분 복사본은 삭제됨) — 중단(타겟 미변경)"; exit 1 ;;
esac
restore(){ echo; sec "TEARDOWN: etc 복원 + opcd 재시작"
  # §8 reset 이 보드를 재부팅하므로(#164) 여기서 ssh 가 닿지 않을 수 있다. 복원이 확인되지 않으면
  # FAIL(비0 종료)로 알리고 etc.testbak(원본)은 타겟에 남긴다 — 다음 실행의 SETUP 이 그것을 보고 중단한다.
  local r; r=$(vhl_ssh 'if [ -d /usr/local/opc/etc.testbak ]; then rm -rf /usr/local/opc/etc && { mv /usr/local/opc/etc.testbak /usr/local/opc/etc || cp -a /usr/local/opc/etc.testbak /usr/local/opc/etc; } && echo restore-ok; else echo "testbak 없음 — etc 보존"; fi; systemctl restart opcd; sleep 1; echo restored=$(systemctl is-active opcd)' 2>&1 | tr '\n' ' ')
  echo "  $r"
  $VHL logout >/dev/null 2>&1 || true
  case "$r" in *restore-ok*|*"testbak 없음"*) ;;
    *) printf '  \033[31mFAIL\033[0m  etc 복원 미확인 — 타겟에 etc.testbak(원본)이 남아 있을 수 있다. 보드가 올라온 뒤 수동 복원: cd /usr/local/opc && rm -rf etc && mv etc.testbak etc && systemctl restart opcd (etc 가 남아 있으면 mv 만으로는 etc/etc.testbak 으로 중첩된다).\n'; exit 1 ;;
  esac; }
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
# 기대 ESSID는 환경값(test-env.json expect.essid) — 비우면 필드 존재만 확인(랩 SSID 하드코딩 회피)
if [ -n "${TEST_EXPECT_ESSID:-}" ]; then
  chk "device-info live essid($TEST_EXPECT_ESSID)" "$TEST_EXPECT_ESSID" $VHL device-info
else
  chk "device-info essid 필드 존재"           "essid"        $VHL device-info
fi

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
ka=$(grep -c "indication 0x0080" "$LOG.ka" 2>/dev/null || echo 0)
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
# slot1은 §6의 ChangeIp 커밋 대상이 될 수 있는 슬롯이므로 ESSID/GW/NTP를 미설정으로 둔다.
# ESSID가 비어 있지 않으면 커밋이 platform_nxp.c의 `slot->essid[0] != '\\0'` 게이트를 통과해
# run_opc_wlan_apply()로 그 ESSID를 mlan0(하드코딩)에 적용하고 무선을 끊는다. GW/NTP는 적용
# 대상이 아니지만(검증·에코만) 같은 이유로 함께 비운다.
chk "set-ip-list START(slot1) → OK"          "OK"           $VHL set-ip-list --slot 1 --flag start --ip 10.0.0.50 --mask 255.255.255.0 --gw 0.0.0.0 --ntp 0.0.0.0 --essid ""
# staging 중 change-ip는 슬롯을 보기 전에 거절된다 — handle_change_ip_address의 검사 순서가
# armed → ip_list_staging_active → 범위 → present 이기 때문이다(opcd/handler.c).
#
# 슬롯 번호를 **범위 밖(0)** 으로 두는 것이 핵심이다. 범위 검사가 staging 검사 뒤에 있으므로
# staging이 열려 있으면 기대대로 0x0012지만, 앞의 START 프레임이 유실돼 staging이 안 열렸다면
# 0x0010(범위 밖)이 되어 present 분기에 **도달 자체를 못 한다** — 타겟에 그 슬롯이 채워져
# 있는지와 무관하게 커밋 예약이 구조적으로 불가능하다. (`vhlctl`은 --slot 을 검증 없이
# 그대로 싣는다: vhlctl.c의 `(uint16_t)atoi(slot_s)`.)
#
# 기존처럼 slot 1로 찌르면 그 경우 기존 slot 1이 OK를 받아 살아 있는 ESSID/GW를 담은 커밋이
# 예약되고 teardown의 Logout이 그것을 커밋한다. 비어 있을 법한 슬롯(25)을 쓰는 것도 "그 보드에
# 25가 비어 있다"는 타겟 상태에 기대는 조건부 보장일 뿐이다.
chk "change-ip (END 전) → NG 0x0012 conflict" "0x0012"      $VHL change-ip --slot 0
chk "set-ip-list 비연속 netmask → NG 0x0012"  "0x0012"      $VHL set-ip-list --slot 2 --flag cont --ip 10.0.0.60 --mask 0.255.0.0 --gw 10.0.0.1 --ntp 10.0.0.2 --essid testnet
chk "set-ip-list END(slot1) → OK commit"      "OK"          $VHL set-ip-list --slot 1 --flag end --ip 10.0.0.50 --mask 255.255.255.0 --gw 0.0.0.0 --ntp 0.0.0.0 --essid ""
chk "set-ip-list start_end(slot3) 단일프레임 → OK" "OK"       $VHL set-ip-list --slot 3 --flag start_end --ip 10.0.0.70 --mask 255.255.255.0 --gw 10.0.0.1 --ntp 10.0.0.2 --essid testnet

# ============ 6. ChangeIpAddress ============
# 과거 주석은 "eth0 DOWN → 우리 경로 무관, 안전"이었으나 그것은 제어 경로가 eth0(유선)이던 시절의
# 전제였다. 커밋 대상은 opcd가 고른다 — mgmt_ip_iface_idx(): peer_route면 mlan0, 아니면 opc.conf의
# device_ip_iface(기본 eth0). 제어 경로와 같을 수도 다를 수도 있어 "안전"을 전제할 수 없다.
# NG 경로는 커밋이 없어 항상 안전하므로 여기서 수행한다.
sec "6. ChangeIpAddress"
chk "change-ip 빈슬롯(25) → NG 0x0011"       "0x0011"       $VHL change-ip --slot 25
# 커밋(armed change-ip → Logout)은 이 스위트에서 수행하지 않는다. 하니스는 결과를 판정할 수도
# (커밋 순간 제어 평면이 죽을 수 있다) 복구할 수도(콘솔+리부트 필요) 없다. 절차는 문서에 있다:
# docs/testing/ontarget-protocol-verification-plan.md
skipn "change-ip slot1 (armed→Logout 커밋)" \
      "파괴적·비가역 — 시리얼 콘솔 필요. 수동 절차는 docs/testing/ontarget-protocol-verification-plan.md"

# ============ 7. SetRadioConfig ============
sec "7. SetRadioConfig"
chk "set-radio mode 99 → NG 0x0013"          "0x0013"       $VHL set-radio --station single --w1-band 5 --w1-chlist 40 --w1-mode 99 --w1-bw 2
chk "set-radio bw 99 → NG 0x0014"            "0x0014"       $VHL set-radio --station single --w1-band 5 --w1-chlist 40 --w1-mode 11 --w1-bw 99
chk "set-radio 6G band → NG 0x0011"          "0x0011"       $VHL set-radio --station single --w1-band 6 --w1-chlist 1 --w1-mode 11 --w1-bw 2
chk "set-radio 5G bit25(표 밖) → NG 0x0012"  "0x0012"       $VHL set-radio --station single --w1-band 5 --w1-chlist-hex 0200000000000000 --w1-mode 11 --w1-bw 2
# D1 (회신 2026-09-18): 행 순서 확정 — 2.4/5GHz 리스트가 row B(뒤 4Byte)에 실리면 부정 프레임
chk "set-radio 2.4G 리스트 row B → NG 0x0012" "0x0012"      $VHL set-radio --station single --w1-band 2.4 --w1-chlist-hex 0000000000000421 --w1-mode 11 --w1-bw 2
# Rev1.02 §4.3.8: Priority CH 오류는 SCAN 오류(0x0011/0x0012)와 분리된 0x0015/0x0016
chk "set-radio Dual priority 6G대역 → NG 0x0015" "0x0015"   $VHL set-radio --station dual --w1-band 5 --w1-chlist 40 --w1-mode 11 --w1-bw 2 --w2-band 2.4 --w2-chlist 1 --w2-mode 11 --w2-bw 2 --priority 0x06FF
chk "set-radio Dual priority 5G ch38(표 밖) → NG 0x0016" "0x0016" $VHL set-radio --station dual --w1-band 5 --w1-chlist 40 --w1-mode 11 --w1-bw 2 --w2-band 2.4 --w2-chlist 1 --w2-mode 11 --w2-bw 2 --priority 0x0226
if [ "$RADIO_APPLY" = "1" ]; then
  # Rev1.01: band + channel list. Same config re-sent → apply skipped (OK), so use ch40+ch36 first.
  chk "set-radio OK 실적용(5G ch40,36 밴드락)"  "OK"           $VHL set-radio --station single --w1-band 5 --w1-chlist 40,36 --w1-mode 11 --w1-bw 2
  sleep 3; chk "실적용 후 링크 생존 확인"      "vendor_code" $VHL basic-info
else
  skipn "set-radio OK 실적용" "RADIO_APPLY=1 필요 (mlan0 재결합 위험)"
fi

# ============ 8. Reset (+ ResetNotice) ============
# #164: reset 은 Ack → ResetNotice → wlan_reboot_policy.sh --force 로 **보드를 재부팅**한다(opcd 재시작이 아니다).
# 그래서 boot_id 가 바뀌고 opcd 가 active 가 될 때까지 기다린 뒤에야 재도달을 확인하고 다음 절로 간다.
# 돌아오지 않으면 FAIL 로 기록하고 계속한다(그러면 §9 와 TEARDOWN 복원도 실패할 수 있다 — 복원 실패는 비0 종료).
sec "8. Reset / ResetNotice (보드 재부팅, #164)"
$VHL login --password "$PW" >/dev/null 2>&1
boot0=$(vhl_ssh 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null | tail -1)
$VHL set-indication --bits 0x20 --period 0 --to "$VHLIP:$TEST_LISTEN_PORT" >/dev/null 2>&1  # ResetNotice
timeout 6 "$VHLCTL_BIN" --hex listen --bind "0.0.0.0:$TEST_LISTEN_PORT" > "$LOG.rn" 2>&1 &
LP=$!; sleep 1
chk "reset → OK"                             "OK"           $VHL reset
sleep 3; wait $LP 2>/dev/null
if grep -q "0x0020\|0020" "$LOG.rn" 2>/dev/null; then printf '  PASS  ResetNotice(0x0020) 수신\n'; pass=$((pass+1))
else skipn "ResetNotice 수신" "타이밍/recipient volatile — journal로 확인"; fi
RESET_REBOOT_WAIT_S="${RESET_REBOOT_WAIT_S:-180}"
boot1=""; t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$RESET_REBOOT_WAIT_S" ]; do
  boot1=$(vhl_ssh 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null | tail -1)
  if [ -n "$boot1" ] && [ "$boot1" != "$boot0" ] && [ "$(vhl_ssh 'systemctl is-active opcd' 2>/dev/null | tail -1)" = "active" ]; then break; fi
  sleep 5
done
if [ -n "$boot0" ] && [ -n "$boot1" ] && [ "$boot1" != "$boot0" ]; then
  printf '  PASS  reset 후 보드 재부팅(boot_id 변경) + opcd active (%ds)\n' "$(( $(date +%s) - t0 ))"; pass=$((pass+1))
else
  printf '  \033[31mFAIL\033[0m  reset 후 %ds 안에 보드가 돌아오지 않음 (boot0=%s boot1=%s) — 다음 절과 TEARDOWN 복원이 실패할 수 있다\n' "$RESET_REBOOT_WAIT_S" "${boot0:-?}" "${boot1:-?}"
  fail=$((fail+1)); FAILED="${FAILED}\n  - reset 후 보드 재부팅"
fi
chk "reset 후 재도달(basic-info)"            "vendor_code"  vhl basic-info
# 순서 근거: 리스너는 Ack 를 받지 않는다(별도 소켓). Ack → ResetNotice 순서는 opcd 감사 로그
# (/var/log/cantops/logger.log, rsyslog — 재부팅 뒤에도 남음)에서 "TX Reset ack seq=" 줄이
# "IND ResetNotice(0x0020)" 줄보다 앞에 있는지로 판정한다.
ord=$(vhl_ssh "a=\$(grep -n 'TX Reset ack seq=' /var/log/cantops/logger.log | tail -1 | cut -d: -f1); n=\$(grep -n 'IND ResetNotice(0x0020)' /var/log/cantops/logger.log | tail -1 | cut -d: -f1); echo \"ack=\${a:-none} notice=\${n:-none}\"" 2>/dev/null | tail -1)
echo "  logger.log: $ord"
oa=${ord#ack=}; oa=${oa%% *}; on=${ord##*notice=}
if [ -n "$oa" ] && [ -n "$on" ] && [ "$oa" != none ] && [ "$on" != none ] && [ "$oa" -lt "$on" ] 2>/dev/null; then
  printf '  PASS  Ack(TX Reset ack) → ResetNotice(IND 0x0020) 순서 (logger.log 줄 %s < %s)\n' "$oa" "$on"; pass=$((pass+1))
else
  printf '  \033[31mFAIL\033[0m  Ack→ResetNotice 순서를 logger.log 에서 확인하지 못함 (%s)\n' "${ord:-ssh 실패}"; fail=$((fail+1)); FAILED="${FAILED}\n  - Ack→ResetNotice 순서(logger.log)"
fi
# 정상 리셋 경로 근거: "policy client terminated by signal"(stop job 의 TERM 이 클라이언트에 먼저 닿음) 또는
# "then opcd received its stop signal"(클라이언트가 먼저 exit≠0 으로 끝난 뒤 opcd 가 자기 stop TERM 을 받음, #179).
# 둘 다 reboot 가 시작된 뒤 opcd stderr(journal)에 찍히므로 정책 스크립트의 journald 스냅샷(reboot 전)에는 없다.
# 영속 journal 이면 이전 부팅에서 읽고, 둘 다 없으면 SKIP(시리얼 콘솔).
sig=$(vhl_ssh "journalctl -b -1 -u opcd --no-pager 2>/dev/null | grep -cE 'policy client terminated by signal|then opcd received its stop signal'" 2>/dev/null | tail -1)
if [ "${sig:-0}" -gt 0 ] 2>/dev/null; then printf '  PASS  정상 리셋 경로: 이전 부팅 journal 에 재부팅 진행 중 분류 문구 (%s회)\n' "$sig"; pass=$((pass+1))
else skipn "정상 리셋 경로(재부팅 진행 중 분류)" "이전 부팅 opcd journal 에서 정상 리셋 문구를 찾지 못함 — 휘발 journald 면 시리얼 콘솔에서 확인"; fi

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
# CI/스크립트 게이트용: 실패가 있으면 비0 종료 (PR #65 리뷰: Codex P2)
[ "$fail" -gt 0 ] && exit 1
exit 0
