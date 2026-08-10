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
make MRS_TOOLCHAIN="$HOME/Development/Mounriver/Toolchain/RISC-V Embedded GCC15/bin" \
     OPENBOOT_TOOLCHAIN="$HOME/Development/Mounriver/Toolchain/RISC-V Embedded GCC12/bin" \
     factory
```

Produces `build/ch592-product-slotA/opendongle-ch592-product-factory.bin`
(~229 KB, blessed slot A, ~48.6 KB application). `rf_crypt.o` and
`hal_aes_ch592.o` in the link line are the confirmation that encryption is
compiled in.

Flash it over SWD with `flash-factory`, naming the CH592's probe explicitly —
the bench has more than one, and the target is the receiver, not the keyboard:

```bash
make ... MRS_TOOLCHAIN=... OPENBOOT_TOOLCHAIN=... \
     flash-factory DONGLE_PROBE=C2228F064754
```

**This erases the dongle's bond**, so pair again afterwards.

## 2. Pair, then key both ends

Pair normally first: a key has nothing to attach to until a bond exists, and
the keyboard advertises its capability during pairing (one broadcast slot in
four), which is what sets `ENC_CAPABLE` on the receiver's record.

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
