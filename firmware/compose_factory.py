#!/usr/bin/env python3
"""Compose a whole-chip factory image: OpenBoot, 0x00 pad, application.

The application links at APP_BASE and the bootloader owns [0, APP_BASE), so the
image is the OpenBoot binary padded out to APP_BASE followed by the application
binary. The application therefore sits at the same file offset as its load
address, and the whole image can be written at flash address 0.

The pad byte MUST be 0x00, never 0xFF: on CH5xx, programming 0xFF programs
nothing, so an 0xFF pad would never land in flash and the post-flash readback
compare would fail across the pad region.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import tempfile

# python3 -I strips the script's directory from sys.path (see finalize_image.py).
sys.path.insert(0, os.fspath(Path(__file__).resolve().parent))

from dongle_image_id import APP_BASE  # noqa: E402

# OpenBoot owns everything below the application's load address.
OPENBOOT_REGION_BYTES = APP_BASE


def compose_factory(openboot: bytes, app: bytes) -> bytes:
    if not 0 < len(openboot) <= OPENBOOT_REGION_BYTES:
        raise ValueError(
            f"OpenBoot image must be 1..{OPENBOOT_REGION_BYTES} bytes, "
            f"got {len(openboot)}")
    if not app:
        raise ValueError("application image is empty")
    return openboot + b"\x00" * (OPENBOOT_REGION_BYTES - len(openboot)) + app


def write_atomic(path: Path, data: bytes) -> None:
    """Replace in one step: an interrupted compose must not leave a short image
    that the next `make` treats as up to date and someone then flashes."""
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--openboot", type=Path, required=True,
                        help="OpenBoot bootloader binary, loaded at 0")
    parser.add_argument("--app", type=Path, required=True,
                        help="finalized application binary, loaded at APP_BASE")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        factory = compose_factory(args.openboot.read_bytes(),
                                  args.app.read_bytes())
    except ValueError as exc:
        print(f"factory composition failed: {exc}", file=sys.stderr)
        return 2
    write_atomic(args.output, factory)
    print(f"factory image: {args.output} ({len(factory)} B)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
