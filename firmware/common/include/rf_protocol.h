/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * Shared 2.4 GHz protocol facts (Convergence phase A).
 *
 * Single source of truth for the on-air protocol constants and the
 * packet-layout helpers used by the shared RF task body (common/src/rf_task.c,
 * run by all three chips since the P6 unification). Everything here is a
 * PROTOCOL fact from PROTOCOL.md / the stock-firmware RE -- no chip, HAL, SDK,
 * or FSM material belongs in this header (rf_task state machines, guards, EV10,
 * hop formulas and dispatch live in rf_task.c behind the hal seams).
 *
 * Layering rules (codex-reviewed):
 *  - header-only: the builder/decoder are static inline so extraction cannot
 *    change code placement (no new out-of-line flash calls on IRQ-reached
 *    paths, no .srodata/.rodata surprises);
 *  - channel LUTs are provided as INITIALIZER MACROS, not extern arrays, so
 *    each body keeps its own array object (sizeof(), section placement and
 *    build-time configurability are preserved);
 *  - only <stdint.h> may be included.
 */
#ifndef RF_PROTOCOL_H
#define RF_PROTOCOL_H

#include <stdint.h>

/* ---- Access addresses ---- */
/* The fixed pair-broadcast access address every Bridge75 keyboard transmits
 * on (PROTOCOL.md "Pair-broadcast access address"). Session AAs are generated
 * per pair and carried in the bond record. */
#define RF_PROTO_PAIR_ACCESS_ADDR  0x71764126u

/* ---- Channel LUTs (PROTOCOL.md "Channel hop LUTs") ---- */
#define RF_PROTO_PAIR_CHANNEL_COUNT 3u
#define RF_PROTO_PAIR_CHANNELS_INIT { 8u, 17u, 26u }

#define RF_PROTO_DATA_CHANNEL_COUNT 5u
#define RF_PROTO_DATA_CHANNELS_INIT { 4u, 13u, 20u, 28u, 33u }

/* ---- On-air payload lengths ---- */
#define RF_PROTO_LEN_POLL        1u   /* CONNECTED keep-alive poll            */
#define RF_PROTO_LEN_LED_RELAY   3u   /* [ctrl][0xA1][led] host-LED relay     */
#define RF_PROTO_LEN_PAIR_BCAST 10u   /* keyboard pair broadcast / HID report */
#define RF_PROTO_LEN_PAIR_ACK   15u   /* dongle pair-completion               */
#define RF_PROTO_LEN_BOOT_KBD   10u   /* [ctrl][0xA1][8-byte boot kbd report] */
#define RF_PROTO_LEN_CONSUMER    4u   /* [ctrl][0xA3][usage LE16]             */
#define RF_PROTO_LEN_MOUSE       7u   /* [ctrl][0xA8][btn][X][Y][wheel][pan]  */

/* HID reports are disambiguated by the tag at frame[3] (payload[1]):
 *   0xA1 = boot-keyboard report (LEN-10, 8-byte [mod][rsv][k0..k5]) -> EP1
 *   0xA3 = consumer/media report (LEN-4, 16-bit usage LE)          -> EP3 ID1
 *   0xA8 = mouse report (LEN-7, 5-byte [btn][X][Y][wheel][pan])    -> EP2
 * (bench-captured 2026-06-13, production keyboard: volume-up = [0xA3][E9 00]
 * usage 0x00E9 = Volume Increment; a VIA mouse-move key = [0xA8][00 dX 00 00
 * 00]. Releases are all-zero payloads.) System-control would be a further
 * tag; add as captured. */
#define RF_PROTO_HID_TAG          0xA1u   /* boot keyboard                  */
#define RF_PROTO_HID_TAG_CONSUMER 0xA3u   /* consumer/media (usage LE16)    */
#define RF_PROTO_HID_TAG_MOUSE    0xA8u   /* mouse [btn][X][Y][wheel][pan]  */

/* USB report ID for the consumer-control report on the composite interface
 * (if2 EP3): the host expects [0x01][usage LE16]. The mouse report (if1 EP2)
 * has no report ID -- the 5-byte body forwards verbatim. */
#define RF_PROTO_USB_REPORT_ID_CONSUMER 0x01u

/* ---- Pair-completion defaults (stock values) ---- */
#define RF_PROTO_PAIR_ACK_TYPE   0x02u  /* type tag; also seeds the data-hop
                                         * index on both ends (tag % 5)       */
