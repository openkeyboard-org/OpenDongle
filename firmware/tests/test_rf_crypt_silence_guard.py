"""The authenticated-HID silence guard is a DEADLINE, not a reception count.

This pins the fix for the defect measured on silicon 2026-08-23: the guard was
budgeted in connected receptions (`RF_CRYPT_SILENCE_FRAMES` 64), spent by every
arriving frame -- overwhelmingly the keyboard's own bare 1-byte poll acks at
~975/s -- but refilled only by an authenticated frame, which the keyboard
schedules against its OWN reception count. Two clocks sharing one budget put the
real deadline at ~65 ms against a ~33 ms keepalive, and the guard released 7 of
7 healthy keyed links: `enc_shape == ok` exactly, zero drops, zero FIFO loss and
a single session mint on every one of them.

A reception count was also attacker-rate-dependent in the wrong direction -- a
faster flood spent the budget sooner, so the mechanism meant to bound an
attacker handed them the teardown instead.

Three classes here:

  * the deadline POLICY, compiled from the real `rf_crypt.h` inline. It must
    scale with the connection interval: a fixed 500 ms is ~15 keepalives at the
    stock interval of 28 but under two at the interval of 300 that
    `bond_record_semantic_valid()` still accepts -- the original defect at a
    slower cadence.
  * the deadline ARITHMETIC. Every failure mode of a modular deadline lives
    here and none is reachable on a chip: a clock wrap, and the
    stamp-from-the-future the radio IRQ sink produces by preempting the caller
    between its stamp load and its clock read.
  * the wiring in `rf_task.c`, so the lifecycle holes found in adversarial
    review cannot silently return.
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "common" / "include"
RF_TASK = ROOT / "common" / "src" / "rf_task.c"

HARNESS = r"""
#include "rf_crypt.h"
#include <stdio.h>

static int fails;

static void ck(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}

