"""The AES validation log reader, exercised without a device.

The reader is the piece that decides whether a hardware run passed. Until now
every result in this campaign was decoded by hand from a hex dump, which is
exactly the step where a tired reader confirms the number they expected. So the
reader itself needs to be trustworthy, and it is the one part of the hardware
suite that can be fully tested on the host: synthesise a record, parse it, and
check the verdict.

The negative controls matter more than the happy path. A reader that reports
PASS on a truncated, desynchronised or hung record is worse than no reader,
because it converts a visible failure into an invisible one.
"""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest

import aes_vectors

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "validation"))

import read_aes_log as R  # noqa: E402

M = R.M

# Vectors 7-11 are contract properties (in-place, overlap, re-key, ...) and all
# reduce to the FIPS-197 C.1 answer, so one expectation covers them.
FIPS = aes_vectors.VECTORS[0][3]
EXPECTATIONS = {
    1: aes_vectors.VECTORS[0][3],
    2: aes_vectors.VECTORS[1][3],
    3: aes_vectors.VECTORS[2][3],
    4: aes_vectors.VECTORS[3][3],
    5: aes_vectors.VECTORS[4][3],
    6: aes_vectors.VECTORS[5][3],
    7: FIPS, 8: FIPS, 9: FIPS, 10: FIPS, 11: FIPS,
}

BOTH_OK = M["AES_LOG_RESULT_BYTES_OK"] | M["AES_LOG_RESULT_STATUS_OK"]


def ct_words(hexstr):
    return list(struct.unpack("<4I", bytes.fromhex(hexstr)))


def build(vectors=None, fold=None, complete=True, diff_enter=True,
          diff_exit=True, checkpoints=None, count=None, version=None,
          magic=None, start=None, overflow=False, backend=None):
    """Assemble a synthetic log exactly as the harness would write one."""
    w = [
        M["AES_LOG_MAGIC"] if magic is None else magic,
        M["AES_LOG_VERSION"] if version is None else version,
        M["AES_LOG_START"] if start is None else start,
        100_000_000,
        M["AES_LOG_BACKEND_ASM_A"] if backend is None else backend,
        0xDEADBEEF,
    ]

    vectors = EXPECTATIONS if vectors is None else vectors
    all_ok = 1
    for vec_id in sorted(vectors):
        entry = vectors[vec_id]
        ct, result = (entry, BOTH_OK) if isinstance(entry, str) else entry
        w.append(M["AES_LOG_VECTOR_BASE"] | vec_id)
        w.extend(ct_words(ct))
        w.append(result)
        if result != BOTH_OK:
            all_ok = 0
    w.append(M["AES_LOG_KAT_END"])
    w.append(all_ok)

    if diff_enter:
        w.append(M["AES_LOG_DIFF_ENTER"])
        cps = list(range(1, 17)) if checkpoints is None else checkpoints
        w.extend(cps)
        if diff_exit:
            w.append(M["AES_LOG_DIFF_EXIT"])
            w.append(M["AES_LOG_DIFF_COUNT"] if count is None else count)
            w.append(int(aes_vectors.EXPECTED_FOLD if fold is None else fold, 16))
            w.extend(ct_words(FIPS))

    for tag, cycles in ((0, 4), (1, 900), (2, 1669)):
        w.append(M["AES_LOG_TIMING_BASE"] | tag)
        w.append(cycles)
    w.append(M["AES_LOG_CORECFGR"])
    w.append(0x25)

    if overflow:
        w.append(M["AES_LOG_OVERFLOW"])
    elif complete:
        w.append(M["AES_LOG_END"])
    return w


