"""Anchor the Python CCM reference before it is used to grade firmware.

`ccm_ref` is the source of truth for the encrypted RF frame bytes, so it must
itself be pinned to something external. Two independent anchors:

  * the AES core against the FIPS-197 / SP 800-38A ECB vectors, and
  * the CCM mode against the RFC 3610 packet vectors.

Only with both green is `ccm_ref.build_frame` trustworthy as the expected-output
generator for `test_rf_crypt.py`. This file compiles nothing — it is pure host
Python and always runs, even on a machine with no C compiler.
"""

from __future__ import annotations

import unittest

import aes_vectors
import ccm_ref


class AesCore(unittest.TestCase):
    """The pure-Python AES-128 must match the published ECB vectors."""

    def test_published_vectors(self):
        for name, key, plaintext, expected in aes_vectors.VECTORS:
            with self.subTest(vector=name):
                out = ccm_ref.aes128_encrypt_block(
                    bytes.fromhex(key), bytes.fromhex(plaintext)
                )
                self.assertEqual(out.hex(), expected)


class CcmMode(unittest.TestCase):
    """The CCM mode must match RFC 3610, and decrypt must invert encrypt."""

    def test_rfc3610_encrypt(self):
        for v in aes_vectors.CCM_VECTORS:
            with self.subTest(vector=v["name"]):
                out = ccm_ref.ccm_encrypt(
                    bytes.fromhex(v["key"]),
                    bytes.fromhex(v["nonce"]),
                    bytes.fromhex(v["aad"]),
                    bytes.fromhex(v["payload"]),
                    ccm_ref.CCM_TAG_BYTES,
                )
                self.assertEqual(out.hex(), v["ct_tag"])

    def test_rfc3610_decrypt_roundtrip(self):
        for v in aes_vectors.CCM_VECTORS:
            with self.subTest(vector=v["name"]):
                pt = ccm_ref.ccm_decrypt(
                    bytes.fromhex(v["key"]),
                    bytes.fromhex(v["nonce"]),
                    bytes.fromhex(v["aad"]),
                    bytes.fromhex(v["ct_tag"]),
                    ccm_ref.CCM_TAG_BYTES,
                )
                self.assertEqual(pt, bytes.fromhex(v["payload"]))

    def test_tamper_is_rejected(self):
        v = aes_vectors.CCM_VECTORS[0]
        key, nonce = bytes.fromhex(v["key"]), bytes.fromhex(v["nonce"])
        aad, ct_tag = bytes.fromhex(v["aad"]), bytearray(bytes.fromhex(v["ct_tag"]))
        ct_tag[0] ^= 0x01
        self.assertIsNone(
            ccm_ref.ccm_decrypt(key, nonce, aad, bytes(ct_tag), ccm_ref.CCM_TAG_BYTES)
        )


class WireFrame(unittest.TestCase):
    """The frame builder round-trips and binds ctrl/tag/counter/session."""

    KEY = bytes(range(16))

    def test_roundtrip(self):
        frame = ccm_ref.build_frame(
            self.KEY, session_id=0x11223344, counter=7,
            ctrl=0x5A, tag=0xA1, body=bytes([1, 2, 3, 4, 5, 6, 7, 8]),
        )
        # LEN-covered length = ctrl+tag+counter(4)+body(8)+tag8(8) = 22.
        self.assertEqual(len(frame), 22)
        got = ccm_ref.open_frame(self.KEY, 0x11223344, frame)
        self.assertIsNotNone(got)
        counter, ctrl, tag, body = got
        self.assertEqual((counter, ctrl, tag), (7, 0x5A, 0xA1))
        self.assertEqual(body, bytes([1, 2, 3, 4, 5, 6, 7, 8]))

    def test_wrong_session_fails(self):
        frame = ccm_ref.build_frame(
            self.KEY, session_id=0x11223344, counter=7,
            ctrl=0x5A, tag=0xA1, body=b"\x09\x0a",
        )
        self.assertIsNone(ccm_ref.open_frame(self.KEY, 0x55667788, frame))

    def test_aad_binds_ctrl(self):
        frame = bytearray(ccm_ref.build_frame(
            self.KEY, session_id=1, counter=1, ctrl=0x00, tag=0xA8,
            body=b"\x01\x02\x03\x04\x05",
        ))
        frame[0] ^= 0x20  # flip a ctrl bit; AAD covers it, so the tag must fail
        self.assertIsNone(ccm_ref.open_frame(self.KEY, 1, bytes(frame)))


if __name__ == "__main__":
    unittest.main()
