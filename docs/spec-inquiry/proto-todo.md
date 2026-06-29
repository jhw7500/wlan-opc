# wlan-opc / Protocol TODOs and Spec Ambiguities

Tracked spec questions, deferred decisions, and call-site back-references.
Each entry should be linked from a `// TODO(proto-todo:<id>): ...` comment in
the source so we can grep both ways.

## T1. Default UDP port — RESOLVED (2026-06-12, user decision)

- **Spec**: TBD (section 3.1.2)
- **Our default**: `50607`, overridable via `/usr/local/opc/etc/opc.conf`
- **Resolution (2026-06-12)**: user decision — a default in the dynamic/private port range
  that nothing else uses, plus the config override, is sufficient; no vendor confirmation
  needed. Indications need no port decision of their own: the recipient IP **and port**
  arrive on the wire in every SetIndication request
  (`opcd_state_t::indication_recipient_port`), so the VHL picks its own listening port at
  runtime.
- **Call sites**: opcd config loader, vhlctl default `--port`

## T2. List Boundary Flag values — ✅ RESOLVED by DFK (2026-06-29, page-22: 開始0x0001 / 継続0x0000 / 完了0x0002)

- **이 값은 우리가 정의하는 게 아니다 — VHL이 보내고 opcd가 *수신·해석*하는 필드.** VHL이
  SetIPConfigList 요청에 담아 보내는 `boundary_flag`를 `opcd/handler.c:709-730`이 읽어 START면
  staging 시작·END면 atomic commit. pack/unpack은 `commands.c:410/423`(값 그대로 전송), 우리
  테스트 송신은 `vhlctl/vhlctl.c:340-342`. 따라서 **어느 값이 START인지는 발주처(VHL 송신측)가
  정하는 항목**이고, 우리는 그 값을 추측해 코드에 박아둔 것뿐 — 자체 확정 권한이 없다.
- **Spec inconsistency**: page 22 body `start=0x0001 / continue=0x0000 / end=0x0002 / start+end=0x0003`
  vs page 24 field description `start=0x0000 / continue=0x0001 / end=0x0002` — 원본 docx에 실재.
- **우리는 줄곧 미확정이었다**: `seed.yaml:36`이 처음부터 *"follow spec page 22 (start=0x0001 …);
  **p.24 inconsistency logged as TODO with vendor follow-up**"* — page-22로 출발하되 벤더 확인이
  필요한 TODO로 명시. 2026-06-11에 "벤더 **구두** 확인 + 원본 2/3 근거"라며 page-24(start=0x0000)로
  추측 변경·코드 반영했으나, 그 구두확인은 서면 근거가 없었다.
- **✅ DFK 서면 확정 (2026-06-29, PPTX 슬라이드10 "기타1")** — 일본어 원본:
  **"仕様書を更新し、開始（0x0001）、継続（0x0000）、完了（0x0002）に統一します"**.
  → **page-22 확정**: 開始/start=**0x0001**, 継続/continue=**0x0000**, 完了/end=**0x0002**.
  ※ 한국어 PPTX의 "접속(0x0000)"은 **`継続`(けいぞく=continue)의 오역** — 가운데값이 continue임이
  일본어로 명확하므로 별도 재확인 불필요. seed.yaml의 원래 page-22와 일치.
- **✅ Code (page-22 반영 완료, 2026-06-29)**: `protocol/commands.h`의 `OPC_LIST_BOUNDARY_START`=**0x0001**,
  `OPC_LIST_BOUNDARY_CONTINUE`=**0x0000**, `END`=0x0002로 flip(daemon을 VHL 송신측에 정렬). pack/unpack은
  값 그대로 무변경, staging 분기·`vhlctl.c`·테스트는 상수 참조로 자동 정합. `test_codec.c`에 와이어 절대값
  (0x0001/0x0000/0x0002) 검증 추가(상수 flip이 조용히 통과 못 하도록). `commands.h` 주석(page-22 채택
  사유)·spec-conformance D6 갱신. START_END(0x0003) 단일프레임 커밋은 DFK 답변에 없음 → 현행대로 미지원
  (START→END 2프레임). `make check` 통과.
