"""The software AES-128 forward cipher must be AES, not merely deterministic.

CH570 has no hardware AES engine, so on that chip every encrypted RF byte comes
out of `common/src/aes_sw.c`. A cipher that is subtly wrong still produces
confident-looking ciphertext and still round-trips against itself, so "it
works on the bench" proves nothing. These tests pin it against the published
vectors instead.

The vectors are the ones the hardware engines on CH592 and CH572 were validated
with, which is what makes the two backends interchangeable: same key, same
block, same bytes out, whichever chip produced them.

This file also re-derives the 512-vector differential fold on the host from the
real cipher. That number (`b106130c`) is the acceptance gate every hardware
backend is measured against; deriving it here from published vectors turns it
from a value copied out of a bench log into one this suite can prove.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import aes_vectors

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "common" / "src" / "aes_sw.c"
INC = ROOT / "common" / "include"

VECTORS = aes_vectors.VECTORS

HARNESS = r"""
#include "aes_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = (uint8_t)((hexval(s[2 * i]) << 4) | hexval(s[2 * i + 1]));
}

static void puthex(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

/*
 * The 512-vector differential, byte-identical to the on-device harness: one
 * xorshift32 stream advanced twice per byte position, key byte from the first
 * advance and plaintext byte from the second, every ciphertext folded into a
 * single FNV-1a-32. Reformatting this changes the answer.
 */
static void differential(void)
{
    uint32_t st = 0x12345678u;
    uint32_t sum = 0x811C9DC5u;
    uint8_t k[16], p[16], c[16];
    aes_sw_ctx_t ctx;

    for (int n = 0; n < 512; n++) {
        for (int i = 0; i < 16; i++) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  k[i] = (uint8_t)st;
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  p[i] = (uint8_t)st;
        }
        aes_sw_expand_key(&ctx, k);
        aes_sw_encrypt_block(&ctx, p, c);
        for (int i = 0; i < 16; i++) { sum ^= c[i]; sum *= 0x01000193u; }
    }
    printf("%08x\n", sum);
}

/*
 * Emit the first `n` (key, plaintext) pairs of the differential stream, so the
 * host generator can be compared against this one directly rather than merely
 * assumed to match.
 */
static void pairs(int n)
{
    uint32_t st = 0x12345678u;
    uint8_t k[16], p[16];

    for (int b = 0; b < n; b++) {
        for (int i = 0; i < 16; i++) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  k[i] = (uint8_t)st;
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  p[i] = (uint8_t)st;
        }
        for (int i = 0; i < 16; i++) printf("%02x", k[i]);
        printf(" ");
        for (int i = 0; i < 16; i++) printf("%02x", p[i]);
        printf("\n");
    }
}

/* argv: <key-hex> <plaintext-hex> [mode]; prints ciphertext hex.
 * modes: --inplace, --overlap, --rekey, --twice; or "--fold" / "--pairs N". */
