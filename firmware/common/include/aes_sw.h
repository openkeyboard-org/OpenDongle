/*
 * OpenKeyboard.org OpenDongle -- software AES-128 forward cipher.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Portable encrypt-only AES-128. This is the primitive; hal_aes.h is the seam
 * the firmware actually calls. Exposed separately so it can be compiled and
 * known-answer tested on the host, independent of any chip.
 *
 * Forward cipher only — counter mode encrypts in both directions, so there is
 * no inverse cipher here. Not constant-time; see hal_aes.h for why that is
 * acceptable for this device and where it would not be.
 */
#ifndef AES_SW_H
#define AES_SW_H

#include <stdint.h>

/* Expanded key schedule: 44 words = 11 round keys of 16 bytes (176 B, as before). */
typedef struct {
    uint32_t rk[44];
} aes_sw_ctx_t;

/* Expand a 16-byte key into `ctx`. The expensive half; do it once per key. */
void aes_sw_expand_key(aes_sw_ctx_t *ctx, const uint8_t key[16]);

/* Encrypt one 16-byte block.
 *
 * ANY overlap of `out` and `in` is safe, not just exact aliasing: all 16 input
 * bytes are read before any output byte is written. This matches the hal_aes.h
 * contract, and firmware/tests/test_aes_sw.py exercises `out = in + 1` against
 * this very source -- the case that catches an implementation which writes as
 * it goes. The comment previously promised only exact aliasing, which would
 * have pushed callers into pointless copies. */
void aes_sw_encrypt_block(const aes_sw_ctx_t *ctx, const uint8_t in[16],
                          uint8_t out[16]);

#endif /* AES_SW_H */
