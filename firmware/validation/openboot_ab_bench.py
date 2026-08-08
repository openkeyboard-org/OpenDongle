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

  W1, W2, W5 and W6 are all UPSTREAM now, and none are renumbered - the gaps
  record what this wrapper used to carry. W1 (action-first argv, -k on the
  power rail) and W2 (raise on a non-zero exit, and on a zero exit carrying a
  "never reached the chip" marker) landed in OpenBoot#12; W5 and W6 landed in
  e957c4c. mc() below now DELEGATES to the pinned implementation rather than
  restating those rules, so there is one copy of each.

  W3  Resolve the serial port from the probe serial via /dev/serial/by-id
      rather than a hardcoded /dev/ttyACM<n>. The numbering is assignment
      order, so a replug silently repoints it -- and opening a WCH-Link CDC
      port RESETS whatever target is attached to that probe.

  W4  Refuse any probe not explicitly allowed. Structural, not procedural:
      this bench carries a CH570 whose dongle must never be written.


  W7  Refuse to start unless the OBP 0.2 CLI has been built. The device
      requires an exact protocol major+minor match, so a 0.1 binary cannot
      HELLO this bootloader at all -- and the resulting timeouts look exactly
      like a dead target rather than a stale tool.

  W8  Make --dry-run cover the openboot CLI as well as minichlink. The harness
      reaches hardware two ways: mc() and run(). Patching only mc() left
      `openboot flash ... --force` executing for real under the flag whose
      whole job is preventing that.

  W9  Make reboot() land in the bootloader deterministically for the lifecycle
      scenario, which does a bare power cycle then a SINGLE probe and expects
      the bootloader to answer. Whether it does is phase-dependent; see the
      function for the measured boot-request alternation.

  W10 Refuse to start unless every target's bench bootloader has been built,
      and report the harness's own assertions as failures rather than as
      tracebacks. W7's rule applied to the other prerequisite and to the other
      exception types: a missing image or a busy CDC port is an OSError, and
      upstream asserts with a bare RuntimeError, none of which the probe
      handler catches. See check_boot_images() and main()'s except clauses.
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


def load_harness():
    if not HARNESS.is_file():
        sys.exit(f"harness not found: {HARNESS}\n"
                 "run: git submodule update --init --recursive")
    spec = importlib.util.spec_from_file_location("ab_bench", HARNESS)
    module = importlib.util.module_from_spec(spec)
    sys.modules["ab_bench"] = module
    spec.loader.exec_module(module)
    return module


def port_for(serial: str, *, require_present: bool = True) -> str:
    """W3: the stable by-id symlink, derived from the same serial as the probe.

    require_present=False is for --dry-run, whose whole point is inspecting the
    command sequence with no bench attached. Returning the by-id path unresolved
    is honest there: it is deterministic, and it is what would be opened.
    """
    link = Path(f"/dev/serial/by-id/usb-wch.cn_WCH-Link_{serial}-if01")
    if not link.exists():
        if not require_present:
            return str(link)
        sys.exit(f"no CDC port for probe {serial} at {link}; is it attached?")
    return os.path.realpath(link)


class MinichlinkError(RuntimeError):
    pass


