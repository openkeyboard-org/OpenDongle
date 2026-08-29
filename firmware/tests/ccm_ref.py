"""Pure-Python AES-128 + CCM reference, and the OpenDongle wire-frame builder.

This is the host-side source of truth for the encrypted RF frame format. The
firmware CCM layer (`common/src/rf_crypt.c`) is checked against what this module
produces, exactly as `test_aes_sw.py` checks the C cipher against published
vectors. Keeping the reference in Python — independent of the C code it grades —
is the point: a bug that lived in both would have to be made twice, the same way.

Trust chain, so nothing here is self-certifying:
  * the AES core is anchored to the FIPS-197 / SP 800-38A ECB vectors already in
    `aes_vectors.VECTORS` (see `test_ccm_ref.py`);
  * the CCM mode is anchored to the RFC 3610 packet vectors in
    `aes_vectors.CCM_VECTORS`;
  * only then is the frame builder used to grade the firmware.

Deliberately NOT named `test*.py`, so `unittest discover` imports it as a module
rather than collecting it.
"""

from __future__ import annotations

# --------------------------------------------------------------- AES-128 core
#
# The S-box and round constants are COMPUTED (GF(2^8) inverse + affine), not
# transcribed. 256 hand-copied hex bytes are 256 chances to introduce a typo the
# FIPS vectors might not catch; the field arithmetic is short and self-evidently
# the definition.


def _xtime(a: int) -> int:
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF


def _gmul(a: int, b: int) -> int:
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        a = _xtime(a)
        b >>= 1
    return p & 0xFF


def _build_sbox():
    # Multiplicative inverse in GF(2^8), 0 mapping to 0 by convention.
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if _gmul(a, b) == 1:
                inv[a] = b
                break
    sbox = [0] * 256
    for a in range(256):
        x = inv[a]
        s = x
        for i in range(1, 5):
            s ^= ((x << i) | (x >> (8 - i))) & 0xFF
        sbox[a] = (s ^ 0x63) & 0xFF
    return sbox


_SBOX = _build_sbox()
_RCON = [0x01]
for _ in range(9):
    _RCON.append(_xtime(_RCON[-1]))


