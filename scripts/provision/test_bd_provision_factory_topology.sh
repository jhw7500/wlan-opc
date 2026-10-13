#!/bin/bash

set -euo pipefail

SCRIPT="$(cd "$(dirname "$0")" && pwd)/bd_provision.sh"
HELPER="${PROVISION_TEST_TOPOLOGY_HELPER:-/usr/local/scripts/wifi_factory_topology.py}"
FAIL_AFTER_SET_HELPER="$(cd "$(dirname "$0")" && pwd)/test_topology_helper_fail_after_set.py"
PASS_COUNT=0

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS: $*"
}

assert_eq() {
    local expected="$1" actual="$2" message="$3"
    [ "$expected" = "$actual" ] || fail "$message (expected=$expected actual=$actual)"
}

assert_file_eq() {
    cmp -s "$1" "$2" || fail "$3"
}

new_root() {
    local root="$1"
    mkdir -p \
        "$root/etc/systemd/network" \
        "$root/etc/systemd" \
        "$root/etc/wpa_supplicant" \
        "$root/opt/wlan/config" \
        "$root/usr/local/etc" \
        "$root/var/backups/bd-provision"
    chmod 755 "$root/usr/local/etc"
    printf '[Match]\nName=mlan0\n\n[Network]\nAddress=192.168.0.100/24\nGateway=192.168.0.1\n' \
        > "$root/etc/systemd/network/20-mlan0.network"
    printf '[Time]\nNTP=192.168.0.1\n' > "$root/etc/systemd/timesyncd.conf"
    printf 'ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n    ssid=6f6c64\n    psk="oldpassword"\n}\n' \
        > "$root/etc/wpa_supplicant/wpa_supplicant-mlan0.conf"
    printf '%s\n' '{"wbridge":{"peer_route":{"enabled":false},"ip_discovery":false,"arp_ignore_always":{"enabled":true}},"keep":"unchanged"}' \
        > "$root/opt/wlan/config/wifi_init_conf.json"
}

run_provision() {
    local root="$1"; shift
    PROVISION_ROOT="$root" \
    PROVISION_TOPOLOGY_HELPER="$HELPER" \
        "$SCRIPT" "$@"
}

run_provision_without_helper() {
    local root="$1"; shift
    PROVISION_ROOT="$root" \
    PROVISION_TOPOLOGY_HELPER="$root/does-not-exist.py" \
        "$SCRIPT" "$@"
}

