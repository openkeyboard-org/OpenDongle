/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * AES-128-CCM link decryption -- see rf_crypt.h for the wire format and the
 * task-context-only contract. Kept byte-identical to the host reference in
 * tests/ccm_ref.py, which is graded against RFC 3610.
 */

#include "rf_crypt.h"

#include "hal_aes.h"

/* ------------------------------------------------------------------ state */

static uint8_t  rf_crypt_key_ready;
static uint8_t  rf_crypt_session_ready;
static uint32_t rf_crypt_session_id;
static uint32_t rf_crypt_last_ctr;

/* ---------------------------------------------------------------- helpers */

static void xor16(uint8_t *acc, const uint8_t *v)
{
    uint8_t i;
    for (i = 0; i < 16u; i++) {
        acc[i] ^= v[i];
    }
}

/* CCM flags byte for the CBC-MAC B0 block: Adata | ((M-2)/2)<<3 | (L-1).
 * M=8, L=2 -> 0x59 with AAD present. */
static uint8_t ccm_b0_flags(uint8_t have_aad)
{
    return (uint8_t)((have_aad ? 0x40u : 0x00u) | (((8u - 2u) / 2u) << 3) | (2u - 1u));
}

static void build_nonce(uint8_t nonce[13], uint32_t session_id,
                        uint8_t direction, uint32_t counter)
{
    nonce[0]  = (uint8_t)session_id;
    nonce[1]  = (uint8_t)(session_id >> 8);
    nonce[2]  = (uint8_t)(session_id >> 16);
    nonce[3]  = (uint8_t)(session_id >> 24);
    nonce[4]  = direction;
    nonce[5]  = (uint8_t)counter;
    nonce[6]  = (uint8_t)(counter >> 8);
    nonce[7]  = (uint8_t)(counter >> 16);
    nonce[8]  = (uint8_t)(counter >> 24);
    nonce[9]  = 0u;
    nonce[10] = 0u;
    nonce[11] = 0u;
    nonce[12] = 0u;
}

/* CTR keystream block A_i for counter index i (L=2). */
static hal_aes_status_t ctr_block(const uint8_t nonce[13], uint16_t i, uint8_t out[16])
{
    uint8_t a[16];
    a[0] = (uint8_t)(2u - 1u);           /* counter-block flags: L'=L-1 */
    for (uint8_t k = 0; k < 13u; k++) {
        a[1 + k] = nonce[k];
    }
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    return hal_aes_encrypt_block(a, out);
}

/* CBC-MAC over B0 || format(AAD) || pad(msg). AAD is <= 14 bytes (one block)
 * and msg <= RF_CRYPT_MAX_BODY (one block); mic_out holds the full 16-byte
 * result. Returns the AES engine status. */
static hal_aes_status_t ccm_cbc_mac(const uint8_t nonce[13],
                                    const uint8_t *aad, uint8_t aad_len,
                                    const uint8_t *msg, uint8_t msg_len,
                                    uint8_t mic_out[16])
{
    uint8_t blk[16];
    uint8_t x[16];
    uint8_t i;

    /* B0 = flags || nonce || l(m) (2-byte big-endian length). */
    blk[0] = ccm_b0_flags(aad_len != 0u);
    for (i = 0; i < 13u; i++) {
        blk[1 + i] = nonce[i];
    }
    blk[14] = (uint8_t)(msg_len >> 8);
    blk[15] = (uint8_t)msg_len;
    if (hal_aes_encrypt_block(blk, x) != HAL_AES_OK) {
        return HAL_AES_ENGINE_TIMEOUT;
    }

    /* AAD block: 2-byte big-endian length prefix, then AAD, zero-padded. */
    if (aad_len != 0u) {
        for (i = 0; i < 16u; i++) {
            blk[i] = 0u;
        }
        blk[0] = (uint8_t)(aad_len >> 8);
        blk[1] = (uint8_t)aad_len;
        for (i = 0; i < aad_len; i++) {
            blk[2 + i] = aad[i];
        }
        xor16(x, blk);
        if (hal_aes_encrypt_block(x, x) != HAL_AES_OK) {
            return HAL_AES_ENGINE_TIMEOUT;
        }
    }

    /* Message block: zero-padded. A zero-length message contributes no block. */
    if (msg_len != 0u) {
        for (i = 0; i < 16u; i++) {
            blk[i] = 0u;
        }
        for (i = 0; i < msg_len; i++) {
            blk[i] = msg[i];
        }
        xor16(x, blk);
        if (hal_aes_encrypt_block(x, x) != HAL_AES_OK) {
            return HAL_AES_ENGINE_TIMEOUT;
        }
    }

    for (i = 0; i < 16u; i++) {
        mic_out[i] = x[i];
    }
    return HAL_AES_OK;
}

