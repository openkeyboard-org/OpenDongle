/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * AES-128-CCM link decryption -- see rf_crypt.h for the wire format and the
 * task-context-only contract. Kept byte-identical to the host reference in
 * tests/ccm_ref.py, which is graded against RFC 3610.
 */

/* Before rf_crypt.h: the per-chip target header is what turns the bench
 * prev-session diagnostic on, and rf_crypt.h only supplies its default. Without
 * this the counters compile out here while iap.c still references them. */
#include "dongle_target.h"

#include "rf_crypt.h"

#include "hal_aes.h"

/* ------------------------------------------------------------------ state */

static uint8_t  rf_crypt_key_ready;
static uint8_t  rf_crypt_session_ready;
static uint32_t rf_crypt_session_id;
static uint32_t rf_crypt_last_ctr;

#if RF_CRYPT_DIAG_PREV_SESSION
/* See rf_crypt.h: bench-only, diagnostic, never gates a forwarding decision. */
static uint32_t rf_crypt_prev_session_id;
static uint8_t  rf_crypt_prev_session_valid;
uint32_t rf_crypt_session_mint_count;
uint32_t rf_crypt_mac_prev_ok;
uint32_t rf_crypt_last_mac_ctr;

uint32_t rf_crypt_mac_same_ok;
uint32_t rf_crypt_same_differs;
volatile uint8_t rf_crypt_in_aes;
uint32_t rf_crypt_bb_during_aes;
uint8_t  rf_crypt_kat_run;
uint8_t  rf_crypt_kat_fail;
volatile uint8_t rf_crypt_fail_latched;
uint8_t  rf_crypt_fail_len;
uint32_t rf_crypt_fail_session;
uint32_t rf_crypt_fail_counter;
uint8_t  rf_crypt_fail_frame[RF_CRYPT_LEN_BOOT_KBD];
uint8_t  rf_crypt_fail_expect1[RF_CRYPT_TAG_BYTES];
uint8_t  rf_crypt_fail_expect2[RF_CRYPT_TAG_BYTES];

/* The KAT must restore the link key afterwards, and hal_aes never lets its
 * cached key back out -- so keep a bench-only copy from install_key. */
static uint8_t rf_crypt_key_copy[RF_CRYPT_KEY_BYTES];
#endif

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
#if RF_CRYPT_DIAG_PREV_SESSION
    {
        uint8_t i;
        for (i = 0; i < RF_CRYPT_KEY_BYTES; i++) {
            rf_crypt_key_copy[i] = key[i];
        }
    }
#endif
    rf_crypt_key_ready = 1u;
    rf_crypt_session_ready = 0u;
    rf_crypt_last_ctr = 0u;
}

