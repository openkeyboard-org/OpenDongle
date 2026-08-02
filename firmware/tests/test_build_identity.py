"""The build id must identify the BINARY, not just the sources.

It is the value every "is the running build the expected one?" check compares -
every "is the running build the expected one?" check. For a long
time it hashed only source contents plus a hand-maintained CONFIG_TEXT string,
so changing an optimisation level or adding a -D produced a different binary
under the same id unless someone remembered to bump that string by hand. The
CH592 -DBLE_SNV=FALSE change depended on exactly that discipline.

These tests pin the property rather than the discipline.
"""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from build_identity import calculate  # noqa: E402

CONFIG = "schema=1||chip=test"


class Flags(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name)
        self.src = d / "a.c"
        self.src.write_text("int main(void){return 0;}\n")
        self.inputs = [self.src]

    def id_for(self, flags=None):
        return calculate(CONFIG, self.inputs, flags)

    def test_optimisation_level_changes_the_id(self):
        self.assertNotEqual(
            self.id_for(["CFLAGS=-O2"]), self.id_for(["CFLAGS=-O0"]),
            "-O level does not move the build id, so two different binaries "
            "would share one identity")

    def test_a_define_changes_the_id(self):
        self.assertNotEqual(
            self.id_for(["CFLAGS=-O2"]), self.id_for(["CFLAGS=-O2 -DFOO=1"]),
            "a -D does not move the build id; this is the CH592 BLE_SNV case")

    def test_linker_flags_change_the_id(self):
        self.assertNotEqual(
            self.id_for(["LDFLAGS=-Wl,--gc-sections"]),
            self.id_for(["LDFLAGS="]),
            "linker flags do not move the build id")

    def test_flag_identity_is_stable(self):
        """Same inputs must give the same id, or nothing downstream can compare."""
        self.assertEqual(self.id_for(["CFLAGS=-O2 -g"]),
                         self.id_for(["CFLAGS=-O2 -g"]))

    def test_absent_flags_reproduce_the_historical_id(self):
        """The change is additive: a caller passing no flags is unaffected, so
        this could not silently invalidate anything that does not opt in."""
        self.assertEqual(self.id_for(), self.id_for([]))

    def test_source_contents_still_matter(self):
        before = self.id_for(["CFLAGS=-O2"])
        self.src.write_text("int main(void){return 1;}\n")
        self.assertNotEqual(before, self.id_for(["CFLAGS=-O2"]),
                            "source contents no longer move the id")

    def test_config_text_still_matters(self):
        self.assertNotEqual(
            calculate(CONFIG, self.inputs, ["CFLAGS=-O2"]),
            calculate(CONFIG + "||x=1", self.inputs, ["CFLAGS=-O2"]))


class RealMakefilesPassTheirFlags(unittest.TestCase):
    """A unit test on calculate() proves nothing if the Makefiles never pass
    --flags. Pin the wiring, in the order that keeps it non-circular."""

    def chips(self) -> list[str]:
        """Chips present in this tree; the ports land in separate commits."""
        chips = sorted(path.parent.name for path in ROOT.glob("ch*/Makefile"))
        if not chips:
            self.skipTest("no chip ports in this tree yet")
        return chips

    def makefile(self, chip: str) -> list[str]:
        return (ROOT / chip / "Makefile").read_text().splitlines()

    def test_both_makefiles_pass_cflags_and_ldflags(self):
        for chip in self.chips():
            with self.subTest(chip=chip):
                text = "\n".join(self.makefile(chip))
                self.assertIn("--flags 'CFLAGS=$(CFLAGS)'", text,
                              f"{chip} does not hash its CFLAGS")
                self.assertIn("--flags 'LDFLAGS=$(LDFLAGS)'", text,
                              f"{chip} does not hash its LDFLAGS")

    def test_build_id_is_computed_before_it_is_defined_into_cflags(self):
        """If -DDONGLE_BUILD_ID were added to CFLAGS before the id is computed,
        the id would hash a value derived from itself and never settle."""
        for chip in self.chips():
            with self.subTest(chip=chip):
                lines = self.makefile(chip)
                compute = next(i for i, l in enumerate(lines)
                               if l.startswith("BUILD_ID :="))
                define = next(i for i, l in enumerate(lines)
                              if "-DDONGLE_BUILD_ID=" in l)
                self.assertLess(
                    compute, define,
                    f"{chip}: build id is defined into CFLAGS at line "
                    f"{define + 1} before being computed at line "
                    f"{compute + 1} - the hash would feed itself")

    def test_no_codegen_flags_are_added_after_the_id_is_computed(self):
        """Anything appended to CFLAGS after the computation is invisible to the
        identity. Only the build-id define itself is allowed there."""
        for chip in self.chips():
            with self.subTest(chip=chip):
                lines = self.makefile(chip)
                compute = next(i for i, l in enumerate(lines)
                               if l.startswith("BUILD_ID :="))
                late = [(i + 1, l) for i, l in enumerate(lines)
                        if i > compute
                        and (l.startswith("CFLAGS +=") or l.startswith("CFLAGS :="))
                        and "-DDONGLE_BUILD_ID=" not in l]
                self.assertEqual(
                    [], late,
                    f"{chip}: CFLAGS changed after the build id was computed, so "
                    f"these flags are not part of the identity: {late}")


if __name__ == "__main__":
    unittest.main()
