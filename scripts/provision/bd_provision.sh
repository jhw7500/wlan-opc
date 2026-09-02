#!/bin/bash
# bd_provision.sh — 무선기판(BD) 현장 프로비저닝 번들 (1회성·운영자 명시 실행 전용)
#
# DFK 배정값(관리 IP/GW/NTP/무선 자격증명)을 기기 1대에 원자적으로 적용한다.
# 근거: docs/dfk-meeting/dfk-consolidated-questions-20260708.md §4 "프로비저닝 번들",
#       meeting-2026-06-26-dfk-network-ip-design.md §12 (opcd로 wlan0 IP/GW/NTP 설정 불가,
#       set-ntp 래치, wbridge 부팅 1회 IP 수집 → 재부팅이 표준 반영 경로).
#
# ★ 설계 원칙 (2026-09-02 사용자 지시):
#   - 부팅 시 자동 감지/자동 교정 로직 없음. systemd 유닛·부팅 훅·디스패처 미설치.
#   - 이 스크립트는 운영자가 명시적으로 실행할 때만 파일을 쓰고 종료한다.
#   - verify는 읽기 전용 — 불일치를 보고만 하고 절대 수정하지 않는다.
#
# 사용법:
#   bd_provision.sh plan   --ip A.B.C.D/NN [--gw A.B.C.D] [--ntp A.B.C.D] [--ssid S --psk P] [--profile a|b]
#   bd_provision.sh apply  (plan과 동일 인자) [--reboot] [--force]
#   bd_provision.sh verify (plan과 동일 인자)
#   bd_provision.sh rollback [--backup FILE.tar]
#
#   plan     : 변경 예정 diff 미리보기 (시스템 무변경)
#   apply    : 백업 → 파일 반영(원자적 mv) → set-ntp 재활성화 → 재부팅 안내(--reboot 시 실행)
#   verify   : 재부팅 후 기대 상태 점검 (읽기 전용, FAIL 있으면 exit 1)
#   rollback : 최신(또는 지정) 백업 tar 복원 → 재부팅 안내
#
# 옵션:
#   --ip A.B.C.D/NN   mlan0 관리 IP(CIDR)                      [plan/apply/verify 필수]
#   --gw A.B.C.D      mlan0 default gateway (미지정 시 기존 Gateway= 불변)
#   --ntp A.B.C.D     NTP 서버 (미지정 시 timesyncd.conf 불변, set-ntp도 건드리지 않음)
#   --ssid NAME       무선 SSID (hex로 기록; --psk 필수 동반)
#   --psk PASSPHRASE  WPA2-PSK 평문 8~63자 (따옴표 평문으로 기록)
#   --profile a|b     b = Option B 3종 토글(peer_route=on, ip_discovery=on,
#                     arp_ignore_always=off) 적용. a(기본) = 토글 불변·현재값 보고만.
#   --reboot          apply 성공 후 실제 재부팅 (기본: 안내만)
#   --force           mlan0(무선) 경유 SSH 세션에서도 apply 진행 (자기절단 경고 무시)
#   --backup-dir DIR  백업 위치 (기본 /var/backups/bd-provision)
#   --backup FILE     rollback 대상 tar (기본: 최신)
#
# 테스트: PROVISION_ROOT=<fakeroot> 로 파일 경로 루트를 옮기면 타겟 가드·라이브 명령
#         (timedatectl/reboot/verify 라이브 점검)을 건너뛰고 파일 변환만 수행한다.

set -euo pipefail

ROOT="${PROVISION_ROOT:-}"
NET_FILE="$ROOT/etc/systemd/network/20-mlan0.network"
TS_FILE="$ROOT/etc/systemd/timesyncd.conf"
WPA_FILE="$ROOT/etc/wpa_supplicant/wpa_supplicant-mlan0.conf"
CONF_CANDIDATES=("$ROOT/opt/wlan/config/wifi_init_conf.json" "$ROOT/etc/wlan/wifi_init_conf.json")
BACKUP_DIR_DEFAULT="$ROOT/var/backups/bd-provision"

