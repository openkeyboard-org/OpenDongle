"""The release gate must actually catch a bench key in a packaged artifact.

The scan's job is narrow: parse the key out of the header it ships in, find
those bytes anywhere in a binary, and refuse to pass vacuously when an
artifact pattern matches nothing. Each property is pinned here because each
failure mode is silent in production - a mis-parsed key or an empty glob
reads as "clean" exactly when the check has stopped checking.
"""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from check_bench_key_absent import parse_bench_key, scan  # noqa: E402

# The header block in its real shape: continuation backslashes, two lines,
# mixed spacing. Parsing must not depend on the surrounding #if state.
HEADER = """
#if DONGLE_BENCH_PROFILE
#define DONGLE_CRYPT_BENCH_KEY_BYTES \\
    { 0x4F,0x70,0x65,0x6E,0x4B,0x62,0x64,0x21, \\
      0xA5,0x5A,0xC3,0x3C,0x69,0x96,0x0F,0xF0 }
#endif
"""

KEY = bytes(
    [0x4F, 0x70, 0x65, 0x6E, 0x4B, 0x62, 0x64, 0x21,
     0xA5, 0x5A, 0xC3, 0x3C, 0x69, 0x96, 0x0F, 0xF0]
)


class ParseBenchKey(unittest.TestCase):
    def test_real_shape_header_parses_to_the_wire_bytes(self):
        self.assertEqual(parse_bench_key(HEADER), KEY)

    def test_missing_macro_is_an_error_not_a_pass(self):
        with self.assertRaises(ValueError):
            parse_bench_key("#define DONGLE_RF_CRYPT 1\n")

    def test_wrong_length_initializer_is_an_error(self):
        with self.assertRaises(ValueError):
            parse_bench_key("#define DONGLE_CRYPT_BENCH_KEY_BYTES { 0x01, 0x02 }")


class Scan(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def _artifact(self, name: str, payload: bytes) -> Path:
        p = self.dir / name
        p.write_bytes(payload)
        return p

    def test_clean_artifact_passes(self):
        p = self._artifact("clean.bin", b"\x00" * 64 + KEY[:8] + b"\xff" * 64)
        self.assertEqual(scan(KEY, [str(p)]), [])

    def test_embedded_key_is_reported_with_its_offset(self):
        p = self._artifact("dirty.bin", b"\x00" * 33 + KEY + b"\x00" * 7)
        failures = scan(KEY, [str(p)])
        self.assertEqual(len(failures), 1)
        self.assertIn("0x21", failures[0])
        self.assertIn(str(p), failures[0])

    def test_empty_glob_fails_rather_than_passing_vacuously(self):
        failures = scan(KEY, [str(self.dir / "no-such-*.bin")])
        self.assertEqual(len(failures), 1)
        self.assertIn("nothing was scanned", failures[0])

    def test_glob_scans_every_match(self):
        self._artifact("a.bin", b"ok")
        dirty = self._artifact("b.bin", KEY)
        failures = scan(KEY, [str(self.dir / "*.bin")])
        self.assertEqual(len(failures), 1)
        self.assertIn(str(dirty), failures[0])

    def test_shipping_header_yields_the_documented_key(self):
        # Tie the test to the real header so a key rotation keeps the gate
        # and this suite honest together.
        header = ROOT / "ch592" / "src" / "dongle_target.h"
        self.assertEqual(
            parse_bench_key(header.read_text()).hex(),
            "4f70656e4b626421a55ac33c69960ff0".lower(),
        )


if __name__ == "__main__":
    unittest.main()
