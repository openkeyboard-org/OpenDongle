#!/usr/bin/env python3
"""Drive a WCH-LinkE's 3V3 target rail directly over USB.

minichlink cannot do this on a CH5xx bench. Its -kt/-k3 power flags were
believed to skip target init and therefore still work, but they do not: on this
bench both return rc=223 with

    WCH-LinkE invalid response failed (-1), command: 81 0d 01 02
    Could not setup interface.

because the probe is still asked for target-connect status before the power
command is issued. Every harness that power-cycles the keyboard through
minichlink is therefore dead on arrival here -- see
bench/README-link-encryption.md for why minichlink cannot reach a CH5xx at all.

The rail commands themselves are two bytes of vendor protocol and need no
target connection, so issue them straight to the probe's bulk endpoint.
Permissions come from the existing 1a86:8010 udev rules, so this needs no root.
"""

import time

import usb.core
import usb.util

WCH_VID = 0x1A86
LINK_PID = 0x8010
CMD_POWER_OFF = bytes([0x81, 0x0D, 0x01, 0x0A])
CMD_POWER_ON = bytes([0x81, 0x0D, 0x01, 0x09])


def _open(serial):
    for dev in usb.core.find(find_all=True, idVendor=WCH_VID, idProduct=LINK_PID):
        try:
            if usb.util.get_string(dev, dev.iSerialNumber) == serial:
                return dev
        except Exception:
            continue
    raise SystemExit(f"WCH-Link probe {serial} not found on USB")


def rail(serial, state, settle=0.0):
    """Set a probe's target rail to "off" or "on". Raises on any failure.

    Failing loudly is the point: a power cycle that silently never happened
    leaves the link up and lets an acceptance test pass vacuously, which is the
    worst shape of failure for a bench gate.
    """
    if state not in ("off", "on"):
        raise ValueError(f"rail state must be 'off' or 'on', got {state!r}")
    dev = _open(serial)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass
    usb.util.claim_interface(dev, 0)
    try:
        payload = CMD_POWER_OFF if state == "off" else CMD_POWER_ON
        try:
            dev.write(0x01, payload, timeout=1000)
            dev.read(0x81, 64, timeout=1000)
        except Exception as exc:
            raise SystemExit(f"keyboard power {state} FAILED on {serial}: {exc}")
    finally:
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
    if settle:
        time.sleep(settle)


def cycle(serial, off_time=3.0, settle=0.0):
    """Power-cycle a target: rail off, hold, rail on."""
    rail(serial, "off")
    time.sleep(off_time)
    rail(serial, "on", settle=settle)
