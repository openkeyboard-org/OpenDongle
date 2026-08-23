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

/* Capability advertisement (keyboard->dongle, at pairing): [ctrl][tag][version].
 * Purely additive -- a stock keyboard never sends it, so pairing is unchanged and
 * the bond stays plaintext. Unauthenticated in this phase (no key exists yet at
 * pairing); the future key-establishment handshake authenticates it. */
#define RF_CRYPT_TAG_CAP       0xA6u
#define RF_CRYPT_LEN_CAP        3u
#define RF_CRYPT_CAP_VERSION    1u

/* Beacon-accept policy for the two pairing-time crypto latches, factored out
 * so the ordering contract is host-testable (tests/test_rf_negotiation.py):
 *
 *   - bond_enc (encryption REQUIRED): drops for a fresh dongle or a peer
 *     change, so a new keyless bond cannot inherit the previous bond's key
 *     requirement. A same-peer accept keeps it -- a reconnect can never
 *     downgrade an encrypted bond to plaintext.
 *   - peer_capable: preserved across every accept, so the advert this pairing
 *     latched is the one that gets persisted. Its only resets are boot and
 *     tombstone; see the latch declaration in rf_task.c for the full scoping
 *     argument.
 *
 *     Why it must stay that way: the advert is anonymous and arrives just
 *     before the beacon we accept, so a clear placed on the accept path WOULD
 *     erase what the advert had just set, and the capability would never be
 *     persisted. That clear existed once and did exactly that -- the 2026-08-16
 *     review's finding 3. Do not reintroduce it.
 *
 *     The "arrives just before" half is only true because the keyboard leads
 *     EVERY beacon with an advert on the same channel (since 2026-08-22). Under
 *     the older slots-0,1-then-one-in-eight schedule it held only for a dongle
 *     camped from the keyboard's very first slot, which is why capability
 *     latched 0/10 in the documented pairing order (OC-01).
 *
 * The unused parameter is the contract: whoever edits this signature is
 * looking at the one place an accept-time clear of the capability latch
 * would have to go, and the comment above is what stops them. */
static inline void rf_crypt_beacon_accept_latches(uint8_t bond_valid,
                                                  uint8_t same_peer,
                                                  volatile uint8_t *bond_enc,
                                                  const uint8_t *peer_capable)
{
    (void)peer_capable;
    if (!bond_valid || !same_peer) {
        *bond_enc = 0u;
    }
}

/* --------------------------------------------- authenticated-liveness guard
 *
 * How long an active encrypted bond may go with nothing authenticating before
 * the dongle releases it. Two pure inlines so the policy and the arithmetic are
 * host-testable (tests/test_rf_crypt_silence_guard.py); the caller owns the
 * "armed" state and the clock.
 *
 * This replaced a fixed 64-RECEPTION budget that released 7 of 7 healthy keyed
 * links on the bench (2026-08-23). A reception count is the wrong unit: it was
 * spent by the keyboard's own bare poll acks (~975/s) but refilled only by an
 * authenticated frame, which the keyboard schedules against its OWN reception
 * count -- and it made the deadline depend on how fast the peer, or an
 * attacker, transmitted. See the commentary in rf_task.c.
 */

/* Must track OpenController's KBD_CRYPT_KEEPALIVE_POLLS. The keyboard puts an
 * authenticated frame on air every this-many receptions; the dongle's deadline
 * is derived from it, so the two cannot be reasoned about separately. */
#define RF_CRYPT_KEEPALIVE_POLLS      32u
/* Consecutive missed keepalives tolerated before releasing.
 *
 * 15 was chosen against the ~37% per-frame shortfall measured on the bench
 * (unexplained; tracked separately): treating misses as independent puts the
 * false-release rate at 0.37^15. That independence is an ASSUMPTION and very
 * likely optimistic -- RF loss and executor stalls are usually burst
 * correlated -- so the number is a starting point justified by soak evidence,
 * not by that arithmetic. Raise it if a soak ever shows a false release; the
 * only cost is a longer bound on an attacker-held key-down. */
#define RF_CRYPT_SILENCE_KEEPALIVES   15u
/* Protocol ticks are 1/32000 s. The floor keeps a fast link from inheriting a
 * deadline barely above the keepalive; the ceiling is what actually bounds an
 * attacker-held key-down, and is the reason this is not simply proportional. */
#define RF_CRYPT_SILENCE_MIN_TICKS 16000u   /* 500 ms */
#define RF_CRYPT_SILENCE_MAX_TICKS 96000u   /* 3 s   */

/* Deadline in protocol ticks for a given connection interval.
 *
 * It MUST scale with the interval. A fixed 500 ms is ~15 keepalives at the
 * stock interval of 28 (875 us/poll) but under TWO at the interval of 300 that
 * bond_record_semantic_valid() still accepts -- which is precisely the original
 * defect, just at a slower cadence.
 *
 * DOMAIN: intervals the bond validator admits, 1..300. Across that range the
 * ceiling still leaves >= 10 keepalives of headroom. The cap makes headroom
 * fall with interval, so far ABOVE the validated range (past ~750) it would
 * drop under four keepalives; that is acceptable only because
 * bond_record_semantic_valid() is the sole source of this argument. Widen the
 * validator and this needs revisiting. */
