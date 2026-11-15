/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * AES-128-CMAC, the CMAC KDF and the key-exchange derivations -- see kex_kdf.h.
 */
#include "kex_kdf.h"

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;

    while (n--) {
        *v++ = 0u;
    }
}

/* Multiply by x in GF(2^128), big-endian, reduction constant 0x87 (RFC 4493
 * subkey generation). The conditional XOR is a mask, not a branch: the value
 * is derived from the key. */
static void gf_double(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    int i;

    for (i = 0; i < 15; i++) {
        b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    }
    b[15] = (uint8_t)((b[15] << 1) ^ ((uint8_t)(0u - carry) & 0x87u));
}

int kex_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16])
{
    uint8_t k[16], x[16], y[16];
    size_t blocks, i, j, last_len;
    int rc = 1;

    if (kex_aes_set_key(key) != 0) {
        goto done;
    }
    for (i = 0; i < 16; i++) {
        x[i] = 0u;
    }
    if (kex_aes_block(x, k) != 0) {      /* L = E(K, 0^128) */
        goto done;
    }
    gf_double(k);                        /* K1 */

    blocks = (len + 15u) / 16u;
    if (blocks == 0u) {
        blocks = 1u;
    }
    last_len = len - (blocks - 1u) * 16u;   /* 0 only for the empty message */
    for (i = 0; i + 1u < blocks; i++) {
        for (j = 0; j < 16; j++) {
            y[j] = (uint8_t)(x[j] ^ msg[16u * i + j]);
        }
        if (kex_aes_block(y, x) != 0) {
            goto done;
        }
    }
    /* Last block: complete -> XOR K1; short or empty -> pad 10* and XOR K2.
     * Message length is public, so this branch is too. */
    if (last_len != 16u) {
        gf_double(k);                    /* K2 */
    }
    for (j = 0; j < 16; j++) {
        uint8_t m = 0u;
        if (j < last_len) {
            m = msg[16u * (blocks - 1u) + j];
        } else if (j == last_len) {
            m = 0x80u;
        }
        y[j] = (uint8_t)(x[j] ^ m ^ k[j]);
    }
    if (kex_aes_block(y, out) != 0) {
        goto done;
    }
    rc = 0;
done:
    wipe(k, sizeof(k));
    wipe(x, sizeof(x));
    wipe(y, sizeof(y));
    if (rc != 0) {
        wipe(out, 16);
    }
    return rc;
}

int kex_kdf(const uint8_t key[16], const char label[2],
            const uint8_t *ctx, size_t ctx_len, uint8_t out[16])
{
    uint8_t m[1u + 2u + 1u + KEX_KDF_MAX_CTX + 2u];
    size_t n = 0, i;
    int rc;

    if (ctx_len > KEX_KDF_MAX_CTX) {
        wipe(out, 16);
        return 1;
    }
    m[n++] = 0x01u;                      /* counter i = 1 */
    m[n++] = (uint8_t)label[0];
    m[n++] = (uint8_t)label[1];
    m[n++] = 0x00u;                      /* separator */
    for (i = 0; i < ctx_len; i++) {
        m[n++] = ctx[i];
    }
    m[n++] = 0x00u;                      /* [L]2 = 128 bits, big-endian */
    m[n++] = 0x80u;
    rc = kex_cmac(key, m, n, out);
    wipe(m, sizeof(m));
    return rc;
}

int kex_derive(const uint8_t z[32], const uint8_t *t, size_t t_len, kex_keys_t *out)
{
    static const uint8_t zero_salt[16] = { 0 };
    uint8_t prk[16], t_tag[16];
    int rc = 1;

    if (kex_cmac(zero_salt, z, 32u, prk) != 0) {
        goto done;
    }
    if (kex_cmac(prk, t, t_len, t_tag) != 0) {
        goto done;
    }
    if (kex_kdf(prk, "CD", t_tag, 16u, out->k_cd) != 0
        || kex_kdf(prk, "CK", t_tag, 16u, out->k_ck) != 0
        || kex_kdf(prk, "LK", t_tag, 16u, out->lk) != 0) {
        goto done;
    }
    rc = 0;
done:
    wipe(prk, sizeof(prk));
    wipe(t_tag, sizeof(t_tag));
    if (rc != 0) {
        wipe(out, sizeof(*out));
    }
    return rc;
}

int kex_confirm(const uint8_t k_confirm[16], uint8_t role,
                const uint8_t mac_a[6], const uint8_t mac_b[6], uint8_t out[16])
{
    uint8_t m[13];
    int i;

    m[0] = role;
    for (i = 0; i < 6; i++) {
        m[1 + i] = mac_a[i];
        m[7 + i] = mac_b[i];
    }
    return kex_cmac(k_confirm, m, sizeof(m), out);
}

uint8_t kex_ct_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0u;

    while (n--) {
        d |= (uint8_t)(*a++ ^ *b++);
    }
    return d;
}
