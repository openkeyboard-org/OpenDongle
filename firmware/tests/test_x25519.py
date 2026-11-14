"""The resumable X25519 must be RFC 7748, bit for bit, and reject low-order points.

Compiles the real `common/src/x25519.c` for the host and drives it one field
operation at a time through `x25519_step()`, the way the firmware does between
polls. Checked against the RFC 7748 section 5.2 and 6.1 vectors, the iterated
vector, and an independent big-integer Montgomery ladder over random inputs --
including inputs chosen to exercise the carry and borrow folds that ordinary
random values almost never reach.
"""

from __future__ import annotations

from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
X25519_C = ROOT / "common" / "src" / "x25519.c"
INC = ROOT / "common" / "include"

P = 2**255 - 19
A24 = 121665

HARNESS = r"""
#include "x25519.h"
#include <stdio.h>
#include <string.h>

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
static int unhex(const char *s, unsigned char *out){
    for (int i = 0; i < 32; i++) {
        int h = hexval(s[2*i]), l = hexval(s[2*i+1]);
        if (h < 0 || l < 0) return 0;
        out[i] = (unsigned char)(h << 4 | l);
    }
    return 1;
}

int main(void){
    char a[80], b[80];
    while (scanf("%79s %79s", a, b) == 2) {
        unsigned char k[32], u[32], out[32], keep[32];
        x25519_ctx_t ctx;
        long steps = 0;
        int zero;
        if (!unhex(a, k) || !unhex(b, u)) return 2;
        memcpy(keep, k, 32);
        x25519_begin(&ctx, k, u);
        do { steps++; } while (x25519_step(&ctx));
        zero = x25519_finish(&ctx, out);
        for (int i = 0; i < 32; i++) printf("%02x", out[i]);
        /* steps, zero flag, caller's scalar untouched, context wiped */
        {
            int wiped = 1;
            const unsigned char *p = (const unsigned char *)&ctx;
            for (size_t i = 0; i < sizeof(ctx.r) + sizeof(ctx.k); i++) if (p[i]) wiped = 0;
            printf(" %ld %d %d %d\n", steps, zero, memcmp(keep, k, 32) == 0, wiped);
        }
    }
    return 0;
}
"""


def ref_x25519(k: bytes, u: bytes) -> bytes:
    """RFC 7748 section 5, over Python integers."""
    kb = bytearray(k)
    kb[0] &= 248
    kb[31] &= 127
    kb[31] |= 64
    scalar = int.from_bytes(kb, "little")
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in range(254, -1, -1):
        kt = (scalar >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = (x2 + z2) % P
        aa = a * a % P
        b = (x2 - z2) % P
        bb = b * b % P
        e = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = d * a % P
        cb = c * b % P
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + A24 * e) % P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


def find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class X25519Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "harness.c").write_text(HARNESS)
        cls.exe = tmp / "x25519_host"
        subprocess.run(
            [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{INC}",
             str(tmp / "harness.c"), str(X25519_C), "-o", str(cls.exe)],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_pairs(self, pairs):
        text = "".join(f"{k.hex()} {u.hex()}\n" for k, u in pairs)
        proc = subprocess.run([str(self.exe)], input=text, capture_output=True,
                              text=True, check=True)
        rows = [line.split() for line in proc.stdout.splitlines()]
        self.assertEqual(len(rows), len(pairs))
        return [(bytes.fromhex(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4]))
                for r in rows]

    def test_rfc7748_section_5_2_vectors(self):
        vectors = [
            ("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
             "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
             "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
            ("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
             "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
             "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
        ]
        got = self.run_pairs([(bytes.fromhex(k), bytes.fromhex(u)) for k, u, _ in vectors])
        for (out, _steps, zero, kept, wiped), (_, _, want) in zip(got, vectors):
            self.assertEqual(out.hex(), want)
            self.assertEqual(zero, 0)
            self.assertEqual(kept, 1, "the caller's scalar must not be clamped in place")
            self.assertEqual(wiped, 1, "finish must wipe the registers and the scalar")

    def test_rfc7748_section_6_1_diffie_hellman(self):
        a = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        b = bytes.fromhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        base = bytes([9]) + bytes(31)
        (pa, *_), (pb, *_) = self.run_pairs([(a, base), (b, base)])
        self.assertEqual(pa.hex(), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
        self.assertEqual(pb.hex(), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        (sa, *_), (sb, *_) = self.run_pairs([(a, pb), (b, pa)])
        self.assertEqual(sa, sb)
        self.assertEqual(sa.hex(), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")

    def test_rfc7748_iterated_vector_1000(self):
        k = u = bytes([9]) + bytes(31)
        for i in range(1000):
            (out, *_), = self.run_pairs([(k, u)])
            k, u = out, k
            if i == 0:
                self.assertEqual(out.hex(),
                                 "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079")
        self.assertEqual(k.hex(), "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51")

    def test_matches_the_big_integer_ladder(self):
        rng = random.Random(0x25519)
        ff = b"\xff" * 32
        edge = [
            bytes(32), ff, bytes([1]) + bytes(31),
            (P - 1).to_bytes(32, "little"), P.to_bytes(32, "little"),
            (P + 1).to_bytes(32, "little"), (2**255 - 1).to_bytes(32, "little"),
            (2**255 - 20).to_bytes(32, "little"), bytes([0xEC]) + b"\xff" * 30 + b"\x7f",
            bytes(31) + b"\x80", (2**224 - 1).to_bytes(32, "little"),
            (38).to_bytes(32, "little"), (2**32 - 1).to_bytes(32, "little"),
        ]
        pairs = []
        for u in edge:
            for k in (ff, bytes(32), rng.randbytes(32)):
                pairs.append((k, u))
        for _ in range(300):
            pairs.append((rng.randbytes(32), rng.randbytes(32)))
        for (out, steps, zero, _, _), (k, u) in zip(self.run_pairs(pairs), pairs):
            want = ref_x25519(k, u)
            self.assertEqual(out, want, f"k={k.hex()} u={u.hex()}")
            self.assertEqual(zero, int(want == bytes(32)))
            self.assertEqual(steps, STEPS, "step count must not depend on the inputs")

    def test_low_order_points_are_flagged(self):
        """An all-zero output must be reported so it is never used as a secret."""
        k = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
        low_order = [
            bytes(32),
            bytes([1]) + bytes(31),
            bytes.fromhex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
            bytes.fromhex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157"),
            (P - 1).to_bytes(32, "little"), P.to_bytes(32, "little"),
            (P + 1).to_bytes(32, "little"),
        ]
        for (out, _, zero, _, _), u in zip(self.run_pairs([(k, u) for u in low_order]), low_order):
            self.assertEqual(out, bytes(32), u.hex())
            self.assertEqual(zero, 1, u.hex())


# 255 ladder steps of 18 operations, 254 squarings and 252 multiplications for
# the inversion, and the final multiplication.
STEPS = 255 * 18 + 254 + 252 + 1


if __name__ == "__main__":
    unittest.main()