int main(void) {
    /* ---- policy: the deadline tracks the connection interval ---- */

    /* Stock interval 28 -> 28*32*15 = 13440 ticks, below the floor. */
    ck(rf_crypt_silence_deadline_ticks(28) == RF_CRYPT_SILENCE_MIN_TICKS,
       "stock interval clamps up to the floor");
    /* Interval 300 is still accepted by bond_record_semantic_valid(); a FIXED
       500 ms would be under two keepalives there. It must scale, then clamp. */
    ck(rf_crypt_silence_deadline_ticks(300) == RF_CRYPT_SILENCE_MAX_TICKS,
       "slow interval clamps down to the ceiling");
    /* Mid-range scales linearly and is not clamped at either end. */
    ck(rf_crypt_silence_deadline_ticks(100) == 100u * 32u * 15u,
       "mid interval scales linearly");
    ck(rf_crypt_silence_deadline_ticks(0) == RF_CRYPT_SILENCE_MIN_TICKS,
       "zero interval still yields a usable deadline");
    /* Whatever the interval, the deadline is always at least this many
       keepalives -- that ratio is the whole point of the fix. */
    for (unsigned ivl = 1; ivl <= 300u; ivl++) {
        uint32_t d = rf_crypt_silence_deadline_ticks((uint16_t)ivl);
        uint32_t keepalive = ivl * RF_CRYPT_KEEPALIVE_POLLS;
        ck(d >= keepalive * 4u, "deadline keeps >=4 keepalives of headroom");
    }

    /* ---- arithmetic ---- */
    const uint32_t dl = 30000000u;   /* 500 ms at 60 ticks/us (CH592) */
    struct { uint32_t now, stamp; int expect; const char *what; } c[] = {
        { 1000u,            1000u,       0, "just authenticated" },
        { 1000u + dl - 1u,  1000u,       0, "one tick short" },
        { 1000u + dl,       1000u,       1, "exactly at deadline" },
        { 1000u + dl + 1u,  1000u,       1, "past deadline" },

        /* 2^32 wrap: a non-modular comparison reads these as "stamp is in the
           future" and would never fire. */
        { 10u,              0xFFFFFFFFu, 0, "wrap, 11 ticks idle" },
        { dl - 2u,          0xFFFFFFFEu, 1, "wrap, past deadline" },

        /* Stamp from the future: the IRQ sink refreshed it between the
           caller's stamp load and its clock read. Must read as fresh, NOT
           underflow to ~2^32 and force-release a healthy link. */
        { 1000u,            2000u,       0, "stamp 1000 ticks ahead" },
        { 0u,               1u,          0, "stamp one ahead across 0" },

        /* Zero is a legitimate clock value, not a sentinel: a link whose
           stamp landed on 0 must still be judged normally. */
        { dl,               0u,          1, "stamp==0 is a real timestamp" },
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        ck(rf_crypt_silence_expired(c[i].now, c[i].stamp, dl) == c[i].expect,
           c[i].what);
    }

    printf(fails ? "FAILURES %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
"""


class SilenceDeadlinePolicyAndArithmetic(unittest.TestCase):
    def test_inline_scales_with_interval_and_survives_wrap(self):
        cc = shutil.which("cc") or shutil.which("gcc")
        self.assertIsNotNone(cc, "need a host C compiler")
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "h.c"
            src.write_text(HARNESS)
            exe = Path(td) / "h"
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 f"-I{INC}", str(src), "-o", str(exe)],
                check=True, capture_output=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("ALL OK", r.stdout)


class SilenceGuardWiring(unittest.TestCase):
    def setUp(self):
        self.src = RF_TASK.read_text()

    def test_guard_is_not_a_frame_count(self):
        """The reception-count budget is what failed; it must not come back."""
        self.assertNotIn("RF_CRYPT_SILENCE_FRAMES", self.src)
        self.assertNotIn("rf_crypt_frames_since_ok", self.src)

    def test_only_a_verified_frame_refreshes_liveness(self):
        """The stamp is the whole security property.

        Exactly one site may refresh it on a LIVE link -- the RF_CRYPT_OK
        branch of the crypt-RX drain. Every other write is a start-of-epoch arm
        and goes through rf_crypt_arm_silence(). If anything an attacker can
        provoke refreshed the stamp, unauthenticated traffic would hold the
        link open, which is precisely what this guard exists to prevent.
        """
        direct = re.findall(r"^\s*rf_crypt_last_auth_tsys\s*=\s*([^;]+);",
                            self.src, re.M)
        # One inside rf_crypt_arm_silence(), one for the verified frame.
        self.assertEqual(len(direct), 2,
                         f"unexpected direct stamp writes: {direct}")

    def test_promotes_arm_inline_not_via_the_mint_event(self):
        """Both promotes to CONNECTED must arm before posting the mint.

        The mint is delivered as RF_EVT_CRYPT_SESSION, but rf_send_poll
        evaluates the guard every conn_interval. If the stamp were only written
        by the mint handler, the guard would run against the PREVIOUS session's
        stamp -- seconds old after a reacquire -- and force-release on the first
        poll of every reconnect. The old reception counter masked this window
        because it needed 64 arrivals to trip; a deadline does not.
        """
        found = 0
        for m in re.finditer(
                r"rf_state = RF_STATE_CONNECTED;(.{0,900}?)"
                r"hal_event_post\(RF_EVT_CRYPT_SESSION\)", self.src, re.S):
            self.assertIn("rf_crypt_arm_silence()", m.group(1),
                          "a promote to CONNECTED posts the mint without "
                          "arming the deadline; the guard fires on poll 1")
            found += 1
        self.assertEqual(found, 2, "expected exactly two promote sites")

    def test_key_destruction_leaves_the_guard_armed(self):
        """Tombstone and live key-removal keep encryption REQUIRED with no key.

        Nothing can authenticate afterwards, so the guard is the only thing
        that ends the link. Standing it down there would let an attacker keep
        supervision fresh on a dead link forever -- a strictly worse outcome
        than the false releases this change fixes.
        """
        tomb = self.src[self.src.index("void RF_TombstoneBond"):]
        tomb = tomb[:tomb.index("\n}\n")]
        self.assertIn("rf_crypt_arm_silence()", tomb,
                      "tombstone must leave the silence guard armed")

    def test_guard_is_not_suppressed_by_a_pending_release(self):
        """The fire site must NOT gate on !rf_crypt_force_release.

        Suppressing the re-post while a release is in flight reads as an
        obvious optimisation and is a trap: RF_EVT_TIMEOUT shares its slot with
        EV10, and rf_arm_connected_supervision() cancels it -- which on CH592
        drains an already-posted event. Lose it once inside a connected epoch
        and the latch stays set with no reachable arm site to clear it, so
        unauthenticated traffic holds the link open forever. Re-posting is
        idempotent; suppression is not safe.
        """
        fire = self.src[self.src.index("rf_crypt_silence_armed)"):]
        fire = fire[:fire.index("hal_event_post(RF_EVT_TIMEOUT)")]
        self.assertNotIn("!rf_crypt_force_release", fire,
                         "the guard must re-evaluate even with a release "
                         "already latched; a cancelled event would otherwise "
                         "disarm it permanently")

    def test_release_is_rechecked_before_teardown(self):
        """A frame can verify between the ISR posting and the handler running.

        Tearing down a link that has just authenticated is the same false
        release, one dispatch later, so the handler re-evaluates the deadline.
        """
        h = self.src[self.src.index("if (rf_crypt_force_release) {"):]
        h = h[:h.index("rf_enter_stock_reacquire();")]
        self.assertIn("rf_crypt_silence_expired(", h,
                      "the timeout handler must re-check the deadline")


if __name__ == "__main__":
    unittest.main()