def make_mc(module, minichlink: str, dry_run: bool):
    """Wrap upstream's mc() with the two things that remain ours.

    W1 and W2 are UPSTREAM now (openkeyboard-org/OpenBoot#12), so this no
    longer reimplements argument ordering or exit-status checking - it calls
    the pinned mc() and lets it own both. Reimplementing them here would mean
    two copies of a rule that has to match minichlink's argv[1] behaviour, and
    the copy nobody runs is the one that rots.

    What is still local:
      W4  the probe allow-list, checked BEFORE anything is executed
      W8  dry-run interception (upstream has no notion of a dry run) - of the
          EXECUTION only. The command is still upstream's, built by the same
          code the live path runs, so what a dry run prints cannot drift from
          what a real run would do.

    Delegating moved the argv construction into the harness, and with it the
    choice of binary: upstream's mc() reads its module-level MC. So --minichlink
    has to be pushed INTO the module rather than closed over. Before delegation
    this wrapper built the argv itself and honoured the flag by construction;
    after it, the live path used upstream's default while only the dry-run print
    reflected the flag. That is the most misleading shape a bug can take here -
    the flag looks correct exactly when you test it the cheap way - and it hid
    because our default string is character-for-character upstream's default,
    so it is only observable when someone actually overrides it.
    """
    module.MC = minichlink
    original = module.mc

    def mc(cfg, *args, t=90, **kw):
        serial = cfg["serial"]
        if serial not in ALLOWED_PROBES:
            raise MinichlinkError(
                f"probe {serial} is not in the allow-list; refusing to drive it")
        # A dry run delegates too. It used to rebuild the argv here in order to
        # print it, which put a SECOND copy of upstream's rule -- the -k
        # transform and "-l trails the action's own operands" -- on the one
        # path that never executes it. That is the exact hazard this docstring
        # names, reintroduced by the thing meant to remove it, and the copies
        # had already drifted: upstream refuses a call with no action (it would
        # put an option at argv[1] and silently disable skip_startup), while
        # the local copy printed `minichlink -l <serial>` and reported success.
        #
        # Executing is prevented by swapping the module's run() rather than by
        # branching before it, because upstream's mc() resolves run from module
        # globals at call time. Swapping it HERE rather than relying on
        # patch_run_for_dry_run() keeps this closure self-contained: a dry run
        # cannot execute even if that patch was never applied.
        saved_run = module.run
        if dry_run:
            module.run = print_only
        try:
            return original(cfg, *args, t=t, **kw)
        except RuntimeError as exc:
            # Upstream raises RuntimeError; keep this module's error type so
            # main()'s handler stays one except clause.
            raise MinichlinkError(str(exc)) from exc
        finally:
            module.run = saved_run

    return mc


def print_only(cmd, t=90):
    """Show a command and report success without running it.

    The single point where --dry-run turns an intent into output, shared by the
    mc() wrapper and the openboot CLI path so that "what a dry run prints" has
    one definition rather than one per call site.
    """
    print("  would run:", " ".join(str(c) for c in cmd))
    return subprocess.CompletedProcess(cmd, 0, "", "")


def patch_run_for_dry_run(module):
    """W8: make --dry-run cover the openboot CLI too, not just minichlink.

    The harness reaches hardware two ways: mc() for minichlink, and run() for
    the openboot CLI. Patching only mc() left `openboot flash ... --force` -
    the single most destructive command here - executing for real under
    --dry-run, which is precisely the flag whose job is to prevent that. It
    survived earlier testing only because probe() returns nothing in dry-run,
    so the scenarios crashed before reaching a flash.
    """
    module.run = print_only


def stub_factory_for_dry_run(module):
    """Dry-run only: skip upstream's factory() readback comparison.

    Upstream now reads the bootloader back and raises on mismatch - which is
    the behaviour this wrapper used to add, and the reason it no longer does.
    Under --dry-run the commands are printed rather than executed, so no
    readback file exists and the comparison would see zero bytes.

    Be explicit about what this costs: a dry run VERIFIES NOTHING about the
    factory write. The -r line it prints carries placeholder operands and is a
    command-sequence illustration, not a simulated flash - so a dry run cannot
    detect a failed or partial bootloader write, and is not evidence that a
    real run would land one. That is the same reason the final verdict says
    "no verdict" rather than PASS.
    """
    def factory(cfg):
        module.mc(cfg, "-E")
        module.mc(cfg, "-w", cfg["boot"], "0x0")
        # Placeholder operands: illustrative only, see the docstring.
        module.mc(cfg, "-r", "<readback-path>", "0x0", "<bootloader-len>")
        module.power_cycle(cfg)

    module.factory = factory


