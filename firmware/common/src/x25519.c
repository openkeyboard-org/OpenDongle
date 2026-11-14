/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * X25519 (RFC 7748), resumable -- see x25519.h.
 *
 * Field elements are eight 32-bit limbs, little-endian, holding any value
 * below 2^256 (not necessarily below p = 2^255 - 19). Every operation folds
 * what overflows 2^256 back in with 2^256 = 38 (mod p); only the final result
 * is reduced to its canonical form. Limb products use 64-bit accumulators,
 * which RV32IM provides as mul + mulhu with no library call.
 */
#include "x25519.h"

enum { R_X1, R_X2, R_Z2, R_X3, R_Z3, R_T0, R_T1, R_T2, R_T3 };
enum { PH_LADDER, PH_INVERT_SQ, PH_INVERT_MUL, PH_FINAL, PH_DONE };
enum { OP_ADD, OP_SUB, OP_MUL, OP_A24 };

const uint8_t x25519_base_point[X25519_KEY_BYTES] = { 9 };

/* r += 38 * c, c small; then once more for the carry that can produce. */
static void fe_fold(uint32_t r[8], uint32_t c)
{
    uint64_t t = (uint64_t)c * 38u;
    int i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 8; i++) {
            t += r[i];
            r[i] = (uint32_t)t;
            t >>= 32;
        }
        t *= 38u;   /* a carry out of the top is worth 38 again; 0 on pass 2 */
    }
}

static void fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < 8; i++) {
        t += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);
}

static void fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint32_t borrow = 0;
    int i, pass;

    for (i = 0; i < 8; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (uint32_t)(t >> 63);
    }
    /* A borrow out of the top added 2^256 = 38 too much: take 38 back off,
     * and once more if that itself borrows (r was then below 38). */
    for (pass = 0; pass < 2; pass++) {
        uint32_t sub = borrow * 38u;
        borrow = 0;
        for (i = 0; i < 8; i++) {
            uint64_t t = (uint64_t)r[i] - sub - borrow;
            r[i] = (uint32_t)t;
            borrow = (uint32_t)(t >> 63);
            sub = 0;
        }
    }
}

static void fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint32_t p[16];
    uint64_t t;
    int i, j;

    for (i = 0; i < 16; i++) {
        p[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        uint32_t carry = 0;
        for (j = 0; j < 8; j++) {
            t = (uint64_t)a[i] * b[j] + p[i + j] + carry;
            p[i + j] = (uint32_t)t;
            carry = (uint32_t)(t >> 32);
        }
        p[i + 8] = carry;
    }
    /* low + 38 * high */
    t = 0;
    for (i = 0; i < 8; i++) {
        t += (uint64_t)p[i] + (uint64_t)p[i + 8] * 38u;
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);
    for (i = 0; i < 16; i++) {
        p[i] = 0;
    }
}

static void fe_mul_a24(uint32_t r[8], const uint32_t a[8])
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < 8; i++) {
        t += (uint64_t)a[i] * 121665u;
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);
}

static void fe_cswap(uint32_t a[8], uint32_t b[8], uint32_t swap)
{
    uint32_t mask = (uint32_t)0 - swap;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t x = mask & (a[i] ^ b[i]);
        a[i] ^= x;
        b[i] ^= x;
    }
}

/* One Montgomery ladder step (RFC 7748 section 5), as field operations over
 * the register file. T0..T3 are scratch. */
