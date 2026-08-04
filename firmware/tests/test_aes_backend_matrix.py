"""Build-level invariants for the AES backends. No device, no cross toolchain.

These are text assertions over the Makefiles and headers, deliberately not
compilations. `make test` must keep working on a machine with neither the
RISC-V toolchain nor a dongle attached, so anything needing `riscv-wch-elf-gcc`
belongs in the hardware suite instead.

Text matching is normally a poor way to test a build, but these particular
invariants are ORDERING and PRESENCE facts that a compile would not reveal:

  - A source file silently absent from `APP_SRC` compiles nothing and fails
    nothing. `ch592/src/hal_aes_ch592.c` was in no build at all until recently
    -- it looked finished, reviewed clean, and had never once been assembled.
  - A flag added BELOW the `BUILD_ID :=` line still changes the binary but not
    its identity, so two different images claim the same id. That is not a
    cosmetic problem here: the hardware validation runner flashes an arm and
    then verifies the build id before trusting the result. If backend selection
    did not move the id, the runner would happily confirm it was running ASM_F
    while the device actually held ASM_A, and report a pass for a backend it
    never executed.

Both of those are invisible to a successful compile, which is why they are
pinned here rather than left to the build.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest

import aes_vectors

ROOT = Path(__file__).resolve().parents[1]
CH570_MAKEFILE = ROOT / "ch570" / "Makefile"
CH592_MAKEFILE = ROOT / "ch592" / "Makefile"
IMPL_H = ROOT / "ch570" / "src" / "hal_aes_ch570_impl.h"
CORECFGR_H = ROOT / "ch570" / "src" / "ch570_corecfgr.h"
RESET_S = ROOT / "ch570" / "src" / "reset_handler_ch570.S"
HAL_AES_H = ROOT / "common" / "include" / "hal_aes.h"


def _lines(path):
    return path.read_text().splitlines()


def _first_index(lines, predicate, what):
    for i, line in enumerate(lines):
        if predicate(line):
            return i
    raise AssertionError(f"{what} not found")


def define_value(path, name):
    """Read a plain `#define NAME <int>` out of a defines-only header."""
    m = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(0[xX][0-9a-fA-F]+|\d+)\b",
        path.read_text(),
        re.MULTILINE,
    )
    if m is None:
        raise AssertionError(f"{name} not defined in {path.name}")
    return int(m.group(1), 0)


def corecfgr_value():
    return define_value(CORECFGR_H, "CH570_CORECFGR_VALUE")


def asm_f_is_available():
    """Whether the ASM_F backend can be built with the CURRENT startup value.

    The hardware suite uses this to decide whether the ASM_F arm runs or is
    reported as skipped. It must be derived from the header, never hard-coded,
    or it would go stale the moment CORECFGR changes.
    """
    return bool(corecfgr_value() & define_value(CORECFGR_H, "CH570_CORECFGR_ROM_LOOP_ACC"))


def documented_cycles(path):
    """Pull {backend: cycles/block} out of a cost table in a comment block.

    Matches only genuine table rows: a backend name, an optional parenthetical
    like "(default)", then the number. Requiring the digits to follow the name
    directly is what keeps surrounding prose out -- both headers contain
    sentences that begin "ASM_A is the default because it is 25x faster..."
    and "C is kept permanently...", which a looser pattern happily misreads as
    a cost of 25 (or, since a comma alone is not a number, as nothing at all).
    """
    found = {}
    for line in _lines(path):
        m = re.match(
            r"^\s*\*\s+(ASM_A|ASM_F|C)\b(?:\s*\([a-z]+\))?\s+(\d[\d,]*)\s", line)
        if m:
            found.setdefault(m.group(1), int(m.group(2).replace(",", "")))
    return found


class BuildWiring(unittest.TestCase):
    """Every AES source is actually compiled, and actually feeds the build id."""

    def test_ch592_driver_is_compiled(self):
        """Regression pin for a driver that was dead code.

        `hal_aes_ch592.c` existed, was reviewed, and was referenced in the
        docs -- and appeared in no Makefile, so it had never been built. A
        compile cannot catch this; only its absence from APP_SRC can.
        """
        text = CH592_MAKEFILE.read_text()
        self.assertRegex(
            text,
            r"(?m)^APP_SRC\s*\+?=.*\bsrc/hal_aes_ch592\.c\b",
            msg="hal_aes_ch592.c is not in APP_SRC: the CH592 AES backend "
                "would not be built into the firmware at all",
        )

    def test_ch570_aes_sources_are_compiled(self):
        text = CH570_MAKEFILE.read_text()
        for src in ("src/hal_aes_ch570.c", "aes_sw.c"):
            with self.subTest(source=src):
                self.assertRegex(text, rf"(?m)^APP_SRC\s*\+?=.*{re.escape(src)}",
                                 msg=f"{src} missing from CH570 APP_SRC")
        self.assertRegex(
            text, r"(?m)^ASM_SRC\s*:?=.*\bsrc/hal_aes_ch570_asm\.S\b",
            msg="the CH570 assembly kernel is not assembled",
        )

    def test_the_asm_kernel_feeds_the_build_id(self):
        """`$(ASM_SRC)` is not covered by `$(APP_SRC)` and needs listing.

        Without it, editing either hand-written AES kernel changes the firmware
        while leaving its build identity untouched -- and the build id is what
        every "is the running image the expected one?" check compares.
        """
        text = CH570_MAKEFILE.read_text()
        self.assertRegex(
            text, r"(?m)^BUILD_ID_INPUTS\s*:?\+?=.*\$\(ASM_SRC\)",
            msg="$(ASM_SRC) missing from BUILD_ID_INPUTS",
        )

    def test_ch592_driver_is_listed_before_the_build_id_is_computed(self):
        """BUILD_ID_INPUTS expands `$(APP_SRC)`, so ordering decides coverage.

        An `APP_SRC +=` below the `BUILD_ID :=` line compiles the file but
        leaves it out of the identity hash.
        """
        lines = _lines(CH592_MAKEFILE)
        added = _first_index(
            lines, lambda ln: re.match(r"^APP_SRC\s*\+?=.*hal_aes_ch592\.c", ln),
            "the APP_SRC line adding hal_aes_ch592.c")
        computed = _first_index(
            lines, lambda ln: ln.startswith("BUILD_ID :="), "the BUILD_ID := line")
        self.assertLess(
            added, computed,
            "hal_aes_ch592.c is added to APP_SRC after BUILD_ID is computed, so "
            "editing the AES driver would not move the build id",
        )


class BackendSelectionIsVisibleInTheBuildId(unittest.TestCase):
    """Choosing a different CH570 backend must produce a different build id.

    This is the invariant the hardware runner's flash-then-verify gate rests
    on. If it broke, the runner would confirm the expected id, run the wrong
    backend, and report a pass for code it never executed -- the exact
    ambiguity that spoiled an earlier A/B campaign.
    """

    def setUp(self):
        self.lines = _lines(CH570_MAKEFILE)

    def test_the_selector_reaches_the_compiler(self):
        self.assertRegex(
            CH570_MAKEFILE.read_text(),
            r"(?m)^CFLAGS\s*\+=\s*-DDONGLE_AES_CH570_IMPL=\$\(AES_IMPL\)",
            msg="AES_IMPL is not passed through to the compiler",
        )

    def test_cflags_are_hashed_into_the_build_id(self):
        """The `-D` only moves the id because CFLAGS is an input to the tool."""
        self.assertRegex(
            CH570_MAKEFILE.read_text(),
            r"--flags\s+'CFLAGS=\$\(CFLAGS\)'",
            msg="CFLAGS is not hashed into the build id, so a backend change "
                "would produce a different binary under the same id",
        )

    def test_the_selector_is_set_before_the_id_is_computed(self):
        selector = _first_index(
            self.lines,
            lambda ln: ln.startswith("CFLAGS += -DDONGLE_AES_CH570_IMPL="),
            "the AES_IMPL CFLAGS line")
        computed = _first_index(
            self.lines, lambda ln: ln.startswith("BUILD_ID :="),
            "the BUILD_ID := line")
        self.assertLess(
            selector, computed,
            "the AES backend selector is added to CFLAGS after the build id is "
            "computed, so all three backends would share one id",
        )


class BackendSelector(unittest.TestCase):
    """The selector header's own guarantees."""

    def setUp(self):
        self.text = IMPL_H.read_text()

    def test_three_backends_have_distinct_values(self):
        values = {
            name: define_value(IMPL_H, f"DONGLE_AES_CH570_IMPL_{name}")
            for name in ("ASM_A", "ASM_F", "C")
        }
        self.assertEqual(len(set(values.values())), 3, f"duplicate ids: {values}")

    def test_default_is_asm_a(self):
        self.assertRegex(
            self.text,
            r"#ifndef\s+DONGLE_AES_CH570_IMPL\s*\n\s*#define\s+"
            r"DONGLE_AES_CH570_IMPL\s+DONGLE_AES_CH570_IMPL_ASM_A",
            msg="the default CH570 AES backend is no longer ASM_A",
        )

    def test_an_unknown_backend_is_rejected_at_compile_time(self):
        """A typo'd AES_IMPL must not silently fall through to no kernel."""
        self.assertIn("#error", self.text)
        self.assertRegex(
            self.text,
            r"#if\s+DONGLE_AES_CH570_IMPL\s*!=.*ASM_A.*&&",
            msg="no compile-time check that AES_IMPL is one of the three",
        )

    def test_asm_f_is_gated_on_the_value_startup_actually_writes(self):
        """The guard must test CH570_CORECFGR_VALUE, not an acknowledgement.

        ASM_F is bit-exact correct without ROM_LOOP_ACC but roughly 15x slower
        than its documented cost. A guard keyed to a separate "I promise bit 3
        is set" macro would prevent the accident and still be able to lie; one
        keyed to the value the reset handler writes cannot.
        """
        self.assertRegex(
            self.text,
            r"#if\s+DONGLE_AES_CH570_IMPL\s*==\s*DONGLE_AES_CH570_IMPL_ASM_F\s*&&\s*\\?\s*\n?"
            r".*CH570_CORECFGR_VALUE.*CH570_CORECFGR_ROM_LOOP_ACC",
            msg="the ASM_F guard does not test CH570_CORECFGR_VALUE",
        )

    def test_the_guard_reflects_the_current_startup_value(self):
        """Whatever CORECFGR is today, the derived availability must agree.

        Passes in both directions: it pins the relationship, not the value, so
        flipping CORECFGR to 0x2D does not make this test wrong -- it makes
        `asm_f_is_available()` start returning True, which is the point.
        """
        expected = bool(corecfgr_value() & 0x08)
        self.assertEqual(asm_f_is_available(), expected)


