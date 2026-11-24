# opendongle-update

Updates a dongle in one step. It replaces the two-command sequence in
[`firmware/BOOT.md`](../firmware/BOOT.md) — `opendongle --enter-bootloader`,
then `openboot flash` inside the bootloader's ten second idle window — and
adds the two things neither tool can do alone: it checks that the bundle is an
OpenDongle update before it reboots anything, and it confirms afterwards that
the dongle it addressed came back running the build it sent.

```sh
opendongle-update --check ch592-product.obb   # check the file; touch no device
opendongle-update ch592-product.obb           # update the attached dongle
opendongle-update --hidraw PATH ch592-product.obb
```

## What it checks before touching the dongle

- The bundle is intact: container CRC, table bounds, per-image CRC.
- It names its chip and carries exactly two builds, one per slot.
- Each build is an **OpenDongle application** linked for the slot it is filed
  under. The `.obb` container cannot say this — an OpenController keyboard
  bundle for the CH592 has an identical header — so the ODG2 header inside each
  image is read and its family, base, length and CRC are checked.
- The dongle's application reports the same family as the bundle.
- Exactly one dongle is running its application (or one was named with
  `--hidraw`), and no OpenBoot bootloader is already on the bus.

## What happens when it is interrupted

A successful HELLO stops the bootloader's idle auto-boot until a reset, so
every path ends by telling the device to boot.

| Stopped at | What the tool does | What the dongle runs |
|---|---|---|
| Before the reboot (a refusal) | Nothing is sent | Unchanged |
| The reboot could not be pinned to one dongle | Nothing is written | Its application, once the bootloader idles out |
| Erasing or writing (failure, or a cancel) | BOOT | The previous build: the running slot is never the one written |
| COMMIT rejected | BOOT | The previous build |
| COMMIT sent, no reply | A fresh HELLO shows which slot is active, then BOOT | The new build if it landed, otherwise the previous one |
| COMMIT sent, no reply, and no reply to that HELLO either | BOOT all the same, then what comes back decides: the new build means it landed, the previous one that the update is not what the dongle boots. If neither is seen, the tool says it does not know | The new build or the previous one |
| COMMIT acknowledged, cross-check failed | BOOT; the bootloader checks the image again as it starts | Whichever passes that check |
| BOOT sent, no reply | Never resent. If the bootloader is still there four seconds on, one new session and one BOOT; the BOOT follows the session's HELLO whether or not that was answered, and a poll on which the bootloader could not be opened is tried again | The new build |
| The reboot request gets no reply | The bus is watched all the same: the dongle resets whether or not its answer was taken. Only if the application is still there afterwards is it an error | Its application if the request never took; otherwise the update goes on |

It then waits up to twenty seconds for the addressed dongle's application to
answer and compares what it finds with what it expected. Another dongle
answering in the meantime does not end the wait; it is reported only if the
addressed one never comes back.

## What it cannot do

It cannot prove, before it erases, that the bootloader it opened is the dongle
it rebooted. The application reports the ROM MAC plus a checksum; the
bootloader reports the flash unique ID on CH592 and the bare MAC on CH570, so
there is no number to compare across the reboot. The single-dongle checks
above are what stand in for that. The confirmation afterwards is same-mode —
the application's UID before against the application's UID after — and is what
would reveal a write that landed on some other unit: the dongle that was
addressed would come back on its old build.

It needs the application to be running. A dongle that is already in the
bootloader is flashed with `openboot flash --force <bundle>` directly.

There is no rollback. An image that passes its CRC and then hangs is started
on every reset; recovery is `--enter-bootloader` if the application still
answers, otherwise SWD or ROM ISP.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | the addressed dongle is running the new build |
| `1` | anything else, including an update that was committed but could not be confirmed |
| `2` | the reboot into the bootloader could not be pinned to one dongle; nothing was written |

## As a library

`artifact::Artifact` is a checked bundle. `transaction::run` is the update,
with the bus, the clock and the device-opening passed in, and returns a
`transaction::Outcome` describing how it ended and what was then running. An
observer receives each stage and each flash event, and can cancel up to
COMMIT and not after.

## Tests

```sh
cargo test
```

No hardware. A simulated dongle (`src/sim.rs`) plays both the application and
the bootloader, with faults to inject at each step. It models the bootloader's
behaviour as read from the OpenBoot source; **the interrupted-update paths have
not been run on OpenDongle hardware**.

## Building

This crate depends on the `openboot` library by a path to a sibling checkout
(`../../OpenBoot/tools`), because that library split is not yet pushed. It is
not built by CI until that becomes a pinned git revision.