#define RF_PROTO_DEFAULT_INTERVAL 28u   /* protocol ticks (1/32000 s each)    */
#define RF_PROTO_DEFAULT_TIMEOUT  600u  /* supervision, interval units        */

/* ---- HID handoff contract (identical in both bodies) ----
 * `tag` is the frame[3] report tag (RF_PROTO_HID_TAG* above) so the sink can
 * route by report type; `data`/`len` are the report body after the tag. */
typedef void (*rf_hid_callback_t)(uint8_t tag, const uint8_t *data, uint8_t len);

/* ---- LE16 field helpers ---- */
static inline uint16_t rf_proto_rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---- Pair-completion (LEN-15) payload builder ----
 * Layout (PROTOCOL.md "Dongle event 0x40", stock RF_Tx at VA 0x8c38):
 *   [0..3]  session access address, MSB-first
 *   [4]     type tag (stock 0x02; consumed by both ends as hop seed % 5)
 *   [5..6]  conn_interval, LE
 *   [7..8]  conn_timeout, LE
 *   [9..14] dongle MAC
 * Every field is an explicit input: the CH59x cached-template flow patches
 * individual fields at bond load/init ON PURPOSE (interval/timeout keep the
 * template values there) -- callers choose what to source, this helper only
 * owns the byte layout. */
static inline void rf_protocol_build_pair_ack(uint8_t out[15],
                                              uint32_t session_aa,
                                              uint8_t type_tag,
                                              uint16_t interval,
                                              uint16_t timeout,
                                              const uint8_t dongle_mac[6])
{
    out[0]  = (uint8_t)((session_aa >> 24) & 0xffu);
    out[1]  = (uint8_t)((session_aa >> 16) & 0xffu);
    out[2]  = (uint8_t)((session_aa >> 8) & 0xffu);
    out[3]  = (uint8_t)(session_aa & 0xffu);
    out[4]  = type_tag;
    out[5]  = (uint8_t)(interval & 0xffu);
    out[6]  = (uint8_t)((interval >> 8) & 0xffu);
    out[7]  = (uint8_t)(timeout & 0xffu);
    out[8]  = (uint8_t)((timeout >> 8) & 0xffu);
    out[9]  = dongle_mac[0];
    out[10] = dongle_mac[1];
    out[11] = dongle_mac[2];
    out[12] = dongle_mac[3];
    out[13] = dongle_mac[4];
    out[14] = dongle_mac[5];
}

/* ---- Pair-completion (LEN-15) payload decoder ----
 * Inverse of rf_protocol_build_pair_ack over the same layout. Callers pass
 * the 15-byte payload (their TX template or a received EV10 re-key). Field
 * extraction only -- which fields a caller consumes, and when, is FSM policy
 * that stays in each rf_task body.
 *
 * These accessors are MACROS, not static inlines: behind an inline
 * function's pointer parameter GCC12 -Os loses the direct-global addressing
 * of `array[k]` sites and emits measurably different (byte-non-identical)
 * sequences on RV32 — verified empirically at the P1e conversion. The macros expand to the
 * exact token shapes the bodies previously open-coded, so converted sites
 * stay byte-identical. */
#define rf_protocol_pair_ack_session_aa(payload)          \
    (((uint32_t)(payload)[0] << 24)                       \
     | ((uint32_t)(payload)[1] << 16)                     \
     | ((uint32_t)(payload)[2] << 8)                      \
     |  (uint32_t)(payload)[3])

#define rf_protocol_pair_ack_interval(payload) rf_proto_rd16le(&(payload)[5])
#define rf_protocol_pair_ack_timeout(payload)  rf_proto_rd16le(&(payload)[7])

/* The channel a dongle camps on for bonded reconnect / give-up recovery --
 * pair LUT[0] (stock behavior: the keyboard's autonomous reconnect and its
 * pair broadcast both start on channel 8). Kept as a plain constant so camp
 * sites compile to an immediate, independent of any LUT array object. */
#define RF_PROTO_RECONNECT_CAMP_CHANNEL 8u

/* ---- Pair-broadcast (LEN-10) payload decoder ----
 * Defined over the 10-byte PROTOCOL PAYLOAD (callers pass &frame[2] -- the
 * HAL RX frame base is [rssi][len][payload...]):
 *   [0..5] keyboard MAC, [6..7] conn_interval LE, [8..9] conn_timeout LE.
 * The MAC needs no copy (compare/copy in place from the payload pointer).
 * Accept GUARDS (bonded lockout, boot window, known peer, HID-tag exclusion,
 * RSSI) are protocol-POLICY and stay in each rf_task body. */
