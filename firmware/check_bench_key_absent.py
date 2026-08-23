"""Refuse a release whose artifacts contain the compiled-in bench key.

The bench profile bakes a shared throwaway AES key into the image
(DONGLE_CRYPT_BENCH_KEY_BYTES in ch592/src/dongle_target.h). A release
artifact carrying those bytes would accept forged keystrokes from anyone who
has read the source, so the release target scans every packaged binary for
them and fails the build on a hit.

This is the byte-level backstop behind the compile-time tripwire in
rf_task.c: the tripwire catches the misconfigured build, this catches the
mispackaged one (a stale object tree, a bench image copied over a product
path, a Makefile regression in the profile plumbing).

The needle is parsed out of the header rather than duplicated here, so
rotating the bench key cannot silently blunt the check. Every artifact
pattern must match at least one existing file - a scan that finds nothing to
scan is a failure, not a pass.
"""

from __future__ import annotations

import argparse
import glob
import re
import sys
from pathlib import Path

MACRO = "DONGLE_CRYPT_BENCH_KEY_BYTES"
KEY_LEN = 16


def parse_bench_key(header_text: str) -> bytes:
    """Extract the key initializer bytes from the dongle_target.h text.

    Textual on purpose: the macro may sit behind an #if, but the bytes it
    names are the bytes a misbuilt image would carry, so preprocessing
    context is irrelevant to the scan.
    """
    m = re.search(MACRO + r"\s*\\?\s*\{([^}]*)\}", header_text, re.DOTALL)
    if not m:
        raise ValueError(f"{MACRO} initializer not found in header")
    key = bytes(int(tok, 16) for tok in re.findall(r"0[xX][0-9a-fA-F]{1,2}", m.group(1)))
    if len(key) != KEY_LEN:
        raise ValueError(f"{MACRO} has {len(key)} bytes, expected {KEY_LEN}")
    return key


def scan(key: bytes, patterns: list[str]) -> list[str]:
    """Return a list of failure messages; empty means every artifact is clean."""
    failures: list[str] = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if not matches:
            failures.append(f"no artifact matches '{pattern}' - nothing was scanned")
            continue
        for path in matches:
            data = Path(path).read_bytes()
            offset = data.find(key)
            if offset >= 0:
                failures.append(f"{path}: bench key found at offset 0x{offset:X}")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--header",
        required=True,
        type=Path,
        help="dongle_target.h that defines %s" % MACRO,
    )
    ap.add_argument(
        "artifacts",
        nargs="+",
        help="artifact paths or globs; each must match at least one file",
    )
    args = ap.parse_args()

    key = parse_bench_key(args.header.read_text())
    failures = scan(key, args.artifacts)
    for f in failures:
        print(f"check_bench_key_absent: {f}", file=sys.stderr)
    if failures:
        return 1
    print(f"check_bench_key_absent: {len(args.artifacts)} pattern(s) clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