class Markers(unittest.TestCase):
    def test_every_marker_is_parsed_from_the_shared_header(self):
        """The reader declares no format constants of its own.

        If it did, it could drift from the firmware and still look right.
        """
        self.assertEqual(M["AES_LOG_MAGIC"], 0x53414531, 'magic is not "SAE1"')
        for name in ("AES_LOG_START", "AES_LOG_END", "AES_LOG_KAT_END",
                     "AES_LOG_DIFF_ENTER", "AES_LOG_DIFF_EXIT"):
            self.assertIn(name, M)

    def test_the_differential_parameters_match_the_host_suite(self):
        """The device and the host must run the SAME differential.

        Comparing a device fold against a host fold computed over different
        inputs would be meaningless, and would look like a pass.
        """
        self.assertEqual(M["AES_LOG_DIFF_COUNT"], aes_vectors.DIFFERENTIAL_COUNT)
        self.assertEqual(M["AES_LOG_DIFF_SEED"], aes_vectors.DIFFERENTIAL_SEED)
        self.assertEqual(M["AES_LOG_DIFF_CHECKPOINT"], aes_vectors.CHECKPOINT_EVERY)
        self.assertEqual(M["AES_LOG_DIFF_FNV_BASIS"], aes_vectors.FNV_OFFSET_BASIS)
        self.assertEqual(M["AES_LOG_DIFF_FNV_PRIME"], aes_vectors.FNV_PRIME)


class HappyPath(unittest.TestCase):
    def setUp(self):
        self.rec = R.parse(build())

    def test_header_fields(self):
        self.assertEqual(self.rec.version, M["AES_LOG_VERSION"])
        self.assertEqual(self.rec.clock_hz, 100_000_000)
        self.assertEqual(self.rec.backend_name, "ASM_A")
        self.assertEqual(self.rec.build_id, 0xDEADBEEF)
        self.assertTrue(self.rec.complete)

    def test_all_vectors_are_recovered(self):
        self.assertEqual(sorted(self.rec.vectors), sorted(EXPECTATIONS))
        self.assertEqual(
            self.rec.vectors[1].ciphertext.hex(), aes_vectors.VECTORS[0][3])

    def test_differential_is_recovered(self):
        d = self.rec.differential
        self.assertTrue(d.entered and d.exited)
        self.assertFalse(d.hung)
        self.assertEqual(d.count, 512)
        self.assertEqual(d.fold, aes_vectors.EXPECTED_FOLD)
        self.assertEqual(len(d.checkpoints), 16)

    def test_timing_and_corecfgr(self):
        self.assertEqual(self.rec.timings[2], 1669)
        self.assertEqual(self.rec.corecfgr, 0x25)

    def test_verify_passes(self):
        self.assertEqual(
            R.verify(self.rec, EXPECTATIONS, aes_vectors.EXPECTED_FOLD), [])

    def test_report_says_pass(self):
        text = R.format_report(self.rec, [])
        self.assertIn("RESULT: PASS", text)
        self.assertIn("ASM_A", text)


class MalformedLogsAreRefused(unittest.TestCase):
    """Parsing must fail loudly rather than produce a partial verdict."""

    def assertRefused(self, words, fragment):
        with self.assertRaises(R.LogError) as cm:
            R.parse(words)
        self.assertIn(fragment, str(cm.exception).lower())

    def test_empty(self):
        self.assertRefused([], "empty")

    def test_bad_magic(self):
        """The commonest real failure: reading the wrong address, or a device
        that never ran the harness at all."""
        self.assertRefused(build(magic=0), "magic")

    def test_unknown_version(self):
        """Refuse rather than guess: a misparsed pass is worse than no answer."""
        self.assertRefused(build(version=99), "version")

    def test_missing_start_sentinel(self):
        self.assertRefused(build(start=0), "start sentinel")

    def test_truncated_vector_record(self):
        self.assertRefused(build()[:9], "truncated")

    def test_desync_is_not_skipped(self):
        """A stray word must stop the parse.

        Scanning forward to resynchronise would eventually land on ciphertext
        that happens to look like a marker and yield a confident wrong report.
        """
        words = build()
        words.insert(M["AES_LOG_HEADER_WORDS"], 0x12345678)
        self.assertRefused(words, "out of sync")


