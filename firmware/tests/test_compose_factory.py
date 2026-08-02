"""Factory composition and chip layout for the OpenBoot boot chain.

A factory image is the OpenBoot bootloader padded with 0x00 to exactly 0x2000
followed by the application, so the app lands at file offset 0x2000 - the same
address it links at. These tests pin the composition byte-for-byte, because the
flash-factory path compares readback against these exact bytes.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import dongle_image_id  # noqa: E402
from compose_factory import (  # noqa: E402
    OPENBOOT_REGION_BYTES,
    compose_factory,
)

APP = bytes((index * 37 + 11) & 0xFF for index in range(0x1000))


def chip_dirs() -> list[str]:
    """Chips present in this tree; the ports land in separate commits."""
    return sorted(path.parent.name for path in ROOT.glob("ch*/Makefile"))


def makefile_value(chip: str, name: str) -> int:
    """Read a `NAME := <int>` assignment out of a chip Makefile."""
    text = (ROOT / chip / "Makefile").read_text()
    match = re.search(rf"^{re.escape(name)} := (\d+)$", text, re.M)
    assert match is not None, f"{chip}/Makefile has no {name}"
    return int(match.group(1))


class ComposeFactory(unittest.TestCase):
    def test_app_lands_at_exactly_0x2000(self):
        for size in (1, 100, 4080, 8191, 8192):
            with self.subTest(openboot_len=size):
                openboot = bytes((index * 7 + 3) & 0xFF for index in range(size))
                factory = compose_factory(openboot, APP)
                self.assertEqual(0x2000 + len(APP), len(factory))
                self.assertEqual(openboot, factory[:size])
                self.assertEqual(APP, factory[0x2000:])

    def test_full_8192_byte_openboot_needs_no_pad(self):
        openboot = bytes(range(256)) * 32
        self.assertEqual(8192, len(openboot))
        factory = compose_factory(openboot, APP)
        self.assertEqual(openboot + APP, factory)

    def test_oversized_openboot_is_rejected(self):
        with self.assertRaises(ValueError):
            compose_factory(b"\x00" * 8193, APP)

    def test_empty_openboot_is_rejected(self):
        with self.assertRaises(ValueError):
            compose_factory(b"", APP)

    def test_empty_app_is_rejected(self):
        with self.assertRaises(ValueError):
            compose_factory(b"\xAA" * 100, b"")

    def test_pad_byte_is_0x00_not_0xff(self):
        """Regression pin: on CH5xx, programming 0xFF programs nothing, so an
        0xFF pad would never land in flash and the flash-factory readback
        compare would fail over the pad region. The pad must be 0x00."""
        openboot = b"\xAA" * 100
        factory = compose_factory(openboot, APP)
        pad = factory[len(openboot):0x2000]
        self.assertEqual(b"\x00" * (0x2000 - len(openboot)), pad)
        self.assertNotIn(0xFF, pad)


class LayoutConsistency(unittest.TestCase):
    def test_openboot_region_is_the_app_base(self):
        """The composer pads to the address the application links at, or the
        app would land somewhere the bootloader never jumps to."""
        self.assertEqual(dongle_image_id.APP_BASE, OPENBOOT_REGION_BYTES)

    def test_app_windows_end_where_the_reserved_pages_begin(self):
        """CH570 keeps the bond page at 0x3A000 and the OpenBoot boot record at
        0x3B000 outside the app window; CH592 runs to the end of code flash."""
        expected = {"ch570": 0x3A000, "ch592": 0x70000}
        chips = chip_dirs()
        if not chips:
            self.skipTest("no chip ports in this tree yet")
        for chip in chips:
            with self.subTest(chip=chip):
                self.assertIn(chip, expected, f"unknown chip port {chip}")
                self.assertEqual(
                    expected[chip],
                    dongle_image_id.APP_BASE + makefile_value(chip, "APP_MAX_BYTES"))


class BuildWiring(unittest.TestCase):
    def test_chip_makefiles_compose_with_this_module(self):
        chips = chip_dirs()
        if not chips:
            self.skipTest("no chip ports in this tree yet")
        for chip in chips:
            with self.subTest(chip=chip):
                text = (ROOT / chip / "Makefile").read_text()
                self.assertIn("compose_factory.py", text,
                              f"{chip} does not compose its factory image here")


if __name__ == "__main__":
    unittest.main()