def _expand_key(key: bytes):
    assert len(key) == 16
    words = [list(key[i : i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        t = list(words[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]                       # RotWord
            t = [_SBOX[b] for b in t]               # SubWord
            t[0] ^= _RCON[i // 4 - 1]
        words.append([words[i - 4][j] ^ t[j] for j in range(4)])
    # Flatten to eleven 16-byte round keys.
    return [bytes(sum(words[r * 4 : r * 4 + 4], [])) for r in range(11)]


def aes128_encrypt_block(key: bytes, block: bytes) -> bytes:
    assert len(block) == 16
    rk = _expand_key(key)
    s = [block[i] ^ rk[0][i] for i in range(16)]    # state is column-major

    def sub(state):
        return [_SBOX[b] for b in state]

    def shift_rows(state):
        out = [0] * 16
        for r in range(4):
            for c in range(4):
                out[r + 4 * c] = state[r + 4 * ((c + r) % 4)]
        return out

    def mix_columns(state):
        out = [0] * 16
        for c in range(4):
            col = state[4 * c : 4 * c + 4]
            out[4 * c + 0] = _gmul(col[0], 2) ^ _gmul(col[1], 3) ^ col[2] ^ col[3]
            out[4 * c + 1] = col[0] ^ _gmul(col[1], 2) ^ _gmul(col[2], 3) ^ col[3]
            out[4 * c + 2] = col[0] ^ col[1] ^ _gmul(col[2], 2) ^ _gmul(col[3], 3)
            out[4 * c + 3] = _gmul(col[0], 3) ^ col[1] ^ col[2] ^ _gmul(col[3], 2)
        return out

    for rnd in range(1, 10):
        s = sub(s)
        s = shift_rows(s)
        s = mix_columns(s)
        s = [s[i] ^ rk[rnd][i] for i in range(16)]
    s = sub(s)
    s = shift_rows(s)
    s = [s[i] ^ rk[10][i] for i in range(16)]
    return bytes(s)


# --------------------------------------------------------------------- CCM
#
# RFC 3610 / NIST SP 800-38C, forward cipher only (encrypt for both the CBC-MAC
# and the CTR keystream) — which is exactly what the OpenDongle hal_aes seam
# offers and why CCM was chosen over a mode that needs the inverse cipher.


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def _ccm_flags(adata: bool, m: int, l: int) -> int:
    return (0x40 if adata else 0) | (((m - 2) // 2) << 3) | (l - 1)


def _format_aad(aad: bytes) -> bytes:
    # Only the small-AAD encoding (len < 2^16 - 2^8) is implemented; the frame
    # AAD is two bytes, and the RFC vectors use 8.
    if not aad:
        return b""
    assert len(aad) < 0xFF00
    blk = len(aad).to_bytes(2, "big") + aad
    if len(blk) % 16:
        blk += b"\x00" * (16 - len(blk) % 16)
    return blk


def _ctr_block(nonce: bytes, l: int, i: int) -> bytes:
    return bytes([_ccm_flags(False, 2, l)]) + nonce + i.to_bytes(l, "big")


def ccm_encrypt(key: bytes, nonce: bytes, aad: bytes, payload: bytes, m: int = 8) -> bytes:
    """Return ciphertext || tag (tag is `m` bytes)."""
    l = 15 - len(nonce)
    b0 = bytes([_ccm_flags(bool(aad), m, l)]) + nonce + len(payload).to_bytes(l, "big")

    mac = aes128_encrypt_block(key, b0)
    for off in range(0, len(_format_aad(aad)), 16):
        mac = aes128_encrypt_block(key, _xor(mac, _format_aad(aad)[off : off + 16]))
    padded = payload + b"\x00" * (-len(payload) % 16)
    for off in range(0, len(padded), 16):
        mac = aes128_encrypt_block(key, _xor(mac, padded[off : off + 16]))
    tag = mac[:m]

    s0 = aes128_encrypt_block(key, _ctr_block(nonce, l, 0))
    u = _xor(tag, s0[:m])

    ct = bytearray()
    for idx, off in enumerate(range(0, len(payload), 16), start=1):
        keystream = aes128_encrypt_block(key, _ctr_block(nonce, l, idx))
        chunk = payload[off : off + 16]
        ct += _xor(chunk, keystream[: len(chunk)])
    return bytes(ct) + u


def ccm_decrypt(key: bytes, nonce: bytes, aad: bytes, ct_and_tag: bytes, m: int = 8):
    """Return the plaintext, or None if the tag does not verify."""
    l = 15 - len(nonce)
    ct, u = ct_and_tag[:-m], ct_and_tag[-m:]

    pt = bytearray()
    for idx, off in enumerate(range(0, len(ct), 16), start=1):
        keystream = aes128_encrypt_block(key, _ctr_block(nonce, l, idx))
        chunk = ct[off : off + 16]
        pt += _xor(chunk, keystream[: len(chunk)])

    b0 = bytes([_ccm_flags(bool(aad), m, l)]) + nonce + len(pt).to_bytes(l, "big")
    mac = aes128_encrypt_block(key, b0)
    for off in range(0, len(_format_aad(aad)), 16):
        mac = aes128_encrypt_block(key, _xor(mac, _format_aad(aad)[off : off + 16]))
    padded = bytes(pt) + b"\x00" * (-len(pt) % 16)
    for off in range(0, len(padded), 16):
        mac = aes128_encrypt_block(key, _xor(mac, padded[off : off + 16]))

    s0 = aes128_encrypt_block(key, _ctr_block(nonce, l, 0))
    if _xor(mac[:m], s0[:m]) != u:
        return None
    return bytes(pt)


# ------------------------------------------------------- OpenDongle wire frame
#
# Must stay byte-identical to common/src/rf_crypt.c. See the plan's wire-format
# table: L=2, 13-byte nonce, 8-byte tag, AAD = ctrl||tag.

DIR_KB_TO_DONGLE = 0x01
DIR_DONGLE_TO_KB = 0x02
CCM_TAG_BYTES = 8


def build_nonce(session_id: int, direction: int, counter: int) -> bytes:
    """13-byte CCM nonce: session_id(4 LE) || dir(1) || counter(4 LE) || 0000."""
    return (
        session_id.to_bytes(4, "little")
        + bytes([direction])
        + counter.to_bytes(4, "little")
        + b"\x00\x00\x00\x00"
    )


def build_frame(key: bytes, session_id: int, counter: int, ctrl: int, tag: int,
                body: bytes, direction: int = DIR_KB_TO_DONGLE) -> bytes:
    """Encrypted on-air frame from rxBuf[2] onward: the LEN-covered bytes.

    Layout: ctrl || tag || counter(4 LE) || ciphertext(len body) || tag8.
    """
    nonce = build_nonce(session_id, direction, counter)
    aad = bytes([ctrl, tag])
    ct_and_tag = ccm_encrypt(key, nonce, aad, body, CCM_TAG_BYTES)
    return bytes([ctrl, tag]) + counter.to_bytes(4, "little") + ct_and_tag


def open_frame(key: bytes, session_id: int, frame: bytes,
               direction: int = DIR_KB_TO_DONGLE):
    """Inverse of build_frame. Returns (counter, ctrl, tag, body) or None."""
    if len(frame) < 2 + 4 + CCM_TAG_BYTES:
        return None
    ctrl, tag = frame[0], frame[1]
    counter = int.from_bytes(frame[2:6], "little")
    ct_and_tag = frame[6:]
    nonce = build_nonce(session_id, direction, counter)
    aad = bytes([ctrl, tag])
    body = ccm_decrypt(key, nonce, aad, ct_and_tag, CCM_TAG_BYTES)
    if body is None:
        return None
    return counter, ctrl, tag, body