class FailuresAreDetected(unittest.TestCase):
    """`verify` must catch each way a run can go wrong."""

    def test_wrong_ciphertext(self):
        bad = dict(EXPECTATIONS)
        bad[3] = "00" * 16
        failures = R.verify(R.parse(build(vectors=bad)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("SP800-38A F.1.1 block 1" in f for f in failures))

    def test_engine_timeout_is_reported_distinctly(self):
        """A CH592 timeout is not the same finding as a wrong cipher."""
        bad = dict(EXPECTATIONS)
        bad[1] = ("00" * 16, 0)  # neither bytes nor status ok
        failures = R.verify(R.parse(build(vectors=bad)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("HAL_AES_OK" in f for f in failures))

    def test_a_broken_on_device_comparison_is_caught(self):
        """Bytes right, device flag says wrong.

        The harness compares on-device and logs a boolean. If that comparison
        is broken it would mask real failures elsewhere, so the reader checks
        the bytes itself and reports the disagreement.
        """
        bad = dict(EXPECTATIONS)
        bad[1] = (FIPS, M["AES_LOG_RESULT_STATUS_OK"])  # bytes_ok clear
        failures = R.verify(R.parse(build(vectors=bad)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(
            any("harness's own comparison is broken" in f for f in failures),
            failures)

    def test_a_lying_pass_flag_does_not_save_a_wrong_ciphertext(self):
        """Device says pass, bytes are wrong -> still a failure.

        This is the reason `verify` does not trust the device's boolean.
        """
        bad = dict(EXPECTATIONS)
        bad[1] = ("11" * 16, BOTH_OK)
        failures = R.verify(R.parse(build(vectors=bad)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("!= expected" in f for f in failures), failures)

    def test_hung_differential_is_distinguished_from_skipped(self):
        """The entry/exit pair exists precisely to tell these apart."""
        hung = R.parse(build(diff_exit=False, complete=False))
        self.assertTrue(hung.differential.hung)
        failures = R.verify(hung, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("hung" in f for f in failures), failures)

        skipped = R.parse(build(diff_enter=False))
        self.assertFalse(skipped.differential.entered)
        failures = R.verify(skipped, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("never ran" in f for f in failures), failures)

    def test_wrong_fold_is_reported(self):
        """The cross-arm assertion: one differing block out of 512."""
        rec = R.parse(build(fold="deadbeef"))
        failures = R.verify(rec, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("fold" in f for f in failures), failures)

    def test_incomplete_record_fails_even_if_everything_read_so_far_passed(self):
        """A run that died after the last check must not score as a pass."""
        rec = R.parse(build(complete=False))
        failures = R.verify(rec, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("incomplete" in f for f in failures), failures)

    def test_overflow_is_reported(self):
        rec = R.parse(build(overflow=True))
        self.assertTrue(rec.overflow)
        failures = R.verify(rec, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("overflow" in f for f in failures), failures)

    def test_missing_vectors_are_named(self):
        subset = {k: v for k, v in EXPECTATIONS.items() if k < 8}
        failures = R.verify(R.parse(build(vectors=subset)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("never ran" in f for f in failures), failures)
        self.assertTrue(any("overlap" in f for f in failures), failures)

    def test_short_differential_is_caught(self):
        """512 blocks were promised; anything less is not the same test."""
        failures = R.verify(R.parse(build(count=256)), EXPECTATIONS,
                            aes_vectors.EXPECTED_FOLD)
        self.assertTrue(any("256" in f for f in failures), failures)


class InputDecoding(unittest.TestCase):
    def test_raw_bytes_round_trip(self):
        words = build()
        raw = b"".join(struct.pack("<I", w) for w in words)
        self.assertEqual(R.words_from_bytes(raw), words)

    def test_partial_word_is_rejected(self):
        with self.assertRaises(R.LogError):
            R.words_from_bytes(b"\x01\x02\x03")

    def test_hex_text_with_address_columns(self):
        """Memory-dump tools emit `addr: w0 w1 w2 w3`; accept it directly."""
        text = "0x20001000: 53414531 00000002 c0de5a45 05f5e100\n"
        self.assertEqual(
            R.words_from_text(text),
            [0x53414531, 0x00000002, 0xC0DE5A45, 0x05F5E100],
        )

    def test_comments_and_blank_lines_are_ignored(self):
        self.assertEqual(R.words_from_text("# note\n\n 53414531 \n"),
                         [0x53414531])


if __name__ == "__main__":
    unittest.main()
