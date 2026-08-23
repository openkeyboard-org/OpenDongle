# SPDX-License-Identifier: Apache-2.0
"""Pin bond_carry_link_key(): a same-peer re-pair must not destroy a key.

A fresh-pair persist rebuilds the candidate record from RAM, which cannot carry
link_key (rf_commit_bond_ram runs in the radio-IRQ sink; the AES engine has no
key readback). Before the fix that keyless candidate was written over a
provisioned key -- measured 8/8 on silicon, flags 0x03 -> 0x01. The carry runs in
rf_persist_bond_task, the one place the stored and candidate records are both in
scope, and it must hold these properties:

  * same peer + keyed store  -> key carried, and bond_tuple_equal then TRUE so
    the write is skipped entirely (the regression pin for OD-01)
  * different peer           -> never inherit a key
  * keyless store            -> candidate untouched
  * candidate already keyed  -> left alone (a host write is authoritative)
  * same peer, new session AA -> key carried but the tuple still differs, so the
    write proceeds WITH the key preserved
  * every result is canonical per bond_key_flags_valid()

Headers-only harness (bond.h is stdint-only), mirroring test_rf_negotiation.py.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

INC = Path(__file__).resolve().parents[1] / "common" / "include"

HARNESS = r"""
#include "bond.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

/* A canonical keyed record for peer AA:..:AA on one session AA. */
static void mk_stored(bond_record_t *r)
{
    int i;
    memset(r, 0, sizeof(*r));
    r->session_aa = 0x11223344u;
    r->conn_interval = 28u;
    r->conn_timeout = 600u;
    for (i = 0; i < 6; i++) { r->peer_mac[i] = 0xAAu; }
    r->flags = BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY;
    for (i = 0; i < 16; i++) { r->link_key[i] = (uint8_t)(0x10 + i); }
}

/* What rf_commit_bond_ram() rebuilds: same tuple, capability only, NO key. */
static void mk_candidate(bond_record_t *r)
{
    int i;
    memset(r, 0, sizeof(*r));
    r->session_aa = 0x11223344u;
    r->conn_interval = 28u;
    r->conn_timeout = 600u;
    for (i = 0; i < 6; i++) { r->peer_mac[i] = 0xAAu; }
    r->flags = BOND_FLAG_ENC_CAPABLE;
}

int main(void)
{
    bond_record_t stored, want;
    int i;

    /* 1. THE OD-01 PIN: same peer, keyed store -> key carried AND the write
     *    is skipped, because the tuple now matches. */
    mk_stored(&stored);
    mk_candidate(&want);
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) != 0, "same peer: ENC_KEY set");
    check(memcmp(want.link_key, stored.link_key, 16) == 0,
          "same peer: key bytes carried");
    check(bond_tuple_equal(&stored, &want) == 1,
          "same peer: tuple equal -> persist skipped");
    check(bond_key_flags_valid(&want) == 1, "same peer: canonical");

    /* 2. A DIFFERENT keyboard must never inherit the previous key. */
    mk_stored(&stored);
    mk_candidate(&want);
    for (i = 0; i < 6; i++) { want.peer_mac[i] = 0xBBu; }
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) == 0, "peer change: no ENC_KEY");
    check(bond_key_is_zero(want.link_key) == 1, "peer change: key stays blank");
    check(bond_key_flags_valid(&want) == 1, "peer change: canonical");

    /* 3. A keyless store leaves the candidate untouched. */
    mk_stored(&stored);
    stored.flags = BOND_FLAG_ENC_CAPABLE;
    memset(stored.link_key, 0, 16);
    mk_candidate(&want);
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) == 0, "keyless store: no ENC_KEY");
    check(bond_key_is_zero(want.link_key) == 1, "keyless store: key blank");

    /* 4. A candidate that already carries a key is authoritative (a host
     *    BondWrite must not be overwritten by the stored key). */
    mk_stored(&stored);
    mk_candidate(&want);
    want.flags |= BOND_FLAG_ENC_KEY;
    for (i = 0; i < 16; i++) { want.link_key[i] = 0x77u; }
    bond_carry_link_key(&stored, &want);
    for (i = 0; i < 16; i++) {
        check(want.link_key[i] == 0x77u, "candidate keyed: own key kept");
    }

    /* 5. Same peer but a NEW session AA: carry the key, but the tuple still
     *    differs so the write proceeds -- with the key preserved. */
    mk_stored(&stored);
    mk_candidate(&want);
    want.session_aa = 0x55667788u;
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) != 0, "new AA: key carried");
    check(bond_tuple_equal(&stored, &want) == 0, "new AA: tuple differs");
    check(bond_key_flags_valid(&want) == 1, "new AA: canonical");

    /* 6. A store whose flags claim a key but whose bytes are blank is not
     *    canonical and must never be propagated. */
    mk_stored(&stored);
    memset(stored.link_key, 0, 16);          /* ENC_KEY set, key blank */
    mk_candidate(&want);
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) == 0, "non-canonical store: ignored");

    /* 7. An all-FF (erased-page) key is likewise refused. */
    mk_stored(&stored);
    memset(stored.link_key, 0xFF, 16);
    mk_candidate(&want);
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) == 0, "all-FF store: ignored");

    /* 8. Unit property only: the carry sets ENC_KEY and nothing else, so a
     *    candidate that somehow arrived without ENC_CAPABLE stays inert under
     *    the AND-gate. NOTE this state is NOT reachable through the same-peer
     *    re-pair path -- rf_crypt_peer_capable is loaded from the stored record
     *    at boot and never cleared on a same-peer accept, so the real candidate
     *    always carries ENC_CAPABLE and the pair is restored to 0x03. Pinned as
     *    a property of the helper, not as a claim about that scenario. */
    mk_stored(&stored);
    mk_candidate(&want);
    want.flags = 0u;
    bond_carry_link_key(&stored, &want);
    check((want.flags & BOND_FLAG_ENC_KEY) != 0, "carry sets ENC_KEY only");
    check((want.flags & BOND_FLAG_ENC_CAPABLE) == 0, "carry adds no capability");
    check(bond_enc_active(&want) == 0, "KEY without CAPABLE stays inert");
    check(bond_key_flags_valid(&want) == 1, "KEY without CAPABLE canonical");

    /* 9. The reachable same-peer case restores the full relationship: the
     *    candidate arrives with CAPABLE (from the boot-loaded latch), so the
     *    carried key yields 0x03 and encryption stays ACTIVE across the re-pair.
     *    This is the behaviour the fix is FOR. */
    mk_stored(&stored);
    mk_candidate(&want);                      /* mk_candidate sets CAPABLE */
    bond_carry_link_key(&stored, &want);
    check(bond_enc_active(&want) == 1, "same peer: encryption still active");
    check((want.flags & (BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY))
          == (BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY), "same peer: 0x03");

    if (!fails) { printf("OK\n"); }
    return fails ? 1 : 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class BondKeyPreservation(unittest.TestCase):
    def test_same_peer_repair_keeps_the_provisioned_key(self):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "bondkeykat"
            subprocess.run(
                [cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 f"-I{INC}", "-o", str(binary), str(d / "harness.c")],
                check=True, capture_output=True,
            )
            proc = subprocess.run([str(binary)], check=True,
                                  capture_output=True, text=True)
            self.assertEqual(proc.stdout.strip(), "OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()
