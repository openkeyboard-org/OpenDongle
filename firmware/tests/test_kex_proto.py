"""The key exchange must agree on one key over a lossy link, or on none.

Compiles the real `common/src/kex_proto.c`, `kex_kdf.c` and `x25519.c` and
runs a dongle instance against a keyboard instance in one process: one frame
each way per slot, a few units of work per slot, frames dropped at a chosen
rate. The link key both ends release is checked against an independent
computation (the big-integer X25519 and `kex_ref`), and every tampering or
fault case must end with NO key on either end.
"""

from __future__ import annotations

from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

import kex_ref
from test_x25519 import ref_x25519

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "common" / "src"
INC = ROOT / "common" / "include"

HARNESS = r"""
#include "kex_proto.h"
#include "aes_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aes_sw_ctx_t g_ctx;
static long g_fail_at = -1, g_blocks = 0;
int kex_aes_set_key(const uint8_t key[16]) { aes_sw_expand_key(&g_ctx, key); return 0; }
int kex_aes_block(const uint8_t in[16], uint8_t out[16]) {
    if (g_blocks++ == g_fail_at) { memset(out, 0xEE, 16); return 1; }
    aes_sw_encrypt_block(&g_ctx, in, out);
    return 0;
}

static uint32_t g_rng;
static unsigned rnd(void){ g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) % 100u; }

static int hexval(char c){ return (c>='0'&&c<='9') ? c-'0' : c-'a'+10; }
static void unhex(const char *s, unsigned char *out, size_t n){
    for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(hexval(s[2*i]) << 4 | hexval(s[2*i+1]));
}
static void put(const unsigned char *p, size_t n){ for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }

/* args: dpriv kpriv dprefix kprefix dmac kmac loss%% steps seed mode maxslots
 * mode: 0 clean; 1 flip a bit in the first dongle PUB fragment delivered;
 *       2 the keyboard is handed an all-zero dongle public key;
 *       3 AES fault on the first derivation block either end runs;
 *       4 expire the dongle at slot 40; 5 flip a bit in a keyboard CONFIRM;
 *       6 replace every dongle CONFIRM chunk with zeros. */
int main(int argc, char **argv){
    unsigned char dpriv[32], kpriv[32], dpre[KEX_T_PREFIX_BYTES], kpre[KEX_T_PREFIX_BYTES], dmac[6], kmac[6];
    static kex_t dg, kb;
    unsigned char f[KEX_PAYLOAD_BYTES], lk_d[16] = {0}, lk_k[16] = {0};
    int loss, steps, mode, maxslots, slot, flipped = 0, took_d, took_k;
    long dg_tx = 0, kb_tx = 0, kb_tx_before_full = 0, kb_conf_before_ok = 0;
    if (argc != 12) return 2;
    unhex(argv[1], dpriv, 32); unhex(argv[2], kpriv, 32);
    unhex(argv[3], dpre, KEX_T_PREFIX_BYTES); unhex(argv[4], kpre, KEX_T_PREFIX_BYTES);
    unhex(argv[5], dmac, 6); unhex(argv[6], kmac, 6);
    loss = atoi(argv[7]); steps = atoi(argv[8]); g_rng = (uint32_t)atol(argv[9]);
    mode = atoi(argv[10]); maxslots = atoi(argv[11]);

    kex_start(&dg, KEX_ROLE_DONGLE, dpriv, dpre, dmac, kmac);
    kex_start(&kb, KEX_ROLE_KEYBOARD, kpriv, kpre, kmac, dmac);
    for (slot = 0; slot < maxslots; slot++) {
        if (mode == 3 && kex_state(&dg) == KEX_DH && g_fail_at < 0) g_fail_at = g_blocks;
        if (mode == 4 && slot == 40) kex_expire(&dg);
        kex_work(&dg, (unsigned)steps);
        kex_work(&kb, (unsigned)steps);
        if (kex_tx(&dg, f)) {
            dg_tx++;
            if (mode == 1 && !flipped && (f[0] >> 4) == KEX_MSG_PUB) { f[3] ^= 0x10; flipped = 1; }
            if (mode == 2 && (f[0] >> 4) == KEX_MSG_PUB) memset(f + 1, 0, 8);
            if (mode == 6 && (f[0] >> 4) == KEX_MSG_CONFIRM) memset(f + 1, 0, 8);
            if ((int)rnd() >= loss) kex_rx(&kb, f);
        }
        if (kex_tx(&kb, f)) {
            kb_tx++;
            if (kb.peer_pub_mask != 0xF) kb_tx_before_full++;
            if ((f[0] >> 4) == KEX_MSG_CONFIRM && !kb.peer_conf_ok) kb_conf_before_ok++;
            if (mode == 5 && !flipped && (f[0] >> 4) == KEX_MSG_CONFIRM) { f[5] ^= 0x01; flipped = 1; }
            if ((int)rnd() >= loss) kex_rx(&dg, f);
        }
        if (kex_state(&dg) == KEX_DONE && kex_state(&kb) == KEX_DONE) break;
        if (kex_state(&dg) == KEX_FAILED && kex_state(&kb) == KEX_FAILED && !dg.abort_tx && !kb.abort_tx) break;
    }
    took_d = kex_take_link_key(&dg, lk_d);
    took_k = kex_take_link_key(&kb, lk_k);
    printf("%d %d %d %d %d %d ", kex_state(&dg), kex_failure(&dg), took_d,
           kex_state(&kb), kex_failure(&kb), took_k);
    put(lk_d, 16); printf(" "); put(lk_k, 16);
    printf(" %d %ld %ld %ld %ld %d %d\n", slot, dg_tx, kb_tx, kb_tx_before_full, kb_conf_before_ok,
           kex_take_link_key(&dg, lk_d), (int)sizeof(kex_t));
    return 0;
}
"""

