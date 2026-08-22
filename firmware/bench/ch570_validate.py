#!/usr/bin/env python3
"""Validate the encrypted link on a CH570 receiver via the PRODUCTION path.

Unlike bench_run.py (a CH592 receiver driven over UART telemetry with the
compiled-in force key), the CH570 dongle has no UART and no force key: it is
driven over USB IAP and encryption comes up the real way --

  1. pair, capability advert negotiated ON AIR (P0 #3 fix) -> the dongle
     records BOND_FLAG_ENC_CAPABLE;
  2. provision the link key over USB IAP: it VERIFIES the write and ACTIVATES
     the running crypto state with NO dongle reset (P0 #2 fix);
  3. the keyboard, keyed over UART, adopts the session the dongle mints on
     activation and starts sealing;
  4. the dongle verifies the sealed frames -> ok_count climbs, and injected
     F13 presses arrive at the host through the dongle's own USB HID node --
     nothing reaches that node without a verified CCM tag.

Gates (each prints PASS/FAIL):
  G1  ENC_CAPABLE latched in the dongle bond after an on-air pair
  G2  ok_count climbs (drop_mac == 0) after USB provisioning, no reset, and
      injected F13 presses are delivered host-side

Keyboard: OpenController CH592, KBD_RF_CRYPT=1 KBD_CRYPT_BENCH_KEY=1, UART on
the probe CDC. Receiver: CH570 on USB (0C45:FEFE, IAP if=4, boot kbd if=0).
"""

import glob
import os
import select
import struct
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
KBD_PORT = "/dev/serial/by-id/usb-wch.cn_WCH-Link_CEBD8F0653EF-if01"
BENCH_KEY = bytes.fromhex("4f70656e4b626421a55ac33c69960ff0")
PROVISION = os.path.join(HERE, "provision_link_key.py")

VID, PID = 0x0C45, 0xFEFE

# Despite the ch570_* name this validator is chip-generic (it finds the dongle by
# USB identity, not family), so the family is a parameter and defaults to the
# CH592 currently on the bench.
# Which dongle this run is allowed to drive, and therefore which 0x94 layout to
# expect. family: 0x70 = CH570, 0x92 = CH592 (DONGLE_CHIP_FAMILY_ID).
# profile: 1 = product, 2 = bench (DONGLE_BUILD_PROFILE). Override per run.
WANT_FAMILY = int(os.environ.get("DONGLE_FAMILY", "0x92"), 0)
WANT_PROFILE = int(os.environ.get("DONGLE_PROFILE", "1"), 0)
OPENDONGLE_CLI = os.environ.get(
    "OPENDONGLE_CLI",
    os.path.join(HERE, "..", "..", "tools", "target", "release", "opendongle"))
OFF_FLAGS, OFF_LINK_KEY = 5, 28
FLAG_ENC_CAPABLE, FLAG_ENC_KEY = 0x01, 0x02
BOND_SIZE = 48

KST_CONNECTED, KST_HAS_BOND, KST_KEYED_OK, KST_KEY_REFUSED = 0x32, 0x35, 0x21, 0x36
F13 = 0x68

t0 = time.time()


def log(m):
    print(f"[{time.time() - t0:7.2f}s] {m}", flush=True)


# ----------------------------------------------------------------- keyboard

def kframe(body):
    b = bytes(body)
    return b + bytes([sum(b) & 0xFF])


