#!/usr/bin/env python3
"""Provision the AES-128-CCM link key into a dongle's bond record, over USB IAP.

BENCH TOOL. Link encryption is negotiated at pairing but stays inert until a key
exists on both ends, and there is no on-air key establishment yet -- so until
that lands, the two ends are handed the same key out of band. This is the
receiver half; the keyboard half is OpenController's 0xAE UART command
(KBD_CRYPT_BENCH_KEY builds).

A shared out-of-band key is a bring-up scaffold, not a security design: anyone
who learns it can both read and forge traffic on that link. Use throwaway keys
on the bench and do not carry one into anything real.

The device never lets the key back out -- BondRead redacts it and marks the
response -- so this reads the record, sets the key and its flags, recomputes the
checksum, and writes the whole record back.

Usage:
    ./provision_link_key.py --key 000102...0f          # 32 hex chars
    ./provision_link_key.py --random                   # generate and print one
    ./provision_link_key.py --show                     # report state, write nothing
    ./provision_link_key.py --clear-key                # drop the key, keep the bond

Requires read/write access to the dongle's IAP hidraw node (interface 4);
that usually means the `plugdev` group or running under sudo.
"""

from __future__ import annotations

import argparse
import glob
import os
import secrets
import select
import struct
import subprocess
import sys
import time

VID, PID, IAP_INTERFACE = 0x0C45, 0xFEFE, 4
REPORT_SIZE = 65          # 1 report-ID byte + 64-byte payload

CMD_HANDSHAKE = 0x5A
CMD_GETDEVINFO = 0x84
CMD_BOND_WRITE = 0x87
CMD_BOND_READ = 0x88
ACK_HANDSHAKE = 0xA5
ACK_GETDEVINFO = 0x04
ACK_BOND_READ = 0x88
HANDSHAKE_PAYLOAD = b"WCH@HFD"

BOND_MAGIC = 0x444E4F42    # 'BOND'
BOND_VERSION = 2
BOND_SIZE = 48
FLAG_ENC_CAPABLE = 0x01
FLAG_ENC_KEY = 0x02
KEY_BYTES = 16

# Byte offsets inside bond_record_t (firmware/common/include/bond.h).
OFF_MAGIC, OFF_VERSION, OFF_FLAGS = 0, 4, 5
OFF_SESSION_AA, OFF_PEER_MAC = 8, 22
OFF_LINK_KEY, OFF_CHECKSUM = 28, 44

# Status bytes the firmware answers BondWrite with.
WRITE_STATUS = {
    0x00: "saved, verified, applied live -- active NOW, no reset needed",
    0x01: "NV erase failed",
    0x02: "NV write failed",
    0xB0: "wrong record length",
    0xB1: "structurally invalid (magic/version/checksum/session AA)",
    0xB2: "semantically rejected (bounds, MAC hygiene, peer == dongle)",
    0xB3: "key/flag combination refused (zero or 0xFF key, or flag mismatch)",
    0xB4: "written but read-back verify FAILED -- running state unchanged",
    0xB5: "saved+verified; applies when the live encrypted link drops",
}