static inline void rf_protocol_decode_pair_broadcast(const uint8_t payload[10],
                                                     uint16_t *interval,
                                                     uint16_t *timeout)
{
    *interval = rf_proto_rd16le(&payload[6]);
    *timeout  = rf_proto_rd16le(&payload[8]);
}

static inline uint8_t rf_proto_mac_nonzero(const uint8_t mac[6])
{
    uint8_t any = 0u;
    for (uint8_t i = 0u; i < 6u; i++) {
        any |= mac[i];
    }
    return (uint8_t)(any != 0u);
}

/* CODEREVIEW N08/GB5 MAC hygiene primitives. */
static inline uint8_t rf_proto_mac_all_ff(const uint8_t mac[6])
{
    uint8_t all = 0xFFu;
    for (uint8_t i = 0u; i < 6u; i++) {
        all &= mac[i];
    }
    return (uint8_t)(all == 0xFFu);
}

static inline uint8_t rf_proto_mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    uint8_t diff = 0u;
    for (uint8_t i = 0u; i < 6u; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (uint8_t)(diff == 0u);
}

static inline uint8_t rf_proto_frame_peer_mac_match(const uint8_t *frame,
                                                    const uint8_t peer_mac[6])
{
    uint8_t diff = 0u;
    for (uint8_t i = 0u; i < 6u; i++) {
        diff |= (uint8_t)(frame[2u + i] ^ peer_mac[i]);
    }
    return (uint8_t)(diff == 0u);
}

/* A LEN=10 connected-state RX is ambiguous by syntax: boot-keyboard HID is
 * [ctrl][0xA1][8-byte report], while a reconnect/pair-broadcast refresh is
 * [peer_mac6][interval][timeout]. If peer_mac[1] happens to be 0xA1, the
 * broadcast's second MAC byte sits at frame[3] and looks exactly like the
 * keyboard HID tag. Use the known peer MAC to suppress that false HID path. */
static inline uint8_t rf_proto_is_pair_broadcast_from_peer(
    const uint8_t *frame, uint8_t len, const uint8_t peer_mac[6])
{
    return (uint8_t)(len == RF_PROTO_LEN_PAIR_BCAST
                     && rf_proto_mac_nonzero(peer_mac)
                     && rf_proto_frame_peer_mac_match(frame, peer_mac));
}

/* ---- Connected-state data-channel hop (the stock formula) ----
 * Validated against the stock firmware (runtime 0x20000ff6..0x20001016 and
 * PROTOCOL.md "Connected data-hop"): each poll measures the elapsed protocol
 * ticks (1/32000 s) since the previous poll on the HOP CLOCK -- a per-chip
 * counter wrapping at RF_PROTO_HOP_WRAP (the stock RTC32K modulus; the HSE-
 * derived hop clocks reproduce it so this arithmetic is chip-agnostic) --
 * advances the channel index by elapsed/interval, and applies the stock
 * repeat-correction: when the computed index lands back on prev_idx, force
 * one slot forward and back-date the anchor by (interval - elapsed mod ...)
 * via last = (now + elapsed - interval) mod WRAP, so the NEXT poll's delta
 * lands one slot away instead of repeating. Time-based (not TX-count-based):
 * a skipped or failed poll TX does not desync the index from the keyboard's
 * elapsed-time hop. The caller maps the returned index through the data LUT.
 *
 * Seeding contract (both chips): after a promote/re-key with hop seed S
 * (= pair-ACK byte [4] % RF_PROTO_DATA_CHANNEL_COUNT, the same value both
 * ends consume), set h->prev_idx = S and h->last = <hop clock at the seed
 * instant> (optionally back-dated, e.g. the 13-tick first-burst rule); the
 * next poll one interval later then computes S+1 -- the keyboard's first
 * connected listen channel. */
/* ---- Shared decision helpers (R2) ---- */

/* The stock tx_ctrl feedback formula (PROTOCOL.md:324): synchronise bit 1
 * with the keyboard's fresh-packet indicator; without it the keyboard's
 * CONNECTED-state RX validator stale-rejects most polls. Returns the new
 * tx_ctrl (unchanged when no update is due). Pure; identical in both stock
 * bodies, host-tested. */
