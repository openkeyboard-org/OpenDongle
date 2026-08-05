#!/usr/bin/env python3
"""Run OpenBoot's A/B bench harness against THIS bench, without editing it.

OpenBoot ships firmware/tests/bench/ab_bench.py: a fresh part reports
active=none/write=A, an image based at the active slot is refused before any
erase, and -- the acceptance test -- power is cut part-way through an update
and the part must come back running the previous application unaided.

We do not edit the vendored copy. A dirty submodule fails
`check_dependencies.py --expect-revision`, which is the gate asserting that the
bytes being validated ARE the pinned revision's; editing the harness to test
the pin would defeat the thing the harness is being run to establish. Its CHIPS
table is also declared bench-local upstream ("change CHIPS to match yours"), so
it is not upstreamable as-is either.

Instead this imports the pinned module and replaces module globals. Every call
site resolves `ab_bench.mc` at call time, so patching it intercepts all of them.

What is patched, and why each one is a correctness fix rather than a
preference:

  W1  minichlink argument order. The harness emits `minichlink -l <serial>
      <action>`. minichlink.c computes skip_startup from argv[1] ONLY, so with
      -l first every invocation runs SetupInterface before reaching the action
      -- and LESetupInterface asserts ndmreset (DMCONTROL 0x80000003). That
      injects a target reset into the very observations the evidence chain is
      built from, and it cannot act on a part that is not currently responding,
      which is exactly the state a power-cut test creates. Action first, with
      -k only on -t/-3: -E/-w/-r genuinely need DetermineChipType and must NOT
      skip init.

  W2  Assert on the exit status. The harness discards it. A silently failed
      power cut turns the acceptance test into a test of nothing that still
      reports PASS -- the single most dangerous failure mode here, because it
      fails green.

  W3  Resolve the serial port from the probe serial via /dev/serial/by-id
      rather than a hardcoded /dev/ttyACM<n>. The numbering is assignment
      order, so a replug silently repoints it -- and opening a WCH-Link CDC
      port RESETS whatever target is attached to that probe.

  W4  Refuse any probe not explicitly allowed. Structural, not procedural:
      this bench carries a CH570 whose dongle must never be written.

  W5  Derive the frame count for the "whole image" case. Upstream passes a
      literal 2, and each write is 16 bytes, so it covers 32 bytes of a witness
      that is larger than that -- meaning the strongest pre-COMMIT case (slot
      fully written, record not yet stored) is never actually exercised.

  W6  Read back and compare after writing the bootloader. The harness's
      factory() writes with no verify, which contradicts every other flash
      recipe in both repositories; minichlink does not check its own writes.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
OPENBOOT = REPO / "third_party" / "openboot"
HARNESS = OPENBOOT / "firmware" / "tests" / "bench" / "ab_bench.py"

# W4: only these probes may ever be driven. CF148F065446 (CH570) is deliberately
# absent -- that path reaches a paired production dongle.
ALLOWED_PROBES = {
    "C2228F064754": "ch572 bench part",
    "CEBD8F0653EF": "ch592 bench part",
}

# W2: minichlink exits 0 in cases where it never reached the chip, so the exit
# status alone is not enough. These are the strings that mean "no target".
FAILURE_MARKERS = (
    "Could not setup interface",
    "Chip Type unknown",
    "link error",
    "marchid : ffffffff",
)


def load_harness():
    if not HARNESS.is_file():
        sys.exit(f"harness not found: {HARNESS}\n"
                 "run: git submodule update --init --recursive")
    spec = importlib.util.spec_from_file_location("ab_bench", HARNESS)
    module = importlib.util.module_from_spec(spec)
    sys.modules["ab_bench"] = module
    spec.loader.exec_module(module)
    return module


def port_for(serial: str) -> str:
    """W3: the stable by-id symlink, derived from the same serial as the probe."""
    link = Path(f"/dev/serial/by-id/usb-wch.cn_WCH-Link_{serial}-if01")
    if not link.exists():
        sys.exit(f"no CDC port for probe {serial} at {link}; is it attached?")
    return os.path.realpath(link)


class MinichlinkError(RuntimeError):
    pass


def make_mc(module, minichlink: str, dry_run: bool):
    """Build the replacement for ab_bench.mc (W1, W2)."""

    def mc(cfg, *args, t=90):
        serial = cfg["serial"]
        if serial not in ALLOWED_PROBES:
            raise MinichlinkError(
                f"probe {serial} is not in the allow-list; refusing to drive it")
        action, rest = args[0], list(args[1:])
        # W1: -k ONLY for the power rail. -E/-w/-r need DetermineChipType.
        if action in ("-t", "-3"):
            action = "-k" + action.lstrip("-")
        # The action leads (minichlink derives skip_startup from argv[1]) and
        # -l trails, AFTER the action's own operands: -w takes <file> <addr>
        # and -r takes <file> <addr> <len> positionally, so slipping "-l
        # <serial>" between them would feed "-l" to -w as a filename.
        cmd = [minichlink, action, *rest, "-l", serial]
        if dry_run:
            print("  would run:", " ".join(cmd))
            return subprocess.CompletedProcess(cmd, 0, "", "")
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=t)
        # W2
        blob = f"{proc.stdout}\n{proc.stderr}"
        if proc.returncode != 0:
            raise MinichlinkError(
                f"{' '.join(cmd)} -> exit {proc.returncode}\n{blob.strip()}")
        for marker in FAILURE_MARKERS:
            if marker in blob:
                raise MinichlinkError(
                    f"{' '.join(cmd)} exited 0 but never reached the chip "
                    f"({marker!r})\n{blob.strip()}")
        return proc

    return mc


def patch_factory(module, minichlink: str):
    """W6: read back and compare the bootloader after writing it."""
    original = module.factory

    def factory(cfg):
        original(cfg)
        image = Path(cfg["boot"])
        if not image.is_file():
            raise MinichlinkError(f"bootloader image missing: {image}")
        readback = image.with_suffix(".readback.bin")
        module.mc(cfg, "-r", str(readback), "0x0", str(image.stat().st_size))
        if readback.read_bytes() != image.read_bytes():
            raise MinichlinkError(
                f"bootloader readback differs from {image} - the write did not land")
        print(f"  [ok] bootloader verified by readback ({image.stat().st_size} B)")

    module.factory = factory


def whole_image_frames(module, cfg) -> int:
    """W5: frames needed to cover the entire witness, not a literal 2."""
    size = Path(cfg["boot"]).stat().st_size
    return (size + 15) // 16


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--chip", choices=["ch572", "ch592"], required=False,
                        help="restrict to one chip (default: every allowed probe)")
    parser.add_argument("--minichlink",
                        default=os.path.expanduser(
                            "~/Development/Personal/WCH/ch32fun/minichlink/minichlink"))
    parser.add_argument("--dry-run", action="store_true",
                        help="print every command without contacting a device")
    parser.add_argument("--scenario", default="all",
                        choices=["all", "lifecycle", "interrupted", "recovery"])
    args = parser.parse_args()

    os.environ.setdefault("OPENBOOT_ROOT", str(OPENBOOT))
    module = load_harness()

    # W7: the harness talks OBP; a 0.1 binary cannot HELLO a 0.2 bootloader.
    cli = OPENBOOT / "tools" / "target" / "release" / "openboot"
    if not cli.is_file() and not args.dry_run:
        sys.exit(f"build the OBP 0.2 CLI first: cargo build --release "
                 f"--manifest-path {OPENBOOT}/tools/Cargo.toml")

    module.mc = make_mc(module, args.minichlink, args.dry_run)
    patch_factory(module, args.minichlink)

    # W3/W4: rewrite the bench-local CHIPS table for THIS bench.
    for name, cfg in list(module.CHIPS.items()):
        if cfg["serial"] not in ALLOWED_PROBES:
            del module.CHIPS[name]
            continue
        cfg["port"] = port_for(cfg["serial"])
        print(f"{name}: probe {cfg['serial']} -> {cfg['port']}")

    if not module.CHIPS:
        sys.exit("no allowed probes present")

    targets = [args.chip] if args.chip else list(module.CHIPS)
    failures = []
    for name in targets:
        cfg = module.CHIPS[name]
        print(f"\n=== {name} ===")
        try:
            if args.scenario in ("all", "lifecycle"):
                module.scenario_lifecycle(name)
            if args.scenario in ("all", "interrupted"):
                module.scenario_interrupted(name, 0, "after ERASE only")
                module.scenario_interrupted(name, 1, "after ERASE + 1 write")
                frames = whole_image_frames(module, cfg)
                module.scenario_interrupted(
                    name, frames,
                    f"after ERASE + the whole image ({frames} frames), before COMMIT")
            if args.scenario in ("all", "recovery"):
                module.scenario_recovery(name)
        except MinichlinkError as exc:
            print(f"  PROBE FAILURE: {exc}", file=sys.stderr)
            failures.append(f"{name}: probe")
        failures.extend(f"{name}: {f}" for f in getattr(module, "fails", []))

    print("\n=== RESULT ===")
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
