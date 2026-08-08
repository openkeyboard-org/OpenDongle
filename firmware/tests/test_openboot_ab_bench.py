"""Pin the wrapper contract in `validation/openboot_ab_bench.py`.

This wrapper exists to add bench-local safety to OpenBoot's pinned A/B harness
without editing the submodule. It has now lost a behaviour TWICE, both times by
the same mechanism: a patch was moved or delegated, and the live path silently
stopped doing what the dry-run path still appeared to do.

  - `--dry-run` once covered `mc()` but not `run()`, so `openboot flash --force`
    executed for real under the flag whose entire job is to prevent that.
  - `--minichlink` stopped reaching the live path when `make_mc()` was changed
    to delegate to upstream's `mc()`, which resolves the binary from its OWN
    module-level `MC`. Only the dry-run print still reflected the flag.

Both were invisible to the obvious test. The second is the sharper lesson: this
bench's `--minichlink` default is character-for-character upstream's default, so
running the harness the normal way exercises the right binary by coincidence.
The flag was only broken when someone actually overrode it -- which is the one
case a bench operator reaches for when they have a second minichlink build and
most need to trust which one ran.

So the invariant under test is deliberately not "the flag is stored somewhere".
It is: **the binary a dry run PRINTS is the binary a live run EXECUTES.** A dry
run is a safety instrument, and one that prints a command other than the one it
would run is worse than no dry run at all.

These tests never contact hardware: `run` is stubbed, so nothing is executed,
and the allow-list is satisfied with a probe serial taken from the wrapper
itself rather than hardcoded here.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "validation" / "openboot_ab_bench.py"
HARNESS = ROOT.parent / "third_party" / "openboot" / "firmware" / "tests" / "bench" / "ab_bench.py"

CUSTOM = "/nonexistent/custom/minichlink"


def load_wrapper():
    spec = importlib.util.spec_from_file_location("openboot_ab_bench", WRAPPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(
    HARNESS.is_file(),
    # An environment skip, NOT a skip of the thing under test: the wrapper
    # imports the pinned harness by path, so with an uninitialised submodule
    # there is no delegation target to assert against. Guarded on the harness
    # file rather than on the call failing, so a genuine breakage in
    # load_harness() still fails loudly instead of silently skipping.
    f"OpenBoot submodule not initialised ({HARNESS} absent)",
)
class MinichlinkSelection(unittest.TestCase):
    """--minichlink must reach the command that actually runs."""

    def setUp(self):
        self.wrapper = load_wrapper()
        self.module = self.wrapper.load_harness()
        self.serial = sorted(self.wrapper.ALLOWED_PROBES)[0]
        self.captured = []
        # Upstream mc() shells out through the module-level run(). Replacing it
        # keeps this test off hardware while still exercising the real argv
        # construction, which is the part that regressed.
        self.module.run = self._fake_run

    def _fake_run(self, cmd, t=90):
        self.captured.append(list(cmd))
        return types.SimpleNamespace(stdout="", stderr="", returncode=0)

    def test_live_path_uses_the_selected_binary(self):
        """The regression: live runs used upstream's default, not the flag."""
        mc = self.wrapper.make_mc(self.module, CUSTOM, dry_run=False)
        mc({"serial": self.serial}, "-A")

        self.assertEqual(len(self.captured), 1, "expected exactly one command")
        self.assertEqual(
            self.captured[0][0], CUSTOM,
            "live path ignored --minichlink and fell back to upstream's MC")

    # Every argv shape the bench actually uses. `-A` alone is not a test: it is
    # the one form where an independent reconstruction trivially agrees, having
    # no -k transform and no positional operands. The forms that catch drift are
    # -t/-3 (rewritten to -kt/-k3, because power control must not need a live
    # chip) and -w/-r, whose operands are positional -- so a wrapper that
    # inserted "-l <serial>" before them would hand "-l" to -w as a filename.
    FORMS = (
        ("-A",),
        ("-E",),
        ("-t",),
        ("-3",),
        ("-w", "/tmp/img.bin", "0x0"),
        ("-r", "/tmp/out.bin", "0x0", "0x100"),
    )

    def test_dry_run_prints_exactly_what_a_live_run_executes(self):
        """The invariant that makes a dry run worth trusting.

        Not "the print looks plausible" but character equality against the
        command the live path really hands to subprocess, across every form.
        A dry run is what an operator consults before touching a bench that
        also carries a CH570 they must never write, so a print that differs
        from the real command is worse than printing nothing.
        """
        for form in self.FORMS:
            with self.subTest(form=" ".join(form)):
                self.captured.clear()
                buf = io.StringIO()
                dry = self.wrapper.make_mc(self.module, CUSTOM, dry_run=True)
                with contextlib.redirect_stdout(buf):
                    dry({"serial": self.serial}, *form)
                self.assertEqual(
                    self.captured, [], "dry run executed something")

                live = self.wrapper.make_mc(self.module, CUSTOM, dry_run=False)
                live({"serial": self.serial}, *form)

                printed = buf.getvalue().split("would run:", 1)[1].strip()
                self.assertEqual(
                    printed, " ".join(self.captured[0]),
                    "the dry-run print and the executed command diverged")

    def test_power_actions_are_rewritten_to_skip_startup(self):
        """-t/-3 must reach minichlink as -kt/-k3, and only those two.

        Pinned because it is the rule most likely to drift and the one whose
        breakage is silent: without -k, minichlink runs SetupInterface, which
        asserts ndmreset and so RESETS the part a power-cut test is trying to
        observe. -E/-w/-r must NOT be rewritten -- they need DetermineChipType.
        """
        cases = {"-t": "-kt", "-3": "-k3", "-E": "-E", "-A": "-A"}
        for given, want in cases.items():
            with self.subTest(action=given):
                self.captured.clear()
                mc = self.wrapper.make_mc(self.module, CUSTOM, dry_run=False)
                mc({"serial": self.serial}, given)
                self.assertEqual(self.captured[0][1], want)

    def test_serial_trails_the_actions_own_operands(self):
        """-l must come last, never between an action and its positionals."""
        self.captured.clear()
        mc = self.wrapper.make_mc(self.module, CUSTOM, dry_run=False)
        mc({"serial": self.serial}, "-w", "/tmp/img.bin", "0x0")
        self.assertEqual(
            self.captured[0],
            [CUSTOM, "-w", "/tmp/img.bin", "0x0", "-l", self.serial])

    def test_dry_run_refuses_what_a_live_run_refuses(self):
        """Divergence found by audit: dry-run accepted a call upstream rejects.

        With no action, minichlink would get an option at argv[1] and silently
        disable skip_startup -- upstream raises rather than emit it. The local
        dry-run copy used to print `minichlink -l <serial>` and report success,
        so the safety preview was more permissive than the real thing.
        """
        for dry in (True, False):
            with self.subTest(dry_run=dry):
                mc = self.wrapper.make_mc(self.module, CUSTOM, dry_run=dry)
                with self.assertRaises(ValueError):
                    with contextlib.redirect_stdout(io.StringIO()):
                        mc({"serial": self.serial})

    def test_default_does_not_mask_a_broken_flag(self):
        """Why this went unnoticed: the default equals upstream's default.

        Asserted rather than described, so that if either default is ever
        changed independently the reason this test exists is not quietly lost.
        """
        fresh = self.wrapper.load_harness()
        parser_default = self._parser_default("--minichlink")
        self.assertEqual(
            fresh.MC, parser_default,
            "wrapper and upstream defaults have diverged -- if that is "
            "intended, this test's rationale needs rewriting")

    def _parser_default(self, flag):
        import argparse
        src = WRAPPER.read_text()
        # Read the default out of the real parser rather than duplicating the
        # literal, so the two cannot drift apart in this file.
        ns = {"os": __import__("os"), "argparse": argparse}
        start = src.index('parser.add_argument("--minichlink"')
        end = src.index("parser.add_argument", start + 1)
        stmt = src[start:end].strip().rstrip(",")
        parser = argparse.ArgumentParser()
        exec(stmt, ns, {"parser": parser})
        return parser.parse_args([]).minichlink

    def test_allow_list_is_checked_before_anything_runs(self):
        """W4 is structural protection; it must precede execution, not follow."""
        mc = self.wrapper.make_mc(self.module, CUSTOM, dry_run=False)
        with self.assertRaises(self.wrapper.MinichlinkError):
            mc({"serial": "CF148F065446"}, "-E")
        self.assertEqual(
            self.captured, [],
            "a disallowed probe reached the command layer")


