# OpenKeyboard.org OpenDongle
# Copyright 2026 Eric Molitor (EMulator)
# SPDX-License-Identifier: Apache-2.0
"""Host test for the connected data-hop model in rf_protocol.h.

The header is chip-agnostic (it includes only <stdint.h>), so the real
rf_proto_hop_step() is compiled with the host C compiler behind a tiny stdin
driver and exercised against the keyboard's hop model, the seeding contract
and the modulus arithmetic. Skipped when no C compiler is on PATH."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

INCLUDE = Path(__file__).resolve().parents[1] / "common" / "include"

DRIVER = r"""
#include <stdio.h>
#include "rf_protocol.h"
/* "S <last> <idx>" seeds, "P <now> <interval>" steps and prints "<last> <idx>". */
int main(void)
{
    rf_proto_hop_t h = {0u, 0u};
    char op; unsigned long a, b;
    printf("C %lx %lu %lu\n", (unsigned long)RF_PROTO_HOP_WRAP,
           (unsigned long)RF_PROTO_HOP_EDGE_LEAD, (unsigned long)RF_PROTO_DATA_CHANNEL_COUNT);
    while (scanf(" %c %lx %lu", &op, &a, &b) == 3) {
        if (op == 'S') { h.last = (uint32_t)a; h.prev_idx = (uint8_t)b; continue; }
        {   /* step first: printf's argument evaluation order is unspecified */
            unsigned idx = rf_proto_hop_step(&h, (uint32_t)a, (uint16_t)b);
            printf("%lx %u\n", (unsigned long)h.last, idx);
        }
    }
    return 0;
}
"""

INTERVAL = 28          # hop ticks per poll slot on the live link (875 us)


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class HopModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler on PATH")
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "hop_driver.c"
        src.write_text(DRIVER)
        cls.exe = Path(cls.tmp.name) / "hop_driver"
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(INCLUDE),
                        "-o", str(cls.exe), str(src)], check=True)
        cls.WRAP, cls.LEAD, cls.NCH = cls._run([])[0]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def _run(cls, script):
        """script: list of ('S', last, idx) / ('P', now, interval), one driver run.
        Returns the constants tuple first, then one (last, idx) per 'P'."""
        text = "".join("%s %x %d\n" % (op, a, b) for op, a, b in script)
        out = subprocess.run([str(cls.exe)], input=text, capture_output=True,
                             text=True, check=True).stdout.split("\n")
        m = re.match(r"C ([0-9a-f]+) (\d+) (\d+)", out[0])
        consts = (int(m.group(1), 16), int(m.group(2)), int(m.group(3)))
        steps = [(int(line.split()[0], 16), int(line.split()[1])) for line in out[1:] if line]
        return [consts] + steps

    def wrap(self, t):
        return t % self.WRAP

    # -- the keyboard's model, as the header documents it: an anchor 12 ticks before
    # the poll it expects (it seeds at 13, its servo and one-tick rollback settle at
    # 12), one edge per whole interval, an edge once elapsed EXCEEDS the interval.
    # Returns the edges the keyboard has counted at time t (t relative to the
    # poll-grid origin, t >= 0): floor((t + 11) / 28), against the dongle's
    # floor((t + 13) / 28); they differ, by exactly one, at remainders 15 and 16.
    @staticmethod
    def keyboard_steps(t):
        return (t + 12 - 1) // INTERVAL if t + 12 - 1 >= 0 else 0

    def test_constants(self):
        self.assertEqual(self.WRAP, 0xA8C00000)
        self.assertEqual(self.LEAD, 13)
        self.assertEqual(self.NCH, 5)

    def test_seed_contract_first_poll_is_seed_plus_one(self):
        """Seed = (grid origin - lead, S); a first poll anywhere from 15 to 42 ticks
        after the origin (one slot, a tick early or late, or half a slot early) computes
        step 1 -> S+1, the keyboard's first connected listen channel. (15 and 16 are
        the dongle's edge only; the keyboard model hops at 17, the two-tick window.)"""
        script, expect = [], []
        for origin in (0, 1000, self.WRAP - 20, self.WRAP - 1):
            for seed in range(self.NCH):
                for first in (15, 27, 28, 29, 42):
                    script += [("S", self.wrap(origin - self.LEAD), seed),
                               ("P", self.wrap(origin + first), INTERVAL)]
                    expect.append(((self.wrap(origin - self.LEAD + INTERVAL), (seed + 1) % self.NCH),
                                   (origin, seed, first)))
                for first in (13, 14):   # not yet an edge: step 0, index and anchor unchanged
                    script += [("S", self.wrap(origin - self.LEAD), seed),
                               ("P", self.wrap(origin + first), INTERVAL)]
                    expect.append(((self.wrap(origin - self.LEAD), seed), (origin, seed, first)))
        res = self._run(script)[1:]
        self.assertEqual(len(res), len(expect))
        for got, (want, why) in zip(res, expect):
            self.assertEqual(got, want, why)

    def test_steady_grid_never_drifts(self):
        """100k on-grid polls with +-1 tick read jitter, crossing the modulus: every
        poll is step 1 and the anchor stays exactly one lead ahead of the grid (the
        jitter lives in the phase remainder, it never accumulates)."""
        n = 100000
        origin = self.WRAP - 40000 * INTERVAL       # the modulus is crossed mid-run
        jit = [((k * 7919) % 3) - 1 for k in range(n)]
        script = [("S", self.wrap(origin - self.LEAD), 2)]
        script += [("P", self.wrap(origin + INTERVAL * (k + 1) + jit[k]), INTERVAL) for k in range(n)]
        r = self._run(script)
        for k in range(n):
            last, idx = r[k + 1]
            self.assertEqual(idx, (2 + k + 1) % self.NCH, k)
            self.assertEqual(last, self.wrap(origin - self.LEAD + INTERVAL * (k + 1)), k)

    def test_coalesced_gaps_track_the_keyboard(self):
        """After k on-grid polls, one poll arrives n_slots slots + r ticks late (the
        first poll after a coalesced gap is off-grid; masked IRQs on the bench), then
        the grid resumes. The dongle's index must equal the keyboard's everywhere
        except the two-tick window where the two ends' edges differ (there it is
        exactly one ahead), and the very next on-grid poll must agree for every r:
        neither end resets its anchor to the poll time, so a window hit costs one
        poll only. Gaps of 5, 10, 15 and 20 intervals (n_slots 4, 9, 14, 19) are the
        ones the recovered stock rule dropped."""
        seed = 4
        k = 3
        cases = []
        script = []
        for origin in (0, self.WRAP - 3 * INTERVAL):
            for n_slots in range(0, 21):
                for r in range(INTERVAL):
                    gap_t = INTERVAL * (k + 1 + n_slots) + r        # the off-grid poll
                    next_t = INTERVAL * (k + 2 + n_slots)             # the very next slot
                    script.append(("S", self.wrap(origin - self.LEAD), seed))
                    script += [("P", self.wrap(origin + INTERVAL * (i + 1)), INTERVAL) for i in range(k)]
                    script += [("P", self.wrap(origin + gap_t), INTERVAL),
                               ("P", self.wrap(origin + next_t), INTERVAL)]
                    cases.append((origin, n_slots, r, gap_t, next_t))
        res = self._run(script)[1:]          # one driver run; k + 2 results per case
        self.assertEqual(len(res), (k + 2) * len(cases))
        for c, (origin, n_slots, r, gap_t, next_t) in enumerate(cases):
            d_gap = res[c * (k + 2) + k][1]
            d_next = res[c * (k + 2) + k + 1][1]
            kb_gap = (seed + self.keyboard_steps(gap_t)) % self.NCH
            kb_next = (seed + self.keyboard_steps(next_t)) % self.NCH
            diff = (d_gap - kb_gap) % self.NCH
            # the models are deterministic: exactly one edge apart at r = 15, 16
            self.assertEqual(diff, 1 if r in (15, 16) else 0, (origin, n_slots, r))
            self.assertEqual(d_next, kb_next, (origin, n_slots, r))
            # the gap is n_slots + 1 intervals; when that is a multiple of 5 the
            # keyboard has cycled all five channels and the correct index is the
            # PREVIOUS one again. The old rule refused that repeat and forced a slot
            # forward: the defect. Here the repeat must stand.
            if (n_slots + 1) % self.NCH == 0 and r < 15:
                self.assertEqual(d_gap, (seed + k) % self.NCH, (origin, n_slots, r))

    def test_modular_add_does_not_overflow(self):
        """The anchor advance is (last + step*interval) mod WRAP. With last near WRAP
        and a large step the 32-bit sum exceeds 2^32; a truncated sum would slip past a
        single reduction (the overflow the old repeat-correction had)."""
        r = self._run([("S", 0xA8BFFFFF, 0), ("P", 0x60000000, INTERVAL)])
        elapsed = self.WRAP - 0xA8BFFFFF + 0x60000000
        step = elapsed // INTERVAL
        self.assertEqual(r[1], (self.wrap(0xA8BFFFFF + step * INTERVAL), step % self.NCH))
        self.assertEqual(r[1][0], 0x5FFFFFF3)
        r = self._run([("S", self.WRAP - 1, 0), ("P", 27, INTERVAL)])    # one slot across the modulus
        self.assertEqual(r[1], (27, 1))
        r = self._run([("S", self.WRAP - 1, 3), ("P", self.WRAP - 1, INTERVAL)])   # zero elapsed
        self.assertEqual(r[1], (self.WRAP - 1, 3))

    def test_zero_interval_is_one_step_per_call(self):
        r = self._run([("S", 100, 1), ("P", 100, 0), ("P", 5000, 0)])
        self.assertEqual(r[1], (100, 2))
        self.assertEqual(r[2], (5000, 3))


if __name__ == "__main__":
    unittest.main()
