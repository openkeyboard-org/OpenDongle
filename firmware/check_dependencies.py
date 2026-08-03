#!/usr/bin/env python3
"""Validate pinned SDK and MounRiver compiler inputs for firmware builds."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys


# MounRiver "RISC-V Embedded GCC15" (WCH GCC 15.2.0). Note the tool prefix
# changed with this release: GCC12 shipped riscv-wch-elf-*, GCC15 ships
# riscv32-wch-elf-*. TOOL_PREFIX below is the single place that encodes it.
PINNED_COMPILER_SHA256 = (
    "9527827d2004aaddfeb3ecac030d0a0ec19678e9601e3ffdb18f9a3100b9bd99"
)
PINNED_COMPILER_VERSION = "15.2.0"
TOOL_PREFIX = "riscv32-wch-elf"


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


def validate_toolchain(toolchain_value: str, tool_prefix: str) -> None:
    """Validate the toolchain the build will ACTUALLY use.

    The prefix is a parameter rather than the module constant because the
    application Makefiles expose it as CROSS. If this validated a hardcoded
    prefix while the build used an overridden one, check-deps would pass having
    inspected a different compiler from the one that compiles the firmware --
    which is worse than not checking at all, because it looks like assurance.
    """
    if not toolchain_value.strip():
        fail("MRS_TOOLCHAIN is required and must name the GCC15 bin directory")
    toolchain = Path(toolchain_value).expanduser().resolve()
    # nm belongs here: the fault validators consume it via --tool-dir, so
    # check-deps passing without it just moves the failure to the middle of
    # a build.
    for suffix in ("gcc", "objcopy", "size", "nm"):
        name = f"{tool_prefix}-{suffix}"
        executable = toolchain / name
        if not executable.is_file() or not os.access(executable, os.X_OK):
            fail(f"MounRiver tool is missing or not executable: {executable}")
    compiler = toolchain / f"{tool_prefix}-gcc"
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
    if PINNED_COMPILER_VERSION not in version:
        fail(f"compiler is not the required GCC {PINNED_COMPILER_VERSION}: {version}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--sdk-revision", required=True)
    parser.add_argument("--toolchain", required=True)
    # Defaults to the pinned prefix so an older caller keeps working; the
    # Makefiles pass $(CROSS) so an override is validated rather than bypassed.
    parser.add_argument("--tool-prefix", default=TOOL_PREFIX)
    args = parser.parse_args()
    try:
        validate_sdk(args.sdk, args.sdk_revision)
        validate_toolchain(args.toolchain, args.tool_prefix)
    except RuntimeError as exc:
        print(f"dependency check failed: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