class BuildHint(unittest.TestCase):
    """W10: the recipe is derived from the path, not restated beside it."""

    def setUp(self):
        self.wrapper = load_wrapper()

    def test_board_suffix_becomes_a_board_variable(self):
        hint = self.wrapper.build_hint(
            Path("/x/firmware/build/ch592-uart+bench-ch592/openboot.bin"))
        self.assertIn("CHIP=ch592", hint)
        self.assertIn("TRANSPORT=uart", hint)
        self.assertIn("BOARD=bench-ch592", hint)

    def test_default_board_emits_no_board_variable(self):
        """A BOARD= for the default board would be wrong, not merely noisy."""
        hint = self.wrapper.build_hint(
            Path("/x/firmware/build/ch572-uart/openboot.bin"))
        self.assertIn("CHIP=ch572", hint)
        self.assertIn("TRANSPORT=uart", hint)
        self.assertNotIn("BOARD=", hint)


class BootImagePreflight(unittest.TestCase):
    """W10: a missing bootloader must not surface as a traceback."""

    def setUp(self):
        self.wrapper = load_wrapper()

    def _module(self, boot_paths):
        return types.SimpleNamespace(
            CHIPS={n: {"boot": str(p)} for n, p in boot_paths.items()})

    def test_missing_image_exits_naming_the_path_and_the_recipe(self):
        module = self._module(
            {"ch592": "/nope/firmware/build/ch592-uart+bench-ch592/ob.bin"})
        with self.assertRaises(SystemExit) as caught:
            self.wrapper.check_boot_images(module, ["ch592"])
        msg = str(caught.exception)
        self.assertIn("ch592-uart+bench-ch592/ob.bin", msg)
        self.assertIn("BOARD=bench-ch592", msg,
                      "the exit did not tell the operator how to build it")

    def test_present_image_passes(self):
        module = self._module({"ch592": WRAPPER})  # any real file
        self.wrapper.check_boot_images(module, ["ch592"])

    def test_every_target_is_checked_before_any_runs(self):
        """The destructive-order point: chip 2 must not be found missing
        only after chip 1 has already been whole-chip erased."""
        module = self._module({"ch572": WRAPPER, "ch592": "/nope/b/ob.bin"})
        with self.assertRaises(SystemExit) as caught:
            self.wrapper.check_boot_images(module, ["ch572", "ch592"])
        self.assertIn("ch592", str(caught.exception))

    def test_only_targets_are_checked(self):
        """An unbuilt image for a chip we are not running is not an error."""
        module = self._module({"ch572": WRAPPER, "ch592": "/nope/b/ob.bin"})
        self.wrapper.check_boot_images(module, ["ch572"])


