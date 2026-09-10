# TODO

Known work, deliberately not done during the initial firmware import so that the
imported tree stays byte-identical to the artifacts that passed hardware
validation. Anything here that touches a firmware source, a linker script, or
`dongle_image_id.py` / `finalize_image.py` changes the compiled build id, so it
should land together with a re-run of the hardware matrix and re-pinned digests.

## Fixed 2026-09-09: EP6 OUT could be left NAKed, wedging the vendor HID interface

`USB_IRQHandler`'s EP6 OUT case latched an IAP packet only on `TOG_OK` with nothing
pending, then NAKed the endpoint unconditionally; the only re-ACK is in `USB_PollEP6()`,
which runs only when a packet is pending, so a toggle mismatch with nothing pending
(a retransmission of a packet already taken, or a bus error) left EP6 OUT NAKed until
a bus reset or a power cycle: no `--info`, no bond operations, no `--enter-bootloader`.
The case now judges the toggle from the interrupt-status sample already taken and
re-ACKs when nothing was latched and nothing is pending; a latched packet keeps the NAK
the poll re-ACKs, and the pending-while-completed branch is defensive (the SIE's
auto-busy holds NAK until the transfer flag is cleared, so ordinary traffic cannot
reach it). The wedge IS reachable by a conforming host: a valid OUT whose ACK is lost
is retried with the same DATA PID after the command has already run, which is exactly
the toggle mismatch with nothing pending (USB 2.0 8.6.4); it needs a bus error, not a
misbehaving host. Not host-reproducible on the bench, so the check is non-regression:
pipelined writes and the normal maintenance flows. Pre-existing and separate: the
bus-reset path re-ACKs EP6 OUT without cancelling a pending command, so an OUT admitted
then can overwrite `EP6_Buf` under the old length. Both chips' bytes change; the CH570
image is compiled, not bench-verified.

## Fixed 2026-09-09: the terminal reconnect camp has a liveness watchdog

**What was wrong.** Every terminal camp (EV10 give-up, closed boot window, unbonded
start) armed RX once and then leaned on the radio's own events to re-drive it. The
intended backstop, `rf_arm_retry_if_failed()`, fires only on a non-zero arm status,
which neither radio library ever returns for the static descriptor, and CH59x
basic-mode RX has no timeout at all. A lost completion or a deaf PHY left the dongle
in `waiting for reconnect` until a chip reset (seen once on the bench, 2026-09-04).

