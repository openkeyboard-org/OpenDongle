# TODO

Known work, deliberately not done during the initial firmware import so that the
imported tree stays byte-identical to the artifacts that passed hardware
validation. Anything here that touches a firmware source, a linker script, or
`dongle_image_id.py` / `finalize_image.py` changes the compiled build id, so it
should land together with a re-run of the hardware matrix and re-pinned digests.

## Defect: EP6 OUT can be left NAKed, wedging the vendor HID interface

**Where:** `firmware/common/src/usb_device.c`, the `UIS_TOKEN_OUT | 6` case
(the unconditional NAK immediately after the packet-latching `if`).

**What is wrong.** The handler latches an incoming IAP packet only when both
conditions hold:

```c
if ((R8_USB_INT_ST & RB_UIS_TOG_OK) && !iap_pkt_pending) {
    ...
    iap_pkt_pending = 1;
}
R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;   /* unconditional */
```

The NAK is outside the guard, but the only place that re-ACKs EP6 OUT is
`USB_PollEP6()`, which runs its body only when `iap_pkt_pending` is set. So when
the guard fails — a toggle mismatch (`TOG_OK` clear), or an OUT arriving while a
previous command is still pending — the packet is dropped, the flag is never
set, nothing re-ACKs, and **EP6 OUT stays NAKed until the device is power
cycled.**

**Impact.** The vendor HID maintenance interface stops responding: no
`--info`, no bond operations, no `--enter-bootloader`. The RF link and the
keyboard/mouse HID interfaces are unaffected, nothing is corrupted, and a replug
clears it. It is a wedge, not a brick.

**Reachability.** A conforming host cannot trigger it. The IAP protocol is
strict request/response, so the host waits for the EP6 IN reply before sending
the next OUT, which is why the hardware campaign never hit it. It needs a USB
error causing a toggle mismatch, or a host that pipelines requests.

**Fix sketch.** Re-ACK when nothing was latched, and evaluate the toggle from
the same interrupt-status sample already taken rather than re-reading the
register:

- if the packet was latched → leave EP6 OUT NAKed (current, correct behaviour —
  it is the flow-control that keeps the host from overrunning the deferred
  command);
- if it was not latched → restore `UEP_R_RES_ACK` so the endpoint stays live.

**Before merging the fix:** it changes firmware bytes on both chips, so re-run
the update and maintenance cases on hardware and re-pin the artifact digests.
Worth adding a regression check that drives two OUTs back-to-back without
reading the reply in between.