CMD="${1:-}"; shift || true
IPCIDR="" GW="" NTP="" SSID="" PSK="" PROFILE="a" DO_REBOOT=0 FORCE=0
BACKUP_DIR="$BACKUP_DIR_DEFAULT" BACKUP_FILE=""

die() { echo "ERROR: $*" >&2; exit 2; }
info() { echo "[bd-provision] $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --ip)         IPCIDR="${2:-}"; shift 2 ;;
        --gw)         GW="${2:-}"; shift 2 ;;
        --ntp)        NTP="${2:-}"; shift 2 ;;
        --ssid)       SSID="${2:-}"; shift 2 ;;
        --psk)        PSK="${2:-}"; shift 2 ;;
        --profile)    PROFILE="${2:-}"; shift 2 ;;
        --reboot)     DO_REBOOT=1; shift ;;
        --force)      FORCE=1; shift ;;
        --backup-dir) BACKUP_DIR="${2:-}"; shift 2 ;;
        --backup)     BACKUP_FILE="${2:-}"; shift 2 ;;
        *) die "알 수 없는 옵션: $1" ;;
    esac
done

is_ipv4() {
    local ip="$1" o
    [[ "$ip" =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]] || return 1
    for o in "${BASH_REMATCH[@]:1}"; do [ "$o" -le 255 ] || return 1; done
    return 0
}

validate_args() {
    [ -n "$IPCIDR" ] || die "--ip A.B.C.D/NN 필수"
    [[ "$IPCIDR" =~ ^([0-9.]+)/([0-9]{1,2})$ ]] || die "--ip 형식 오류: $IPCIDR (A.B.C.D/NN)"
    # is_ipv4가 BASH_REMATCH를 덮어쓰므로 먼저 로컬 변수로 회수 (192.168.x/NN 오탐 버그 수정, 2026-09-02 실기 리허설 발견)
    local ip_part="${BASH_REMATCH[1]}" ip_prefix="${BASH_REMATCH[2]}"
    is_ipv4 "$ip_part" || die "--ip 주소 오류: $ip_part"
    [ "$ip_prefix" -ge 1 ] && [ "$ip_prefix" -le 32 ] || die "--ip prefix 오류: /$ip_prefix"
    [ -z "$GW" ] || is_ipv4 "$GW" || die "--gw 주소 오류: $GW"
    [ -z "$NTP" ] || is_ipv4 "$NTP" || die "--ntp 주소 오류: $NTP"
    if [ -n "$SSID" ] || [ -n "$PSK" ]; then
        # verify는 파일의 ssid만 대조하므로 psk 없이 --ssid 단독 허용 (비밀값 재입력 불요)
        if [ "$CMD" != "verify" ]; then
            [ -n "$SSID" ] && [ -n "$PSK" ] || die "--ssid 와 --psk 는 함께 지정해야 함"
        fi
        if [ -n "$PSK" ]; then
            local n=${#PSK}
            [ "$n" -ge 8 ] && [ "$n" -le 63 ] || die "--psk 길이 오류(8~63자): ${n}자"
        fi
        [ -z "$SSID" ] || [ "${#SSID}" -le 32 ] || die "--ssid 32바이트 초과"
    fi
    case "$PROFILE" in a|b) ;; *) die "--profile 은 a 또는 b" ;; esac
}

find_conf() {
    local f
    for f in "${CONF_CANDIDATES[@]}"; do [ -f "$f" ] && { echo "$f"; return 0; }; done
    return 1
}

guard_target() {
    [ -n "$ROOT" ] && return 0   # 테스트 모드: 가드 생략
    [ -f "$NET_FILE" ] || die "타겟이 아님: $NET_FILE 없음 (테스트는 PROVISION_ROOT 사용)"
    ip link show mlan0 >/dev/null 2>&1 || die "타겟이 아님: mlan0 인터페이스 없음"
}