DONE, FAILED = 5, 6
F_LOW_ORDER, F_ENGINE, F_CONFIRM, F_PEER_ABORT, F_TIMEOUT = 1, 2, 3, 4, 5
BASE = bytes([9]) + bytes(31)
ZERO16 = bytes(16).hex()


class KexProtoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = next((p for p in map(shutil.which, ("cc", "gcc", "clang")) if p), None)
        if cc is None:
            raise unittest.SkipTest("no host C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "harness.c").write_text(HARNESS)
        cls.exe = tmp / "kex_proto_host"
        subprocess.run(
            [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{INC}",
             str(tmp / "harness.c"), str(SRC / "kex_proto.c"), str(SRC / "kex_kdf.c"),
             str(SRC / "x25519.c"), str(SRC / "aes_sw.c"), "-o", str(cls.exe)],
            check=True,
        )
        cls.rng = random.Random(0x4B4558)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def material(self):
        r = self.rng
        return dict(dpriv=r.randbytes(32), kpriv=r.randbytes(32),
                    prefix=b"OKX1" + bytes([0xA6, 2]) + r.randbytes(25),
                    dmac=r.randbytes(6), kmac=r.randbytes(6))

    def run_kex(self, m, loss=0, steps=4, seed=1, mode=0, maxslots=200000, kprefix=None):
        out = subprocess.run(
            [str(self.exe), m["dpriv"].hex(), m["kpriv"].hex(), m["prefix"].hex(),
             (kprefix or m["prefix"]).hex(), m["dmac"].hex(), m["kmac"].hex(),
             str(loss), str(steps), str(seed), str(mode), str(maxslots)],
            capture_output=True, text=True, check=True).stdout.split()
        keys = ("dg_state", "dg_fail", "dg_took", "kb_state", "kb_fail", "kb_took")
        res = {k: int(v) for k, v in zip(keys, out[:6])}
        res.update(lk_d=out[6], lk_k=out[7], slots=int(out[8]), dg_tx=int(out[9]),
                   kb_tx=int(out[10]), kb_early=int(out[11]), kb_conf_early=int(out[12]),
                   second_take=int(out[13]), size=int(out[14]))
        return res

    def expected_lk(self, m):
        d_pub = ref_x25519(m["dpriv"], BASE)
        k_pub = ref_x25519(m["kpriv"], BASE)
        z = ref_x25519(m["dpriv"], k_pub)
        self.assertEqual(z, ref_x25519(m["kpriv"], d_pub))
        return kex_ref.derive(z, m["prefix"] + d_pub + k_pub + bytes(8))["lk"].hex()

    def assert_agreed(self, res, m):
        self.assertEqual((res["dg_state"], res["kb_state"]), (DONE, DONE), res)
        self.assertEqual((res["dg_took"], res["kb_took"]), (0, 0))
        self.assertEqual(res["lk_d"], res["lk_k"])
        self.assertEqual(res["lk_d"], self.expected_lk(m))
        self.assertEqual(res["second_take"], 1, "the link key must be released only once")

    def assert_no_key(self, res):
        self.assertNotEqual(res["dg_state"], DONE, res)
        self.assertEqual(res["dg_took"], 1)
        self.assertEqual(res["lk_d"], ZERO16)

    def test_clean_link_agrees_on_the_reference_key(self):
        m = self.material()
        res = self.run_kex(m)
        self.assert_agreed(res, m)
        self.assertEqual(res["kb_early"], 0,
                         "the keyboard must not speak before it holds the whole dongle key")
        self.assertEqual(res["kb_conf_early"], 0,
                         "the keyboard must not confirm before the dongle has proved itself")

    def test_agreement_survives_loss_and_any_work_rate(self):
        for loss in (10, 50, 80):
            for steps in (1, 3, 40):
                for seed in (1, 2, 3):
                    m = self.material()
                    res = self.run_kex(m, loss=loss, steps=steps, seed=seed)
                    self.assert_agreed(res, m)
                    self.assertEqual(res["kb_early"], 0)
                    self.assertEqual(res["kb_conf_early"], 0)

    def test_a_tampered_public_key_fragment_yields_no_key(self):
        m = self.material()
        res = self.run_kex(m, mode=1)
        self.assert_no_key(res)
        self.assertEqual((res["kb_state"], res["kb_fail"]), (FAILED, F_CONFIRM))
        self.assertEqual((res["dg_state"], res["dg_fail"]), (FAILED, F_PEER_ABORT))
        self.assertEqual(res["kb_took"], 1)

    def test_a_different_transcript_yields_no_key(self):
        m = self.material()
        other = bytearray(m["prefix"])
        other[20] ^= 0x01
        res = self.run_kex(m, kprefix=bytes(other))
        self.assert_no_key(res)
        self.assertEqual(res["kb_took"], 1)
        self.assertEqual(res["lk_k"], ZERO16)

    def test_a_low_order_public_key_is_refused(self):
        m = self.material()
        res = self.run_kex(m, mode=2)
        self.assertEqual((res["kb_state"], res["kb_fail"]), (FAILED, F_LOW_ORDER))
        self.assert_no_key(res)

    def test_an_engine_fault_yields_no_key(self):
        m = self.material()
        res = self.run_kex(m, mode=3)
        # The two instances share one AES in this harness, so the injected
        # fault lands on whichever end derives first. Either way: that end
        # reports the engine, the other is told, and neither has a key.
        ends = {(res["dg_state"], res["dg_fail"]), (res["kb_state"], res["kb_fail"])}
        self.assertEqual(ends, {(FAILED, F_ENGINE), (FAILED, F_PEER_ABORT)})
        self.assert_no_key(res)
        self.assertEqual(res["kb_took"], 1)
        self.assertEqual(res["lk_k"], ZERO16)

    def test_expiry_yields_no_key_and_tells_the_peer(self):
        m = self.material()
        res = self.run_kex(m, mode=4, steps=1)
        self.assertEqual((res["dg_state"], res["dg_fail"]), (FAILED, F_TIMEOUT))
        self.assertEqual((res["kb_state"], res["kb_fail"]), (FAILED, F_PEER_ABORT))
        self.assert_no_key(res)

    def test_a_forged_keyboard_confirmation_is_refused(self):
        m = self.material()
        res = self.run_kex(m, mode=5, maxslots=20000)
        self.assertEqual((res["dg_state"], res["dg_fail"]), (FAILED, F_CONFIRM))
        self.assert_no_key(res)

    def test_a_forged_dongle_confirmation_is_refused(self):
        m = self.material()
        res = self.run_kex(m, mode=6, maxslots=20000)
        self.assertEqual((res["kb_state"], res["kb_fail"]), (FAILED, F_CONFIRM))
        self.assertEqual(res["kb_took"], 1)
        self.assert_no_key(res)

    def test_context_size_is_reported(self):
        """Not a limit, a number worth seeing: this is static RAM on both ends."""
        res = self.run_kex(self.material())
        self.assertLess(res["size"], 640)


if __name__ == "__main__":
    unittest.main()
