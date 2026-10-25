# Local persistence outcomes (issue 144)

The writer fsyncs the candidate file, closes it, renames it over the target, then fsyncs the target parent directory. A successful file fsync alone does not confirm durability of the renamed directory entry. [Linux fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html)

`opc_store_write_atomic_ex` reports the replacement state even on error; async completions carry the same outcome alongside result and saved_errno:

| Outcome | Visible target | Result |
|---------|----------------|--------|
| UNCHANGED | Existing target not replaced by this operation | Failure before rename |
| REPLACED_UNSYNCED | New bytes already visible | Post-rename confirmation failed |
| DURABLE | New bytes installed | File and directory synchronization confirmed |

This is a local Linux-filesystem contract. Network filesystem rename ambiguity is outside it. A failed directory fsync does not undo a successful rename; a memory-only rollback would therefore risk divergence from the visible file. [Linux rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html)

The parent handle is opened before creating the temporary candidate. Pre-rename failures preserve the target and remove the candidate. File-close failure is not retried on Linux because the fd can already have been released and reused. [Linux close(2)](https://man7.org/linux/man-pages/man2/close.2.html)

The legacy int API remains for existing callers. Its -1 must never be interpreted as proof that old bytes survived; reconciliation callers use the extended outcome. Genuine write failures still return NVRAM NG, including a visible replacement whose durability could not be confirmed. The response therefore does not imply that the requested value stayed unchanged.

## Publication and recovery

The IP list is stored as the host specification's configuration-file text (issue 194, `iplist_text.h`), whose length is known only after the commit. Admission therefore reserves the largest text, 24 KiB, and the commit submits its actual length, which may not exceed that reservation. The reply follows opcd's own copy; once that copy is published, the same text is queued for the customer file as a log-only write, so a failure there leaves the published list and the reply unchanged. That write also records the bytes it put in the customer file (`iplist.user.last`) once the file shows them. At the next start a customer file equal to that record has not been replaced by the customer, so opcd's own copy stays in effect; a commit whose customer-file write failed is therefore not undone by the older file.

Each persisted resource admits one unresolved transaction across all UDP source ports. Password and committed IP-list candidates remain private until completion. UNCHANGED retains the prior published state; either visible replacement outcome publishes the candidate. A partial-IP validation failure keeps its immediate error response, while its prefix commit retains ownership and reconciles without a second ACK.

Trailing IP staging can depend on a prefix committed earlier in the same request. It remains private with that commit and publishes only when the prefix becomes visible. Logout or fresh Login invalidates this trailing cycle through the session generation, so a late completion cannot revive an earlier session's staging. Pre-rename failure cannot leak failed entries into a later END request.

Radio applies live before persistence. On UNCHANGED, completion attempts restoration before releasing the resource gate, without waiting for another UDP datagram. Restoration requires a successfully applied, valid baseline covering every interface changed by the candidate. Startup file contents alone are not a verified live baseline; an old SINGLE configuration cannot restore WLAN#2 changed by DUAL. Unknown baselines and failed restoration retain explicit degraded recovery state rather than claiming success. REPLACED_UNSYNCED retains the live, visible candidate as uncommitted so retry can persist it again.

After an NVRAM NG the client re-sends the same request, and the host communication specification has a split IP list re-sent from its START. The re-send writes the value again and converges to OK once the write is durable. Re-sending only the failed END depends on an outcome the client cannot see: after UNCHANGED the cycle opened by the earlier START is still open and the END commits, but after REPLACED_UNSYNCED that END already closed the cycle, so a lone END answers 0x0018 (no open cycle) although the new list is live. A password re-send still names the replaced password as its old password, so opcd keeps that value while the live password comes from a REPLACED_UNSYNCED write. It accepts a re-send whose old password is that value and whose new password is the live one. The next durable password write clears it, after which the old password is a mismatch again.

Graceful shutdown stops writer admission, joins submitted jobs and harvests/reconciles their results while the platform and UDP socket remain alive, then destroys the writer. This can wait on blocked storage; it does not guarantee bounded shutdown or recovery after SIGKILL.

## Radio cross-file window (issue 178)

SetRadioConfig changes files in two places. The wlan-package helper `opc_wlan_apply.sh` writes the new wpa_supplicant configuration to a temporary file, syncs it, renames it over the configuration, syncs the file and its directory, and only then runs `wpa_cli reconfigure`; mode and bandwidth go to `wifi_init_conf.json` after the reconfigure. opcd submits `radio.conf` only after that apply succeeded. After an abrupt power loss the files can therefore diverge in one direction only: a new wpa_supplicant configuration with the old `radio.conf` (and possibly the old JSON). The window runs from the helper's directory sync to the directory fsync of `radio.conf`.

opcd does not compare the files at start. It reads `radio.conf` as the committed configuration, with no verified live baseline (`radio_applied_known` stays false). What follows depends on the client:

- If it re-sends the new configuration, which it never saw acknowledged, the request differs from `radio.conf` and is applied and stored, so the files converge.
- If it sends the old configuration, equal to `radio.conf`, the equal-configuration branch answers OK without applying. The device keeps running the new wpa_supplicant configuration while GetDeviceInfo reports the old SCAN band and list.

The same state can arise without power loss: if the first apply after a start is followed by a `radio.conf` write that fails before rename (UNCHANGED), no baseline is known, no restoration is armed, and the request answers NVRAM NG while the new configuration stays live.

Decision (2026-10-20): this window is accepted and documented here. A WAL would not close it, because the wpa_supplicant configuration is replaced before `radio.conf` is submitted. Re-applying the first equal request after every start was rejected: if a client re-sends its configuration at start, every boot would drop the link for a reconfigure. Comparing the files at start was rejected for its parsing surface (band and channel list from the wpa_supplicant configuration, plus the mode/bandwidth JSON). If a power-cut test or a field report shows the window matters, the planned fix is a transaction marker: written and fsynced just before the apply, removed once `radio.conf` is DURABLE. A marker found at start makes opcd start with `radio_committed` false, so the next equal request takes the normal apply-and-store path; a marker that survives wrongly costs one extra reapply.

## Verification and limits

`test_store_atomic` uses the real filesystem and worker with injected file-fsync, rename, close and directory-fsync failures. It checks exact visible bytes, candidate cleanup, result/errno/outcome propagation, private file mode and relative-path directory synchronization. The old implementation fails the parent-sync behavior tests.

`test_persist_failure` exercises the real handler, worker and writer with injected fsync/rename failures, synchronous fallback, resource ownership across ports, completion-triggered radio recovery and degraded cases. IP-chain regressions cover failed prefix resurrection, partial-error writes, successful trailing-cycle publication and Logout/fresh Login before completion. Re-sends after REPLACED_UNSYNCED converge to OK for password (writer thread and synchronous fallback), single-frame IP lists, split IP lists re-sent from their START, and radio. DFK ChangeIP and Logout retain the mlan0 plane in peer-route mode. Host sanitizer checks do not qualify physical power loss or the target's full topology.

A verified snapshot of every affected interface/helper-managed file, a WAL/recovery journal, cross-file radio transaction, device-cache flush support and physical power-cut behavior remain unverified. The replaced password is remembered in memory only: Login always takes the live password, so a client logged out before its re-send logs in with the new one, and an opcd restart forgets the replaced value so the re-send becomes a password mismatch. [Linux writeback cache-control](https://docs.kernel.org/block/writeback_cache_control.html)