void rf_crypt_new_session(uint32_t session_id)
{
#if RF_CRYPT_DIAG_PREV_SESSION
    /* Remember what this mint displaces, but only if a session was actually
     * live -- otherwise the first mint would record the uninitialised 0 as a
     * "previous session" and every later retry would test against garbage. */
    if (rf_crypt_session_ready) {
        rf_crypt_prev_session_id = rf_crypt_session_id;
        rf_crypt_prev_session_valid = 1u;
    }
    rf_crypt_session_mint_count++;
#endif
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
#if RF_CRYPT_DIAG_PREV_SESSION
    {
        uint8_t i;
        for (i = 0; i < RF_CRYPT_KEY_BYTES; i++) {
            rf_crypt_key_copy[i] = 0u;
        }
    }
#endif
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
#if RF_CRYPT_DIAG_PREV_SESSION
        rf_crypt_last_mac_ctr = counter;

        /* Same-session re-verify (TODO.md section 4). Run the FULL pipeline
         * again -- fresh keystream into a private scratch, XOR with the still-
         * valid FIFO bytes, then the tag -- under the UNTOUCHED current-session
         * nonce. Recomputing only the tag over out_body would be blind to a
         * corrupted pass-1 keystream block. Must run BEFORE the prev-session
         * retry below, which overwrites nonce/keystream/expect. Side-effect
         * free: no state advances, the frame is still dropped. */
        {
            uint8_t same_body[RF_CRYPT_MAX_BODY];
            uint8_t same_ks[16];
            uint8_t expect2[RF_CRYPT_TAG_BYTES];

            if (ctr_block(nonce, 1u, same_ks) == HAL_AES_OK) {
                for (i = 0; i < n; i++) {
                    same_body[i] = (uint8_t)(ct[i] ^ same_ks[i]);
                }
                if (ccm_tag(nonce, aad, 2u, same_body, n, expect2) == HAL_AES_OK) {
                    uint8_t diff2 = 0u;
                    uint8_t ddiff = 0u;
                    for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
                        diff2 |= (uint8_t)(expect2[i] ^ tag8[i]);
                        ddiff |= (uint8_t)(expect2[i] ^ expect[i]);
                    }
                    if (diff2 == 0u) {
                        rf_crypt_mac_same_ok++;      /* receiver transient */
                    }
                    if (ddiff != 0u) {
                        rf_crypt_same_differs++;     /* nondeterministic compute */
                    }
                    /* First failure per boot: latch the exact bytes + both
                     * computed tags for the offline ccm_ref.py oracle. */
                    if (!rf_crypt_fail_latched) {
                        uint8_t flen = (uint8_t)(6u + n + RF_CRYPT_TAG_BYTES);
                        rf_crypt_fail_len = flen;
                        rf_crypt_fail_session = rf_crypt_session_id;
                        rf_crypt_fail_counter = counter;
                        for (i = 0; i < flen; i++) {
                            rf_crypt_fail_frame[i] = frame[i];
                        }
                        for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
                            rf_crypt_fail_expect1[i] = expect[i];
                            rf_crypt_fail_expect2[i] = expect2[i];
                        }
                        rf_crypt_fail_latched = 1u;
                    }
                }
            }

            /* One-shot KAT at the first DROP_MAC: FIPS-197 C.1 through the
             * live engine + key-cache path, link key restored afterwards.
             * A wrong answer here is a receiver engine/key fault, full stop. */
            if (!rf_crypt_kat_run) {
                static const uint8_t kat_key[16] = {
                    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
                static const uint8_t kat_pt[16] = {
                    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
                static const uint8_t kat_ct[16] = {
                    0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                    0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
                uint8_t kat_out[16];

                rf_crypt_kat_run = 1u;
                hal_aes_set_key(kat_key);
                if (hal_aes_encrypt_block(kat_pt, kat_out) == HAL_AES_OK) {
                    for (i = 0; i < 16u; i++) {
                        if (kat_out[i] != kat_ct[i]) {
                            rf_crypt_kat_fail = 1u;
                            break;
                        }
                    }
                } else {
                    rf_crypt_kat_fail = 2u;
                }
                hal_aes_set_key(rf_crypt_key_copy);
            }
        }

        if (rf_crypt_prev_session_valid) {
            /* Re-run the WHOLE CCM under the displaced session id. Recomputing
             * only the tag over the plaintext recovered above would be wrong:
             * the session id is part of the nonce, so it changes the CTR
             * keystream too, and the plaintext under the old session is a
             * different string entirely. */
            uint8_t prev_body[RF_CRYPT_MAX_BODY];
            build_nonce(nonce, rf_crypt_prev_session_id,
                        RF_CRYPT_DIR_KB_TO_DONGLE, counter);
            if (ctr_block(nonce, 1u, keystream) == HAL_AES_OK) {
                for (i = 0; i < n; i++) {
                    prev_body[i] = (uint8_t)(ct[i] ^ keystream[i]);
                }
                if (ccm_tag(nonce, aad, 2u, prev_body, n, expect) == HAL_AES_OK) {
                    diff = 0u;
                    for (i = 0; i < RF_CRYPT_TAG_BYTES; i++) {
                        diff |= (uint8_t)(expect[i] ^ tag8[i]);
                    }
                    if (diff == 0u) {
                        rf_crypt_mac_prev_ok++;
                    }
                }
            }
        }
        /* Deliberately fall through to the same drop. The retry observes; it
         * must never rescue a frame, advance rf_crypt_last_ctr, or leave the
         * recovered plaintext where the caller might forward it. */
#endif
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
