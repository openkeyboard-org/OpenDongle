#!/usr/bin/env python3
"""Exercise the USB robustness fixes against a plugged dongle (M2 batch).

Two regression checks that a conforming host never performs, which is why
the hardware campaign never hit the defects:

  pipelined   N EP6 OUT commands back-to-back WITHOUT reading a reply in
              between. Before the fix, the second OUT arrived while
              iap_pkt_pending was set, the guard refused it, and the
              unconditional NAK wedged EP6 until replug (review finding 8;
              the re-ACK now keeps the endpoint live). PASS = the interface
              still answers a handshake + status afterwards.

  reset       arm the IAP mutation session, then issue a USBDEVFS_RESET.
              Before the fix the armed session (and any latched command)
              survived the reset (finding 13; IAP_Reset now cancels it).
              PASS = after re-enumeration the session is DISARMED: a
              BondRead without a fresh arm must be rejected (timeout), and
              arming again must work.

Not covered here, still manual: CLEAR_FEATURE direction (needs raw control
transfers -- pyusb with the kernel driver detached) and suspend/resume
replay (needs host PM manipulation); see docs/reviews/2026-08-16-review.md
findings 24 and 9 for what to look for.

Usage: usb_robustness_test.py [--hidraw /dev/hidrawN] [pipelined|reset|all]
Root or hidraw+usbfs udev access required. The reset check re-enumerates the
device: expect the hidraw node to change.
"""

import argparse
import fcntl
import glob
import os
import struct
import sys
import time

VID, PID, IFACE = 0x0C45, 0xFEFE, 4
USBDEVFS_RESET = 0x5514  # _IO('U', 20)

CMD_HANDSHAKE, CMD_GETDEVINFO, CMD_STATUS, CMD_BOND_READ = 0x5A, 0x84, 0x91, 0x88


def find_hidraw():
    """The dongle's IAP interface node, matching provision_link_key.py."""
    hits = []
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            uevent = open(os.path.join(path, "device/uevent")).read()
        except OSError:
            continue
        if f"{VID:04X}:0000{PID:04X}".upper() not in uevent.upper():
            continue
        phys = [l for l in uevent.splitlines() if l.startswith("HID_PHYS")]
        if phys and phys[0].endswith(f"input{IFACE}"):
            hits.append("/dev/" + os.path.basename(path))
    if len(hits) != 1:
        raise SystemExit(
            f"need exactly one dongle IAP node, found {hits or 'none'} "
            "(pass --hidraw)")
    return hits[0]


def usbfs_node(hidraw):
    """The usbfs device node behind a hidraw, for USBDEVFS_RESET."""
    dev = os.path.realpath(f"/sys/class/hidraw/{os.path.basename(hidraw)}/device")
    # Walk up to the USB *device* (the directory with busnum/devnum).
    d = dev
    for _ in range(6):
        if os.path.exists(os.path.join(d, "busnum")):
            bus = int(open(os.path.join(d, "busnum")).read())
            devn = int(open(os.path.join(d, "devnum")).read())
            return f"/dev/bus/usb/{bus:03d}/{devn:03d}"
        d = os.path.dirname(d)
    raise SystemExit(f"no usbfs node above {dev}")


def packet(cmd, body=b""):
    body = bytes(body)
    pkt = bytearray(65)
    pkt[1] = cmd
    pkt[2] = len(body)
    pkt[3:3 + len(body)] = body
    pkt[3 + len(body)] = (cmd + len(body) + sum(body)) & 0xFF
    return bytes(pkt)


def txn(fd, cmd, body=b"", timeout=1.0):
    os.write(fd, packet(cmd, body))
    import select
    r, _, _ = select.select([fd], [], [], timeout)
    return os.read(fd, 64) if r else None


def drain(fd):
    import select
    while select.select([fd], [], [], 0.05)[0]:
        os.read(fd, 64)


def check_alive(fd, label):
    r = txn(fd, CMD_HANDSHAKE, b"WCH@HFD")
    if not r or r[0] != 0xA5:
        print(f"FAIL [{label}]: handshake got {r.hex() if r else 'timeout'}")
        return False
    r = txn(fd, CMD_STATUS)
    if not r or r[0] != 0x91:
        print(f"FAIL [{label}]: status got {r.hex() if r else 'timeout'}")
        return False
    print(f"  ok [{label}]: handshake + status answered")
    return True


def test_pipelined(hidraw):
    print("pipelined EP6 OUTs (wedge regression, finding 8):")
    fd = os.open(hidraw, os.O_RDWR)
    try:
        # Fire 8 commands with no reads in between. Some are dropped by the
        # one-slot flow control -- the CONTRACT is only that the endpoint
        # stays alive afterwards.
        for _ in range(8):
            os.write(fd, packet(CMD_STATUS))
        time.sleep(0.3)
        drain(fd)
        return check_alive(fd, "after 8 unread OUTs")
    finally:
        os.close(fd)


def test_reset(hidraw):
    print("bus reset cancels the IAP session (finding 13):")
    usbfs = usbfs_node(hidraw)
    fd = os.open(hidraw, os.O_RDWR)
    try:
        if not txn(fd, CMD_HANDSHAKE, b"WCH@HFD"):
            print("FAIL: no handshake before reset")
            return False
        # GetDevInfo(1) arms the mutation session.
        r = txn(fd, CMD_GETDEVINFO, bytes([1, 0, 0, 0]))
        if not r or r[0] != 0x04:
            print(f"FAIL: arm got {r.hex() if r else 'timeout'}")
            return False
        # BondRead needs an armed session -- prove it IS armed now.
        if not txn(fd, CMD_BOND_READ):
            print("FAIL: BondRead refused while armed (setup broken)")
            return False
    finally:
        os.close(fd)

    ufd = os.open(usbfs, os.O_WRONLY)
    try:
        fcntl.ioctl(ufd, USBDEVFS_RESET, 0)
    finally:
        os.close(ufd)
    print("  reset issued; waiting for re-enumeration...")
    time.sleep(2.0)

    node = find_hidraw()  # node may have changed
    fd = os.open(node, os.O_RDWR)
    try:
        # The reset must have DISARMED the session: BondRead without a fresh
        # arm is rejected by silent timeout (send_reject).
        r = txn(fd, CMD_BOND_READ)
        if r is not None:
            print(f"FAIL: BondRead answered {r.hex()} after reset -- "
                  "session survived (IAP_Reset did not run?)")
            return False
        print("  ok: session disarmed by the reset")
        txn(fd, CMD_HANDSHAKE, b"WCH@HFD")
        r = txn(fd, CMD_GETDEVINFO, bytes([1, 0, 0, 0]))
        if not r:
            print("FAIL: cannot re-arm after reset")
            return False
        return check_alive(fd, "after reset + re-arm")
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hidraw")
    ap.add_argument("which", nargs="?", default="all",
                    choices=["pipelined", "reset", "all"])
    args = ap.parse_args()
    node = args.hidraw or find_hidraw()
    print(f"device: {node}")

    ok = True
    if args.which in ("pipelined", "all"):
        ok &= test_pipelined(node)
    if args.which in ("reset", "all"):
        ok &= test_reset(args.hidraw or find_hidraw())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