guard_selfcut() {
    # 무선(mlan0) 경유 SSH 세션에서 apply 하면 자기절단 위험 — 명시적 --force 요구
    [ -n "$ROOT" ] && return 0
    [ "$FORCE" -eq 1 ] && return 0
    local mlan_ip ssh_dst
    mlan_ip="$(ip -4 -o addr show mlan0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
    if [ -n "${SSH_CONNECTION:-}" ]; then
        ssh_dst="$(echo "$SSH_CONNECTION" | awk '{print $3}')"
        if [ -n "$mlan_ip" ] && [ "$ssh_dst" = "$mlan_ip" ]; then
            die "현재 SSH가 mlan0($mlan_ip) 경유 — 적용 중 접속이 끊길 수 있음. 유선(eth0)으로 접속하거나 --force 지정"
        fi
    fi
}

ssid_to_hex() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }

# ---------- 파일 변환 (stdin → stdout 아닌, src → dst 생성) ----------

gen_network() {  # $1=src $2=dst
    awk -v ip="$IPCIDR" -v gw="$GW" '
        BEGIN { in_net=0; addr_done=0; gw_done=0; have_gw=(gw!="") }
        /^\[Network\]/ { in_net=1; print; next }
        /^\[/ {
            if (in_net) {
                if (!addr_done) { print "Address=" ip; addr_done=1 }
                if (have_gw && !gw_done) { print "Gateway=" gw; gw_done=1 }
            }
            in_net=0; print; next
        }
        in_net && /^Address=/ { if (!addr_done) { print "Address=" ip; addr_done=1 }; next }
        in_net && /^Gateway=/ { if (have_gw) { if (!gw_done) { print "Gateway=" gw; gw_done=1 } } else print; next }
        { print }
        END {
            if (in_net) {
                if (!addr_done) print "Address=" ip
                if (have_gw && !gw_done) print "Gateway=" gw
            } else if (!addr_done) {
                print "[Network]"; print "Address=" ip
                if (have_gw && !gw_done) print "Gateway=" gw
            }
        }
    ' "$1" > "$2"
}

gen_timesyncd() {  # $1=src $2=dst  (--ntp 지정 시에만 호출)
    awk -v ntp="$NTP" '
        BEGIN { in_time=0; done=0 }
        /^\[Time\]/ { in_time=1; print; next }
        /^\[/ { if (in_time && !done) { print "NTP=" ntp; done=1 }; in_time=0; print; next }
        in_time && /^NTP=/ { if (!done) { print "NTP=" ntp; done=1 }; next }
        { print }
        END {
            if (!done) { if (!in_time) print "[Time]"; print "NTP=" ntp }
        }
    ' "$1" > "$2"
}

gen_wpa() {  # $1=src $2=dst  (--ssid/--psk 지정 시에만 호출; 첫 network 블록만 수정)
    local hex; hex="$(ssid_to_hex "$SSID")"
    awk -v ssid_hex="$hex" -v psk="$PSK" '
        BEGIN { depth=0; in_first=0; first_done=0 }
        /^network=\{/ {
            depth=1
            if (!first_done) in_first=1
            print; next
        }
        depth==1 && /^\}/ {
            depth=0
            if (in_first) { in_first=0; first_done=1 }
            print; next
        }
        in_first && /^[ \t]*ssid=/ { print "    ssid=" ssid_hex; next }
        in_first && /^[ \t]*psk=/ { print "    psk=\"" psk "\""; next }
        { print }
    ' "$1" > "$2"
}

gen_conf_toggles() {  # $1=src $2=dst  (profile b 에서만 호출)
    jq '.wbridge.peer_route.enabled = true
        | .wbridge.ip_discovery = true
        | .wbridge.arp_ignore_always.enabled = false' "$1" > "$2"
    jq empty "$2"  # 산출 JSON 유효성 재확인
}

# ---------- 대상 파일 목록 산출 ----------

# 변경 대상을 "src|이름" 줄로 출력 (조건부 항목은 옵션에 따라)
list_targets() {
    echo "$NET_FILE|20-mlan0.network"
    [ -n "$NTP" ] && echo "$TS_FILE|timesyncd.conf"
    [ -n "$SSID" ] && echo "$WPA_FILE|wpa_supplicant-mlan0.conf"
    if [ "$PROFILE" = "b" ]; then
        local conf; conf="$(find_conf)" || die "wifi_init_conf.json 을 찾지 못함 (${CONF_CANDIDATES[*]})"
        echo "$conf|wifi_init_conf.json"
    fi
    return 0
}

gen_one() {  # $1=src $2=이름 $3=dst — 이름에 따라 변환기 선택
    case "$2" in
        20-mlan0.network)          gen_network "$1" "$3" ;;
        timesyncd.conf)            gen_timesyncd "$1" "$3" ;;
        wpa_supplicant-mlan0.conf) gen_wpa "$1" "$3" ;;
        wifi_init_conf.json)       gen_conf_toggles "$1" "$3" ;;
        *) die "내부 오류: 알 수 없는 대상 $2" ;;
    esac
}

