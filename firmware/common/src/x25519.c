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

/* Where the arithmetic lives. A port whose flash executes in place slowly can
 * move the inner routines to RAM:
 *   X25519_ARITH_IN_RAM=1  multiply, square, add, subtract
 *   X25519_ARITH_IN_RAM=2  those plus the small-constant multiply and the
 *                          conditional swap
 *   X25519_ARITH_IN_RAM=3  those plus the step dispatcher
 * Timings on a CH592 at 60 MHz are in the commit that last changed this file. */
#ifndef X25519_ARITH_IN_RAM
#define X25519_ARITH_IN_RAM 0
#endif
#define X25519_RAM __attribute__((section(".highcode"), noinline))
#if X25519_ARITH_IN_RAM >= 1
#define X25519_HOT X25519_RAM
#else
#define X25519_HOT __attribute__((noinline))
#endif
#if X25519_ARITH_IN_RAM >= 2
#define X25519_HOT2 X25519_RAM
#else
#define X25519_HOT2 __attribute__((noinline))
#endif
#if X25519_ARITH_IN_RAM >= 3
#define X25519_HOT_STEP __attribute__((section(".highcode")))
#else
#define X25519_HOT_STEP
#endif
#define X25519_INLINE static inline __attribute__((always_inline))

enum { R_X1, R_X2, R_Z2, R_X3, R_Z3, R_T0, R_T1, R_T2, R_T3 };
enum { PH_LADDER, PH_INVERT, PH_FINAL, PH_DONE };

const uint8_t x25519_base_point[X25519_KEY_BYTES] = { 9 };

/* Zero n words through a volatile pointer, so the stores survive. Scratch
 * that held secret-derived values is wiped with this, never with plain
 * assignments, which the compiler is free to drop. */
X25519_INLINE void wipe_words(uint32_t *p, unsigned n)
{
    volatile uint32_t *v = p;

    while (n--) {
        *v++ = 0u;
    }
}

/* r += 38 * c, for c <= 121664 (so 38 * c fits a word; the callers give at
 * most 1, 38 and 121664). If the addition carries out of the top, the wrapped
 * value is below 38 * c, so the 38 that carry is worth lands in limb 0
 * without a further carry. Always inlined: it runs wherever its caller does. */
X25519_INLINE void fe_fold(uint32_t r[8], uint32_t c)
{
    uint32_t carry = c * 38u;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t old = r[i];
        uint32_t sum = old + carry;
        r[i] = sum;
        carry = (uint32_t)(sum < old);
    }
    r[0] += ((uint32_t)0 - carry) & 38u;
}

/* r = low(p) + 38 * high(p), folded: the tail of every multiplication. */
X25519_INLINE void fe_reduce(uint32_t r[8], const uint32_t p[16])
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < 8; i++) {
        t += (uint64_t)p[i] + (uint64_t)p[i + 8] * 38u;
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);          /* t <= 38 */
}

X25519_HOT
static void fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint32_t carry = 0;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t ai = a[i];
        uint32_t s = ai + b[i];
        uint32_t v = s + carry;
        carry = (uint32_t)(s < ai) | (uint32_t)(v < s);
        r[i] = v;
    }
    fe_fold(r, carry);
}

X25519_HOT
static void fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint32_t borrow = 0;
    uint32_t sub;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t ai = a[i], bi = b[i];
        uint32_t d = ai - bi;
        uint32_t v = d - borrow;
        borrow = (uint32_t)(ai < bi) | (uint32_t)(d < borrow);
        r[i] = v;
    }
    /* A borrow out of the top added 2^256 = 38 too much: take 38 back off.
     * If that itself borrows, the wrapped value's limb 0 is at least
     * 2^32 - 38, so the second 38 comes off limb 0 alone. */
    sub = borrow * 38u;
    for (i = 0; i < 8; i++) {
        uint32_t old = r[i];
        r[i] = old - sub;
        sub = (uint32_t)(old < sub);
    }
    r[0] -= ((uint32_t)0 - sub) & 38u;
}

/* Self-contained on purpose: no call leaves this routine (the first row is
 * written rather than accumulated, so the product needs no zeroing, which the
 * compiler would turn into a memset call into flash), and the reduction is
 * inline. r may alias a or b: nothing is written to r until the whole product
 * is formed. */
X25519_HOT
static void fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint32_t p[16];
    uint32_t carry = 0;
    uint64_t t;
    int i, j;

    for (j = 0; j < 8; j++) {
        t = (uint64_t)a[0] * b[j] + carry;
        p[j] = (uint32_t)t;
        carry = (uint32_t)(t >> 32);
    }
    p[8] = carry;
    for (i = 1; i < 8; i++) {
        carry = 0;
        for (j = 0; j < 8; j++) {
            t = (uint64_t)a[i] * b[j] + p[i + j] + carry;
            p[i + j] = (uint32_t)t;
            carry = (uint32_t)(t >> 32);
        }
        p[i + 8] = carry;
    }
    fe_reduce(r, p);
    wipe_words(p, 16);
}

