#!/usr/bin/env python3
"""Read OpenBoot's A/B slot geometry out of the pinned submodule.

OpenBoot splits the application region into two slots and puts each slot's
32-byte boot record in the TOP erase block of that slot, so the usable image
size is one erase block less than the slot. The application must be compiled
with -DOPENBOOT_SLOT_BASE/-DOPENBOOT_SLOT_SIZE matching the bootloader exactly:
openboot_app.c derives its record address as BASE + SIZE - ERASE_BLOCK, and a
mismatch is silent. One block too large and the app reads slot B's record while
believing it read its own.

So these values are DERIVED, never duplicated. OpenBoot generates
openboot_config.h from its board file with a plain printf rule that is not in
its TOOLCHAIN_GOALS, which means it builds with no cross compiler installed --
the same mechanism OpenBoot's own test_board_config.py uses. Reading the
generated header is therefore cheap, needs no toolchain, and cannot drift from
what the bootloader was built with.

Prints one line to stdout, for consumption by $(shell) in the chip Makefiles:

    SLOT_BASE SLOT_SIZE CAPACITY RECORD_SIZE IMAGE_PATH

the four sizes as 0x-prefixed hex. The bootloader image path rides along
because it comes from the same `make print-image-path` call this already has to
make; asking for it separately would mean a second nested make on every parse.
Any problem is a message on stderr and a non-zero exit, which leaves $(shell)
empty so the Makefile's own guard fires.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

# The header OpenBoot generates from the board file, alongside the image.
CONFIG_HEADER = "openboot_config.h"


def run_make(openboot: Path, args: list[str], goal: str) -> str:
    """Invoke OpenBoot's firmware make for one toolchain-free goal.

    MAKEFLAGS is scrubbed because this runs inside $(shell) during the parent's
    parse: inheriting the jobserver flags there makes GNU Make warn
    "jobserver unavailable" on every single invocation and leak descriptors.
    --no-print-directory is mandatory, not tidiness -- "Entering directory"
    goes to stdout and would be captured as part of the value.
    """
    cmd = ["make", "--no-print-directory", "-C", str(openboot / "firmware")]
    cmd += args + [goal]
    proc = subprocess.run(
        cmd, capture_output=True, text=True, env={"PATH": _path(), "MAKEFLAGS": ""})
    if proc.returncode != 0:
        raise RuntimeError(
            f"`make {goal}` failed in the OpenBoot submodule "
            f"(exit {proc.returncode}); is it checked out?\n{proc.stderr.strip()}")
    return proc.stdout.strip()


def _path() -> str:
    import os
    return os.environ.get("PATH", "/usr/bin:/bin")


def parse_defines(text: str, names: tuple[str, ...]) -> dict[str, int]:
    """Pull `#define NAME 0x...` values out of a C header."""
    found: dict[str, int] = {}
    for name in names:
        match = re.search(
            rf"^\s*#\s*define\s+{re.escape(name)}\s+(0[xX][0-9a-fA-F]+|\d+)",
            text, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"{name} not found; OpenBoot's layout changed")
        found[name] = int(match.group(1), 0)
    return found


def geometry(openboot: Path,
             make_args: list[str]) -> tuple[int, int, int, int, str]:
    """Return (slot_base, slot_size, capacity, record_size, image_path)."""
    image = run_make(openboot, make_args, "print-image-path")
    if not image:
        raise RuntimeError("print-image-path produced nothing")
    # The caller splits this line on whitespace, so a path containing any
    # would silently truncate every field after it. Refuse rather than emit
    # something the Makefile would misparse into a wrong slot size.
    if len(image.split()) != 1:
        raise RuntimeError(f"bootloader image path contains whitespace: {image!r}")
    # print-image-path's contract is that the sibling artifacts share the
    # directory, so this is supported rather than a guess at build_dir's
    # template -- which is exactly the duplication print-image-path exists
    # to retire.
    build_dir = Path(image).parent
    config = build_dir / CONFIG_HEADER
    run_make(openboot, make_args, str(config))

    cfg = parse_defines(
        config.read_text(),
        ("OB_SLOT_A_BASE", "OB_SLOT_B_BASE", "OB_SLOT_SIZE", "OB_FLASH_APP_END"))
    proto = parse_defines(
        (openboot / "protocol" / "openboot_protocol.h").read_text(),
        ("OB_BOOT_RECORD_SIZE",))
    # Not hardcoded 4096: openboot_app.c computes the record address with this
    # value, so if it ever moves, this must move with it.
    app = parse_defines(
        (openboot / "firmware" / "app" / "openboot_app.c").read_text(),
        ("OPENBOOT_ERASE_BLOCK",))

    base = cfg["OB_SLOT_A_BASE"]
    size = cfg["OB_SLOT_SIZE"]
    erase = app["OPENBOOT_ERASE_BLOCK"]
    record = proto["OB_BOOT_RECORD_SIZE"]

    # Cross-checks. Each of these would otherwise fail silently at runtime, on
    # hardware, as a wrong record address rather than a build error.
    if cfg["OB_SLOT_B_BASE"] - base != size:
        raise RuntimeError(
            f"slots are not contiguous: B 0x{cfg['OB_SLOT_B_BASE']:X} "
            f"- A 0x{base:X} != size 0x{size:X}")
    if size <= erase:
        raise RuntimeError(
            f"slot size 0x{size:X} leaves no room for the record block 0x{erase:X}")
    if cfg["OB_SLOT_B_BASE"] + size > cfg["OB_FLASH_APP_END"]:
        raise RuntimeError(
            f"slot B 0x{cfg['OB_SLOT_B_BASE']:X}+0x{size:X} overruns the app "
            f"region end 0x{cfg['OB_FLASH_APP_END']:X}")
    if record > erase:
        raise RuntimeError(
            f"record 0x{record:X} does not fit its erase block 0x{erase:X}")
    return base, size, size - erase, record, image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--openboot", type=Path, required=True,
                        help="path to the OpenBoot submodule root")
    parser.add_argument("--chip", required=True)
    parser.add_argument("--transport", default="usb")
    parser.add_argument("--board", required=True)
    args = parser.parse_args()

    make_args = [f"CHIP={args.chip}", f"TRANSPORT={args.transport}",
                 f"BOARD={args.board}"]
    try:
        base, size, capacity, record, image = geometry(args.openboot, make_args)
    except (RuntimeError, OSError) as exc:
        print(f"openboot geometry unavailable: {exc}", file=sys.stderr)
        return 1
    print(f"0x{base:X} 0x{size:X} 0x{capacity:X} 0x{record:X} {image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
