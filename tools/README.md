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
opendongle --fault                             # retained fault page only; arms/disarms nothing
opendongle --status --samples 50 --period-ms 200  # unarmed link-state sampler
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

If another bootloader is already on the bus it refuses **before** rebooting
anything, leaving the dongle running.

**The flash itself cannot be aimed at the wrong device**, which is worth stating
because it is easy to assume otherwise. `openboot` is not told which hidraw path
was identified — it selects by VID:PID — but it narrows by HID usage page first,
so a sibling running its *application* is filtered out, and it then refuses
outright when more than one bootloader interface matches. The failure mode with
several dongles attached is a clear error, not a misdirected write. These checks
exist so that error arrives before a working dongle has been rebooted for
nothing, not because the write is unsafe.

`--image` is the safety interlock: the image's ODG2 family is compared against
the connected device's reported family **before** the reboot, while the
application is still the thing answering, so a CH570 image cannot be sent toward
a CH592 or vice versa. It also refuses a *factory* image with a pointed error —
those are written at address 0 with a debug probe, not over USB. `--force` skips
the guard when you have no image at hand; note the flash itself is unguarded
either way, because OpenBoot's COMMIT attests only length and CRC.

Flags: `--vid` / `--pid` (accept `0x..`/decimal), `--interface`, `--hidraw`
(alias `--path`, explicit device path), `--info`, `--fault`, `--status`, `--diag`, `--rf-poke`
(`--samples N`, `--period-ms MS`), `--enter-bootloader`, `--image FILE`,
`--force`.