class ScenarioFailureHandling(unittest.TestCase):
    """W10: upstream's own assertions are results, not crashes.

    factory() raises a BARE RuntimeError when the bootloader reads back wrong,
    and MinichlinkError subclasses RuntimeError -- so the probe clause must
    stay above it or it would swallow probe failures and mislabel them. Both
    orderings are pinned here.
    """

    def setUp(self):
        self.wrapper = load_wrapper()

    def _run_with(self, exc):
        w = self.wrapper
        harness = types.SimpleNamespace(
            CHIPS={"ch592": {"serial": "CEBD8F0653EF", "boot": str(WRAPPER)}},
            HERE=str(ROOT), mc=lambda *a, **k: None, run=lambda *a, **k: None,
            fails=[], MC="unset",
        )

        def scenario_recovery(name):
            raise exc

        harness.scenario_recovery = scenario_recovery
        orig_load, orig_port = w.load_harness, w.port_for
        w.load_harness = lambda: harness
        w.port_for = lambda serial, require_present=True: "/dev/null"
        argv = sys.argv
        sys.argv = ["b", "--chip", "ch592", "--scenario", "recovery"]
        err = io.StringIO()
        try:
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(err):
                rc = w.main()
        finally:
            w.load_harness, w.port_for = orig_load, orig_port
            sys.argv = argv
        return rc, err.getvalue()

    def test_bare_runtime_error_is_reported_not_raised(self):
        rc, err = self._run_with(
            RuntimeError("factory(): bootloader readback mismatch (0 B read)"))
        self.assertEqual(rc, 1)
        self.assertIn("HARNESS FAILURE", err)
        self.assertIn("readback mismatch", err)

    def test_oserror_is_reported_not_raised(self):
        rc, err = self._run_with(OSError("could not open port /dev/ttyACM0"))
        self.assertEqual(rc, 1)
        self.assertIn("BENCH FAILURE", err)

    def test_probe_failure_keeps_its_own_label(self):
        """MinichlinkError must not be captured by the RuntimeError clause."""
        rc, err = self._run_with(
            self.wrapper.MinichlinkError("probe CF14 is not in the allow-list"))
        self.assertEqual(rc, 1)
        self.assertIn("PROBE FAILURE", err)
        self.assertNotIn("HARNESS FAILURE", err)


if __name__ == "__main__":
    unittest.main()
