# TODO

Known work, deliberately not done during the initial firmware import so that the
imported tree stays byte-identical to the artifacts that passed hardware
validation. Anything here that touches a firmware source, a linker script, or
`dongle_image_id.py` / `finalize_image.py` changes the compiled build id, so it
should land together with a re-run of the hardware matrix and re-pinned digests.

## Fixed in source, awaiting the next hardware matrix

The five defect entries this file used to carry in full are fixed on the
`em-m2-robustness` / `em-ccm-bench-verify` branches (their analyses live in
the fixing commits and in `docs/reviews/2026-08-16-review.md`):

- **EP6 OUT permanent NAK** — the ISR now re-ACKs when nothing was latched
  and evaluates the toggle from the already-taken status sample.
- **CH570 26-bit one-shot truncation** — `st_rearm()` clamps to
  `ST_MAX_DELTA` and re-arms to the absolute deadline; the invariant is
  `st_armed_delta` == the value written to `R32_TMR_CNT_END`.
- **Suspend replay of mouse/consumer** — the suspend gate NAKs EP2/EP3 with
  the same T_RES-only mask as EP1.
- **CLEAR_FEATURE(ENDPOINT_HALT) direction** — wIndex bit 7 is decoded; EP6
  OUT resets its own half; nonexistent pairs STALL.
- **CH570 bond-persist teardown-before-validate** — the semantic check is
  hoisted above the radio teardown, so an invalid record returns before any
  radio state changes.

Also fixed en route (the M1 batch): the single-slot CH570 HID buffer became
per-type slots with mouse-delta accumulation, the pre-verify sink counters
moved behind the bench profile, and BondWrite gained readback verification
plus live activation. All of it still needs the hardware matrix re-run and
digest re-pin the preamble demands before any of these branches ship.

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

## Deferred review findings

Real improvements that were not taken during the import because each one changes
the build id for no functional gain, or expands scope beyond the import:

- **`dongle_image_id.py`** — validate the ODG2 `format` and `header_len` fields
  before writing the CRC, not just `image_len`. Defence in depth against a
  malformed header.
- **`finalize_image.py`** — create both output parent directories and stage each
  temporary file beside its own destination, so a cross-directory or
  cross-filesystem `--output-bin` cannot leave an ELF written and a BIN missing.
- **`check_dependencies.py`** — the pinned digest covers the `riscv32-wch-elf-gcc`
  driver only, not `cc1`, the assembler, the linker or `objcopy`, so a partially
  replaced toolchain directory can pass the gate and still produce different
  firmware under the same build id. Widening this to a full toolchain manifest
  would close the gap. Related: the SDK cleanliness check ignores files matched
  by gitignore rules, which could hide a stray input the compiler consumes.
- **`check_dependencies.py`** — add an explicit opt-out (e.g.
  `--allow-unpinned-compiler`) that states plainly at build time that the
  resulting artifacts are not the pinned bytes and the build id no longer
  implies them. Today a contributor on another host platform or MounRiver
  release is hard-blocked. See `firmware/README.md`, "The toolchain pin".
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
  import. The host suite has since grown to fifteen-plus modules (including
  `test_rf_crypt.py` and `test_rf_negotiation.py`), but a whole-tree grep for
  `hop_step` still finds only the definition and its single call site. Two options, and the second is better:
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
