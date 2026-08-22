# Bench setup: validating the encrypted link

The keyboard-side AES-128-CCM transmit path lives in OpenController
(`firmware-link-encryption`). This is the receiver half of the bench setup: a
CH592 dongle image with `DONGLE_RF_CRYPT` compiled in, and the key both ends
must share.

Encryption stays inert until a bond is **capable** *and* **keyed**. Capability
is negotiated on air at pairing; the key is not, because on-air key
establishment does not exist yet. So both ends are handed the same key out of
band — this file is that procedure.

> A shared out-of-band key is a bring-up scaffold, not a security design.
> Anyone who has it can read and forge that link. Use throwaway keys on the
> bench and do not carry one into anything real.

## 1. Build and flash the receiver

The application builds with **GCC15**, the bootloader with **GCC12** — the
build enforces that split, so pass both:

```bash
cd firmware/ch592
make PROFILE=bench ALLOW_BENCH_FACTORY=1 \
     MRS_TOOLCHAIN="$HOME/Development/Mounriver/Toolchain/RISC-V Embedded GCC15/bin" \
     OPENBOOT_TOOLCHAIN="$HOME/Development/Mounriver/Toolchain/RISC-V Embedded GCC12/bin" \
     factory
```

`PROFILE=bench` selects the bench scaffolding this document describes (UART
telemetry, the force key, the prev-session diagnostics); packaging a bench
image requires the explicit `ALLOW_BENCH_FACTORY=1` acknowledgment. The
default (product) profile compiles none of it and `--info` reports which one
is running via the profile byte.

Produces `build/ch592-bench-slotA/opendongle-ch592-bench-factory.bin`
(~229 KB, blessed slot A, ~49.9 KB application). `rf_crypt.o` and
`hal_aes_ch592.o` in the link line are the confirmation that encryption is
compiled in.

Flash it over SWD from the top-level `firmware/` Makefile, naming the CH592's
probe explicitly — the bench has more than one, and the target is the
receiver, not the keyboard:

```bash
cd firmware
make ch592-factory-flash MRS_TOOLCHAIN=... OPENBOOT_TOOLCHAIN=... \
     MINICHLINK=... WCHLINK_SERIAL=CF148F065446 ALLOW_BONDED_FLASH=1
```

*(Earlier revisions of this file said `flash-factory DONGLE_PROBE=...` — no
Makefile reads that variable, and probe `C2228F064754` has left the bench.)*

**This erases the dongle's bond**, so pair again afterwards.

## 2. Pair, then key both ends

Pair normally first: a key has nothing to attach to until a bond exists, and
the keyboard advertises its capability during pairing (broadcast slots 0-1,
then every 8th slot), which is what sets `ENC_CAPABLE` on the receiver's
record. The tool refuses a bond without that flag rather than forcing it —
a forced flag on a keyboard that never advertised is a dead link by
construction.

Then give both ends the *same* key.

Receiver:

```bash
firmware/bench/provision_link_key.py --hidraw /dev/hidrawN --random
```

It prints the key it generated. `--hidraw` is effectively required whenever two
dongles are attached: they are indistinguishable by VID:PID, and keying the
wrong one produces a dead link that looks like a crypto bug. The tool refuses to
guess and lists the candidates. `--show` reports state without writing;
`--clear-key` drops the key but keeps the bond.

Keyboard — build with the bench key command compiled in, then send that key:

```bash
cd ../OpenController/firmware
make KBD_RF_CRYPT=1 KBD_CRYPT_BENCH_KEY=1 bundle
make ... update OB_PORT=/dev/serial/by-id/usb-wch.cn_WCH-Link_<kbd>-if01
```

The keyboard accepts `[0xAE][16 key bytes][checksum]` on its UART and answers
`5B 21` if it took the key, `5B 36` if it refused (no bond yet, or an all-zero /
all-0xFF key, which are erased-flash patterns rather than keys).

`KBD_CRYPT_BENCH_KEY` is deliberately separate from `KBD_RF_CRYPT`: an
encrypted *release* image carries no key-write command at all, because a key
anything on the host wire could overwrite would hand an attacker the ability to
forge keystrokes.

Confirm with `opendongle --hidraw /dev/hidrawN --info` — the bond should read
`capable + key`, and `provision_link_key.py --show` should print
`encryption ACTIVE`.