static inline uint8_t rf_proto_ctrl_update(uint8_t tx_ctrl, uint8_t rx_ctrl)
{
    if ((rx_ctrl ^ tx_ctrl) & 0x02u) {
        return (uint8_t)(((tx_ctrl & (uint8_t)~0x02u) | (rx_ctrl & 0x02u))
                         ^ 0x01u);
    }
    return tx_ctrl;
}

/* HID-report classifier over the HAL RX frame ([rssi][len][payload...]):
 * returns the report tag at frame[3] if it is a forwardable HID report
 * (boot keyboard 0xA1, consumer 0xA3, mouse 0xA8), else 0. The report body is
 * frame[4..], length len-2. */
static inline uint8_t rf_proto_hid_report_tag(const uint8_t *frame, uint8_t len)
{
    if (len < 2u) {
        return 0u;
    }
    /* Require the EXACT on-air payload length per tag before forwarding. A short
     * 0xA1 frame -- e.g. the LEN=3 [ctrl][0xA1][led] LED relay, or a truncated
     * report -- must NOT be classified as a boot keyboard, or the sink reads an
     * 8-byte report past the (len-2)-byte body. Body length is len-2 (8/2/5). */
    switch (frame[3]) {
    case RF_PROTO_HID_TAG:
        return (len == RF_PROTO_LEN_BOOT_KBD) ? RF_PROTO_HID_TAG : 0u;
    case RF_PROTO_HID_TAG_CONSUMER:
        return (len == RF_PROTO_LEN_CONSUMER) ? RF_PROTO_HID_TAG_CONSUMER : 0u;
    case RF_PROTO_HID_TAG_MOUSE:
        return (len == RF_PROTO_LEN_MOUSE) ? RF_PROTO_HID_TAG_MOUSE : 0u;
    default:
        return 0u;
    }
}

static inline uint8_t rf_proto_hid_report_tag_for_peer(
    const uint8_t *frame, uint8_t len, const uint8_t peer_mac[6])
{
    if (rf_proto_is_pair_broadcast_from_peer(frame, len, peer_mac)) {
        return 0u;
    }
    return rf_proto_hid_report_tag(frame, len);
}

/* Back-compat boolean gate (boot keyboard only). */
static inline uint8_t rf_proto_is_hid_report(const uint8_t *frame, uint8_t len)
{
    return (uint8_t)(rf_proto_hid_report_tag(frame, len) == RF_PROTO_HID_TAG);
}

/* The pair-channel scan and data-hop domains both derive from the transmitted
 * pair-ACK type tag (byte [4] of the 15-byte payload), but they use different
 * LUT sizes. Keep the helpers separate so a stock-style pair scan cannot
 * accidentally use the five-channel data-hop seed. */
static inline uint8_t rf_proto_pair_scan_seed(uint8_t type_tag)
{
    return (uint8_t)(type_tag % RF_PROTO_PAIR_CHANNEL_COUNT);
}

static inline uint8_t rf_proto_hop_seed(uint8_t type_tag)
{
    return (uint8_t)(type_tag % RF_PROTO_DATA_CHANNEL_COUNT);
}

#define RF_PROTO_HOP_WRAP 0xA8C00000u

typedef struct {
    uint32_t last;      /* hop-clock value anchoring the next delta          */
    uint8_t  prev_idx;  /* data-channel index of the previous poll           */
} rf_proto_hop_t;

static inline uint32_t rf_proto_hop_delta(uint32_t now, uint32_t last)
{
    if (now >= last) {
        return now - last;
    }
    return RF_PROTO_HOP_WRAP - last + now;
}

static inline uint8_t rf_proto_hop_step(rf_proto_hop_t *h, uint32_t now,
                                        uint16_t interval)
{
    uint32_t elapsed = rf_proto_hop_delta(now, h->last);
    uint32_t step;
    uint8_t idx;

    h->last = now;
    step = (interval == 0u) ? 1u : (elapsed / interval);
    idx = (uint8_t)((h->prev_idx + step) % RF_PROTO_DATA_CHANNEL_COUNT);
    if (idx == h->prev_idx) {
        uint32_t ci = interval;
        uint32_t sum = h->last + elapsed;

        idx = (uint8_t)((h->prev_idx + 1u) % RF_PROTO_DATA_CHANNEL_COUNT);
        if (sum >= RF_PROTO_HOP_WRAP) {
            sum -= RF_PROTO_HOP_WRAP;
        }
        h->last = (sum >= ci) ? (sum - ci) : (RF_PROTO_HOP_WRAP - (ci - sum));
    }
    h->prev_idx = idx;
    return idx;
}

#endif /* RF_PROTOCOL_H */
