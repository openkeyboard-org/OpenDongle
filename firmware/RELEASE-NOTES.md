# OpenDongle firmware — release notes

Firmware for the OpenDongle 2.4 GHz keyboard receiver, on WCH CH570D and CH592F
silicon. This document states the security property of the RF link, the known
issues that ship with it, and the manufacturing steps a unit needs before it
leaves the bench.

## Boot architecture: OpenBoot

The dongle boots via the [OpenBoot](../third_party/openboot) bootloader (pinned
submodule): bootloader at `[0x0000,0x2000)`, application at `0x2000`, a boot
record in the top erase block of each A/B slot, and updates performed **inside
the bootloader** over its OBP USB protocol. The bootloader re-enumerates under
the dongle's own `0C45:FEFE` identity, told apart from the application by its
vendor HID usage page `0xFF00` (the application uses `0xFFFF` and `0xFF60`), and
the flash returns it to the application. See [BOOT.md](BOOT.md) for the flash maps, the
boot-record semantics, the update/factory/recovery flows, and the honest caveats
(fail-stay after an abandoned session; wrong-family protection is host-side).

Behavioural properties worth knowing:

- Two different interruptions have two different outcomes; they are not in
  conflict, but they are easy to confuse:
  - **The transfer dies** (host crashes, cable pulled, power cut mid-write).
    The unit comes back **running the previous application**: mutations target
    the inactive slot, so the running image is never touched and BOOT falls
    back to it. Retry the flash.
  - **A session is opened and then abandoned** (a successful HELLO, then the
    host walks away without flashing or sending BOOT). The unit **stays in the
    bootloader** — a HELLO suppresses idle auto-boot until reset, which is the
    fail-stay rule. Power-cycle it, or end scripts with an explicit
    `openboot boot`.
- A factory image carries slot A's boot record, so a blank part boots the
  application on first power-on with no host and no bless step. On a blessed
  unit `openboot bless` is not merely unnecessary but impossible: the device is
  `active=A, write=B`, and bless resolves against the write slot.
- What the boot-time CRC proves is narrower than it looks: the record stores a
  length and a CRC over that many bytes from the slot base, so it is protection
  against accidental corruption, not an identity check. `BOOT.md` states the
  exact rule.
- `OB_IDLE_TIMEOUT_MS` is now **real milliseconds**. The boards' unchanged
  `10000` means 10 seconds, where it previously bench-measured ~273 s on CH570
  and ~86 s on CH592 — the value did not move, its meaning did. Anything that
  waited out the old window is ~27x too slow.
- Finish an SWD factory flash with a real power cycle, not `minichlink -b`:
  `-b` resumes the core instead of resetting through the boot path, so the boot
  decision never re-runs from a cold start. `flash-factory` does this for you.
  Established by controlled comparison on a CH570 — same part, same bytes, same
  SWD path, only the final step differing: with `-b` nothing enumerated at all,
  with a real power cycle the application came up and ran.
- Hardware validation after the A/B adoption, on a CH570 taken from blank
  silicon to a working dongle: bootloader and application USB enumeration under
  `0C45:FEFE` with usage-page disambiguation, OBP 0.2, on-silicon slot geometry
  and the bond clamp (`write window 0x2000..0x1D000` as reported by the device
  itself), the dry-run/`--force` flash path, `--enter-bootloader`, the 10 s
  idle auto-boot, and factory-blessed first-power-on boot. Product level:
  pairing, typing, media keys, indicator LEDs, reconnect and sleep/wake.
  CH592 covers the same protocol and geometry over UART plus a verified COMMIT.
- **The A->B slot transition is validated on hardware** as of the dual-slot
  build. A CH570 was taken through a full A->B->A round trip over USB with the
  real application, using a `.obb` bundle:

  | step | device reported | bundle selected |
  |---|---|---|
  | start | `active A, writing B`, window `0x1E000..0x39000` | slot B, crc32 `0x893FE89B` |
  | after | `active B, writing A`, window `0x2000..0x1D000` | slot A, crc32 `0xD7958914` |
  | after | `active A, writing B` | — |

  The running build id matched the selected slot's image each time
  (`0x44126E29` for slot B, `0x9F666DEE` for slot A), so the variant really was
  chosen by the device's `write_base` rather than assumed. **The RF bond
  survived both updates** - it sits at `0x3A000`, above the `OB_APP_END` clamp,
  so OBP cannot reach it.