A successful write takes effect immediately (`0x00`: saved, read-back
verified, and installed into the running RF task — a fresh session is minted
if the link is up). No dongle restart is involved. The one deferred case is
`0xB5`: removing the key while the encrypted link is live keeps that link
fail-closed-dead until it drops, then the bond is plaintext again.

## 3. What to watch for

- **HID delivery.** Keystrokes should reach the host exactly as before. The
  frames are now 22 bytes rather than 10, and the receiver forwards only what
  its CCM tag verified.
- **The idle link.** The receiver force-releases after 64 connected receptions
  with nothing authenticated among them — and it counts bare poll acks, so an
  idle keyboard that only acked would be dropped in ~56 ms. The keyboard sends
  an authenticated frame every 32 receptions to stay inside that. An idle link
  that drops and reacquires on a ~56 ms cadence is this margin being wrong, not
  a radio problem.
- **Reconnect and re-key.** The receiver mints a fresh session on every connect
  and every EV10 re-key, announcing it in place of a poll. The keyboard adopts
  it and keeps its counter climbing — the counter is monotonic per key, never
  restarted per session, because a restart would re-issue nonces the receiver
  has already seen. HID should resume after each.
- **Downgrade refusal.** A plaintext HID frame on an encrypted bond must be
  dropped by the receiver, and the keyboard must never send one: if no sealed
  frame is ready it sends the bare ack instead.
- **Reset the keyboard physically**, never through the debug probe —
  probe-mediated resets produce a spurious recovery failure (see
  OpenController's `firmware/README.md`).

## 4. The UART-only bench (2026-08: no dongle USB attached)

When the receiver's own USB is not connected, everything above that runs over
hidraw (provisioning, `CMD_CRYPT_DIAG`, OBP updates) is unreachable. Two
bench-only gates in `ch592/src/dongle_target.h` replace it. Both live under
the `PROFILE=bench` build profile — a product build cannot compile them
(rf_task.c `#error`s on the force key outside the profile, and the release
target byte-scans every packaged artifact for the key bytes):

- `DONGLE_UART_DIAG` — broadcasts the full crypto telemetry once per second on
  UART1's default PA9 pin (127-byte `0x5E` frame: every `CMD_CRYPT_DIAG`
  field plus `mac_same_ok`, `same_differs`, `bb_during_aes`, the KAT result,
  and the first-DROP_MAC frame latch). Non-blocking: at most one TX-FIFO fill
  per main-loop pass. Reader: OpenController `firmware/bench/rx_uart_diag.py`.
- `DONGLE_CRYPT_BENCH_FORCE_KEY` — at bond load, force link decryption ACTIVE
  for any valid bond using the compiled throwaway key
  `4f70656e4b626421a55ac33c69960ff0` (RAM only; the record is untouched). The
  keyboard is keyed with the same bytes over its `0xAE` bench command. This
  also sidesteps the capability advert that never lands.

Flashing goes over SWD (`ch592-factory-flash` above; the bond it erases is
re-created by pairing). The receiver has no UART RX path — it only transmits —
so it is restarted for the pairing/mint dance by power-cycling its probe rails
(`minichlink -kt` / `-k3`). The full sequence is automated in OpenController
`firmware/bench/bench_run.py --fresh`.

IAP `0x95` (`CMD_CRYPT_LAST_FAIL`) exposes the same failure latch over USB for
when a receiver with USB returns.

## Bench tooling: minichlink cannot reach a CH5xx part

`minichlink` pre-selects `CHIP_CH32V10x` before the WCH-LinkE target-connect, and
a CH5xx only answers that connect after `81 0c 02 <family>`. Every attempt
therefore fails with `81 55 01 01` ("no target") -- the same message an unwired
probe gives, which makes it easy to misdiagnose as a hardware fault. Proven over
raw USB against a live CH592: family `0x01` fails, family `0x0b` (CH59x) returns
`82 0d 05 0b 92 ...`, i.e. chip id `0x92`.

Consequences and workarounds:

- `make flash-factory` and any probe power-cycle path are affected. A locally
  patched minichlink with a family-sweep fallback works; it is not upstream.
- **WCH OpenOCD works** but must be told the family:
  `openocd -f wch-riscv.cfg -c "adapter serial <PROBE>" -c "chip_id CH59x"`.
  Its `wch_riscv` flash driver erases and reads correctly but fails a single
  large image write -- write in 16 KB chunks and it verifies clean.
