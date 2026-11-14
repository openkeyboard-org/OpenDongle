#!/usr/bin/env python3
# Copyright 2026 Eric Molitor (EMulator)
# SPDX-License-Identifier: Apache-2.0
"""Validate pinned SDK inputs and the properties of the MounRiver compiler.

The compiler gate checks PROPERTIES, not bytes:

  * the tool prefix the build uses is present (gcc, objcopy, size, nm);
  * the driver reports the required GCC major (15 for the application);
  * the driver implements ``__attribute__((interrupt("WCH-Interrupt-fast")))``
    -- the WCH fork's fast-interrupt ABI that the CH59x BLE library was
    compiled against. Accepting the spelling is not enough (a partial port
    could accept it and emit the ordinary ABI), so the probe compiles a handler
    to assembly and checks the GENERATED CODE: a fast handler lets the hardware
    preserve ra/t0/a0 and spills only the callee-saved s0 before ``mret``,
    while the ordinary ``interrupt`` ABI spills all four. A compiler that emits
    the ordinary shape for the fast attribute, or the same shape for both, is
    refused. Mainline GCC and clang reject the argument outright.

The compiler's self-reported identity (``--version`` first line, including the
WCH build tag) is what the chip Makefiles fold into ``CONFIG_TEXT`` and hence
into the build id, so a different compiler build yields a DIFFERENT build id
instead of a refused build. The driver's SHA-256 is printed for the build log
and the factory manifest; ``--expect-compiler-sha256`` turns it back into a
hard gate for anyone who needs byte-reproducible releases.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


# MounRiver "RISC-V Embedded GCC15" (WCH GCC 15.x). Note the tool prefix
# changed with this release: GCC12 shipped riscv-wch-elf-*, GCC15 ships
# riscv32-wch-elf-*.
#
# TOOL_PREFIX is only the DEFAULT. The prefix the build actually uses comes from
# CROSS in firmware/Makefile and in each chip Makefile, and is passed in via
# --tool-prefix; this constant applies when no caller supplies one. Keeping the
# validated prefix and the compiling prefix the same is the entire point --
# validating a hardcoded prefix while the build used an overridden one would
# look like assurance while checking a different compiler.
REQUIRED_COMPILER_MAJOR = 15
TOOL_PREFIX = "riscv32-wch-elf"
FAST_IRQ_ATTRIBUTE = "WCH-Interrupt-fast"
# Default probe target when the caller passes no --probe-flags: a conservative
# arch every WCH RISC-V toolchain accepts. The chip Makefiles pass their own
# $(ARCH) so the probe compiles for the product target (CH570's -march carries
# the WCH-only "xw" extension, which is itself a property worth exercising).
DEFAULT_PROBE_FLAGS = ("-march=rv32imac", "-mabi=ilp32")
# -O2 -fomit-frame-pointer -mno-save-restore: no frame, no libcall prologues,
# so the only stores are the register spills the interrupt ABI itself demands.
PROBE_CODEGEN_FLAGS = ("-O2", "-fomit-frame-pointer", "-mno-save-restore", "-Wall", "-Wattributes", "-Werror", "-S")
# The clobber forces every one of ra/t0/a0/s0 to be considered live across the
# handler: the fast ABI (hardware push/pop of the caller-saved set) spills only
# s0; the ordinary ABI spills all four.
PROBE_SOURCE_TEMPLATE = (
    "__attribute__(({attr})) void probe_isr(void) {{\n"
    '    __asm__ volatile ("" ::: "ra", "t0", "a0", "s0");\n'
    "}}\n"
)
HW_SAVED = ("ra", "t0", "a0")


def fail(message: str) -> None:
    """Abort the check with a message; main() prints it and exits 2."""
    raise RuntimeError(message)


def run_git(sdk: Path, *args: str) -> str:
    """Run a git query inside the SDK checkout and return its trimmed output."""
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
    """Require the SDK checkout to be initialised, at the pinned revision, and clean."""
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


def compiler_version_line(compiler: Path) -> str:
    """The driver's first --version line: its identity (name, vendor tag, version)."""
    try:
        line = subprocess.run(
            [os.fspath(compiler), "--version"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.splitlines()[0].strip()
    except (OSError, subprocess.CalledProcessError, IndexError) as exc:
        fail(f"cannot identify compiler {compiler}: {exc}")
        line = ""
    if not line:
        fail(f"cannot identify compiler {compiler}: --version printed a blank first line")
    return line


def compiler_numeric_version(compiler: Path) -> str:
    """The numeric GCC version (``-dumpfullversion``, falling back to
    ``-dumpversion``, which GCC allows to be a bare major): the options GCC
    provides for exactly this purpose. The ``--version`` banner is kept for
    identity only -- a vendor tag there can carry its own dotted number, so it
    is not parsed for the major."""
    for flag in ("-dumpfullversion", "-dumpversion"):
        result = subprocess.run(
            [os.fspath(compiler), flag],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        value = result.stdout.strip()
        if result.returncode == 0 and re.fullmatch(r"\d+(?:\.\d+){0,2}", value):
            return value
    fail(f"cannot read a numeric GCC version (-dumpfullversion/-dumpversion) from {compiler}")
    return ""  # unreachable: fail() raises


def compiler_major(numeric_version: str) -> int:
    """The major component of a numeric GCC version such as ``15.2.0`` or ``15``."""
    return int(numeric_version.split(".")[0])


def _spills(asm: str) -> set[str]:
    """Registers the handler stores to the stack (``sw <reg>,<off>(sp)``)."""
    return set(re.findall(r"^\s*sw\s+([a-z][a-z0-9]*)\s*,\s*-?\d+\(sp\)", asm, re.M))


def _compile_to_asm(compiler: Path, attr: str, probe_flags: tuple[str, ...]) -> str:
    """Compile the probe handler under ``attr`` to assembly text, or fail."""
    with tempfile.TemporaryDirectory(prefix="wch-probe-") as tmp:
        src = Path(tmp) / "probe.c"
        out = Path(tmp) / "probe.s"
        src.write_text(PROBE_SOURCE_TEMPLATE.format(attr=attr))
        result = subprocess.run(
            [os.fspath(compiler), *probe_flags, *PROBE_CODEGEN_FLAGS,
             os.fspath(src), "-o", os.fspath(out)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        asm = out.read_text() if out.is_file() else ""
    if result.returncode != 0 or not asm.strip():
        diag = (result.stderr or result.stdout).strip().splitlines()
        detail = "\n  ".join(diag[:6]) if diag else f"exit status {result.returncode}"
        fail(
            f"compiler cannot build a __attribute__(({attr})) handler: {compiler}\n  {detail}\n"
            "the CH59x BLE library is compiled against WCH's fast-interrupt ABI; "
            "only the MounRiver/WCH GCC fork implements it (mainline GCC and clang "
            "reject the argument), and a plain-interrupt build wedges before any "
            "IRQ is delivered"
        )
    return asm


def probe_fast_interrupt(compiler: Path, probe_flags: tuple[str, ...]) -> None:
    """Refuse a compiler that does not IMPLEMENT the WCH fast-interrupt ABI."""
    fast = _compile_to_asm(compiler, f'interrupt("{FAST_IRQ_ATTRIBUTE}")', probe_flags)
    plain = _compile_to_asm(compiler, "interrupt", probe_flags)
    fast_spills = _spills(fast)
    plain_spills = _spills(plain)
    problems = []
    if not re.search(r"^\s*mret\b", fast, re.M):
        problems.append("no mret: the handler does not return from machine mode")
    if "s0" not in fast_spills:
        problems.append("s0 is not preserved: the clobbered callee-saved register was not spilled")
    hw = sorted(fast_spills & set(HW_SAVED))
    if hw:
        problems.append(
            f"{', '.join(hw)} spilled in software: the ordinary interrupt ABI, not the "
            "hardware push/pop of the WCH fast ABI"
        )
    if not (set(HW_SAVED) <= plain_spills):
        problems.append(
            "the ordinary `interrupt` control does not spill ra/t0/a0 either, so the two "
            "ABIs are indistinguishable in this compiler's output"
        )
    if problems:
        fail(
            f"compiler accepts __attribute__((interrupt(\"{FAST_IRQ_ATTRIBUTE}\"))) but "
            f"does not implement the WCH fast-interrupt ABI: {compiler}\n  "
            + "\n  ".join(problems)
            + f"\n  fast handler spills: {sorted(fast_spills) or '-'}; ordinary handler "
              f"spills: {sorted(plain_spills) or '-'}\n"
            "the CH59x BLE library depends on that ABI; a plain-interrupt build "
            "wedges before any IRQ is delivered"
        )


def validate_toolchain(
    toolchain_value: str,
    tool_prefix: str,
    required_major: int,
    expect_sha256: str | None,
    probe_flags: tuple[str, ...],
) -> str:
    """Validate the toolchain the build will ACTUALLY use; return its identity.

    The prefix is a parameter rather than the module constant because the
    application Makefiles expose it as CROSS. If this validated a hardcoded
    prefix while the build used an overridden one, check-deps would pass having
    inspected a different compiler from the one that compiles the firmware --
    which is worse than not checking at all, because it looks like assurance.
    """
    if not toolchain_value.strip():
        fail("MRS_TOOLCHAIN is required and must name the MounRiver GCC bin directory")
    # An ABSOLUTE prefix would make this check inspect a different file from the
    # one the build compiles with: pathlib discards the left operand when the
    # right is absolute (Path("/opt/mrs") / "/tmp/gcc" == Path("/tmp/gcc")),
    # while the Makefiles concatenate as strings and get "/opt/mrs//tmp/gcc".
    # Only absolute prefixes diverge -- a relative one such as "sub/riscv32-wch-elf"
    # resolves identically both ways, so rejecting every separator would ban a
    # form that works.
    if Path(tool_prefix).is_absolute():
        fail(
            f"tool prefix must be relative, got {tool_prefix!r}: an absolute "
            "prefix makes this check validate a different compiler from the "
            "one the build uses"
        )
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
    version = compiler_version_line(compiler)
    numeric = compiler_numeric_version(compiler)
    major = compiler_major(numeric)
    if major != required_major:
        fail(
            f"compiler is GCC {major} ({numeric}), this target requires GCC "
            f"{required_major}: {version}\n  {compiler}"
        )
    probe_fast_interrupt(compiler, probe_flags)
    try:
        digest = hashlib.sha256(compiler.read_bytes()).hexdigest()
    except OSError as exc:
        fail(f"cannot read the compiler driver to record its digest: {compiler}: {exc}")
    if expect_sha256 is not None and digest != expect_sha256:
        fail(
            f"compiler SHA-256 is {digest}, expected {expect_sha256}: {compiler}\n"
            "--expect-compiler-sha256 asks for byte-reproducible artifacts from "
            "ONE MounRiver release on ONE host platform (the gcc driver only, not "
            "cc1/as/ld/objcopy); drop the flag to build with any WCH GCC of the "
            "required major, at the cost of a different build id"
        )
    print(f"compiler: {version} [{numeric}] (sha256 {digest}; probe {' '.join(probe_flags)})")
    return version


def main() -> int:
    """Parse the command line and run the SDK and toolchain checks; exit 2 on failure."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--sdk-revision", required=True)
    parser.add_argument("--toolchain", required=True)
    # Defaults to the GCC15 prefix so an older caller keeps working; the
    # Makefiles pass $(CROSS) so an override is validated rather than bypassed.
    parser.add_argument("--tool-prefix", default=TOOL_PREFIX)
    parser.add_argument(
        "--compiler-major",
        type=int,
        default=REQUIRED_COMPILER_MAJOR,
        help="required GCC major of the application compiler (default %(default)s)",
    )
    parser.add_argument(
        "--probe-flags",
        default=" ".join(DEFAULT_PROBE_FLAGS),
        help="target flags for the fast-interrupt probe, e.g. the chip's -march/-mabi "
             "(default: %(default)s)",
    )
    parser.add_argument(
        "--expect-compiler-sha256",
        default=None,
        help="also require this exact gcc-driver digest (byte-reproducible releases)",
    )
    args = parser.parse_args()
    expect = None
    if args.expect_compiler_sha256 is not None:
        # A digest copied out of a manifest may carry whitespace or upper-case
        # hex; normalise it, and refuse anything that is not one SHA-256.
        expect = args.expect_compiler_sha256.strip().lower()
        if not re.fullmatch(r"[0-9a-f]{64}", expect):
            print(
                f"dependency check failed: --expect-compiler-sha256 must be 64 hex "
                f"digits, got {args.expect_compiler_sha256!r}",
                file=sys.stderr,
            )
            return 2
    try:
        validate_sdk(args.sdk, args.sdk_revision)
        validate_toolchain(
            args.toolchain,
            args.tool_prefix,
            args.compiler_major,
            expect,
            tuple(args.probe_flags.split()),
        )
    except RuntimeError as exc:
        print(f"dependency check failed: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
