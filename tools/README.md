# opendongle (Rust)

Cross-platform USB-HID maintenance tool for OpenDongle dongles (WCH CH570 /
CH592F). Runs on **Linux, Windows, and macOS**, and builds to a single
executable with no runtime data files.

On Linux that executable is not fully static: `hidapi` uses the hidraw backend,
so the binary links `libudev.so.1` at run time (`ldd` will show it). Any
mainstream distribution already provides it — `libudev1` on Debian and Ubuntu,
`systemd-libs` on Fedora — but a minimal container image may not.

It reads device, firmware, bond and fault information over the dongle's vendor
HID interface, and hands the device off to the
[OpenBoot](../third_party/openboot) bootloader when you want to update it.
**It does not flash firmware itself** — updates happen inside the bootloader
over OBP, driven by the `openboot` CLI that ships with that submodule. See
[`firmware/BOOT.md`](../firmware/BOOT.md) for the boot architecture and the full
update, factory and recovery flows.

## Why hidapi

The dongle's vendor channel is a HID-class interface. `hidapi` rides each OS's
native HID stack (hidraw on Linux, `hid.dll` on Windows, IOKit on macOS), so the
interface opens on all three with no driver replacement — unlike a raw-USB
approach (libusb/nusb), which would need Zadig/WinUSB on Windows and is
impractical for a HID interface on macOS.

## Build

```bash
cargo build --release
# binary: target/release/opendongle
```

Dependencies (`hidapi`, `clap`, `anyhow`, `crc32fast`) are pulled by Cargo.

- **Linux:** the hidapi hidraw backend links `libudev`. Install the dev package
  (Debian/Ubuntu: `sudo apt install libudev-dev`). If `pkg-config` can't find it
  (e.g. a linuxbrew `pkg-config` shadows the system one), point it at the system
  dir: `PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig cargo build`.
- **Windows / macOS:** no extra system packages.

## Usage

Safe by default: with no action it displays read-only status.

```bash
opendongle                                     # same full status as --info
opendongle --info                              # device, build, update, link, bond, health
opendongle --enter-bootloader --image app.bin  # family-checked, then reboots into OpenBoot
opendongle --enter-bootloader --force          # reboot with no family guard

# then, with the device enumerated as OpenBoot. The bootloader shares the
# application's VID:PID, so the openboot CLI needs both flags — its own
# defaults (1209:0001) are the generic/bench identity, not this product's.
# It tells the two modes apart by HID usage page 0xFF00 usage 0x01, which the
# application's interfaces (0xFFFF, 0xFF60) deliberately avoid.
openboot --vid 0x0C45 --pid 0xFEFE flash --force app.bin
```

**Attach one dongle at a time.** Neither this tool nor the `openboot` CLI can
pin a *physical* device across the re-enumeration into the bootloader: both
select by VID:PID (plus HID usage page), and `--serial` selects by ROM UID,
which the application and the bootloader do not present identically. With two
dongles attached, a flash could therefore target the wrong one.

`--enter-bootloader` narrows the window but does not close it. It records which
OpenBoot devices were already present before the reboot, and reports success
only when the application interface it addressed has **left** the bus *and*
exactly **one previously-absent** bootloader has appeared. It exits 2, with an
explanation, if the addressed application never left, if more than one new
bootloader appears, or — after warning — when bootloaders were already present.

**What it still cannot do is aim the flash.** `openboot` selects by VID:PID
(plus HID usage page); it is not told which hidraw path `--enter-bootloader`
identified. So with several dongles attached the flash can still reach a
different one, however cleanly the reboot was observed. Treat the single-device
rule as the actual guarantee and these checks as a way of catching the obvious
mistakes.

`--image` is the safety interlock: the image's ODG2 family is compared against
the connected device's reported family **before** the reboot, while the
application is still the thing answering, so a CH570 image cannot be sent toward
a CH592 or vice versa. It also refuses a *factory* image with a pointed error —
those are written at address 0 with a debug probe, not over USB. `--force` skips
the guard when you have no image at hand; note the flash itself is unguarded
either way, because OpenBoot's COMMIT attests only length and CRC.

Flags: `--vid` / `--pid` (accept `0x..`/decimal), `--interface`, `--hidraw`
(alias `--path`, explicit device path), `--info`, `--enter-bootloader`,
`--image FILE`, `--force`.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | ok |
| `1` | device/permission/runtime error, or a refused request |
| `2` | EnterBootloader was accepted but OpenBoot never appeared on the bus within 10 s (or the application never left it) |
| `3` | `--image` was given while only the OpenBoot bootloader is on the bus, so the image's ODG2 family could **not** be checked against the device. The guard lives in the application, which is not present. Pass `--force` to proceed anyway, and note the output says plainly that the family was not verified |

`--info` reports an absent or invalid bond record as information rather than an
error.

On Linux, opening the device needs hidraw permissions (run as root or use a
`plugdev`/udev rule).

## Tests

```bash
cargo test
```

28 tests, no hardware required: Intel-HEX parsing including checksum rejection,
ODG2 header integrity, the device-family binding, wrong-load-base and
factory-image rejection, bond-record and fault-record decoding, status parsing
with legacy fallback, IAP packet golden vectors and transport CRC-32, the
`int(s,0)`-style number parser, and the CLI contract.

## macOS caveat

This is a 5-interface composite HID device and the vendor channel is interface
4. On Linux/Windows hidapi reports the interface number reliably; on macOS it
can be `-1` for composite devices, in which case interface matching needs a
usage-page fallback. Verify on macOS hardware before relying on it.
