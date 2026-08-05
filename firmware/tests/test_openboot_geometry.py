"""OpenBoot A/B slot wiring: the values this build shares with the bootloader.

Replaces test_compose_factory.py. Factory composition moved into OpenBoot when
it stopped being a concatenation -- the image now carries slot A's boot record
so a blank part boots unattended -- and OpenDongle's own composer went with it.
What is left to pin here is the coupling that composition used to hide: this
build hands openboot_app.c two -D values that MUST equal what the bootloader
was built with, and links into a slot whose size it does not choose.

Every check below is for a failure that is otherwise silent. A wrong
OPENBOOT_SLOT_SIZE moves the app's boot-record address into the neighbouring
slot and compiles cleanly. A stale OPENBOOT_REVISION composes a factory image
from an unpinned bootloader with every gate green -- the gap firmware/Makefile
documented for a year.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
OPENBOOT = ROOT.parent / "third_party" / "openboot"
GEOMETRY_TOOL = ROOT / "openboot_geometry.py"

if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# chip -> the OpenBoot board file that build uses.
BOARDS = {"ch570": "opendongle-ch570", "ch592": "opendongle-ch592"}


def chip_dirs() -> list[str]:
    """Chips present in this tree; the ports land in separate commits."""
    return sorted(path.parent.name for path in ROOT.glob("ch*/Makefile"))


def require_openboot() -> None:
    if not (OPENBOOT / "firmware" / "Makefile").is_file():
        raise unittest.SkipTest(
            "third_party/openboot is not checked out "
            "(git submodule update --init --recursive)")


def geometry(chip: str) -> tuple[int, int, int, int]:
    """(slot_base, slot_size, capacity, record_size) straight from OpenBoot."""
    proc = subprocess.run(
        [sys.executable, "-I", str(GEOMETRY_TOOL), "--openboot", str(OPENBOOT),
         "--chip", chip, "--transport", "usb", "--board", BOARDS[chip]],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise unittest.SkipTest(f"openboot geometry unavailable: {proc.stderr.strip()}")
    fields = proc.stdout.split()
    return tuple(int(value, 0) for value in fields[:4])  # type: ignore[return-value]


def makefile_text(chip: str) -> str:
    return (ROOT / chip / "Makefile").read_text()


def link_ld_length(chip: str) -> int:
    text = (ROOT / chip / "link.ld").read_text()
    match = re.search(
        r"FLASH\s*\(rx\)\s*:\s*ORIGIN\s*=\s*[^,]+,\s*LENGTH\s*=\s*(0[xX][0-9a-fA-F]+)",
        text)
    assert match is not None, f"{chip}/link.ld has no FLASH LENGTH"
    return int(match.group(1), 0)


class SlotGeometry(unittest.TestCase):
    def test_link_script_length_is_the_slot_capacity(self):
        """The app gets slot size MINUS the erase block holding that slot's
        record. Linking across that block still boots -- the bootloader CRCs
        only img_len bytes -- and is then destroyed by the first COMMIT, which
        erases the block to store the record."""
        require_openboot()
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                _, size, capacity, _ = geometry(chip)
                self.assertEqual(capacity, link_ld_length(chip))
                self.assertLess(capacity, size,
                                "capacity must exclude the record block")

    def test_slot_defines_are_derived_not_literal(self):
        """The whole point: these come from OpenBoot's generated config, so a
        board-file change cannot leave this build pointing at the old layout."""
        require_openboot()
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                text = makefile_text(chip)
                self.assertIn("-DOPENBOOT_SLOT_BASE=$(OPENBOOT_SLOT_BASE)", text)
                self.assertIn("-DOPENBOOT_SLOT_SIZE=$(OPENBOOT_SLOT_SIZE)", text)
                self.assertIn("openboot_geometry.py", text)

    def test_record_fits_the_reserved_block(self):
        require_openboot()
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                _, size, capacity, record = geometry(chip)
                self.assertLessEqual(record, size - capacity)


class PinnedRevision(unittest.TestCase):
    def test_declared_revision_matches_the_checkout(self):
        """OPENBOOT_REVISION is declared in each chip Makefile so it is a
        parse-time constant. If it drifts from the actual submodule, check-deps
        fails the build -- but only for whoever runs it, and only once they
        have a toolchain. Catch it here instead."""
        require_openboot()
        head = subprocess.run(
            ["git", "-C", str(OPENBOOT), "rev-parse", "HEAD"],
            capture_output=True, text=True)
        if head.returncode != 0:
            self.skipTest("openboot checkout is not a git worktree")
        actual = head.stdout.strip()
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                match = re.search(r"^OPENBOOT_REVISION := ([0-9a-f]{40})$",
                                  makefile_text(chip), re.M)
                self.assertIsNotNone(match, f"{chip}/Makefile has no OPENBOOT_REVISION")
                self.assertEqual(actual, match.group(1))

    def test_revision_is_in_the_build_identity(self):
        """Without this the bootloader's identity is absent from the build id:
        CONFIG_TEXT said only `boot=openboot-usb`, so a locally modified
        bootloader composed into a factory image and nothing recorded it."""
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                config = re.search(r"^CONFIG_TEXT := (.*)$",
                                   makefile_text(chip), re.M)
                self.assertIsNotNone(config)
                self.assertIn("openboot=$(OPENBOOT_REVISION)", config.group(1))


class FactoryDelegation(unittest.TestCase):
    def test_factory_is_composed_by_openboot_with_bless_forced(self):
        """FACTORY_BLESS=1 is passed explicitly even though OpenBoot defaults
        it. An unblessed factory image is byte-plausible and boots into the
        bootloader instead of the app -- discovered at the customer, not here."""
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                text = makefile_text(chip)
                self.assertIn("FACTORY_BLESS=1", text)
                self.assertRegex(text, r"MRS_TOOLCHAIN=\"\$\(OPENBOOT_TOOLCHAIN\)\"")

    def test_local_composer_is_gone(self):
        """It produced no boot record, so a part flashed with its output sat in
        the bootloader waiting for `openboot bless` -- which the A/B change then
        made impossible, since bless resolves against the write slot (B) and the
        image is slot A."""
        self.assertFalse((ROOT / "compose_factory.py").exists())
        for chip in chip_dirs():
            with self.subTest(chip=chip):
                self.assertNotIn("compose_factory.py", makefile_text(chip))


if __name__ == "__main__":
    unittest.main()
