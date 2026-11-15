"""AES-CMAC, the CMAC KDF and the key-exchange derivations must match the reference.

Compiles the real `common/src/kex_kdf.c` against the software AES and checks
it against RFC 4493 (through `kex_ref`, itself pinned to the same vectors
here), over random inputs of every message length around the block boundaries,
and for failing closed when the AES engine faults partway through.
"""

from __future__ import annotations

from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

import kex_ref

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "common" / "src"
INC = ROOT / "common" / "include"

HARNESS = r"""
#include "kex_kdf.h"
#include "aes_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aes_sw_ctx_t g_ctx;
static long g_fail_at = -1;   /* fail the Nth AES block (0-based), then disarm */
static long g_blocks = 0;

int kex_aes_set_key(const uint8_t key[16]) { aes_sw_expand_key(&g_ctx, key); return 0; }
int kex_aes_block(const uint8_t in[16], uint8_t out[16]) {
    if (g_blocks++ == g_fail_at) { memset(out, 0xEE, 16); return 1; }
    aes_sw_encrypt_block(&g_ctx, in, out);
    return 0;
}

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
static size_t unhex(const char *s, unsigned char *out, size_t cap){
    size_t n = 0;
    if (s[0] == '-' && s[1] == 0) return 0;
    while (s[0] && s[1] && n < cap) {
        out[n++] = (unsigned char)(hexval(s[0]) << 4 | hexval(s[1]));
        s += 2;
    }
    return n;
}
static void put(const unsigned char *p, size_t n){
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

/* One command per line:
 *   cmac    <key> <msg|->            -> rc mac
 *   kdf     <key> <label2> <ctx|->   -> rc out
 *   derive  <z> <transcript> <fail>  -> rc k_cd k_ck lk
 *   confirm <key> <role> <mac_a> <mac_b> -> rc mac
 */
int main(void){
    static char cmd[16], a[1100], b[1100], c[1100], d[1100];
    static unsigned char ba[512], bb[512], bc[512], bd[512], out[16];
    while (scanf("%15s", cmd) == 1) {
        g_fail_at = -1; g_blocks = 0;
        if (!strcmp(cmd, "cmac")) {
            if (scanf("%1099s %1099s", a, b) != 2) return 2;
            unhex(a, ba, sizeof ba);
            size_t n = unhex(b, bb, sizeof bb);
            int rc = kex_cmac(ba, bb, n, out);
            printf("%d ", rc); put(out, 16); printf("\n");
        } else if (!strcmp(cmd, "kdf")) {
            if (scanf("%1099s %1099s %1099s", a, b, c) != 3) return 2;
            unhex(a, ba, sizeof ba);
            size_t n = unhex(c, bc, sizeof bc);
            int rc = kex_kdf(ba, b, bc, n, out);
            printf("%d ", rc); put(out, 16); printf("\n");
        } else if (!strcmp(cmd, "derive")) {
            kex_keys_t keys;
            if (scanf("%1099s %1099s %1099s", a, b, c) != 3) return 2;
            unhex(a, ba, sizeof ba);
            size_t n = unhex(b, bb, sizeof bb);
            g_fail_at = atol(c);
            memset(&keys, 0xAA, sizeof keys);
            int rc = kex_derive(ba, bb, n, &keys);
            printf("%d ", rc); put(keys.k_cd, 16); printf(" "); put(keys.k_ck, 16);
            printf(" "); put(keys.lk, 16); printf(" %ld\n", g_blocks);
        } else if (!strcmp(cmd, "confirm")) {
            if (scanf("%1099s %1099s %1099s %1099s", a, b, c, d) != 4) return 2;
            unhex(a, ba, sizeof ba); unhex(c, bc, sizeof bc); unhex(d, bd, sizeof bd);
            int rc = kex_confirm(ba, (uint8_t)strtol(b, NULL, 16), bc, bd, out);
            printf("%d ", rc); put(out, 16); printf("\n");
        } else {
            return 3;
        }
    }
    return 0;
}
"""

