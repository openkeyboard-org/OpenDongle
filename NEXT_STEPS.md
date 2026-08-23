# Next Steps — encrypted-link branches

Tracks the work remaining before the link-encryption effort can merge to `main`
and ship. Written 2026-08-18, after the P0/robustness fixes and the on-silicon
bench validation.

## Open PRs (none merge-ready yet — see gates below)

- **OpenDongle #26** — `em-ccm-bench-verify → main` — M0 release gates + M1
  (the four P0s + bench/product profile split + dongle-side crypto hardening).
- **OpenDongle #27** — `em-m2-robustness → em-ccm-bench-verify` (stacked) — M2
  USB/CH570 robustness, M3 key-establishment design, and the on-silicon
  validation + the CH570 `0x700` stack-floor cut.
- **OpenController #9** — `firmware-link-encryption → main` — the keyboard side
  (CCM, capability advert, TX_FINISH + double-compute stale-abort fix).

## Validated on silicon this session (context)

Both dongle chips, over the production path (capability negotiated on air →
USB-IAP provisioning → live activation → 0-MAC encrypted HID): capability
(P0 #3 -- but see the note below), live provisioning (P0 #2), encrypted soaks (CH570 ~14.6k / CH592
~6.2k frames, `drop_mac 0`), USB robustness (EP6 wedge + bus-reset IAP
cancel), CH570 26-bit reacquire clamp, CH592 boot KAT, and stack watermarks
(CH570 548 B → floor `0x700`; CH592 516 B of 1824 B). CH570 product image now
links (P0 #4 closed).

**Capability qualifier (2026-08-22):** those runs camped the dongle in pairing
BEFORE the keyboard broadcast. In the order this project documents -- keyboard
first, dongle restarted into the running stream -- capability latched 0/10
(OC-01). Fixed by leading every beacon with the advert; now 12/12 in both
orders, with the 24-trial experiment as the regression gate.

---

## Open review finding NOT yet fixed: EP6 OUT flow control (byte-changing)

Both Copilot and CodeRabbit independently flagged `usb_device.c:684` as Major,
and **they are right**. The `else` branch re-ACKs EP6 OUT for two different
cases, and only one of them should:

- *toggle mismatch, nothing latched* — re-ACK is correct. The unconditional NAK
  this replaced left the endpoint dead forever (review finding 8), because the
  only re-ACK sits behind `iap_pkt_pending` in `USB_PollEP6`.
- *an OUT while `iap_pkt_pending` is still set* (a pipelining host) — re-ACKing
  **defeats the one-slot flow control**: the next OUT can overwrite `EP6_Buf`
  before `USB_PollEP6()` consumes it, while `iap_pkt_len` still describes the
  earlier packet, so the command runs over mismatched data.

The fix is to split the branch — NAK when `iap_pkt_pending` is set, ACK
otherwise. That does not re-introduce finding 8: with the flag set,
`USB_PollEP6` re-ACKs unconditionally once it has run the command (verified in
source). The patch was written and builds clean.

**It is deliberately NOT landed here**, and after a second attempt on CH592 the
reason is now sharper than "the A/B was noisy".

**The hazard is not host-reachable, so no A/B can settle it.** Tried on the
CH592 dongle (2026-08-23) with a probe aimed at the actual failure rather than
at end-to-end gates: `USB_PollEP6` runs `ep6_out_cb(EP6_Buf, iap_pkt_len)` and
only *then* clears the flag and re-ACKs, so the window is "a second OUT arrives
while the callback is still reading the buffer". `CMD_STATUS` returns in
microseconds, so the window is invisible; `BondWrite` erases and programs flash
for milliseconds with IRQs masked, which is as wide as that window ever gets.
Pipelining a second OUT directly behind a valid `BondWrite`, **30/30 rounds
returned `0x00 saved`** on the UN-fixed image, with the bond intact afterwards.

That is explicable, not luck: once the ISR latches a packet it sets NAK, so the
SIE *refuses* the next OUT and it never reaches `EP6_Buf`. Reaching the
pre-fix `else` branch needs the SIE to have accepted a second packet in the
microseconds *before* the NAK takes effect — a hardware race a host cannot
drive. (An earlier CH570 attempt was worse than inconclusive: the running build
id and OpenBoot's `active` slot pointer disagreed after `openboot flash`, so
which image executed could not even be confirmed.)

**So judge this fix on review, not on bench evidence.** The reasoning is sound
and was independently confirmed against the source ordering above; it simply
cannot be demonstrated or refuted here, which is characteristic of a race that
shows up in the field rather than on a bench. It is byte-changing (CH570 build
id `0x132BF22D -> 0x87F41F8A`, slot A crc32 `0xD0BA5455 -> 0xD597CAD8`) and
touches `common/`, so both chips move and Gate 1 reopens. Land it batched with
the other three held firmware findings (`stack_watermark.h:72`, `iap.c:76`,
`rf_task.c:3861`) in one matrix-gated change rather than paying that cost twice.

## Gate 1 — full hardware matrix + digest re-pin (blocks merge)

The byte-changing discipline (`TODO.md` preamble): every firmware /
linker / build-id change must land with a complete matrix run and re-pinned
digests.

**Status 2026-08-22: digests ARE re-pinned (`firmware/RELEASE-NOTES.md`), the
CH592 half of the matrix ran, and the CH570 *image* pin is now verified on
silicon.** A CH570 joined the bench later that day and was recovered over USB
(WCH BootROM ISP to clear the config, then `openboot flash ch570-product.obb`).
COMMIT answered `crc32 0xD0BA5455` — computed by the device — matching the pin,
and the booted app reported build `132BF22D` over IAP. It is now live on USB as
`0c45:fefe`.

Note for anyone reading an older revision of this file: the SWD route was tried
first and its byte-for-byte "verify" was **wrong**. The app slot was still empty
afterwards. Confirm CH570 flashes over USB, never from `ch570_swd_flash.py`
output.

**The CH570 production path now passes on silicon** (2026-08-22, after reseating
the keyboard UART): `bench/ch570_validate.py` gives **G1 capability-on-air 6/6**
and **G2 live activation 4/5**, `drop_mac 0`/`replay 0` throughout, run in the
documented pairing order — the order OC-01 is about. The one G2 miss was a
shortened 8 s observation window, not a crypto failure; at the intended 30 s
hold it is 2/2.

Getting there needed three fixes to the harness itself, all timing, all of which
had been reporting as a dead radio link: the dongle accepts a pair only for its
first ~2-3 s of app uptime, the keyboard broadcasts for only ~5.3 s, and
`--enter-bootloader` takes ~12 s to return — so the pair window has to be armed
*late*, during the dongle's reboot, with the bond cleared *before* the keyboard
arms (its tombstone is what stops an instant pair that the reset would then
destroy). See the commit for the full account.

**OC-01 acceptance PASSES** (`firmware/bench/oc01_experiment.py`, 12 trials per
arm): Arm A (documented order) **12/12**, Arm B (control) **12/12**, against a
0/10 pre-fix baseline in that same order — Fisher exact **p = 1.55e-06**. The
plan's gate was A >= 11/12 and B 12/12.

**OD-01 acceptance is NOT established, and an earlier 8/8 "pass" was vacuous.**
Across 22 reboots — including runs blanketing the whole boot window with a
continuously re-armed fresh-pair broadcast — the dongle stayed in
`conn=waiting-reconnect` and never took the re-pair, so the key-preservation
path was never entered. The keyboard's `0x32 CONNECTED` in those runs is
spurious and drops ~3 s later; since its bond was just cleared it has nothing to
reconnect with, so a *sustained* link is the detector the harness now uses.

Do not reach for "did the session AA change?" instead: `rf_generate_session_aa()`
runs only on the *no bond yet* boot branch (`rf_task.c:3411`), a bonded boot
loads `rec.session_aa` (`:3426`), so an accepted same-peer re-pair keeps the same
AA. An interim revision of these notes argued from an unchanged AA; that was
wrong.

**Open question:** how to force the rendezvous — the window is 10 x 300 ms with
only ODD steps on the pair AA (~1.5 s) and the dongle camps on one channel while
the keyboard hops. Until that is solved OD-01 cannot be exercised on hardware,
and the A1 key-preservation fix rests on `test_bond_key_preservation.py` plus
code review alone.

**The CH570 soak and reacquire legs did NOT run. Root cause: a bonded reconnect
establishes but the resulting link is UNSTABLE.** (An earlier revision of this
file said reconnect "never completes" — that was wrong, and wrong for an
instructive reason; see the sampling trap below.)

Measured on CH570 build `132BF22D`, keyboard reconnect armed 9.5 s into the
dongle's reboot so it lands in the boot window:

| cycle | connected | link lifetime | frames verified | drop_mac |
|---|---|---|---|---|
| 1 | yes | held past the 60 s window | 1574 | 0 |
| 2 | yes | 3.6 s | 18 | 0 |
| 3 | yes | 9.7 s | 217 | 0 |

So the reconnect handshake succeeds every time and the encrypted link genuinely
carries traffic — it just dies after a few seconds two times in three, and never
recovers afterwards. Separately, with **no** dongle reset (the steady-state
`waiting` camp) there is no reconnect at all: every counter stays frozen,
including `plain_drop`, so the dongle hears nothing rather than hearing and
rejecting.

**The sampling trap that produced the wrong conclusion:**
`RF_GetConnectionStatus()` (`rf_task.c:4046`) reports *every* bonded
non-CONNECTED state as `waiting`, and the first post-promote supervision
deadline is ~4.4 ms (`:1018`), so polling status at 0.5 s cannot see the
connected state at all. "Dongle stuck in waiting-reconnect" was an artifact of
the sampling rate, not a state. The counters are also RAM and reset with the
dongle, so a baseline taken before a reset makes a working link look dead.

**Leading mechanism (Codex, from source):** fresh pair and bonded reconnect
anchor connected-mode hop timing differently. The dongle sends the initial ACK
on its current AA/channel (`:2825`) but seeds `rf_hop.last` only at the chained
session burst ~50 ms later (`:2030`, `:2890`); a fresh pair has both ends anchor
on that later burst, whereas on reconnect the keyboard has already entered
CONNECTED on the *first* ACK (OpenController `rf_task.c:937`). Marginal
alignment fits the observed all-or-nothing lifetimes. Channel and AA are ruled
out: the keyboard reconnects on the stored session AA across {8,17,26} and the
dongle camps on 8, and `0x32` is only emitted by `rf_enter_connected()` after a
valid LEN-15 carrying the stored dongle MAC, so rendezvous provably happened.

**Instrumented follow-up (2026-08-23).** No on-air capture was possible — a
sniffer needs a third radio and both CH5xx on this bench are participants. (The
`Dongle-Sniffer` tree has listen-only CH582F/CH592F firmware that builds clean,
35648 B, if a spare board ever appears.) Instead the CH570 was flashed with the
bench diag counters enabled (`EXTRA_CFLAGS=-DRF_CRYPT_DIAG_PREV_SESSION=1`),
which required `EXTRA_LDFLAGS=-Wl,--defsym=CH570_STACK_FLOOR=0x640` — those
counters do not fit under the shipped `0x700` floor, exactly as the comment at
`rf_task.c:1505` warns. **The pinned product image has since been restored and
re-attested (`crc32 0xD0BA5455`, build `132BF22D`).**

During a healthy reconnect the counters were unambiguous:

```
ok=314  enc_shape=314  conn_rx=9072  fifo=0  flush=0  mac=0  plain=0
len_max=22 tag=0xA1
```

- `enc_shape == ok` with `mac/fifo/flush/plain` all zero, and `len_max` reaching
  the full 22/0xA1 HID shape: **every frame that arrives verifies.** Nothing is
  lost in the sink, the FIFO, the mint flush, or the crypto path — so the loss
  is on air, not in software.
- The **keyboard drops first** (`0x33`) while the dongle is still verifying
  frames, so the dongle->keyboard direction fails first.
- After the drop **every** counter freezes, `conn_rx` included: the dongle
  leaves connected state entirely and never re-establishes.

**Two things that complicate the reconnect-specific story, and are why this is
not yet a diagnosis:** fresh pairing is not 100% either (G2 ~4/5 on the product
image, 2/3 on the instrumented one), so some of this may be baseline link
flakiness rather than a reconnect defect; and an attempt to measure fresh-pair
link lifetime for comparison was invalid (the harness used `_arm_pair()`, which
omits the `A6 52` unpair, so the keyboard tried to reconnect on a stale bond and
no pair completed). **A clean fresh-pair vs reconnect lifetime comparison is
still owed.**

**Prior art — this class of defect is already documented in-tree.**
`rf_task.c:2131` records that a promote-time hop re-anchor "defeats the burst#1
anchor ... fresh-pair tolerates the offset but bonded reconnect ... produces a
fixed hop-phase offset -> deaf connected poll -> rx=0 (bench + codex
2026-07-07)", and CH570 already compiles that re-anchor out
(`RF_TASK_EXECUTOR_TMOS 0`), keeping the burst#1 anchor with a 13-tick backdate.
So the known mitigation is in place and the residual is a phase-precision
problem, not the gross ~200 ms offset. Codex re-derived the same mechanism
independently from source.

**THE INSTABILITY IS NOT RECONNECT-SPECIFIC (2026-08-23, the owed comparison).**
Fresh pair and bonded reconnect were finally measured the same way — both keyed,
both timed from the keyboard's `0x32` to its `0x33`, 3 cycles each, 75 s window:

| arm | connected | held past 75 s | deaths | frames verified |
|---|---|---|---|---|
| fresh pair | 3/3 | 1 | 3.5 s, 3.0 s | 2574, 0, 0 |
| bonded reconnect | 3/3 | 1 | 3.2 s, 3.2 s | 2618, 0, 0 |

The two arms are indistinguishable, and the behaviour is **bimodal**: a link
either runs indefinitely carrying ~2600 verified frames, or dies in ~3 s having
carried none. There is no middle.

That matters because it **weakens the leading hypothesis**. The hop-anchor
asymmetry predicts fresh pair is aligned and reconnect misaligned — but fresh
pairs fail at the same rate and in the same shape, so whatever kills the link is
common to both paths, not a property of the reconnect anchor. Chase the
all-or-nothing acquisition instead: something at connect either locks or does
not, and when it does not, nothing is ever verified.

(Two harness bugs were found and fixed getting here, both of which had produced
confident nonsense: `measure()` snapshotted the status list on entry and so
missed a CONNECTED the caller had already pumped, reporting "no connect" for
pairs that provably succeeded; and keying the keyboard *after* provisioning the
dongle meant it could not verify the announce, so the fresh arm showed 0 frames
throughout. That is the same ordering rule TODO.md now states — provision before
the encrypted reconnect.)

**AND IT IS NOT CH570-SPECIFIC EITHER (2026-08-23, CH592F dongle on the bench).**
The same experiment on the CH592 dongle (pinned image `44899EB2`), which takes
the OTHER hop-anchor branch (`RF_TASK_EXECUTOR_TMOS 1`):

| chip | arm | held past 75 s | deaths | frames |
|---|---|---|---|---|
| CH570 | fresh | 1/3 | 3.5 s, 3.0 s | 2574, 0, 0 |
| CH570 | reconnect | 1/3 | 3.2 s, 3.2 s | 2618, 0, 0 |
| CH592 | fresh | 2/3 | 37.5 s | 0, 1069, 2608 |
| CH592 | reconnect | 2/3 | 2.4 s | 2510, 0, 2617 |

CH592 holds more links than CH570 (4/6 vs 2/6) but **still drops them**, at
2.4 s and 37.5 s. At n=3 per arm that difference is not meaningful; what IS
meaningful is that deaths occur on both chips and both pairing paths. Combined
with the fresh-vs-reconnect equivalence above, that leaves the hop-anchor
asymmetry explaining neither axis of the data — it is chip-specific and
path-specific, and the failure is neither.

A third mode showed up here too: CH592 fresh cycle 1 stayed connected for the
whole 75 s window and carried **zero** verified frames. So "up" and "carrying
traffic" are separable.

**Caveat on the zero-frame counts, stated so nobody over-reads the table.**
Half these links (6/12) carried no verified frames, but `ch570_validate.py` --
which sequences the pair/key/provision steps more carefully -- passes G2 at
roughly 80-100% on both chips. So an unknown part of that 50% is this
lifetime harness's own ordering rather than the firmware. The DEATHS are the
robust datum; the zero-frame rate is not yet a defect rate.

**Still the decisive measurement:** a time-correlated on-air capture on channel 8
plus the first data channel (28 for `type_tag 0x02`), decoding both AAs, looking
for `kbd LEN10 -> dongle LEN15 -> ~50 ms -> LEN15 burst -> data-channel polls`
and whether the keyboard answers those polls. That needs the third radio.

Two further things that leg-hunting turned up:
- `minichlink -kt/-k3` does **not** work on a CH5xx probe. The belief that those
  flags skip target init and so survive minichlink's CH5xx limitation is wrong:
  both return rc=223 (`WCH-LinkE invalid response failed (-1), command:
  81 0d 01 02`). `firmware/bench/linke_power.py` now drives the rail directly
  over USB instead, and both legs use it.
- Opening the keyboard CDC DTR-resets the keyboard, so a harness can never
  "attach to a live encrypted link" — the attach destroys it. Any soak has to
  establish its own link.
- The stack-watermark read (`0x96`) returns nothing on the pinned product image;
  it needs a `DONGLE_STACK_WATERMARK=1` build, which is byte-changing and so
  carries its own matrix cost.

What is still not covered: no CH572 on this bench at all; the CH570
soak/reacquire legs (above); and two legs blocked by tooling — `aes-hw-validate`
and the A/B power-cut bench both flash over SWD with minichlink, which cannot
connect to ANY CH5xx part (it pre-selects `CHIP_CH32V10x`; see
`firmware/bench/README-link-encryption.md`).

- [x] Both chips: `make -C firmware release` end-to-end with both pinned
      toolchains (GCC15 app + GCC12 OpenBoot) — app + factory + both slots +
      bundles + `aes-hw-build`. **PASS**, `release: all gates passed`.
- [x] Digests re-pinned for both chips / both slots, and CH592 slot A verified
      on silicon (bundle COMMIT `verify OK (device crc32 0x20E39055)`, then IAP
      `0x91` reported build id `44899EB2`).
- [x] Production path on the pinned images: capability negotiated on air,
      `BondWrite -> 0x00` live activation with no reset, `ok 0->143`,
      `drop_mac 0`, F13 delivered host-side, boot KAT OK.
- [x] P1 regression gates on the pinned images: same-peer re-pair preserves the
      key (0 destroyed / 6 kept, was 8/8 destroyed); capability negotiation in
      the documented pairing order (was 0/10).
- [x] Backwards compatibility on ONE unmodified dongle image: a plaintext
      keyboard pairs and delivers HID with `ENC_CAPABLE` never latched and zero
      CCM frames, provisioning correctly refuses that bond, and the same dongle
      then auto-negotiates encryption with the encrypted keyboard.
- [ ] **BLOCKED (no CH572 on this bench; minichlink cannot drive CH5xx):**
      `make -C firmware aes-hw-validate` — the six on-silicon AES/CCM arms
      (`ch570-asm-a/-asm-f/-c`, `ch572-hw/-asm-a`, `ch592-hw`). The three CH570
      arms are now reachable in principle — a CH570 is on the bench and flashes
      cleanly over USB OpenBoot — so this needs the harness ported off
      minichlink onto that path rather than new hardware.
- [ ] **BLOCKED (minichlink cannot drive CH5xx):** OpenBoot A/B power-cut
      acceptance (`firmware/validation/openboot_ab_bench.py`) on both chips. Its
      probe allow-list was inverted and is now fixed, so it is ready to run on a
      bench that can reach the target. Same escape as the AES arms: its CH570
      leg only needs porting onto the USB OpenBoot path.
- [ ] Suspend/resume replay bench case: queue a mouse or consumer report, force
      a host suspend/resume, confirm no stale/stuck report (M2 EP2/EP3 fix).
- [ ] EP6 pipelined-OUT + bus-reset-mid-IAP on **CH592** as well
      (`firmware/bench/usb_robustness_test.py` — done on CH570). The reset leg
      needs `USBDEVFS_RESET`, i.e. root, which this session did not have.
- [ ] CH570 26-bit clamp: wall-clock the reacquire watchdog against ~2.125 s
      (qualitatively confirmed via forced reconnect; the plan wants a measured
      wall-clock case).
- [ ] Per-type HID slots (CH570): concurrent mouse **and** keyboard traffic to
      exercise the mouse-delta-accumulation / no-cross-type-clobber path (only
      keyboard F13 delivery was exercised).
- [x] **CH570 stack re-measure — DONE 2026-08-23, confirms `0x700`.** Built with
      `EXTRA_CFLAGS=-DDONGLE_STACK_WATERMARK=1`, flashed over OpenBoot, and
      sampled across every path this bench can reach: idle **400 B**, then a
      steady **548 B** after the full production crypto path (pair, provision,
      live activation, encrypted HID) and unchanged at 548 B through two
      forced-outage reacquire cycles and two dongle-reset boot-window/fresh-pair
      cycles. Peak 548 B against the `0x700` = 1792 B floor is **1244 B spare,
      3.3x headroom**, reproducing the original reading that the P0 #4 cut was
      based on. The pinned image was restored and re-verified afterwards
      (slot A `0xD0BA5455`, slot B `0xA927252B`).
- [x] **CH592 stack re-measure — DONE 2026-08-23, and it settles the BLE-arena
      review thread.** Same method on the CH592F dongle: idle **384 B**, peak
      **516 B** after the full production crypto path, and 412-476 B across two
      forced-outage reacquire cycles and two reset boot-window cycles. Peak 516 B
      of the **1824 B** stack region is **1308 B spare, 3.5x headroom** —
      reproducing the historical CH592 figure. Pinned image restored and
      re-verified (slot A `0x20E39055`/50308, slot B `0x3C396A1F`/50388, device
      reporting build `44899EB2`).

      On the open `stack_watermark.h:72` thread (PR #26): **the finding is
      correct for that branch, and is already fixed on the stacked #27.** #26's
      copy scans from `_end` unconditionally; `57d35dd` on `em-m2-robustness`
      made the floor per-chip (`_susrstack` on CH592, `_end` on CH570) and
      switched `handle_stack_watermark()` to report `stack_watermark_floor()`.
      Verified on silicon today: the device reported floor `0x200060D0`, exactly
      `_susrstack = _rf_arena_end` from the link map, giving the correct 1824 B
      stack span. Scanning from `_end` (`0x20004718`) would have spanned
      **8408 B**, wrongly including **6584 B** of RF arena, heap and fault
      record — so the reported depth really would have been bogus, as the review
      said. Both hunks live inside `#if DONGLE_STACK_WATERMARK`, which product
      builds leave off, so this is byte-neutral either way.
- [ ] Still outstanding on the stack matrix: the **forced fault-handler pass** —
      no safe way to provoke a fault from the bench was found on either chip.
- [ ] Factory flash + bond-clear-and-verify on both chips (manufacturing
      identity: a factory image does not clear a CH592 DataFlash bond).
- [ ] **Re-pin digests**: refresh `firmware/RELEASE-NOTES.md` build ids and
      image crc32 for both chips / both slots after the matrix.

## Gate 2 — two-repo lockstep

- [ ] Rebase OpenController `firmware-link-encryption` onto its `main` (~4
      behind) and re-test; PR #9 notes this.
- [ ] Land OpenDongle + OpenController **together** — the CCM wire format,
      the capability advert, and the `0x94` diag layout are shared contracts.
- [ ] Merge order for the stacked OpenDongle PRs: #26 to `main` first (GitHub
      then retargets #27 to `main`), or squash-decide the `91e732e`/`e89cd44`
      MIE-mask revert pair if it should not ride along (it is documented as a
      "tried and withdrawn" record — see the OpenController TODO §0).

## Gate 3 — M3 key-establishment decisions (before implementing KEXv1)

`firmware/docs/key-establishment.md` is a review-corrected draft, **not yet
approved for implementation**. Two decisions are open in its §0:

- [ ] **Power-loss commit window** (protocol F3): adopt dongle
      persist-before-announce **and** keyboard retains `LK_old` until a full
      reconnect verifies under `LK_new`, or accept a re-pair on that edge.
- [ ] **Silent plaintext downgrade** with `KBD_REQUIRE_ENC` off (protocol F4):
      add a user-visible "encryption never engaged" signal (keyboard LED), or
      accept the fail-open default as-is.

Also flagged as real scope when implementation starts (Appendix A #9–#12):
SESSION_REQ needs a **new** dongle uplink control-frame classifier + empty-body
MAC-verify verb (not additive); state KDF-vs-direct-CMAC domain separation
normatively; EV10 provisional-apply must snapshot and restore session/counter; fix
the §5.4 dual-`K_ann` verify wording.

## Deferred / lower priority (`TODO.md`)

Not part of this effort; batch with a future re-validation:

- [ ] Hop repeat-correction 32-bit overflow — **deviates from recovered stock
      behaviour**; needs a deliberate decision + its own bench campaign.
- [ ] AUTO_TOG / IN-handler XOR cleanup — examined and rejected as a drop-in;
      do **not** "just delete the XORs".
- [ ] EP0 malformed-control-transfer hygiene (stall vs ACK); `CLEAR_FEATURE`
      already fixed.
- [ ] Toolchain-manifest widening (`cc1`/`as`/`ld`/`objcopy`), OpenBoot HEAD in
      the build id, and the other P3 items in the review (`firmware/docs/
      reviews/2026-08-16-review.md`) and TODO.

## Bench housekeeping / optional

- [ ] Reflash the CH592 dongle (probe `CF148F065446`) from the current
      `DONGLE_STACK_WATERMARK` build back to the plain product image.
- [ ] (Optional) Demonstrate a dongle-side double-compute **catch** (`aes_redo`
      > 0) via induced/prolonged AES-vs-radio contention — this session saw
      0 MAC over ~6.2k CH592 frames but no collision to catch, so the catch
      mechanism itself was not triggered.
