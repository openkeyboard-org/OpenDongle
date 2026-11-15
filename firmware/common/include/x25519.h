/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * X25519 (RFC 7748), resumable.
 *
 * The scalar multiplication is far too long to run in one piece beside an
 * 875 us poll grid, so it is a state machine: x25519_begin() loads the work
 * area, each x25519_step() performs ONE field operation (a multiplication at
 * most) and returns, and x25519_finish() produces the result. The caller
 * decides how many steps fit in the time it has. About 4,900 steps make one
 * scalar multiplication.
 *
 * Constant time with respect to the scalar and the point: no secret-dependent
 * branch, index or loop count. All state lives in the caller's context; the
 * functions use a small fixed amount of stack and no static data.
 *
 * Shared verbatim with OpenController (the keyboard end), like rf_crypt.c.
 */
#ifndef X25519_H
#define X25519_H

#include <stdint.h>

#define X25519_KEY_BYTES 32u

typedef struct {
    uint32_t r[9][8];       /* field registers, radix 2^32, little-endian limbs */
    uint8_t  k[32];         /* clamped scalar */
    uint16_t count;         /* ladder bit index, then inversion exponent index */
    uint8_t  op;            /* position inside the current ladder step */
    uint8_t  phase;
    uint8_t  swap;
} x25519_ctx_t;

/* Start scalar * u. The scalar is clamped per RFC 7748 (a copy; the caller's
 * bytes are not modified) and the top bit of u is ignored. */
void x25519_begin(x25519_ctx_t *ctx,
                  const uint8_t scalar[X25519_KEY_BYTES],
                  const uint8_t u[X25519_KEY_BYTES]);

/* Perform one field operation. Returns 1 while more remain, 0 when done. */
int x25519_step(x25519_ctx_t *ctx);

/* After x25519_step() returned 0: write the 32-byte result and wipe the
 * context. Returns 0 on success, nonzero if the result is all zero -- the
 * peer sent a low-order point and the output MUST NOT be used as a secret. */
int x25519_finish(x25519_ctx_t *ctx, uint8_t out[X25519_KEY_BYTES]);

/* The base point u = 9, for deriving a public key from a private scalar. */
extern const uint8_t x25519_base_point[X25519_KEY_BYTES];

#endif /* X25519_H */