*Found by CodeRabbit during the import review; confirmed by reading the code.
Six other findings from the same pass alleged defects that the source does not
have — the dispositions, with the evidence for each, are in the review comments
on [#3](https://github.com/openkeyboard-org/OpenDongle/pull/3).*

## Defect: the terminal reconnect camp has no time-based liveness backstop

**Where:** `firmware/common/src/rf_task.c`, `rf_stock_reacquire_giveup()`, the closed
boot window (`RF_EVT_BOOT_WINDOW`) and the unbonded `RF_EVT_START` camp; the guard
`rf_arm_retry_if_failed()`.

**What is wrong.** Every terminal camp ends with no active soft-timer slot and arms
RX once. From then on the only thing that re-arms the receiver is the radio's own
30 ms RFIP timeout event (`hal_rf_ch570.c`, `hal_rf_start_rx` translates the
"camp indefinitely" arm into a 30 ms window whose expiry the sink turns into
`RF_EVT_RX_RESTART`). The intended backstop, `rf_arm_retry_if_failed()`, fires only
when the arm status is non-zero -- and on this radio library that never happens:
the linked `RFIP_SetRx` returns non-zero only for a NULL descriptor or a zero DMA
address, both impossible for the static descriptor. So one RX arm whose
completion event is never delivered, or a PHY that stays deaf while the timeout
loop keeps cycling, leaves the dongle in `waiting for reconnect` until a chip
reset. Nothing in the main loop checks radio liveness; USB suspend/resume and
bus reset touch no radio state.

**Evidence.** Structure confirmed by source reading (six independent reviews and
a disassembly of the linked library, 2026-09-04). A failure of exactly this shape
was observed once on the bench the same day (a dongle up since 2026-09-02 that
answered neither keyboard build for >10 min and was cured only by a power cycle)
but it predates the diagnostic page, so it is not yet characterised. The
deterministic reconnect failure fixed by the companion OpenController branch (pair-ACK vs the
keyboard's MR4 window) is a different mechanism and does not depend on this.

**Fix sketch.** Give every terminal camp a slot-backed watchdog (the boot-window
slot is free there): every ~200 ms compare the PHY event counter with the last
value; on the first silent period re-arm RX from task context; on the second,
shut + `hal_rf_init()` + re-arm; keep counting. Post the work as an event from
the timer callback (callbacks run in TMR IRQ context; the vendor init must not
run there). A deaf-but-cycling PHY needs a separate policy (e.g. a preventive
re-init after a long interval with timeouts only). The pair-ACK burst has the
same hole for a `StartTx` that returns 0 without a `TX_FINISH`; it needs a
per-TX completion timer, not the `burst_active` flag (which legitimately stays
set across the 50 ms inter-burst gap). Diagnostics to run first (firmware from the separate RF-diagnostics draft PR): IAP `0x92`
pages 0, 1 and 4 (`rx_armed`, `rx_timeout` rate, `lle_irqs`, LLE mode) and the
armed `0x94` ladder (rung 1 re-arm, rung 2 re-init), which tell a dead software
loop from a deaf PHY in place.

*Found by a multi-lens source analysis with adversarial verification and an
independent codex review during the bonded-reconnect investigation; the camp
chain and the inert guard were verified against the linked library's
disassembly.*

## Defect: `IAP_Service()` bounds its reboot fail-safe with a clock that stops in the reconnect camp

**Where:** `firmware/common/src/iap.c`, `IAP_Service()` — `IAP_SVC_RF_QUIESCE_TICKS`
(500 ms) and `IAP_SVC_USB_DRAIN_TICKS` (250 ms) are measured with `hal_now()`.

**What is wrong.** On CH570 `hal_now()` is the `st_*` software epoch, which only
advances while some soft-timer slot is armed: with nothing armed `st_rearm()`
stops the TMR and `st_now()` returns a constant. The terminal reconnect camp arms
no slot (the 30 ms RFIP timeout drives its re-arm), so the clock is frozen there
— measured on the bench (2026-09-04, IAP `0x92` page 0): `hal_now()` read the
same value in samples 3 s apart. `IAP_Service()`'s two "wall time" bounds
therefore never expire in that state, nor after the quiesce has cancelled every
slot. A quiesce request that never completes, or an EP6 reply the host never
consumes, then blocks the promised fallback reset indefinitely instead of for
500 ms + 250 ms. The comment in `iap.c` ("Deadlines are WALL time via
hal_now()") records the intent, not the behaviour.

**Impact.** No effect on the RF link (no camp-side acceptance gate depends on
elapsed `hal_now()` time; relative timing resumes as soon as a slot is armed).
It only weakens `--enter-bootloader`'s fail-safe. Latent; the normal path
completes both stages long before either bound.

**Fix sketch.** Bound the two stages with a continuous source (SysTick, which
`st_now()` already uses while a slot is periodic) or with pass counts sized to
the main-loop rate measured by the diag page (~44 k passes/s), and say so in
the comment.

*Found by codex during the review of the IAP `0x92` RF diagnostics page,
confirmed by the page's own readout.*

## Defect: CH570 one-shot arms wider than 26 bits are silently truncated

**Where:** `firmware/ch570/src/main.c`, `st_rearm()` — `R32_TMR_CNT_END = d`
with no clamp. Reached with an oversized `d` from
`firmware/common/src/rf_task.c`, the EV10 reacquire watchdog.

**What is wrong.** The CH570 SDK documents the register plainly:

```c
#define R32_TMR_CNT_END (*((PUINT32V)0x4000240C)) // RW, TMR end count value, only low 26 bit
```

The scheduler writes a full 32-bit tick delta into it. At the CH570's fixed
100 MHz, 26 bits caps a single hardware arm at 67,108,863 ticks — **671 ms**.
The reacquire watchdog is `RF_SUPERVISION_STOCK_WATCHDOG_TMOS` (0x0d48 = 3400
TMOS units) × `HAL_TMOS_UNIT_TICKS` (62,500) = **212,500,000 ticks, 2.125 s**.
The hardware keeps the low 26 bits, 11,173,408 ticks, and fires after about
**112 ms** — a factor of 19 early.

It gets worse than an early fire. `st_armed_delta` keeps the *untruncated*
212,500,000, and the expiry path advances `st_epoch` by that amount, so the
software clock jumps roughly 2.01 s ahead of real time in one step. Every other
soft timer with a deadline inside that span is then past due and dispatches in
the same pass.

**Reachability (corrected 2026-09-04, measured).** Latent, not confirmed. The
oversized arm is written to the register only when the EV10 slot is the
NEAREST active deadline. `rf_enter_stock_reacquire()` exits the periodic grid
(its own `hal_timer_cancel(HAL_TMR_SLOT_CONNECTED_POLL)`; note
`rf_send_keys_up_on_link_loss()` cancels nothing), posts `RF_EVT_PAIR_PREP`
and then arms EV10 -- and `rf_send_pair_prep()` re-posts itself as a 30 ms
delayed event for the whole scan, so the 30 ms deadline is re-selected on every
`st_set` and the truncated value is live only for the odd main-loop pass
between the arm and the next re-post. Bench (IAP `0x92` page 3, connected
-> keyboard gone): 71 pair-prep arms per scan = ~2.1 s, i.e. the watchdog
expired at its intended time in every observed cycle. It would bite whenever a
>671 ms one-shot is the nearest armed deadline with nothing shorter alongside
it (a >= 2 ms pump stall at the wrong pass, or a future caller); the fix below
stands, but this is not the bonded-reconnect failure.

**Scope: CH570 only.** CH592 routes the same call through
`hal_tmos_units_from_tsys()`, which divides back to 3400 TMOS units and arms a
TMOS software timer. No 26-bit register is involved. The 300 ms boot window
(480 TMOS units = 30,000,000 ticks) fits on both chips, which is consistent with
the boot window behaving correctly in the hardware campaign while this stayed
hidden.

**Fix sketch.** Clamp the hardware arm to the register's maximum and advance the
epoch by *what was actually programmed*, re-arming until the absolute deadline
is reached. `st_armed_delta` must always equal the value written to
`R32_TMR_CNT_END`, never the requested delta — that invariant is what the
current code breaks.

**Before merging the fix:** it changes firmware bytes on CH570, so re-run the
hardware matrix and re-pin the digests. Worth adding a bench case that forces a
link loss on CH570 and measures the reacquire watchdog against a wall clock.

*Found by codex during the import review; confirmed by reading the SDK register
definition, the tick constants, and the cancel-then-arm ordering in
`rf_send_keys_up_on_link_loss()`.*
## Hazard: the IN handlers fight the hardware toggle, and the obvious fix is not safe

**Where:** `firmware/common/src/usb_device.c` — `USB_HID_IN_TOG_MODE` is
`RB_UEP_AUTO_TOG`, applied to EP1/EP2/EP3/EP5 (and EP6 directly), while each
IN-completion case also does `R8_UEPn_CTRL ^= RB_UEP_T_TOG`.

**Nothing is broken today.** The two disciplines are not both in force: with
`AUTO_TOG` set the hardware owns the toggle bit, so the software XOR does not
reach the transmit sequencer. That is not inferred from silence — the datasheet
assigns bit 4 the auto-flip and confirms it covers EP1/2/3/5/6 on both chips,
the sibling CH582F firmware runs AUTO_TOG-only and works, and the project's own
notes state the rule outright. It is also the only model consistent with the
bench: if both halves were live they would cancel, EP1 would deliver exactly one
keystroke report and go silent, and the IAP endpoint would wedge on its first
response.

**Why it is still worth recording.** The file states its own contract one line
above the define — "production HID endpoints use the validated automatic-toggle
mode" — and the surviving XORs contradict it. They are benign only because the
hardware wins the argument. They become load-bearing the moment someone drops
`AUTO_TOG` to return to the manual discipline, at which point the XORs silently
go live and the `CLEAR_FEATURE(HALT)` DATA0 reset changes meaning too. The
project has already written this combination up as a silent-data-loss
anti-pattern and already removed it once on CH582F; the shared CH592/CH570 path
reintroduced it.

**Do not "just delete the XORs".** That is the obvious remediation and it was
specifically examined and rejected — the cleanup is not safe as a
drop-in. Whoever takes this needs to work out the correct end state first, on
the bench, rather than removing five lines because they look dead.

**Note this is a divergence, not a stock reproduction.** WCH's own examples do
carry the manual XOR, but their bus-reset handler re-arms the endpoints
*without* `AUTO_TOG`, so in vendor code the two disciplines never coexist. This
firmware re-applies `AUTO_TOG` on every bus reset, so here they do.

## Defect: CLEAR_FEATURE(ENDPOINT_HALT) ignores the direction bit

**Where:** `firmware/common/src/usb_device.c`, the `CLEAR_FEATURE` handler —
`wIndex` is masked with `0x0F`, discarding bit 7.

The switch therefore always clears the **IN** half of the named endpoint. EP6 is
bidirectional — `R8_UEP567_MOD` enables both TX and RX and the descriptor
declares OUT endpoint `0x06` — so a host clearing a halt on EP6 **OUT** resets
the IN half instead: wrong endpoint direction reset, and the OUT half left as it
was.

**Reachability is low.** No in-box HID class driver issues this request against
this device, and the maintenance path reaches EP6 through hidraw rather than a
class driver. It needs a host that deliberately halts and clears an endpoint.

**Fix:** decode bit 7 of `wIndex` and act on the matching half. Changes firmware
bytes, so it lands with a re-validation.

## Hygiene: EP0 accepts malformed control transfers instead of stalling

**Where:** `firmware/common/src/usb_device.c`, the standard-request switch.

Four host-to-device requests never inspect the direction bit of
`bmRequestType`: `SET_ADDRESS`, `SET_CONFIGURATION`, `SET_IDLE` and
`SET_PROTOCOL`. A host that sets the IN bit on one of them is ACKed rather than
stalled, the state assignment still happens, and because `usb_setup_len` is left
at `wLength` with `usb_desc_ptr` NULL, the device returns up to 56 bytes of
`EP0_Buf` residue on the data stage. (`GET_CONFIGURATION` with the direction bit
cleared is also unvalidated but inert: it stages a zero-length IN and the stray
OUT is ignored.)

**Why this is filed as hygiene rather than a disclosure.** Every writer of
`EP0_Buf` was enumerated: the buffer only ever holds the host's own SETUP bytes,
public descriptor fragments, single status/config/idle/protocol bytes, and the
keyboard LED byte. No serial-number string is served — string indices other than
0 and 2 stall — and IAP traffic never touches EP0, it is EP6-only. There is no
over-read either, since the transmit length is clamped to `sizeof(EP0_Buf)`. So
what leaks back is data the host already has or could ask for legitimately, and
the requests themselves only set state the host is entitled to set. No
conforming host emits these tuples; it takes a fuzzer, a compliance suite, or a
deliberately hostile host.

**Worth doing anyway**, because stalling malformed control transfers is what USB
chapter 9 requires and it is the difference between passing and failing
compliance tooling. Note the direction checking in this file is already
opportunistic rather than systematic — `GET_DESCRIPTOR`, `GET_REPORT`,
`GET_IDLE` and `GET_PROTOCOL` omit it too — so the tidy fix is a single
direction check at the dispatch point rather than four scattered ones.

## Defect: the hop repeat-correction can forward-date its own anchor and then overflow 32 bits

**Where:** `firmware/common/include/rf_protocol.h`, `rf_proto_hop_step()` — the
`sum = h->last + elapsed` computation inside the repeat-correction branch, and
the anchor write below it.

**Not the finding as filed.** CodeRabbit asked for `h->last` to be reduced
modulo `RF_PROTO_HOP_WRAP` before the addition. That is a no-op: `h->last` was
overwritten with `now` a few lines earlier and `rf_hop_read()` already reduces
`now` mod WRAP, so it is always in range. The overflow is real, but it arrives
by a route the finding does not describe.

**The actual mechanism.** The branch fires whenever `step = elapsed / interval`
is *any* multiple of 5, not only zero. For `step == 0` it back-dates the anchor,
which is the documented intent. For `step >= 5` the same expression *forward*-
dates it: `h->last` lands `elapsed - interval` ticks **ahead** of the hop clock.
`rf_proto_hop_delta()` cannot tell a future-dated anchor from a genuine wrap, so
the next poll takes the wrap arm and returns an `elapsed` near WRAP (~2.83e9).
If that poll's `step` is also a multiple of 5, the branch is re-entered with
`sum = now + elapsed` ~5.3e9, which truncates: the single `if (sum >= WRAP)`
reduction is skipped because the truncated value is already below WRAP, and the
anchor lands low by exactly `2^32 - WRAP` = 1,463,812,096 ticks.

**Reachability: ordinary operation, no attacker.** Reproduced by transcribing
the function and driving it with realistic poll schedules. A brute-force sweep
over gap pairs at the live interval of 28 ticks found 2996 overflowing
schedules. The smallest trigger is a **5-slot coalesced poll gap — 140 ticks,
4.38 ms** — followed by a normal-cadence poll. Poll-event coalescing is
explicitly designed for and documented as benign elsewhere in `rf_task.c`, so
this is not an exotic input. The one real precondition is that the hop clock be
past `2^32 - WRAP`, i.e. more than 12.7 hours into its 24.576-hour cycle, which
is true roughly 48% of the time.

Note this is *not* the "12.7 hours between two consecutive polls" route, which
is genuinely unreachable — supervision tears the link down and re-anchors long
before that. Checking only that route is what made this look like a false
positive on the first pass.

**Impact.** Bounded. The overflowing poll itself still transmits on a
well-defined channel (`idx` is computed before the sum arithmetic) and the
anchor stays within range, so nothing is corrupted and the device does not
brick. What follows is a mis-anchored hop clock: channel selection diverges from
the keyboard's until supervision notices the dead link and re-seeds the anchor.
The observable symptom is a brief deaf patch, not a failure needing a replug.

**Worth knowing before "fixing" it.** The forward-dating comes from faithfully
reproducing the stock dongle's formula, and stock is 32-bit with the same wrap
constant, so the stock firmware has the same overflow. Any change here diverges
from the recovered behaviour that the pairing and connected paths were validated
against. Correcting the arithmetic to 64-bit — or reducing `sum` mod WRAP
properly instead of subtracting once — should be a deliberate decision to
deviate, taken with a bench campaign, not a quiet cleanup.

**Before merging a fix:** it changes firmware bytes on both chips, so re-run the
hardware matrix and re-pin the digests.

*Filed by CodeRabbit with the wrong mechanism and the wrong remedy; the real
route was found by adversarial review and then reproduced independently.*

## Defect: an invalid bond record can leave the CH570 radio shut down

**Where:** `firmware/common/src/rf_task.c`, the bond-persist path — the
`#if !RF_TASK_EXECUTOR_TMOS` teardown block sits *above* the
`bond_record_semantic_valid()` check that can return early.

**What is wrong.** On the not-currently-connected path the block cancels all
four timer slots (`PAIR_ACK`, `EV10_REKEY`, `BOOT_WINDOW`, `CONNECTED_POLL`) and
calls `hal_rf_shut()`. Only *after* that does the record get validated:

```c
#if !RF_TASK_EXECUTOR_TMOS
    if (!was_connected) {
        hal_timer_cancel(...);      /* all four slots */
        hal_rf_shut();
    }
#endif
    if (!bond_record_semantic_valid(&want, rf_factory_mac)) {
        return;                      /* radio still shut, timers still cancelled */
    }
    /* hal_rf_init(), rf_state = RF_STATE_PAIRING, rf_start_rx(),
       rf_arm_retry_if_failed() all live BELOW this point */
```

The restoring half never runs. The dongle is left with the radio powered down,
no timers, no re-camp and no re-arm driver — **deaf until replug or reset**.
`rf_bond_persist_pending` was cleared earlier, so nothing retries. This directly
contradicts the comment a few lines below it, which promises that leaving
`rf_bond_persisted = 0` "keeps the session usable this boot".

**Scope: CH570 only.** The block is inside `#if !RF_TASK_EXECUTOR_TMOS`, and the
macro is 0 in `ch570/src/dongle_target.h` and 1 in `ch592/src/dongle_target.h`,
so the code does not exist in the CH592 build.

**Reachability: currently none — this is latent.** `want` is built by
`rf_commit_bond_ram()` from values that cannot fail the check today: the session
AA comes from `rf_generate_session_aa()` (never 0, `0xFFFFFFFF`, or the pair AA),
the interval and timeout come from our own compiled pair-ACK template, and the
peer MAC has already been screened by `rf_accept_peer_mac()` against exactly the
zero / all-FF / own-MAC cases `bond_record_semantic_valid()` rejects. The check
is defence in depth that should not fire. The ordering is still wrong, and the
consequence if it ever does fire is severe enough that it should not stay wrong.

**Fix sketch.** Hoist the `bond_record_semantic_valid()` check above the
`#if !RF_TASK_EXECUTOR_TMOS` teardown so an invalid record returns before any
radio state changes. The sibling `if (save_rc != 0) return;` is already correctly
placed after the restore, so this is the only site with the problem.

**Before merging the fix:** it changes CH570 firmware bytes, so re-run the
hardware matrix and re-pin the digests.

*Found by CodeRabbit during the import review; the ordering was confirmed by
reading the code, and the reachability analysis is what downgraded it from
"bricks the radio" to "latent".*

## Follow-up: bench case for the mouse/consumer suspend replay fix (needs a controller harness)

**What was fixed without it:** `USB_SuspendResume` now NAKs EP2/EP3 at suspend
and `USB_PollEP6` reconciles them on resume (mouse all-up, consumer no-key,
coordinated with fresh reports through `usb_reconcile_ep2/ep3`). The change was
reviewed (three codex passes to Merge) and regression-tested on the keyboard
path (host asleep 60 s, link intact, 0 lapses, the resume flush executed against
a real host), but the defect scenario itself, a mouse or consumer press armed
when SOF stops, has not been reproduced or shown fixed on hardware.

**Why not:** the bench keyboard stand-in, OpenController, only ever transmits
the keyboard RF tag (0xA1). The dongle routes 0xA3 to EP3 (consumer,
`[id 1][usage LE16]`) and 0xA8 to EP2 (mouse, 5-byte body), and the
controller's UART accepts no frame that would make it send either.

**What the case needs:**

- **A report source:** a tagged-report UART frame in OpenController (its
  `keyboard_uart.c` parser: `expected_for_header` / `frame_is_valid` /
  `dispatch_frame`; `main.c` frame callback; `rf_task.c` `RF_QueueHIDReport`
  generalised with a tag and length so the response TX writes the tag into
  `tx_payload[1]`), streamed at the RF poll rate while the host goes to sleep,
  so an EP2/EP3 report is armed in the last poll interval before SOF stops.
- **An oracle:** page 4 of the 0x92 diagnostics is full (bytes 2-61), so a
  dongle-side "armed at suspend" / "stale IN completion after resume" pair
  needs a new page or a page-6 spare; the host side alternative is cursor
  tracking around the sleep, which needs pyobjc (Quartz) on the bench Mac.
- **Both directions:** a pending press (the original defect) and a pending
  release (the mirror case the NAK alone would have created), each followed by a
  host sleep of at least 60 s and a wake from the host's own keyboard.

## Deferred review findings

Real improvements that were not taken during the import because each one changes
the build id for no functional gain, or expands scope beyond the import:

- **`dongle_image_id.py`** — validate the ODG2 `format` and `header_len` fields
  before writing the CRC, not just `image_len`. Defence in depth against a
  malformed header.
- **`finalize_image.py`** — create both output parent directories and stage each
  temporary file beside its own destination, so a cross-directory or
  cross-filesystem `--output-bin` cannot leave an ELF written and a BIN missing.
- **`check_dependencies.py`** — the optional strict mode (`--expect-compiler-sha256`)
  covers the `riscv32-wch-elf-gcc` driver only, not `cc1`, the assembler, the
  linker or `objcopy`, so a partially replaced toolchain directory can pass it and
  still produce different firmware under the same build id; the default gate
  (GCC major + fast-interrupt probe) makes no byte promise at all. Widening the
  strict mode to a full toolchain manifest would close the gap. Related: the SDK
  cleanliness check ignores files matched by gitignore rules, which could hide a
  stray input the compiler consumes.
- **`check_dependencies.py`** — `validate_sdk()` resolves the SDK's git toplevel
  with `rev-parse --show-toplevel`. If it is ever pointed at an *uninitialised*
  submodule root (an empty directory), git walks up and answers with the
  superproject, so the revision and cleanliness checks would silently test the
  wrong repository. This does not fire on the documented path — the Makefiles
  pass `.../<chip>/EVT/EXAM`, which does not exist until the submodule is
  initialised, so the existing "SDK is missing / initialize submodules" message
  is what a contributor actually sees — but a manual invocation naming the
  submodule root would be misdiagnosed. Worth a guard that rejects a toplevel
  which is not the SDK's own checkout.
- **`hal_rf.h` — two seam contracts that do not match both implementations.**
  Neither is a runtime defect; both are headers that promise more than one of
  the ports delivers.
  - `hal_rf_start_tx()` reads as though its `access_addr` argument is
    authoritative. CH570 programs it per transmit; CH592 ignores it
    (`(void)access_addr;`) and uses whatever the last `hal_rf_configure()` set.
    The invariant holds at all five call sites in `rf_task.c` — every
    `rf_access_addr` assignment is followed by a `hal_rf_configure()` or
    `rf_start_rx()` before any transmit — so the contract should say the caller
    must configure with a matching address first.
  - `hal_rf_rx_buf()` is documented as "stable for the life of the program".
    True on CH570, which returns a static array; false on CH592, which returns
    NULL until the first receive. The function has **no callers** and is
    garbage-collected out of both images, so the honest options are to delete it
    or to document the nullable return.
- **`rxBuf` NULL handling in `rf_phy_event_sink()` is inconsistent.** One call is
  guarded (`rsr == 0 && rxBuf`) while a dozen later dereferences are not. The
  seam cannot deliver NULL on `HAL_RF_EV_RX_DONE` — CH570 passes a static array,
  and on CH592 the vendor library's contract is that the buffer is the DMA
  buffer, which its own reference callbacks dereference unguarded — so the
  unguarded dereferences are safe and the lone guard is what is out of place.
  Either drop the `rxBuf ?` hedge in `hal_rf_ch592.c` so the seam's non-NULL
  guarantee is explicit and matches CH570, or widen the early-out to cover it.
  Worth settling because as written it invites exactly the review finding it
  received.
- **The hop formula is described as "host-tested" and this tree has no such
  test.** The wording appears twice — in `rf_task.c` at the call site and again
  in `rf_protocol.h` — and it was true where the code came from: the covering
  test lives in the private reverse-engineering tree and was not part of this
  import. `make test` discovers only `test_build_identity.py` and
  `test_compose_factory.py`, and a whole-tree grep for `hop_step` finds only the
  definition and its single call site. Two options, and the second is better:
  drop the clause, or port a host test for `rf_proto_hop_step()`. Porting it is
  worth real effort — the adversarial review above turned up a reachable
  overflow in exactly this function, and a table-driven test pinning the stock
  vectors would have caught the regression that a "fix" here could introduce.
  The comments are in build-id-bearing files; a new test file is not, so the
  test can land first and independently.
- **`ch592_boot_reset_status` cannot be inspected the way the startup comment
  says.** `startup_CH592_phased.S` tells a debugger to read boot-entry evidence
  from that name, but it is a file-scope `static uint8_t` in
  `platform_ch592.c` with no `volatile` and no `used` attribute, and nothing in
  the tree ever reads it — so at the shipping `-O2` it is optimised away and a
  debugger answers "optimized out". The retained fault record is where the reset
  status actually survives, so the comment should name that field instead. The
  dead variable should go with it.
- **`usb_device.c` — the `usb_config` invariant comment is not quite true.** It
  says a bus reset is the only thing that clears `usb_config`, and concludes
  that a pending report implies the device is configured. `SET_CONFIGURATION`
  with `wValue` 0 is explicitly accepted and also clears it, without clearing
  `usb_kbd_pending_valid`, so the implication can be violated. Behaviour stays
  safe — the gate simply fails closed — but the reasoning written down is not
  the reasoning the code relies on. (`USB_ClearPendingKeyboard()` is a second
  clearer of the stash, called on RF link loss; worth naming in the same edit.)
- **`platform_ch592.c` — two small hardenings around `dongle_nv_is_erased()`.**
  The reported failure (that the 32-byte cap makes every bond clear report
  failure) does not occur: `bond_record_t` does not exceed the cap. But nothing
  *pins* that relationship, so a future field added to the record would break
  bond-clear verification silently — a `_Static_assert` tying
  `sizeof(bond_record_t)` to the capacity both chips assume is cheap insurance.
  Separately, the local buffer would be better with an explicit
  `__attribute__((aligned(4)))`: it happens to be aligned today, which is not
  the same as being guaranteed.
- **`usb_descriptors.c` — record the IF1 mouse descriptor's provenance.** The
  button usage range declares three usages against a report count of five, which
  is genuinely non-conformant and will be re-reported by every future reviewer.
  It is not ours to fix: the descriptor is a byte-for-byte copy of the
  production dongle's own IF1 report descriptor, and changing it would diverge
  from the device being reproduced. Users are unaffected. A one-line provenance
  comment naming the source would stop this cycling.
- **Three stale or unsupported comments in the CH570 port**, all comment-only
  and all in build-id-bearing files, so they batch with the group below:
  - `hal_usb_ch570.c` — `hal_usb_pins_predetach()` is empty, but the call site
    in `common/src/usb_device.c` says it "releases the PA0/PA1 debug mux for USB
    (and runs any pull-up dance)". The mux release actually happens later, in
    `hal_usb_pins_enable()`. Deferring it there is functionally correct; the
    call-site comment is what is wrong.
  - `ch570/src/main.c` — the comment claims the contradiction between
    `RB_PWR_EXTEND` and the never-sleep policy is caught at build time. No
    `_Static_assert` or `#error` implements that anywhere. Either write the
    check or drop the claim.
  - `ch570/src/main.c` and `ch570/src/sched.h` — both describe
    `st_periodic_nudge()` as refusing when a `CYC_END` is latched or the cycle
    is past its midpoint. The REQUEST-only redesign moved the
    `R32_TMR_CNT_END` write into the TMR ISR and removed both guards; the code
    now tests only `st_periodic_slot_p1`, `st_nudge_req` and `delta`. One
    documentation update was missed at two sites.
- **Stale comments inside build-id-bearing files.** Each is a comment-only edit
  that nonetheless moves the compiled build id, so they are worth batching into
  one commit with the next re-validation rather than taken piecemeal:
  - `ch570/src/dongle_chip.h:4` and `ch592/src/dongle_chip.h:4` name
    `fw-common/src/usb_device.c`; the directory is `firmware/common/`.
  - `ch592/src/startup_CH592_phased.S`, the phase-`0xC4` comment, carries three
    addresses that no longer match the link: the stack starts at `0x20005FF0`
    (not `0x20006000`), `_susrstack` is `0x200060D0`, and `_ebss` is
    `0x20004360` (not `0x20003718`). The claim the comment is making — that the
    `0x20005800` sentinel sits above `.bss` and survives the clear — is still
    true, and `.fault_keep` is its own `NOLOAD` section, so this is an accuracy
    fix rather than a defect.
- **The OpenBoot checkout is the one pinned input nothing validates.** Its bytes
  go straight into every factory image via `compose_factory.py`, which only
  bounds their size. `check_dependencies.py` takes `--sdk`, `--sdk-revision` and
  `--toolchain` and has no OpenBoot parameter; the recursive OpenBoot build runs
  OpenBoot's own checker, which validates *its* openwch submodules and the
  toolchain but never OpenBoot's own HEAD or worktree cleanliness. Nothing
  compares `third_party/openboot` against this repository's gitlink. Neither
  `CONFIG_TEXT` nor `BUILD_ID_INPUTS` names an OpenBoot revision, so the factory
  image's identity does not depend on which bootloader was packaged. The
  application-side companion headers (`openboot_app.h`, `openboot_protocol.h`)
  *are* hashed — the bootloader image is not. Fixing this means a new gate and a
  new field in the identity, both of which move every build id, so it belongs
  with a re-run of the hardware matrix. The inaccurate "check-deps verifies both
  sides" comment in `firmware/Makefile` has been corrected to state the gap.
- **`ch570/link.ld`** — add `ASSERT(_vector_base == ORIGIN(RAM), ...)`. The early
  reset path hard-codes the vector base (`0x20000000`) and the `mtvec` value
  (`0x20000003`) instead of deriving them from `_vector_base`, which the *late*
  path does use. The literal is correct today — the map confirms
  `_vector_base == 0x20000000` — and RAM origin is fixed by hardware, so this is
  drift insurance rather than a fix. It matters because the file otherwise
  converted to linker-derived addressing (its `FAULT_KEEP_ADDR` macro derives
  from `_fault_keep_start`), leaving this literal the odd one out, and because
  `link.ld` already carries a dozen layout `ASSERT`s while none covers this one.
  Note CH592 is incidentally protected here: its `.dalign` section pins `.data`
  to RAM base + `SIZEOF(.highcode)`, so inserting a section ahead of it makes
  the link fail outright. CH570 has no equivalent.
  **Do not "simplify" by substituting `_vector_base` for the `mtvec` literal** —
  `0x20000003` carries the mode bits, and a bare symbol would drop them.

## Project

- **No CI.** There is no `.github/workflows`, so nothing on GitHub compiles this
  tree. A minimal workflow running `git submodule update --init --recursive`,
  `make -C firmware test` and `cargo test --manifest-path tools/Cargo.toml`
  would cover everything except the cross-compile, since the MounRiver toolchain
  is not redistributable.
- **Validation record not published.** The bench harness and the per-case
  hardware validation record are kept out of this repository for now; they carry
  bench-specific hardware identifiers and need a scrub pass before publication.

## Power-management follow-ups (CH592 Tier 1, 2026-09-06)

- **Heartbeat tax: measured and retired.** The keyboard-absent A/B/C (1 ms heartbeat
  10.931 mA, 10 ms 10.819 mA, none 10.796 mA, 2026-09-06) showed the cost is the
  interrupt (~0.13 mA per 1000/s), not the 60 MHz counter (<= 0.01 mA). The
  exact-deadline mode (`PM_EXACT_DEADLINE`, plan section 12) arms TMR3 to the next
  application deadline and caps the sleep for library timers; no RTC trigger needed.
  Left open: the cap default (20 ms) against the library timers' tolerance, and
  whether the 500 ms/120 s HAL calibration would rather be bounded tighter.
- **Clock gating cadence effect: characterised, shipped on (2026-09-09).** The 1.6 %
  the 09-06 absolute-rate A/B showed is not what the reply-ratio oracle sees: it puts
  the gates at ~0.2 % of poll replies (same binary, mask 0 vs 0x4DF6; both halves of
  the mask carry ~0.15 % each, so no single block explains it; the old oracle also
  counted downlink loss, the likely rest). Default is
  now `PM_CLK_GATE=1`; the mask knob stays for anyone who wants the ungated radio
  margin back at +0.25 mA. Open: the physical mechanism (supply ripple on the LDO
  changing the receiver's margin at the arm edge is the guess), which the cadence bench
  cannot see.
- **USB NAK wakes.** ~3000 wakes/s from the 1 ms-interval HID IN endpoints while the
  host is awake. Raising `bInterval` on the mouse/consumer interfaces (keep the
  keyboard at 1 ms) is a product decision; measure before changing.
- **USB IN-token wake rate is host-dependent: ~23 000 wakes/s on the bench Mac (2026-09-09).**
  Page-5 attribution in the keyboard-absent camp with the host awake reads `wake_usb`
  22 900-24 800/s in both heartbeat modes (fixed 1 ms and exact-deadline), against the
  ~3000/s the R2a finding above recorded on 2026-09-06 before the bench was unplugged and
  replugged that evening. The count is three HID IN endpoints (EP1/EP2/EP3, `bInterval`
  1 ms) at about 8000 tokens/s each, i.e. one token per 125 us microframe, as if the host
  scheduled the full-speed interrupt endpoints at the high-speed interval (EP5/EP6 are not
  polled until a client opens the vendor interfaces). Idle duty in that state collapses to
  8-13 % (it was ~86 % at 3000 wakes/s), which is what the meter shows as the camp reading
  bouncing between ~10.45 and ~10.85 mA. Not a firmware defect and not part of PR #38
  (the fixed image shows it too). To do: identify the host path (which port, hub or TT the
  dongle sits behind; `system_profiler` is unavailable inside the sandbox), check whether
  a USB 2.0 hub in between restores 1 ms polling, and decide whether a larger `bInterval`
  on the boot interfaces is acceptable for the product. Every awake-host power figure in
  the release notes for the camp state was taken in this host's polling regime.
- **The poll reply ratio moves with code layout on the receive-arm path (2026-09-09).**
  Interleaved A/B runs of `pollrate_phy.py` (dongle page-1 `rx_done` over the controller's
  valid-poll count, 30 s each) put the fixed 1 ms heartbeat at 99.47-99.53 %, one
  exact-deadline build at 99.80-99.83 % and the next (same rule, one extra conditional
  pass, no change on the poll path) at 99.51-99.52 %; declaring the CH592 `rf_diag`
  block `volatile`, which only reorders two stores, gave 99.22-99.35 %. The receive arm
  after a TX completion has no slack, so where the flash fetches of `hal_rf_ch592.c` land
  matters at the 0.3 % level. Any change near that path needs the interleaved A/B, not a
  single run, and a byte-identical fixed-mode gate does not cover it. Candidate fix:
  place the CH592 RF seam's hot functions in RAM (`__HIGH_CODE`) and re-measure.
- **Suspend policy.** Idle while suspended is on (10.71 mA); radio duty-cycling while
  the host sleeps (toward the USB suspend budget) is out of Tier 1 and needs the
  remote-wake latency contract first.
- **`TEM_SAMPLE`.** The library's 1 s ADC temperature sample is pre-existing; it costs
  an ADC conversion and one stale-pending clear per second. Measure before disabling.
- **CH592 OpenBoot USB bootloader on macOS** never binds as HID (usage page 0xFF00);
  USB updates of the CH592 dongle are impossible from this host. Validated only on CH570.
- **Production PB15 strap.** The park sets PB15 input pull-down; page 6 [43] reports the
  boot-time PU/PD/DIR/debug ownership. Read it from the first production CH592D unit.