test_no_flag_does_not_require_helper_or_touch_selector() {
    local root="$TEST_TMP/no-flag"
    new_root "$root"
    run_provision_without_helper "$root" apply --ip 192.168.0.101/24 >/dev/null
    grep -q '^Address=192.168.0.101/24$' "$root/etc/systemd/network/20-mlan0.network" \
        || fail "legacy apply did not update network"
    [ ! -e "$root/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "legacy apply touched selector"
    sleep 1
    run_provision_without_helper "$root" apply --ip 192.168.0.101/24 --profile b >/dev/null
    assert_eq "true,true,false" \
        "$(jq -r '[.wbridge.peer_route.enabled,.wbridge.ip_discovery,.wbridge.arp_ignore_always.enabled] | @csv' "$root/opt/wlan/config/wifi_init_conf.json" | tr -d '"')" \
        "legacy profile b behavior changed"
    [ ! -e "$root/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "legacy profile b inferred a selector"
    [ ! -e "$root/usr/local/etc/.wifi_factory_topology.json.lock" ] \
        || fail "legacy apply created a selector lock without --factory-topology"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    python3 "$HELPER" set --path "$selector" --topology peer-route \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" >/dev/null
    sleep 1
    run_provision_without_helper "$root" apply --ip 192.168.0.102/24 >/dev/null
    local backup
    backup="$(find "$root/var/backups/bd-provision" -name 'bd-provision-*.tar' -printf '%T@ %p\n' \
        | sort -nr | head -1 | cut -d' ' -f2-)"
    python3 "$HELPER" set --path "$selector" --topology eth0-ip \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" >/dev/null
    run_provision_without_helper "$root" rollback --backup "$backup" >/dev/null
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" \
        "UNMANAGED rollback touched selector"
    pass "commands without --factory-topology do not require helper or touch selector"
}

test_bad_value_and_missing_helper_fail_before_active_write() {
    local root="$TEST_TMP/preflight"
    new_root "$root"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    if run_provision "$root" apply --ip 192.168.0.101/24 --factory-topology invalid >/dev/null 2>&1; then
        fail "invalid topology was accepted"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "invalid topology changed network"
    if run_provision_without_helper "$root" apply --ip 192.168.0.101/24 --factory-topology peer-route >/dev/null 2>&1; then
        fail "missing topology helper was accepted"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "missing helper changed network"
    [ ! -e "$root/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "failed preflight created selector"
    rmdir "$root/usr/local/etc"
    if run_provision "$root" apply --ip 192.168.0.101/24 --factory-topology peer-route >/dev/null 2>&1; then
        fail "missing selector directory was accepted"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "missing selector directory changed network"
    pass "invalid values, missing helper and missing selector directory fail before active writes"
}

test_create_change_noop_and_metadata() {
    local root="$TEST_TMP/create-change"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    run_provision "$root" plan --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null
    [ ! -e "$selector" ] || fail "plan created selector"
    run_provision "$root" apply --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null
    assert_eq "peer-route" "$(jq -r .topology "$selector")" "initial topology"
    assert_eq "1" "$(jq -r .schema_version "$selector")" "selector schema"
    assert_eq "600" "$(stat -c %a "$selector")" "selector mode"
    assert_eq "$(id -u):$(id -g)" "$(stat -c %u:%g "$selector")" "selector owner in fakeroot"
    local selector_before selector_after
    selector_before="$(sha256sum "$selector" | awk '{print $1}')"
    run_provision "$root" plan --ip 192.168.0.100/24 --factory-topology eth0-ip >/dev/null
    selector_after="$(sha256sum "$selector" | awk '{print $1}')"
    assert_eq "$selector_before" "$selector_after" "plan changed selector"
    local before_count after_count
    before_count="$(find "$root/var/backups/bd-provision" -name 'bd-provision-*.tar' | wc -l)"
    run_provision "$root" apply --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null
    after_count="$(find "$root/var/backups/bd-provision" -name 'bd-provision-*.tar' | wc -l)"
    assert_eq "$before_count" "$after_count" "no-op apply created backup"
    sleep 1
    run_provision "$root" apply --ip 192.168.0.100/24 --factory-topology eth0-ip >/dev/null
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" "existing topology change"
    pass "selector creation, change, no-op and canonical metadata"
}

test_corrupt_and_unsafe_selectors_fail_without_action() {
    local corrupt="$TEST_TMP/corrupt-selector"
    new_root "$corrupt"
    local selector="$corrupt/usr/local/etc/wifi_factory_topology.json"
    printf '{broken\n' > "$selector"
    chmod 600 "$selector"
    cp "$corrupt/etc/systemd/network/20-mlan0.network" "$corrupt/network.before"
    if run_provision "$corrupt" apply --ip 192.168.0.101/24 \
        --factory-topology peer-route >/dev/null 2>&1; then
        fail "corrupt selector was accepted"
    fi
    assert_file_eq "$corrupt/network.before" "$corrupt/etc/systemd/network/20-mlan0.network" \
        "corrupt selector changed network"
    assert_eq "0" "$(find "$corrupt/var/backups/bd-provision" -name '*.tar' | wc -l)" \
        "corrupt selector created backup"

    local unsafe="$TEST_TMP/unsafe-selector"
    new_root "$unsafe"
    local victim="$unsafe/victim"
    printf 'do-not-touch\n' > "$victim"
    ln -s "$victim" "$unsafe/usr/local/etc/wifi_factory_topology.json"
    cp "$unsafe/etc/systemd/network/20-mlan0.network" "$unsafe/network.before"
    if run_provision "$unsafe" apply --ip 192.168.0.101/24 \
        --factory-topology peer-route >/dev/null 2>&1; then
        fail "symlink selector was accepted"
    fi
    assert_file_eq "$unsafe/network.before" "$unsafe/etc/systemd/network/20-mlan0.network" \
        "unsafe selector changed network"
    assert_eq "do-not-touch" "$(sed -n '1p' "$victim")" "unsafe selector clobbered symlink target"
    pass "corrupt and unsafe selectors fail before backup or active writes"
}

test_unsafe_shared_selector_lock_fails_before_active_write() {
    local root="$TEST_TMP/unsafe-shared-lock"
    new_root "$root"
    local lock="$root/usr/local/etc/.wifi_factory_topology.json.lock"
    local victim="$root/lock-victim"
    printf 'do-not-touch\n' > "$victim"
    ln -s "$victim" "$lock"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    if run_provision "$root" apply --ip 192.168.0.101/24 \
        --factory-topology peer-route >/dev/null 2>&1; then
        fail "symlink shared selector lock was accepted"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "unsafe shared lock changed active network"
    assert_eq "do-not-touch" "$(sed -n '1p' "$victim")" \
        "unsafe shared lock clobbered symlink target"
    [ ! -e "$root/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "unsafe shared lock created selector"

    rm -f "$lock"
    : > "$lock"
    chmod 644 "$lock"
    if run_provision "$root" apply --ip 192.168.0.101/24 \
        --factory-topology peer-route >/dev/null 2>&1; then
        fail "wrong-mode shared selector lock was accepted"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "wrong-mode shared lock changed active network"
    pass "unsafe shared selector lock fails before active writes"
}

test_profile_and_factory_topology_are_independent() {
    local root="$TEST_TMP/profile-independent"
    new_root "$root"
    local conf="$root/opt/wlan/config/wifi_init_conf.json"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    run_provision "$root" apply --ip 192.168.0.100/24 --profile a --factory-topology peer-route >/dev/null
    assert_eq "false,false,true" \
        "$(jq -r '[.wbridge.peer_route.enabled,.wbridge.ip_discovery,.wbridge.arp_ignore_always.enabled] | @csv' "$conf" | tr -d '"')" \
        "profile a toggles changed"
    assert_eq "peer-route" "$(jq -r .topology "$selector")" "profile a inferred wrong selector"
    sleep 1
    run_provision "$root" apply --ip 192.168.0.100/24 --profile b --factory-topology eth0-ip >/dev/null
    assert_eq "true,true,false" \
        "$(jq -r '[.wbridge.peer_route.enabled,.wbridge.ip_discovery,.wbridge.arp_ignore_always.enabled] | @csv' "$conf" | tr -d '"')" \
        "profile b toggles not applied"
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" "profile b inferred selector"
    pass "runtime profile and durable factory topology remain independent"
}

test_verify_is_read_only_and_rejects_invalid_selector() {
    local root="$TEST_TMP/verify"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    run_provision "$root" apply --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null
    local before after
    before="$(sha256sum "$selector" | awk '{print $1}')"
    run_provision "$root" verify --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null
    after="$(sha256sum "$selector" | awk '{print $1}')"
    assert_eq "$before" "$after" "verify changed valid selector"
    if run_provision "$root" verify --ip 192.168.0.100/24 \
        --factory-topology eth0-ip >/dev/null 2>&1; then
        fail "verify accepted a topology mismatch"
    fi
    after="$(sha256sum "$selector" | awk '{print $1}')"
    assert_eq "$before" "$after" "mismatch verify changed selector"
    printf '{broken\n' > "$selector"
    chmod 600 "$selector"
    before="$(sha256sum "$selector" | awk '{print $1}')"
    if run_provision "$root" verify --ip 192.168.0.100/24 --factory-topology peer-route >/dev/null 2>&1; then
        fail "verify accepted invalid selector"
    fi
    after="$(sha256sum "$selector" | awk '{print $1}')"
    assert_eq "$before" "$after" "verify changed invalid selector"
    pass "verify reads selector without repairing it"
}

test_rollback_restores_absent_and_present_selector_states() {
    local absent="$TEST_TMP/rollback-absent"
    new_root "$absent"
    cp "$absent/etc/systemd/network/20-mlan0.network" "$absent/network.before"
    run_provision "$absent" apply --ip 192.168.0.101/24 --factory-topology peer-route >/dev/null
    local backup
    backup="$(find "$absent/var/backups/bd-provision" -name 'bd-provision-*.tar' | head -1)"
    run_provision "$absent" rollback --backup "$backup" >/dev/null
    assert_file_eq "$absent/network.before" "$absent/etc/systemd/network/20-mlan0.network" \
        "rollback did not restore network"
    [ ! -e "$absent/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "rollback did not restore absent selector"

    local present="$TEST_TMP/rollback-present"
    new_root "$present"
    local selector="$present/usr/local/etc/wifi_factory_topology.json"
    python3 "$HELPER" set --path "$selector" --topology peer-route \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" >/dev/null
    cp -p "$selector" "$present/selector.before"
    sleep 1
    run_provision "$present" apply --ip 192.168.0.100/24 --factory-topology eth0-ip >/dev/null
    backup="$(find "$present/var/backups/bd-provision" -name 'bd-provision-*.tar' | head -1)"
    run_provision "$present" rollback --backup "$backup" >/dev/null
    assert_file_eq "$present/selector.before" "$selector" "rollback did not restore selector bytes"
    assert_eq "$(stat -c %a:%u:%g "$present/selector.before")" "$(stat -c %a:%u:%g "$selector")" \
        "rollback did not restore selector metadata"
    pass "rollback restores absent and present selector state"
}

test_legacy_tar_does_not_modify_selector() {
    local root="$TEST_TMP/legacy-tar"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    python3 "$HELPER" set --path "$selector" --topology peer-route \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" >/dev/null
    local backup="$root/legacy.tar"
    tar -cf "$backup" -C "$root" etc/systemd/network/20-mlan0.network
    printf 'Address=203.0.113.9/24\n' > "$root/etc/systemd/network/20-mlan0.network"
    run_provision "$root" rollback --backup "$backup" >/dev/null
    assert_eq "peer-route" "$(jq -r .topology "$selector")" "legacy rollback changed selector"
    pass "legacy tar remains compatible and leaves selector alone"
}

test_mid_apply_failure_rolls_back_files_and_selector() {
    local root="$TEST_TMP/mid-apply"
    new_root "$root"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    cp "$root/etc/systemd/timesyncd.conf" "$root/timesync.before"
    local bin="$root/test-bin"
    mkdir -p "$bin"
    # shellcheck disable=SC2016 # 아래 문자열은 실행 시 생성될 독립 wrapper의 본문이다.
    printf '%s\n' \
        '#!/bin/bash' \
        'for last_arg in "$@"; do :; done' \
        '[ "$last_arg" != "$PROVISION_TEST_FAIL_DEST" ] || exit 73' \
        'exec /usr/bin/mv "$@"' > "$bin/mv"
    chmod +x "$bin/mv"
    if PATH="$bin:$PATH" PROVISION_TEST_FAIL_DEST="$root/etc/systemd/timesyncd.conf" \
        PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
        "$SCRIPT" apply --ip 192.168.0.101/24 --ntp 192.168.0.2 \
            --factory-topology peer-route >/dev/null 2>&1; then
        fail "injected mid-apply failure unexpectedly succeeded"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "mid-apply failure left changed network"
    assert_file_eq "$root/timesync.before" "$root/etc/systemd/timesyncd.conf" \
        "mid-apply failure changed timesync"
    [ ! -e "$root/usr/local/etc/wifi_factory_topology.json" ] \
        || fail "mid-apply failure left selector"
    pass "mid-apply failure restores already changed targets"
}

test_visible_selector_failure_rolls_back_whole_apply() {
    local root="$TEST_TMP/selector-commit-failure"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    if PROVISION_ROOT="$root" \
        PROVISION_TOPOLOGY_HELPER="$FAIL_AFTER_SET_HELPER" \
        PROVISION_TEST_REAL_HELPER="$HELPER" \
        PROVISION_TEST_FAIL_PATH="$selector" \
        "$SCRIPT" apply --ip 192.168.0.101/24 \
            --factory-topology peer-route >/dev/null 2>&1; then
        fail "post-visible selector failure unexpectedly succeeded"
    fi
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "selector failure left changed network"
    [ ! -e "$selector" ] || fail "selector failure left NEW selector visible"
    pass "post-visible selector failure rolls back the whole apply"
}

test_cooperative_writer_waits_through_mid_file_failure_rollback() {
    local root="$TEST_TMP/cooperative-mid-failure"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    local bin="$root/test-bin" marker="$root/network-commit-started"
    mkdir -p "$bin"
    # shellcheck disable=SC2016
    printf '%s\n' \
        '#!/bin/bash' \
        'for last_arg in "$@"; do :; done' \
        'if [ "$last_arg" = "$PROVISION_TEST_MARK_DEST" ]; then : > "$PROVISION_TEST_COMMIT_MARKER"; sleep 1; fi' \
        '[ "$last_arg" != "$PROVISION_TEST_FAIL_DEST" ] || exit 73' \
        'exec /usr/bin/mv "$@"' > "$bin/mv"
    chmod +x "$bin/mv"

    PATH="$bin:$PATH" \
        PROVISION_TEST_MARK_DEST="$root/etc/systemd/network/20-mlan0.network" \
        PROVISION_TEST_FAIL_DEST="$root/etc/systemd/timesyncd.conf" \
        PROVISION_TEST_COMMIT_MARKER="$marker" \
        PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
        "$SCRIPT" apply --ip 192.168.0.101/24 --ntp 192.168.0.2 \
            --factory-topology peer-route > "$root/apply.log" 2>&1 &
    local apply_pid=$! tries=0
    while [ ! -e "$marker" ] && [ "$tries" -lt 100 ]; do
        sleep 0.05
        tries=$((tries + 1))
    done
    [ -e "$marker" ] || { wait "$apply_pid" || true; fail "apply did not reach first active-file commit"; }

    python3 "$HELPER" set --path "$selector" --topology eth0-ip \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" > "$root/writer.log" 2>&1 &
    local writer_pid=$! writer_was_blocked=0 apply_rc=0 writer_rc=0
    sleep 0.15
    kill -0 "$writer_pid" 2>/dev/null && writer_was_blocked=1
    wait "$apply_pid" || apply_rc=$?
    wait "$writer_pid" || writer_rc=$?
    [ "$apply_rc" -ne 0 ] || fail "injected mid-file failure unexpectedly succeeded"
    assert_eq "0" "$writer_rc" "cooperative writer failed after host rollback"
    assert_eq "1" "$writer_was_blocked" "cooperative writer did not wait for host rollback"
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" \
        "cooperative writer did not win after host rollback released the selector lock"
    pass "cooperative writer waits through mid-file failure rollback"
}

test_cooperative_writer_waits_through_post_visible_set_failure_rollback() {
    local root="$TEST_TMP/cooperative-post-set-failure"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    local marker="$root/post-set-visible"
    PROVISION_ROOT="$root" \
        PROVISION_TOPOLOGY_HELPER="$FAIL_AFTER_SET_HELPER" \
        PROVISION_TEST_REAL_HELPER="$HELPER" \
        PROVISION_TEST_FAIL_PATH="$selector" \
        PROVISION_TEST_POST_SET_MARKER="$marker" \
        "$SCRIPT" apply --ip 192.168.0.101/24 --factory-topology peer-route \
        > "$root/apply.log" 2>&1 &
    local apply_pid=$! tries=0
    while [ ! -e "$marker" ] && [ "$tries" -lt 100 ]; do
        sleep 0.05
        tries=$((tries + 1))
    done
    [ -e "$marker" ] || { wait "$apply_pid" || true; fail "apply did not expose selector before injected helper failure"; }

    python3 "$HELPER" set --path "$selector" --topology eth0-ip \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" > "$root/writer.log" 2>&1 &
    local writer_pid=$! writer_was_blocked=0 apply_rc=0 writer_rc=0
    sleep 0.15
    kill -0 "$writer_pid" 2>/dev/null && writer_was_blocked=1
    wait "$apply_pid" || apply_rc=$?
    wait "$writer_pid" || writer_rc=$?
    [ "$apply_rc" -ne 0 ] || fail "post-visible selector failure unexpectedly succeeded"
    assert_eq "0" "$writer_rc" "cooperative writer failed after selector rollback"
    assert_eq "1" "$writer_was_blocked" "cooperative writer did not wait for selector rollback"
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" \
        "cooperative writer did not win after failed host set rollback released the lock"
    pass "cooperative writer waits through post-visible selector failure rollback"
}

test_same_second_backups_are_unique_and_private() {
    local root="$TEST_TMP/unique-backups"
    new_root "$root"
    local bin="$root/test-bin"
    mkdir -p "$bin"
    # The fake date script expands its own arguments at runtime.
    # shellcheck disable=SC2016
    printf '%s\n' \
        '#!/bin/bash' \
        'if [ "${1:-}" = "+%Y%m%d-%H%M%S" ]; then printf "%s\n" 20261014-040000; else exec /usr/bin/date "$@"; fi' \
        > "$bin/date"
    chmod +x "$bin/date"
    (
        umask 022
        PATH="$bin:$PATH" PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
            "$SCRIPT" apply --ip 192.168.0.101/24 --factory-topology peer-route >/dev/null
        PATH="$bin:$PATH" PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
            "$SCRIPT" apply --ip 192.168.0.102/24 --factory-topology eth0-ip >/dev/null
    )
    local tar_count manifest_count artifact
    tar_count="$(find "$root/var/backups/bd-provision" -maxdepth 1 -type f -name 'bd-provision-*.tar' | wc -l)"
    manifest_count="$(find "$root/var/backups/bd-provision" -maxdepth 1 -type f -name 'bd-provision-*.tar.manifest' | wc -l)"
    assert_eq "2" "$tar_count" "same-second backup tar collision"
    assert_eq "2" "$manifest_count" "same-second backup manifest collision"
    while IFS= read -r artifact; do
        assert_eq "600" "$(stat -c %a "$artifact")" "backup artifact is not private"
        assert_eq "$(id -u):$(id -g)" "$(stat -c %u:%g "$artifact")" \
            "backup artifact owner mismatch"
    done < <(find "$root/var/backups/bd-provision" -maxdepth 1 -type f \
        \( -name 'bd-provision-*.tar' -o -name 'bd-provision-*.tar.manifest' \) | sort)
    pass "same-second backups are unique complete pairs with mode 0600"
}

test_auto_rollback_ignores_incomplete_orphan() {
    local root="$TEST_TMP/orphan-backup"
    new_root "$root"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    run_provision "$root" apply --ip 192.168.0.101/24 --factory-topology peer-route >/dev/null
    local orphan="$root/var/backups/bd-provision/bd-provision-99999999-999999-orphan.tar"
    tar -cf "$orphan" -C "$root" etc/systemd/network/20-mlan0.network
    touch "$orphan"
    printf 'Address=203.0.113.9/24\n' > "$root/etc/systemd/network/20-mlan0.network"
    run_provision "$root" rollback >/dev/null
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "automatic rollback selected incomplete orphan"
    pass "automatic rollback selects only a complete tar/manifest pair"
}

test_managed_rollback_holds_shared_selector_lock() {
    local root="$TEST_TMP/managed-rollback-lock"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    run_provision "$root" apply --ip 192.168.0.101/24 \
        --factory-topology peer-route >/dev/null
    local backup
    backup="$(find "$root/var/backups/bd-provision" -name 'bd-provision-*.tar' | head -1)"
    local bin="$root/test-bin" marker="$root/restore-started"
    mkdir -p "$bin"
    # shellcheck disable=SC2016
    printf '%s\n' \
        '#!/bin/bash' \
        'if [ "${1:-}" = "-xf" ]; then : > "$PROVISION_TEST_RESTORE_MARKER"; sleep 1; fi' \
        'exec /usr/bin/tar "$@"' > "$bin/tar"
    chmod +x "$bin/tar"
    PATH="$bin:$PATH" PROVISION_TEST_RESTORE_MARKER="$marker" \
        PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
        "$SCRIPT" rollback --backup "$backup" > "$root/rollback.log" 2>&1 &
    local rollback_pid=$! tries=0
    while [ ! -e "$marker" ] && [ "$tries" -lt 100 ]; do
        sleep 0.05
        tries=$((tries + 1))
    done
    [ -e "$marker" ] || { wait "$rollback_pid" || true; fail "managed rollback did not reach restore"; }

    python3 "$HELPER" set --path "$selector" --topology eth0-ip \
        --owner-uid "$(id -u)" --owner-gid "$(id -g)" > "$root/writer.log" 2>&1 &
    local writer_pid=$! writer_was_blocked=0 rollback_rc=0 writer_rc=0
    sleep 0.15
    kill -0 "$writer_pid" 2>/dev/null && writer_was_blocked=1
    wait "$rollback_pid" || rollback_rc=$?
    wait "$writer_pid" || writer_rc=$?
    assert_eq "0" "$rollback_rc" "managed rollback failed"
    assert_eq "0" "$writer_rc" "cooperative writer failed after managed rollback"
    assert_eq "1" "$writer_was_blocked" "cooperative writer did not wait for managed rollback"
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" \
        "cooperative writer did not win after managed rollback released the lock"
    pass "managed rollback holds the shared selector lock through restore"
}

test_concurrent_apply_is_rejected_by_operation_lock() {
    local root="$TEST_TMP/concurrent-lock"
    new_root "$root"
    local bin="$root/test-bin" marker="$root/first-at-commit"
    mkdir -p "$bin"
    # shellcheck disable=SC2016 # 아래 문자열은 실행 시 생성될 독립 wrapper의 본문이다.
    printf '%s\n' \
        '#!/bin/bash' \
        ': > "$PROVISION_TEST_COMMIT_MARKER"' \
        'sleep 2' \
        'exec /usr/bin/mv "$@"' > "$bin/mv"
    chmod +x "$bin/mv"
    PATH="$bin:$PATH" PROVISION_TEST_COMMIT_MARKER="$marker" \
        PROVISION_ROOT="$root" PROVISION_TOPOLOGY_HELPER="$HELPER" \
        "$SCRIPT" apply --ip 192.168.0.101/24 --factory-topology peer-route \
        > "$root/first.log" 2>&1 &
    local first_pid=$! tries=0
    while [ ! -e "$marker" ] && [ "$tries" -lt 100 ]; do
        sleep 0.05
        tries=$((tries + 1))
    done
    [ -e "$marker" ] || { wait "$first_pid" || true; fail "first apply did not reach commit"; }
    local second_rc=0 first_rc=0
    run_provision "$root" apply --ip 192.168.0.102/24 --factory-topology eth0-ip \
        > "$root/second.log" 2>&1 || second_rc=$?
    wait "$first_pid" || first_rc=$?
    assert_eq "0" "$first_rc" "first locked apply failed"
    [ "$second_rc" -ne 0 ] || fail "concurrent apply was not rejected"
    grep -q '다른 프로비저닝 작업' "$root/second.log" \
        || fail "concurrent rejection did not identify operation lock"
    pass "operation lock rejects a concurrent apply"
}

test_external_selector_update_aborts_without_clobbering_it() {
    local root="$TEST_TMP/external-selector-update"
    new_root "$root"
    local selector="$root/usr/local/etc/wifi_factory_topology.json"
    cp "$root/etc/systemd/network/20-mlan0.network" "$root/network.before"
    local apply_rc=0
    PROVISION_ROOT="$root" \
        PROVISION_TOPOLOGY_HELPER="$FAIL_AFTER_SET_HELPER" \
        PROVISION_TEST_REAL_HELPER="$HELPER" \
        PROVISION_TEST_FAIL_PATH="$selector" \
        PROVISION_TEST_EXTERNAL_TOPOLOGY=eth0-ip \
        PROVISION_TEST_SNAPSHOT_COUNTER="$root/snapshot.count" \
        "$SCRIPT" apply --ip 192.168.0.101/24 --factory-topology peer-route \
        > "$root/apply.log" 2>&1 || apply_rc=$?
    [ "$apply_rc" -ne 0 ] || fail "external selector update was reported as successful apply"
    assert_file_eq "$root/network.before" "$root/etc/systemd/network/20-mlan0.network" \
        "external selector update did not roll back owned network change"
    assert_eq "eth0-ip" "$(jq -r .topology "$selector")" \
        "rollback clobbered the external selector update"
    grep -q '외부 변경' "$root/apply.log" \
        || fail "external selector update failure was not explicit"
    pass "generation mismatch rolls back owned files without clobbering external selector"
}

[ -f "$HELPER" ] || fail "topology helper not ready: $HELPER"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

test_no_flag_does_not_require_helper_or_touch_selector
test_bad_value_and_missing_helper_fail_before_active_write
test_create_change_noop_and_metadata
test_corrupt_and_unsafe_selectors_fail_without_action
test_unsafe_shared_selector_lock_fails_before_active_write
test_profile_and_factory_topology_are_independent
test_verify_is_read_only_and_rejects_invalid_selector
test_rollback_restores_absent_and_present_selector_states
test_legacy_tar_does_not_modify_selector
test_mid_apply_failure_rolls_back_files_and_selector
test_visible_selector_failure_rolls_back_whole_apply
test_cooperative_writer_waits_through_post_visible_set_failure_rollback
test_cooperative_writer_waits_through_mid_file_failure_rollback
test_same_second_backups_are_unique_and_private
test_auto_rollback_ignores_incomplete_orphan
test_managed_rollback_holds_shared_selector_lock
test_concurrent_apply_is_rejected_by_operation_lock
test_external_selector_update_aborts_without_clobbering_it

echo "RESULT: $PASS_COUNT factory-topology scenarios passed"
