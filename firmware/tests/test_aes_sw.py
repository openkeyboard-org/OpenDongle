"""The software AES-128 forward cipher must be AES, not merely deterministic.

CH570 has no hardware AES engine, so on that chip every encrypted RF byte comes
out of `common/src/aes_sw.c`. A cipher that is subtly wrong still produces
confident-looking ciphertext and still round-trips against itself, so "it
works on the bench" proves nothing. These tests pin it against the published
vectors instead.

The vectors are the ones the hardware engines on CH592 and CH572 were validated
with, which is what makes the two backends interchangeable: same key, same
block, same bytes out, whichever chip produced them.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "common" / "src" / "aes_sw.c"
INC = ROOT / "common" / "include"

# (name, key, plaintext, expected ciphertext)
VECTORS = [
    (
        "FIPS-197 C.1",
        "000102030405060708090a0b0c0d0e0f",
        "00112233445566778899aabbccddeeff",
        "69c4e0d86a7b0430d8cdb78070b4c55a",
    ),
    (
        "all-zero key and plaintext",
        "00" * 16,
        "00" * 16,
        "66e94bd4ef8a2c3b884cfa59ca342b2e",
    ),
    # NIST SP 800-38A F.1.1 ECB-AES128 encrypt, the four sample blocks.
    (
        "SP800-38A F.1.1 block 1",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "6bc1bee22e409f96e93d7e117393172a",
        "3ad77bb40d7a3660a89ecaf32466ef97",
    ),
    (
        "SP800-38A F.1.1 block 2",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "f5d3d58503b9699de785895a96fdbaaf",
    ),
    (
        "SP800-38A F.1.1 block 3",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "43b1cd7f598ece23881b00e3ed030688",
    ),
    (
        "SP800-38A F.1.1 block 4",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "f69f2445df4f9b17ad2b417be66c3710",
        "7b0c785e27e8ad3f8223207104725dd4",
    ),
]

HARNESS = r"""
#include "aes_sw.h"
#include <stdio.h>
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

/* argv: <key-hex> <plaintext-hex> [--inplace]; prints ciphertext hex. */
int main(int argc, char **argv)
{
    uint8_t key[16], in[16], out[16];
    aes_sw_ctx_t ctx;

    if (argc < 3) return 2;
    unhex(argv[1], key, 16);
    unhex(argv[2], in, 16);

    aes_sw_expand_key(&ctx, key);
    if (argc > 3 && strcmp(argv[3], "--inplace") == 0) {
        memcpy(out, in, 16);
        aes_sw_encrypt_block(&ctx, out, out);
    } else {
        aes_sw_encrypt_block(&ctx, in, out);
    }

    for (int i = 0; i < 16; i++) printf("%02x", out[i]);
    printf("\n");
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