static inline uint32_t rf_crypt_silence_deadline_ticks(uint16_t conn_interval)
{
    uint32_t d = (uint32_t)conn_interval * RF_CRYPT_KEEPALIVE_POLLS
                 * RF_CRYPT_SILENCE_KEEPALIVES;

    if (d < RF_CRYPT_SILENCE_MIN_TICKS) {
        d = RF_CRYPT_SILENCE_MIN_TICKS;
    } else if (d > RF_CRYPT_SILENCE_MAX_TICKS) {
        d = RF_CRYPT_SILENCE_MAX_TICKS;
    }
    return d;
}

/* Has the deadline passed? Pure arithmetic -- the caller decides whether the
 * guard is armed at all, because a zero timestamp is a legitimate clock value
 * and must not double as a sentinel for security state.
 *
 *   - the subtraction is deliberately 2^32-modular, so it stays correct across
 *     a clock wrap; the same idiom the supervision lapse check uses.
 *   - now < stamp happens for real: the radio IRQ sink can refresh the stamp
 *     between the caller's stamp load and its clock read. That must read as
 *     "just authenticated", NOT underflow to ~2^32 and force-release a healthy
 *     link. Callers load the stamp first to narrow the window; this rejects the
 *     case outright rather than relying on caller discipline alone. */
static inline int rf_crypt_silence_expired(uint32_t now_tsys,
                                           uint32_t stamp_tsys,
                                           uint32_t deadline_tsys)
{
    uint32_t idle = now_tsys - stamp_tsys;

    /* Treat anything past the half-range as a stamp from the future rather than
     * a very idle link. That disambiguation is a CHOICE, not a proof: with
     * 32-bit modular timestamps the two cases are genuinely indistinguishable
     * (stamp=1, now=0x80000001 is a real half-wrap and reads as fresh here).
     * It is sound only because the caller evaluates this every connection
     * interval -- milliseconds -- so a live link can never accumulate half a
     * wrap unobserved, while the IRQ-sink preemption it does catch is a
     * routine, every-connection event. A caller that polled it rarely would
     * need a wider counter instead. */
    if (idle >= 0x80000000u) {
        return 0;
    }
    return idle >= deadline_tsys;
}

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

/* Bench-only: on a MAC failure, retry the whole CCM under the session id the
 * most recent mint displaced, and count the frames that would have verified
 * under it. That is the direct test of "the keyboard is sealing under a stale
 * session", which is otherwise indistinguishable from a genuine tag failure --
 * both land in DROP_MAC and nothing else moves.
 *
 * OFF by default and enabled only for the CH592 bench target. It costs one
 * RF_CRYPT_MAX_BODY scratch buffer plus a second full decrypt inside
 * rf_crypt_rx(), and the CH570 build sits against its stack-floor assert with
 * only ~20 bytes of margin, so it must not be enabled there without redoing
 * that budget.
 *
 * The retry is diagnostic ONLY: it never advances the replay high-water mark,
 * never touches the current session, and never changes what rf_crypt_rx()
 * returns or what the caller forwards. A frame that fails under the live
 * session is still dropped, fail-closed, exactly as before. */
#ifndef RF_CRYPT_DIAG_PREV_SESSION
#define RF_CRYPT_DIAG_PREV_SESSION 0
#endif

/* Bench-only (see dongle_target.h): force-activate link decryption for any
 * valid loaded bond using a compiled-in throwaway key, because this bench has
 * no path to the USB provisioning tool. Must never ship enabled. */
#ifndef DONGLE_CRYPT_BENCH_FORCE_KEY
#define DONGLE_CRYPT_BENCH_FORCE_KEY 0
#endif

/* Stale-output-abort hardening for a shared hardware AES engine, PRODUCT
 * path (CH592 sets both; the CH570 software backends cannot abort and pay
 * nothing). The CH592 engine lives on the BLE baseband's register cluster:
 * when a BLEB interrupt preempts a block operation the engine silently
 * ABORTS -- the busy bit reads back complete while the output latch still
 * holds the PREVIOUS block's result. On the keyboard's seal path that was
 * the 12.3% MAC-failure campaign (2026-08); the fix that closed it to
 * 0/20k+ there is the same one gated here: compute every block twice and
 * compare, because a stale abort cannot survive an honest recompute. Do NOT
 * reach for mstatus.MIE masking instead -- tried on both ends, hung both
 * (reverted in e89cd44).
 *
 *   RF_CRYPT_AES_DOUBLE  every rf_crypt block runs twice + compare, one
 *                        bounded retry loop, engine-fault on exhaustion;
 *                        rf_crypt_aes_redo counts collisions caught. Also
 *                        arms the announce-seal rebuild retry in rf_task.c
 *                        (a wrongly-tagged announce is otherwise a silent
 *                        dead epoch: it is sent only 8 times and never
 *                        verified locally).
 *   RF_CRYPT_BOOT_KAT    one FIPS-197 C.1 vector through the live engine in
 *                        rf_crypt_init(), before any key is trusted;
 *                        telemetry only (rf_crypt_boot_kat_run/_fail over
 *                        CMD_CRYPT_DIAG) -- an encryption-active bond
 *                        already fails closed on a dead engine. */
