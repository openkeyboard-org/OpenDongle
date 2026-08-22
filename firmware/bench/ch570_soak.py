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
    Kbd, Iap, BENCH_KEY, KBD_PORT, WANT_FAMILY, WANT_PROFILE, inject_f13, log)

MINICHLINK = os.path.expanduser(
    "~/Development/Personal/WCH/ch32fun/minichlink/minichlink")
KBD_PROBE = "CEBD8F0653EF"


def kbd_power(state):
    """Drive the keyboard's rail, and FAIL LOUDLY if minichlink refuses.

    The returncode used to be discarded, so a power cycle that never happened
    (probe busy, wrong serial, binary present but failing) left the link up and
    the test then passed vacuously -- the worst shape of failure for an
    acceptance test, and precisely what the vendored ab_bench harness warns
    about. NOTE: minichlink cannot connect to CH5xx parts at all on this bench
    (it pre-selects CHIP_CH32V10x before the LinkE connect); -kt/-k3 skip target
    init so they still work, but see bench/README-link-encryption.md.
    """
    flag = "-kt" if state == "off" else "-k3"
    p = subprocess.run([MINICHLINK, "-C", "linke", flag, "-l", KBD_PROBE],
                       capture_output=True, timeout=30)
    if p.returncode != 0:
        raise SystemExit(
            f"keyboard power {state} FAILED (minichlink rc={p.returncode}): "
            f"{p.stderr.decode(errors='replace').strip()[:200]}")


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
    # Liveness is checked per WINDOW, not once at the end: `ok > ok0` over the
    # whole soak is satisfied by a link that verified for five seconds and then
    # died, and worst_mac then stays 0 precisely BECAUSE nothing is arriving.
    # Track the tail separately so a mid-window death fails.
    tail_window = 20.0
    tail_ok = ok0
    tail_mark = time.time()
    stalls = 0
    while time.time() < end:
        kbd.pump()
        if time.time() - last_inj >= 3.0:
            inject_f13(kbd)
            last_inj = time.time()
        fresh = dg.crypt_diag()
        d = fresh or d
        if d:
            worst_mac = max(worst_mac, d["mac"])
        if time.time() - tail_mark >= tail_window:
            # A window in which nothing verified is a stall, even if the
            # cumulative counter climbed earlier in the run. Judge on the LAST
            # good sample, not on `fresh`: a single 1 s IAP read timeout is not
            # evidence the link died (CodeRabbit review).
            latest = d["ok"] if d else tail_ok
            if latest <= tail_ok:
                stalls += 1
                log(f"  STALL: no verified frames in the last {tail_window:.0f}s "
                    f"(ok stuck at {tail_ok})")
            tail_ok = latest
            tail_mark = time.time()
        if time.time() - last_log >= 20.0 and d:
            log(f"  ok={d['ok']} mac={d['mac']} replay={d['replay']} "
                f"aes_redo={d.get('aes_redo', '-')} "
                f"announce_retry={d.get('announce_retry', '-')}")
            last_log = time.time()
        time.sleep(0.2)
    final = dg.crypt_diag() or d
    climbed = bool(final and final["ok"] > ok0)
    # Pre-verify drops are silent in the cumulative counters, so require them
    # flat too: shape/inactive/engine moving means frames arrived and were
    # rejected before the MAC check ever ran.
    # Pre-verify drops are invisible in the cumulative counters. enc_shape only
    # exists on the bench layout, but plain_drop is now exported on product too,
    # so at least one of these checks runs on every profile.
    shape_clean = True
    for field in ("enc_shape", "plain_drop"):
        if final and d0 and field in final and field in d0:
            if final[field] != d0[field]:
                shape_clean = False
                log(f"{label}: {field} moved {d0[field]} -> {final[field]}")
    log(f"{label}: ok {ok0}->{final['ok'] if final else '?'}, "
        f"worst drop_mac {worst_mac}, stalled windows {stalls}")
    if stalls:
        log(f"{label}: FAIL -- {stalls} window(s) verified nothing")
    if not shape_clean:
        log(f"{label}: FAIL -- pre-verify drops occurred")
    climbed = climbed and stalls == 0 and shape_clean
    return climbed and worst_mac == 0


def main():
    dg = Iap()
    dg.handshake()
    dg.arm()
    dg.require(WANT_FAMILY, WANT_PROFILE)
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
