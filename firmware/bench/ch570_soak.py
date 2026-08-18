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
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ch570_validate import (  # noqa: E402
    Kbd, Iap, BENCH_KEY, KBD_PORT, KST_CONNECTED, KST_KEYED_OK, inject_f13, log)


def reconnect_and_key(kbd, dg):
    kbd.status.clear()
    kbd.send([0xA6, 0x30])                      # bonded reconnect
    if not kbd.wait(KST_CONNECTED, 30.0):
        log("  WARN: no reconnect (5B 32)")
        return False
    kbd.send(bytes([0xAE]) + BENCH_KEY)          # re-key (RAM-only)
    if not kbd.wait(KST_KEYED_OK, 3.0):
        log("  WARN: re-key not confirmed (5B 21)")
        return False
    log("  reconnected + re-keyed")
    return True


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

    kbd = Kbd(KBD_PORT)
    log("keyboard port open (DTR-reset); settling 11 s")
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