/* Squaring is a multiplication. A dedicated routine (36 limb products
 * instead of 64, column by column) was written, tested and measured on a
 * CH592 with the arithmetic in SRAM: it made a scalar multiplication 6%
 * SLOWER and cost 470 more bytes of SRAM, because without a carry flag the
 * three-word column accumulator costs more than the 28 products it skips. */
#define fe_sq(r, a) fe_mul((r), (a), (a))

X25519_HOT2
static void fe_mul_a24(uint32_t r[8], const uint32_t a[8])
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < 8; i++) {
        t += (uint64_t)a[i] * 121665u;
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);          /* t <= 121664 */
}

X25519_HOT2
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

/* Reduce to the canonical value below p and store it little-endian. Returns
 * the OR of the limbs (zero exactly when the value is zero). */
static uint32_t fe_tobytes(uint8_t out[32], const uint32_t a[8])
{
    uint32_t v[8], t[8];
    uint32_t top, mask, acc = 0;
    uint64_t c;
    int i, pass;

    for (i = 0; i < 8; i++) {
        v[i] = a[i];
    }
    /* Fold bit 255 down (2^255 = 19) twice: after the first pass v is below
     * 2^255 + 19, after the second below 2^255. */
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
    /* Subtract p when v >= p: t = v + 19 has bit 255 set exactly then, and t
     * with that bit cleared is v - p. */
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
        out[4 * i]     = (uint8_t)v[i];
        out[4 * i + 1] = (uint8_t)(v[i] >> 8);
        out[4 * i + 2] = (uint8_t)(v[i] >> 16);
        out[4 * i + 3] = (uint8_t)(v[i] >> 24);
        acc |= v[i];
    }
    wipe_words(v, 8);
    wipe_words(t, 8);
    return acc;
}

/* Inversion: T0 = z2^(p-2), p-2 = 2^255 - 21, by the usual addition chain --
 * 254 squarings and 11 multiplications instead of the 254 + 252 of plain
 * square-and-multiply. Each row is {dst, a, b, n}: n == 0 is dst = a * b;
 * n > 0 is dst = a^(2^n) (b unused). The exponent is public, so the schedule
 * is too. Comments give the exponent held in the destination. */