class Kbd:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.buf = b""
        self.status = []

    def pump(self):
        self.buf += self.ser.read(256)
        changed = True
        while changed:
            changed = False
            i = self.buf.find(b"\x5b")
            if i >= 0 and len(self.buf) - i >= 3:
                pkt = self.buf[i:i + 3]
                if (sum(pkt[:-1]) & 0xFF) == pkt[-1]:
                    self.status.append((time.time() - t0, pkt[1]))
                self.buf = self.buf[:i] + self.buf[i + 3:]
                changed = True
        if len(self.buf) > 256:
            self.buf = self.buf[-256:]

    def send(self, body):
        self.ser.write(kframe(body))

    def saw(self, code, since=0.0):
        return any(c == code and t >= since for t, c in self.status)

    def wait(self, code, timeout, since=0.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump()
            if self.saw(code, since):
                return True
            time.sleep(0.02)
        return False


# --------------------------------------------------------------- dongle IAP

def find_hidraw(iface):
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            uevent = open(os.path.join(path, "device/uevent")).read()
        except OSError:
            continue
        if f"{VID:04X}:0000{PID:04X}".upper() not in uevent.upper():
            continue
        phys = [l for l in uevent.splitlines() if l.startswith("HID_PHYS")]
        if phys and phys[0].endswith(f"input{iface}"):
            return "/dev/" + os.path.basename(path)
    return None


class Iap:
    def __init__(self):
        node = find_hidraw(4)
        if not node:
            raise SystemExit("no CH570 dongle IAP interface (0C45:FEFE if=4)")
        self.node = node
        self.fd = os.open(node, os.O_RDWR)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def txn(self, cmd, body=b"", timeout=1.0):
        body = bytes(body)
        pkt = bytearray(65)
        pkt[1] = cmd
        pkt[2] = len(body)
        pkt[3:3 + len(body)] = body
        pkt[3 + len(body)] = (cmd + len(body) + sum(body)) & 0xFF
        os.write(self.fd, bytes(pkt))
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 64) if r else None

    def handshake(self):
        return self.txn(0x5A, b"WCH@HFD")

    def arm(self):
        return self.txn(0x84, struct.pack("<I", 1))

    def bond_flags(self):
        """(valid, flags). BondRead reply: [0x88][len==48][status][record48]."""
        r = self.txn(0x88)
        if not r or r[0] != 0x88 or r[1] != BOND_SIZE:
            return (False, 0)
        status = r[2]
        rec = r[3:3 + BOND_SIZE]
        if status & 0x01:                     # bit0 = no valid bond
            return (False, 0)
        return (True, rec[OFF_FLAGS])

    def crypt_diag(self):
        """Parse 0x94. Dispatch on the EXACT payload length, never a threshold.

        iap.c serves two layouts: 38 bytes (product) and 62 (bench, which is the
        whole 64-byte EP6 report). A `n >= 38` test matched BOTH, so a bench
        reply was parsed as product and silently mislabelled conn_rx/enc_shape as
        aes_redo/announce_retry and two middle bytes of fifo_full as the boot-KAT
        result -- fabricating an AES self-test failure from a nonzero counter.
        """
        r = self.txn(0x94)
        if not r or r[0] != 0x94:
            return None
        n = r[1]
        if n < 30:
            return None                       # too short for even the common head
        reasons = list(struct.unpack_from("<6I", r, 6))
        d = {"ok": struct.unpack_from("<I", r, 2)[0],
             "mac": reasons[3], "replay": reasons[4], "layout": n}
        if n in (38, 42):                     # product (42 since plain_drop)
            d["aes_redo"], d["announce_retry"] = struct.unpack_from("<2I", r, 30)
            d["kat_run"], d["kat_fail"] = r[38], r[39]
            if n == 42:
                d["plain_drop"] = struct.unpack_from("<I", r, 40)[0]
        elif n == 62:                         # bench v4
            (d["conn_rx"], d["enc_shape"], d["fifo_full"],
             d["flush_drop"], d["plain_drop"]) = struct.unpack_from("<5I", r, 30)
        return d

    def identity(self):
        """(family, profile) from 0x91. Wire index = 2 + payload offset."""
        r = self.txn(0x91)
        if not r or r[0] != 0x91 or r[1] < 32:
            return None
        return (r[3], r[8])

    def require(self, family, profile):
        """Refuse to run against the wrong dongle.

        Two dongles on one bench are indistinguishable by VID:PID, and
        find_hidraw() binds the first match, so without this every gate could be
        attributed to the wrong device -- and the 0x94 layout depends on the
        profile.
        """
        ident = self.identity()
        if ident is None:
            raise SystemExit("dongle did not answer 0x91 status")
        got_family, got_profile = ident
        if got_family != family or got_profile != profile:
            raise SystemExit(
                f"wrong dongle on {self.node}: family 0x{got_family:02X} "
                f"profile {got_profile} (want family 0x{family:02X} "
                f"profile {profile})")

    def watermark(self):
        r = self.txn(0x96)
        if not r or r[0] != 0x96 or r[1] < 12:
            return None
        return struct.unpack_from("<3I", r, 2)   # (low, end, top)

    def status_line(self):
        r = self.txn(0x91)
        if not r or r[0] != 0x91:
            return "unknown"
        conn = {0: "unavailable", 1: "pairing", 2: "waiting-reconnect",
                3: "connected"}.get(r[4], "?")
        return f"conn={conn}"


# --------------------------------------------------------------------- main

