#!/usr/bin/env python3
"""Parse, validate, and finalize OpenDongle ODG2 application images."""

from __future__ import annotations

import binascii
import struct

IMAGE_ID_OFF = 0x20
IMAGE_ID_LEN = 0x20
IMAGE_ID_MAGIC = b"ODG2"
IMAGE_ID_FORMAT = 2
IMAGE_KIND_APP = 1
IMAGE_CRC_FIELD_OFF = 0x10
# Slot A's base, uniform across the supported chips. NOT "the app base" any
# more: under OpenBoot's A/B slots an application is linked once per slot, and
# slot B sits at a chip-specific address (0x1E000 on CH570, 0x39000 on CH592).
# Callers that handle both pass valid_bases= to validate_app_image_identity;
# this stays the default so an un-updated caller keeps rejecting slot B rather
# than silently accepting any base.
APP_BASE = 0x2000
MIN_APP_IMAGE_LEN = 0x1000
COMMIT_ALIGNMENT = 4


def image_crc32(image: bytes | bytearray) -> int:
    """CRC the complete image with the ODG2 checksum field treated as zero."""
    data = bytearray(image)
    off = IMAGE_ID_OFF + IMAGE_CRC_FIELD_OFF
    if len(data) < off + 4:
        raise ValueError("image is too short for the ODG2 CRC field")
    data[off:off + 4] = b"\0\0\0\0"
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_image_id(image):
    if len(image) < IMAGE_ID_OFF + IMAGE_ID_LEN:
        return None
    h = image[IMAGE_ID_OFF:IMAGE_ID_OFF + IMAGE_ID_LEN]
    if h[:4] != IMAGE_ID_MAGIC:
        return None
    values = struct.unpack_from("<4sBBBBIIIIII", h)
    return {
        "format": values[1],
        "family": values[2],
        "kind": values[3],
        "header_len": values[4],
        "base": values[5],
        "image_len": values[6],
        "image_crc32": values[7],
        "build_id": values[8],
        "flags": values[9],
        "extension_len": values[10],
    }


def finalize_image(image: bytes) -> bytes:
    """Return an immutable image with its ODG2 CRC field finalized."""
    identity = parse_image_id(image)
    if identity is None:
        raise ValueError(f"missing ODG2 header at offset 0x{IMAGE_ID_OFF:X}")
    if identity["image_len"] != len(image):
        raise ValueError(
            f"ODG2 image_len {identity['image_len']} != binary size {len(image)}")
    out = bytearray(image)
    off = IMAGE_ID_OFF + IMAGE_CRC_FIELD_OFF
    out[off:off + 4] = image_crc32(out).to_bytes(4, "little")
    return bytes(out)


def validate_app_image_identity(image, *, loaded_base, expected_family=None,
                                expected_kind=IMAGE_KIND_APP,
                                require_header=False, max_image_len=None,
                                valid_bases=None):
    """Check an application image against the ODG2 contract.

    `valid_bases` is the set of addresses OpenBoot may hand control to - the
    A/B slot bases for this chip. It defaults to (APP_BASE,), i.e. slot A only,
    which is what a caller that has not been taught about slots should get:
    accepting any base by default would let a mis-linked image through
    silently.

    The base is checked two ways, and they are different questions. The image
    must be linked at a base OpenBoot actually jumps to, AND the ODG2 header
    must agree with where the image is really loaded - a header claiming slot A
    on an image linked for slot B is exactly the confusion the host tooling
    exists to catch, and comparing both against one constant could not see it.
    """
    problems = []
    actual_len = len(image)
    bases = tuple(valid_bases) if valid_bases is not None else (APP_BASE,)
    if loaded_base not in bases:
        allowed = ", ".join(f"0x{b:X}" for b in bases)
        problems.append(
            f"load base 0x{loaded_base:X} is not an OpenBoot slot base ({allowed})")
    if actual_len < MIN_APP_IMAGE_LEN:
        problems.append(f"image is only {actual_len} bytes; minimum is {MIN_APP_IMAGE_LEN}")
    if actual_len % COMMIT_ALIGNMENT:
        problems.append(f"image length {actual_len} is not 4-byte aligned")
    if max_image_len is not None and actual_len > max_image_len:
        problems.append(f"image length {actual_len} exceeds slot {max_image_len}")

    identity = parse_image_id(image)
    if identity is None:
        if require_header:
            problems.append(f"missing ODG2 header at offset 0x{IMAGE_ID_OFF:X}")
        return None, problems
    if identity["format"] != IMAGE_ID_FORMAT:
        problems.append(f"unknown ODG2 format {identity['format']}")
    if identity["header_len"] != IMAGE_ID_LEN:
        problems.append(f"ODG2 header_len is {identity['header_len']}, expected {IMAGE_ID_LEN}")
    if expected_family is not None and identity["family"] != expected_family:
        problems.append(f"chip family 0x{identity['family']:02X} != 0x{expected_family:02X}")
    if expected_kind is not None and identity["kind"] != expected_kind:
        problems.append(f"image kind 0x{identity['kind']:02X} != 0x{expected_kind:02X}")
    if identity["base"] != loaded_base:
        problems.append(
            f"ODG2 header base 0x{identity['base']:X} != load base "
            f"0x{loaded_base:X}")
    if identity["image_len"] != actual_len:
        problems.append(f"header image_len {identity['image_len']} != {actual_len}")
    if identity["extension_len"] != 0:
        problems.append("ODG2 extensions are not supported by format 2")
    if identity["flags"] != 0:
        problems.append("ODG2 flags are not supported by format 2")
    if identity["image_crc32"] != image_crc32(image):
        problems.append("ODG2 whole-image CRC mismatch")
    return identity, problems
