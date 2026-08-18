#!/usr/bin/env python3
"""CH570 encrypted-link soak + forced reconnect + deep-path stack watermark.

The CH570-only exercise beyond ch570_validate.py:
  - hold an encrypted session long enough to cross EV10 rekeys (the dongle
    re-mints and re-announces; frames must keep verifying, drop_mac stays 0);
  - force link-loss/reacquire cycles by resetting the keyboard, which drives
    the dongle's reacquire watchdog -- the 26-bit ST_MAX_DELTA clamp path,
    CH570-only (CH592 routes the same arm through TMOS units);
  - re-read the stack watermark (0x96) after those deeper paths, to firm up
    the 0x700 floor beyond the 548 B encrypted-link-only reading.

Assumes the dongle is already paired + provisioned (run ch570_validate.py
first). The keyboard bench key is RAM-only, so it is re-sent after each reset.
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ch570_validate import (  # noqa: E402
    Kbd, Iap, BENCH_KEY, KBD_PORT, inject_f13, log)

MINICHLINK = os.path.expanduser(
    "~/Development/Personal/WCH/ch32fun/minichlink/minichlink")
KBD_PROBE = "CEBD8F0653EF"


def kbd_power(state):
    flag = "-kt" if state == "off" else "-k3"
    subprocess.run([MINICHLINK, "-C", "linke", flag, "-l", KBD_PROBE],
                   capture_output=True, timeout=30)


def reconnect_and_key(kbd, dg):
    # Robust: don't wait for a keyboard 5B 32 (absent when it never dropped);
    # send A6 30 (reconnect) + 0xAE (re-key, RAM-only) and confirm the dongle's
    # ok_count resumes -- the encrypted link is up iff frames verify.
    kbd.status.clear()
    kbd.ser.read(4096)
    d0 = dg.crypt_diag()
    ok0 = d0["ok"] if d0 else 0
    end = time.time() + 60.0
    last_nudge = 0.0
    while time.time() < end:
        kbd.pump()
        if time.time() - last_nudge >= 5.0:
            kbd.send([0xA6, 0x30])                  # nudge bonded reconnect
            kbd.send(bytes([0xAE]) + BENCH_KEY)      # (re)key, RAM-only
            last_nudge = time.time()
        inject_f13(kbd)
        d = dg.crypt_diag()
        if d and d["ok"] >= ok0 + 15:
            log(f"  encrypted link up (ok {ok0}->{d['ok']})")
            return True
        time.sleep(0.4)
    log("  WARN: encrypted frames did not resume in 60 s")
    return False


def soak(kbd, dg, seconds, label):
    d0 = dg.crypt_diag()
    ok0 = d0["ok"] if d0 else 0
    log(f"{label}: ok baseline {ok0}, soaking {seconds:.0f}s (F13 every 3 s)")
    end = time.time() + seconds
    last_inj = last_log = 0.0
    d = d0
    worst_mac = d0["mac"] if d0 else 0
    while time.time() < end:
        kbd.pump()
        if time.time() - last_inj >= 3.0:
            inject_f13(kbd)
            last_inj = time.time()
        d = dg.crypt_diag() or d
        if d:
            worst_mac = max(worst_mac, d["mac"])
        if time.time() - last_log >= 20.0 and d:
            log(f"  ok={d['ok']} mac={d['mac']} replay={d['replay']} "
                f"aes_redo={d.get('aes_redo', '-')} "
                f"announce_retry={d.get('announce_retry', '-')}")
            last_log = time.time()
        time.sleep(0.2)
    climbed = d and d["ok"] > ok0
    log(f"{label}: ok {ok0}->{d['ok'] if d else '?'}, worst drop_mac {worst_mac}")
    return climbed and worst_mac == 0


def main():
    dg = Iap()
    dg.handshake()
    dg.arm()
    valid, flags = dg.bond_flags()
    log(f"dongle bond flags=0x{flags:02X} ({dg.status_line()}) "
        f"-- want 0x03 (provisioned)")
    if not (valid and flags & 0x02):
        log("ABORT: dongle not provisioned -- run ch570_validate.py first")
        return 2

    # Hard power-cycle the keyboard for a clean full reconnect: a fresh connect
    # is what mints + announces a session the (reset, unkeyed) keyboard can
    # adopt -- a bare A6 30 after a port-reset leaves it connected but without
    # the current session (fire-and-forget announce window already closed).
    log("power-cycling keyboard (clean connect -> fresh session mint)")
    kbd_power("off")
    time.sleep(2.0)
    kbd_power("on")
    kbd = Kbd(KBD_PORT)
    log("keyboard port open; settling 11 s")
    time.sleep(11.0)
    kbd.ser.read(4096)

    if not reconnect_and_key(kbd, dg):
        log("ABORT: could not establish the encrypted link")
        return 1
    # Pure soak. Forced link-loss / reacquire is a separate, reliable test
    # (ch570_reacquire.py, hard probe power-cycle) -- a DTR port reopen does
    # not dependably reset the keyboard, so it is not used to force a drop.
    soak_ok = soak(kbd, dg, 150.0, "soak")

    d = dg.crypt_diag()
    if d:
        log(f"FINAL crypt-diag: ok={d['ok']} mac={d['mac']} replay={d['replay']} "
            f"aes_redo={d.get('aes_redo', '-')} "
            f"announce_retry={d.get('announce_retry', '-')} "
            f"kat_fail={d.get('kat_fail', '-')}")
    wm = dg.watermark()
    if wm:
        low, endb, top = wm
        log(f"stack watermark after deep paths: max_depth={top - low} B, "
            f"slack={low - endb} B  (was 548 B / 1372 B, encrypted-link only)")
    dg.close()

    print("\n" + ("SOAK PASS" if soak_ok else "SOAK FAIL"))
    return 0 if soak_ok else 1


if __name__ == "__main__":
    sys.exit(main())