int main(int argc, char **argv)
{
    uint8_t key[16], in[16], out[16];
    aes_sw_ctx_t ctx;

    if (argc >= 2 && strcmp(argv[1], "--fold") == 0) { differential(); return 0; }
    if (argc >= 3 && strcmp(argv[1], "--pairs") == 0) {
        pairs(atoi(argv[2]));
        return 0;
    }
    if (argc < 3) return 2;
    unhex(argv[1], key, 16);
    unhex(argv[2], in, 16);
    const char *mode = (argc > 3) ? argv[3] : "";

    aes_sw_expand_key(&ctx, key);

    if (strcmp(mode, "--inplace") == 0) {
        memcpy(out, in, 16);
        aes_sw_encrypt_block(&ctx, out, out);
    } else if (strcmp(mode, "--overlap") == 0) {
        /* out = in + 1. The contract promises ANY overlap is safe, because both
         * backends read all 16 input bytes before writing any output byte. A
         * naive byte-at-a-time implementation corrupts its own input here. */
        uint8_t buf[17];
        memcpy(buf, in, 16);
        aes_sw_encrypt_block(&ctx, buf, buf + 1);
        memcpy(out, buf + 1, 16);
    } else if (strcmp(mode, "--rekey") == 0) {
        /* Key K1 -> other key -> back to K1 must reproduce the K1 result, i.e.
         * expand_key fully replaces the schedule and leaves no residue. */
        uint8_t other[16];
        for (int i = 0; i < 16; i++) other[i] = (uint8_t)(key[i] ^ 0xFF);
        aes_sw_encrypt_block(&ctx, in, out);
        aes_sw_expand_key(&ctx, other);
        aes_sw_encrypt_block(&ctx, in, out);
        aes_sw_expand_key(&ctx, key);
        aes_sw_encrypt_block(&ctx, in, out);
    } else if (strcmp(mode, "--twice") == 0) {
        /* Same block twice without re-keying: the seam promises no cross-block
         * chaining state, so the second result must equal the first. */
        uint8_t first[16];
        aes_sw_encrypt_block(&ctx, in, first);
        aes_sw_encrypt_block(&ctx, in, out);
        if (memcmp(first, out, 16) != 0) { printf("MISMATCH\n"); return 1; }
    } else {
        aes_sw_encrypt_block(&ctx, in, out);
    }

    puthex(out, 16);
    return 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class SoftwareAes(unittest.TestCase):
    """Compile the real firmware source and run the published vectors."""

    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls._tmp = tempfile.TemporaryDirectory()
        d = Path(cls._tmp.name)
        harness = d / "harness.c"
        harness.write_text(HARNESS)
        cls.binary = d / "aeskat"
        subprocess.run(
            [
                cc, "-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                f"-I{INC}", "-o", str(cls.binary), str(harness), str(SRC),
            ],
            check=True,
            capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        tmp = getattr(cls, "_tmp", None)
        if tmp is not None:
            tmp.cleanup()

    def encrypt(self, key, plaintext, extra=()):
        proc = subprocess.run(
            [str(self.binary), key, plaintext, *extra],
            check=True,
            capture_output=True,
            text=True,
        )
        return proc.stdout.strip()

    def test_published_vectors(self):
        for name, key, plaintext, expected in VECTORS:
            with self.subTest(vector=name):
                self.assertEqual(self.encrypt(key, plaintext), expected)

    def test_in_place_aliasing(self):
        """hal_aes.h promises `out` may alias `in`; CTR callers rely on it."""
        _, key, plaintext, expected = VECTORS[0]
        self.assertEqual(self.encrypt(key, plaintext, ("--inplace",)), expected)

    def test_partial_overlap(self):
        """The contract promises ANY overlap, not just exact aliasing.

        `out = in + 1` is the case that catches an implementation which writes
        output bytes before it has finished reading input.
        """
        _, key, plaintext, expected = VECTORS[0]
        self.assertEqual(self.encrypt(key, plaintext, ("--overlap",)), expected)

    def test_key_change_leaves_no_residue(self):
        """K1 -> K2 -> K1 must reproduce the K1 ciphertext."""
        _, key, plaintext, expected = VECTORS[0]
        self.assertEqual(self.encrypt(key, plaintext, ("--rekey",)), expected)

    def test_no_cross_block_state(self):
        """The seam promises purity: same key, same input, same output."""
        _, key, plaintext, expected = VECTORS[0]
        self.assertEqual(self.encrypt(key, plaintext, ("--twice",)), expected)

    def test_differential_fold_matches_the_hardware_gate(self):
        """Re-derive the 512-vector acceptance value from the real cipher.

        Every hardware backend is accepted by reproducing this number. Deriving
        it here means the gate is anchored to published vectors and this source,
        not to a value transcribed from a bench log.
        """
        proc = subprocess.run(
            [str(self.binary), "--fold"],
            check=True, capture_output=True, text=True,
        )
        self.assertEqual(proc.stdout.strip(), aes_vectors.EXPECTED_FOLD)

    def test_python_and_c_generators_agree(self):
        """`aes_vectors.differential_pairs` must match the C loop byte for byte.

        The hardware runner uses the Python generator to describe what the
        device did; if the two drifted, a passing checksum would be compared
        against the wrong inputs and still look correct.

        This compares the actual generated bytes. An earlier version of this
        test only checked lengths and a hex regex, which would have passed for
        any two generators that produced 16-byte values -- i.e. it could not
        have caught the drift it was named for.
        """
        n = 24
        proc = subprocess.run(
            [str(self.binary), "--pairs", str(n)],
            check=True, capture_output=True, text=True,
        )
        c_pairs = [tuple(line.split()) for line in proc.stdout.splitlines()]
        py_pairs = list(aes_vectors.differential_pairs(n))

        self.assertEqual(len(c_pairs), n)
        self.assertEqual(c_pairs, py_pairs,
                         "the host and device differential generators disagree")

    def test_key_avalanche(self):
        """Flipping one key bit must change roughly half the output bits.

        Guards against a cipher that ignores most of the key - which still
        passes a single vector if that vector was what it was tuned against.
        """
        _, key, plaintext, expected = VECTORS[0]
        flipped = f"{int(key, 16) ^ 1:032x}"
        other = self.encrypt(flipped, plaintext)
        self.assertNotEqual(other, expected)
        differing = bin(int(other, 16) ^ int(expected, 16)).count("1")
        self.assertGreater(differing, 40, "avalanche too weak")
        self.assertLess(differing, 88, "avalanche implausibly strong")

    def test_plaintext_avalanche(self):
        _, key, plaintext, expected = VECTORS[0]
        flipped = f"{int(plaintext, 16) ^ 1:032x}"
        other = self.encrypt(key, flipped)
        differing = bin(int(other, 16) ^ int(expected, 16)).count("1")
        self.assertGreater(differing, 40, "avalanche too weak")
        self.assertLess(differing, 88, "avalanche implausibly strong")


if __name__ == "__main__":
    unittest.main()