def find_hidraw() -> str:
    """The dongle's IAP interface node, via udev properties.

    Refuses to guess when more than one dongle is attached. A bench commonly has
    both a CH570 and a CH592 present, and they are indistinguishable by VID:PID
    -- picking one silently would key the wrong receiver and produce a link
    failure that looks like a crypto bug.
    """
    found = []
    for node in sorted(glob.glob("/dev/hidraw*")):
        try:
            props = subprocess.run(
                ["udevadm", "info", "-q", "property", node],
                capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        low = props.lower()
        if f"{VID:04x}" not in low or f"{PID:04x}" not in low:
            continue
        path = ""
        iface = None
        for line in props.splitlines():
            if line.startswith("ID_USB_INTERFACE_NUM="):
                try:
                    iface = int(line.split("=", 1)[1])
                except ValueError:
                    iface = None
            elif line.startswith("ID_PATH="):
                path = line.split("=", 1)[1]
        if iface == IAP_INTERFACE:
            found.append((node, path))

    if not found:
        raise SystemExit(
            f"no OpenDongle IAP interface found ({VID:04x}:{PID:04x} if={IAP_INTERFACE}).\n"
            "Is the dongle plugged in, and do you have access to its hidraw node?")
    if len(found) > 1:
        listing = "\n".join(f"  --hidraw {n}   ({p})" for n, p in found)
        raise SystemExit(
            "more than one dongle is attached; name the one you mean:\n" + listing +
            "\n\n(`opendongle --hidraw <node> --info` reports which chip each is.)")
    return found[0][0]


class Iap:
    """`[cmd][len][body][checksum]` in a 65-byte report; replies are 64 bytes
    with the ACK at index 0 (the kernel strips the report ID)."""

    def __init__(self, path: str):
        # Non-blocking: a device that answers nothing (wrong interface, or
        # firmware without IAP) must time out rather than hang the bench.
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

    def close(self):
        os.close(self.fd)

    def xfer(self, cmd: int, body: bytes = b"", timeout: float = 1.0):
        if len(body) > REPORT_SIZE - 4:
            raise ValueError("body too large for one report")
        pkt = bytearray(REPORT_SIZE)
        pkt[1] = cmd
        pkt[2] = len(body)
        pkt[3:3 + len(body)] = body
        pkt[3 + len(body)] = (cmd + len(body) + sum(body)) & 0xFF
        os.write(self.fd, bytes(pkt))
        time.sleep(0.001)

        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            if not select.select([self.fd], [], [], remaining)[0]:
                return None
            try:
                data = os.read(self.fd, 64)
            except BlockingIOError:
                continue
            if data:
                return bytes(data)

    def handshake(self):
        r = self.xfer(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD)
        if not r or r[0] != ACK_HANDSHAKE:
            raise SystemExit("handshake failed — is this an OpenDongle IAP interface?")

    def arm(self):
        """GetDevInfo(1) both identifies the device and arms the session;
        BondWrite is refused otherwise. The argument is a 4-byte LE word --
        the firmware rejects any other body length outright."""
        r = self.xfer(CMD_GETDEVINFO, struct.pack("<I", 1))
        if not r or r[0] != ACK_GETDEVINFO:
            raise SystemExit("GetDevInfo failed — session not armed, refusing to write")
        return r

    def bond_read(self):
        r = self.xfer(CMD_BOND_READ)
        if not r or r[0] != ACK_BOND_READ:
            raise SystemExit("BondRead failed")
        length, status = r[1], r[2]
        if length != BOND_SIZE:
            if length == 32:
                raise SystemExit(
                    "this dongle reports a 32-byte (v1) bond record, so its firmware\n"
                    "predates link encryption -- there is no key field to write.\n"
                    "Flash a build from a branch with DONGLE_RF_CRYPT enabled first;\n"
                    "see firmware/bench/README-link-encryption.md.")
            raise SystemExit(f"unexpected bond record size {length} (expected {BOND_SIZE})")
        return bytearray(r[3:3 + BOND_SIZE]), status

    def bond_write(self, rec: bytes):
        r = self.xfer(CMD_BOND_WRITE, bytes(rec))
        if not r:
            raise SystemExit("BondWrite got no reply")
        status = r[1] if r[0] == 0x0F else r[-1]
        for idx in (1, 2, 3):
            if idx < len(r) and r[idx] in WRITE_STATUS:
                status = r[idx]
                break
        return status


def checksum(rec: bytes) -> int:
    return sum(rec[:OFF_CHECKSUM]) & 0xFFFFFFFF


def describe(rec: bytes, status: int) -> None:
    magic, = struct.unpack_from("<I", rec, OFF_MAGIC)
    version = rec[OFF_VERSION]
    flags = rec[OFF_FLAGS]
    aa, = struct.unpack_from("<I", rec, OFF_SESSION_AA)
    peer = ":".join(f"{b:02X}" for b in rec[OFF_PEER_MAC:OFF_PEER_MAC + 6])
    stored, = struct.unpack_from("<I", rec, OFF_CHECKSUM)

    print(f"  bond present   {'no' if status & 0x01 else 'yes'}")
    print(f"  magic/version  {magic:08X} / v{version}"
          f"{'  (unexpected)' if magic != BOND_MAGIC else ''}")
    print(f"  session AA     0x{aa:08X}")
    print(f"  keyboard MAC   {peer}")
    print(f"  flags          0x{flags:02X}"
          f"  capable={'yes' if flags & FLAG_ENC_CAPABLE else 'no'}"
          f"  key={'yes' if flags & FLAG_ENC_KEY else 'no'}")
    active = (flags & (FLAG_ENC_CAPABLE | FLAG_ENC_KEY)) == (FLAG_ENC_CAPABLE | FLAG_ENC_KEY)
    print(f"  encryption     {'ACTIVE' if active else 'off'}")
    if status & 0x02:
        print("  (key redacted in this view — the device never reveals it)")
    if checksum(rec) != stored:
        print("  WARNING: checksum mismatch in the record as read")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--key", help="16-byte key as 32 hex characters")
    g.add_argument("--random", action="store_true", help="generate a key and print it")
    g.add_argument("--show", action="store_true", help="report bond state, write nothing")
    g.add_argument("--clear-key", action="store_true",
                   help="drop the key and its flag, keeping the bond")
    ap.add_argument("--hidraw", help="explicit hidraw node (skip discovery)")
    args = ap.parse_args()

    if args.key:
        try:
            key = bytes.fromhex(args.key)
        except ValueError:
            raise SystemExit("--key must be hex")
        if len(key) != KEY_BYTES:
            raise SystemExit(f"--key must be {KEY_BYTES} bytes ({KEY_BYTES * 2} hex chars)")
        if key == bytes(KEY_BYTES) or key == b"\xff" * KEY_BYTES:
            raise SystemExit("all-zero and all-0xFF keys are erased-flash patterns; refused")
    elif args.random:
        key = secrets.token_bytes(KEY_BYTES)
    else:
        key = None

    dev = Iap(args.hidraw or find_hidraw())
    try:
        dev.handshake()
        dev.arm()
        rec, status = dev.bond_read()

        print("before:")
        describe(rec, status)

        if args.show:
            return 0
        if status & 0x01:
            raise SystemExit(
                "\nno valid bond on the dongle — pair the keyboard first.\n"
                "A key has nothing to attach to until a bond exists.")

        if args.clear_key:
            rec[OFF_FLAGS] &= ~FLAG_ENC_KEY & 0xFF
            rec[OFF_LINK_KEY:OFF_LINK_KEY + KEY_BYTES] = bytes(KEY_BYTES)
        else:
            # ENC_CAPABLE is the keyboard's claim, negotiated on air at
            # pairing and persisted by the dongle. Forcing it here used to
            # paper over the accept-path bug that erased the negotiation
            # (2026-08-16 review, finding 3) -- and a forced flag on a bond
            # whose keyboard never advertised produces exactly the failure
            # this tool exists to avoid: encryption required, peer unable.
            if not rec[OFF_FLAGS] & FLAG_ENC_CAPABLE:
                raise SystemExit(
                    "\nthis bond is not marked encryption-capable, so the "
                    "keyboard never advertised\nencryption at pairing. "
                    "Re-pair with an encryption-capable keyboard firmware\n"
                    "and run this again -- forcing the flag would only "
                    "produce a dead link.")
            rec[OFF_FLAGS] |= FLAG_ENC_KEY
            rec[OFF_LINK_KEY:OFF_LINK_KEY + KEY_BYTES] = key
        struct.pack_into("<I", rec, OFF_CHECKSUM, checksum(rec))

        st = dev.bond_write(rec)
        print(f"\nBondWrite -> 0x{st:02X} ({WRITE_STATUS.get(st, 'unknown status')})")
        if st not in (0x00, 0xB5):
            return 1

        rec2, status2 = dev.bond_read()
        print("\nafter:")
        describe(rec2, status2)

        if key is not None:
            print(f"\nkey: {key.hex()}")
            print("Give the SAME key to the keyboard, or the link authenticates nothing:")
            print(f"  A6-style bench frame 0xAE — see OpenController firmware/bench")
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