- **The CH592 dongle has no usable SDI at all** (the connect fails under every
  family while the part is provably alive on USB), consistent with a USB image
  clearing `RB_PIN_DEBUG_EN`. Drive it over USB IAP, and update it over USB
  OpenBoot (`opendongle --enter-bootloader` then `openboot ... flash <obb>`),
  which needs neither SDI nor root.
- `CFG_RESET_EN=0` on these parts: NRST is a GPIO, so a reset button does
  nothing and only a power-cycle (or `--enter-bootloader`) resets the dongle.

## Recovering a CH570: use USB, not SWD

**The supported path is `wchisp` (WCH BootROM ISP) then `openboot flash`.** Only
that path has ever been shown to commit on this bench, and only it produces real
evidence: OpenBoot's COMMIT is a *device-computed* crc32, so it can be checked
against the `RELEASE-NOTES` pin rather than trusted.

```
# 1. Chip in ISP (it enters on its own when the app slot is empty; ~10 s window,
#    and the window is an IDLE timeout, so a whole sequence fits in one session).
#    ch37x binds the device, so detach it first -- permissions are already fine.
wchisp config disable-readout      # BootROM 2.30 refuses Program while readable
# 2. Then, once OpenBoot is up (0c45:fefe):
openboot --vid 0x0c45 --pid 0xfefe flash firmware/ch570/build/ch570-product.obb --force
```

Measured 2026-08-22: `commit OK (len 30924, crc32 0xD0BA5455)`, matching the
CH570 slot A pin, and the booted app then reported build `132BF22D` over IAP.

Two traps in that sequence. `wchisp erase` (and `flash` without `--no-erase`)
knocks the BootROM off the bus mid-operation -- `Erased 118 code flash sectors`
is followed by `ENODEV` -- so let OpenBoot do the erasing. And each `wchisp`
step can reset the part, so a script must wait for a *fresh* appearance rather
than a still-present one, or every later step silently runs against a dying
session.

### SWD, and why its "verify" cannot be believed

`bench/ch570_swd_flash.py` exists for a part with no bootloader to talk to. It
programs, but **its readback is not proof of a commit**: a run that reported all
118816 bytes matching left the app slot *empty* -- the part booted to the WCH
factory ISP and OpenBoot then reported `slots 2 (active none)`. This is the
standing CH5xx rule (SWD reads return stale-but-plausible data) biting again, so
always confirm over USB. What follows is about getting the tool to run at all,
not about trusting its output.

- **The debug window is a few ms wide.** PA0/PA1 are SWDIO/SWCLK *and* USB
  D-/D+, so once an image reaches USB init it clears `RB_PIN_DEBUG_EN` and takes
  the pins. Power-cycle the LinkE 3V3 rail and hammer the target-connect with no
  delay between attempts; it lands ~20 ms after power-on. A blank part is
  misleadingly easy to attach to -- it faults before ever closing the window --
  so a grab loop calibrated on blank flash will fail the moment the part works.
- **A single large image write fails; 16 KB chunks land cleanly.**
- **The read-protect unlock covers exactly one flash operation.** `flash protect
  0 0 last off` reports `Success to Disable Read-Protect`, and the *next* write
  still fails with `Read-Protect Status Currently Enabled`. Reissue it before
  every erase and every write.
- **Whole-image reads come back 100% periodic at 16 KB**, so `verify_image`
  across the image never passes and its failure says nothing either way. Reading
  each chunk back immediately after writing it *does* match -- but see the
  warning above: that match is not evidence the write committed.
- **`0xf3f9bda9` is what ERASED CH570 flash reads as**, not a fault and not a
  protection artifact. Confirmed from the running app: a cleared bond region
  reads `a9 bd f9 f3` repeated and `opendongle --info` correctly reports the bond
  absent. Do not read that pattern as "the read path is broken" -- an earlier
  pass through this wasted hours on exactly that misreading. (The pre-existing
  bench note already said `a9bdf9f3`-pattern reads happen even with read
  protection "disabled".)
- **A freshly grabbed chip is halted in BootROM.** Reads taken before the
  session's first erase/write look like plausible firmware -- structurally valid
  startup code that matches no build in the tree.

There is no sound way to confirm a CH570 is programmed from SWD alone. `pc`
advancing through `__HIGH_CODE` with `mcause == 0` only shows *something* is
running, and on a part whose app slot is empty that something is the BootROM.
Confirm over USB: `openboot probe` must report an active slot, and
`opendongle --info` must report the pinned build id.
