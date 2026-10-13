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

Each persisted resource admits one unresolved transaction across all UDP source ports. Password and committed IP-list candidates remain private until completion. UNCHANGED retains the prior published state; either visible replacement outcome publishes the candidate. A partial-IP validation failure keeps its immediate error response, while its prefix commit retains ownership and reconciles without a second ACK.

Trailing IP staging can depend on a prefix committed earlier in the same request. It remains private with that commit and publishes only when the prefix becomes visible. Logout or fresh Login invalidates this trailing cycle through the session generation, so a late completion cannot revive an earlier session's staging. Pre-rename failure cannot leak failed entries into a later END request.

Radio applies live before persistence. On UNCHANGED, completion attempts restoration before releasing the resource gate, without waiting for another UDP datagram. Restoration requires a successfully applied, valid baseline covering every interface changed by the candidate. Startup file contents alone are not a verified live baseline; an old SINGLE configuration cannot restore WLAN#2 changed by DUAL. Unknown baselines and failed restoration retain explicit degraded recovery state rather than claiming success. REPLACED_UNSYNCED retains the live, visible candidate as uncommitted so retry can persist it again.

Graceful shutdown stops writer admission, joins submitted jobs and harvests/reconciles their results while the platform and UDP socket remain alive, then destroys the writer. This can wait on blocked storage; it does not guarantee bounded shutdown or recovery after SIGKILL.

## Verification and limits

`test_store_atomic` uses the real filesystem and worker with injected file-fsync, rename, close and directory-fsync failures. It checks exact visible bytes, candidate cleanup, result/errno/outcome propagation, private file mode and relative-path directory synchronization. The old implementation fails the parent-sync behavior tests.

`test_persist_failure` exercises the real handler, worker and writer with injected fsync/rename failures, synchronous fallback, resource ownership across ports, completion-triggered radio recovery and degraded cases. IP-chain regressions cover failed prefix resurrection, partial-error writes, successful trailing-cycle publication and Logout/fresh Login before completion. DFK ChangeIP and Logout retain the mlan0 plane in peer-route mode. Host sanitizer checks do not qualify physical power loss or the target's full topology.

A verified snapshot of every affected interface/helper-managed file, a WAL/recovery journal, cross-file radio transaction, device-cache flush support and physical power-cut behavior remain unverified. Client recovery after an NG with a visible password replacement also needs an explicit contract; blindly retrying with the previous password can yield a password-mismatch response. [Linux writeback cache-control](https://docs.kernel.org/block/writeback_cache_control.html)
