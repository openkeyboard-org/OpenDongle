/*
 * OpenKeyboard.org OpenDongle -- software AES-128 forward cipher.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Encrypt-only AES-128 per FIPS-197, used by the CH570 hal_aes backend because
 * that silicon has no hardware AES engine (see hal_aes.h for the measurement).
 * Deliberately the compact byte-oriented form rather than a T-table: one
 * 256-byte S-box in flash and a 176-byte round-key schedule in RAM, against a
 * 12 KB SRAM budget and an 875 us poll slot that this comfortably fits.
 *
 * There is no inverse cipher here on purpose — counter mode encrypts in both
 * directions, so the inverse S-box and InvMixColumns would be dead weight.
 *
 * State layout is FIPS-197 column-major, which is also plain input order:
 * byte i of the input is row (i mod 4), column (i / 4). So a column is
 * s[4c..4c+3] and a row is s[r], s[r+4], s[r+8], s[r+12]. Keeping the state in
 * input order means AddRoundKey is a flat 16-byte XOR with no permutation.
 */
#include "aes_sw.h"

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16,
};

/* Round constants for the ten AES-128 key-expansion rounds. */
static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                 0x20, 0x40, 0x80, 0x1b, 0x36};

/* Multiply by x in GF(2^8) modulo the AES polynomial 0x11b. */
static uint8_t xtime(uint8_t a)
{
    return (uint8_t)((uint8_t)(a << 1) ^ (uint8_t)((a >> 7) * 0x1bu));
}

void aes_sw_expand_key(aes_sw_ctx_t *ctx, const uint8_t key[16])
{
    uint8_t *rk = ctx->rk;

    for (uint8_t i = 0; i < 16; i++)
        rk[i] = key[i];

    /* Words 4..43. Each word is 4 bytes; word i lives at rk[4i..4i+3]. */
    for (uint8_t i = 4; i < 44; i++) {
        uint8_t t[4];
        uint8_t p = (uint8_t)((i - 1) * 4);

        t[0] = rk[p + 0];
        t[1] = rk[p + 1];
        t[2] = rk[p + 2];
        t[3] = rk[p + 3];

        if ((i & 3u) == 0u) {
            /* RotWord, SubWord, then XOR the round constant into byte 0. */
            uint8_t tmp = t[0];
            t[0] = sbox[t[1]];
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[tmp];
            t[0] ^= rcon[(i >> 2) - 1];
        }

        uint8_t q = (uint8_t)((i - 4) * 4);
        uint8_t d = (uint8_t)(i * 4);
        rk[d + 0] = (uint8_t)(rk[q + 0] ^ t[0]);
        rk[d + 1] = (uint8_t)(rk[q + 1] ^ t[1]);
        rk[d + 2] = (uint8_t)(rk[q + 2] ^ t[2]);
        rk[d + 3] = (uint8_t)(rk[q + 3] ^ t[3]);
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    for (uint8_t i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

static void sub_bytes(uint8_t s[16])
{
    for (uint8_t i = 0; i < 16; i++)
        s[i] = sbox[s[i]];
}

/* Row r rotates left by r. Row r is s[r], s[r+4], s[r+8], s[r+12]. */
static void shift_rows(uint8_t s[16])
{
    uint8_t t;

    t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;

    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;

    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;
}

static void mix_columns(uint8_t s[16])
{
    for (uint8_t c = 0; c < 16; c += 4) {
        uint8_t a0 = s[c + 0], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
        uint8_t sum = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

        s[c + 0] ^= (uint8_t)(sum ^ xtime((uint8_t)(a0 ^ a1)));
        s[c + 1] ^= (uint8_t)(sum ^ xtime((uint8_t)(a1 ^ a2)));
        s[c + 2] ^= (uint8_t)(sum ^ xtime((uint8_t)(a2 ^ a3)));
        s[c + 3] ^= (uint8_t)(sum ^ xtime((uint8_t)(a3 ^ a0)));
    }
}

void aes_sw_encrypt_block(const aes_sw_ctx_t *ctx, const uint8_t in[16],
                          uint8_t out[16])
{
    uint8_t s[16];

    /* Work in a local so `out` may alias `in`. */
    for (uint8_t i = 0; i < 16; i++)
        s[i] = in[i];

    add_round_key(s, &ctx->rk[0]);

    for (uint8_t round = 1; round < 10; round++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, &ctx->rk[round * 16]);
    }

    /* Final round omits MixColumns. */
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, &ctx->rk[160]);

    for (uint8_t i = 0; i < 16; i++)
        out[i] = s[i];
}
