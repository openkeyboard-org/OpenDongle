#!/usr/bin/env python3
"""Flash and verify a CH570 over SWD, around three WCH-driver quirks.

The CH570 is the awkward part on this bench: its SWD pins ARE its USB pins
(PA0 = SWDIO/UDM, PA1 = SWCLK/UDP), so a running image can take the debug
interface, and WCH's OpenOCD `wch_riscv` driver misbehaves in three separate
ways that each look like a hardware fault. All three were measured on silicon
2026-08-22 while recovering a part left half-programmed:

1. A single large image write fails. 16 KB chunks land cleanly.

2. The read-protect unlock covers exactly ONE flash operation. `flash protect
   ... off` reports "Success to Disable Read-Protect" and then the very next
   write fails with "Read-Protect Status Currently Enabled", so the unlock is
   reissued before every erase and every write.

3. The flash READ path only exposes the most recently programmed 16 KB window.
   Every other address either returns a constant 0xf3f9bda9 or aliases that
   window -- a whole-image readback is 100% periodic at 16 KB. Two corollaries
   that cost hours if you don't know them:
     - `verify_image` over the whole image can never pass, and its failure says
       nothing about whether the flash is correct.
     - issuing `flash protect ... off` immediately BEFORE a read is what puts
       the controller into the 0xf3f9bda9 mode, so an otherwise-good verify
       fails purely because of the unlock in front of it.
   This tool therefore reads each chunk back immediately after writing it,
   while that chunk's own window is still the live one. That is a real
   byte-for-byte verify of the whole image, just taken in eight pieces.

A freshly grabbed chip is also halted in BootROM with the flash array unmapped,
so reads taken before the session's first erase/write alias ROM and look like
plausible-but-wrong firmware. Never trust a read taken before a flash
operation.

Usage:
    ch570_swd_flash.py <image.bin> [--probe SERIAL]

See bench/README-link-encryption.md for the wider CH5xx probe story.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

import usb.core
import usb.util

WCH_VID = 0x1A86
LINK_PID = 0x8010
CH570_FAMILY = 0x8B          # the LinkE's family selector for CH570
CH570_CHIP_ID = 0x70
CHUNK = 16 * 1024
SECTOR = 1024

OPENOCD_DIR = os.environ.get(
    "WCH_OPENOCD_BIN",
    "/usr/share/MRS2/MRS-linux-x64/resources/app/resources/linux"
    "/components/WCH/OpenOCD/OpenOCD/bin")


def _find_probe(serial):
    """Return the WCH-Link usb device with this serial, or exit."""
    for dev in usb.core.find(find_all=True, idVendor=WCH_VID, idProduct=LINK_PID):
        try:
            if usb.util.get_string(dev, dev.iSerialNumber) == serial:
                return dev
        except Exception:
            continue
    sys.exit(f"WCH-Link probe {serial} not found")


def grab_window(serial, off=4.0, tries=400, lead=0.005, verbose=True):
    """Power-cycle the target's rail and claim its post-reset debug window.

    Debug is enabled out of reset, but an image that brings up USB clears
    RB_PIN_DEBUG_EN and takes PA0/PA1 back, so the window can be only a few ms
    wide. Cycle the LinkE 3V3 rail and hammer the target-connect with no
    inter-attempt delay; the connect halts the core, which holds debug open for
    the OpenOCD session that follows. An earlier version slept 50 ms between
    attempts, which was wide enough only while the flash was blank (a faulting
    core never reaches the code that closes the window).
    """
    dev = _find_probe(serial)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass
    usb.util.claim_interface(dev, 0)

    def cmd(payload, timeout=300):
        """One LinkE request/response, as hex; "ERR" on any USB failure."""
        try:
            dev.write(0x01, payload, timeout=timeout)
            return bytes(dev.read(0x81, 64, timeout=timeout)).hex()
        except Exception:
            return "ERR"

    try:
        cmd(bytes([0x81, 0x0D, 0x01, 0x0A]))                # 3V3 off
        time.sleep(off)
        cmd(bytes([0x81, 0x0D, 0x01, 0x09]))                # 3V3 on
        t_on = time.time()
        if lead:
            time.sleep(lead)
        for attempt in range(tries):
            cmd(bytes([0x81, 0x0D, 0x01, 0xFF]))
            cmd(bytes([0x81, 0x0C, 0x02, CH570_FAMILY, 0x02]))
            reply = cmd(bytes([0x81, 0x0D, 0x01, 0x02]))
            # 8155... is the LinkE's "no target", which is also what an unwired
            # probe answers -- easy to misread as a dead board.
            if reply == "ERR" or reply.startswith("8155"):
                continue
            # Refuse to erase a part that is not the one we were aimed at. A
            # good connect looks like 82 0d 05 8b 70 ... -- family then chip id.
            raw = bytes.fromhex(reply)
            if len(raw) < 5 or raw[3] != CH570_FAMILY or raw[4] != CH570_CHIP_ID:
                print(f"attached part is not a CH570 (connect reply {reply}); "
                      f"refusing to flash", file=sys.stderr)
                return None
            if verbose:
                print(f"debug window claimed on attempt {attempt + 1} "
                      f"({(time.time() - t_on) * 1000:.0f} ms after "
                      f"power-on): {reply}")
            return reply
        return None
    finally:
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)


def _openocd(serial, commands, timeout=560):
    """Run one OpenOCD session with these -c commands; return its output."""
    binary = pathlib.Path(OPENOCD_DIR) / "openocd"
    if not binary.exists():
        sys.exit(f"WCH OpenOCD not found at {binary}\n"
                 f"Set WCH_OPENOCD_BIN to the directory holding it.")
    argv = [str(binary), "-f", "wch-riscv.cfg",
            "-c", f"adapter serial {serial}",
            "-c", "chip_id CH570"]
    for c in commands:
        argv += ["-c", c]
    proc = subprocess.run(argv, cwd=OPENOCD_DIR, capture_output=True,
                          text=True, timeout=timeout)
    return proc.stdout + proc.stderr


def flash(image_path, serial):
    """Erase, program and verify `image_path`. Returns True on a clean verify."""
    img = pathlib.Path(image_path).read_bytes()
    if not img:
        sys.exit(f"{image_path} is empty")
    # flush: the grab's failures go to stderr, so an unflushed stdout here
    # makes the bench log read backwards.
    print(f"flashing {len(img)} bytes from {image_path} via {serial}", flush=True)

    tmp = tempfile.mkdtemp(prefix="ch570flash-")
    try:
        chunks = []
        commands = ["init", "halt"]
        for base in range(0, len(img), CHUNK):
            blk = img[base:base + CHUNK]
            src = pathlib.Path(tmp) / f"c{base:06x}.bin"
            dst = pathlib.Path(tmp) / f"r{base:06x}.bin"
            src.write_bytes(blk)
            chunks.append((base, blk, dst))
            first = base // SECTOR
            last = (base + len(blk) - 1) // SECTOR
            commands += [
                # Quirk 2: the unlock covers one operation, so reissue it.
                "flash protect 0 0 last off",
                f"flash erase_sector 0 {first} {last}",
                "flash protect 0 0 last off",
                f'flash write_image "{src}" 0x{base:08x}',
                # Quirk 3: read back now, while this window is the live one.
                f'dump_image "{dst}" 0x{base:08x} {len(blk)}',
            ]
        commands.append("shutdown")

        if grab_window(serial) is None:
            print("failed to claim the debug window", file=sys.stderr)
            return False

        out = _openocd(serial, commands)
        for line in out.splitlines():
            if re.search(r"^Error", line):
                print(f"  openocd: {line.strip()}")

        ok = True
        for base, want, dst in chunks:
            if not dst.exists():
                print(f"  0x{base:06x}  {len(want):6d} B  NO READBACK")
                ok = False
                continue
            got = dst.read_bytes()
            bad = sum(1 for a, b in zip(want, got) if a != b)
            bad += abs(len(want) - len(got))
            print(f"  0x{base:06x}  {len(want):6d} B  "
                  f"{'OK' if bad == 0 else f'{bad} BAD BYTES'}")
            ok &= bad == 0
        print(f"{'VERIFIED' if ok else 'FAILED'}: {len(img)} bytes")
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    """CLI entry point: 0 when the whole image verified, 1 otherwise."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="raw .bin to program at flash offset 0")
    ap.add_argument("--probe", default="CF148F065446",
                    help="WCH-Link serial driving the CH570 (default: the "
                         "bench dongle probe)")
    args = ap.parse_args()
    if not pathlib.Path(args.image).is_file():
        sys.exit(f"no such image: {args.image}")
    return 0 if flash(args.image, args.probe) else 1


if __name__ == "__main__":
    sys.exit(main())
