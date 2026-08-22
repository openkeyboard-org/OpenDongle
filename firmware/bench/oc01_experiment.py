#!/usr/bin/env python3
"""OC-01 acceptance: does the capability advert land in the DOCUMENTED order?

Arm A (documented): dongle bond cleared, keyboard already broadcasting, dongle
       reset so it boots into the middle of that stream.
Arm B (control):    dongle bond cleared and reset first, camped in pairing, and
       only then does the keyboard start broadcasting.

Each trial clears the dongle bond and unpairs the keyboard, so no trial can
inherit a previous trial's ENC_CAPABLE. Success = bond flags bit0.

Corrections over oc01_experiment.py, both of which otherwise corrupt a run:
  * k.wait(0x32, ...) defaulted to since=0 while Kbd.status accumulates for the
    whole run, so from trial 2 on it returned instantly on an older CONNECTED.
    Every wait is scoped to a mark taken just before that trial's pair.
  * Arm A held only 6 s before arming. Measured on this bench the dongle's app
    is back ~12 s after the reset and the keyboard broadcasts for only ~5.3 s,
    so the window was marginal; 9.5 s is the value validated by
    ch570_validate.py, which reaches a pair 6/6.

Arm A never re-issues A6 51 on a failed pair: that would restart
pair_bcast_count at 0 (an advert slot) and bias the arm toward success.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("DONGLE_FAMILY", "0x70")
os.environ.setdefault("DONGLE_PROFILE", "1")
import ch570_validate as V  # noqa: E402

OPENDONGLE = os.environ.get(
    "OPENDONGLE_CLI",
    os.path.join(HERE, "..", "..", "tools", "target", "release", "opendongle"))
TRIALS = int(sys.argv[1]) if len(sys.argv) > 1 else 12
ARM_A_LEAD = 9.5


def log(m):
    print(f"[{time.time() - V.t0:7.1f}s] {m}", flush=True)


def iap(tries=25):
    for _ in range(tries):
        try:
            d = V.Iap()
            if d.handshake():
                d.arm()
                return d
        except BaseException:
            pass
        time.sleep(1.0)
    return None


def clear_bond():
    d = iap()
    if not d:
        return False
    try:
        r = d.txn(0x89)
        return bool(r) and r[2] == 0x00
    finally:
        d.close()


def reset_dongle():
    subprocess.run([OPENDONGLE, "--enter-bootloader", "--force"],
                   capture_output=True, timeout=30)


def wait_pairing(timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = iap(tries=1)
        if d:
            try:
                if "pairing" in d.status_line():
                    return True
            finally:
                d.close()
        time.sleep(0.5)
    return False


def kbd_enter_pairing(k):
    k.send([0xA6, 0x30]); time.sleep(0.5); k.pump()
    k.send([0xA6, 0x52]); time.sleep(1.2); k.pump()
    k.send([0xA6, 0x30]); time.sleep(0.5); k.pump()
    k.send([0xA6, 0x51]); time.sleep(0.2)


def read_flags(timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = iap(tries=1)
        if d:
            try:
                v, f = d.bond_flags()
                if v:
                    return f
            finally:
                d.close()
        time.sleep(0.5)
    return None


def trial(k, arm):
    if not clear_bond():
        return ("bondclear-failed", None)
    if arm == "A":
        reset_dongle()
        time.sleep(ARM_A_LEAD)
        mark = time.time() - V.t0
        kbd_enter_pairing(k)
    else:
        reset_dongle()
        if not wait_pairing():
            return ("dongle-not-pairing", None)
        mark = time.time() - V.t0
        kbd_enter_pairing(k)
    got = k.wait(0x32, 45.0, since=mark)
    f = read_flags()
    if f is None:
        return ("no-bond", None)
    return ("paired" if got else "paired(no-5B32)", f)


def main():
    log(f"OC-01 acceptance: {TRIALS} trials per arm")
    k = V.Kbd(V.KBD_PORT)
    log("keyboard port open; settling 12 s (DTR reset + OpenBoot hold)")
    time.sleep(12)
    k.ser.read(4096)

    res = {"A": [], "B": []}
    for arm in ("A", "B"):
        log(f"\n=== ARM {arm}: "
            + ("documented, keyboard-first (dongle joins mid-stream)"
               if arm == "A" else "control, dongle listening first") + " ===")
        for i in range(TRIALS):
            st, f = trial(k, arm)
            cap = (f & 0x01) if f is not None else None
            res[arm].append(cap)
            log(f"  trial {i + 1:2d}: {st:18s} "
                f"flags={'--' if f is None else '0x%02X' % f}"
                f"  ENC_CAPABLE={'?' if cap is None else cap}")

    print("\n================ OC-01 RESULT ================")
    for arm in ("A", "B"):
        v = [c for c in res[arm] if c is not None]
        n, s = len(v), sum(v)
        print(f"  Arm {arm}: capability latched {s}/{n}"
              + (f" ({100.0 * s / n:.0f}%)" if n else "")
              + f"   [{len(res[arm]) - n} inconclusive]")
    a = [c for c in res["A"] if c is not None]
    b = [c for c in res["B"] if c is not None]
    if a and b:
        ra, rb = sum(a) / len(a), sum(b) / len(b)
        print()
        if ra >= 0.9 and rb >= 0.9:
            print(f"  >>> OC-01 FIXED: capability latches in BOTH orders "
                  f"(A {ra:.0%}, B {rb:.0%}). The documented order was 0/10 "
                  f"before the advert-lead change.")
        elif rb >= 0.9 and ra <= 0.5:
            print(f"  >>> OC-01 STILL REPRODUCES: order matters "
                  f"(A {ra:.0%} vs B {rb:.0%})")
        else:
            print(f"  >>> INCONCLUSIVE (A {ra:.0%}, B {rb:.0%})")
    k.ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