#ifndef RF_CRYPT_AES_DOUBLE
#define RF_CRYPT_AES_DOUBLE 0
#endif
#ifndef RF_CRYPT_BOOT_KAT
#define RF_CRYPT_BOOT_KAT 0
#endif

#if RF_CRYPT_DIAG_PREV_SESSION
/* Mints seen. Should be 1 for a single healthy connected epoch; a value that
 * climbs during one run means repeated reconnect/re-promote cycles, which would
 * feed session-boundary MAC failures back into the disconnect that caused them. */
extern uint32_t rf_crypt_session_mint_count;
/* MAC failures that WOULD have verified under the displaced session id. */
extern uint32_t rf_crypt_mac_prev_ok;
/* Clear counter of the most recent MAC failure, for spotting ordering. */
extern uint32_t rf_crypt_last_mac_ctr;

/* ---- same-session re-verify --------------------------------------------
 * (experiment write-up: OpenController firmware/docs/TODO.md section 4 --
 * NOT this repo's TODO.md, which has no numbered sections)
 * On each DROP_MAC, immediately re-run the FULL CCM (fresh keystream into a
 * private scratch, then the tag) under the SAME session/counter/AAD/frame
 * bytes. Side-effect free: it can never rescue the frame or advance state.
 *
 *   mac_same_ok    > 0  => the identical bytes verified on a second attempt:
 *                          receiver-side transient compute failure.
 *   mac_same_ok   == 0  across dozens of failures => the receiver rejects
 *                          deterministically: the fault is upstream (keyboard).
 *   same_differs   > 0  => the retry's computed tag differed from pass 1's
 *                          over identical inputs: direct, per-event proof of
 *                          receiver nondeterminism (independent of the wire). */
extern uint32_t rf_crypt_mac_same_ok;
extern uint32_t rf_crypt_same_differs;

/* Authenticated-HID silence-guard releases, and how long (ms, saturating at
 * 0xFFFF) the link had gone unauthenticated when the last one tripped. The
 * guard and a supervision lapse both leave CONNECTED through
 * rf_enter_stock_reacquire(), which freezes every other counter at the same
 * instant -- so without these a post-mortem cannot attribute the teardown.
 * guard_fires unchanged across a death exonerates the guard; incremented
 * convicts it. */
/* Connected receptions rejected for CRC error or type mismatch. These exit the
 * sink before conn_rx/enc_shape are counted, so without this counter a frame
 * that arrived CORRUPT and one that never arrived look identical. */
extern uint32_t rf_crypt_crc_err;
extern uint16_t rf_crypt_guard_fires;
extern uint16_t rf_crypt_guard_at_fire;

/* BB/LLE interrupts that landed while rf_crypt_rx() was inside its CCM
 * computation (flag set by the executor around the call, counted by the RF
 * status callback). Correlate with DROP_MAC: the vendor ISR touches AES_STA. */
extern volatile uint8_t rf_crypt_in_aes;
extern uint32_t rf_crypt_bb_during_aes;

/* One-shot known-answer test of the cached key/AES engine, run at the first
 * DROP_MAC after boot: kat_run=1 once attempted; kat_fail: 0 ok, 1 wrong
 * ciphertext, 2 engine error. A failure means the receiver engine/key cache
 * is bad, full stop. */
extern uint8_t rf_crypt_kat_run;
extern uint8_t rf_crypt_kat_fail;

/* First-DROP_MAC-per-boot frame latch, for the offline ccm_ref.py oracle:
 * the exact on-air bytes (from ctrl), the live session id, and both computed
 * tags. Latched once, never cleared at runtime -- one fingerprint per boot. */
extern volatile uint8_t rf_crypt_fail_latched;
extern uint8_t  rf_crypt_fail_len;                       /* bytes valid in fail_frame */
extern uint32_t rf_crypt_fail_session;
extern uint32_t rf_crypt_fail_counter;
extern uint8_t  rf_crypt_fail_frame[RF_CRYPT_LEN_BOOT_KBD];
extern uint8_t  rf_crypt_fail_expect1[RF_CRYPT_TAG_BYTES];
extern uint8_t  rf_crypt_fail_expect2[RF_CRYPT_TAG_BYTES];
#endif

#endif /* RF_CRYPT_H */
