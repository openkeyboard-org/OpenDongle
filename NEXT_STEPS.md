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
(P0 #3), live provisioning (P0 #2), encrypted soaks (CH570 ~14.6k / CH592
~6.2k frames, `drop_mac 0`), USB robustness (EP6 wedge + bus-reset IAP
cancel), CH570 26-bit reacquire clamp, CH592 boot KAT, and stack watermarks
(CH570 548 B → floor `0x700`; CH592 516 B of 1824 B). CH570 product image now
links (P0 #4 closed).

---

## Gate 1 — full hardware matrix + digest re-pin (blocks merge)

The byte-changing discipline (`firmware/TODO.md` preamble): every firmware /
linker / build-id change must land with a complete matrix run and re-pinned
digests. This session exercised the crypto and robustness paths but **not** the
full matrix. Remaining:

- [ ] Both chips: `make -C firmware release` end-to-end on a machine with both
      pinned toolchains (GCC15 app + GCC12 OpenBoot) — app + factory + both
      slots + bundles + `aes-hw-build`.
- [ ] `make -C firmware aes-hw-validate` — the six on-silicon AES/CCM arms
      (`ch570-asm-a/-asm-f/-c`, `ch572-hw/-asm-a`, `ch592-hw`).
- [ ] OpenBoot A/B power-cut acceptance (`firmware/validation/openboot_ab_bench.py`)
      on **both** chips (CH592 has less A/B hardware evidence than CH570).
- [ ] Suspend/resume replay bench case: queue a mouse or consumer report, force
      a host suspend/resume, confirm no stale/stuck report (M2 EP2/EP3 fix).
- [ ] EP6 pipelined-OUT + bus-reset-mid-IAP on **CH592** as well
      (`firmware/bench/usb_robustness_test.py` — done on CH570).
- [ ] CH570 26-bit clamp: wall-clock the reacquire watchdog against ~2.125 s
      (qualitatively confirmed via forced reconnect; the plan wants a measured
      wall-clock case).
- [ ] Per-type HID slots (CH570): concurrent mouse **and** keyboard traffic to
      exercise the mouse-delta-accumulation / no-cross-type-clobber path (only
      keyboard F13 delivery was exercised).
- [ ] Full-matrix stack re-measure both chips incl. a forced fault-handler pass
      (`firmware/bench/STACK-WATERMARK.md` matrix) — this session's readings
      cover the crypto/reconnect paths only. Won't change the `0x700` decision;
      confirms it.
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
normatively; EV10 provisional-apply must snapshot+restore session/counter; fix
the §5.4 dual-`K_ann` verify wording.

## Deferred / lower priority (`firmware/TODO.md`)

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