def patch_reboot_for_lifecycle(module):
    """W9: make reboot() land in the bootloader deterministically.

    scenario_lifecycle does `reboot(cfg)` then a single `probe(cfg)` and
    expects the BOOTLOADER to answer. Whether it does is phase-dependent:
    measured on this bench over SWD, the boot-request word at 0x200067F0 goes
    0xB007CA11 -> 0x00000000 -> 0xB007CA11 as the bootloader consumes the magic
    and the application re-arms it. That is the same alternation
    enter_bootloader() documents and retries four times for -- but the
    lifecycle path does not use it, so a single shot can land on the
    application half, probe times out, and every later check fails as a
    cascade.

    A real power cut makes this materially more likely than a reset that leaves
    SRAM intact, because the magic does not survive the cut: the part boots the
    application first, and only the NEXT reset can be caught in the bootloader.
    So this matters more once the cut actually cuts.

    Only used for lifecycle. scenario_interrupted deliberately requires the
    APPLICATION to be running after a cut, so it keeps the original.
    """
    original = module.reboot

    def reboot(cfg, tries=4):
        for _ in range(tries):
            original(cfg)
            if module.probe(cfg)[0]:
                return
        # Leave it to the scenario's own check to report the mismatch.

    module.reboot = reboot
    return original





def build_hint(image: Path) -> str:
    """Reconstruct the make invocation that produces a bench bootloader.

    Upstream's build dir is `<chip>-<transport>[+<board>]`, so the recipe can be
    derived from the path the CHIPS table already names rather than restated
    here and left to drift against it.
    """
    name = image.parent.name
    head, _, board = name.partition("+")
    chip, _, transport = head.partition("-")
    goal = f"CHIP={chip} TRANSPORT={transport}"
    if board:
        goal += f" BOARD={board}"
    return (f"make -C {OPENBOOT}/firmware {goal} "
            f"MRS_TOOLCHAIN=<gcc12-bin> image")


