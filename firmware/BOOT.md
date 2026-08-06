# Boot architecture: OpenBoot

The dongle boots via [OpenBoot](../third_party/openboot) (pinned submodule),
replacing the in-house stage-1 and its staged update path. OpenBoot's own
design and wire protocol are documented in the submodule
(`docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`); this file covers what is
OpenDongle-specific.

## Flash and RAM map

OpenBoot splits the application region into two A/B slots. Each slot's 32-byte
`OBR2` boot record occupies the **top erase block of its own slot**, so the
usable image is one block less than the slot.

| | CH592 | CH570 |
|---|---|---|
| OpenBoot (USB transport) | `0x00000–0x01FFF` | `0x00000–0x01FFF` |
| Slot A application | `0x02000–0x37FFF` | `0x02000–0x1CFFF` |
| Slot A `OBR2` record | `0x38000–0x38FFF` | `0x1D000–0x1DFFF` |
| Slot B (unused in Phase 1) | `0x39000–0x6FFFF` | `0x1E000–0x39FFF` |
| Usable image per slot | `0x36000` (216 KiB) | `0x1B000` (108 KiB) |
| Bond store | DataFlash logical `0x5000` (phys `0x75000`) | code flash `0x3A000` |
| App RAM top (`_eusrstack`) | `0x200067F0` | `0x20002FF0` |

The 16 bytes above the RAM top are OpenBoot's boot-request word (the app's
`openboot_request_update()` writes the magic there and resets). Both link.ld
files carry ASSERTs pinning the stack top; the ODG2 identity header stays at
image offset `0x20` with `base = 0x2000`.

The CH570 board file clamps OpenBoot's `OB_APP_END` to `0x3A000`, so no OBP
erase/write/commit can reach the bond. Confirmed on silicon: `openboot probe`
on a **blank** CH570 reports `app region 0x00002000..0x0003A000`,
`slots 2 (active none, writing A)` and `write window 0x00002000..0x0001D000`.
Once a factory image has installed slot A's record the device comes up
`active=A, writing=B` and the window moves to `0x0001E000..0x00039000` — the
window always describes the slot being written, not the one running.
`0x3B000` no longer holds anything —
OpenBoot reclaimed its old reserved record page when records moved into the
slots, and our clamp keeps OBP off it anyway.

**CH592 DataFlash is entirely the application's again.** The boot record used
to live at logical `0x7000` and no longer does. One consequence inverts: on
CH592 `minichlink -E` now *does* clear boot state, because the record is in
code flash. The bond at logical `0x5000` still survives `-E`, which is why
`flash-factory` keeps its `ALLOW_BONDED_FLASH` guard.

## Boot decision and the record

OpenBoot launches the app only under a valid `OBR2` record, and our boards set
`OB_BOOT_IMAGE_CRC=1`: the whole image is CRC-checked at every boot (ms-scale;
the CRC runs after clock init at 60/100 MHz).

**Factory images are self-blessing.** `make ch5xx-factory` delegates to
OpenBoot's `factory` target with `FACTORY_BLESS=1`, which composes slot A's
record into the image. A blank part programmed with it boots the application on
**first power-on** — no host, no `openboot bless`. The images are
`0x2000 + capacity + 32` bytes exactly: 118,816 (CH570) and 229,408 (CH592),
and the recipe asserts that size, because an unblessed image is byte-plausible
and would only be discovered when a unit failed to start.

`openboot bless` is not merely unnecessary now — on a blessed unit it is
**impossible**. The device comes up `active=A, write=B`, and `bless` resolves
against the *write* slot, so a slot-A image is refused there by design.

What the boot CRC proves is narrower than it looks: the record stores a length
and a CRC-32, and the bootloader recomputes that CRC over exactly that many
bytes from the slot base. Treat it as protection against accidental corruption,
not as an identity check. Never write CH592 DataFlash over SWD — bench-proven
to corrupt it.

**Finish an SWD factory flash with a real power cycle, not `minichlink -b`.**
`-b` reboots out of halt: it resumes the core rather than resetting through the
boot path, so the boot decision never re-runs from a cold start. That matters
most on CH57x, where errata F26 lets the XIP view serve stale data after a
controller write within the same power cycle — which is exactly why OpenBoot's
own BOOT command resets rather than jumping.

Established by controlled comparison on a bench CH570 — same part, same
application bytes, same SWD write and readback, same subsequent move to USB,
with only the recipe's final step differing:

| recipe ends with | result on USB |
|---|---|
| `minichlink -b` | nothing enumerates at all — neither application nor bootloader |
| `-kt` then `-k3` | application enumerates and runs (`last reset: external`) |