/* Produce the 8-byte CCM tag: first M bytes of (CBC-MAC XOR E(A_0)). */
static hal_aes_status_t ccm_tag(const uint8_t nonce[13],
                                const uint8_t *aad, uint8_t aad_len,
                                const uint8_t *msg, uint8_t msg_len,
                                uint8_t tag_out[RF_CRYPT_TAG_BYTES])
{
    uint8_t mic[16];
    uint8_t s0[16];
    uint8_t i;

    if (ccm_cbc_mac(nonce, aad, aad_len, msg, msg_len, mic) != HAL_AES_OK) {
        return HAL_AES_ENGINE_TIMEOUT;
    }
    if (ctr_block(nonce, 0u, s0) != HAL_AES_OK) {
        return HAL_AES_ENGINE_TIMEOUT;
    }
    for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
        tag_out[i] = (uint8_t)(mic[i] ^ s0[i]);
    }
    return HAL_AES_OK;
}

/* ------------------------------------------------------------------- API */

void rf_crypt_init(void)
{
    hal_aes_init();
}

void rf_crypt_install_key(const uint8_t key[RF_CRYPT_KEY_BYTES])
{
    hal_aes_set_key(key);
    rf_crypt_key_ready = 1u;
    rf_crypt_session_ready = 0u;
    rf_crypt_last_ctr = 0u;
}

void rf_crypt_new_session(uint32_t session_id)
{
    rf_crypt_session_id = session_id;
    rf_crypt_last_ctr = 0u;
    rf_crypt_session_ready = 1u;
}

void rf_crypt_clear(void)
{
    static const uint8_t zero_key[RF_CRYPT_KEY_BYTES] = { 0 };
    /* Overwrite the backend's cached key/schedule so the real key does not
     * linger in RAM after a bond is torn down. */
    hal_aes_set_key(zero_key);
    rf_crypt_key_ready = 0u;
    rf_crypt_session_ready = 0u;
    rf_crypt_session_id = 0u;
    rf_crypt_last_ctr = 0u;
}

int rf_crypt_active(void)
{
    return rf_crypt_key_ready && rf_crypt_session_ready;
}

uint8_t rf_crypt_encrypted_body_len(uint8_t tag, uint8_t len)
{
    switch (len) {
    case RF_CRYPT_LEN_CONSUMER:
        return (tag == RF_PROTO_HID_TAG_CONSUMER) ? 2u : 0u;
    case RF_CRYPT_LEN_MOUSE:
        return (tag == RF_PROTO_HID_TAG_MOUSE) ? 5u : 0u;
    case RF_CRYPT_LEN_BOOT_KBD:
        return (tag == RF_PROTO_HID_TAG) ? 8u : 0u;
    default:
        return 0u;
    }
}