def check_boot_images(module, names) -> None:
    """W10: refuse to start when a bench bootloader has not been built.

    Same rule as W7, applied to the other prerequisite. Upstream's factory()
    opens cfg["boot"] as its very first statement, before any minichlink call,
    so a missing image is a FileNotFoundError - an OSError, which is neither a
    MinichlinkError nor caught anywhere - and the run ends in a bare traceback
    pointing at a line inside the vendored harness. That reads like the harness
    is broken when the actual meaning is "you have not built this yet".

    Checked for every target up front rather than per scenario, because the
    scenarios are destructive: without this, `--scenario all` can whole-chip
    erase the first part, run for minutes, and only then discover that the
    second chip's image was never built.
    """
    missing = [(n, Path(module.CHIPS[n]["boot"])) for n in names
               if not Path(module.CHIPS[n]["boot"]).is_file()]
    if not missing:
        return
    lines = ["bench bootloader not built:"]
    for name, image in missing:
        lines.append(f"  {name}: {image}")
        lines.append(f"    {build_hint(image)}")
    sys.exit("\n".join(lines))


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

    # The scenarios name witness images by bare filename ("ch592-A.bin"), so
    # they resolve against the CWD -- upstream's README runs them from the
    # bench directory. Run from anywhere else and `openboot flash` exits 1 on a
    # missing file, which the harness reports as "flash into slot A succeeds:
    # 1" and every later check then fails as a cascade.
    os.chdir(module.HERE)

    module.mc = make_mc(module, args.minichlink, args.dry_run)
    if args.dry_run:
        patch_run_for_dry_run(module)
        stub_factory_for_dry_run(module)

    # W3/W4: rewrite the bench-local CHIPS table for THIS bench.
    for name, cfg in list(module.CHIPS.items()):
        if cfg["serial"] not in ALLOWED_PROBES:
            del module.CHIPS[name]
            continue
        cfg["port"] = port_for(cfg["serial"], require_present=not args.dry_run)
        print(f"{name}: probe {cfg['serial']} -> {cfg['port']}")

    if not module.CHIPS:
        sys.exit("no allowed probes present")

    # A chip the allow-list filtered out would otherwise reach CHIPS[name] below
    # and die with a bare KeyError traceback, which reads like a crash rather
    # than "that probe is not on this bench".
    if args.chip and args.chip not in module.CHIPS:
        parser.error(
            f"--chip {args.chip} is not available here; "
            f"present: {', '.join(sorted(module.CHIPS)) or 'none'}")

    targets = [args.chip] if args.chip else list(module.CHIPS)

    # Not under --dry-run: nothing is opened there (factory() is stubbed), and
    # inspecting the command sequence without a built tree is a thing worth
    # being able to do - same reasoning as the port check above.
    if not args.dry_run:
        check_boot_images(module, targets)

    failures = []
    for name in targets:
        cfg = module.CHIPS[name]
        print(f"\n=== {name} ===")
        try:
            if args.scenario in ("all", "lifecycle"):
                original_reboot = patch_reboot_for_lifecycle(module)
                try:
                    module.scenario_lifecycle(name)
                finally:
                    module.reboot = original_reboot
            if args.scenario in ("all", "interrupted"):
                module.scenario_interrupted(name, 0, "after ERASE only")
                module.scenario_interrupted(name, 1, "after ERASE + 1 write")
                # writes=None means "the whole image" upstream now, derived
                # from the witness rather than a literal frame count.
                module.scenario_interrupted(
                    name, None, "after ERASE + the whole image, before COMMIT")
            if args.scenario in ("all", "recovery"):
                module.scenario_recovery(name)
        except MinichlinkError as exc:
            print(f"  PROBE FAILURE: {exc}", file=sys.stderr)
            failures.append(f"{name}: probe")
        except TypeError:
            # Dry-run feeds the scenarios empty command output, so the first
            # check that needs a parsed device response gets None and the
            # harness raises. That is the end of what dry-run can show; it
            # validates command CONSTRUCTION, not scenario logic.
            if not args.dry_run:
                raise
            print("  (dry run: stopped where a real device response is needed)")
            continue
        except OSError as exc:
            # pyserial's SerialException IS an OSError, raised when the CDC
            # port is busy or has been repointed by a replug - the failure W3
            # exists to make rare, not one it can eliminate. Also covers any
            # file the harness opens mid-scenario. Not a verdict about the
            # firmware, so it is named as a bench fault rather than a FAIL
            # anyone might read as "the bootloader is broken".
            print(f"  BENCH FAILURE: {exc}", file=sys.stderr)
            failures.append(f"{name}: bench")
        except RuntimeError as exc:
            # Upstream asserts with a bare RuntimeError, and MinichlinkError
            # subclasses it - so this must stay BELOW the clause above or it
            # would swallow probe failures and mislabel them.
            #
            # The one that matters is factory()'s readback mismatch: minichlink
            # ignores the return values of its CH5xx erase/write calls, so a
            # zero exit is not evidence the image landed, and upstream raises
            # when the bootloader reads back wrong. That is the single most
            # important signal this harness produces - it means the part has no
            # working bootloader - and it was reaching the operator as a
            # traceback, which is how a real finding gets mistaken for a bug in
            # the script.
            print(f"  HARNESS FAILURE: {exc}", file=sys.stderr)
            failures.append(f"{name}: harness")
        failures.extend(f"{name}: {f}" for f in getattr(module, "fails", []))

    print("\n=== RESULT ===")
    if args.dry_run:
        # Never print PASS here. A dry run contacts nothing, so every check
        # either did not run or was fed empty output; reporting a pass would
        # be indistinguishable from a real one in a log.
        print("DRY RUN - commands shown, nothing executed, no verdict")
        return 0
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
