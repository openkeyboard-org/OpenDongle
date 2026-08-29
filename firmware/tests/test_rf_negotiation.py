"""Encryption is negotiated, never required: the CAPABLE && KEY decision.

`bond_enc_active()` is the single gate that decides whether a bond runs
encrypted. Backwards compatibility depends entirely on it being AND, not OR: a
stock keyboard (neither bit), a capable keyboard awaiting a key, and a key
provisioned for a peer that never advertised must all stay plaintext, so no
existing device is ever locked out. This pins the truth table by compiling the
real inline helper from `bond.h` — no chip, no `bond.c`, just the header.
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


if __name__ == "__main__":
    unittest.main()
