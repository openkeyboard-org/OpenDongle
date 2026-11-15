"""Encryption is negotiated, never required: the CAPABLE && KEY decision.

`bond_enc_active()` is the single gate that decides whether a bond runs
encrypted. Backwards compatibility depends entirely on it being AND, not OR: a
stock keyboard (neither bit), a capable keyboard awaiting a key, and a key
provisioned for a peer that never advertised must all stay plaintext, so no
existing device is ever locked out. This pins the truth table by compiling the
real inline helper from `bond.h` — no chip, no `bond.c`, just the header.

The second class pins the beacon-accept latch policy from `rf_crypt.h`: the
encryption-required latch drops for a fresh dongle or a peer change (a new
keyless bond must not inherit the old key requirement) and survives a
same-peer accept (a reconnect can never downgrade to plaintext) — while the
capability latch survives EVERY accept. The capability half is the regression
pin for the 2026-08-16 review's finding 3: the advert precedes the first
beacon, so an accept-time clear erased the negotiation deterministically and
`BOND_FLAG_ENC_CAPABLE` was never persisted.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

INC = Path(__file__).resolve().parents[1] / "common" / "include"

HARNESS = r"""
#include "bond.h"
#include <stdio.h>

int main(void) {
    bond_record_t r;
    int fails = 0;

    /* (capable, key) -> active? Only (1,1) is active. */
    const struct { unsigned char flags; int active; } cases[] = {
        { 0,                                              0 },
        { BOND_FLAG_ENC_CAPABLE,                          0 },
        { BOND_FLAG_ENC_KEY,                              0 },
        { BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY,      1 },
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        r.flags = cases[i].flags;
        if ((bond_enc_active(&r) != 0) != cases[i].active) {
            printf("FAIL:flags=0x%02x\n", cases[i].flags);
            fails++;
        }
    }
    if (!fails) printf("OK\n");
    return fails ? 1 : 0;
}
"""


ACCEPT_HARNESS = r"""
#include "rf_crypt.h"
#include <stdio.h>

int main(void) {
    int fails = 0;

    /* (bond_valid, same_peer) -> encryption-required latch survives?
     * Only a same-peer accept on a valid bond keeps it. */
    const struct { unsigned char valid, same, enc_survives; } cases[] = {
        { 0, 0, 0 },   /* fresh dongle, new peer      */
        { 0, 1, 0 },   /* fresh dongle (no old peer to match, still keyless) */
        { 1, 0, 0 },   /* bonded, DIFFERENT keyboard  */
        { 1, 1, 1 },   /* bonded, same peer re-pair   */
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        volatile uint8_t enc = 1u;
        uint8_t capable = 1u;   /* latched by the advert two slots earlier */
        rf_crypt_beacon_accept_latches(cases[i].valid, cases[i].same,
                                       &enc, &capable);
        if ((enc != 0u) != cases[i].enc_survives) {
            printf("FAIL:enc valid=%u same=%u\n", cases[i].valid, cases[i].same);
            fails++;
        }
        /* The finding-3 pin: no accept may consume the capability latch. */
        if (capable != 1u) {
            printf("FAIL:capable valid=%u same=%u\n", cases[i].valid, cases[i].same);
            fails++;
        }
    }
    if (!fails) printf("OK\n");
    return fails ? 1 : 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class Negotiation(unittest.TestCase):
    def test_active_requires_both_capability_and_key(self):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "negkat"
            subprocess.run(
                [cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 f"-I{INC}", "-o", str(binary), str(d / "harness.c")],
                check=True, capture_output=True,
            )
            proc = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True
            )
            self.assertEqual(proc.stdout.strip(), "OK")


class BeaconAcceptLatches(unittest.TestCase):
    def test_capability_survives_accept_and_enc_scopes_to_peer(self):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "harness.c").write_text(ACCEPT_HARNESS)
            binary = d / "acceptkat"
            subprocess.run(
                [cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 f"-I{INC}", "-o", str(binary), str(d / "harness.c")],
                check=True, capture_output=True,
            )
            proc = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True
            )
            self.assertEqual(proc.stdout.strip(), "OK")


if __name__ == "__main__":
    unittest.main()