static const uint8_t ladder_prog[18][4] = {
    { OP_ADD, R_T0, R_X2, R_Z2 },   /* A  = x2 + z2            */
    { OP_SUB, R_T1, R_X2, R_Z2 },   /* B  = x2 - z2            */
    { OP_ADD, R_T2, R_X3, R_Z3 },   /* C  = x3 + z3            */
    { OP_SUB, R_T3, R_X3, R_Z3 },   /* D  = x3 - z3            */
    { OP_MUL, R_T3, R_T3, R_T0 },   /* DA = D * A              */
    { OP_MUL, R_T2, R_T2, R_T1 },   /* CB = C * B              */
    { OP_ADD, R_X3, R_T3, R_T2 },   /*      DA + CB            */
    { OP_MUL, R_X3, R_X3, R_X3 },   /* x3 = (DA + CB)^2        */
    { OP_SUB, R_Z3, R_T3, R_T2 },   /*      DA - CB            */
    { OP_MUL, R_Z3, R_Z3, R_Z3 },   /*      (DA - CB)^2        */
    { OP_MUL, R_Z3, R_Z3, R_X1 },   /* z3 = x1 * (DA - CB)^2   */
    { OP_MUL, R_T0, R_T0, R_T0 },   /* AA = A^2                */
    { OP_MUL, R_T1, R_T1, R_T1 },   /* BB = B^2                */
    { OP_MUL, R_X2, R_T0, R_T1 },   /* x2 = AA * BB            */
    { OP_SUB, R_T1, R_T0, R_T1 },   /* E  = AA - BB            */
    { OP_A24, R_T2, R_T1, R_T1 },   /*      a24 * E            */
    { OP_ADD, R_T2, R_T0, R_T2 },   /*      AA + a24 * E       */
    { OP_MUL, R_Z2, R_T1, R_T2 },   /* z2 = E * (AA + a24 * E) */
};

void x25519_begin(x25519_ctx_t *ctx,
                  const uint8_t scalar[X25519_KEY_BYTES],
                  const uint8_t u[X25519_KEY_BYTES])
{
    int i, j;

    for (i = 0; i < 9; i++) {
        for (j = 0; j < 8; j++) {
            ctx->r[i][j] = 0;
        }
    }
    for (i = 0; i < 32; i++) {
        ctx->k[i] = scalar[i];
    }
    ctx->k[0] &= 248u;
    ctx->k[31] &= 127u;
    ctx->k[31] |= 64u;

    for (i = 0; i < 8; i++) {
        ctx->r[R_X1][i] = (uint32_t)u[4 * i]
                        | ((uint32_t)u[4 * i + 1] << 8)
                        | ((uint32_t)u[4 * i + 2] << 16)
                        | ((uint32_t)u[4 * i + 3] << 24);
    }
    ctx->r[R_X1][7] &= 0x7FFFFFFFu;          /* RFC 7748: ignore the top bit */
    for (i = 0; i < 8; i++) {
        ctx->r[R_X3][i] = ctx->r[R_X1][i];
    }
    ctx->r[R_X2][0] = 1u;
    ctx->r[R_Z3][0] = 1u;

    ctx->count = 254u;
    ctx->op = 0u;
    ctx->swap = 0u;
    ctx->phase = PH_LADDER;
}

