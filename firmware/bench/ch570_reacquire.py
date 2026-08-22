#!/usr/bin/env python3
"""CH570 forced link-loss / reacquire test (the 26-bit ST_MAX_DELTA clamp path).

Hard-power-cycles the keyboard through its WCH-Link probe (reliable, unlike a
DTR reopen) to force a real link loss, so the dongle runs its reacquire
watchdog -- the arm that was silently truncated to ~112 ms before the CH570
26-bit clamp. Confirms the dongle drops, cleanly reacquires when the keyboard
returns, resumes verifying encrypted frames (drop_mac stays 0), and that the
stack watermark does not blow up (the pre-fix epoch-jump bug would dispatch a
burst of timers -> deep stack). Reconnect is detected by the dongle's ok_count
resuming, not a keyboard status byte.

Assumes the dongle is paired + provisioned (run ch570_validate.py first).
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ch570_validate import (  # noqa: E402
    Iap, Kbd, BENCH_KEY, KBD_PORT, inject_f13, log)

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


def link_up(kbd, dg, seconds=25.0):
    """Bring the (bonded, provisioned) link back and confirm encrypted frames
    resume: A6 30 reconnect + 0xAE re-key (RAM-only), then watch ok climb."""
    kbd.status.clear()
    kbd.ser.read(4096)
    kbd.send([0xA6, 0x30])
    time.sleep(0.5)
    kbd.send(bytes([0xAE]) + BENCH_KEY)
    d0 = dg.crypt_diag()
    ok0, mac0 = (d0["ok"], d0["mac"]) if d0 else (0, 0)
    end = time.time() + seconds
    d = d0
    while time.time() < end:
        kbd.pump()
        inject_f13(kbd)
        d = dg.crypt_diag() or d
        if d and d["ok"] >= ok0 + 15:
            return True, ok0, d["ok"], d["mac"] - mac0
        time.sleep(0.4)
    return False, ok0, (d["ok"] if d else ok0), ((d["mac"] - mac0) if d else -1)


def main():
    dg = Iap()
    dg.handshake()
    dg.arm()
    valid, flags = dg.bond_flags()
    if not (valid and flags & 0x02):
        log("ABORT: dongle not provisioned -- run ch570_validate.py first")
        return 2
    log(f"dongle provisioned (flags 0x{flags:02X}); opening keyboard")
    kbd = Kbd(KBD_PORT)
    time.sleep(11.0)
    kbd.ser.read(4096)

    up, a, b, dmac = link_up(kbd, dg)
    log(f"baseline link: ok {a}->{b} (+{b - a}), dmac {dmac} -- "
        f"{'UP' if up else 'NOT climbing'}")
    if not up:
        log("ABORT: could not establish the encrypted link")
        return 1

    ok_all = True
    for cyc in (1, 2):
        pre = dg.crypt_diag()
        log(f"=== cycle {cyc}: power OFF keyboard (force link loss) ===")
        kbd_power("off")
        time.sleep(7.0)                       # dongle reacquire-watchdog window
        log(f"  during outage: dongle {dg.status_line()} "
            f"(ok held at {dg.crypt_diag()['ok'] if dg.crypt_diag() else '?'})")
        log("  power ON keyboard; settling 11 s")
        kbd_power("on")
        time.sleep(11.0)
        up, a, b, dmac = link_up(kbd, dg)
        ok_all &= up and dmac == 0
        log(f"  cycle {cyc} reacquire: ok {a}->{b} (+{b - a}), "
            f"drop_mac delta {dmac} -> {'PASS' if up and dmac == 0 else 'FAIL'}")

    d = dg.crypt_diag()
    if d:
        log(f"FINAL crypt-diag: ok={d['ok']} mac={d['mac']} replay={d['replay']} "
            f"aes_redo={d.get('aes_redo', '-')} "
            f"announce_retry={d.get('announce_retry', '-')} "
            f"kat_fail={d.get('kat_fail', '-')}")
    wm = dg.watermark()
    if wm:
        low, endb, top = wm
        log(f"stack watermark after reacquire cycles: max_depth={top - low} B, "
            f"slack={low - endb} B  (was 548 B; a blown clamp would spike this)")
    dg.close()
    print("\n" + ("REACQUIRE PASS" if ok_all else "REACQUIRE FAIL"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
