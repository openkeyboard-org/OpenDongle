/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Link-key exchange, protocol core (docs/key-establishment.md sections 4-7,
 * the minimal subset: PUB, CONFIRM, ABORT; no rekey, no continuity field).
 *
 * One state machine for both ends, with no radio, clock or storage of its
 * own. The port:
 *   - calls kex_start() with a fresh private scalar, the transcript prefix
 *     and both MAC addresses, once the link is CONNECTED;
 *   - hands every received KX payload to kex_rx();
 *   - asks kex_tx() once per transmit slot whether a KX frame should go out
 *     in place of the poll (dongle) or of the response (keyboard);
 *   - calls kex_work() from task context with a step budget that fits
 *     between two polls;
 *   - watches kex_state() and calls kex_expire() when its deadline passes.
 *
 * Message flow (fragments repeat round-robin until the peer's next message
 * shows they arrived; there are no acknowledgements or sequence numbers):
 *   dongle   -> keyboard   PUB     4 fragments, its X25519 public key
 *   keyboard -> dongle     PUB     4 fragments
 *   both                   shared secret, keys from secret + transcript
 *   dongle   -> keyboard   CONFIRM 2 fragments, CMAC under K_cd
 *   keyboard -> dongle     CONFIRM 2 fragments, CMAC under K_ck, sent only
 *                                  after the dongle's CONFIRM verified
 * The dongle is DONE when the keyboard's CONFIRM verifies. The keyboard is
 * DONE when the dongle's CONFIRM verifies, and keeps repeating its own
 * CONFIRM until the port stops asking (it sees the first sealed session).
 *
 * The link key is released only through kex_take_link_key(), once, in DONE.
 *
 * Shared verbatim with OpenController (the keyboard end).
 */
#ifndef KEX_PROTO_H
#define KEX_PROTO_H

#include <stdint.h>

#include "kex_kdf.h"
#include "x25519.h"

/* On-air KX payload as this module sees it: [hdr][chunk:8]. The port puts
 * [ctrl][tag] in front (tag 0xA7 keyboard->dongle, 0xA9 dongle->keyboard),
 * which makes the LEN-11 frame of design section 4. */
#define KEX_PAYLOAD_BYTES 9u
#define KEX_TAG_UP        0xA7u
#define KEX_TAG_DOWN      0xA9u
#define KEX_FRAME_LEN     11u
#define KEX_VERSION       1u

/* hdr = mmmm ffvv */
#define KEX_MSG_PUB     1u
#define KEX_MSG_CONFIRM 2u
#define KEX_MSG_ABORT   3u

/* Host entropy seed. One packet, the same bytes on both transports: the
 * dongle takes it as an IAP command on its USB interface, the keyboard's
 * radio module takes it from its host MCU on the UART.
 *   [KEX_SEED_CMD][KEX_SEED_BYTES][seed:32][sum of every byte before it]
 * The port folds the seed into its entropy pool at the next private-key
 * draw. It adds to what the pool collected and never replaces it, so a seed
 * an attacker knows costs nothing; without one the key rests on the pool
 * alone (best effort). */
#define KEX_SEED_CMD      0x9Cu
#define KEX_SEED_BYTES    32u
#define KEX_SEED_PACKET   (KEX_SEED_BYTES + 3u)

/* Transcript prefix, supplied by the port, identical bytes on both ends:
 *   "OKX1"(4) || 0xA6 || cap_version (2) || beacon, ctrl zeroed (10)
 *   || pair-ACK, ctrl zeroed (15)
 * The module appends D_pub(32) || K_pub(32) || cont(8, zero in this subset). */
#define KEX_T_PREFIX_BYTES 31u
#define KEX_T_BYTES        (KEX_T_PREFIX_BYTES + 32u + 32u + 8u)

typedef enum {
    KEX_IDLE = 0,
    KEX_KEYGEN,        /* computing own public key                          */
    KEX_PUB,           /* exchanging public keys                            */
    KEX_DH,            /* computing the shared secret                       */
    KEX_CONFIRM,       /* exchanging confirmations                          */
    KEX_DONE,          /* link key available                                */
    KEX_FAILED
} kex_state_t;

