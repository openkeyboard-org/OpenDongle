"""Canonical AES test vectors and differential parameters.

Single source of truth for both the host suite (`tests/test_aes_*.py`) and the
hardware validation suite. Deliberately NOT named `test*.py`, so `unittest
discover` treats it as a plain module rather than collecting it.

The differential parameters below define the 512-vector fold that every backend
— the two CH570 assembly kernels, the portable C cipher, and the CH592/CH572
hardware engines — must reproduce. They are byte-exact constants, not
conventions: change the seed, the count, the key/plaintext interleaving or the
fold and `EXPECTED_FOLD` becomes a different number, silently invalidating
comparison against every result ever recorded. If you think one of them wants
tidying, it does not.
"""

from __future__ import annotations

# (name, key, plaintext, expected ciphertext) — all hex, 16 bytes each.
VECTORS = [
    (
        "FIPS-197 C.1",
        "000102030405060708090a0b0c0d0e0f",
        "00112233445566778899aabbccddeeff",
        "69c4e0d86a7b0430d8cdb78070b4c55a",
    ),
    (
        "all-zero key and plaintext",
        "00" * 16,
        "00" * 16,
        "66e94bd4ef8a2c3b884cfa59ca342b2e",
    ),
    # NIST SP 800-38A F.1.1 ECB-AES128 encrypt, the four sample blocks.
    (
        "SP800-38A F.1.1 block 1",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "6bc1bee22e409f96e93d7e117393172a",
        "3ad77bb40d7a3660a89ecaf32466ef97",
    ),
    (
        "SP800-38A F.1.1 block 2",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "f5d3d58503b9699de785895a96fdbaaf",
    ),
    (
        "SP800-38A F.1.1 block 3",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "43b1cd7f598ece23881b00e3ed030688",
    ),
    (
        "SP800-38A F.1.1 block 4",
        "2b7e151628aed2a6abf7158809cf4f3c",
        "f69f2445df4f9b17ad2b417be66c3710",
        "7b0c785e27e8ad3f8223207104725dd4",
    ),
]

# ---------------------------------------------------------------- differential
#
# 512 blocks, each with an INDEPENDENT pseudorandom key and plaintext, folded
# into one FNV-1a-32 checksum. Independent keys matter: a fixed-key sweep would
# not exercise the key schedule, which on the assembly backends is a separate
# hand-written path from the cipher.
DIFFERENTIAL_SEED = 0x12345678      # xorshift32 state
DIFFERENTIAL_COUNT = 512
FNV_OFFSET_BASIS = 0x811C9DC5
FNV_PRIME = 0x01000193

# What every correct backend produces. Originally established against a CH572
# hardware AES engine; `test_aes_sw.py` re-derives it on the host from the real
# `common/src/aes_sw.c`, so it is a checked value rather than bench folklore.
EXPECTED_FOLD = "b106130c"

# Checkpoint cadence: the on-device harness emits the running fold every 32
# blocks so a divergence localises to a 32-block window instead of only being
# visible as a wrong final number.
CHECKPOINT_EVERY = 32


def xorshift32_stream(seed: int = DIFFERENTIAL_SEED):
    """Yield the same 32-bit sequence the C harness consumes.

    Kept here so a host test can generate the identical (key, plaintext) pairs
    without a device. The advance is applied TWICE per byte position — once for
    the key byte, once for the plaintext byte — so key and plaintext are drawn
    from one interleaved stream rather than two separate ones. That detail is
    part of what `EXPECTED_FOLD` encodes.
    """
    state = seed & 0xFFFFFFFF
    while True:
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        yield state


def differential_pairs(count: int = DIFFERENTIAL_COUNT):
    """Yield `count` (key_hex, plaintext_hex) pairs, matching the C loop."""
    stream = xorshift32_stream()
    for _ in range(count):
        key = bytearray(16)
        plaintext = bytearray(16)
        for i in range(16):
            key[i] = next(stream) & 0xFF
            plaintext[i] = next(stream) & 0xFF
        yield key.hex(), plaintext.hex()


def fnv1a_fold(ciphertexts) -> str:
    """Fold ciphertext blocks exactly as the C harness does."""
    acc = FNV_OFFSET_BASIS
    for block in ciphertexts:
        for byte in bytes.fromhex(block) if isinstance(block, str) else block:
            acc = ((acc ^ byte) * FNV_PRIME) & 0xFFFFFFFF
    return f"{acc:08x}"
