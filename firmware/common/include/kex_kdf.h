/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Key-exchange symmetric half: AES-128-CMAC (RFC 4493), the SP 800-108
 * counter-mode KDF over it, and the derivations of docs/key-establishment.md
 * section 3. No hash function: everything is built on the AES block the link
 * already has.
 *
 * The AES itself is the port's: it supplies kex_aes_set_key() and
 * kex_aes_block(), so each end routes these blocks through whatever hardening
 * its engine needs (the CH592 double-compute). Setting the key here REPLACES
 * the engine's key; the caller reinstalls the link key afterwards.
 *
 * INVARIANT (design section 3): CMAC is only ever computed under a SECRET
 * key. Compressing the transcript under a public or fixed key would be
 * collision-forgeable. The one fixed-key call, the extract step, takes the
 * secret as its MESSAGE, which is the SP 800-56C construction.
 *
 * Shared verbatim with OpenController (the keyboard end).
 */
#ifndef KEX_KDF_H
#define KEX_KDF_H

#include <stddef.h>
#include <stdint.h>

/* Port hooks. Both return 0 on success, nonzero on an engine fault; a fault
 * aborts the derivation and nothing derived so far may be used. */
int kex_aes_set_key(const uint8_t key[16]);
int kex_aes_block(const uint8_t in[16], uint8_t out[16]);

/* out = CMAC(key, msg[0..len)). Returns 0 on success. */
int kex_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16]);

/* out = CMAC(key, 0x01 || label[2] || 0x00 || ctx || 0x00 0x80): one 128-bit
 * block of SP 800-108 counter mode. ctx_len <= KEX_KDF_MAX_CTX. */
#define KEX_KDF_MAX_CTX 16u
int kex_kdf(const uint8_t key[16], const char label[2],
            const uint8_t *ctx, size_t ctx_len, uint8_t out[16]);

typedef struct {
    uint8_t k_cd[16];   /* dongle's confirmation key   */
    uint8_t k_ck[16];   /* keyboard's confirmation key */
    uint8_t lk[16];     /* the link key                */
} kex_keys_t;

/* From the X25519 shared secret z and the transcript t:
 *   PRK = CMAC(0^16, z)        T_tag = CMAC(PRK, t)
 *   K_cd = KDF(PRK, "CD", T_tag)   K_ck = KDF(PRK, "CK", T_tag)
 *   LK   = KDF(PRK, "LK", T_tag)
 * Returns 0 on success; on failure *out is zeroed. PRK and T_tag do not
 * leave this function. */
int kex_derive(const uint8_t z[32], const uint8_t *t, size_t t_len, kex_keys_t *out);

/* Confirmation MAC: CMAC(k_confirm, role || mac_a[6] || mac_b[6]).
 * Dongle:   role 0x44, mac_a = dongle MAC,   mac_b = keyboard MAC, key K_cd.
 * Keyboard: role 0x4B, mac_a = keyboard MAC, mac_b = dongle MAC,   key K_ck. */
#define KEX_ROLE_DONGLE   0x44u
#define KEX_ROLE_KEYBOARD 0x4Bu
int kex_confirm(const uint8_t k_confirm[16], uint8_t role,
                const uint8_t mac_a[6], const uint8_t mac_b[6], uint8_t out[16]);

/* Constant-time comparison; returns 0 when the n bytes are equal. */
uint8_t kex_ct_diff(const uint8_t *a, const uint8_t *b, size_t n);

#endif /* KEX_KDF_H */
