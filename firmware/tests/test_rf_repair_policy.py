# SPDX-License-Identifier: Apache-2.0
"""Pin rf_crypt_repair_offered(): which pairing beacons from the stored
keyboard open the way to dropping its link key.

A keyboard that is unpaired and paired again has erased its key; one that is
only put into pairing has not. The dongle cannot tell them apart from the
beacon, so the beacon only makes an OFFER, and what the keyboard sends once
connected decides it: a frame that verifies under the stored key withdraws
the offer, a plaintext HID frame takes it up (rf_task.c, rf_repair_void).

This test pins the offer's rule: given the caller's three judgements, it is
made when and only when

  * the dongle has a bond and the beacon is from that bond's peer,
  * the beacon passes the gate a new keyboard's must (pairing window open,
    signal floor, usable MAC), and
  * it arrived on the pairing address, not the bond's.

A same-address beacon that has not earned what a new keyboard must earn --
outside the window, too weak, or on the bond's address -- makes no offer.
(An offer an earlier beacon made in the same power-up still stands.)

What it does not cover, and no host test does: whether the caller's
judgements are right (which address the frame really arrived on), the accept
site that calls this, the sink that turns plaintext into a request, the task
that drops the key, and the persist that writes the keyless record. Those
live in rf_task.c, which has no host harness.

Headers-only harness, mirroring test_rf_negotiation.py.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

INC = Path(__file__).resolve().parents[1] / "common" / "include"

HARNESS = r"""
#include "rf_crypt.h"
#include <stdio.h>

int main(void)
{
    int fails = 0;
    unsigned v;

    /* Every combination: the answer is the conjunction, nothing else. */
    for (v = 0; v < 16u; v++) {
        uint8_t bond_valid = (uint8_t)(v & 1u);
        uint8_t same_peer = (uint8_t)((v >> 1) & 1u);
        uint8_t new_pair_ok = (uint8_t)((v >> 2) & 1u);
        uint8_t on_bond_aa = (uint8_t)((v >> 3) & 1u);
        uint8_t want = (uint8_t)(bond_valid && same_peer && new_pair_ok && !on_bond_aa);
        uint8_t got = rf_crypt_repair_offered(bond_valid, same_peer,
                                              new_pair_ok, on_bond_aa);
        if (got != want) {
            printf("FAIL: valid=%u same=%u ok=%u bond_aa=%u -> %u\n",
                   bond_valid, same_peer, new_pair_ok, on_bond_aa, got);
            fails++;
        }
    }

    /* The cases that matter, by name. */
    if (!rf_crypt_repair_offered(1, 1, 1, 0)) {
        printf("FAIL: an authorised re-pair on the pairing address makes the offer\n");
        fails++;
    }
    if (rf_crypt_repair_offered(1, 1, 1, 1)) {
        printf("FAIL: a reconnect on the bond's address offers nothing\n");
        fails++;
    }
    if (rf_crypt_repair_offered(1, 1, 0, 0)) {
        printf("FAIL: a same-address beacon that fails the new-keyboard gate offers nothing\n");
        fails++;
    }
    if (rf_crypt_repair_offered(1, 0, 1, 0)) {
        printf("FAIL: a different keyboard is not this rule's case\n");
        fails++;
    }
    if (rf_crypt_repair_offered(0, 1, 1, 0)) {
        printf("FAIL: an unbonded dongle has no key to drop\n");
        fails++;
    }

    /* The offer never touches the accept latches: a same-peer accept keeps
     * the encryption requirement whether or not an offer was made. */
    {
        volatile uint8_t enc = 1u;
        uint8_t capable = 1u;

        (void)rf_crypt_repair_offered(1, 1, 1, 0);
        rf_crypt_beacon_accept_latches(1, 1, &enc, &capable);
        if (enc != 1u || capable != 1u) {
            printf("FAIL: the accept itself must keep the requirement\n");
            fails++;
        }
    }

    puts(fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class RepairPolicy(unittest.TestCase):
    def test_only_an_authorised_beacon_on_the_pairing_address_offers_a_repair(self):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "repairkat"
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
