/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * AES-128-CCM link decryption for the keyboard->dongle RX path.
 *
 * Built entirely on the forward-cipher hal_aes seam: CCM derives both its
 * CBC-MAC and its CTR keystream by ENCRYPTING, so no inverse cipher is needed.
 * The dongle only ever DECRYPTS keyboard->dongle traffic here (CTR is
 * symmetric), and it authenticates every frame with an 8-byte CCM tag over the
 * clear ctrl/tag header (AAD) so a bit-flipping attacker cannot inject
 * keystrokes.
 *
 * WIRE FORMAT (L=2 CCM, 13-byte nonce, 8-byte tag), bytes from rxBuf[2] onward:
 *
 *   [ctrl][tag][counter:u32 LE][ciphertext(body)][tag8]
 *
 *   nonce = session_id(4 LE) || direction(1) || counter(4 LE) || 0,0,0,0
 *   AAD   = ctrl || tag          (clear, authenticated)
 *
 * session_id and direction are RECEIVER state: the dongle rebuilds the nonce
 * from its own current session_id plus the frame's counter, so a transcript
 * captured under an old session fails the tag after any reconnect/reboot. The
 * counter is a per-session strictly-increasing sequence; the dongle keeps a
 * high-water mark and rejects counter <= last.
 *
 * CONTEXT: task/boot only. hal_aes is not reentrant, so nothing here may be
 * called from the IRQ RX sink -- the sink copies the frame out and posts an
 * event, and the executor calls rf_crypt_rx().
 */
#ifndef RF_CRYPT_H
#define RF_CRYPT_H

#include <stdint.h>

#include "rf_protocol.h"   /* RF_PROTO_HID_TAG* report tags */

#define RF_CRYPT_KEY_BYTES     16u
#define RF_CRYPT_TAG_BYTES      8u   /* CCM MIC, M=8 */
#define RF_CRYPT_CTR_BYTES      4u   /* on-wire per-session counter */
#define RF_CRYPT_MAX_BODY       8u   /* boot-keyboard report: largest encrypted body */

/* Direction byte folded into the nonce (domain-separates the two link halves). */
#define RF_CRYPT_DIR_KB_TO_DONGLE 0x01u
#define RF_CRYPT_DIR_DONGLE_TO_KB 0x02u

/* LEN (rxBuf[1], counted from ctrl) of each encrypted HID frame:
 * ctrl(1)+tag(1)+counter(4)+body(n)+mic(8) = n + 14. New lengths, disjoint from
 * the plaintext set {1,3,4,7,10,15}, so tag+length classification still routes. */
#define RF_CRYPT_LEN_CONSUMER  16u   /* tag 0xA3, body 2 */
#define RF_CRYPT_LEN_MOUSE     19u   /* tag 0xA8, body 5 */
#define RF_CRYPT_LEN_BOOT_KBD  22u   /* tag 0xA1, body 8 */
#define RF_CRYPT_FRAME_OVERHEAD 14u  /* ctrl+tag+counter+mic; body = LEN - this */

/* Session-nonce control frame (dongle->keyboard), authenticated, empty payload:
 * [ctrl][tag][session_id:u32 LE][mic:8]. */
#define RF_CRYPT_TAG_SESSION   0xA5u
#define RF_CRYPT_LEN_SESSION   14u

typedef enum {
    RF_CRYPT_OK = 0,
    RF_CRYPT_DROP_SHAPE,      /* not a well-formed encrypted frame */
    RF_CRYPT_DROP_INACTIVE,   /* no key/session installed */
    RF_CRYPT_DROP_MAC,        /* tag mismatch (forgery, noise, wrong session) */
    RF_CRYPT_DROP_REPLAY,     /* counter <= high-water mark */
    RF_CRYPT_FAULT_ENGINE     /* AES engine wedged -- terminal, see hal_aes.h */
} rf_crypt_status_t;

/* Bring the AES backend up. Safe to call more than once. Task/boot context. */
void rf_crypt_init(void);

/* Install the 16-byte link key (runs the CH570 key schedule once). */
void rf_crypt_install_key(const uint8_t key[RF_CRYPT_KEY_BYTES]);

/* Start a fresh session: adopt session_id and reset the replay high-water mark.
 * Called at each connect/re-key promote for an ACTIVE bond. */
void rf_crypt_new_session(uint32_t session_id);

/* Zeroize the key schedule (via a zero-key set) and clear all module state. */
void rf_crypt_clear(void);

/* Non-zero once a key AND a session are installed. */
int rf_crypt_active(void);

/* If (tag, len) is a valid encrypted HID shape, return the plaintext body length
 * (2/5/8); otherwise 0. */
uint8_t rf_crypt_encrypted_body_len(uint8_t tag, uint8_t len);

/* Verify + decrypt one encrypted HID frame. `frame` points at rxBuf[2] (ctrl),
 * `len` is rxBuf[1] (LEN). On RF_CRYPT_OK, *out_tag / out_body[0..*out_n) hold the
 * decrypted report for rf_hid_callback, and the replay high-water mark has
 * advanced. out_body must be >= RF_CRYPT_MAX_BODY. On any non-OK return nothing
 * is forwarded and the high-water mark is unchanged. */
rf_crypt_status_t rf_crypt_rx(const uint8_t *frame, uint8_t len,
                              uint8_t *out_tag, uint8_t *out_body, uint8_t *out_n);

/* Build the authenticated session-nonce frame for the current session into
 * out[0..RF_CRYPT_LEN_SESSION). Returns RF_CRYPT_FAULT_ENGINE on an engine wedge,
 * RF_CRYPT_DROP_INACTIVE if no key/session, else RF_CRYPT_OK. */
rf_crypt_status_t rf_crypt_build_session_frame(uint8_t ctrl, uint8_t *out);

#endif /* RF_CRYPT_H */