**Fix.** The camp keeps the boot-window slot, free in every terminal camp, as a 200 ms
liveness tick (`rf_camp_watchdog_tick`, dispatched through `RF_EVT_BOOT_WINDOW` with the
window closed, so it costs no delayed-post slot on CH570). A tick that saw an RX-side
event since the last one (data, CRC error or timeout, from the PHY counters) leaves the
radio alone. A silent tick re-arms RX from task context under the 0x94 ladder's IRQ
mask: on CH59x a silent camp is the normal state, so every silent tick re-arms
preventively (shut + config + RX, ~100 µs per 200 ms); on CH570 the 30 ms camp timeout
keeps the counter moving, so a silent tick means the loop died and the second in a row
escalates to shut + vendor re-init + re-arm (rung 2 of the 0x94 ladder's code path, not
bench-verified on CH570 here). The tick
self-disables outside the exact terminal camp; the burst accept in the radio sink and
every other boot-window cancel end it. A restart that lands in a terminal camp without
the tick (the bond-clear tombstone paths) starts it from the `RF_EVT_RX_RESTART` handler,
so no camp depends on its entry site remembering to. `--rf-poke 3` shuts the radio and leaves it
deaf as the fault injection; page 1 `camp_wd_rearms` (u16, the page's last free bytes)
counts the re-arms. Bench: see the release note.

**Still open.** A deaf-but-cycling CH570 PHY (its 30 ms camp timeouts keep firing while
it receives nothing) looks alive to this test, which measures event-loop progress, not
reception; it needs the separate policy the original entry named (a preventive re-init
after a long interval with timeouts only), and the diagnostics to tell it apart are the
0x94 ladder and page 1 `rx_timeout` against `rx_done`. The pair-ACK burst has the TX-side
twin (a `StartTx` that returns 0 without a `TX_FINISH`), which needs a per-TX completion
timer. The CH570 rung-2 escalation count (`rfd_camp_wd_reinits`) has no page byte left
and is debugger-only. CH570 is compiled, not bench-verified (no CH570 on this bench).
Bounds as shipped: a lost completion is re-armed within two ticks; rung 3 resets the
tick's baseline and is re-armed at the next one.

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

## Fixed 2026-09-09: the connected hop lost the keyboard after a coalesced gap of 5k poll slots

**Where:** `firmware/common/include/rf_protocol.h`, `rf_proto_hop_step()`; the seed
sites in `firmware/common/src/rf_task.c`.

**What the filed overflow really was.** The entry below this one used to describe a
32-bit overflow in the repeat-correction branch (reachable, bounded, "a deliberate
decision to deviate from the recovered rule"). The bench campaign that decision
triggered found the branch itself is the defect, and the overflow only its rarest
symptom: the recovered rule reset the anchor to the poll time, took
`step = elapsed / interval`, and whenever the index landed back on the previous one
forced a slot forward and forward-dated the anchor. That is right for a poll a tick
early (step 0) and wrong for a coalesced gap of exactly 5, 10, 15 ... slots, where
the keyboard is already on that channel: the forced slot misses it, the next poll
takes the wrap arm and hops two slots, and supervision tears the link down.
Masking the dongle's interrupts for a whole number of poll slots on a live link
(`opendongle --rf-poke 40+N`) dropped the link every time at 5 and 10 slots and
never at 1-4, 6-8 or 12. Restricting the correction to step 0 only moved the
failure into the remainder band after a 5k gap (codex counterexample: 5 slots +
15..27 ticks).

**The fix is a model, not a patch.** The dongle now keeps the keyboard's model: an
edge anchor 13 ticks before the poll it expects (`RF_PROTO_HOP_EDGE_LEAD`, every seed
site backdates the grid origin by it), `step = elapsed / interval` from that anchor,
the anchor advanced by step WHOLE intervals (`rf_proto_hop_add`, the poll's jitter
stays in the phase), the index by step mod 5. No correction branch, no reset to the
poll time, and the modular add subtracts before it adds so the sum cannot pass 2^32
(the original finding, closed by construction). Both ends now count the same edges
however the polls are spaced; the one structural exception (the keyboard's servo
settles a tick short of the dongle's lead, so a poll in that two-tick window counts
one edge more, for that poll only) is documented at the definition.

**Bench (CH592, OpenController as the keyboard):** masked gaps of 1..15 whole slots
and the 5- and 6-slot remainder bands (3-tick steps across the slot) all held the
link with 0 lapses; 30 alternating 5/10-slot gaps back to back, 0 lapses; EV10 scan
cadence, bonded reconnect (10/10, 5 ms) and fresh pair unchanged; poll reply ratio
unchanged. `firmware/tests/test_hop_model.py` compiles the real function with the host
compiler and pins the seed contract, a 100k-poll grid across the modulus, every gap
of 0..20 slots and 0..27 ticks against the keyboard model, and the modulus
arithmetic (the "host-tested" clause in the source is true again).

**Not covered:** CH570 compiled, not bench-verified (the model is shared; its seed
sites changed the same way). A production keyboard was not exercised on this bench.

## Fixed 2026-09-09: an invalid bond record could leave the CH570 radio shut down

The bond-persist path's `#if !RF_TASK_EXECUTOR_TMOS` teardown (cancel all four timer
slots, `hal_rf_shut()`) ran before `bond_record_semantic_valid()`, whose early return
then left the radio shut with no timer and no re-arm driver, deaf until a reset. Latent
(the producer cannot build a record the check rejects today), CH570 only. The check is
now hoisted above the teardown, as the entry's fix sketch said, so an invalid record
returns before any radio state changes; found again by the codex pass on the camp
watchdog, which the teardown also cancelled.

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
- **Done 2026-09-09: the hop formula's "host-tested" clause is true again.**
  `firmware/tests/test_hop_model.py` compiles `rf_proto_hop_step()` with the host
  compiler behind a stdin driver and pins the seed contract, the steady grid, every
  coalesced gap against the keyboard model, and the modulus arithmetic (`make test`
  discovers it; it skips without a C compiler).
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
  Update 2026-09-09 evening: a bench restart (replug) returned the rate to ~3000/s
  (`wake_usb` 2970/s, camp idle duty 87 %), so the 8 kHz regime was host state that
  re-enumeration clears, not the topology (the dongle sits on the same USB 2.1 hub
  before and after). Worth knowing when a camp reading looks 0.4 mA high.
- **The poll reply ratio moves with code layout on the receive-arm path: pinned (2026-09-09).**
  Interleaved A/B runs of `pollrate_phy.py` put the fixed 1 ms heartbeat at 99.47-99.53 %,
  one exact-deadline build at 99.80-99.83 % and the next (no change on the poll path) at
  99.51-99.52 %; a `volatile` on the CH592 `rf_diag` block, which only reorders two stores,
  gave 99.22-99.35 %. Cause, from the map: the RAM-code image was loaded ahead of flash
  `.text`, so every RAM-code edit shifted the whole BLE library, and the library's sections
  were linked after every application function, so every application edit shifted them too;
  the poll path runs mostly from flash (`RF_Rx`/`RF_Tx`/`RF_Shut`, the library's receive and
  transmit processing, TMOS, and the application's `RF_ProcessEvent`,
  `rf_connected_poll_cb`, `rf_arm_connected_supervision`, `hal_rf_shut`). `link.ld` now links
  the radio path first (library, then those functions) and loads the RAM-code image after
  `.text`: the library's addresses are identical across RAM-code and application
  perturbations (`RF_Rx` 0x3cee in every variant), and the pinned application functions move
  only when a function ahead of them in that list changes. Open: the flash-fetch period
  itself (a `TEXT_PAD` sweep on a noisy afternoon bench could not resolve it; `TEXT_PAD`
  must be even, and the effective shift is N rounded up to the first pinned section's
  alignment, so read `RF_Rx` in the map), so an edit to a radio-path function can still
  move the functions behind it. Re-run on a quiet bench after the pin (2026-09-09, once a
  bench restart had cleared the host polling storm): plain 99.76-100.26 %, `volatile`
  99.70-100.05 %, overlapping, where the same perturbation separated by 0.3 % with
  non-overlapping pairs before the pin (the ratio's absolute value carries a few tenths
  of a percent of window bookkeeping between the two counters, so only the comparison
  means anything); and the byte-identity gates
  no longer apply across this change (the linker relaxes 22 library calls to `c.jal` from
  the new proximity, 68 bytes smaller), so the gate for link-order changes is the symbol
  set with sizes plus the bench oracles.
- **Tier 2 R1 shipped: windowed receiver (2026-09-10).** The production keyboard rests by
  stopping its session 5 s after the last key and reconnecting once a second (1.010 s,
  sd 15 ms, each connection ~10 ms); the dongle used to spend the whole second in the
  reacquire scan, and the absent camp kept RX on: the receiver is 7.15 mA of the 10.2 to
  10.7 mA idle states, the core's idle path 3.06 mA (host-independent: 3.063 awake,
  3.059 asleep with the receiver never armed). The scan and camp now open 30 ms channel-8
  windows every 200 ms plus a phase-locked window per second; awake resting state 5.74 mA
  (bench builds ran with `PM_RX_GRACE_S=0`; the shipped grace is a nominal 60 s).
  Open: R2 sweeps P in {100, 200, 500} and W in {25, 30, 40} for the shipped pair (the
  production probe is caught on any single channel, so W can shrink); R3a is a spike on
  Halt with the radio shut (datasheet Table 5-2: USB wake exists for Halt, not Sleep; a
  sleep behind TMOS hangs the next RF op on OpenController's bench) before R3b halts the
  core between windows in suspend, re-basing `hal_now()` by the halted RTC ticks; the
  ~3 % of resting cycles that fell back to the continuous scan in earlier builds were the
  detector's traffic threshold; the final build observed 0 fallbacks over 297 cycles. A resting production
  keyboard yields one lapse, one EV10 entry and one promote per second by design; "0 lapses"
  oracles describe OpenController, which never drops on its own.
- **Suspend policy.** Idle while suspended is on (10.71 mA); radio duty-cycling while
  the host sleeps (toward the USB suspend budget) is out of Tier 1 and needs the
  remote-wake latency contract first.
- **`TEM_SAMPLE`.** The library's 1 s ADC temperature sample is pre-existing; it costs
  an ADC conversion and one stale-pending clear per second. Measure before disabling.
- **CH592 OpenBoot USB bootloader on macOS** never binds as HID (usage page 0xFF00);
  USB updates of the CH592 dongle are impossible from this host. Validated only on CH570.
- **Production PB15 strap.** The park sets PB15 input pull-down; page 6 [43] reports the
  boot-time PU/PD/DIR/debug ownership. Read it from the first production CH592D unit.
