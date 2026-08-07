"""Slot-awareness of the ODG2 image-identity validator.

Under OpenBoot's A/B slots an application is linked once per slot, so "the app
base" is no longer a single number. These pin the two questions the validator
must keep separate:

  1. is the image linked at a base OpenBoot will actually jump to?
  2. does the ODG2 header agree with where the image is really loaded?

Both used to be answered by comparing against one constant, which cannot see a
header that claims slot A on an image linked for slot B -- the exact confusion
the host tooling exists to catch, and the one that would send a slot-B image to
a device expecting slot A.
"""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import dongle_image_id as ids  # noqa: E402

# The real slot geometry, per chip. Slot A is uniform; slot B is not.
CH570_SLOTS = (0x2000, 0x1E000)
CH592_SLOTS = (0x2000, 0x39000)


def make_image(*, base: int, length: int = ids.MIN_APP_IMAGE_LEN,
               family: int = 0x70, kind: int = ids.IMAGE_KIND_APP) -> bytes:
    """A minimal image carrying a well-formed ODG2 header at IMAGE_ID_OFF."""
    image = bytearray(b"\x00" * length)
    header = struct.pack(
        "<4sBBBBIIIIII", ids.IMAGE_ID_MAGIC, ids.IMAGE_ID_FORMAT, family, kind,
        ids.IMAGE_ID_LEN, base, length, 0, 0, 0, 0)
    image[ids.IMAGE_ID_OFF:ids.IMAGE_ID_OFF + ids.IMAGE_ID_LEN] = header
    return ids.finalize_image(bytes(image))


def problems(image: bytes, *, loaded_base: int, **kw) -> list[str]:
    _, found = ids.validate_app_image_identity(
        image, loaded_base=loaded_base, require_header=True, **kw)
    return found


class SlotBases(unittest.TestCase):
    def test_slot_a_still_passes_by_default(self):
        img = make_image(base=0x2000)
        self.assertEqual([], problems(img, loaded_base=0x2000))

    def test_slot_b_is_rejected_unless_declared(self):
        """The default must stay slot-A-only. A caller that has not been taught
        about slots should reject a slot-B image, not accept any base: the
        failure mode of a too-permissive default is a wrong-slot flash."""
        img = make_image(base=0x1E000)
        found = problems(img, loaded_base=0x1E000)
        self.assertTrue(any("not an OpenBoot slot base" in p for p in found), found)

    def test_slot_b_passes_when_declared(self):
        # The family must match the chip, or the ch592 case silently validates
        # a CH570-family image and proves nothing about CH592.
        for chip, family, slots in (("ch570", 0x70, CH570_SLOTS),
                                    ("ch592", 0x92, CH592_SLOTS)):
            with self.subTest(chip=chip):
                img = make_image(base=slots[1], family=family)
                self.assertEqual(
                    [], problems(img, loaded_base=slots[1], valid_bases=slots,
                                 expected_family=family))

    def test_an_address_between_slots_is_refused(self):
        """Not merely "different from slot A" -- it must be one of the two.
        A garbled geometry read producing some other address would otherwise
        pass validation and be unbootable, since the bootloader only ever
        jumps to a slot base."""
        img = make_image(base=0x10000)
        found = problems(img, loaded_base=0x10000, valid_bases=CH570_SLOTS)
        self.assertTrue(any("not an OpenBoot slot base" in p for p in found), found)


class HeaderAgreesWithLoadBase(unittest.TestCase):
    def test_header_claiming_the_wrong_slot_is_caught(self):
        """Both bases are individually legal here, so a single-constant check
        could not catch this. The image is loaded at slot B but its header says
        slot A."""
        img = make_image(base=0x2000)
        found = problems(img, loaded_base=0x1E000, valid_bases=CH570_SLOTS)
        self.assertTrue(
            any("header base" in p and "load base" in p for p in found), found)

    def test_matching_header_and_load_base_is_clean(self):
        img = make_image(base=0x1E000)
        self.assertEqual(
            [], problems(img, loaded_base=0x1E000, valid_bases=CH570_SLOTS))


if __name__ == "__main__":
    unittest.main()
