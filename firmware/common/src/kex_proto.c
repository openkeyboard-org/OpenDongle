/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Link-key exchange, protocol core -- see kex_proto.h.
 */
#include "kex_proto.h"

#define HDR(msg, frag) ((uint8_t)(((msg) << 4) | ((frag) << 2) | KEX_VERSION))
#define ABORT_REPEATS  8u

static void wipe(void *p, unsigned n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;

    while (n--) {
        *v++ = 0u;
    }
}

static void fail(kex_t *k, kex_fail_t why)
{
    uint8_t role = k->role;

    wipe(k, sizeof(*k));
    k->role = role;
    k->state = KEX_FAILED;
    k->fail = (uint8_t)why;
    /* Tell the peer, unless the peer is the one who told us. */
    k->abort_tx = (why == KEX_FAIL_PEER_ABORT) ? 0u : ABORT_REPEATS;
}

void kex_reset(kex_t *k)
{
    wipe(k, sizeof(*k));
}

void kex_start(kex_t *k, uint8_t role, const uint8_t priv[32],
               const uint8_t t_prefix[KEX_T_PREFIX_BYTES],
               const uint8_t own_mac[6], const uint8_t peer_mac[6])
{
    unsigned i;

    wipe(k, sizeof(*k));
    k->role = role;
    for (i = 0; i < 32u; i++) {
        k->priv[i] = priv[i];
    }
    for (i = 0; i < KEX_T_PREFIX_BYTES; i++) {
        k->t_prefix[i] = t_prefix[i];
    }
    for (i = 0; i < 6u; i++) {
        k->own_mac[i] = own_mac[i];
        k->peer_mac[i] = peer_mac[i];
    }
    x25519_begin(&k->x, k->priv, x25519_base_point);
    k->x_busy = 1u;
    k->state = KEX_KEYGEN;
}

/* Shared secret is in z: derive the keys, both confirmations and the link
 * key. Returns 0 on success. */
static int derive(kex_t *k, const uint8_t z[32])
{
    uint8_t t[KEX_T_BYTES];
    kex_keys_t keys;
    const uint8_t *d_pub = (k->role == KEX_ROLE_DONGLE) ? k->own_pub : k->peer_pub;
    const uint8_t *k_pub = (k->role == KEX_ROLE_DONGLE) ? k->peer_pub : k->own_pub;
    const uint8_t *d_mac = (k->role == KEX_ROLE_DONGLE) ? k->own_mac : k->peer_mac;
    const uint8_t *k_mac = (k->role == KEX_ROLE_DONGLE) ? k->peer_mac : k->own_mac;
    uint8_t dg_conf[16], kb_conf[16];
    unsigned i;
    int rc = 1;

    for (i = 0; i < KEX_T_PREFIX_BYTES; i++) {
        t[i] = k->t_prefix[i];
    }
    for (i = 0; i < 32u; i++) {
        t[KEX_T_PREFIX_BYTES + i] = d_pub[i];
        t[KEX_T_PREFIX_BYTES + 32u + i] = k_pub[i];
    }
    for (i = 0; i < 8u; i++) {
        t[KEX_T_PREFIX_BYTES + 64u + i] = 0u;   /* cont: no continuity yet */
    }
    if (kex_derive(z, t, sizeof(t), &keys) != 0) {
        goto done;
    }
    if (kex_confirm(keys.k_cd, KEX_ROLE_DONGLE, d_mac, k_mac, dg_conf) != 0
        || kex_confirm(keys.k_ck, KEX_ROLE_KEYBOARD, k_mac, d_mac, kb_conf) != 0) {
        goto done;
    }
    for (i = 0; i < 16u; i++) {
        k->own_conf[i]    = (k->role == KEX_ROLE_DONGLE) ? dg_conf[i] : kb_conf[i];
        k->expect_conf[i] = (k->role == KEX_ROLE_DONGLE) ? kb_conf[i] : dg_conf[i];
        k->lk[i] = keys.lk[i];
    }
    k->keys_ready = 1u;
    rc = 0;
done:
    wipe(t, sizeof(t));
    wipe(&keys, sizeof(keys));
    wipe(dg_conf, sizeof(dg_conf));
    wipe(kb_conf, sizeof(kb_conf));
    return rc;
}