class CorecfgrHasOneDefinition(unittest.TestCase):
    """The ASM_F guard is only meaningful if startup uses the same constant."""

    def test_the_reset_handler_includes_the_shared_header(self):
        self.assertRegex(
            RESET_S.read_text(), r'#include\s+"ch570_corecfgr\.h"',
            msg="reset_handler_ch570.S does not include ch570_corecfgr.h",
        )

    def test_the_reset_handler_writes_the_shared_constant_not_a_literal(self):
        """A literal here would let startup and the guard disagree silently."""
        text = RESET_S.read_text()
        self.assertRegex(
            text, r"li\s+\w+,\s*CH570_CORECFGR_VALUE",
            msg="the CORECFGR value is not taken from the shared header",
        )
        m = re.search(r"csrw\s+0xbc0,\s*(\w+)", text)
        self.assertIsNotNone(m, "no csrw to CORECFGR (0xbc0) in the reset handler")

    def test_ie_remap_en_is_set(self):
        """Clearing bit 5 makes CSR 0x800 read-only, which silently breaks
        __enable_irq/__disable_irq and this firmware's own csrrs/csrrc on it.
        ch32fun ships 0x0f/0x1f, which do exactly that -- never copy them."""
        value = corecfgr_value()
        remap = define_value(CORECFGR_H, "CH570_CORECFGR_IE_REMAP_EN")
        self.assertTrue(
            value & remap,
            f"CH570_CORECFGR_VALUE={value:#04x} clears IE_REMAP_EN: interrupt "
            "enable/disable would stop working",
        )