def provision_dongle():
    """Delegate the read-modify-write to the shipping tool (correct checksum,
    ENC_CAPABLE gate, live-activation status). Returns True on 0x00/0xB5."""
    p = subprocess.run(
        ["/usr/bin/python3", "-I", PROVISION, "--key", BENCH_KEY.hex()],
        capture_output=True, text=True, timeout=30)
    sys.stdout.write(p.stdout)
    if p.stderr.strip():
        sys.stderr.write(p.stderr)
    return p.returncode == 0


def inject_f13(kbd):
    kbd.send(bytes([0xA1, 0, 0, F13, 0, 0, 0, 0, 0]))   # F13 down (inert)
    time.sleep(0.05)
    kbd.send(bytes([0xA1] + [0] * 8))                    # all up


def _reopen_iap(timeout=40.0):
    """Re-open IAP after a dongle reset (the hidraw node changes).

    Retry only while the device is ABSENT; the identity assertion runs once,
    afterwards, so a wrong dongle fails immediately instead of being retried
    into the timeout.
    """
    deadline = time.time() + timeout
    dev = None
    while time.time() < deadline:
        try:
            cand = Iap()                     # raises SystemExit while absent
            if cand.handshake():
                dev = cand
                break
            cand.close()
        except KeyboardInterrupt:
            raise
        except BaseException:
            pass                          # absent/settling: keep waiting
        time.sleep(1.0)
    if dev is None:
        raise SystemExit("dongle did not come back after reset")
    dev.arm()
    dev.require(WANT_FAMILY, WANT_PROFILE)
    return dev


