# wlan-opc / Manual Run-through Checklist

This is the 1st-stage acceptance checklist, ticked once on an actual ARM64
target build (NXP88W9098 + i.MX8MM) before the work is considered Phase 5
complete. Until Phase 3 lands, sections marked `[Phase ≥ 3]` are placeholders.

## 0. Build & install

- [ ] `cd wlan-opc && make` produces `build/arm64/{protocol/libopcproto.a, opcd/opcd, vhlctl/vhlctl}` with **no warnings** (ARM64 cross via `aarch64-linux-gnu-*`). `make both` additionally builds the host (`build/native/`) tree alongside it
- [ ] `wlan-package/build.sh` stages outputs into `dist/usr/local/opc/{bin/opcd,bin/vhlctl,etc/...,opcd.service}` and emits a `.deb` (Phase 4)
- [ ] `file dist/usr/local/opc/bin/opcd` reports `ELF 64-bit LSB ... ARM aarch64`
- [ ] `dpkg -i wlan-proc_*.deb` on the target succeeds
- [ ] `/etc/systemd/system/opcd.service` exists as a symlink to `/usr/local/opc/opcd.service` (created by postinst + `daemon-reload`)
- [ ] By design the package does **not** auto-enable opcd (postinst policy: "1차 단계 자동 enable 금지 — 운영자 결정"). Operator runs `systemctl enable opcd`, after which `systemctl is-enabled opcd` returns `enabled`
- [ ] `systemctl start opcd && systemctl is-active opcd` returns `active`

## 1. Pre-Login [Phase ≥ 2]

- [ ] `vhlctl --host <opc-ip> basic-info` returns Vendor / Product / Device Status = `0x01` (Ready)
- [ ] `vhlctl device-info` without Login returns NG with Error Cause = `0x0001` (Login violation)

## 2. Login & exclusion [Phase ≥ 2]

- [ ] `vhlctl login --password <secret>` returns OK
- [ ] `basic-info` now reports Device Status = `0x02` (Logged-in)
- [ ] From a second host, `vhlctl login` returns NG with Error Cause = `0x0002` (Login condition violation)
- [ ] After 5 minutes of idle from the holder, `device-info` returns NG with Error Cause = `0x0001` (auto-logout)

## 3. Configuration writes [Phase ≥ 2]

- [ ] `set-password --old <secret> --new <newsecret>` returns OK and survives `systemctl restart opcd`
- [ ] `set-ip-list --file iplist.cfg` (covering all 128 slots in multiple requests) commits **only** when the boundary-end flag is seen — verified by `stat /usr/local/opc/etc/iplist.cfg`
- [ ] Sending `change-ip --slot 25` **before** the boundary-end flag returns NG with Error Cause = `0x0012` (conflict)
- [ ] `change-ip --slot 25` followed by `logout` applies the new IP **after** Logout response is sent (verify with `ip addr` on the target post-logout)
- [ ] Power-cycle the target — IP returns to the value stored in `iplist.cfg` (the change-ip change is volatile, as spec'd)
- [ ] `set-radio --station single --w1-freq 5180 --w1-ch 0x0224 --w1-mode 11 --w1-bw 2` returns OK and persists across reboot

## 4. Indications [Phase ≥ 2]

- [ ] `set-indication --bits 0x85 --period 5 --to <vhl-ip>:9999` returns OK
- [ ] `vhlctl listen --bind 0.0.0.0:9999` decodes the InitComplete `0x00 → 0x01 → 0x02` boot sequence after the next `opcd` restart
- [ ] KeepAlive (`0x0080`) arrives every ~5 s when Period=5; arrives never when Period=0
- [ ] While indications are enabled, `device-info` returns NG with Error Cause = `0x0010` (indication-setting violation)
- [ ] Manual disconnect from the AP triggers WlanStatusChange (`0x0002`) with the right CH
- [ ] AP-initiated disassoc emits ApDisconnect (`0x0008`) with the AP MAC

## 5. Reset path [Phase ≥ 2]

- [ ] `reset` returns OK
- [ ] `opcd` exits and systemd brings it back up within `RestartSec` (default 2 s)
- [ ] If indications were enabled before reset, ResetNotice (`0x0020`) is emitted before the daemon goes down
- [ ] After restart, indication settings are gone (volatile) — must be re-set

## 6. Package lifecycle

- [ ] `dpkg --purge wlan-proc` removes `/usr/local/opc/**` and `/etc/systemd/system/opcd.service`
- [ ] `find /usr/local/opc -mindepth 1 2>/dev/null` returns nothing
- [ ] `systemctl list-unit-files | grep opcd` returns nothing

## 7. Sign-off

- [ ] Date: ____________________
- [ ] Hardware: NXP88W9098 + i.MX8MM ARM64, FW ____________________
- [ ] Tested by: ____________________
- [ ] Notes / deviations: ____________________