class DocumentedCosts(unittest.TestCase):
    """The two cost tables must not drift apart."""

    def test_the_two_cost_tables_agree(self):
        """`hal_aes.h` and `hal_aes_ch570_impl.h` both publish cycles/block.

        They have already drifted once (3,796 vs 3,797). A caller budgeting
        against the seam header and a maintainer reading the backend header
        must not get different numbers.
        """
        seam = documented_cycles(HAL_AES_H)
        impl = documented_cycles(IMPL_H)
        self.assertEqual(set(seam), {"ASM_A", "ASM_F", "C"},
                         f"unexpected backends in hal_aes.h table: {sorted(seam)}")
        self.assertEqual(seam, impl,
                         "the cost tables in hal_aes.h and hal_aes_ch570_impl.h "
                         "disagree")

    def test_the_costs_are_ordered_as_documented(self):
        """ASM_A fastest, then ASM_F, then portable C by a wide margin."""
        c = documented_cycles(IMPL_H)
        self.assertLess(c["ASM_A"], c["ASM_F"])
        self.assertLess(c["ASM_F"], c["C"])

    def test_the_backend_header_cites_the_shared_checksum(self):
        """The impl header quotes the differential fold as its correctness
        claim; it must be the same number the host suite re-derives."""
        self.assertIn(aes_vectors.EXPECTED_FOLD, IMPL_H.read_text())


if __name__ == "__main__":
    unittest.main()