RFC4493_KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
RFC4493_MSG = bytes.fromhex(
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710")
RFC4493 = [
    (0, "bb1d6929e95937287fa37d129b756746"),
    (16, "070a16b46b4d4144f79bdd9dd04a287c"),
    (40, "dfa66747de9ae63030ca32611497c827"),
    (64, "51f0bebf7e3b9d92fc49741779363cfe"),
]


def hx(b: bytes) -> str:
    return b.hex() if b else "-"


class KexKdfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = next((p for p in map(shutil.which, ("cc", "gcc", "clang")) if p), None)
        if cc is None:
            raise unittest.SkipTest("no host C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "harness.c").write_text(HARNESS)
        cls.exe = tmp / "kex_host"
        subprocess.run(
            [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{INC}",
             str(tmp / "harness.c"), str(SRC / "kex_kdf.c"), str(SRC / "aes_sw.c"),
             "-o", str(cls.exe)],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_lines(self, lines):
        proc = subprocess.run([str(self.exe)], input="\n".join(lines) + "\n",
                              capture_output=True, text=True, check=True)
        rows = [line.split() for line in proc.stdout.splitlines()]
        self.assertEqual(len(rows), len(lines))
        return rows

    def test_reference_matches_rfc4493(self):
        for n, want in RFC4493:
            self.assertEqual(kex_ref.cmac(RFC4493_KEY, RFC4493_MSG[:n]).hex(), want)

    def test_firmware_cmac_matches_rfc4493(self):
        rows = self.run_lines([f"cmac {RFC4493_KEY.hex()} {hx(RFC4493_MSG[:n])}"
                               for n, _ in RFC4493])
        for (rc, mac), (_, want) in zip(rows, RFC4493):
            self.assertEqual((rc, mac), ("0", want))

    def test_cmac_every_length_around_the_block_boundaries(self):
        rng = random.Random(4493)
        cases = [(rng.randbytes(16), rng.randbytes(n))
                 for n in list(range(0, 50)) + [63, 64, 65, 103, 127, 128, 129, 255]]
        rows = self.run_lines([f"cmac {k.hex()} {hx(m)}" for k, m in cases])
        for (rc, mac), (k, m) in zip(rows, cases):
            self.assertEqual(rc, "0")
            self.assertEqual(mac, kex_ref.cmac(k, m).hex(), f"len {len(m)}")

    def test_kdf_layout(self):
        rng = random.Random(800108)
        cases = [(rng.randbytes(16), label, rng.randbytes(n))
                 for label in (b"CD", b"CK", b"LK", b"AN", b"SK")
                 for n in (0, 4, 16)]
        rows = self.run_lines([f"kdf {k.hex()} {label.decode()} {hx(c)}" for k, label, c in cases])
        for (rc, out), (k, label, c) in zip(rows, cases):
            self.assertEqual(rc, "0")
            self.assertEqual(out, kex_ref.kdf(k, label, c).hex())
            # The layout itself, spelled out once, so the reference cannot
            # drift together with the firmware unnoticed.
            self.assertEqual(out, kex_ref.cmac(k, b"\x01" + label + b"\x00" + c + b"\x00\x80").hex())

    def test_kdf_refuses_an_oversized_context(self):
        (rc, out), = self.run_lines([f"kdf {bytes(16).hex()} LK {bytes(17).hex()}"])
        self.assertEqual(rc, "1")
        self.assertEqual(out, bytes(16).hex())

    def test_derive_and_confirm_match_the_reference(self):
        rng = random.Random(25519)
        for _ in range(8):
            z, t = rng.randbytes(32), rng.randbytes(103)
            want = kex_ref.derive(z, t)
            (rc, k_cd, k_ck, lk, _), = self.run_lines([f"derive {z.hex()} {t.hex()} -1"])
            self.assertEqual(rc, "0")
            self.assertEqual((k_cd, k_ck, lk),
                             (want["k_cd"].hex(), want["k_ck"].hex(), want["lk"].hex()))
            self.assertEqual(len({k_cd, k_ck, lk}), 3, "the three keys must be distinct")
            dg, kb = rng.randbytes(6), rng.randbytes(6)
            (rc1, m1), (rc2, m2) = self.run_lines([
                f"confirm {k_cd} 44 {dg.hex()} {kb.hex()}",
                f"confirm {k_ck} 4b {kb.hex()} {dg.hex()}",
            ])
            self.assertEqual((rc1, rc2), ("0", "0"))
            self.assertEqual(m1, kex_ref.confirm(want["k_cd"], kex_ref.ROLE_DONGLE, dg, kb).hex())
            self.assertEqual(m2, kex_ref.confirm(want["k_ck"], kex_ref.ROLE_KEYBOARD, kb, dg).hex())
            self.assertNotEqual(m1, m2, "the two confirmations must not be reflectable")

    def test_a_changed_transcript_changes_every_key(self):
        rng = random.Random(7)
        z, t = rng.randbytes(32), bytearray(rng.randbytes(103))
        (_, a_cd, a_ck, a_lk, _), = self.run_lines([f"derive {z.hex()} {t.hex()} -1"])
        t[50] ^= 1
        (_, b_cd, b_ck, b_lk, _), = self.run_lines([f"derive {z.hex()} {t.hex()} -1"])
        self.assertNotEqual(a_cd, b_cd)
        self.assertNotEqual(a_ck, b_ck)
        self.assertNotEqual(a_lk, b_lk)

    def test_an_engine_fault_anywhere_yields_no_keys(self):
        """Fail each AES block of a derivation in turn: never a partial result."""
        rng = random.Random(11)
        z, t = rng.randbytes(32), rng.randbytes(103)
        (rc, *_, total), = self.run_lines([f"derive {z.hex()} {t.hex()} -1"])
        self.assertEqual(rc, "0")
        total = int(total)
        self.assertGreater(total, 10)
        rows = self.run_lines([f"derive {z.hex()} {t.hex()} {i}" for i in range(total)])
        zero = bytes(16).hex()
        for i, (rc, k_cd, k_ck, lk, _) in enumerate(rows):
            self.assertEqual(rc, "1", f"fault at block {i} was not reported")
            self.assertEqual((k_cd, k_ck, lk), (zero, zero, zero),
                             f"fault at block {i} left key material behind")


if __name__ == "__main__":
    unittest.main()