def main():
    hold = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    fails = []

    dg = Iap()
    if not dg.handshake():
        raise SystemExit("dongle IAP handshake failed")
    dg.arm()
    dg.require(WANT_FAMILY, WANT_PROFILE)
    log(f"dongle IAP up on {dg.node} ({dg.status_line()}) "
        f"family 0x{WANT_FAMILY:02X} profile {WANT_PROFILE}")

    # G1 asks whether the capability advert survives THIS pair's beacon accept
    # (P0 #3). BondRead returns the NV record, so a dongle left provisioned by an
    # earlier run answers flags 0x03 the moment the keyboard reports CONNECTED --
    # satisfying G1 without the advert having landed at all. Clear the bond first
    # so the record read below can only have come from this pair. The clear
    # tombstones pair-accept until the next MCU reset, so reset before pairing.
    r = dg.txn(0x89)
    if not r or r[0] != 0x0F or r[2] != 0x00:
        raise SystemExit(f"dongle BondClear failed: {r.hex() if r else 'timeout'}")
    log("dongle bond cleared (0x89); resetting to lift the pair tombstone")
    dg.close()
    rst = subprocess.run([OPENDONGLE_CLI, "--enter-bootloader", "--force"],
                         capture_output=True, timeout=30)
    if rst.returncode != 0:
        raise SystemExit(
            f"dongle reset failed (rc={rst.returncode}); the pair tombstone is "
            f"still up and no pair can complete: "
            f"{rst.stderr.decode(errors='replace').strip()[:200]}")
    dg = _reopen_iap()
    log(f"dongle back up ({dg.status_line()}), bond {dg.bond_flags()}")

    kbd = Kbd(KBD_PORT)
    log("keyboard port open (DTR-reset); settling 11 s through OpenBoot window")
    time.sleep(11.0)
    kbd.ser.read(4096)

    # Clear the stale keyboard bond (survived the SWD factory flash), then pair.
    kbd.send([0xA6, 0x30]); time.sleep(0.6); kbd.pump()
    kbd.send([0xA6, 0x52]); time.sleep(1.0); kbd.pump()
    log("keyboard unpaired (A6 52)")
    kbd.send([0xA6, 0x30]); time.sleep(0.6); kbd.pump()
    kbd.send([0xA6, 0x51])
    log("A6 51: broadcasting for pair; waiting for CONNECTED + dongle bond")

    deadline = time.time() + 60.0
    dg_valid, dg_flags = False, 0
    while time.time() < deadline:
        kbd.pump()
        if kbd.saw(KST_CONNECTED):
            dg_valid, dg_flags = dg.bond_flags()
            if dg_valid:
                break
        time.sleep(0.1)
    if not kbd.saw(KST_CONNECTED):
        print("ABORT: keyboard never reported CONNECTED (5B 32) within 60 s")
        return 1
    if not dg_valid:
        print("ABORT: CONNECTED but dongle shows no valid bond within 60 s")
        return 1
    log(f"paired. dongle bond flags = 0x{dg_flags:02X} ({dg.status_line()})")

    # ---- G1: capability negotiated on air (P0 #3) ----
    # Staleness guard: a genuine fresh pair can never persist ENC_KEY
    # (rf_commit_bond_ram carries capability only), so a key here means the
    # record predates this run and the bond clear above did not take.
    if dg_flags & FLAG_ENC_KEY:
        print(f"\n=== G1 capability on air: FAIL "
              f"(stale record: flags 0x{dg_flags:02X} carries ENC_KEY, which a "
              f"fresh pair cannot persist) ===\n")
        return 1
    cap = bool(dg_flags & FLAG_ENC_CAPABLE)
    print(f"\n=== G1 capability on air: {'PASS' if cap else 'FAIL'} "
          f"(ENC_CAPABLE {'set' if cap else 'NOT set'}) ===\n")
    if not cap:
        print("advert lost -> bond not encryption-capable; provisioning would "
              "refuse. Stopping.")
        return 1

    # Key the keyboard (holds the key; seals once it adopts a session).
    since = time.time() - t0
    kbd.send(bytes([0xAE]) + BENCH_KEY)
    if not kbd.wait(KST_KEYED_OK, 3.0, since=since):
        print("ABORT: keyboard did not confirm the key (no 5B 21; refused=%s)"
              % kbd.saw(KST_KEY_REFUSED, since=since))
        return 1
    log("keyboard keyed (5B 21)")

    # ---- Provision the dongle over USB IAP: verify + live-activate (P0 #2) ----
    base = dg.crypt_diag()
    base_ok = base["ok"] if base else 0
    dg.close()                                  # free the hidraw for the tool
    log("provisioning dongle over USB IAP (verify + live-activate, no reset)...")
    ok_prov = provision_dongle()
    dg = Iap(); dg.handshake(); dg.arm()        # reopen
    valid, flags = dg.bond_flags()
    keyed_bond = valid and (flags & FLAG_ENC_KEY) and (flags & FLAG_ENC_CAPABLE)
    if not ok_prov or not keyed_bond:
        print(f"ABORT: provisioning not confirmed (tool_ok={ok_prov}, "
              f"bond flags=0x{flags:02X}, want ENC_CAPABLE|ENC_KEY)")
        return 1
    log(f"dongle provisioned + live-activated (bond flags 0x{flags:02X}, no reset). "
        f"ok_count baseline {base_ok}; injecting F13, watching {hold:.0f}s")

    # ---- G2: ok_count climbs (mac==0) + host-side F13 delivery ----
    knode = find_hidraw(0)                       # dongle boot-keyboard node
    hid_fd = os.open(knode, os.O_RDONLY | os.O_NONBLOCK) if knode else None
    delivered, down = 0, False
    end = time.time() + hold
    last_inject = 0.0
    dg2 = base
    while time.time() < end:
        kbd.pump()
        if time.time() - last_inject >= 2.0:
            inject_f13(kbd)
            last_inject = time.time()
        if hid_fd is not None:
            try:
                while True:
                    rep = os.read(hid_fd, 8)
                    if not rep:
                        break
                    d = F13 in rep[2:]
                    if d and not down:
                        delivered += 1
                    down = d
            except BlockingIOError:
                pass
        dg2 = dg.crypt_diag() or dg2
        if dg2 and dg2["ok"] >= base_ok + 20 and delivered >= 3:
            break
        time.sleep(0.2)
    if hid_fd is not None:
        os.close(hid_fd)

    ok = dg2["ok"] if dg2 else 0
    mac = dg2["mac"] if dg2 else -1
    g2 = ok > base_ok and mac == 0 and delivered > 0
    print(f"\n=== G2 live activation: {'PASS' if g2 else 'FAIL'} "
          f"(ok {base_ok}->{ok}, drop_mac {mac}, host F13 delivered {delivered}) ===\n")
    if not g2:
        fails.append("G2")

    if dg2:
        log(f"final crypt-diag: ok={dg2['ok']} mac={dg2['mac']} replay={dg2['replay']} "
            f"aes_redo={dg2.get('aes_redo', '-')} "
            f"announce_retry={dg2.get('announce_retry', '-')} "
            f"kat_fail={dg2.get('kat_fail', '-')}")
    wm = dg.watermark()
    if wm:
        low, endb, top = wm
        log(f"stack watermark: max_depth={top - low} B, slack={low - endb} B "
            f"(low=0x{low:08X} floor_base=0x{endb:08X} top=0x{top:08X})")
    dg.close()

    print("\n" + ("ALL GATES PASS" if not fails else f"FAILED: {', '.join(fails)}"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