int x25519_step(x25519_ctx_t *ctx)
{
    switch (ctx->phase) {
    case PH_LADDER: {
        const uint8_t *ins = ladder_prog[ctx->op];

        if (ctx->op == 0u) {
            uint8_t bit = (uint8_t)((ctx->k[ctx->count >> 3] >> (ctx->count & 7u)) & 1u);
            ctx->swap ^= bit;
            fe_cswap(ctx->r[R_X2], ctx->r[R_X3], ctx->swap);
            fe_cswap(ctx->r[R_Z2], ctx->r[R_Z3], ctx->swap);
            ctx->swap = bit;
        }
        switch (ins[0]) {
        case OP_ADD: fe_add(ctx->r[ins[1]], ctx->r[ins[2]], ctx->r[ins[3]]); break;
        case OP_SUB: fe_sub(ctx->r[ins[1]], ctx->r[ins[2]], ctx->r[ins[3]]); break;
        case OP_MUL: fe_mul(ctx->r[ins[1]], ctx->r[ins[2]], ctx->r[ins[3]]); break;
        default:     fe_mul_a24(ctx->r[ins[1]], ctx->r[ins[2]]);             break;
        }
        if (++ctx->op == 18u) {
            ctx->op = 0u;
            if (ctx->count == 0u) {
                int i;
                fe_cswap(ctx->r[R_X2], ctx->r[R_X3], ctx->swap);
                fe_cswap(ctx->r[R_Z2], ctx->r[R_Z3], ctx->swap);
                /* Invert z2: T0 = z2^(p-2), p-2 = 2^255 - 21. The exponent's
                 * bits are all ones except bits 2 and 4, so: start from z2,
                 * then for each bit from 253 down to 0 square, and multiply by
                 * z2 unless the bit is 2 or 4. The exponent is public. */
                for (i = 0; i < 8; i++) {
                    ctx->r[R_T0][i] = ctx->r[R_Z2][i];
                }
                ctx->count = 253u;
                ctx->phase = PH_INVERT_SQ;
            } else {
                ctx->count--;
            }
        }
        return 1;
    }
    case PH_INVERT_SQ:
        fe_mul(ctx->r[R_T0], ctx->r[R_T0], ctx->r[R_T0]);
        if (ctx->count != 2u && ctx->count != 4u) {
            ctx->phase = PH_INVERT_MUL;
        } else {
            ctx->count--;          /* never the last bit: 2 and 4 are not 0 */
        }
        return 1;
    case PH_INVERT_MUL:
        fe_mul(ctx->r[R_T0], ctx->r[R_T0], ctx->r[R_Z2]);
        if (ctx->count == 0u) {
            ctx->phase = PH_FINAL;
        } else {
            ctx->count--;
            ctx->phase = PH_INVERT_SQ;
        }
        return 1;
    case PH_FINAL:
        fe_mul(ctx->r[R_X2], ctx->r[R_X2], ctx->r[R_T0]);
        ctx->phase = PH_DONE;
        return 0;
    default:
        return 0;
    }
}

int x25519_finish(x25519_ctx_t *ctx, uint8_t out[X25519_KEY_BYTES])
{
    uint32_t v[8], t[8];
    uint32_t top, mask, acc = 0;
    uint64_t c;
    int i, pass;
    volatile uint8_t *wipe = (volatile uint8_t *)ctx;

    for (i = 0; i < 8; i++) {
        v[i] = ctx->r[R_X2][i];
    }
    /* Fold bit 255 down (2^255 = 19) twice: v is then below 2^255 + 19... and
     * after the second pass below 2^255. */
    for (pass = 0; pass < 2; pass++) {
        top = v[7] >> 31;
        v[7] &= 0x7FFFFFFFu;
        c = (uint64_t)top * 19u;
        for (i = 0; i < 8; i++) {
            c += v[i];
            v[i] = (uint32_t)c;
            c >>= 32;
        }
    }
    /* v < 2^255. Subtract p when v >= p: t = v + 19 has bit 255 set exactly
     * then, and t with that bit cleared is v - p. */
    c = 19u;
    for (i = 0; i < 8; i++) {
        c += v[i];
        t[i] = (uint32_t)c;
        c >>= 32;
    }
    mask = (uint32_t)0 - (t[7] >> 31);
    t[7] &= 0x7FFFFFFFu;
    for (i = 0; i < 8; i++) {
        v[i] = (v[i] & ~mask) | (t[i] & mask);
    }

    for (i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)v[i];
        out[4 * i + 1] = (uint8_t)(v[i] >> 8);
        out[4 * i + 2] = (uint8_t)(v[i] >> 16);
        out[4 * i + 3] = (uint8_t)(v[i] >> 24);
        acc |= v[i];
        v[i] = 0;
        t[i] = 0;
    }
    for (i = 0; i < (int)sizeof(*ctx); i++) {
        wipe[i] = 0;
    }
    ctx->phase = PH_DONE;
    return acc == 0u;
}
