#!/usr/bin/env python3
"""OD-01 acceptance: does a same-peer re-pair still destroy the link key?

Same experiment as od01_experiment.py, with two corrections without which a
multi-trial run reports nonsense:

  * k.wait(0x32, ...) defaulted to since=0 and Kbd.status accumulates for the
    whole run, so from trial 2 on it returned instantly on trial 1's CONNECTED.
    OD-01 then read the bond ~2 s later, before the re-pair had completed, and
    scored whatever the previous trial left behind. Every wait is now scoped to
    a mark taken immediately before that trial's pair.
  * ensure_keyed() paired by polling until the dongle reported "pairing" and
    only then arming the keyboard. The dongle accepts a pair for ~2-3 s of app
    uptime and the keyboard broadcasts for ~5.3 s, so that always missed. It now
    uses the same late-arm timing as the trial itself.

Acceptance (plan): 8/8 key destroyed BEFORE the fix -> 0/8 after, with flags
staying 0x03.
"""
import os
import struct
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
PROVISION = os.path.join(HERE, "provision_link_key.py")
TRIALS = int(sys.argv[1]) if len(sys.argv) > 1 else 8

# Reset -> dongle radio ready is ~11-12 s; the keyboard's window is ~5.3 s and
# kbd_pair() spends ~2.4 s before A6 51 lands, so hold 6 s here.
REBOOT_HOLD = 6.0


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


def flags():
    d = iap()
    if not d:
        return None
    try:
        v, f = d.bond_flags()
        return f if v else None
    finally:
        d.close()


def record():
    """Raw 48-byte bond record, or None. Layout per common/include/bond.h:
    magic[0:4] version[4] flags[5] conn_interval[6:8] session_aa[8:12] ...
    peer_mac[22:28] link_key[28:44] (the device redacts the key) checksum[44:48].
    """
    d = iap()
    if not d:
        return None
    try:
        r = d.txn(0x88)
        return bytes(r[3:3 + 48]) if r and r[0] == 0x88 else None
    finally:
        d.close()


def session_aa(rec):
    return None if rec is None else struct.unpack_from("<I", rec, 8)[0]


def reset_dongle():
    subprocess.run([OPENDONGLE, "--enter-bootloader", "--force"],
                   capture_output=True, timeout=30)


def kbd_pair(k):
    k.send([0xA6, 0x30]); time.sleep(0.5); k.pump()
    k.send([0xA6, 0x52]); time.sleep(1.2); k.pump()
    k.send([0xA6, 0x30]); time.sleep(0.5); k.pump()
    k.send([0xA6, 0x51]); time.sleep(0.2)


def pair_into_boot_window(k, clear_first):
    """Reset the dongle and land the keyboard's pair window on its boot.

    Returns (connected, mark). `mark` scopes the CONNECTED check to this pair.
    """
    if clear_first:
        d = iap()
        if d:
            d.txn(0x89)
            d.close()
    reset_dongle()
    time.sleep(REBOOT_HOLD)
    mark = time.time() - V.t0
    kbd_pair(k)
    return k.wait(0x32, 45.0, since=mark), mark


def ensure_keyed(k):
    """Get the pair back to flags == 0x03 (capable + key)."""
    if flags() == 0x03:
        return True
    pair_into_boot_window(k, clear_first=True)
    time.sleep(2.0)
    f = flags() or 0
    if not (f & 0x01):
        return False
    p = subprocess.run(["/usr/bin/python3", "-I", PROVISION,
                        "--key", V.BENCH_KEY.hex()],
                       capture_output=True, text=True, timeout=40)
    if p.returncode != 0:
        return False
    k.send(bytes([0xAE]) + V.BENCH_KEY); time.sleep(1.0); k.pump()
    return flags() == 0x03


def main():
    log(f"OD-01 acceptance: {TRIALS} trials")
    k = V.Kbd(V.KBD_PORT)
    log("keyboard port open; settling 12 s (DTR reset + OpenBoot hold)")
    time.sleep(12)
    k.ser.read(4096)

    wiped = kept = bad = 0
    for i in range(TRIALS):
        if not ensure_keyed(k):
            log(f"  trial {i + 1:2d}: SETUP FAILED (could not reach flags 0x03)")
            bad += 1
            continue
        # The experiment: same-peer re-pair into the boot window with the bond
        # NOT cleared. Pre-fix this re-persisted a keyless record over the key.
        before = record()
        got, _ = pair_into_boot_window(k, clear_first=False)
        time.sleep(2.0)
        after = record()
        f = flags()
        # PRECONDITION CHECK. A fresh pair mints a new session AA, so an
        # unchanged AA means the dongle never accepted the pair and never
        # re-persisted -- the key-preservation path was not entered at all and
        # "key kept" would be vacuous. Measured 2026-08-22: a dongle holding a
        # valid bond stays in conn=waiting-reconnect and refuses a same-peer
        # fresh pair even when the keyboard re-arms across the whole reboot, so
        # every trial lands here. Without this check the run reported 8/8 FIXED
        # while measuring nothing.
        if before is not None and after is not None \
                and session_aa(before) == session_aa(after):
            log(f"  trial {i + 1:2d}: PRECONDITION NOT MET -- session AA "
                f"unchanged (0x{session_aa(after):08X}); the dongle never "
                f"accepted the re-pair, so nothing was re-persisted "
                f"(connected={got})")
            bad += 1
        elif f is None:
            log(f"  trial {i + 1:2d}: no bond after re-pair (connected={got})")
            bad += 1
        elif f == 0x03:
            log(f"  trial {i + 1:2d}: 0x03 -> 0x{f:02X}  key KEPT "
                f"(AA re-minted, connected={got})")
            kept += 1
        else:
            log(f"  trial {i + 1:2d}: 0x03 -> 0x{f:02X}  *** KEY DESTROYED *** "
                f"(connected={got})")
            wiped += 1

    print("\n================ OD-01 RESULT ================")
    print(f"  key destroyed : {wiped}")
    print(f"  key kept      : {kept}")
    print(f"  inconclusive  : {bad}")
    if wiped:
        print(f"\n  >>> OD-01 STILL REPRODUCES: {wiped} same-peer re-pairs "
              f"re-persisted a keyless bond over a provisioned key.")
    elif kept:
        print(f"\n  >>> OD-01 key preserved across {kept}/{kept} same-peer re-pairs\n      that the dongle actually accepted (session AA re-minted each time).")
    else:
        print("\n  >>> INCONCLUSIVE: no trial met the precondition. The dongle\n      never accepted a same-peer fresh pair, so the key-preservation\n      path was never entered and this run proves nothing either way.")
    k.ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