- **Still not validated on hardware: the interrupted-update paths.** Upstream's
  bench harness reaches the bootloader through a CDC-open target reset that this
  bench does not exhibit. That is a limitation of OpenBoot's test code, not the
  product: a source audit of `firmware/core/`, `ports/` and `transports/`
  against every hardware observation found no bootloader defect.

## Security property: the RF link provides no confidentiality

The Bridge75 2.4 GHz data path applies **no confidentiality protection**, by
design and for wire compatibility with the production keyboard. Specifically:

- The per-session access address is sent in **cleartext** in the pair-ACK on the
  well-known pairing address.
- The 5-channel frequency-hop schedule follows **deterministically** from a known
  constant.
- HID reports travel **verbatim** on the link (`rf_protocol.h`, `rf_task.c`).

The prebuilt vendor BLE archive contains cryptographic and SMP routines, but this
product uses raw `RF_Rx`/`RF_Tx` and never invokes them. An attacker within radio
range can recover keystrokes. This is a property of the wire protocol, not a
defect to be fixed in this firmware — fixing it would break interoperability with
the production keyboard. It is stated here so it is an explicit, accepted property
rather than an implicit one.

## Known issues

**CH592: rare fatal fault under a reset during BLE activity.** Under a debug
attach (a halt-and-reset) while the BLE stack is active, the CH592 can take a
fatal fault inside the vendor TMOS layer (`mcause` load fault/misalign, `mepc` in
`TMOS_SystemProcess`/`tmos_proces_system_time`). It is characterised but not
root-caused, and it lives in the prebuilt BLE archive, not in OpenDongle code.
Accepted because:

- the only observed trigger is a reset landing during BLE activity, which a
  **debug attach** produces and a production unit — which has no debug access —
  cannot;
- plain power-rail cycling is clean (10/10 in the characterisation run, and a
  50-cycle soak on the shipped configuration);
- the fault **self-recovers** through the firmware's one-permitted-software-reset
  path (`last reset software`, `boot count` advances, `action recovered`), and a
  real power cycle clears the retained record entirely.

Repeated events within a single power cycle can still reach the fault handler's
repeat fail-stop state rather than recovering; a power cycle clears it. The
application's own reboot-into-bootloader path (vendor HID command `0x85`)
quiesces the radio before resetting, specifically to keep that reset away from
live BLE activity.

## Manufacturing / provisioning duties

**Clear the bond before shipping a CH592 unit.** The CH592 factory flash cannot
erase the bond — it lives in DataFlash, which the probe's whole-chip erase does
not reach — so `make ch592-factory-flash` **fails** if a `BOND` record survives,
and points you at clearing it over the vendor HID interface (command `0x89`). A
shipped unit must not carry a test fixture's pairing identity.
`ALLOW_BONDED_FLASH=1` overrides the check for a deliberate reflash of a bonded
development unit. On CH570 the bond is in code flash and the factory erase clears
it, so a factory-flashed CH570 always needs a fresh pairing.

**Recovery is USB ISP or the bootloader on production hardware.** SWD recovery
was validated on the devboard only. On the production keyboard `PB15` is tied to
GND (it is also TCK), so two-wire debug is unavailable; a stuck production unit is
recovered via USB ISP (`wchisp`) or, if the application still answers, by
rebooting into OpenBoot and flashing from there:

```sh
opendongle --enter-bootloader --image <app.bin>
openboot flash --force <app.bin>
```

Naming the image on the first command is what engages the wrong-family guard,
which compares the image's ODG2 family against the running application before
the reboot — the only point at which that check is possible. Plain
`opendongle --enter-bootloader` is also accepted and reboots the unit, but with
no image to compare it performs no family check.

On CH570 specifically, the debug pins *are* the USB pins: a USB-attached device
has no SWD, and an SWD session is usable only between a whole-chip erase and the
first boot.
