#!/usr/bin/env python3
"""Calculate a reproducible 32-bit identity for a firmware source/config set."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def calculate(config_text: str, paths: list[Path],
              flags: list[str] | None = None) -> str:
    """Identity over the things that decide the binary.

    `flags` carries the actual compiler and linker flags. Without them the
    identity covered only source contents and the hand-maintained config text,
    so changing an optimisation level or adding a -D produced a DIFFERENT binary
    under the SAME build id unless someone remembered to bump the config text by
    hand. That id is what every "is the running build the expected one?" check
    compares, so a
    collision there is not cosmetic.

    Passing no flags reproduces the previous hash exactly, so this is additive:
    the id only moves for a caller that actually supplies them.
    """
    by_name: dict[str, Path] = {}
    for path in paths:
        name = path.name
        if name in by_name and by_name[name].resolve() != path.resolve():
            raise ValueError(f"duplicate build-input basename: {name}")
        by_name[name] = path

    digest = hashlib.sha256(b"opendongle-build-id-v1\0")
    digest.update(config_text.encode("utf-8"))
    digest.update(b"\0")
    for item in (flags or []):
        digest.update(b"flags\0")
        digest.update(item.encode("utf-8"))
        digest.update(b"\0")
    for name, path in sorted(by_name.items()):
        data = path.read_bytes()
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()[:8].upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-text", required=True)
    parser.add_argument(
        "--flags", action="append", default=[], metavar="NAME=VALUE",
        help="a flag set that affects codegen, e.g. CFLAGS=...; repeatable. "
             "Order is significant and comes from the caller.")
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    print(calculate(args.config_text, args.inputs, args.flags))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