- **잔여**: 갱신 사양서 수령 시 page-22 표기 최종 대조(상호운용 리스크 낮음 — DFK 서면+일본어로 확정).
- **Historical note**: page-22(seed 초기) → 2026-06-11 page-24(구두추측) → 2026-06-29 DFK 서면 page-22 복귀.

## T3. Header `length` field vs UDP payload — RESOLVED (original docx figures)

- **Spec text**: "Length max 1416 B" AND "UDP payload max 1424 B" with a 64-byte-header diagram (그림 3-6).
- **Resolution**: the original docx byte-map figures are authoritative and self-consistent. Common header = **64 bytes** (8-byte fixed part + 56-byte Reserve at bytes 8..63); body starts at **offset 64**; `Length = total_frame - 8` (= Reserve 56 + payload). Then payload max 1360 -> frame/UDP max 1424 -> Length max 1416, all matching the spec text. The earlier "64 + 1416 = 1480 > 1424" contradiction was a misreading: Length is `total - 8` (reserve + payload), NOT the body length.
- **Our choice**: `OPC_FIXED_HEADER_SIZE = 8`, `OPC_HEADER_SIZE = 64`, `OPC_PAYLOAD_MAX = 1360`, `OPC_FRAME_MAX = 1424` (`protocol/proto.h`, static_assert-pinned).
- **Call sites**: `protocol/proto.h`, `protocol/frame.c`, recv loop in opcd
- **Status**: resolved — header 64 B / `Length = total - 8` confirmed by the original docx figures (그림 3-6 + per-command formats). Code refactored M1->M2; spec.md / seed.yaml updated to match.
- **History**: a transient 60-byte / `total - 4` reading (the `.md` reconstruction's "60 ┘" reserve marker mistaken for the body offset) was implemented and then reverted once the docx figures showed the Reserve ending at byte 63 (its last row labelled 60) with the body at 64.

## T4. Password storage format — RESOLVED (2026-06-12, user decision: plaintext stays)

- **Spec**: silent (only error codes for "invalid characters" and "NULL termination")
- **Our choice**: **plain text**, file mode `0600`, owned by root (or the opcd service user)
- **Resolution (2026-06-12)**: user decision — plaintext storage is final. The wire protocol
  itself carries the password in clear UDP, so at-rest hashing would only defend a
  disk/backup leak; the trusted-L2 premise (SECURITY.md) plus mode 0600 covers that
  surface. The real operational risk stays the default password (`MyPassword`) —
  change-on-first-deploy remains mandatory.
- **Call sites**: opcd password store, SetPassword handler, vhlctl `set-password` packer

## T5. IEEE 802.11r / 11ai / 11k / 11v support flags

- **Spec**: GetDeviceInformation response carries one byte each (`0x00` unsupported / `0x01` supported)
- **Current**: caps load from `etc/device_info.json` at startup (`opcd/inventory.c`) so
  operators can edit them without rebuilding — defaults all `1`. (The earlier
  "platform_hook stub emits 0" description is obsolete.)
- **Call sites**: `opcd/inventory.c`, GetDeviceInformation builder
- **Status (2026-06-12)**: to be finalized via **customer inquiry** (user decision) —
  registered in issue #35. The JSON values need an authoritative source before sign-off.
- **Resolve when**: customer/vendor confirms which subset the NXP88W9098 silicon advertises

## T6. FaultDetect (0x0010) congestion thresholds — INTERIM (2026-06-12, PR #39)

- **Spec**: enumerates `CPU / Memory / Disk-I/O / Network-I/O` congestion IDs but leaves thresholds open
- **Current (PR #39 — interim policy, every value provisional pending issue #35)**: polling
  probe implemented. 80% threshold for every resource (`opc.conf` `congestion_*` keys
  override), one sample per indication period, re-notify each period while congestion
  persists, Current Val units CPU/Disk=% Network=Mbps. Memory (0x0002) is **not emitted** —
  the target is swapless so the spec's paging definition cannot hold; Disk-I/O (0x0003)
  covers it.
- **Call sites**: `opcd/fault_probe.c::opcd_fault_probe_sample`, `opcd/indication.c::opcd_ind_tick`, opc.conf loader
- **Resolve when**: 발주처 confirms per-resource thresholds / units / persistence /
  re-notify policy / Memory interpretation (issue #35, 5-item inquiry table)
- **⚠️ DFK answer (2026-06-29, PPTX 260618 slide 10 "Indication 1")**: "값으로는 사용률을
  0-100%로 CurrentVal로 설정하십시오 … 임계치는 각 벤더의 성능으로서 동작 불안정이 될 수 있는
  기준으로 정의" — i.e. (a) **thresholds are delegated to the vendor (us)** → our 80% policy
  stands (confirmed), but (b) **Current Val unit must be utilization % (0-100) for *every*
  resource**, including Network I/O which we currently emit as **Mbps**. The % is already computed
  in `fault_probe.c` (`net_over` = link-capacity %); only the reported Current Val field changes.
  **✅ DONE (2026-06-29)**: Network I/O Current Val changed Mbps → utilisation-% (`fault_probe.c`
  `net_pct` = mbps*100/link, 0-100% clamp; `indication.c`; `net_over` unchanged — proven equivalent
  by code-review). `test_fault_probe` adds %/clamp cases; `make check` passes. DFK will add the
  0-100% Current Val definition to the spec.

## T7. Vendor maximum response time — RESOLVED (2026-06-12, user decision: log actuals)

- **Spec**: "If the regulation response time cannot be met, vendor provides the max response time per radio model"
- **1st-stage**: we use the regulation timers (1 s / 2 min) and a Result=NG fast-path on overload
- **Resolution (2026-06-12)**: user decision — don't block on the vendor matrix; log the
  actual per-request service time instead. opcd now logs
  `req 0x%04X seq=%u served in N.NNN ms` for every wire response — sync path in the
  opcd.c main loop, deferred NVRAM acks in `handler.c::opcd_store_async_on_ready()`
  (measured from request receipt, `(deferred)` suffix). If a vendor matrix ever arrives,
  compare it against these logs.
- **Call sites**: opcd.c recv loop (T7 comment), handler.c::persist_blob / opcd_store_async_on_ready

## T8. Reset (0x2001) re-init mechanism

- **Spec**: "after ack send, the device resets"
- **Our choice**: opcd `exit(0)` after sending the Acknowledgment; systemd `Restart=always` brings it back up
- **Call sites**: Reset handler, opcd.service
- **Resolve when**: never (this is a decided constraint, kept here for traceability)

## T9. ResetNotice (0x0020) trigger sources — DEFERRED (발주처 확인 대기, #35)

- **Spec**: §3.4.6 "emitted before the device autonomously resets" — 자율리셋 **전**
  통지만 규정하고, **무엇이 자율리셋을 트리거하는지는 미정의**.
- **Status (2026-06-16, user decision)**: producer 배관(emitter / `OPCD_PEVT_RESET_NOTICE`
  enum / `on_platform_event` consumer)은 완비됐으나 트리거 정책(자원 폭주 지속?
  watchdog 타임아웃? 임계·주기?)이 사양에 없어 producer 미구현 — 발주처 확인 대기로
  **issue #35 항목 5** 등록. 명시적 Reset(0x2001) 경로의 ResetNotice(cause=USER) 발행은
  그대로 동작.
- **Call sites**: `opcd/indication.c::opcd_ind_reset_notice` (emitter),
  `opcd/handler.c::handle_reset` (sole producer today). Platform event plumbing is ready —
  `OPCD_PEVT_RESET_NOTICE` (`opcd/platform.h`) + consumer (`opcd/opcd.c::on_platform_event`) —
  but no platform backend produces the event yet (`nxp_drain_events` is a no-op).
- **Resolve when**: 발주처가 (a) 자율리셋 트리거 조건(예: 자원 폭주 지속시간 / watchdog
  타임아웃)과 (b) 신규 reset cause 값(현재 `OPC_RESET_CAUSE_USER=0x01`만)을 확정 (#35).
  배관 완비 — 정책 확정 시 producer만 추가하면 됨.

## T10. Header `Length` field — per-command values — RESOLVED (original docx figures)

- **Rule**: `Length = total_frame - 8` (8-byte fixed header excluded; counts Reserve 56 + payload). Equivalently `Length = payload + 56`. Worked examples:
  - Login Req payload 128 -> frame 192 -> Length 184
  - Login Ack payload 4 -> frame 68 -> Length 60
  - GetBasicInfo Ack payload 16 -> frame 80 -> Length 72
  - GetDeviceInfo Ack payload 352 -> frame 416 -> Length 408
- **Empty-body requests**: Logout / GetBasicInfo / GetDeviceInfo / Reset transmit ONLY the 8-byte fixed header (no Reserve), so `Length = 8 - 8 = 0` falls straight out of the rule — NOT an exception. (Reset Ack's spec "0" was a separate typo; see T11, now 60.)
- **Our choice**: body-bearing frames use `opc_frame_build` (64-byte header + payload at offset 64); empty requests use `opc_empty_frame_build` (8 bytes). Per-command `OPC_*_LENGTH` macros carry the literal value. Receivers validate **frame size**, not Length (`opc_frame_parse` requires >= 8 B and slices body = frame_len - 64 when present).
- **Call sites**: `protocol/commands.h` per-command `*_LENGTH` macros, `protocol/frame.c::opc_frame_build` / `opc_empty_frame_build`
- **Status**: resolved — `total - 8` with empty requests = 8-byte frames, confirmed by the docx figures. With T11 (Reset Ack = 60) the rule has **zero exceptions**.

## T11. Reset Ack `Length` — RESOLVED (spec typo, 60 adopted)

- **Spec text**: "응답 포맷 (Length=0 ※헤더상 Length=0, Result/Error Cause 포함)" — the spec writes Length=0 while explicitly stating the Result+ErrorCause body IS present.
- **Why it is a typo**: (1) the line directly above is the Reset *Request* `(Length=0)`, legitimately empty — the `0` looks copy-pasted into the response box; (2) every other Ack with the same 4-byte Result/ErrorCause body uses Length 60; (3) setting it to 60 removes the **only** remaining exception, making the Length rule fully uniform (`no body -> 0`, `body -> payload+56`).
- **Our choice**: treat the spec `0` as a typo and emit **Length=60** like every other simple Ack (`OPC_RESET_ACK_LENGTH = 60`). Parsing is unaffected either way (receivers slice the body by frame size, not by Length).
- **Call sites**: `protocol/commands.h::OPC_RESET_ACK_LENGTH`, `protocol/commands.c::opc_reset_ack_pack`, `protocol/tests/test_codec.c::test_reset`
- **Status**: resolved — 60 adopted (user decision). If a real device is ever observed emitting 0, revert this single macro.

## T12. GetDeviceInfo Ack reserve area trailing length — RESOLVED

- **Spec figure**: the response payload ends around offset 412 with a trailing reserve block.
- **Under M2 (64-byte header, `Length = total - 8`)**: Length 408 -> frame 416 -> payload = 416 - 64 = 352 (offset 64..415). The trailing reserve runs to byte 415 (40 bytes); the figure's "412" label is one row short.
- **Our choice**: trust Length=408, emit a 352-byte payload (`OPC_GET_DEVICE_INFO_ACK_BODY_LEN`) with a 40-byte trailing reserve.
- **Call sites**: `protocol/commands.c::opc_get_device_info_ack_pack`
- **Status**: resolved under the 64-byte-header / `total - 8` model; payload = 352 B.

## T13. SetRadioConfig (0x1004) — WLAN#2 FREQ/CH order reversed in spec — RESOLVED

- **Spec text**: the Korean markdown transcription (`opc_vhl_protocol_Rev1.00_KO.md`) showed
  WLAN#1 "FREQ then CH" but WLAN#2 "CH then FREQ".
- **Resolution (2026-06-11)**: checked the original docx
  (`무선기판공통제어통신사양서_Rev1.00_KO.docx`) — the §3.3.8 request-format figure
  (image33.emf) shows **WLAN#2 FREQ then CH**, identical to WLAN#1 and to the
  GetDeviceInfo layout. The reversal was a transcription error introduced while
  converting the docx to markdown, not a spec inconsistency. The markdown has been
  corrected in place (76-byte row + correction note).
- **Code**: already correct — `opc_set_radio_config_req_pack/unpack` packs both WLANs
  uniformly FREQ→CH (`protocol/commands.c:541-543`). No code change needed.
  (Historical note: an earlier revision of this entry said "swap inside the codec";
  the code was later unified to FREQ→CH and this entry had gone stale.)

## T14. Roaming Indication (0x0004) — CH Number offset row table mis-aligned — RESOLVED

- **Spec text (as transcribed)**: "64 SNR|RSSI / 68 Connect AP MAC (6B) / 72 CH Number" — read
  literally, the "72 CH Number" row appeared to overlap the 6-byte MAC.
- **Resolution (2026-06-11)**: checked the original docx figure (image42.emf) — the MAC(6Byte)
  cell at row 68 is vertically merged into the **left half of row 72 (bytes 72..73)**, and
  CH Number is an independent cell in the **right half (bytes 74..75)**. No overlap exists in
  the original; the apparent mis-alignment was an artifact of flattening the merged-cell figure
  into one text row per offset. The markdown transcription has been corrected
  ("72 (MAC 계속) | CH Number (74~75)"). Same pattern fixed in §3.4.4 ApDisconnect (image44).
- **Code**: already correct — SNR(1)+RSSI(1)+reserve(2) at body 0..3, MAC(6) at 4..9,
  CH Number(2) at body 10..11 (= frame 74..75), Length=68
  (`protocol/indications.c::opc_ind_roaming_pack/unpack`). No change needed.

## T15. Reset (0x2001) Ack — figure says Length=0 but the frame carries Reserve+Result (68B) — RESOLVED (2026-06-12, user decision: 60)

- **Spec figure (original docx, image38.emf — confirmed 2026-06-11)**: the Reset response
  figure marks **Length=0** while simultaneously drawing Reserve 8..63 and Result|Error Cause
  64..67 — i.e. a 68-byte frame whose Length field would be 60 under the universal
  Length = total − 8 rule (T3/T10). The figure is self-contradictory.
- **Our choice**: follow the universal rule — emit Length=60 (`OPC_RESET_ACK_LENGTH`,
  `protocol/commands.h:406`). Consistent with every other simple ack on the wire.
- **Risk**: if the vendor's VHL implements the figure literally (expects Length=0 in the Reset
  ack), it may reject our ack. Conversely our SEC-003 length gate would reject a vendor frame
  that declares Length=0 while carrying 68 bytes.
- **Resolution (2026-06-12)**: user decision — **Length=60 stands**. The figure's 0 is judged
  a copy-paste typo from the Reset *request* (a genuine empty request), consistent with the
  universal Length = total − 8 rule and the ResetNotice figure (§3.4.6, Length=60). Dropped
  from the vendor-confirmation queue; any vendor mismatch will surface naturally at the
  interop test (Reset command exchange fails loudly, not silently).
- **✅ DFK answer (2026-06-29, PPTX slide 10 "기타 3") + user confirmation**: DFK answered the
  "Reset response figure shows Length=0 yet includes Reserve(8~63)+Result/ErrorCause(64~67)"
  contradiction with **"오기입니다. 사양서 갱신시 삭제하겠습니다"** ("it is a typo; will delete on
  spec update"). **User confirmation (2026-06-29): the Length=0 marking is the typo — Length=60 is
  correct.** So "삭제" = delete the erroneous Length=0 marking; the Reset Ack keeps its
  Result/ErrorCause body → **Length=60**. Our code (`OPC_RESET_ACK_LENGTH=60`) is correct as-is —
  **no change**. T11/T15 fully RESOLVED, ambiguity removed; DFK will correct the figure on spec update.

---

## DFK-2026-06-18. 발주처 공식 답변 회신 (PPTX 9~10p) — 미확정 항목 정의

> **출처**: `tmp/DFK 답변기입 _ … (260618).pptx` 슬라이드 9(무선설정 5건)·10(Indication 3 / 에러 3 / 기타 5건).
> DFK가 우리 문의(`tmp/DFK_opcd_protocol_QA.md` / `docs/spec-inquiry/spec-inquiry-letter.md`)에 "→"로 답변을 기입한 회신본.
> **정리 2026-06-29** — *문서 확정 pass*(코드 미변경, 충돌 5건은 TODO로 명시; 사용자 결정).
> **교차참조**: `docs/dfk-meeting/meeting-2026-06-26-dfk-opcd-qa.md`(T#/R#/I#/E#/M#) · `docs/spec-inquiry/spec-conformance.md`(A#/D#/V#).

### 🟢 확정 — DFK 답변이 우리 구현/입장과 일치 (현행 유지)

| QA(PPTX) | DFK 답변 | 우리 현황 | 확정 근거 |
|---|---|---|---|
| 무선1 Dual | "Station type을 single로 설정하면 됨" | Single만 유효 | Single 운용 확정 (R1) |
| 무선2 SetRadio OK | "설정된 것에 대한 Result, **연결까지는 아님**" | OK=설정+적용, 재접속 대기X | §3.3.8 OK 의미 확정 (R2 / D9) |
| 무선3 ESSID 위치 | "건물/플로어 이동 일괄변경용 → IP리스트에 정리" | IP리스트 구현 | 현 위치 유지, 이관 불요 (R3) |
| 무선5 11r/k/v/ai | "**링크상태 아님, 칩 지원여부**" | 정적 capability | 지원여부 해석 확정 (R5 / V5 / T5) |
| 기타2 헤더 64=8+56 | "**YES**" | 8+56, body@64, Len=전체−8 | A8 / T3 / T10 확정 |
| 기타4 자동로그아웃 IP변경 | "**폐기 부탁**" | 명시적 Logout 없으면 폐기 | A12 확정 |
| 기타5 Device Status 0x0 | "**기동~초기화 완료까지**" | pre-READY 부팅구간 | M5 확정 (D11 기각·D15 종결; 패닉은 0x0 범위 아님 — DFK는 별도통지 여부 미언급) |
| 기타3 Reset Ack Length | "오기" (도면 Length=0 표기) | Length=60 (body4+reserve56) | **Length=60 확정** (사용자 2026-06-29: Length=0 표기가 오기, 60이 맞음 — 코드 그대로) — T11/T15 |

### 🔴 코드 변경/정렬 필요 — DFK 답변이 현 구현과 다름 (본 pass는 TODO만)

> 주: 기타1 List Boundary는 "우리 확정을 번복한 충돌"이 아니라 **우리가 미확정이던 수신 해석값을 발주처가 확정**한 것(VHL이 보내는 값 → opcd 해석). 나머지 4건은 현 구현 입장과의 실질 충돌.

| QA(PPTX) | DFK 답변 | 현 구현 | 조치 (코드 미변경) |
|---|---|---|---|
| **기타1 List Boundary** | 開始0x0001/継続0x0000/完了0x0002 (**page-22**, 日원문; 한글 "접속"은 継続 오역) | page-24 → **page-22 정렬** | **✅ 코드 반영 완료** — commands.h flip + 와이어 테스트, make check 통과 |
| **무선4 미접속 필드값** | "무효치 **ALL F(0xFFFF)** 규정 추가" | 미접속 시 **설정값** fallback | **T16** — 미접속 무선필드 0xFFFF 검토 (G11 토글 연계) |
| **Ind1 단위** | "CurrentVal=**사용률 0-100%**" + "임계는 벤더 재량" | Network Mbps → **사용률%** | **✅ 단위 반영 완료**(fault_probe net_pct); 임계 80% 벤더위임=확정 |
| **Ind3 주기내 다중사상** | "주기 끝 **최신 1회 통지**(coalesce)" | 에지 즉시·개별 발행 | **T17** — 이벤트성 indication coalesce 모델 재검토 |
| **에러2 0x0013** | "**0x0013은 오기, 삭제**" | 0x0013 override 제거됨 | **✅ 제거 완료** — 0x0002 회귀(handler/ids.h/vhlctl), make check 통과 |

### 🟡 신규 정의 / 모호 — DFK 사양 갱신 약속 또는 추가 확인 필요

| QA(PPTX) | DFK 답변 | 상태 |
|---|---|---|
| 에러1 무효문자 | 허용: 영숫자 + . - _ + / : = ~ @ (日원본 "使用可能な文字は…@（アットマーク）") | **✅ 구현 완료**(valid_opc_charset, Login/SetPassword/ESSID). 백틱 제외→DFK 확인. ⚠️ 레거시 락아웃 리스크(spec-conformance A5) |
| 에러3 start없는 boundary | "사양 갱신시 **추기**" (값 미제시) | A17 — 0x0018 채택 여부는 갱신본 확인 대기 |
| Ind2 적용범위 | "Dual이면 양쪽 다 통지 가정" | Single 납품 한정으로 현행 OK, **Dual 시 wlan_id 부재 문제 재기 필요** (G4 = `spec-inquiry.md` 마스터 ID; meeting I2/R1) |

### DFK가 사양서 갱신을 약속한 항목 (갱신본 수령 시 대조)
1. 미접속 시 무선필드 무효값(0xFFFF) 규정 추기 — 무선4 (T16)
2. Indication Current Val = 사용률 0-100% 규정 추가 — Ind1 (T6)
3. 패스워드/ESSID 허용 문자집합 명문화 — 에러1 (A5)
4. SetIndication 0x0013 삭제 — 에러2 (A14)
5. 비정상 boundary 시퀀스 에러코드 추기 — 에러3 (A17)
6. List Boundary Flag 값 통일 (page-22: 개시0x0001) — 기타1 (T2/D6)
7. Reset 응답 도면 Length 정정 — 기타3 (T11/T15)

> **진행 현황(2026-06-29 오후)**: 확정 4건 **코드 반영 완료**(`make check` 통과, working tree, 커밋 전) —
> ① List Boundary page-22 flip ② 0x0013 제거→0x0002 ③ Network Current Val Mbps→% ④ 무효문자 화이트리스트.
> code-reviewer 적대적 검토 must-fix 0(net_over 동등성 증명). **보류**: T16(미접속 0xFFFF — 필드폭 갱신본
> 대기)·T17(coalesce — 회의 협의). T15(Reset)는 Length=60 확정으로 코드 무변경. ⚠️ A5 레거시 password
> 락아웃 리스크(spec-conformance A5 기록 — 출하 전 password 화이트리스트 적합 확인 / 마이그레이션 가드 결정 대기).

## T16. GetDeviceInfo 미접속 시 무선필드 무효값 — NEW (2026-06-29, DFK: 0xFFFF)

- **Spec**: §3.3.4 leaves Mode/BandWidth/FREQ/CH undefined when not associated (Status=0x0000).
- **DFK answer (PPTX 무선4)**: "사양서를 갱신해 무효치의 규정을 추기 … 기본적으로는 ALL "F"
  (예: 0xFFFF 16bit의 경우)를 무효 값으로 할 예정" — i.e. **when not associated, the radio fields
  should report 0xFFFF (all-F invalid sentinel)**, not the configured value.
- **Conflict**: current code (`opcd/handler.c` `select_devinfo_freq_ch`, `snapshot.c`) returns the
  **configured cache value** for FREQ/CH/Mode/BW when not associated. Directly opposite to 0xFFFF.
- **TODO(proto-todo:T16)**: when Status=0x0000 (not associated), emit 0xFFFF for the radio fields
  per DFK. Coordinate with **+G11 `device_info_freq_source`** toggle (config/live/auto). Code NOT
  changed (doc-only pass).
- **Resolve when**: updated spec defines the exact invalid-value width per field (0xFFFF for 16-bit
  fields, 0xFFFFFFFF for any 32-bit field?) — confirm field widths.

## T17. Indication 주기내 다중 사상 — coalesce vs edge — NEW (2026-06-29, DFK: 주기 끝 최신 1회)

- **Spec**: §3.3.9 / 그림3-3 — per-period notify with seq increment, but multi-event-within-one-period
  handling undefined.
- **DFK answer (PPTX Ind3)**: "주기내에 복수의 상태 변화가 발생했을 경우, 통지 주기의 마지막 상태
  변화가 발생한 정보를 통지해 주세요" — i.e. **coalesce: report only the *last* state change of the
  period** (level/snapshot model).
- **Conflict**: current code emits event-type indications (WlanStatusChange/Roaming/ApDisconnect) on
  an **edge trigger — immediately and individually, each with its own seq** (`opcd/opcd.c`,
  `indication.c`), no coalescing (intentional, see I3). DFK wants period-end coalescing.
- **TODO(proto-todo:T17)**: if DFK confirms period-end coalesce, gate event-type indications by
  Indication Period and emit only the latest snapshot per period (currently only KeepAlive/FaultDetect
  are period-gated). Behavioral model change; flag interop impact. Code NOT changed (doc-only pass).
- **Resolve when**: meeting/updated spec confirms coalesce vs edge model (note our edge model never
  loses/merges events — argue the trade-off at the meeting).
