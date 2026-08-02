# OpenDongle firmware — release notes

Firmware for the OpenDongle 2.4 GHz keyboard receiver, on WCH CH570D and CH592F
silicon. This document states the security property of the RF link, the known
issues that ship with it, and the manufacturing steps a unit needs before it
leaves the bench.

## Boot architecture: OpenBoot

The dongle boots via the [OpenBoot](../third_party/openboot) bootloader (pinned
submodule): bootloader at `[0x0000,0x2000)`, application at `0x2000`, an
out-of-band boot record, and updates performed **inside the bootloader** over its
OBP USB protocol — the dongle re-enumerates as `1209:0001` for the flash, then
returns as the application. See [BOOT.md](BOOT.md) for the flash maps, the
boot-record semantics, the update/factory/recovery flows, and the honest caveats
(fail-stay after an abandoned session; wrong-family protection is host-side).

Behavioural properties worth knowing:

- An interrupted update leaves the unit **in the bootloader** until the host
  retries — it never runs a half-written image. The boot record is invalidated
  before the first flash mutation.
- With the boot-time image CRC enabled, an SWD write of application bytes needs
  `openboot bless <app.bin>` before the application will launch again — unless a
  surviving boot record still validates against what was written. That happens
  on CH592, where `minichlink -E` does not reach DataFlash: the record stores a
  length and a CRC over that many bytes, so it still validates when the newly
  written bytes have the recorded image as a prefix, which in practice means
  re-flashing the same build. `BOOT.md` states the exact rule; treat blessing as
  the thing you always do, and the surviving-record case as the reason a unit
  sometimes boots without it rather than as something to rely on.
- Both chips completed a hardware validation campaign covering install, update,
  interrupted update, corrupt-image refusal, boot-request and idle behaviour, a
  50-cycle power soak, pairing and the RF link.

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
