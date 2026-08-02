# Boot architecture: OpenBoot

The dongle boots via [OpenBoot](../third_party/openboot) (pinned submodule),
replacing the in-house stage-1 and its staged update path. OpenBoot's own
design and wire protocol are documented in the submodule
(`docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`); this file covers what is
OpenDongle-specific.

## Flash and RAM map

| | CH592 | CH570 |
|---|---|---|
| OpenBoot (USB transport) | `0x00000–0x01FFF` | `0x00000–0x01FFF` |
| Application | `0x02000–0x6FFFF` | `0x02000–0x39FFF` |
| Bond store | DataFlash logical `0x5000` (phys `0x75000`) | code flash `0x3A000` |
| OBR1 boot record | DataFlash logical `0x7000` (phys `0x77000`) | code flash `0x3B000` |
| App RAM top (`_eusrstack`) | `0x200067F0` | `0x20002FF0` |

The 16 bytes above the RAM top are OpenBoot's boot-request word (the app's
`openboot_request_update()` writes the magic there and resets). Both link.ld
files carry ASSERTs pinning the stack top; the ODG2 identity header stays at
image offset `0x20` with `base = 0x2000`.

The CH570 board file clamps OpenBoot's `APP_END` to `0x3A000`, so no OBP
erase/write/commit can ever reach the bond or record pages. On CH592 the
record and bond live in DataFlash, which OBP cannot address at all.

## Boot decision and the record

OpenBoot launches the app only under a valid OBR1 record, and our boards set
`OB_BOOT_IMAGE_CRC=1`: the whole image is CRC-checked at every boot (ms-scale;
the CRC runs after clock init at 60/100 MHz). Consequences:

- **Every SWD write of app bytes needs `openboot bless <app.bin>`** before
  the app will boot — unless a surviving record still validates against what
  was written (the next point says exactly when that happens).
- On CH592, `minichlink -E` does NOT touch DataFlash, so a record survives a
  reflash. What the boot check proves is narrower than it first looks: the
  record stores a length and a CRC-32, and the bootloader recomputes that CRC
  over exactly that many bytes from the app base. A surviving record therefore
  validates whenever the newly flashed bytes have the recorded image as a
  **prefix** — in practice, re-flashing the same build — which is not the same
  claim as full-image byte identity. Anything else mismatches and the device
  stays in the bootloader until blessed. Treat this as protection against
  accidental corruption, not as an identity check. Never write CH592 DataFlash
  over SWD — bench-proven to corrupt it; only OpenBoot itself writes the record.
- On CH570, `-E` erases the record page too, so a factory flash always lands
  in the bootloader deterministically.

## Update flow

```sh
opendongle --enter-bootloader --image new-app.bin   # family-guarded reboot
openboot flash --force new-app.bin                  # in OpenBoot (1209:0001)
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

An interrupted update leaves the device **in the bootloader** (the record is
invalidated before the first mutation) — never running a half-written app.
Retry the flash to recover. This replaces the old staged model where the old
app kept running through an interrupted transfer.

## Idle timeout / fail-stay

`OB_IDLE_TIMEOUT_MS` counts poll iterations, not wall time (nominal 10000;
real time is calibrated on the bench — see the board files). Two honest
caveats:

- After any successful HELLO, idle auto-boot is suppressed until reset: an
  abandoned OBP session leaves the dongle in the bootloader until
  power-cycled or told to boot (fail-stay). Scripts must finish with a flash
  or an explicit `openboot boot`.
- A CRC-valid but wedged app cannot be forced into the bootloader from
  outside (no strap): recovery is `--enter-bootloader` if IAP still answers,
  else SWD / ROM ISP.

## Factory install

Factory image = OpenBoot ‖ `0x00` pad to `0x2000` ‖ app (the pad is
programmed flash, so the post-flash readback compare covers the whole file;
`0xFF` padding would program nothing and fail the compare).

```sh
make ch592-factory-flash WCHLINK_SERIAL=… MRS_TOOLCHAIN=…
# then, if the unit sits in OpenBoot (on CH570 it always does):
openboot bless ch592/build/ch592-product/opendongle-ch592-product.bin
openboot boot
```

The bless-over-USB step doubles as a manufacturing check of the shipped
update path.

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