`flash-factory` now ends with the power cycle on both chips.

## Update flow

```sh
opendongle --enter-bootloader --image new-app.bin   # family-guarded reboot
openboot --vid 0x0C45 --pid 0xFEFE flash --force new-app.bin   # in OpenBoot
```

`--enter-bootloader` checks the image's ODG2 family against the device before
rebooting. The firmware side (IAP command 0x85, armed session +
`OB_BOOTREQ_MAGIC` payload) quiesces RF, drains the final reply, masks IRQs,
and takes OpenBoot's safe-access reset. The bond survives updates: OBP writes
only the app region.

**Wrong-family protection is host-side only.** OpenBoot's COMMIT attests only
length and CRC by design, so a raw OBP client can flash anything. The guard
lives in `opendongle --enter-bootloader --image`, which refuses an image whose
ODG2 family does not match the connected device — before the reboot, while the
app is still the thing answering. A deliberate raw-OBP client bypasses it; that
is an accepted property of the update path, not a defect.

An interrupted update leaves the device **running the previous application**,
not stranded in the bootloader. Under A/B, mutations target the *inactive*
slot, so the running image is never touched and BOOT falls back to it. Retry
the flash to recover. (This inverts the pre-A/B behaviour, where an interrupted
transfer left the device in the bootloader.) Not yet exercised on this bench —
see the Phase 1 note below.

## Idle timeout / fail-stay

`OB_IDLE_TIMEOUT_MS` is **real milliseconds**, so the boards' `10000` means
10 seconds. It previously counted poll iterations, where the same value
bench-measured ~273 s on CH570 and ~86 s on CH592 — the value did not change,
its meaning did. Measured on a CH570 after the bump: bootloader at t+1 s,
application back at t+11 s.

Anything that waited out the old window is now ~27× too slow, and
`opendongle --enter-bootloader` returns with most of the 10 s already spent —
a human typing the next command by hand will usually miss it. Script it.

Two further caveats:

- After any successful HELLO, idle auto-boot is suppressed until reset: an
  abandoned OBP session leaves the dongle in the bootloader until
  power-cycled or told to boot (fail-stay). Scripts must finish with a flash
  or an explicit `openboot boot`.
- A CRC-valid but wedged app cannot be forced into the bootloader from
  outside (no strap): recovery is `--enter-bootloader` if IAP still answers,
  else SWD / ROM ISP.

## Factory install

Factory image = OpenBoot ‖ `0x00` pad to `0x2000` ‖ app ‖ `0x00` pad ‖ slot A's
`OBR2` record. The pad is programmed flash, so the post-flash readback compare
covers the whole file; `0xFF` padding would program nothing and fail the
compare.

```sh
make ch592-factory-flash WCHLINK_SERIAL=… MRS_TOOLCHAIN=… OPENBOOT_TOOLCHAIN=…
```

That is the whole procedure. The unit boots the application on first power-on;
there is no bless step and none is possible (see above). `OPENBOOT_TOOLCHAIN`
must point at GCC 12 — `check-openboot-toolchain` refuses anything else,
because OpenBoot's own compiler check went advisory upstream while GCC 15
remains unvalidated on ch57x.

## Phase 1 scope

The application is currently built for **slot A only**. Consequences:

- **In-field OTA does not work, and this must not ship to users.** A
  factory-blessed unit is `active=A, write=B` on its very first OBP session, so
  a slot-A image is refused immediately. The 10 s idle timeout means the dongle
  returns to its application on its own — a refusal, not a brick.
- The A→B transition and the interrupted-update paths are therefore unexercised
  with real product code.

Phase 2 links the application a second time at the slot B base, ships both in
an `.obb` bundle, and restores OTA.

## Pairing procedure

A **bonded** dongle accepts a fresh pair only in the **first few seconds after
boot**: it boots into reconnect, and the boot-window timer alternates onto the
pair address for roughly three seconds so a new keyboard can still get in.
Order matters there: put the keyboard into pairing mode FIRST (so it is already
broadcasting), then power-cycle or replug the dongle. Clearing the bond over IAP
(command 0x89) is the other way in.

An **unbonded** dongle — factory fresh, or just cleared — arms no boot-window
timer and camps in pairing indefinitely, so there is no window to miss. If you
are unsure which you are holding, clear the bond and the timing stops
mattering.

Pairing is a mode-selection sequence at the keyboard
(select USB → select 2.4G → pair), not a single command; a combined
"factory pair" that skips the transport bring-up does not reach the air. A
bonded link that shows `waiting for reconnect` joins on the first keystroke —
keyboards sleep between activity.
