#!/usr/bin/env python3
"""Validate pinned SDK and MounRiver compiler inputs for firmware builds."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys


PINNED_COMPILER_SHA256 = (
    "7f2d3c114b98fe9e48ac6abe6259a4574291a8e2aba960b21dce73528ece9ff2"
)


def fail(message: str) -> None:
    raise RuntimeError(message)


def run_git(sdk: Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", os.fspath(sdk), *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(f"cannot inspect SDK checkout {sdk}: {exc}")
    return result.stdout.strip()


def validate_sdk(sdk: Path, revision: str) -> None:
    if not sdk.is_dir():
        fail(
            f"SDK is missing: {sdk}\n"
            "initialize submodules with: git submodule update --init --recursive"
        )
    root = Path(run_git(sdk, "rev-parse", "--show-toplevel"))
    actual = run_git(root, "rev-parse", "HEAD")
    if actual != revision:
        fail(f"SDK {root} is at {actual}, expected {revision}")
    # Untracked files count as dirty. They were excluded, which meant a stray
    # header dropped into the SDK could be picked up by an include path and
    # change the build while this gate still reported a clean, pinned checkout -
    # the one thing it exists to rule out. The revision check above cannot see
    # them either, because an untracked file does not move HEAD.
    status = run_git(root, "status", "--porcelain", "--untracked-files=normal")
    if status:
        listed = "\n  ".join(status.splitlines()[:10])
        fail(f"SDK checkout is dirty: {root}\n  {listed}")


def validate_toolchain(toolchain_value: str) -> None:
    if not toolchain_value.strip():
        fail("MRS_TOOLCHAIN is required and must name the GCC12 bin directory")
    toolchain = Path(toolchain_value).expanduser().resolve()
    # nm belongs here: the fault validators consume it via --tool-dir, so
    # check-deps passing without it just moves the failure to the middle of
    # a build.
    for name in ("riscv-wch-elf-gcc", "riscv-wch-elf-objcopy",
                 "riscv-wch-elf-size", "riscv-wch-elf-nm"):
        executable = toolchain / name
        if not executable.is_file() or not os.access(executable, os.X_OK):
            fail(f"MounRiver tool is missing or not executable: {executable}")
    compiler = toolchain / "riscv-wch-elf-gcc"
    digest = hashlib.sha256(compiler.read_bytes()).hexdigest()
    if digest != PINNED_COMPILER_SHA256:
        fail(
            f"compiler SHA-256 is {digest}, expected {PINNED_COMPILER_SHA256}: "
            f"{compiler}\n"
            "the pin names ONE MounRiver release built for ONE host platform, and "
            "covers the gcc driver only (not cc1/as/ld/objcopy)\n"
            "see firmware/README.md, 'The toolchain pin', before changing it: the "
            "build id advertises this digest, so artifacts built against another "
            "toolchain are not the bytes that id implies"
        )
    try:
        version = subprocess.run(
            [os.fspath(compiler), "--version"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.splitlines()[0]
    except (OSError, subprocess.CalledProcessError, IndexError) as exc:
        fail(f"cannot identify compiler {compiler}: {exc}")
    if "12.2.0" not in version:
        fail(f"compiler is not the required GCC 12.2.0: {version}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--sdk-revision", required=True)
    parser.add_argument("--toolchain", required=True)
    args = parser.parse_args()
    try:
        validate_sdk(args.sdk, args.sdk_revision)
        validate_toolchain(args.toolchain)
    except RuntimeError as exc:
        print(f"dependency check failed: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