rf_crypt_status_t rf_crypt_rx(const uint8_t *frame, uint8_t len,
                              uint8_t *out_tag, uint8_t *out_body, uint8_t *out_n)
{
    uint8_t ctrl;
    uint8_t tag;
    uint8_t n;
    uint32_t counter;
    const uint8_t *ct;
    const uint8_t *tag8;
    uint8_t aad[2];
    uint8_t nonce[13];
    uint8_t keystream[16];
    uint8_t expect[RF_CRYPT_TAG_BYTES];
    uint8_t diff;
    uint8_t i;

    if (!rf_crypt_active()) {
        return RF_CRYPT_DROP_INACTIVE;
    }

    ctrl = frame[0];
    tag = frame[1];
    n = rf_crypt_encrypted_body_len(tag, len);
    if (n == 0u || n > RF_CRYPT_MAX_BODY) {
        return RF_CRYPT_DROP_SHAPE;
    }

    counter = (uint32_t)frame[2] | ((uint32_t)frame[3] << 8)
            | ((uint32_t)frame[4] << 16) | ((uint32_t)frame[5] << 24);
    ct = &frame[6];
    tag8 = &frame[6 + n];

    /* Cheap replay reject on the clear counter. A tampered counter changes the
     * nonce and so fails the tag below, and this never advances state, so it is
     * safe to short-circuit here. Counter 0 is reserved (never transmitted). */
    if (counter == 0u || counter <= rf_crypt_last_ctr) {
        return RF_CRYPT_DROP_REPLAY;
    }

    build_nonce(nonce, rf_crypt_session_id, RF_CRYPT_DIR_KB_TO_DONGLE, counter);
    aad[0] = ctrl;
    aad[1] = tag;

    /* Recover the plaintext (needed for the MAC) with the CTR keystream. */
    if (ctr_block(nonce, 1u, keystream) != HAL_AES_OK) {
        return RF_CRYPT_FAULT_ENGINE;
    }
    for (i = 0; i < n; i++) {
        out_body[i] = (uint8_t)(ct[i] ^ keystream[i]);
    }

    /* Authenticate: recompute the tag over the recovered plaintext + AAD. */
    if (ccm_tag(nonce, aad, 2u, out_body, n, expect) != HAL_AES_OK) {
        return RF_CRYPT_FAULT_ENGINE;
    }
    diff = 0u;
    for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
        diff |= (uint8_t)(expect[i] ^ tag8[i]);
    }
    if (diff != 0u) {
        return RF_CRYPT_DROP_MAC;   /* out_body holds unverified bytes; caller drops */
    }

    rf_crypt_last_ctr = counter;
    *out_tag = tag;
    *out_n = n;
    return RF_CRYPT_OK;
}

rf_crypt_status_t rf_crypt_build_session_frame(uint8_t ctrl, uint8_t *out)
{
    uint8_t nonce[13];
    uint8_t aad[2];
    uint8_t tag8[RF_CRYPT_TAG_BYTES];
    uint8_t i;

    if (!rf_crypt_active()) {
        return RF_CRYPT_DROP_INACTIVE;
    }

    /* Direction 0x02, counter 0: disjoint from every kb->dongle data nonce
     * (different direction byte) and unique across sessions (session_id). */
    build_nonce(nonce, rf_crypt_session_id, RF_CRYPT_DIR_DONGLE_TO_KB, 0u);
    aad[0] = ctrl;
    aad[1] = RF_CRYPT_TAG_SESSION;
    if (ccm_tag(nonce, aad, 2u, (const uint8_t *)0, 0u, tag8) != HAL_AES_OK) {
        return RF_CRYPT_FAULT_ENGINE;
    }

    out[0] = ctrl;
    out[1] = RF_CRYPT_TAG_SESSION;
    out[2] = (uint8_t)rf_crypt_session_id;
    out[3] = (uint8_t)(rf_crypt_session_id >> 8);
    out[4] = (uint8_t)(rf_crypt_session_id >> 16);
    out[5] = (uint8_t)(rf_crypt_session_id >> 24);
    for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
        out[6 + i] = tag8[i];
    }
    return RF_CRYPT_OK;
}