/* The peer's CONFIRM is complete: verify it. */
static void check_peer_confirm(kex_t *k)
{
    if (k->peer_conf_ok || k->peer_conf_mask != 0x3u || !k->keys_ready) {
        return;
    }
    if (kex_ct_diff(k->peer_conf, k->expect_conf, 16u) != 0u) {
        fail(k, KEX_FAIL_CONFIRM);
        return;
    }
    k->peer_conf_ok = 1u;
    k->state = KEX_DONE;
}

kex_state_t kex_work(kex_t *k, unsigned steps)
{
    while (steps--) {
        if (k->state == KEX_KEYGEN) {
            if (x25519_step(&k->x)) {
                continue;
            }
            k->x_busy = 0u;
            /* A clamped scalar times the base point is never zero. */
            (void)x25519_finish(&k->x, k->own_pub);
            k->state = KEX_PUB;
        }
        if (k->state == KEX_PUB) {
            if (k->peer_pub_mask != 0xFu) {
                break;                       /* waiting for the peer's key */
            }
            x25519_begin(&k->x, k->priv, k->peer_pub);
            k->x_busy = 1u;
            k->state = KEX_DH;
            continue;
        }
        if (k->state == KEX_DH) {
            uint8_t z[32];
            int zero, rc;

            if (x25519_step(&k->x)) {
                continue;
            }
            k->x_busy = 0u;
            zero = x25519_finish(&k->x, z);
            wipe(k->priv, sizeof(k->priv));   /* the scalar has done its job */
            if (zero) {
                wipe(z, sizeof(z));
                fail(k, KEX_FAIL_LOW_ORDER);
                break;
            }
            rc = derive(k, z);
            wipe(z, sizeof(z));
            if (rc != 0) {
                fail(k, KEX_FAIL_ENGINE);
                break;
            }
            k->tx_next = 0u;
            k->state = KEX_CONFIRM;
            /* The peer's CONFIRM may have arrived while we were computing. */
            check_peer_confirm(k);
            break;
        }
        break;                               /* CONFIRM, DONE, FAILED, IDLE */
    }
    return (kex_state_t)k->state;
}

void kex_rx(kex_t *k, const uint8_t payload[KEX_PAYLOAD_BYTES])
{
    uint8_t hdr = payload[0];
    uint8_t msg = (uint8_t)(hdr >> 4);
    uint8_t frag = (uint8_t)((hdr >> 2) & 0x3u);
    unsigned i;

    if ((hdr & 0x3u) != KEX_VERSION) {
        return;
    }
    if (k->state == KEX_IDLE || k->state == KEX_FAILED) {
        return;
    }
    switch (msg) {
    case KEX_MSG_PUB:
        /* First copy of each fragment wins. A different later copy is noise
         * or tampering; either way the confirmation decides. Ignored once
         * the secret is being computed from what we have. */
        if (k->state != KEX_KEYGEN && k->state != KEX_PUB) {
            return;
        }
        if (k->peer_pub_mask & (uint8_t)(1u << frag)) {
            return;
        }
        for (i = 0; i < 8u; i++) {
            k->peer_pub[8u * frag + i] = payload[1u + i];
        }
        k->peer_pub_mask |= (uint8_t)(1u << frag);
        break;
    case KEX_MSG_CONFIRM:
        /* Only once our own public-key exchange has reached the shared
         * secret. A confirmation heard earlier cannot belong to this
         * exchange -- the peer cannot have derived anything yet -- and
         * keeping it (first copy wins) would make the real one, when it
         * comes, lose to a leftover from an exchange the peer abandoned. */
        if (frag > 1u || (k->state != KEX_DH && k->state != KEX_CONFIRM)) {
            return;
        }
        if (k->peer_conf_mask & (uint8_t)(1u << frag)) {
            return;
        }
        for (i = 0; i < 8u; i++) {
            k->peer_conf[8u * frag + i] = payload[1u + i];
        }
        k->peer_conf_mask |= (uint8_t)(1u << frag);
        check_peer_confirm(k);
        break;
    case KEX_MSG_ABORT:
        if (k->state != KEX_DONE) {
            fail(k, KEX_FAIL_PEER_ABORT);
        }
        break;
    default:
        break;
    }
}