# ---------- 서브커맨드 ----------

require_conf_if_b() {
    [ "$PROFILE" = "b" ] || return 0
    find_conf >/dev/null || die "wifi_init_conf.json 을 찾지 못함 (${CONF_CANDIDATES[*]})"
}

do_plan() {
    validate_args; guard_target; require_conf_if_b
    local tmp; tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT
    local src name changed=0
    while IFS='|' read -r src name; do
        [ -f "$src" ] || die "대상 파일 없음: $src"
        gen_one "$src" "$name" "$tmp/$name"
        if diff -u "$src" "$tmp/$name" > "$tmp/$name.diff"; then
            info "$name: 변경 없음"
        else
            changed=1
            echo "----- $name ($src) -----"
            # diff 출력에서 psk 마스킹
            sed 's/psk=".*"/psk="********"/' "$tmp/$name.diff"
        fi
    done < <(list_targets)
    if [ "$PROFILE" = "a" ]; then
        local conf; conf="$(find_conf 2>/dev/null)" || true
        if [ -n "${conf:-}" ]; then
            info "profile a — 토글 불변. 현재값: $(jq -c '{peer_route: .wbridge.peer_route.enabled, ip_discovery: .wbridge.ip_discovery, arp_ignore_always: .wbridge.arp_ignore_always.enabled}' "$conf")"
        fi
    fi
    [ "$changed" -eq 1 ] && info "적용하려면: apply (동일 인자). 적용 후 재부팅 필요." || info "적용할 변경이 없음."
}

do_apply() {
    validate_args; guard_target; require_conf_if_b; guard_selfcut
    local tmp; tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT

    # 1) 전 대상 변환을 먼저 성공시킨다 (하나라도 실패 시 시스템 무변경)
    local src name pairs=() changed=()
    while IFS='|' read -r src name; do
        [ -f "$src" ] || die "대상 파일 없음: $src"
        gen_one "$src" "$name" "$tmp/$name"
        pairs+=("$src|$name")
        diff -q "$src" "$tmp/$name" >/dev/null || changed+=("$name")
    done < <(list_targets)

    # 변경이 전혀 없으면 백업·반영·재부팅을 모두 생략 (rollback 이력 오염 방지)
    if [ "${#changed[@]}" -eq 0 ]; then
        info "적용할 파일 변경 없음 — 백업/반영/재부팅 생략"
        if [ -n "$NTP" ] && [ -z "$ROOT" ]; then
            timedatectl set-ntp true && info "timedatectl set-ntp true 실행 (멱등)" \
                || info "경고: set-ntp true 실패 — 수동 확인 필요"
        fi
        return 0
    fi

    # 2) 백업 — 상대경로 -C 방식 (GNU/BusyBox tar 공통; 2026-09-02 실기 리허설에서 BusyBox 비호환 발견)
    mkdir -p "$BACKUP_DIR"
    local stamp bk
    stamp="$(date +%Y%m%d-%H%M%S)"
    bk="$BACKUP_DIR/bd-provision-$stamp.tar"
    local rels=() p
    for p in "${pairs[@]}"; do
        src="${p%%|*}"
        rels+=("${src#"$ROOT"/}")   # ROOT 공백이면 선두 / 제거 → / 기준 상대경로
    done
    tar -cf "$bk" -C "${ROOT:-/}" "${rels[@]}"
    {
        echo "# bd_provision backup $stamp"
        echo "# args: ip=$IPCIDR gw=$GW ntp=$NTP ssid=${SSID:+SET} profile=$PROFILE"
        printf '# file: %s\n' "${rels[@]}"
    } > "$bk.manifest"
    info "백업: $bk"

    # 3) 원자 반영 (tmp → 대상, 권한 보존)
    for p in "${pairs[@]}"; do
        src="${p%%|*}"; name="${p##*|}"
        if diff -q "$src" "$tmp/$name" >/dev/null; then
            info "$name: 변경 없음 (skip)"
            continue
        fi
        # 권한/소유자 보존 — stat 기반 (chmod --reference는 GNU 전용, BusyBox 미지원)
        local mode own
        mode="$(stat -c %a "$src" 2>/dev/null || echo '')"
        own="$(stat -c %u:%g "$src" 2>/dev/null || echo '')"
        [ -n "$mode" ] && chmod "$mode" "$tmp/$name"
        [ -n "$own" ] && chown "$own" "$tmp/$name" 2>/dev/null || true
        mv "$tmp/$name" "$src"
        info "$name: 반영 완료 → $src"
    done

    # 4) NTP 재활성화 (postinst set-ntp false 래치 해제) — --ntp 지정 시에만, 1회 명령
    if [ -n "$NTP" ] && [ -z "$ROOT" ]; then
        timedatectl set-ntp true && info "timedatectl set-ntp true 실행" \
            || info "경고: set-ntp true 실패 — 수동 확인 필요"
    fi

    # 5) 재부팅 (networkd·wifi_init·wbridge 모두 부팅 시 1회 read → 재부팅이 표준 반영 경로)
    if [ -z "$ROOT" ] && [ "$DO_REBOOT" -eq 1 ]; then
        info "5초 후 재부팅합니다 (Ctrl-C로 중단)..."
        sleep 5
        systemctl reboot
    else
        info "★ 재부팅 필요 — 재부팅 후 'verify' (동일 인자)로 점검하세요. 롤백: rollback"
    fi
}

