"""Reference for the key-exchange symmetric half (docs/key-establishment.md §3).

Pure Python over ccm_ref's AES: AES-128-CMAC (RFC 4493), the SP 800-108
counter-mode KDF with CMAC as the PRF, the derivation chain, and the
confirmation MACs. This is the oracle both firmwares' host tests check
against; it is anchored to the RFC 4493 vectors in test_kex_kdf.py.
"""

from __future__ import annotations

from ccm_ref import aes128_encrypt_block

ROLE_DONGLE = 0x44
ROLE_KEYBOARD = 0x4B


def _double(block: bytes) -> bytes:
    n = int.from_bytes(block, "big") << 1
    if n >> 128:
        n = (n & ((1 << 128) - 1)) ^ 0x87
    return n.to_bytes(16, "big")


def cmac(key: bytes, msg: bytes) -> bytes:
    k1 = _double(aes128_encrypt_block(key, bytes(16)))
    k2 = _double(k1)
    blocks = [msg[i:i + 16] for i in range(0, len(msg), 16)] or [b""]
    last = blocks[-1]
    if len(last) == 16:
        last = bytes(a ^ b for a, b in zip(last, k1))
    else:
        padded = last + b"\x80" + bytes(15 - len(last))
        last = bytes(a ^ b for a, b in zip(padded, k2))
    x = bytes(16)
    for block in blocks[:-1]:
        x = aes128_encrypt_block(key, bytes(a ^ b for a, b in zip(x, block)))
    return aes128_encrypt_block(key, bytes(a ^ b for a, b in zip(x, last)))


def kdf(key: bytes, label: bytes, ctx: bytes) -> bytes:
    assert len(label) == 2
    return cmac(key, b"\x01" + label + b"\x00" + ctx + b"\x00\x80")


def derive(z: bytes, transcript: bytes) -> dict:
    prk = cmac(bytes(16), z)
    t_tag = cmac(prk, transcript)
    return {
        "k_cd": kdf(prk, b"CD", t_tag),
        "k_ck": kdf(prk, b"CK", t_tag),
        "lk": kdf(prk, b"LK", t_tag),
    }


def confirm(k_confirm: bytes, role: int, mac_a: bytes, mac_b: bytes) -> bytes:
    return cmac(k_confirm, bytes([role]) + mac_a + mac_b)