int kex_tx(kex_t *k, uint8_t payload[KEX_PAYLOAD_BYTES])
{
    const uint8_t *src = 0;
    uint8_t msg = 0u, nfrag = 0u, frag;
    unsigned i;

    if (k->state == KEX_FAILED) {
        if (k->abort_tx == 0u) {
            return 0;
        }
        k->abort_tx--;
        payload[0] = HDR(KEX_MSG_ABORT, 0u);
        payload[1] = k->fail;
        for (i = 2; i < KEX_PAYLOAD_BYTES; i++) {
            payload[i] = 0u;
        }
        return 1;
    }
    if (k->role == KEX_ROLE_KEYBOARD && k->state == KEX_PUB) {
        /* The keyboard's first PUB fragment is the dongle's only evidence
         * that its own public key arrived whole, so the keyboard stays quiet
         * until it has all four fragments (it is then in KEX_DH, below). */
        return 0;
    }
    if (k->state == KEX_PUB || k->state == KEX_DH) {
        /* Keep offering the public key while the secret is computed: the
         * peer may still be missing fragments. */
        msg = KEX_MSG_PUB;
        nfrag = 4u;
        src = k->own_pub;
    } else if (k->state == KEX_CONFIRM || k->state == KEX_DONE) {
        /* The keyboard speaks its CONFIRM only after the dongle's verified:
         * the dongle must prove it holds the key first. Until then it keeps
         * offering its public key, which the dongle may still need. */
        if (k->role == KEX_ROLE_KEYBOARD && !k->peer_conf_ok) {
            msg = KEX_MSG_PUB;
            nfrag = 4u;
            src = k->own_pub;
        } else if (k->role == KEX_ROLE_DONGLE && k->state == KEX_DONE) {
            return 0;                        /* nothing more to say */
        } else {
            msg = KEX_MSG_CONFIRM;
            nfrag = 2u;
            src = k->own_conf;
        }
    } else {
        return 0;                            /* IDLE, KEYGEN */
    }
    frag = (uint8_t)(k->tx_next % nfrag);
    k->tx_next = (uint8_t)((frag + 1u) % nfrag);
    payload[0] = HDR(msg, frag);
    for (i = 0; i < 8u; i++) {
        payload[1u + i] = src[8u * frag + i];
    }
    return 1;
}

int kex_rewind(kex_t *k)
{
    unsigned i;

    if (k->state != KEX_KEYGEN && k->state != KEX_PUB && k->state != KEX_DH) {
        return 1;                     /* the scalar is gone, or it is over */
    }
    /* Drop everything that came from the old connection -- public-key
     * fragments can arrive during key generation too -- including a shared
     * secret half computed. The key pair, or the key generation still in
     * progress, stays. */
    for (i = 0; i < 32u; i++) {
        k->peer_pub[i] = 0u;
    }
    for (i = 0; i < 16u; i++) {
        k->peer_conf[i] = 0u;
    }
    k->peer_pub_mask = 0u;
    k->peer_conf_mask = 0u;
    k->peer_conf_ok = 0u;
    k->keys_ready = 0u;
    k->tx_next = 0u;
    if (k->state != KEX_KEYGEN) {
        k->x_busy = 0u;
        k->state = KEX_PUB;
    }
    return 0;
}

void kex_expire(kex_t *k)
{
    if (k->state != KEX_DONE && k->state != KEX_FAILED && k->state != KEX_IDLE) {
        fail(k, KEX_FAIL_TIMEOUT);
    }
}

int kex_take_link_key(kex_t *k, uint8_t out[16])
{
    unsigned i;

    if (k->state != KEX_DONE || k->lk_taken) {
        return 1;
    }
    for (i = 0; i < 16u; i++) {
        out[i] = k->lk[i];
    }
    wipe(k->lk, sizeof(k->lk));
    k->lk_taken = 1u;
    return 0;
}
