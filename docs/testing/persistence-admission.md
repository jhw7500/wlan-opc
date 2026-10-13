# Persistence admission under queue saturation (issue 144)

The async writer has 16 fixed slots. SetPassword, SetIpConfigList and changed SetRadioConfig requests reserve a validated path/length and check ACK capacity before changing password, list staging/committed state, or live radio settings. Completed jobs are harvested once without waiting before admission is retried.

When capacity is still full, the request remains unaccepted: no settings change, no pending ACK and no NVRAM NG response. The client retries on its protocol timeout. There is no extra server request queue; a retry can therefore wait the configured NVRAM response timeout (120 seconds). An accepted retry uses its own sequence number. Each persisted resource also admits only one unresolved transaction across all source ports. Requests for that resource remain unaccepted until completion and reconciliation; the accepted request's original endpoint and sequence number receive the eventual response.

A reservation is submitted or canceled within the current dispatch. Radio apply failure, IP requests that never commit and other non-submitting exits release it. Reserved jobs are not worker jobs; graceful shutdown drains only submitted data as before. A partially committing IP-list frame still answers its earlier validation error immediately, with its prefix write already guaranteed a slot and its completion logged without a second ACK.

This change removes the former 500ms completion wait and false NVRAM failure caused by transient queue pressure. Genuine write failures still return NVRAM NG. Synchronous fallback when writer creation fails can still block on storage. [Persistence outcomes](persistence-outcomes.md) describes parent-directory synchronization, publication and bounded radio recovery. A complete platform snapshot, WAL and abrupt-power-loss qualification remain open under issue 144.

## Verification

`test_persist_admission` runs the real handler, worker and atomic writer with only fsync held at the storage boundary. It saturates every slot, checks prompt silent rejection with no state/live-radio mutation, verifies unrelated queries still work, then releases storage and verifies successful retry, single deferred ACK, original endpoint/sequence and duplicate suppression. Additional cases exercise reservation release for radio apply failure and IP staging without a commit.

`test_store_async` exercises reservation capacity, cancellation, argument validation and one-time submission alongside its existing copied-data/FIFO/write-error/graceful-drain cases. `make check` is the complete host gate; ARM64 NXP compilation is separate from runtime hardware qualification.