typedef enum {
    KEX_FAIL_NONE = 0,
    KEX_FAIL_LOW_ORDER,   /* peer public key gave an all-zero secret       */
    KEX_FAIL_ENGINE,      /* AES engine fault during derivation            */
    KEX_FAIL_CONFIRM,     /* peer's confirmation did not verify            */
    KEX_FAIL_PEER_ABORT,  /* peer sent ABORT                               */
    KEX_FAIL_TIMEOUT      /* the port's deadline passed                    */
} kex_fail_t;

typedef struct {
    x25519_ctx_t x;
    uint8_t  priv[32];
    uint8_t  own_pub[32];
    uint8_t  peer_pub[32];
    uint8_t  own_conf[16];
    uint8_t  peer_conf[16];
    uint8_t  expect_conf[16];
    uint8_t  lk[16];
    uint8_t  t_prefix[KEX_T_PREFIX_BYTES];
    uint8_t  own_mac[6];
    uint8_t  peer_mac[6];
    uint8_t  role;              /* KEX_ROLE_DONGLE / KEX_ROLE_KEYBOARD      */
    uint8_t  state;             /* kex_state_t                              */
    uint8_t  fail;              /* kex_fail_t                               */
    uint8_t  peer_pub_mask;     /* fragments received, bits 0..3            */
    uint8_t  peer_conf_mask;    /* fragments received, bits 0..1            */
    uint8_t  peer_conf_ok;      /* peer's CONFIRM verified                  */
    uint8_t  keys_ready;        /* own_conf / expect_conf / lk are valid    */
    uint8_t  x_busy;            /* a scalar multiplication is in progress   */
    uint8_t  tx_next;           /* round-robin fragment index               */
    uint8_t  abort_tx;          /* ABORT frames still to send               */
    uint8_t  lk_taken;
} kex_t;

/* Begin an exchange. `priv` must come from a real entropy source; it is
 * copied and the copy is wiped when the exchange ends either way. */
void kex_start(kex_t *k, uint8_t role, const uint8_t priv[32],
               const uint8_t t_prefix[KEX_T_PREFIX_BYTES],
               const uint8_t own_mac[6], const uint8_t peer_mac[6]);

/* Task context: do up to `steps` units of work (one X25519 field operation
 * each; the key derivation counts as one). Returns the state. */
kex_state_t kex_work(kex_t *k, unsigned steps);

/* A KX payload arrived from the peer. Safe to call in any state. */
void kex_rx(kex_t *k, const uint8_t payload[KEX_PAYLOAD_BYTES]);

/* A transmit slot is available. Returns 1 and fills `payload` when a KX frame
 * should use it, 0 when the port should send what it normally would. */
int kex_tx(kex_t *k, uint8_t payload[KEX_PAYLOAD_BYTES]);

/* The connection the exchange was running on ended and a new one began. An
 * end still generating its key pair, or that has it and has not yet finished
 * the shared secret, keeps the key pair and forgets everything received from
 * the peer, so the expensive half is not redone. Returns 0 when the exchange
 * can continue on the new connection, nonzero when it cannot (the caller
 * should kex_reset() and start afresh). */
int kex_rewind(kex_t *k);

/* The port's deadline passed: fail with KEX_FAIL_TIMEOUT unless already
 * DONE. */
void kex_expire(kex_t *k);

/* Stop and wipe everything, whatever the state. */
void kex_reset(kex_t *k);

static inline kex_state_t kex_state(const kex_t *k) { return (kex_state_t)k->state; }
static inline kex_fail_t  kex_failure(const kex_t *k) { return (kex_fail_t)k->fail; }

/* DONE only, once: copy the link key out and wipe the module's copy.
 * Returns 0 on success, nonzero if there is no key to take. */
int kex_take_link_key(kex_t *k, uint8_t out[16]);

#endif /* KEX_PROTO_H */