# verify: 읽기 전용 — 어떤 것도 고치지 않는다 (사용자 지시: 자동 교정 금지)
V_FAIL=0
chk() {  # $1=PASS|FAIL|SKIP|INFO $2=메시지
    echo "  [$1] $2"
    [ "$1" = "FAIL" ] && V_FAIL=1
    return 0
}

do_verify() {
    validate_args; guard_target
    info "verify (읽기 전용 — 불일치는 보고만 함)"

    # 파일 레벨 (테스트 모드에서도 수행)
    grep -q "^Address=$IPCIDR$" "$NET_FILE" \
        && chk PASS "20-mlan0.network Address=$IPCIDR" \
        || chk FAIL "20-mlan0.network Address 불일치 (기대 $IPCIDR)"
    if [ -n "$GW" ]; then
        grep -q "^Gateway=$GW$" "$NET_FILE" \
            && chk PASS "20-mlan0.network Gateway=$GW" \
            || chk FAIL "20-mlan0.network Gateway 불일치 (기대 $GW)"
    fi
    if [ -n "$NTP" ]; then
        grep -q "^NTP=$NTP$" "$TS_FILE" \
            && chk PASS "timesyncd.conf NTP=$NTP" \
            || chk FAIL "timesyncd.conf NTP 불일치 (기대 $NTP)"
    fi
    if [ -n "$SSID" ]; then
        local hex; hex="$(ssid_to_hex "$SSID")"
        grep -q "ssid=$hex" "$WPA_FILE" \
            && chk PASS "wpa conf ssid(hex) 일치" \
            || chk FAIL "wpa conf ssid 불일치"
    fi
    local conf; conf="$(find_conf 2>/dev/null)" || true
    if [ -n "${conf:-}" ]; then
        local t
        t="$(jq -r '[.wbridge.peer_route.enabled, .wbridge.ip_discovery, .wbridge.arp_ignore_always.enabled] | @csv' "$conf")"
        if [ "$PROFILE" = "b" ]; then
            [ "$t" = "true,true,false" ] \
                && chk PASS "토글 3종 = Option B (peer_route=on, ip_discovery=on, arp_ignore_always=off)" \
                || chk FAIL "토글 3종 불일치 (현재 $t, 기대 true,true,false)"
        else
            chk INFO "토글 3종 현재값(참고): $t"
        fi
    fi

    # 라이브 레벨 (타겟에서만)
    if [ -n "$ROOT" ]; then
        chk SKIP "라이브 점검 (PROVISION_ROOT 테스트 모드)"
    else
        ip -4 -o addr show mlan0 2>/dev/null | grep -q "$IPCIDR" \
            && chk PASS "mlan0 라이브 IP $IPCIDR" \
            || chk FAIL "mlan0 라이브 IP 불일치 (재부팅 전이면 정상 — 재부팅 후 재실행)"
        if [ -n "$GW" ]; then
            ip route show default 2>/dev/null | grep -q "via $GW dev mlan0" \
                && chk PASS "default route via $GW dev mlan0" \
                || chk FAIL "default route 불일치"
            ping -c2 -W2 "$GW" >/dev/null 2>&1 \
                && chk PASS "GW $GW ping 왕복" \
                || chk FAIL "GW $GW ping 실패 (L2 도달성/ARP 확인 필요)"
        fi
        if [ -n "$NTP" ]; then
            local ntp_on sync
            ntp_on="$(timedatectl show -p NTP --value 2>/dev/null || echo '?')"
            sync="$(timedatectl show -p NTPSynchronized --value 2>/dev/null || echo '?')"
            [ "$ntp_on" = "yes" ] && chk PASS "timedatectl NTP=yes" \
                || chk FAIL "timedatectl NTP=$ntp_on (set-ntp 래치 미해제?)"
            [ "$sync" = "yes" ] && chk PASS "NTPSynchronized=yes" \
                || chk INFO "NTPSynchronized=$sync (동기까지 수 분 걸릴 수 있음)"
        fi
        if [ -n "$SSID" ]; then
            local wst wssid
            wst="$(wpa_cli -i mlan0 status 2>/dev/null | awk -F= '/^wpa_state=/{print $2}')"
            wssid="$(wpa_cli -i mlan0 status 2>/dev/null | awk -F= '/^ssid=/{print $2}')"
            [ "$wst" = "COMPLETED" ] && chk PASS "wpa_state=COMPLETED" \
                || chk FAIL "wpa_state=$wst"
            [ "$wssid" = "$SSID" ] && chk PASS "접속 SSID=$SSID" \
                || chk INFO "접속 SSID=$wssid (기대 $SSID — 재접속 대기 중일 수 있음)"
        fi
        pgrep -f "wifi-wbridge" >/dev/null 2>&1 \
            && chk PASS "wbridge 프로세스 동작" \
            || chk INFO "wbridge(pcap) 프로세스 없음 (moal 엔진이면 정상)"
        ss -ulpn 2>/dev/null | grep -q ":50607" \
            && chk INFO "opcd UDP 50607 리슨 중" \
            || chk INFO "opcd 미리슨 (opcd 미설치 기기면 정상)"
    fi

    if [ "$V_FAIL" -eq 1 ]; then
        info "verify 결과: FAIL 있음 — 아무것도 자동 수정하지 않았음. 필요 시 rollback 또는 수동 조치."
        exit 1
    fi
    info "verify 결과: 전체 통과"
}

do_rollback() {
    guard_target
    local bk="$BACKUP_FILE"
    if [ -z "$bk" ]; then
        bk="$(ls -1t "$BACKUP_DIR"/bd-provision-*.tar 2>/dev/null | head -1)" \
            || true
        [ -n "$bk" ] || die "백업 없음: $BACKUP_DIR"
    fi
    [ -f "$bk" ] || die "백업 파일 없음: $bk"
    info "복원: $bk"
    tar -xf "$bk" -C "${ROOT:-/}"
    info "★ 복원 완료 — 재부팅해야 라이브 상태에 반영됨 (자동 재부팅하지 않음)"
}

case "$CMD" in
    plan)     do_plan ;;
    apply)    do_apply ;;
    verify)   do_verify ;;
    rollback) do_rollback ;;
    ""|-h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "알 수 없는 서브커맨드: $CMD (plan|apply|verify|rollback)" ;;
esac