`--diag` reads the RF diagnostics pages (command 0x92, up to seven 62-byte
pages: 0-3 required of any firmware that answers the command, 4 present on the
current firmware of both chips but skipped if an earlier firmware answers it
with an empty payload, plus the CH592 power pages 5 "power" and 6 "power
detail" that exist only on a `PM_IDLE=1` build and are skipped the same way;
provided by firmware carrying the RF diagnostics page -- a separate draft PR;
firmware without it answers nothing and the tool reports a timeout;
firmware `RF_DiagFill` in `firmware/common/src/rf_task.c`) with unarmed
exchanges only: the runtime snapshot (state, channel, the access address in
RAM vs the one the radio was last armed with, the last RX/TX/shut status, an
`rx_armed` latch, peer MAC, last LEN-10 disposition, last persist outcome), the
PHY and executor counters (RX arm attempts and failures, RX done/CRC/timeout,
TX start/fail/done, the pending event mask, both delayed-post slots, the
timer slots' remaining times) and the protocol counters (beacons seen and
accepted or rejected with the reason, pair-ACK scheduled/started/finished,
EV10 entries and give-ups, promotes, persist attempts). Counters wrap; with
`--samples N --period-ms T` every counter that changed between samples is
printed as a per-second rate, which is how a healthy reconnect camp reads
(RX re-armed ~33/s on its 30 ms timeout) and how a deaf one is told apart
(no RX events, or a non-zero arm status, or a radio address that is not the
bond's). Every page ends with its raw bytes. CH592 firmware answers the page
with zeros for the CH570-only fields.

On a `PM_IDLE=1` CH592 build each sample after the first also prints two
derived figures. `idle_duty` is page 5 `idle_tsys` (60 MHz SysTick ticks spent
in WFE) over the `hal_now` ticks that elapsed; both are 32-bit and wrap every
71.6 s, and a wrapped numerator cannot be recovered from host time, so the duty
is exact for `--period-ms` under 60000 and prints `n/a` beyond that rather
than a silently low figure. `radio_wake_ratio` is
`wake_both / (wake_radio + wake_both)`: the firmware snapshots what is pending
right after the WFE, before any ISR runs, and `wake_radio` counts exits where a
radio IRQ was the ONLY source pending (proof that the radio ended the WFE)
while `wake_both` counts exits where another source was pending too, which the
counters cannot order (a radio IRQ that never woke the core and waited for the
next TMR3/TMR0/USB looks the same as a radio wake with a heartbeat landing in
the read window). The ratio is a co-pending frequency, not a wake-failure
rate; the radio-wake proof is the protocol around it: a keyboard-absent
negative control must read `wake_radio == wake_both == 0`, then every
reconnect/pair must advance `wake_radio`, and a radio that never ends the WFE
shows as `wake_both` advancing with `wake_radio` flat. In the exact-deadline heartbeat
mode (page 5 flag `exact_deadline`, whose `deadline_cap_us` replaces `heartbeat_us`)
page 6 also carries `hb_arm_deadline` / `hb_arm_cap`, how many sleeps armed TMR3 to an
application timer's deadline versus to the cap; `hb_irqs` then runs at tens a second
rather than a thousand when no application deadline is nearer than the cap (a nearer
deadline arms the timer sooner, and on a live link TMR0 wakes the core before it
fires). The page 6 remote-wake
arm count is 16 bits on both sides and its rate wraps accordingly.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | ok |
| `1` | device/permission/runtime error, or a refused request |
| `2` | the handoff could not be pinned to one device. Either OpenBoot never appeared within 10 s, or the addressed application never left the bus, or more than one new bootloader appeared, or another bootloader was already present when the command started |
| `3` | `--image` was given while only the OpenBoot bootloader is on the bus, so the image's ODG2 family could **not** be checked against the device. The guard lives in the application, which is not present. Pass `--force` to proceed anyway, and note the output says plainly that the family was not verified |

`--fault` issues only the unarmed FaultRead (0x93) and decodes the 40-byte
fault page. The CH570 page v5 and the CH592 page v1 share one layout, and both
are recognised; any other (family, page version) pair is printed as a warning
plus the raw bytes, with no fields decoded. For a **valid** record every field
is shown: startup reset cause and the record's own, the startup phase marker,
the reset-keeper state live and as recorded, kind, action, flags, all four
counters and the trap registers. For an **invalid** record the firmware
populates only the family, page version, valid flag, live keeper, startup
marker and startup reset status; the record-derived fields are zero fill and
are reported as `unavailable` rather than decoded. The raw 40 bytes are always
the last line. Use `--fault` first on a misbehaving unit: it does not arm or
disarm a session (a session left armed by an interrupted earlier run stays
exactly as it was), so it perturbs the device less than `--info`.

`--status` issues a single unarmed Status (0x91) exchange per sample and prints
one line each with a timestamp, the connection state and the last RSSI. With
`--samples N` and `--period-ms MS` it repeats, which is how to watch a link
flicker between *waiting for reconnect* and *connected* without a maintenance
session ever being armed. Note the CH570 SKU is reported to return a constant
RSSI, so treat that column as a hint on that part.

`--rf-poke RUNG` is the one diagnostic that changes device state. It arms a
maintenance session (like the bond commands), sends IAP `0x94` with the rung,
and always disarms. Rung `1` re-arms the receiver from task context; rung `2`
shuts the radio, re-runs the vendor init and re-arms (CH570 only: CH592 answers
"unsupported" for it, a second vendor role init is not validated on the TMOS
radio); rung `3` shuts the radio and leaves it deaf, the fault injection for the
terminal camp's liveness watchdog, which re-arms it at its next 200 ms tick on
both chips (rung 3 resets the tick's baseline; page 1 `camp_wd_rearms` advances
by one; on CH570 the escalation to a vendor re-init needs a second silent tick).
The firmware refuses every rung unless the dongle is in its exact terminal
reconnect camp -- not connected, no EV10 reacquire scan, no boot window or
relisten, no pair-ACK burst in flight -- and refuses them all while quiescing
for a reboot. Exit status is non-zero on any refusal, and the reply names the
reason. Use it on a dongle that is deaf to its
keyboard, rung 1 first: which rung restores the link says whether the software
re-arm loop or the PHY was dead.

The build id `--info` prints is the one to check against the tree that built
the image: `make -C firmware ch592-print-build-id MRS_TOOLCHAIN=... <the same
knobs>` (or `ch570-print-build-id`) prints exactly the id that command line
builds, and `firmware/<chip>/build/<profile>/.build-id-stamp` holds the id the
last build actually compiled in. All three must agree; a mismatch means the
flashed image is not the configuration you think it is.

`--info` reports an absent or invalid bond record as information rather than an
error, and shows a best-effort split of the invalid record's fields (magic,
format, flags, interval, session AA, timeout, both MACs, stored vs computed
checksum) alongside the raw bytes. An all-zero bond record **may** indicate a
failed NV read - the firmware zero-initialises the reply and `bond_load` returns
without filling it on an NV error - but a record that genuinely is, or was
corrupted to, all zeros looks identical, so the tool says "may indicate" and no
more.

With `--hidraw`, `--fault` and `--status` first confirm from HID enumeration
alone that the named path carries the `--vid`/`--pid`/`--interface` selectors,
and refuse to send anything if it does not: an explicit path bypasses discovery,
so nothing else would stop a vendor report going to an unrelated HID node.

On Linux, opening the device needs hidraw permissions (run as root or use a
`plugdev`/udev rule).

## Tests

```bash
cargo test
```

60 tests, no hardware required: Intel-HEX parsing including checksum rejection,
ODG2 header integrity, the device-family binding, wrong-load-base and
factory-image rejection, bond-record and fault-record decoding and the exact
text each renders (valid, invalid/best-effort, unknown layout), status parsing
with legacy fallback, IAP packet golden vectors and transport CRC-32, the
`--hidraw` identity check, the `int(s,0)`-style number parser, and the CLI
contract.

## macOS caveat

This is a 5-interface composite HID device and the vendor channel is interface
4. On Linux/Windows hidapi reports the interface number reliably; on macOS it
can be `-1` for composite devices, in which case interface matching needs a
usage-page fallback. Verify on macOS hardware before relying on it.