static const uint8_t invert_prog[22][4] = {
    { R_T0, R_Z2, 0,    1   },   /* 2                         */
    { R_T1, R_T0, 0,    2   },   /* 8                         */
    { R_T1, R_Z2, R_T1, 0   },   /* 9                         */
    { R_T0, R_T0, R_T1, 0   },   /* 11                        */
    { R_T2, R_T0, 0,    1   },   /* 22                        */
    { R_T1, R_T1, R_T2, 0   },   /* 31 = 2^5 - 1              */
    { R_T2, R_T1, 0,    5   },   /* 2^10 - 2^5                */
    { R_T1, R_T2, R_T1, 0   },   /* 2^10 - 1                  */
    { R_T2, R_T1, 0,    10  },   /* 2^20 - 2^10               */
    { R_T2, R_T2, R_T1, 0   },   /* 2^20 - 1                  */
    { R_T3, R_T2, 0,    20  },   /* 2^40 - 2^20               */
    { R_T2, R_T3, R_T2, 0   },   /* 2^40 - 1                  */
    { R_T2, R_T2, 0,    10  },   /* 2^50 - 2^10               */
    { R_T1, R_T2, R_T1, 0   },   /* 2^50 - 1                  */
    { R_T2, R_T1, 0,    50  },   /* 2^100 - 2^50              */
    { R_T2, R_T2, R_T1, 0   },   /* 2^100 - 1                 */
    { R_T3, R_T2, 0,    100 },   /* 2^200 - 2^100             */
    { R_T2, R_T3, R_T2, 0   },   /* 2^200 - 1                 */
    { R_T2, R_T2, 0,    50  },   /* 2^250 - 2^50              */
    { R_T1, R_T2, R_T1, 0   },   /* 2^250 - 1                 */
    { R_T1, R_T1, 0,    5   },   /* 2^255 - 2^5               */
    { R_T0, R_T1, R_T0, 0   },   /* 2^255 - 21                */
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

X25519_HOT_STEP
int x25519_step(x25519_ctx_t *ctx)
{
#define REG(n) (ctx->r[R_##n])
    switch (ctx->phase) {
    case PH_LADDER:
        /* One Montgomery ladder step (RFC 7748 section 5), one field
         * operation per call. T0..T3 are scratch. The case index is the
         * position in the step, never anything secret. */
        switch (ctx->op) {
        case 0: {
            uint8_t bit = (uint8_t)((ctx->k[ctx->count >> 3] >> (ctx->count & 7u)) & 1u);
            ctx->swap ^= bit;
            fe_cswap(REG(X2), REG(X3), ctx->swap);
            fe_cswap(REG(Z2), REG(Z3), ctx->swap);
            ctx->swap = bit;
            fe_add(REG(T0), REG(X2), REG(Z2));      /* A  = x2 + z2            */
            break;
        }
        case 1:  fe_sub(REG(T1), REG(X2), REG(Z2)); break;   /* B  = x2 - z2            */
        case 2:  fe_add(REG(T2), REG(X3), REG(Z3)); break;   /* C  = x3 + z3            */
        case 3:  fe_sub(REG(T3), REG(X3), REG(Z3)); break;   /* D  = x3 - z3            */
        case 4:  fe_mul(REG(T3), REG(T3), REG(T0)); break;   /* DA = D * A              */
        case 5:  fe_mul(REG(T2), REG(T2), REG(T1)); break;   /* CB = C * B              */
        case 6:  fe_add(REG(X3), REG(T3), REG(T2)); break;   /*      DA + CB            */
        case 7:  fe_sq(REG(X3), REG(X3));           break;   /* x3 = (DA + CB)^2        */
        case 8:  fe_sub(REG(Z3), REG(T3), REG(T2)); break;   /*      DA - CB            */
        case 9:  fe_sq(REG(Z3), REG(Z3));           break;   /*      (DA - CB)^2        */
        case 10: fe_mul(REG(Z3), REG(Z3), REG(X1)); break;   /* z3 = x1 * (DA - CB)^2   */
        case 11: fe_sq(REG(T0), REG(T0));           break;   /* AA = A^2                */
        case 12: fe_sq(REG(T1), REG(T1));           break;   /* BB = B^2                */
        case 13: fe_mul(REG(X2), REG(T0), REG(T1)); break;   /* x2 = AA * BB            */
        case 14: fe_sub(REG(T1), REG(T0), REG(T1)); break;   /* E  = AA - BB            */
        case 15: fe_mul_a24(REG(T2), REG(T1));      break;   /*      a24 * E            */
        case 16: fe_add(REG(T2), REG(T0), REG(T2)); break;   /*      AA + a24 * E       */
        default: fe_mul(REG(Z2), REG(T1), REG(T2)); break;   /* z2 = E * (AA + a24 * E) */
        }
        if (++ctx->op == 18u) {
            ctx->op = 0u;
            if (ctx->count == 0u) {
                /* RFC 7748 swaps once more here, by the last scalar bit
                 * processed. That is bit 0, which the clamp in
                 * x25519_begin() always clears, so the swap is by zero and
                 * is omitted -- it must come back if the clamp ever goes. */
                ctx->count = 0u;          /* squarings done in the current row */
                ctx->phase = PH_INVERT;
            } else {
                ctx->count--;
            }
        }
        return 1;
    case PH_INVERT: {
        const uint8_t *ins = invert_prog[ctx->op];

        if (ins[3] == 0u) {
            fe_mul(ctx->r[ins[0]], ctx->r[ins[1]], ctx->r[ins[2]]);
        } else {
            /* First squaring reads the source; the rest square in place. */
            fe_sq(ctx->r[ins[0]], ctx->count ? ctx->r[ins[0]] : ctx->r[ins[1]]);
            if (++ctx->count < ins[3]) {
                return 1;
            }
            ctx->count = 0u;
        }
        if (++ctx->op == 22u) {
            ctx->phase = PH_FINAL;
        }
        return 1;
    }
    case PH_FINAL:
        fe_mul(REG(X2), REG(X2), REG(T0));
        ctx->phase = PH_DONE;
        return 0;
    default:
        return 0;
    }
#undef REG
}

int x25519_finish(x25519_ctx_t *ctx, uint8_t out[X25519_KEY_BYTES])
{
    uint32_t acc = fe_tobytes(out, ctx->r[R_X2]);

    wipe_words((uint32_t *)(void *)ctx, sizeof(*ctx) / sizeof(uint32_t));
    ctx->phase = PH_DONE;
    return acc == 0u;
}

#ifdef X25519_TEST_HOOKS
/* Host tests only: run one primitive on full-width operands and return the
 * canonical result. alias 0: separate destination; 1: destination is a;
 * 2: destination is a and b is a too. */
void x25519_test_op(int op, int alias, const uint32_t a_in[8], const uint32_t b_in[8],
                    uint8_t out[32])
{
    uint32_t a[8], b[8], r[8];
    uint32_t *dst = alias ? a : r;
    const uint32_t *bp = (alias == 2) ? a : b;
    int i;

    for (i = 0; i < 8; i++) {
        a[i] = a_in[i];
        b[i] = b_in[i];
        r[i] = 0xA5A5A5A5u;
    }
    switch (op) {
    case 0: fe_add(dst, a, bp); break;
    case 1: fe_sub(dst, a, bp); break;
    case 2: fe_mul(dst, a, bp); break;
    case 3: fe_sq(dst, a); break;
    case 4: fe_mul_a24(dst, a); break;
    default: for (i = 0; i < 8; i++) { dst[i] = a[i]; } break;   /* canonicalise only */
    }
    (void)fe_tobytes(out, dst);
}
#endif
