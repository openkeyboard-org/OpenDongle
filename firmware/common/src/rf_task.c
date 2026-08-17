/*
 * UNIFIED RF task body (P6, 2026-06-11 colocation → 2026-07-07 unification):
 * the single 2.4G RF receiver run by ALL THREE chips (CH592, CH582, CH570).
 * Originated as the CH59x (CH592F product) body; the CH570 legacy body
 * (rf_task_ch570.c) converged onto it and was deleted in P6. Chip differences
 * survive only behind the hal seams (hal_timing / hal_rf / hal_dispatch) and
 * the RF_TASK_EXECUTOR_TMOS family switch. Includes resolve per-chip via the
 * building tree's -I src / -I common/include (rf_task.h, CONFIG.h etc.).
 *
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * 2.4G RF Receiver Task
 *
 * Reverse-engineered protocol parameters from stock firmware.
 * See PROTOCOL.md for full specification.
 */

#include "dongle_target.h"      /* RF_TASK_EXECUTOR_TMOS + tick constants */
#include "dongle_platform.h"
#if RF_TASK_EXECUTOR_TMOS
#include "CONFIG.h"   /* TMOS: the CH59x-family executor (registration + timers) */
#include "HAL.h"
#endif
#include "dongle_chip.h"   /* per-tree SDK common header; supplies the __risc_v_*
                            * critical-section shim on CH582 (the ch583 SDK's
                            * core_riscv.h predates it -- see ch582 dongle_chip.h) */
#include "rf_task.h"
#include "bond.h"
#include "rf_crypt.h"

/* CH570 pair-ACK pre-TX gap (V1, bench-derived 2026-07-10). The real Bridge75
 * needs 1..8 ms between its PAIR_BCAST and our 15-byte pair-ACK; answering
 * immediately misses its receive window and the link flaps forever. The
 * shipping build supplied this ACCIDENTALLY, as the ~6.3 ms busy-wait of
 * PRINT("PAIR_BCAST ..."). Schedule it instead: a delayed post runs the
 * existing RF_EVT_TX_PAIR_15 handler from the pump, so nothing blocks the
 * radio IRQ.
 *
 * Expressed in TMOS units (625 us) like every other delay in this file --
 * hal_event_post_delayed() takes Tsys ticks and CH592 converts back with
 * hal_tmos_units_from_tsys(), so a raw microsecond value would silently
 * quantize there (2000 us -> 3 units -> 1875 us). Stating the unit count makes
 * the quantization explicit and keeps the constant portable if CH59x ever
 * needs the same gap.
 *
 * Sweep: 0 -> 0/3, 500 us -> 2/3, 1000/2000/6337 us -> 3/3, 12 ms -> 1/3,
 * 20 ms -> 0/3. Per-value reconnect quality (bench): 1875 us -> 6/6 but
 * 0.22 s / up to 12 flips; 2500 us -> 8/8, 0.11 s, 0-1 flips (chosen: 4 units).
 * Value set by each target's dongle_target.h. */
#ifndef RF_CH570_PAIR_ACK_PRE_TX_TMOS
#error "RF_CH570_PAIR_ACK_PRE_TX_TMOS must be defined by the target header"
#endif

/* Confirm-before-persist gate (Issue #23): defer the durable bond write until a
 * connected RX confirms the peer accepted the session. Named as its own feature
 * so both executors can share the state machine (the durable-arm site and the
 * CX4 write stay per-executor). Every target header must set it explicitly. */
#ifndef RF_CONFIRM_BEFORE_PERSIST
#error "RF_CONFIRM_BEFORE_PERSIST must be defined by the target header"
#endif

#include "usb_device.h"
#include "hal_timing.h"        /* RF deadline-scheduler seam */
#include "hal_timing_ch592.h"  /* CH592 task-dispatch shim (CONNECTED_POLL slot) */
#include "hal_dispatch.h"      /* deferred-event seam (chip header aliases above) */
#if !RF_TASK_EXECUTOR_TMOS
/* CX1 (codex): CONNECTED-state RF re-arms are timing-critical (the keyboard
 * replies inside the ~875 us window) and the CH570 pump's worst-case latency
 * under EP6 IAP flash traffic is milliseconds — so on this executor the
 * radio-IRQ sink arms them SYNCHRONOUSLY (its validated legacy shape). On
 * TMOS they stay posted events (the validated CH59x shape — CX2). */
static void rf_arm_post_poll_rx(void);
static void rf_rearm_rx(void);
#define RF_CONNECTED_POST_POLL_RX_ARM() rf_arm_post_poll_rx()
#define RF_CONNECTED_RX_REARM()         rf_rearm_rx()
/* CH570 hardware poll grid (ch570/Makefile knob; 0 on other !TMOS ports).
 * When set, the connected-poll cadence is a hardware auto-reload grid
 * anchored at the pair/re-key TX and PROMOTED by ownership transfer
 * (hal_timer_promote_periodic -> st_periodic_transfer, zero hardware
 * writes) instead of re-armed at the promote instant — the CH592 TMR0
 * phase-lock mirror. See analysis/ch570-hw-poll-grid/baseline.md for the
 * +3380 ppm software-grid drift this replaces. */
void hal_timer_promote_periodic(uint8_t from_slot, uint8_t to_slot,
                                uint32_t fallback_period,
                                hal_timer_cb_t cb);  /* hal_timing_ch570.c */
uint8_t hal_timer_grid_nudge(uint32_t ticks);        /* hal_timing_ch570.c */
/* Phase acquisition (Codex option C, 2026-07-08). A crystal-flat grid that
 * starts outside the sleeping keyboard's narrow RX window misses it
 * IDENTICALLY forever (real-Bridge75 finding: promote+bond OK, rx=0 for
 * every poll at two different anchors). The v0.96 software timer acquired
 * by accident: its +3380 ppm drift (~+3 us/poll) swept the phase across the
 * slot until a poll landed and the keyboard's ±31 us/packet servo locked.
 * Deliberate version: start the first period at the promote anchor plus a
 * fixed offset (v0.96's effective mod-slot phase ≈ the ~9.7 ms print delay
 * ≈ +75 us; replace with the measured CH592 value when its probe returns),
 * then, while CONNECTED with zero poll responses, stretch the grid +8 us
 * per poll (Codex band 4-8 us — bigger steps can hop a narrow window) until
 * the first reply, then hold flat.
 * N10 SCOPE (CODEREVIEW, accepted residual): this sweep is LOCAL to one
 * uninterrupted grid — a ~26-poll session covers ~0..267 us, and the step
 * counter does NOT carry physical phase into the next re-anchored session, so
 * full-slot cross-session tiling is NOT achieved. The stock Bridge75 locks in
 * practice (production-validated); a true cumulative-phase primitive is
 * deferred pending a phase-controllable failing control. See rf_ch570_acq_arm
 * and analysis/codex-review-campaign-2026-07-17/change12-N10-sweep-spike.md. */
#define RF_CH570_GRID_PHASE_OFFSET_TICKS 7500u   /* 75 us @100 MHz */
#define RF_CH570_ACQ_STEP_TICKS 800u             /* 8 us per poll */
#define RF_CH570_ACQ_MAX_NUDGES 150u   /* 150*8us=1.2ms; > one full slot only
                                        * at the stock 28-tick interval (875us
                                        * slot) — a larger provisioned interval
                                        * (up to 300 -> 9.375ms slot) is NOT
                                        * covered by this fixed cap (N10) */
static void rf_ch570_acq_arm(uint8_t seed);  /* flash helper below */
static volatile uint32_t rf_ch570_acq_base; /* RX tally at promote:
                                         * ANY connected RX freezes the
                                         * sweep (Codex: the first reply may
                                         * be a LEN-10 HID report, not a
                                         * LEN-1 poll response)             */
static volatile uint32_t rf_ch570_acq_nudges;
/* No TMOS on this executor: map the TMOS mem helpers the body uses onto the
 * builtins. NOTE tmos_memcmp is TRUE-when-EQUAL (BLE-lib convention). */
#define tmos_memcpy(dst, src, n) __builtin_memcpy((void *)(dst), (const void *)(src), (n))
#define tmos_memset(dst, v, n)   __builtin_memset((void *)(dst), (v), (n))
#define tmos_memcmp(a, b, n)     (__builtin_memcmp((const void *)(a), (const void *)(b), (n)) == 0)
/* The sink's internal sta vocabulary — numerically the CH59x BLE-lib codes
 * (CH59xBLE_LIB.h) so telemetry stays comparable across chips. */
#define TX_MODE_TX_FINISH 0x01
#define RX_MODE_RX_DATA   0x03
#define TX_MODE_TX_FAIL   0x11
#else
#define RF_CONNECTED_POST_POLL_RX_ARM() hal_event_post(RF_EVT_POST_POLL_RX)
#define RF_CONNECTED_RX_REARM()         hal_event_post(RF_EVT_RX_RESTART)
#endif
#include "hal_rf.h"             /* RF PHY seam */
#include "rf_protocol.h"        /* shared on-air protocol facts (phase A) */
#include "rf_supervision.h"     /* stock-style supervision timing helpers */

#if !RF_TASK_EXECUTOR_TMOS
/* The acquisition sweep stops as soon as any connected packet is received. */
static volatile uint32_t rf_ch570_connected_rx_count;
#endif
/* RF arm status is recorded in IRQ and task context; task-context guard sites
 * consume it and re-arm when needed. */
volatile uint8_t  rf_last_arm_status;
#if !RF_TASK_EXECUTOR_TMOS
/* Cold-boot SRAM entropy: main() hashes the pristine power-on state of the free
 * RAM gap into this word before normal stack use, and the session-AA RNG mixes
 * it into its seed so each cold boot (i.e. each replug,
 * i.e. each fresh pair in practice) gets a distinct, unpredictable AA rather
 * than the UID-deterministic value. Costs one word — the hashed RAM is the
 * normal stack/heap, not reserved. See ch570_capture_boot_entropy() in main.c. */
volatile uint32_t rf_ch570_boot_entropy;
#endif

/* Supervision/re-acquire runs the production CH592 stock shape — the
 * bench-validated v0.93+ product behavior on every build path (including the
 * CH582 port and direct sub-targets). The legacy OpenDongle-style recovery
 * flavor and its CH592_STOCK_EV10_EXPERIMENT A/B flag were retired in the
 * unify-rf-task P1a collapse (flag had defaulted to 1 since v0.94). */

#define CH592_STOCK_EV10_LAPSE_MARGIN_TICKS 128u
#define CH592_STOCK_EV10_RECOVERY_CONFIRM_WINDOWS 1u
#define RF_RTC_BACKSTEP_TOLERANCE_TICKS 1024u

/* ---------- RF PHY parameters (from firmware RE) ---------- */

/* The product boots directly into the raw 2.4 GHz listener on its configured
 * access address and channel. Pair completion persists the bond to DataFlash
 * so the dongle reconnects after a reboot without reprovisioning. */

/* Promote after this numbered session-AA burst has completed locally.
 * Stock promotes from state 1 on TX_MODE_TX_FINISH; it does not wait for
 * a six-packet recovery burst. The current 08FD/CEBD reconnect bench
 * shows the keyboard reporting CONNECTED after burst #1, so default to
 * the stock-shaped early promote and keep the old "after all bursts"
 * behavior. */
#define RF_BURST_PROMOTE_AFTER_SESSION_BURST 1

#if RF_BURST_PROMOTE_AFTER_SESSION_BURST < 1 || RF_BURST_PROMOTE_AFTER_SESSION_BURST > 6
#error "RF_BURST_PROMOTE_AFTER_SESSION_BURST must be in 1..6"
#endif

/* Tick gap between burst TXes. 1 TMOS tick = 625 µs on CH58x.
 *
 * The keyboard's first-pair branch in FUN_ram_20001192 does two DataFlash
 * persists (FUN_ram_00009a40 writes MAC at flash 0x6000, FUN_ram_00009aa4
 * writes AA at flash 0x4000), each = page-erase (~25ms) + page-write
 * (~5ms). Total RF-callback blocking time: ~50-100 ms before the radio
 * reconfig completes and the library re-arms RX on the new session AA.
 *
 * An immediate-fire burst (tmos_set_event, ~0 ms gap) lands entirely
 * inside that blocked window — verified live: 6 TXes fired in 101 ms,
 * keyboard state stayed 0x35. Pacing each burst TX 80 ticks (~50 ms)
 * apart puts burst#2 at +50ms, burst#3 at +100ms, etc. — burst#3..5 are
 * past the DataFlash blocking window and should land on a re-armed
 * keyboard RX.
 *
 */
#define RF_PAIR_ACK_BURST_GAP 80

/* Pair-completion defaults. Keep these before static initializers that
 * reference them.
 *
 * RF_SESSION_ACCESS_ADDR: session AA we'll advertise in the pair-completion.
 * Must match one of the dongle's generator patterns. 0x6A7DAEE6 was dumped
 * live from the stock dongle's SRAM so we know it passes validation.
 *
 * RF_DONGLE_MAC_*: MAC we'll advertise as the "dongle" in the 15-byte
 * pair-completion frame. Two valid choices depending on the test target:
 *   (a) Bench test against a fresh CH592F dev board running stock keyboard
 *       fw (no stored peer): use this chip's own factory BLE MAC so the
 *       bond is durable across reboots. Default below.
 *   (b) Field test against a real Bridge75 keyboard already bonded to its
 *       stock dongle: use the stock dongle's MAC 0xBA 0x47 0x8B 0x72 0xAB
 *       0x3C (read live from its SRAM at 0x20003849..0x2000384E) so the
 *       keyboard matches us on the known-peer branch without touching its
 *       pairing memory.
 * The advertised dongle MAC is held at runtime in rf_bond_dongle_mac,
 * defaulting to RF_DONGLE_MAC_* and overridden from the DataFlash bond
 * record. */
#define RF_SESSION_ACCESS_ADDR   0x6A7DAEE6u
#define RF_DONGLE_MAC_0 0xD1
#define RF_DONGLE_MAC_1 0x4C
#define RF_DONGLE_MAC_2 0x85
#define RF_DONGLE_MAC_3 0x26
#define RF_DONGLE_MAC_4 0x3B
#define RF_DONGLE_MAC_5 0x38

/* 15-byte pair-ACK payload. The session access address and dongle MAC must
 * match the advertised session identity for reconnect tests; otherwise the
 * keyboard can receive a completion packet but enter connected mode on the
 * wrong AA / peer identity. */
/* RF_PAIR_TIMEOUT — advertised conn_timeout in our 15-byte pair-ACK.
 * Default 600 matches what the keyboard's pair broadcast advertises. */
#define RF_PAIR_TIMEOUT RF_PROTO_DEFAULT_TIMEOUT
#define RF_PAIR_ACK15_ACCESS_ADDR RF_SESSION_ACCESS_ADDR

static uint8_t rf_pair_ack15[15] = {
    /* session access addr (MSB-first u32) */
    (uint8_t)((RF_PAIR_ACK15_ACCESS_ADDR >> 24) & 0xFF),
    (uint8_t)((RF_PAIR_ACK15_ACCESS_ADDR >> 16) & 0xFF),
    (uint8_t)((RF_PAIR_ACK15_ACCESS_ADDR >>  8) & 0xFF),
    (uint8_t)( RF_PAIR_ACK15_ACCESS_ADDR        & 0xFF),
    /* type tag (also seeds the data-hop index on both ends via % 5) */
    RF_PROTO_PAIR_ACK_TYPE,
    /* conn_interval (u16 LE) = 0x001C = 28. MUST match the keyboard's
     * advertised conn_interval in PAIR_BCAST. The keyboard hops data
     * channels every conn_interval protocol ticks of 1/32000 s
     * (= 875 µs at 28 — the "32K" clock is the calibrated 32000 Hz
     * LSI, not 32768 Hz); our TMR0 POLL cadence derives from this same
     * value via rf_tmr0_steady_count(). Advertising 0x001E = 30 (the
     * previous value) caused a ~10% rate mismatch: fw-ch592f polls at
     * 937 µs while the keyboard hops at 875 µs, so polls land outside the
     * keyboard's RX window after a few hops and the link drops at
     * ~3 s. Found 2026-05-20 via keyboard SRAM dump (rx_count=0
     * post-CONNECTED). See
     * analysis/fw-ch592f-vs-stock-audit-2026-05-20.md G3. */
    (uint8_t)(RF_PROTO_DEFAULT_INTERVAL & 0xFF),
    (uint8_t)((RF_PROTO_DEFAULT_INTERVAL >> 8) & 0xFF),
    /* conn_timeout (u16 LE) — RF_PAIR_TIMEOUT bytes LE */
    (uint8_t)(RF_PAIR_TIMEOUT & 0xFF),
    (uint8_t)((RF_PAIR_TIMEOUT >> 8) & 0xFF),
    /* our (dongle) MAC */
    RF_DONGLE_MAC_0, RF_DONGLE_MAC_1, RF_DONGLE_MAC_2,
    RF_DONGLE_MAC_3, RF_DONGLE_MAC_4, RF_DONGLE_MAC_5,
};

/* Burst state machine — accessed from both IRQ (TX_FINISH callback) and
 * task context (RF_EVT_TX_PAIR_15B handler). volatile for IRQ safety. */
volatile uint8_t rf_inject_burst_active = 0;
volatile uint8_t rf_inject_burst_idx = 0;  /* 0..5, 0 = first TX in flight */

/* Channel set for the burst. The keyboard's first-pair radio reconfig
 * (FUN_ram_000072e6 in firmwareB.bin) does only RF_Shut + RF_Config —
 * it sets the session AA but does NOT change channel. So immediately
 * after first-pair the keyboard is listening on session AA on whichever
 * pair channel it last broadcast on (∈ {8, 17, 26}). Hitting the data
 * LUT {4, 13, 20, 28, 33} therefore misses the keyboard entirely in
 * state==1. Burst on the pair LUT instead.
 *
 * Two rounds of the shared pair LUT (rf_pair_channels, declared below from
 * RF_PROTO_PAIR_CHANNELS_INIT) give 6 chances spaced over the burst window —
 * biases for the keyboard's listen channel being one of those three. The
 * burst indexes the LUT modulo its size (rf_burst_channel(idx)) so a
 * pair-channel change in rf_protocol.h propagates here automatically. */
#define RF_BURST_TX_COUNT (2u * RF_PROTO_PAIR_CHANNEL_COUNT)

volatile uint8_t rf_supervision_ev10_active = 0;

/* LED-relay queue counter — increments each time rf_queue_led_relay() queues a
 * host LED byte for the poll path. SWD oracle to confirm the relay was queued
 * (the on-air TX is then confirmed by the keyboard's 5A status). */

/* 1760u diag: count faithful-EV10 attempts that completed (caught
 * the LEN-10, sent the LEN-15 re-key, returned to CONNECTED) vs aborted (no
 * LEN-10 in the budget). */

/* Hop-clock timestamp (rf_hop_read(): HSE-derived protocol ticks) captured at
 * the EV10 LEN-15 re-key RF_Tx. Used by RF_EV10_GRID_ALIGNED_POLL to phase
 * the first resumed data poll to rekey_TX + 1 interval (stock's measured
 * convention), independent of the post-TX reconfigure latency. */
volatile uint32_t rf_ev10_rekey_tx_hop;

/* Stock-shaped queued app payload. Per disasm of stock firmware.bin,
 * event 0x80 (LED relay) doesn't transmit directly — it queues
 * [0xA1, LED_byte] and the normal event 0x02 (poll) helper prepends
 * the control byte and sends as [ctrl][0xA1][LED_byte] in the next
 * poll slot. Same TMR0 cadence, same hop schedule — the keyboard's
 * CONNECTED-state RX only listens in those slots, so out-of-band
 * direct LED TX lands in dead air.
 *
 * rf_app_tx_pending nonzero ⇒ rf_send_poll consumes the queue
 * (sends 3-byte [ctrl,buf[0],buf[1]] instead of 1-byte [ctrl]) while
 * decrementing the count. Stock parity: a single LEN=3 relay per host LED
 * change + one at connect/reconnect -- no burst, no periodic heartbeat. */
volatile uint8_t  rf_app_tx_pending;
volatile uint8_t  rf_app_tx_buf[2];

/* Queue the host->keyboard LED relay payload [0xA1][led] so the next connected
 * poll sends it as [ctrl][0xA1][led] (rf_send_poll consumes rf_app_tx_pending).
 * Replaces the stock-shaped delayed RF_EVT_POST_PROMOTE_LED3 event, which the
 * supervision/EV10 rebind paths cancelled before it could fire (the relay never
 * transmitted). The queue buffer is NOT cancelled by those rebind stop sites, so
 * it survives to the next valid poll. Caller contexts are NOT both cooperative:
 * RF_SetLEDState runs in TMOS task context, but the burst-promote re-sync runs
 * inside rf_phy_event_sink -- an IRQ-tail deferred callback that CAN preempt
 * task context (see the P2.4 note at the sink). No IRQ critical section is
 * needed anyway because every shared item (rf_led_state, rf_app_tx_buf bytes,
 * rf_app_tx_pending) is a single volatile byte (atomic on RV32), preemption is
 * strictly one-way (sink over task, never the reverse), and the foreground
 * latches rf_led_state BEFORE any queue access -- so every interleaving
 * converges to the queue holding the current LED byte with pending=1. */
static __attribute__((noinline)) void rf_queue_led_relay(uint8_t led)
{
    rf_app_tx_buf[0]  = RF_PROTO_HID_TAG;
    rf_app_tx_buf[1]  = (uint8_t)(led & 0x07u);
    rf_app_tx_pending = 1u;          /* stock parity: single LEN=3 relay, fire-and-forget */
}

/* Stock dispatcher event 0x04 calls RF_Rx(gp-0x604, 10, 0xff, 0xff):
 * the same control/poll buffer used by empty-poll RF_Tx, but with RX
 * length 10. The shared poll/RX buffer itself is the normal rf_poll_buf below. */

/* Product reconnect default. */
#define RF_DEFAULT_ACCESS_ADDR  0xAC1649CEu


/* CRC initial value */
/* CRC init + LLE mode moved to hal_rf_ch592.c (P2.4) -- the PHY seam owns
 * the rfConfig_t construction. */

/* LLE mode: 2M PHY | basic mode | whitening ON | channel index | RSSI byte */
/* Stock dongle's rfConfig.LLEMode = 0x10 (BASIC + WHITENING_ON +
 * PHY_2M) per the OQ7 SRAM dump. Keyboard uses 2M PHY. Bench-proven
 * 2026-05-17: with PFIC_EnableIRQ(BLEB|BLEL) + RF_Rx(NULL,0,...) +
 * LLEMode=0, fw-ch592f receives 241 RX_DATA callbacks/10s on BLE
 * ADV ch37 (1M PHY). Restoring LLEMode=0x10 for 2M PHY compatibility
 * with the keyboard's proprietary 2.4G protocol. */

/* Minimum RSSI for pair acceptance (dBm). Gates the fresh-pair path (no
 * valid bond) AND a boot-window accept of a DIFFERENT keyboard; the
 * known-peer reconnect path has no RSSI gate (a bonded keyboard must
 * reconnect at range).
 *
 * This floor has now been raised twice for the same reason, which is the
 * point worth carrying forward: -55 rejected most real captures, and -75
 * rejected the product's primary scenario. Bisected with diagnostic builds
 * on stock v0.96.15 with a bonded link: -128 accepted, -90 accepted, -82
 * accepted, -75 REJECTED. So the keyboard's pair broadcast arrives at
 * -81..-76 dBm with both boards on one bench - i.e. -75 had NEGATIVE margin
 * at legitimate pairing distance, and a dongle behind a PC case or across a
 * desk sits in that band or below.
 *
 * -90 keeps what the gate is actually for. Its job is stopping an
 * unprovisioned dongle auto-pairing with a distant keyboard in a dense
 * environment; at 2.4 GHz a cross-room signal (several metres plus a wall)
 * generally lands below -90 while same-room stays above it. That preserves
 * the "not the neighbour's office" property with ~10 dB over what we
 * measured, instead of defending a threshold the bench already fails.
 *
 * Treat this as a heuristic sanity floor, not a security boundary. CH59x
 * RSSI is coarse and uncalibrated chip to chip, so the same number means
 * different real distances on different units, and the gate is reported
 * inert on the CH570 SKU (its RSSI byte reads constant) - so the product
 * line already tolerates a no-gate configuration.
 *
 * If mispairing in dense environments ever becomes a real complaint, the
 * fix is NOT a tighter floor: it is strongest-candidate selection - collect
 * broadcasters briefly during pairing and take the highest RSSI, keeping
 * this only as a sanity floor. An absolute threshold encodes antenna and
 * geometry assumptions; relative selection targets the actual failure mode.
 *
 * Overridable for bench profiles without editing source (this file is shared,
 * so substitute the chip you are building):
 *   make -C firmware ch570 PAIR_MIN_RSSI=-95
 *   make -C firmware ch592 PAIR_MIN_RSSI=-95
 * The live value is readable over IAP (status byte
 * DONGLE_STATUS_OFF_LAST_RSSI), so re-measuring at a different geometry
 * does not need four diagnostic builds the way this bisect did. */
#ifndef RF_PAIR_MIN_RSSI
#define RF_PAIR_MIN_RSSI        (-90)
#endif

/* Pairing packet payload length (exactly 10 bytes) */
#define RF_PAIR_PKT_LEN         RF_PROTO_LEN_PAIR_BCAST

/* Connection timeout in TMOS ticks (1600 = 1 second). Default 5 seconds. */
#define RF_CONN_TIMEOUT_TICKS   (1600 * 5)

/* 15-byte pair-completion TX buffer. Populated at pair-accept time
 * from the layout we reversed via the sniffer's PAIR_ACK decoder and
 * the stock firmware's RF_Tx call at VA 0x8c38:
 *   [0..3]  session access address (MSB-first u32)
 *   [4]     type tag (<5)
 *   [5..6]  conn_interval (u16 LE)
 *   [7..8]  conn_timeout  (u16 LE)
 *   [9..14] dongle MAC (6 bytes)
 * Keyboard validator: type<5 + AA matches 0x6A????E6 or 0xAC????CE.
 * TX channel is whichever channel the broadcast RX left the radio on —
 * matching stock event 0x40, which doesn't call RF_SetChannel. */
static uint8_t rf_pair_ack_buf[15] = {0};

/* RF_SESSION_ACCESS_ADDR / RF_DONGLE_MAC_* are defined (with full rationale)
 * in the single defaults block near the top of the file. */

#define RF_POLL_TX_LEN          RF_PROTO_LEN_POLL
#define RF_POST_POLL_RX_LEN    10u

/* CONNECTED-mode poll state. LEN=1 poll payload is a single control
 * byte; the stock dongle emits `ctrl ∈ {0x00, 0x03}` on the session AA
 * every 28 RTC32K ticks (~875 us, ~1143/s total), rotating across the
 * 5-channel data LUT. Stock parity builds should use the RTC-derived
 * formula rather than the older phase-arbitrary sequence rotation.
 *
 * Stock uses the same gp-0x604 pointer for empty-poll RF_Tx length 1
 * and post-poll RF_Rx length 10. Size this buffer for the RX arm while
 * keeping the TX length explicit at RF_POLL_TX_LEN. */
static uint8_t rf_poll_buf[RF_POST_POLL_RX_LEN] = { 0x00 };
static const uint8_t rf_data_channels[RF_PROTO_DATA_CHANNEL_COUNT] =
    RF_PROTO_DATA_CHANNELS_INIT;
static uint8_t rf_data_ch_idx;

/* Stock event 0x20 uses the pair-channel LUT {8,17,26} and calls
 * RF_Rx(gp-0x604, 6, ...). The exact on-air effect of that RF_Rx(6)
 * pre-phase is still unresolved, but it clearly exists in stock and
 * is separate from event 0x40's explicit 15-byte RF_Tx. */
static const uint8_t rf_pair_channels[RF_PROTO_PAIR_CHANNEL_COUNT] =
    RF_PROTO_PAIR_CHANNELS_INIT;
static uint8_t rf_pair_prep_idx;
static uint8_t rf_pair_prep_buf[6] = { 0 };
/* Stock connection_setup stores 0x30 into gp-0x614 and event 0x20
 * reschedules itself on that interval while state==1. TMOS ticks are
 * 625 us here, so 0x30 ticks = 30 ms. */
#define RF_PAIR_PREP_TICKS      0x30
/* CONNECTED-state RX counters — written from the RF IRQ, read from
 * task context. Purpose: tell us whether any session-AA traffic is
 * getting through while we experiment with poll rates / hop strategy.
 * Volatile so the compiler doesn't hoist reads across the IRQ/task
 * boundary. */



/* ---------- TMR0-driven poll / EV10 re-key pacing ----------
 *
 * Stock dongle times its 2.4 GHz event cadence off TMR0, not off TMOS
 * (PROTOCOL.md §"Timer-driven event scheduling"). TMR0 counts at the
 * current system clock (Tsys = 60 MHz via SetSysClock(CLK_SOURCE_PLL_60MHz)
 * in main.c; no prescaler). On CYC_END the TMR0 IRQ posts the next RF
 * task event, giving us microsecond-precision pacing independent of
 * the TMOS 625 µs scheduling tick.
 *
 * Two counts are relevant:
 *   - First pair-completion delay: 0x4650 = 18000 Tsys counts = 300 µs.
 *     Armed in the PAIRING RX callback on pair-accept, before the first
 *     15-byte TX fires.
 *   - Steady-state poll / pair-completion: conn_interval * 1875 Tsys
 *     counts. With the stock keyboard's conn_interval=28 this is 52500
 *     counts = 875 µs → ~1142.9 Hz, which matches the observed live
 *     poll rate of the stock dongle.
 *
 * TMR0 is periodic (CYC_END reloads and keeps counting). The IRQ
 * handler dispatches on rf_state, so the same free-running timer drives
 * the EV10 pair-ACK pacing and CONNECTED polling without
 * re-arming per transition. Event-handler helpers re-init TMR0 only
 * when the period itself changes — notably on the first pair-completion
 * TX, where the 300 µs bootstrap gets replaced with the steady-state
 * conn_interval-derived count. */
#define RF_TMR0_PAIR_INIT_COUNT   (300u * HAL_TICKS_PER_US)   /* 300 µs in Tsys (18000 @ 60 MHz) */
#define RF_TMR0_DEFAULT_INTERVAL  RF_PROTO_DEFAULT_INTERVAL           /* stock keyboard's conn_interval — fallback before a pair broadcast sets rf_conn_interval */

/* rf_tmr0_start/stop are non-static: the hal_timing_ch592 seam drives them for
 * the CONNECTED_POLL slot (declared in hal_timing_ch592.h). */
static uint32_t rf_tmr0_steady_count(void);
static uint32_t rf_connected_poll_delay_count(uint32_t delay_ticks);

/* ---------- State ---------- */

#if RF_TASK_EXECUTOR_TMOS
static uint8_t rf_taskID;   /* TMOS task handle (the dispatch macros use it) */
#endif
/* volatile: rf_state is written from task/IRQ-tail-callback context and READ in
 * the true TMR0 hardware ISR (rf_tmr0_isr_dispatch posts the event matching the
 * current rf_state), so it must not be cached across that read. */
static volatile uint8_t rf_state;
/* Latest host HID LED byte (CapsLock/NumLock/ScrollLock, low 3 bits). Set by
 * RF_SetLEDState from the foreground; queued directly via rf_queue_led_relay
 * (sent as [ctrl][0xA1][led] by the next poll). Persists across reconnect so
 * the post-promote re-sync restores the keyboard LED. */
static volatile uint8_t rf_led_state;
static int8_t  rf_rssi;
static uint8_t rf_ctrl_byte;

/* Pairing data */
static uint32_t rf_access_addr;
static uint8_t  rf_channel;
static uint8_t  rf_peer_mac[6];
static uint16_t rf_conn_interval;
static uint16_t rf_conn_timeout;

/* Pre-reboot quiesce latch (RF_QuiesceRequest / RF_Quiesced). volatile: set
 * in executor context, polled by the main loop, read by the IRQ-context
 * gates in the timing callbacks and the PHY sink. One-way until reset. */
static volatile uint8_t rf_quiesced;


/* Runtime bond identity. These default to the compiled-in identity, so an
 * un-provisioned chip behaves exactly as before; RF_TaskInit() overrides them
 * from a DataFlash bond record when one is present (see rf_load_persistent_bond
 * and bond.c). Every runtime use of the session/default access address reads
 * these instead of the macros, which makes the binary identity-agnostic rather
 * than pinned to one keyboard. The static rf_pair_ack15[] initializer and the
 * speculative-path buffer builds still reference the macros (compile-time
 * defaults / not in the shipping build); rf_pair_ack15[] is patched at init. */
static uint32_t rf_bond_aa         = RF_SESSION_ACCESS_ADDR;
static uint32_t rf_bond_default_aa = RF_DEFAULT_ACCESS_ADDR;

/* ----- Stock-style boot reconnect/pair window (Step 5) -----
 * With a valid (non-zero-AA) bond present, the dongle spends ~3 s after boot
 * alternating its listen AA between the bond's session AA (reconnect, the
 * tick-0 default so it is always tried first) and the pair AA (so a new
 * keyboard can still pair during the window, e.g. after a replug), then
 * settles on reconnect-only. Mirrors the stock dongle's event-0x200 boot
 * behavior. A fresh/un-provisioned dongle (no valid bond) leaves rf_bond_valid
 * at 0, skips the window, and stays in pairing forever (its existing path). */
/* RF_BOND_FROM_DATAFLASH gates both the boot window (below, used from ~line 2178
 * and in RF_ProcessEvent) and the bond loader (~line 4276). Its default is also
 * declared next to the loader, but that is ~1000 lines AFTER the boot-window
 * #if blocks — so it MUST be defaulted here, before first use, or the window
 * silently compiles out (the macro reads as 0/undefined at those earlier #ifs).
 * The later #ifndef then no-ops. */
#define RF_PAIR_ACCESS_ADDR RF_PROTO_PAIR_ACCESS_ADDR
#define RF_BOOT_WINDOW_STEPS 10u   /* 10 x 300 ms ~= 3 s, like stock 0x200 */
#define RF_BOOT_WINDOW_TICKS 480u  /* 300 ms in 625 us TMOS units (0x1e0) */

/* P3a: hal_timing deltas are Tsys ticks (hal_timing.h contract); the CH592
 * shim divides back to the 625 us TMOS units its backing timers want. This
 * is the TMOS-unit deadline expressed in Tsys -- an exact multiple, so the
 * armed TMOS value is bit-identical to the pre-P3a literal. */
#define RF_BOOT_WINDOW_TICKS_TSYS      (RF_BOOT_WINDOW_TICKS * HAL_TMOS_UNIT_TICKS)
static uint8_t rf_bond_valid;          /* a valid, non-zero-AA bond loaded     */
/* Deferred bond-persist state (see rf_request_bond_persist / rf_persist_bond_task).
 * Declared here (ahead of RF_ProcessEvent) since the RF_EVT_PERSIST_BOND handler
 * touches rf_bond_persist_pending. */
static volatile uint8_t rf_bond_persisted; /* rf_bond_persisted == the RAM tuple matches a
                                            * VERIFIED DataFlash record (set on TMOS load /
                                            * idempotent-match / verified save; cleared when
                                            * a fresh candidate overwrites the tuple). */
static volatile uint8_t rf_bond_persist_pending; /* event posted, write not yet done */
static bond_record_t rf_bond_pending_rec __attribute__((aligned(4))); /* snapshot   */
/* CODEREVIEW N08: the chip's factory MAC, captured at RF_TaskInit BEFORE any
 * record override rewrites rf_bond_dongle_mac. Feeds the semantic validator's
 * own-identity leg (boot load + IAP BondWrite via RF_FactoryMac()). */
static uint8_t rf_factory_mac[6];
/* CODEREVIEW N09: set when the loaded record carried a nonzero dongle_mac
 * override. Auto-persist then carries the ACTIVE advertised identity into the
 * new record instead of silently reverting it to zero (= chip-derived) — the
 * keyboard bonded to the OVERRIDE MAC, so a zero-persisted record broke the
 * next boot's supervision re-key identity. Per-boot (reset re-derives it from
 * the record); a fresh chip / plain pair keeps the zero = chip-MAC semantics. */
static uint8_t rf_dongle_mac_overridden;
/* CODEREVIEW N06: set by RF_TombstoneBond() when the host clears the DataFlash bond
 * over IAP (BondClear). It invalidates the in-RAM bond and BLOCKS any deferred
 * persist from re-writing it AND blocks accepting/promoting a new pair, until the
 * next MCU reset (power-on/reset zeroes .bss -- deliberately NOT lifted at
 * RF_EVT_START, so a delayed-RF-init BondClear on CH570 stays a completed clear).
 * Without it the running RF task's still-valid in-RAM bond (or an already-posted
 * RF_EVT_PERSIST_BOND) re-saves the record the host just erased -- so the clear
 * does not survive to the next boot (bench-observed CH570). */
static volatile uint8_t rf_bond_tombstone;

#if RF_CONFIRM_BEFORE_PERSIST
/* CODEREVIEW N11 (Issue #23: shared by both executors): defer the durable
 * fresh-pair bond write until a connected RX confirms the peer accepted the
 * session. rf_confirm_state runs NONE -> WAIT_RX (fresh promote, awaiting
 * confirm, deadline armed) -> WAIT_REARM (confirm RX seen, awaiting a SUCCESSFUL
 * RX re-arm before the flash write). rf_pair_is_fresh is captured at the
 * pair-broadcast accept: a promote is tentative unless it is a KNOWN DURABLE
 * reconnect (bond valid AND already persisted AND on the bond session AA AND
 * same peer MAC). Ported to CH570 (Issue #23) so a fresh pair that promoted on
 * our own pair-ACK TX-finish alone never persists a dead bond. */
#define RF_CONFIRM_STATE_NONE       0u
#define RF_CONFIRM_STATE_WAIT_RX    1u
#define RF_CONFIRM_STATE_WAIT_REARM 2u
/* 1200 x 625 us = 0.75 s. "TMOS" here names the 625 us tick unit
 * (HAL_TMOS_UNIT_TICKS, defined on both chips), not the executor. */
#define RF_CONFIRM_TIMEOUT_TMOS 1200u
static volatile uint8_t rf_confirm_state;
static uint8_t rf_pair_is_fresh;
#endif /* RF_CONFIRM_BEFORE_PERSIST */
#if RF_TASK_EXECUTOR_TMOS
/* Codex full-batch review (merge blocker): set by a burst-active async TX_FAIL in
 * the radio callback (IRQ) to defer the dual-AA fresh-pair fallback to task
 * context — rf_return_to_fresh_pair does RF_Shut+reconfigure, illegal from the
 * callback. Without it a burst TX_FAIL re-arms RX on the session AA only and can
 * strand a keyboard that missed the first pair-AA ACK on the pair AA. */
static volatile uint8_t rf_burst_txfail_fallback;
#endif
static uint8_t rf_boot_window_active;  /* the alternating window timer is live  */
static uint8_t rf_boot_window_step;    /* tick counter 0..RF_BOOT_WINDOW_STEPS  */
static uint8_t rf_pair_window_open;    /* new-keyboard pairing currently allowed*/
static uint8_t rf_stock_first_supervision_armed;

/* Dongle MAC advertised in the supervision re-key. The keyboard matches this
 * against its stored dongle MAC to accept the LEN-15 re-key, so it must track
 * the bond (not the compiled default) or supervision fails for a dongle whose
 * bond differs from the compiled identity. Defaults to the compiled MAC. */
static uint8_t  rf_bond_dongle_mac[6] = {
    RF_DONGLE_MAC_0, RF_DONGLE_MAC_1, RF_DONGLE_MAC_2,
    RF_DONGLE_MAC_3, RF_DONGLE_MAC_4, RF_DONGLE_MAC_5
};

/* HID data callback */
static rf_hid_cb_t rf_hid_callback;

/* CH59x-BLE-lib RF-descriptor arena poke: the stock CH592 firmware writes the
 * next TX channel + a late byte into the BLE library's internal RF config
 * cells at a FIXED SRAM address before each connected-poll RF_Tx. This is
 * LIBCH59xBLE-specific and the address lives in CH592's 26 KB SRAM. On the
 * CH570 (RFIP radio, 12 KB SRAM) it is both meaningless — the channel is set
 * via hal_rf_set_channel/the RFIP descriptor — and FATAL: 0x200060A0 is far
 * past the CH570 SRAM top (load access fault, P4-caught). No-op off TMOS. */
#if RF_TASK_EXECUTOR_TMOS
#define RF_STOCK_RF_ARENA_BASE ((volatile uint32_t *)0x200060A0u)

static void rf_stock_stage_channel_arena(uint8_t channel, uint8_t late_byte)
{
    uint32_t a0 = RF_STOCK_RF_ARENA_BASE[0];

    RF_STOCK_RF_ARENA_BASE[0] =
        (a0 & 0xFFFF0000u) | ((uint32_t)channel << 8) | 0x10u;
    RF_STOCK_RF_ARENA_BASE[11] = ((uint32_t)late_byte << 24);
}

static void rf_stock_stage_poll_arena(void)
{
    uint8_t late_byte =
        (uint8_t)rf_conn_interval;
    rf_stock_stage_channel_arena(rf_channel, late_byte);
}
#else
static inline void rf_stock_stage_poll_arena(void) { }
#endif

#if RF_TASK_EXECUTOR_TMOS
/* CH59x-family only: the RTC32K counter register (diag validator + entropy).
 * Since P3a nothing on the product path measures time with it. */
#define R32_RTC_CNT_32K_ADDR  0x40001038u
/* Hop-clock modulus: RF_PROTO_HOP_WRAP from rf_protocol.h (single source —
 * a local duplicate here once risked drifting from the shared hop stepper). */

/* R32_RTC_CNT_32K crosses from the asynchronous 32 kHz clock domain; a
 * single in-flight read can capture a torn value. Observed on this board
 * (2026-07-05, P3b logger, 2.06M samples): +2880-tick (+90 ms) forward
 * jumps at ~7e-6 per read, punching through supervision deltas and the
 * phase-error buckets. Read until two consecutive loads agree — the
 * vendor's own RTC_GetCycle32k (CH59x_clk.c) shape. Converges in 1-2
 * iterations: back-to-back loads are ~130 ns apart vs the 31.25 us tick.
 * Called from the radio-IRQ tail too: on healthy hardware this converges
 * in 0-1 retries (sub-microsecond); like the vendor loop it has no
 * iteration cap, so a counter that never produced two equal consecutive
 * reads (broken oscillator/peripheral) would spin — a hardware-failure
 * mode, not a software path. */
static inline uint32_t rtc_read_32k(void)
{
    uint32_t v = *(volatile uint32_t *)R32_RTC_CNT_32K_ADDR;

    for (;;) {
        uint32_t v2 = *(volatile uint32_t *)R32_RTC_CNT_32K_ADDR;

        if (v2 == v) {
            break;
        }
        v = v2;
    }
    return v;
}

static inline uint32_t rtc_delta_32k(uint32_t now, uint32_t last)
{
    return rf_supervision_wrapped_delta(now, last, RF_PROTO_HOP_WRAP,
                                        RF_RTC_BACKSTEP_TOLERANCE_TICKS);
}

/* Entropy for the session-AA RNG seed: the async RTC domain adds true jitter
 * on CH59x; the CH570 (no such register) uses the free-running Tsys tick. */
#define RF_ENTROPY_TICKS() rtc_read_32k()
#else
#define RF_ENTROPY_TICKS() hal_now()
#endif /* RF_TASK_EXECUTOR_TMOS */

/* ---------- Hop clock (P3b iii-b) ----------
 *
 * The channel-hop grid is anchored and stepped in protocol ticks of 1/32000 s
 * (the "32K" clock rate is the LSI's calibration target CAB_LSIFQ=32000, NOT
 * 32768 Hz), read from an HSE-derived software counter: one protocol tick =
 * exactly 1875 Tsys ticks at 60 MHz, accumulated from modular hal_now()
 * deltas with a sub-tick remainder, and WRAPPED AT THE SAME 0xA8C00000
 * MODULUS as the RTC counter -- so rtc_delta_32k, the repeat-correction wrap
 * branch and every existing anchor computation work unchanged.
 *
 * The RTC32K/LSI fallback (RF_HOP_ANCHOR_HSE=0, the stock shape) was retired
 * in the unify-rf-task P1b pass after the P3b gate campaign closed on the HSE
 * anchor: ~16M samples across cold/warm/fault/load/2h-soak conditions, zero
 * >=100 us per-interval RTC-vs-HSE disagreement vs the ~875 us RX window,
 * while the LSI itself wandered -27..-526 ppm with thermal state -- the
 * variability the switch removed (analysis/p3b-gate-evidence-2026-07-05.md).
 * The keyboard side runs a per-RX phase servo (PROTOCOL.md:824), so
 * CONNECTED is closed-loop.
 *
 * Liveness: modular hal_now() deltas are valid only under the 71.6 s SysTick
 * wrap. rf_hop_read() is called from every poll (CONNECTED), every pair-prep
 * rotation (PAIRING/EV10 scan -- see the keepalive in rf_send_pair_prep), and
 * the promote/seed sites, so consecutive reads stay far below the wrap while
 * any hop timestamp is live. Across a LONG camped idle (bonded, keyboard off,
 * no pair traffic) the absolute counter may alias -- harmlessly: every
 * consumer uses deltas between reads that both happen after re-acquisition
 * re-seeds the anchors (no live timestamp spans the gap).
 *
 * Context: called from BOTH the radio-IRQ event sink (promote/seed sites) and
 * TMOS task context (rf_send_poll), so the counter update runs in a short
 * CSR-0x800 critical section (the same save/restore idiom usb_device.c uses;
 * no PFIC helpers, no RF/TMOS calls inside). u32-only arithmetic -- no
 * 64-bit soft-division on the IRQ path. */

static uint32_t rf_hop_hse_ticks;    /* protocol ticks, mod RF_PROTO_HOP_WRAP */
static uint32_t rf_hop_hse_last;     /* hal_now() at the last update */
static uint32_t rf_hop_hse_rem;      /* sub-tick Tsys remainder (0..1874) */
static uint8_t  rf_hop_hse_started;

__HIGH_CODE
static uint32_t rf_hop_read(void)
{
    uint32_t irq = __risc_v_disable_irq();
    uint32_t now = hal_now();
    uint32_t ticks;

    if (!rf_hop_hse_started) {
        rf_hop_hse_started = 1;
        rf_hop_hse_last = now;
    } else {
        uint32_t d = now - rf_hop_hse_last;          /* modular, < 71.6 s */
        uint32_t q = d / HAL_TICKS_PER_PROTO_TICK;
        uint32_t r = d - q * HAL_TICKS_PER_PROTO_TICK;
        uint32_t t;

        rf_hop_hse_last = now;
        rf_hop_hse_rem += r;                         /* < 2*1875: no overflow */
        if (rf_hop_hse_rem >= HAL_TICKS_PER_PROTO_TICK) {
            rf_hop_hse_rem -= HAL_TICKS_PER_PROTO_TICK;
            q++;
        }
        t = rf_hop_hse_ticks + q;                    /* < WRAP + 2.3e6: no overflow */
        if (t >= RF_PROTO_HOP_WRAP) {
            t -= RF_PROTO_HOP_WRAP;
        }
        rf_hop_hse_ticks = t;
    }
    ticks = rf_hop_hse_ticks;
    __risc_v_enable_irq(irq);
    return ticks;
}

/* Wrap-safe backdate on the hop clock's modulus (codex: never leave a raw
 * `- 13` that can underflow past zero near the wrap). always_inline makes the
 * no-call-from-IRQ contract explicit (callers include the radio-IRQ sink). */
__attribute__((always_inline))
static inline uint32_t rf_hop_backdate(uint32_t t, uint32_t ticks)
{
    return (t >= ticks) ? (t - ticks) : (t + RF_PROTO_HOP_WRAP - ticks);
}

/* Timestamp of the last CONNECTED-state RX, in hal_now() Tsys ticks (P3a —
 * supervision lapse measurement moved off RTC32K; deltas are plain 2^32
 * modular subtraction: stamps refresh every connected RX and every read
 * happens within the 10 s supervision deadline, far under the 71.6 s wrap).
 * 0 = no RX this session (the 1-in-2^32 hal_now()==0 stamp collision merely
 * defers supervision by one RX — same sentinel semantics as the old RTC
 * form). Written from the radio IRQ-tail sink, read from task context. */
static volatile uint32_t rf_last_conn_rx_tsys;

/* OQ7 Track 2d: stock dongle's event-0x02 channel hop formula state.
 * Hoisted here (file-scope) because both the burst-promote branch
 * and rf_send_poll use it. See implementation in rf_send_poll. */



/* The shared stock hop state (rf_protocol.h). Field semantics identical to
 * the old stock_last_rtc/stock_prev_idx pair; writes happen in both the
 * radio-IRQ sink (seed sites) and task context (per-poll step), same as
 * before -- the formula itself runs only in task context (rf_send_poll). */
static volatile rf_proto_hop_t rf_hop;
static volatile uint8_t  stock_seed_burst_applied;

/* RF_STOCK_PROMOTE_SEED: reconnect A/B knob. A 2026-05-22 live stock
 * single-step trace at firmware.bin runtime 0x20000f8c and EMU-Dongle
 * exp58 showed the first connected poll entering the hop formula with
 * prev_idx=2, elapsed=0x1d, interval=0x1c, producing idx=3 -> channel 28.
 * Keep the stock first-hop shape by default. With early burst promotion this
 * no longer papers over the old six-burst delay; it only preserves the stock
 * first-poll checkpoint while TMR0 starts at the state-1 TX-finish boundary. */

/* ---------- Forward declarations ---------- */

static void rf_start_rx(void);
static void rf_rearm_rx(void);
static void rf_configure(uint32_t access_addr);
static void rf_send_poll(void);
static void rf_send_pair_prep(void);
static void rf_send_pair_ack(void);
static void rf_request_bond_persist(void);
static void rf_persist_bond_task(void);
#if RF_CONFIRM_BEFORE_PERSIST
static void rf_commit_bond_ram(void);
static void rf_arm_bond_persist(void);
#endif

/* Pair-accept peer-MAC guard (V2 zero-MAC; widened by N08/GB5: all-FF is
 * jam/garbage, and a peer equal to OUR OWN advertised identity would
 * air-persist a record the boot validator then rejects — re-pair every boot
 * — so it must never be accepted in the first place). On CH570 the accept
 * sites live in the __HIGH_CODE radio-IRQ sink and the product sits against
 * the 2 KB stack-floor assert, so keep the check flash-resident (a call, not
 * an inline body — same precedent as rf_return_to_fresh_pair /
 * rf_test_synth_hid_tick). On CH59x (TMOS, ample SRAM) inline it. */
#if !RF_TASK_EXECUTOR_TMOS
__attribute__((noinline))
static uint8_t rf_accept_peer_mac(const uint8_t *mac)
{
    return (uint8_t)(rf_proto_mac_nonzero(mac) && !rf_proto_mac_all_ff(mac)
                     && !rf_proto_mac_equal(mac, rf_bond_dongle_mac));
}
/* Review finding D6: clear the boot bond-persist latch when a boot-window
 * accept CHANGES the peer MAC (new keyboard), so the promote persists the new
 * bond; a same-peer reconnect (MAC equal) leaves the latch set (CX4 — no
 * redundant flash write). Flash-resident noinline: the sole caller is the
 * __HIGH_CODE radio-IRQ accept site and the product sits against the 2 KB
 * stack-floor assert (same precedent as rf_accept_peer_mac). tmos_memcmp
 * is TRUE-when-equal, so !tmos_memcmp == changed. */
__attribute__((noinline))
static void rf_relatch_bond_on_peer_change(const uint8_t *newmac)
{
    if (!tmos_memcmp(rf_peer_mac, newmac, 6)) {
        rf_bond_persisted = 0;
    }
}
#else
static inline uint8_t rf_accept_peer_mac(const uint8_t *mac)
{
    return (uint8_t)(rf_proto_mac_nonzero(mac) && !rf_proto_mac_all_ff(mac)
                     && !rf_proto_mac_equal(mac, rf_bond_dongle_mac));
}
#endif

/* hal_timing CONNECTED_POLL slot callback: adapts the seam's cb(slot) signature
 * to the existing connected poll. Dispatched in TMOS task context (CH59x cannot
 * run RF calls from a timer ISR). Passed to hal_timer_arm() at the connected-poll
 * arm sites; the seam stores it for the P2.5 shared-rf_task dispatch (the CH592
 * connected poll still dispatches natively via RF_EVT_POLL this increment). */
#if !RF_TASK_EXECUTOR_TMOS
/* Promote-time acquisition arm (option-C note at the top of the file):
 * snapshot the connected-RX tally the sweep freezes on, and on the FIRST
 * session seed the fixed v0.96-derived offset. rf_ch570_acq_nudges survives
 * re-promotes as accepted-request TELEMETRY/wrap bookkeeping — NOT as
 * cross-session physical phase: this arm never converts the counter back into
 * grid phase, and every promote re-anchors the physical grid (fresh via
 * st_set_periodic ALL_CLEAR; EV10 inherits the freshly-anchored pair-ACK
 * grid). So the "cumulative cross-session tiling" the older comments claimed
 * is NOT implemented (CODEREVIEW N10, accepted residual — the stock Bridge75
 * is production-validated regardless; a true cumulative-phase primitive is
 * deferred pending a phase-controllable failing control, see
 * analysis/codex-review-campaign-2026-07-17/change12-N10-sweep-spike.md).
 * Flash-resident noinline: the promote paths run in the HIGH_CODE sink (SRAM). */
__attribute__((noinline))
static void rf_ch570_acq_arm(uint8_t seed)
{
    rf_ch570_acq_base = rf_ch570_connected_rx_count;
    if (seed && rf_ch570_acq_nudges == 0u) {
        (void)hal_timer_grid_nudge(RF_CH570_GRID_PHASE_OFFSET_TICKS);
    }
}
#endif

static void rf_connected_poll_cb(uint8_t slot)
{
    (void)slot;
    if (rf_quiesced) {
        return;
    }
    rf_send_poll();
#if !RF_TASK_EXECUTOR_TMOS
    /* Phase-acquisition sweep (see the option-C note at the top of the
     * file): while CONNECTED with zero poll responses this session, walk
     * the grid +8 us per poll until the sleeping keyboard's narrow RX
     * window answers; first reply freezes the grid (flat from then on —
     * the keyboard's per-packet servo holds it). Runs after the TX so the
     * stretch applies to the NEXT period; the primitive self-guards
     * (early-cycle only, one pending stretch). */
    if (rf_state == RF_STATE_CONNECTED &&
        rf_ch570_connected_rx_count == rf_ch570_acq_base) {
        /* Within ONE uninterrupted grid the sweep walks +8 us/poll from the
         * ownership-entry phase; the first reply freezes it (the keyboard's
         * per-packet servo then holds it). N10 CAVEAT: this is a LOCAL sweep —
         * a ~26-poll session covers only ~0..267 us of the 875 us stock slot
         * (requests are pipelined: the shift lands one cycle late, so the last
         * ~2 accepted steps are not yet on a transmitted poll at teardown),
         * and the counter below does NOT carry physical phase into the next
         * (re-anchored) session, so consecutive sessions re-walk substantially
         * the SAME band. The stock Bridge75 still locks (jitter + the differing
         * fresh/EV10 anchor bases reach its window in practice), but full-slot
         * cross-session coverage is NOT achieved. See rf_ch570_acq_arm (N10). */
        if (rf_ch570_acq_nudges >= RF_CH570_ACQ_MAX_NUDGES) {
            rf_ch570_acq_nudges = 0u;   /* wrap the telemetry counter (accepted
                                         * requests), not a physical slot walk */
        }
        if (hal_timer_grid_nudge(RF_CH570_ACQ_STEP_TICKS)) {
            rf_ch570_acq_nudges++;      /* accepted steps only */
        }
    }
#endif
}

/* PAIR_ACK slot callback (P2.3 ii-b): paces the 15-byte pair-completion TX.
 * Same shim shape as rf_connected_poll_cb — the seam stores it for the P2.5
 * shared-rf_task dispatch; this increment the pair-ACK still dispatches
 * natively via TMR0_IRQHandler → RF_EVT_SEND_PAIR_ACK in task context. */
static void rf_pair_ack_cb(uint8_t slot)
{
    (void)slot;
    if (rf_quiesced) {
        return;
    }
    rf_send_pair_ack();
}

/* EV10_REKEY / BOOT_WINDOW slot callbacks (P2.3 ii): on CH59x these slots are
 * TMOS-timer-backed (625 us TMOS units until P3a moves the deadlines onto
 * Tsys/hal_now). The cbs just post the same TMOS event the expiring timer
 * would, so a future seam-side dispatch is behavior-identical. */
static void rf_ev10_rekey_cb(uint8_t slot)
{
    (void)slot;
    hal_event_post(RF_EVT_TIMEOUT);
}

static void rf_boot_window_cb(uint8_t slot)
{
    (void)slot;
    hal_event_post(RF_EVT_BOOT_WINDOW);
}

/* TMOS deadline primitives the CH592 seam fronts for those two slots (the
 * seam cannot reach rf_taskID / the RF_EVT_* bits itself). NOTE the
 * transitional unit divergence from hal_timing.h's Tsys contract: these two
 * slots take 625 us TMOS units on CH592 until P3a. */
#if RF_TASK_EXECUTOR_TMOS
void rf_ev10_deadline_start(uint32_t tmos_ticks)
{
    tmos_start_task(rf_taskID, RF_EVT_TIMEOUT, tmos_ticks);
}

void rf_ev10_deadline_stop(void)
{
    tmos_stop_task(rf_taskID, RF_EVT_TIMEOUT);
}

void rf_boot_window_deadline_start(uint32_t tmos_ticks)
{
    tmos_start_task(rf_taskID, RF_EVT_BOOT_WINDOW, tmos_ticks);
}

void rf_boot_window_deadline_stop(void)
{
    tmos_stop_task(rf_taskID, RF_EVT_BOOT_WINDOW);
}
#endif /* RF_TASK_EXECUTOR_TMOS — the CH570 hal_timing serves these slots
        * natively (st_set/st_cancel); no rf_task-provided backing needed. */

static uint16_t rf_active_conn_interval(void)
{
    uint16_t interval = rf_conn_interval;

    if (interval == 0) {
        interval = rf_protocol_pair_ack_interval(rf_pair_ack15);
    }
    return rf_supervision_conn_interval_or_default(interval);
}

static uint16_t rf_active_conn_timeout(void)
{
    uint16_t timeout = rf_conn_timeout;

    if (timeout == 0) {
        timeout = rf_protocol_pair_ack_timeout(rf_pair_ack15);
    }
    return rf_supervision_conn_timeout_or_default(timeout);
}

static uint32_t rf_connected_supervision_ticks(uint8_t first_arm)
{
    uint16_t tmos = first_arm
                  ? rf_supervision_stock_first_tmos(rf_active_conn_interval())
                  : rf_supervision_stock_timeout_tmos(rf_active_conn_timeout());

    rf_stock_first_supervision_armed = first_arm ? 1u : 0u;
    return (uint32_t)tmos * HAL_TMOS_UNIT_TICKS;
}

static void rf_arm_connected_supervision(uint8_t first_arm)
{
    uint32_t ticks = rf_connected_supervision_ticks(first_arm);

    hal_timer_cancel(HAL_TMR_SLOT_EV10_REKEY);
    hal_timer_arm(HAL_TMR_SLOT_EV10_REKEY, ticks, rf_ev10_rekey_cb);
}

/* Per-packet supervision refresh from the radio IRQ-tail RX sink (~1143/s on a
 * connected link). Arm-only: the same-slot restart replaces the deadline in
 * place, so skipping the hal_timer_cancel avoids a TMOS timer-node free+alloc
 * on every connected RX. A stale RF_EVT_TIMEOUT that slips past (the cancel
 * would otherwise have drained it) lands in the healthy-link grace path and
 * benignly re-arms — the EV10-entry sites, where the drained event is
 * load-bearing, keep their explicit cancel+arm. */
static void rf_refresh_connected_supervision(void)
{
    hal_timer_arm(HAL_TMR_SLOT_EV10_REKEY,
                  rf_connected_supervision_ticks(0), rf_ev10_rekey_cb);
}

static void rf_send_keys_up_on_link_loss(void)
{
    if (rf_state != RF_STATE_CONNECTED || !rf_hid_callback) {
        return;
    }
    /* Always drop any wake-time key-DOWN stash first: the keyboard is gone, so
     * no matching key-up is coming, and the resume flush must not replay it as
     * a stuck key (CODEREVIEW P2). This covers the case where link loss is
     * detected while still suspended (the common path) AND the narrow window
     * where the host has just resumed but the stash is not yet flushed. */
    USB_ClearPendingKeyboard();
    if (USB_IsSuspended()) {
        /* Host asleep: do NOT push keys-up through the normal send path -- that
         * would call usb_hid_in_ready() and request a spurious remote wake for a
         * keyboard that merely died. The stash is already neutralized above, so
         * the eventual resume flush emits an explicit all-keys-up on its own. */
        return;
    }
    static const uint8_t keys_up[8] = { 0 };
    rf_hid_callback(RF_PROTO_HID_TAG, keys_up, sizeof(keys_up));
}

/* Abort any in-flight pair-ACK burst chain: clear the FSM latch and cancel the
 * delayed chain step so a state transition (TX fail, supervision teardown)
 * can't leave a stale burst that later resurrects on an unrelated TX_FINISH or
 * fires TX_PAIR_15B onto a wrong channel/AA. Safe to call when no burst is
 * active. */
static void rf_abort_pair_burst(void)
{
    rf_inject_burst_active = 0;
    rf_inject_burst_idx    = 0;
    hal_event_cancel(RF_EVT_TX_PAIR_15B);
}

/* Fresh-pair fallback (SHARED across executors as of the CH59x P1/P4 port). On
 * CH570 the caller is the unconfirmed-pair tally check in rf_enter_stock_reacquire;
 * on CH59x it is the P1 pair-ACK-TX recovery. The ACTION below (re-camp the
 * boot-window relisten) is identical; only the DETECTION differs per executor.
 *
 * Unconfirmed fresh pair: the CONNECTED tallies are still zero for this boot,
 * so no keyboard has EVER exchanged a connected frame with us — the promote
 * came purely from our own pair-ACK TX_FINISHes and the keyboard never
 * confirmed the ACK (bench-reproduced 2026-07-07 with mangled-ACK fault
 * injection). Two sub-cases, indistinguishable from here: it heard NOTHING
 * (still pair-broadcasting on the PAIR AA — which no session-AA scan or camp
 * can ever hear), or it heard only the first ACK (stored our AA+MAC as a
 * bond, then reconnect-broadcasts on the SESSION AA). Re-run the stock
 * boot-window alternation, which listens on both AAs; the known-peer accept
 * (the bond carries the keyboard's MAC from the failed attempt) completes
 * either path without opening the pair window to other keyboards. If the
 * keyboard is gone, the window expiry settles on the session-AA reconnect
 * camp — the pre-fix terminal state. Repeated failures re-enter here on each
 * unconfirmed promote, so the retry loop sustains while the keyboard keeps
 * broadcasting. (Detection reads the always-on rf_ch570_len1/len10 tallies
 * from task context — deliberately NOT a per-promote latch in the radio-IRQ
 * sink: the sink is __HIGH_CODE and CH570 SRAM sits against the 2 KB
 * stack-floor assert. Consequence: after any CONFIRMED link this boot, a
 * later failed re-pair takes the old reacquire path — acceptable, since a
 * new-keyboard pair needs a replug boot window anyway.) */
static void rf_return_to_fresh_pair(void)
{
    hal_timer_cancel(HAL_TMR_SLOT_CONNECTED_POLL);
    hal_timer_cancel(HAL_TMR_SLOT_PAIR_ACK);
    hal_timer_cancel(HAL_TMR_SLOT_EV10_REKEY);
    hal_event_cancel(RF_EVT_PAIR_PREP);
    hal_event_cancel(RF_EVT_SEND_PAIR_ACK);
    hal_event_cancel(RF_EVT_POLL);
    hal_event_cancel(RF_EVT_POST_POLL_RX);
    rf_abort_pair_burst();
#if RF_CONFIRM_BEFORE_PERSIST
    /* N11: clear the per-attempt confirm state (NOT rf_bond_persisted/rf_bond_valid
     * — the tentative peer is kept so the relisten retries the SAME keyboard; the
     * rf_bond_persisted clear for a genuinely tentative WAIT_RX pair stays at the
     * confirm-timeout site, since this helper is also the good-bond relisten path). */
    rf_confirm_state = RF_CONFIRM_STATE_NONE;
    hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
#endif
#if RF_TASK_EXECUTOR_TMOS
    rf_burst_txfail_fallback = 0u;   /* consumed with this attempt */
#endif

    rf_supervision_ev10_active = 0;
    rf_stock_first_supervision_armed = 0;
    rf_last_conn_rx_tsys = 0;

    rf_boot_window_active = 1u;
    rf_pair_window_open   = 0u;
    rf_boot_window_step   = 0u;
    rf_state = RF_STATE_PAIRING;
    rf_access_addr = rf_bond_aa;
    rf_channel = RF_PROTO_RECONNECT_CAMP_CHANNEL;
    rf_start_rx();
    hal_timer_arm(HAL_TMR_SLOT_BOOT_WINDOW, RF_BOOT_WINDOW_TICKS_TSYS,
                  rf_boot_window_cb);
    
}

static void rf_enter_stock_reacquire(void)
{
#if !RF_TASK_EXECUTOR_TMOS
    if (rf_ch570_connected_rx_count == 0u) {
        rf_return_to_fresh_pair();
        return;
    }
#endif

#if RF_TASK_EXECUTOR_TMOS
    /* Codex full-batch review (+ 2-fix re-review): send the all-keys-up and LEAVE
     * CONNECTED under one IRQ mask. rf_send_keys_up_on_link_loss only fires while
     * rf_state==CONNECTED, and the connected RX sink (radio IRQ) forwards a
     * key-DOWN while CONNECTED — so without this, a late HID RX landing between the
     * keys-up here and the rf_state flip below would forward a key-down that is
     * then torn down with no key-up (stuck key). Setting rf_state out of CONNECTED
     * atomically with the keys-up closes that RF-ordering window.
     *
     * CH570 uses its established non-TMOS path below.
     * KNOWN RESIDUAL (Codex, NOT closed here): this closes the RF-state ordering
     * race, not the full host-delivery guarantee — a key-DOWN already consumed by
     * the host can leave a pending EP1 completion IRQ that NAKs the newly-stashed
     * keys-up after unmask; fully closing that needs USB endpoint-completion
     * ownership in the shared usb_device.c (a separate all-chip effort). The
     * RF-ordering race itself is very narrow (a supervision lapse means the
     * keyboard has gone silent, so an inbound key-DOWN at this instant is
     * contradictory). The redundant rf_state=IDLE at the camp setup below is
     * harmless. */
    uint32_t ku_irq = __risc_v_disable_irq();
    rf_send_keys_up_on_link_loss();
    rf_state = RF_STATE_IDLE;
    (void)__risc_v_enable_irq(ku_irq);
#else
    rf_send_keys_up_on_link_loss();
#endif

    hal_timer_cancel(HAL_TMR_SLOT_CONNECTED_POLL);
    hal_event_cancel(RF_EVT_POLL);
    hal_event_cancel(RF_EVT_POST_POLL_RX);
    hal_event_cancel(RF_EVT_SEND_PAIR_ACK);
    hal_event_cancel(RF_EVT_PAIR_PREP);
    rf_abort_pair_burst();

    rf_boot_window_active = 0;
    rf_pair_window_open = 0;
    rf_stock_first_supervision_armed = 0;

    /* Match the production shape: event 0x10 tears down to state 0, then
     * connection_setup enters state 1 and starts event-0x20 scanning. */
    rf_state = RF_STATE_IDLE;
    rf_access_addr = rf_bond_aa;
    rf_conn_interval = rf_active_conn_interval();
    rf_conn_timeout = rf_active_conn_timeout();
    rf_data_ch_idx = rf_proto_hop_seed(rf_pair_ack15[4]);
    rf_pair_prep_idx = rf_proto_pair_scan_seed(rf_pair_ack15[4]);
    rf_channel = rf_pair_channels[rf_pair_prep_idx];
    rf_supervision_ev10_active = 1;

    hal_rf_shut();
    rf_configure(rf_access_addr);

    rf_state = RF_STATE_PAIRING;
    hal_event_post(RF_EVT_PAIR_PREP);
    hal_timer_arm(HAL_TMR_SLOT_EV10_REKEY,
                  RF_SUPERVISION_STOCK_WATCHDOG_TMOS * HAL_TMOS_UNIT_TICKS,
                  rf_ev10_rekey_cb);
    
}

#ifdef RF_CH570_ARM_RETRY_TMOS
/* Old override name (pre-P1/P4-port). Renamed to RF_ARM_RETRY_TMOS; fail loudly
 * so a custom build using the old knob does not silently lose it. */
#error "RF_CH570_ARM_RETRY_TMOS was renamed to RF_ARM_RETRY_TMOS"
#endif
#ifndef RF_ARM_RETRY_TMOS
/* dongle_target.h (included first, unconditionally) MUST define this. Checked for
 * ALL executors (CH570 + CH59x) as of the P1/P4 port. */
#error "RF_ARM_RETRY_TMOS must be defined by the target header"
#endif
#if RF_ARM_RETRY_TMOS == 0
/* A 0 delay would make the failed-arm reschedule fire immediately and, on a
 * persistent arm failure, hot-spin RF_EVT_RX_RESTART — never let it apply. */
#error "RF_ARM_RETRY_TMOS must be > 0"
#endif
/* P4 deaf-camp guard (SHARED: CH570 polled + CH59x TMOS). Call immediately after
 * arming RF_Rx in a terminal camp whose only re-arm driver would otherwise be a
 * radio RX timeout (the give-up reacquire camp, the closed boot window, the
 * connected self-heal loop). If that arm FAILED the radio never started, so
 * nothing ever re-drives the camp and it is silently deaf forever — worse on
 * CH59x, whose basic-mode RF_Rx has NO hardware timeout at all (hal_rf_ch592.c),
 * so the guard is the SOLE backstop there. Schedule an independent
 * RF_EVT_RX_RESTART so the re-arm loop restarts; the restart re-arms and, if it
 * fails too, re-schedules — the loop self-sustains until an arm succeeds, then
 * the normal RX-completion re-drive resumes. TASK-context only: the arm
 * functions also run from the radio IRQ sink (RF_CONNECTED_RX_REARM), which must
 * NOT schedule here — that path has the supervision watchdog as its backstop. */
static void rf_arm_retry_if_failed(void)
{
    if (rf_last_arm_status != 0u) {
        /* RESIDUAL (accepted, same as CH570): hal_event_post_delayed is void, so
         * a failed TMOS timer-node allocation is not detected here — the camp
         * would stay deaf and this counter would plateau. Every terminal camp
         * that relies on this guard CANCELS all four timer slots + its events
         * first, so the TMOS timer pool has free nodes when this runs; the bench
         * fault-injection confirms the reschedule actually fires. */
        hal_event_post_delayed(RF_EVT_RX_RESTART,
                               RF_ARM_RETRY_TMOS * HAL_TMOS_UNIT_TICKS);
    }
}

/* P1' pair-ACK TX guard. Call with the hal_rf_start_tx() status right after a
 * pair-ACK burst TX. On CH570 a SYNCHRONOUS StartTx refusal (RFIP_StartTx returns
 * nonzero) raises no TX_FINISH, so the burst chain never advances and RX is never
 * re-armed — a deaf strand with no recovery timer (the TX-side twin of the P4
 * RX-arm gap; note CH570 never raises TX_FAIL, so the shared TX_MODE_TX_FAIL path
 * is unreachable here). Recover through the proven unconfirmed-pair boot-window
 * relisten, which camps on rf_bond_aa (== the advertised session AA on a fresh
 * dongle) AND alternates onto the pair AA; the LEN-10 accept gate then re-hears
 * the keyboard on either AA — a fresh dongle via its !rf_bond_valid clause, a
 * bonded dongle accepting a new keyboard via the known-peer MAC match. Returns
 * nonzero if it recovered (caller must not keep chaining the burst).
 * RESIDUAL (not covered): a StartTx that returns 0 then silently never raises
 * TX_FINISH would still stall the burst; unobserved on CH570 (the bench repro is
 * the nonzero-return kind) and would need a burst watchdog timer to close. */
static uint8_t rf_pair_tx_recover_if_failed(uint8_t status)
{
    if (status == 0u) {
        return 0u;
    }
    rf_return_to_fresh_pair();   /* aborts the burst + boot-window relisten */
    return 1u;
}

static void rf_stock_reacquire_giveup(void)
{

    rf_supervision_ev10_active = 0;
    rf_stock_first_supervision_armed = 0;
    rf_last_conn_rx_tsys = 0;
    rf_boot_window_active = 0;
    rf_pair_window_open   = 0;

    /* FULL timer + event teardown (v0.95 hardening, bench 2026-07-05): the
     * failed EV10 cycle can leave CONNECTED_POLL / EV10_REKEY armed (the
     * grid-align promote sites re-arm them mid-cycle), and any stale dispatch
     * leaking into the camp races the reconnect-accept burst this camp exists
     * to serve. Tear down every slot and event the connected machinery owns
     * so the camp is exactly as quiescent as the post-boot-window state.
     * Validated: keyboard killed mid-link -> timeout/reacquire/giveup ->
     * keyboard's autonomous boot reconnect links against this camp in ~0.2 s. */
    hal_timer_cancel(HAL_TMR_SLOT_CONNECTED_POLL);
    hal_timer_cancel(HAL_TMR_SLOT_PAIR_ACK);
    hal_timer_cancel(HAL_TMR_SLOT_EV10_REKEY);
    hal_timer_cancel(HAL_TMR_SLOT_BOOT_WINDOW);
    hal_event_cancel(RF_EVT_PAIR_PREP);
    hal_event_cancel(RF_EVT_SEND_PAIR_ACK);
    hal_event_cancel(RF_EVT_POLL);
    hal_event_cancel(RF_EVT_POST_POLL_RX);
    rf_abort_pair_burst();
#if RF_CONFIRM_BEFORE_PERSIST
    /* Codex fix re-review: a WAIT_REARM here means the peer HAD confirmed the
     * session (a valid connected RX was seen) but the post-confirm rearm never
     * succeeded and reacquire has now exhausted. The pair is genuine, so make it
     * durable before giving up (matches the confirm-timeout WAIT_REARM branch);
     * otherwise a confirmed bond is silently lost if this races ahead of the
     * confirm deadline (e.g. a failed RF_EVT_CONFIRM_TIMEOUT allocation). */
    /* Codex 2-fix re-review (blocking): claim the confirm state under an IRQ mask
     * (the confirm-timeout handler's model). Giveup runs in task context but a late
     * EV10 TX_FINISH can promote back to CONNECTED just before this, and the radio
     * IRQ sink can advance WAIT_RX->WAIT_REARM (and forward HID) between an unmasked
     * read and our action -> wrong routing / lost persist / HID-without-keys-up.
     * Snapshot + clear the confirm state and leave CONNECTED atomically, then act. */
    uint32_t gv_irq = __risc_v_disable_irq();
    uint8_t gv_confirm = rf_confirm_state;
    rf_confirm_state = RF_CONFIRM_STATE_NONE;   /* N11: no tentative pair survives giveup */
    if (rf_state == RF_STATE_CONNECTED) {
        /* a late EV10 promote restored CONNECTED; leave it inside the mask so the
         * sink can't forward a key-DOWN or advance the confirm state after here. */
        rf_state = RF_STATE_PAIRING;
    }
#if RF_TASK_EXECUTOR_TMOS
    rf_burst_txfail_fallback = 0u;
#endif
    (void)__risc_v_enable_irq(gv_irq);
    if (gv_confirm == RF_CONFIRM_STATE_WAIT_REARM) {
        rf_arm_bond_persist();
    } else if (gv_confirm == RF_CONFIRM_STATE_WAIT_RX) {
        /* Codex full-batch review (#3, low): an UNCONFIRMED fresh pair reached
         * giveup. Normally RF_EVT_CONFIRM_TIMEOUT routes an unconfirmed pair to the
         * dual-AA fresh-pair fallback, but if that delayed post failed to allocate
         * there is no deadline, so reacquire exhaustion lands here instead. The
         * keyboard may still be pair-broadcasting on the PAIR AA, which the
         * reconnect-only camp below cannot hear. Return to the dual-AA boot-window
         * relisten (rf_return_to_fresh_pair re-camps BOTH AAs) instead of stranding
         * on the session-AA reconnect camp. */
        rf_return_to_fresh_pair();
        return;
    }
    hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
#endif

    rf_state = RF_STATE_PAIRING;
    /* Camp EXACTLY like the closed boot window (RF_EVT_BOOT_WINDOW terminal
     * state, bench-proven for months): session AA + channel 8, reconnect-only.
     * rf_bond_aa — NOT rf_bond_default_aa: the default is the PAIR AA until a
     * bond is loaded/persisted, and this path only runs after a connection
     * existed, where rf_bond_aa is the live session AA the keyboard will
     * reconnect on. (Like the closed boot window, pairing a NEW keyboard
     * from this camp needs a dongle replug.) */
    rf_access_addr = rf_bond_aa;
    rf_channel = RF_PROTO_RECONNECT_CAMP_CHANNEL;
    rf_start_rx();
    rf_arm_retry_if_failed();   /* P4: don't strand deaf on a failed arm */
    
}

#if DONGLE_RF_CRYPT
/* ---------- Encrypted-link (AES-128-CCM) integration ----------
 *
 * Every site that follows is gated so a plaintext bond -- every bond today --
 * behaves byte-for-byte as before. The IRQ sink only ever copies an encrypted
 * frame into a small FIFO and posts an event; all AES work runs in task context
 * (hal_aes is not reentrant), matching rf_crypt.h's contract. */

/* Set in task context at bond load once a keyed+capable bond is installed (the
 * key lives in rf_crypt); read by the IRQ sink. Single writer, plain byte. */
static volatile uint8_t rf_crypt_bond_enc;

/* Latched when a peer advertises encryption at pairing; rf_commit_bond_ram
 * persists it as BOND_FLAG_ENC_CAPABLE (read exactly once, at burst-promote).
 *
 * Scope: boot (bond load derives it from the record's flag, or zeroes it
 * unbonded) and tombstone are the reset points -- NOT the beacon accept, and
 * NOT the fresh-pair relisten. Both of those look like tidy scope boundaries
 * and both lose the negotiation:
 *   - the advert is anonymous and arrives BEFORE the first beacon (the
 *     keyboard sends it in pair-broadcast slots 0-1, the beacon in slot 2),
 *     so clearing at accept deterministically erased the latch the advert
 *     had just set -- the capability was never persisted and encryption
 *     could never activate (2026-08-16 review, finding 3);
 *   - the relisten path retries the SAME still-broadcasting keyboard, whose
 *     adverts now come only one slot in eight, so a reset there loses the
 *     race again on every unconfirmed-promote retry.
 * The cost of the wide scope is a mislatch when a capable keyboard advertises
 * and a DIFFERENT keyboard's pair completes in the same camp session. That is
 * inert -- capability alone never enables encryption; a key must be
 * host-provisioned -- and the establishment handshake that binds and
 * authenticates the advert is where it gets closed properly. */
static uint8_t rf_crypt_peer_capable;

/* SPSC frame FIFO: the IRQ sink produces, the RF_EVT_CRYPT_RX task consumes.
 * Entries hold the LEN-covered bytes (from rxBuf[2]). A ring of N slots holds
 * N-1 frames; N=2 (one in-flight frame) is what keeps CH570 under its 2 KB
 * stack floor with encryption on. That is ample in normal use -- an encrypted
 * frame arrives at most once per 875 us poll slot and the task drains it within
 * the slot -- and only bites if the executor stalls (EP6 IAP flash traffic,
 * hal_dispatch.h): a second encrypted frame arriving during a >875 us stall is
 * dropped, fail-closed (never a security issue; the keyboard's next report
 * recovers state). The producer publishes the tail AFTER the payload and the
 * consumer the head after the read -- the one-way-preemption discipline of
 * rf_app_tx_buf. */
#define RF_CRYPT_FIFO_N 2u
static uint8_t          rf_crypt_fifo_buf[RF_CRYPT_FIFO_N][RF_CRYPT_LEN_BOOT_KBD];
static uint8_t          rf_crypt_fifo_len[RF_CRYPT_FIFO_N];
static volatile uint8_t rf_crypt_fifo_head;   /* task consumer */
static volatile uint8_t rf_crypt_fifo_tail;   /* IRQ producer */

/* Pre-built session-nonce frame and the number of polls that should still carry
 * it (a short announce window; an authenticated RX confirms and stops it).
 * announce_count is written by the task (session mint, publish-AFTER building
 * session_tx) and decremented by rf_send_poll -- which is the TMR ISR on CH570 --
 * so it is volatile and read before session_tx (the same publish/consume
 * discipline as the FIFO): rf_send_poll only touches session_tx when the count
 * it read is nonzero, and the task sets the count only after session_tx is whole. */
#define RF_CRYPT_ANNOUNCE_POLLS 8u
static uint8_t          rf_crypt_session_tx[RF_CRYPT_LEN_SESSION];
static volatile uint8_t rf_crypt_announce_count;

/* Authenticated-HID silence guard. The sink counts EVERY connected RX on an
 * active encrypted bond (garbage, plaintext-downgrade, polls, all of it); the
 * task resets it whenever a frame verifies. rf_send_poll, once the count crosses
 * the bound, posts RF_EVT_TIMEOUT -- the existing link-loss path, which releases
 * the keyboard and re-enters reacquire in task context -- so an attacker who
 * keeps supervision alive with unauthenticated traffic cannot hold a
 * previously-forwarded key-down stuck. Frame-count based, so no chip-specific
 * tick constant; ~64 poll slots is roughly 56 ms at the connected cadence.
 * Volatile: incremented in the IRQ sink, read/reset in task and (CH570) ISR. */
#define RF_CRYPT_SILENCE_FRAMES 64u
static volatile uint16_t rf_crypt_frames_since_ok;
/* Set by the silence guard, honored by the RF_EVT_TIMEOUT handler to FORCE a
 * reacquire even though unauthenticated traffic keeps supervision's RX stamp
 * fresh (otherwise the timeout handler would just re-arm and never release). */
static volatile uint8_t  rf_crypt_force_release;
/* Why frames were dropped, indexed by rf_crypt_status_t. A single total says a
 * peer is being rejected but not whether its tag failed, its counter looked
 * replayed, or its shape was wrong -- which are entirely different bugs on the
 * transmitting end. Product telemetry: with ok_count these are the "is the
 * encrypted link healthy" signal, read over IAP (CMD_CRYPT_DIAG). */
uint32_t rf_crypt_drop_reason[6];
uint32_t rf_crypt_ok_count;

#if RF_CRYPT_DIAG_PREV_SESSION
static uint32_t rf_crypt_drops;   /* diagnostic (approximate): frames dropped;
                                   * redundant with drop_reason + fifo_full */
/* Pre-verify sink telemetry. rf_crypt_drop_reason only counts frames that
 * actually reached rf_crypt_rx(); a frame lost BEFORE that -- never received,
 * not recognised as an encrypted shape, refused by a full FIFO, or discarded by
 * the session-mint flush -- leaves every exported counter at zero, which reads
 * identically to "the keyboard transmitted nothing". These split those cases:
 *
 *   conn_rx     every connected RX while the bond is encryption-active
 *   len_max     largest LEN seen in that state, and the tag byte beside it --
 *               the decisive datum. Never reaching 22/0xA1 means the frame is
 *               lost on air or before the sink; reaching it while ok_count
 *               stays 0 puts the loss between the sink and rf_crypt_rx
 *   enc_shape   frames the classifier accepted (a FIFO push was attempted)
 *   fifo_full   pushes refused because the 2-deep FIFO was full or the frame
 *               was oversize (an executor stall shows up here and nowhere else)
 *   flush_drop  frames discarded unverified by the RF_EVT_CRYPT_SESSION flush,
 *               which runs on every mint and so up to 8 times per announce
 *   plain_drop  plaintext HID refused as a downgrade on an encrypted bond
 *
 * Diagnostic only: approximate under preemption, and nothing here gates a
 * forwarding decision. Read over IAP (CMD_CRYPT_DIAG, bench layout).
 *
 * Bench-gated since the sink-forensics campaign answered its question: these
 * ten-odd bytes of .bss were part of what pushed CH570 through its stack
 * floor (the 2026-08-16 review, finding 4), and a lost pre-verify frame in
 * the field still shows up as the drop_reason[] pattern above. */
uint32_t rf_crypt_conn_rx;
uint32_t rf_crypt_enc_shape;
uint32_t rf_crypt_fifo_full;
uint32_t rf_crypt_flush_drop;
uint32_t rf_crypt_plain_drop;
uint8_t  rf_crypt_len_max;
uint8_t  rf_crypt_len_max_tag;
#endif /* RF_CRYPT_DIAG_PREV_SESSION */

static uint32_t rf_crypt_gen_session_id(void);   /* defined near rf_aa_rng16 */

/* IRQ-sink helper (flash-resident noinline: the sink is __HIGH_CODE and CH570
 * sits against the 2 KB stack-floor assert -- same precedent as
 * rf_accept_peer_mac). Copy one encrypted frame into the FIFO; 1 if queued. */
__attribute__((noinline))
static uint8_t rf_crypt_fifo_push(const uint8_t *frame, uint8_t len)
{
    uint8_t tail = rf_crypt_fifo_tail;
    uint8_t next = (uint8_t)((tail + 1u) % RF_CRYPT_FIFO_N);
    uint8_t i;

    if (len > RF_CRYPT_LEN_BOOT_KBD || next == rf_crypt_fifo_head) {
#if RF_CRYPT_DIAG_PREV_SESSION
        rf_crypt_drops++;
        rf_crypt_fifo_full++;   /* the only trace this frame ever existed */
#endif
        return 0u;   /* oversize or full: drop newest */
    }
    for (i = 0; i < len; i++) {
        rf_crypt_fifo_buf[tail][i] = frame[i];
    }
    rf_crypt_fifo_len[tail] = len;
    rf_crypt_fifo_tail = next;   /* publish AFTER the payload */
    return 1u;
}

/* IRQ-sink helper: may this frame steer the poll ctrl feedback on an ACTIVE
 * encrypted bond? Polls (LEN-1) and well-formed encrypted frames may; a
 * plaintext HID-shaped frame (a downgrade/forgery) may not, so an attacker
 * cannot desync the poll feedback with unauthenticated bytes. */
__attribute__((noinline))
static uint8_t rf_crypt_ctrl_feedback_ok(const uint8_t *rxBuf)
{
    if (!rf_crypt_bond_enc) {
        return 1u;
    }
    if (rxBuf[1] == RF_PROTO_LEN_POLL) {
        return 1u;
    }
    return rf_crypt_encrypted_body_len(rxBuf[3], rxBuf[1]) ? 1u : 0u;
}
#endif /* DONGLE_RF_CRYPT */

/* ---------- RF status callback (runs in interrupt context) ---------- */






/* PHY-event sink (P2.4): registered with the seam via hal_rf_set_event_cb at
 * RF_TaskInit. The CH592 vendor callback (RF_2G4StatusCallBack, now a thin
 * extractor in hal_rf_ch592.c) maps (sta, rsr) onto hal_rf_event_t and calls
 * here -- one event per invocation. CONTEXT: the CH59x BLE controller runs this
 * sink as an IRQ-tail DEFERRED callback (TMOS_SysRegister/tmosSign ECALL
 * trampoline in ble_task_scheduler.S), NOT a raw BB/LLE ISR -- but it can still
 * PREEMPT RF_ProcessEvent before the interrupted foreground/TMOS work resumes,
 * so state shared with the task (e.g. the hal_timing TMR0 arm/cancel latch) must
 * be guarded; it is NOT cooperatively serialized. The legacy body below
 * is preserved VERBATIM (it is heavily bench-validated); the (sta, rsr) pair
 * it branches on is re-synthesized from the event. */
__HIGH_CODE
static void rf_phy_event_sink(hal_rf_event_t ev, const uint8_t *rx, uint8_t rxlen)
{
    uint8_t *rxBuf = (uint8_t *)rx;
    uint8_t sta, rsr;

    (void)rxlen;
    /* Quiesce gate: an IRQ tail already in flight when hal_rf_shut() ran
     * must not re-arm RX/TX out of the shut radio. */
    if (rf_quiesced) {
        return;
    }
    switch (ev) {
    case HAL_RF_EV_RX_DONE:   sta = RX_MODE_RX_DATA;   rsr = 0; break;
    case HAL_RF_EV_RX_CRCERR: sta = RX_MODE_RX_DATA;   rsr = 1; break;
    case HAL_RF_EV_TX_DONE:   sta = TX_MODE_TX_FINISH; rsr = 0; break;
    case HAL_RF_EV_TX_FAIL:   sta = TX_MODE_TX_FAIL;   rsr = 0; break;
    case HAL_RF_EV_RX_TIMEOUT:
        /* The extractor maps unknown vendor sta here (codex P2.4 #1): give
         * the preserved body a sta no case matches so it falls into its
         * switch default -- the legacy defensive RX_RESTART. */
        sta = 0xFFu; rsr = 0; break;
    default:
        return;
    }
    if (sta == 0x03) {
        if (rsr == 0 && rxBuf
#if DONGLE_RF_CRYPT
            /* On an ACTIVE encrypted bond, only polls and well-formed encrypted
             * frames may steer the poll ctrl feedback; a plaintext HID forgery
             * must not (it is dropped below, and its clear ctrl is unauthenticated).
             * An encrypted frame's ctrl is authenticated as AAD and re-checked in
             * the task -- a tampered ctrl there fails the tag and is dropped, so
             * the only residual is a jammer desyncing poll feedback (DoS), never
             * HID injection. */
            && rf_crypt_ctrl_feedback_ok(rxBuf)
#endif
           ) {
            /* Stock dongle's tx_ctrl feedback formula (PROTOCOL.md:324):
             *   if ((rx_ctrl ^ tx_ctrl) & 2)
             *       tx_ctrl = ((tx_ctrl & ~2) | (rx_ctrl & 2)) ^ 1;
             * Synchronises bit 1 with the keyboard's "fresh packet"
             * indicator. Without this update our tx_ctrl never matches
             * what the keyboard expects, so its CONNECTED-state RX
             * validator stale-rejects most polls. */
            rf_poll_buf[0] = rf_proto_ctrl_update(rf_poll_buf[0],
                                                  rxBuf[2]);
        }
    }
    switch (sta) {
    case RX_MODE_RX_DATA:
        if (rsr != 0) {
            /* CRC error or type mismatch - restart RX */
            hal_event_post(RF_EVT_RX_RESTART);
            break;
        }

        rf_rssi = (int8_t)rxBuf[0];

        /* Gate the pair/listener decoder block on NOT-CONNECTED.
         * Once we've reached state==CONNECTED, this path can consume packets
         * before the normal CONNECTED-state RX handler runs. Skip it so
         * control falls through to the `rf_state == RF_STATE_CONNECTED`
         * handler around line 1401. */
        if (rf_state != RF_STATE_CONNECTED
            && !rf_supervision_ev10_active
        ) {
            /* LEN=10 keyboard pair broadcast on the pair-broadcast
             * access address. Layout per PROTOCOL.md:
             *   [0..5]  keyboard MAC (6 bytes)
             *   [6..7]  conn_interval (u16 LE)
             *   [8..9]  conn_timeout  (u16 LE)
             * NOTE: this catches LEN=10 packets BEFORE the HID-report
             * decoder below while not CONNECTED. On the session access
             * address the keyboard's HID reports also have LEN=10 with
             * rxBuf[3]==0xA1; bonded lockout below prevents treating normal
             * HID as a known-peer reconnect unless its first 6 payload bytes
             * exactly match the peer MAC. */
            if (rxBuf[1] == 10
                && !rf_supervision_ev10_active
                /* CODEREVIEW N06: after a host BondClear, accept no new pair (nor
                 * a reconnect of the just-cleared bond) until the next reset --
                 * BondClear is a completed transaction, so nothing repopulates a
                 * record the host just erased. */
                && !rf_bond_tombstone
                /* Accept a known-peer reconnect unconditionally (its LEN=10
                 * broadcast on the bonded AA must reconnect at any range), but
                 * gate a fresh pair, or a boot-window accept of a DIFFERENT
                 * keyboard, on signal strength: an unprovisioned dongle must
                 * not auto-pair to a distant/unintended keyboard that happens
                 * to be broadcasting in a dense test/user environment. The
                 * bonded lockout (reject a different keyboard outside the boot
                 * window) is preserved by the (!rf_bond_valid||window) guard. */
                /* CODEREVIEW V2 (site 1): the known-peer disjunct must not
                 * match a zero/broadcast peer — a provisioned zero-peer
                 * (learn-mode) record left rf_peer_mac all-zero, and a
                 * crafted all-zero LEN-10 then matched 6-of-6 and was
                 * accepted unconditionally. A real keyboard's MAC is nonzero,
                 * so learn-mode records take the gated accept path below. */
                && ((rf_bond_valid && rf_accept_peer_mac(rf_peer_mac)
                     && tmos_memcmp(rf_peer_mac, &rxBuf[2], 6))
                    || ((!rf_bond_valid || rf_pair_window_open)
                        && rf_rssi >= RF_PAIR_MIN_RSSI
                        /* Reject an all-zero MAC on the accept-of-new path
                         * (V2, v0.96 review): a real keyboard always
                         * broadcasts its 6-byte non-zero MAC, so this only
                         * drops crafted/garbage LEN-10 frames. Load-bearing on
                         * CH570, whose RSSI byte is a constant 0xFF (the
                         * proximity gate is inert), where an accepted zero-MAC
                         * frame would persist a zero-peer bond that then
                         * open-reconnects to any zero-MAC frame. */
                        && rf_accept_peer_mac(&rxBuf[2])))
            ) {
                /* Learn the keyboard (peer) MAC from the pair broadcast so a
                 * completed pair can be persisted to the bond record. */
#if !RF_TASK_EXECUTOR_TMOS
                /* Persist a boot-window peer change (review D6) — see the
                 * helper. Must run BEFORE the memcpy overwrites rf_peer_mac. */
                rf_relatch_bond_on_peer_change(&rxBuf[2]);
#endif
#if RF_CONFIRM_BEFORE_PERSIST
                /* CODEREVIEW N11: classify this accept as a KNOWN-DURABLE reconnect
                 * vs a fresh/tentative pair, BEFORE rf_peer_mac is overwritten. A
                 * reconnect is durable ONLY if the bond is valid AND already
                 * verified in DataFlash AND we are camped on that bond's session AA
                 * AND the broadcaster MAC matches the stored peer. Everything else
                 * is fresh: crucially, a RETRY after a timed-out fresh pair still
                 * has rf_bond_persisted == 0, so it stays fresh and cannot persist
                 * without a fresh confirmation.
                 *
                 * KNOWN LIMITATION -- N11 is PARTIAL for bond-replacing re-pairs
                 * (Codex impl-review; OWNER-ACCEPTED WONTFIX 2026-07-13): such a
                 * re-pair reuses the loaded session AA (advertised in the pair-ACK),
                 * which the OLD keyboard also knows. If the new keyboard misses every
                 * ACK yet the old keyboard is still powered and polling that AA, the
                 * old keyboard's LEN-1 satisfies the confirm for the NEW candidate ->
                 * a dead bond (new MAC + old AA) is persisted; the user recovers by
                 * simply restarting pairing. This is narrower than pre-N11 (which
                 * persisted an unconfirmed re-pair UNCONDITIONALLY) and requires two
                 * keyboards both ACTIVE during a deliberate re-pair -- it cannot
                 * occur in single-keyboard use, nor in steady-state reconnect (a
                 * reconnect never rewrites the bond; only the pairing moment does).
                 * A full fix was designed (advertise a freshly generated candidate
                 * AA for a bond-replacing re-pair so only the new keyboard can
                 * confirm, with a RAM shadow + rollback) but deemed not worth the
                 * state-machine complexity for this narrow, user-recoverable case. */
                {
                    uint8_t peer_mac_eq =
                        tmos_memcmp(rf_peer_mac, &rxBuf[2], 6) ? 1u : 0u;
                    uint8_t known_durable = rf_bond_valid && rf_bond_persisted
                                          && rf_access_addr == rf_bond_aa
                                          && peer_mac_eq;
                    rf_pair_is_fresh = known_durable ? 0u : 1u;
                    if (rf_pair_is_fresh) {
                        rf_bond_persisted = 0;  /* tuple not the verified DataFlash one */
                    }
                }
#endif
#if DONGLE_RF_CRYPT
                /* Peer-change / fresh-dongle accept ONLY (this runs before the
                 * memcpy, so rf_peer_mac is still the OLD peer; tmos_memcmp is
                 * true-when-equal). A DIFFERENT keyboard, or an unbonded dongle,
                 * starts a keyless bond -> drop the encryption-required latch so it
                 * cannot inherit the previous bond's key requirement. A SAME-peer
                 * reconnect must NOT take this path: it keeps the loaded encryption
                 * state (its key is still installed), so a reconnect can never
                 * downgrade an encrypted bond to plaintext. (Byte writes, IRQ-safe;
                 * the stale hal_aes key is unused while enc is off, zeroized at
                 * next bond load/tombstone.)
                 *
                 * rf_crypt_peer_capable is deliberately NOT cleared here: the
                 * capability advert precedes this first acceptable beacon (slots
                 * 0-1 vs 2), so a clear at accept erased what the advert just
                 * latched, 100% of the time -- see the latch's declaration for
                 * the full scoping argument. The policy itself lives in
                 * rf_crypt.h so the host suite pins it. */
                rf_crypt_beacon_accept_latches(
                    rf_bond_valid,
                    (uint8_t)(tmos_memcmp(rf_peer_mac, &rxBuf[2], 6) != 0),
                    &rf_crypt_bond_enc,
                    &rf_crypt_peer_capable);
#endif
                tmos_memcpy(rf_peer_mac, &rxBuf[2], 6);
                /* A new pair is now in progress (this only runs for a fresh
                 * dongle, or a bonded one inside its boot window — see the
                 * lockout guard above). Close the boot window so its AA
                 * alternation can't flip rf_access_addr out from under the
                 * pair-ACK handshake we're about to start. Clearing the flag
                 * makes the next RF_EVT_BOOT_WINDOW tick a no-op; the
                 * tmos_stop_task drops the already-scheduled one too. */
                rf_boot_window_active = 0;
                rf_pair_window_open   = 0;
                hal_timer_cancel(HAL_TMR_SLOT_BOOT_WINDOW);
                /* Arm the burst state machine — the IRQ-side TX_FINISH
                 * for this first TX will then chain RF_EVT_TX_PAIR_15B
                 * to fire 5 more 15-byte TXes on session AA across the
                 * data-channel LUT. */
                rf_inject_burst_active = 1;
                rf_inject_burst_idx    = 0;
                /* Trigger the 15-byte pair-ACK TX from task context.
                 * Don't restart RX yet — the TX path will re-arm RX
                 * after TX_FINISH so we keep watching for the keyboard
                 * to either (a) stop broadcasting (success) or (b)
                 * keep going (need to re-TX). */
#if !RF_TASK_EXECUTOR_TMOS && RF_CH570_PAIR_ACK_PRE_TX_TMOS
                /* Give the keyboard its turnaround window before we answer.
                 * NOTE: hal_event_post_delayed() has two slots and degrades to
                 * an IMMEDIATE post if both are taken -- which would silently
                 * restore the broken behaviour. The other consumers (pair-prep
                 * cadence, burst gap) do not overlap this instant. */
                hal_event_post_delayed(RF_EVT_TX_PAIR_15,
                    RF_CH570_PAIR_ACK_PRE_TX_TMOS * HAL_TMOS_UNIT_TICKS);
#else
                hal_event_post(RF_EVT_TX_PAIR_15);
#endif
                break;
            }

#if DONGLE_RF_CRYPT
            /* Encryption-capability advertisement from a keyboard that supports
             * link encryption (purely additive; a stock keyboard never sends it).
             * Latch it so a completed pair persists BOND_FLAG_ENC_CAPABLE. The
             * latch is scoped to this pairing (reset at the fresh-pair accept).
             * The advert is UNAUTHENTICATED and unbound this phase, but that is
             * inert: capability alone never enables encryption -- a key must also
             * be provisioned (a host action) -- so a forged advert only marks a
             * keyless bond "capable", which stays plaintext. The future
             * establishment handshake binds and authenticates it. */
            if (rxBuf[1] == RF_CRYPT_LEN_CAP && rxBuf[3] == RF_CRYPT_TAG_CAP
                && rxBuf[4] == RF_CRYPT_CAP_VERSION) {
                rf_crypt_peer_capable = 1u;
                hal_event_post(RF_EVT_RX_RESTART);
                break;
            }
#endif

            /* Ignore any non-pair packet while listening for a connection. */
            hal_event_post(RF_EVT_RX_RESTART);
            break;
        }


        if (rf_state == RF_STATE_CONNECTED) {
            uint8_t len = rxBuf[1];
            uint32_t rx_now = hal_now();   /* P3a: supervision lapse is
                                            * measured on Tsys (2^32 modular;
                                            * stamps refresh every RX, reads
                                            * within the 10 s deadline) */
            rf_last_conn_rx_tsys = rx_now;

#if !RF_TASK_EXECUTOR_TMOS
            rf_ch570_connected_rx_count++;
#endif

#if DONGLE_RF_CRYPT
            /* Authenticated-liveness counter: every connected RX on an active
             * encrypted bond -- verified HID, garbage, downgrade, or poll --
             * counts here; rf_send_poll releases the keyboard once it crosses the
             * bound without a verifying frame resetting it (see the guard). */
            if (rf_crypt_bond_enc && rf_crypt_frames_since_ok < 0xFFFFu) {
                rf_crypt_frames_since_ok++;
            }
#if RF_CRYPT_DIAG_PREV_SESSION
            if (rf_crypt_bond_enc) {
                /* Record the shape BEFORE any classification, so a frame the
                 * classifier rejects still leaves evidence of what arrived. */
                rf_crypt_conn_rx++;
                if (len > rf_crypt_len_max) {
                    rf_crypt_len_max = len;
                    rf_crypt_len_max_tag = (len >= 2u) ? rxBuf[3] : 0u;
                }
            }
#endif
#endif

            /* A LEN-10 from the known peer showing up in CONNECTED means our
             * previous session ended on the keyboard's side (it is
             * re-broadcasting). The supervision/EV10 machinery handles the
             * recovery. */

            /* Save control byte */
            rf_ctrl_byte = rxBuf[2];

            /* Dispatch HID data if callback registered and payload present.
             * On-wire layout (validated against sniff-mode decoder at the
             * top of this file): rxBuf[0]=RSSI, rxBuf[1]=LEN, rxBuf[2]=ctrl,
             * rxBuf[3]=0xA1 (HID tag), rxBuf[4..] = HID report. For LEN=10
             * that's an 8-byte boot-keyboard report starting at rxBuf[4].
             * Gate on the 0xA1 tag so non-HID traffic (e.g. LEN=3 LED
             * relay at rxBuf[3]=0xA1, LEN=6 event-0x20 TXes, LEN=1 polls)
             * doesn't get misclassified as a keyboard report. */
            {
#if DONGLE_RF_CRYPT
                uint8_t enc_handled = 0u;
                if (rf_crypt_bond_enc) {
                    if (rf_crypt_encrypted_body_len(rxBuf[3], len)) {
                        /* Encrypted HID frame: copy out and defer verify+decrypt
                         * to the executor (RF_EVT_CRYPT_RX). Nothing is forwarded
                         * until the CCM tag verifies. */
#if RF_CRYPT_DIAG_PREV_SESSION
                        rf_crypt_enc_shape++;
#endif
                        if (rf_crypt_fifo_push(&rxBuf[2], len)) {
                            hal_event_post(RF_EVT_CRYPT_RX);
                        }
                        enc_handled = 1u;
                    } else if (rf_proto_hid_report_tag_for_peer(rxBuf, len,
                                                                rf_peer_mac)) {
                        /* Plaintext HID length on an ACTIVE encrypted bond: a
                         * downgrade/forgery attempt. Drop it -- never forward,
                         * never confirm. (Polls and other frames fall through so
                         * poll-ack confirm + supervision are unchanged.) */
#if RF_CRYPT_DIAG_PREV_SESSION
                        rf_crypt_plain_drop++;
#endif
                        enc_handled = 1u;
                    }
                }
                if (!enc_handled)
#endif
                {
                uint8_t hid_tag =
                    rf_proto_hid_report_tag_for_peer(rxBuf, len, rf_peer_mac);
                if (hid_tag && rf_hid_callback) {
                    rf_hid_callback(hid_tag, &rxBuf[4], len - 2);
                }
#if RF_CONFIRM_BEFORE_PERSIST
                /* CODEREVIEW N11: a valid connected RX from the peer confirms it
                 * accepted the tentative fresh-pair session. A LEN-1 poll ack OR a
                 * validated HID tag (0xA1, peer-matched — hid_tag != 0) both
                 * count; LEN-3 LED relays, LEN-6, and noise do not (that is why
                 * confirmation cannot be spoofed by non-peer traffic). Latch only;
                 * the durable write is armed later, after a successful RX re-arm. */
                if (rf_confirm_state == RF_CONFIRM_STATE_WAIT_RX
                    && (len == 1u || hid_tag != 0u)) {
                    rf_confirm_state = RF_CONFIRM_STATE_WAIT_REARM;
                }
#endif
                }
            }

            /* Reset supervision timer on every valid connected packet. Default
             * builds keep the validated long OpenDongle deadline; the
             * stock-EV10 experiment uses production-style timeout/20. Arm-only
             * refresh (no cancel) — see rf_refresh_connected_supervision. */
#if RF_TASK_EXECUTOR_TMOS
            /* N11 fix (see the fresh-promote note): keep supervision refresh
             * un-gated so a fresh pair's connected-poll RX phase can lock via the
             * EV10-rekey reacquire. Gating this to confirm_state==NONE (original
             * N11 shape) disabled the phase-lock -> deaf fresh pairs. Only
             * LIVENESS is un-gated: a non-confirming frame (LEN-3/6/other) may
             * refresh supervision but CANNOT extend the confirm deadline or cause
             * a persist (that stays gated on WAIT_REARM). Codex-reviewed safe. */
            rf_refresh_connected_supervision();
#else
            rf_refresh_connected_supervision();
#endif
            hal_event_post(RF_EVT_RX_RESTART);

        } else if (rf_state == RF_STATE_PAIRING) {
            uint8_t len = rxBuf[1];

            if (len == RF_PAIR_PKT_LEN) {
                const uint8_t *payload = &rxBuf[2];
                const uint8_t *peer_mac = payload;
                uint8_t mac_is_zero = 1;

                for (int j = 0; j < 6; j++) {
                    if (rf_peer_mac[j] != 0) {
                        mac_is_zero = 0;
                        break;
                    }
                }

                int accept = 0;

                /* V2 (v0.96 review): never accept an all-zero incoming MAC. A
                 * real keyboard always broadcasts its non-zero MAC, so this
                 * only drops crafted/garbage frames. Load-bearing on CH570
                 * (inert 0xFF RSSI gate); it also closes the zero-vs-zero
                 * known-peer match when unbonded (stored MAC still all-zero).
                 * CODEREVIEW N06: also accept nothing after a host BondClear --
                 * neither a new pair nor a retained-peer EV10 reacquire -- until
                 * the next reset (transparent when not tombstoned). */
                if (!rf_bond_tombstone && rf_accept_peer_mac(peer_mac)) {
                    if (tmos_memcmp(rf_peer_mac, peer_mac, 6)) {
                        /* Known peer - accept */
                        accept = 1;
                    } else if (mac_is_zero && rf_rssi >= RF_PAIR_MIN_RSSI) {
                        /* No stored peer and strong signal - initial pairing */
                        tmos_memcpy(rf_peer_mac, peer_mac, 6);
                        accept = 1;
                        
                    }
                }

                if (accept) {
                    /* Extract connection parameters (little-endian) */
                    rf_protocol_decode_pair_broadcast(payload,
                                                      &rf_conn_interval,
                                                      &rf_conn_timeout);

                    

                    /* Build the 15-byte pair-completion payload and TX
                     * it on the {8,17,26} rotation. Layout matches the
                     * stock firmware's RF_Tx call at VA 0x8c38. */
                    rf_protocol_build_pair_ack(rf_pair_ack_buf, rf_bond_aa,
                                               RF_PROTO_PAIR_ACK_TYPE,
                                               rf_conn_interval,
                                               rf_conn_timeout,
                                               rf_bond_dongle_mac);

                    if (rf_supervision_ev10_active) {
                        hal_event_cancel(RF_EVT_PAIR_PREP);
                        hal_timer_arm_periodic(HAL_TMR_SLOT_PAIR_ACK,
                                      RF_TMR0_PAIR_INIT_COUNT, rf_pair_ack_cb);
                        
                        break;
                    }

                    /* Non-EV10 accepts simply keep listening -- the burst
                     * path (which owns live fresh-pair end to end) was
                     * already armed in the pair-broadcast handler. */
                }
            }

            RF_CONNECTED_RX_REARM();

        } else {
            hal_event_post(RF_EVT_RX_RESTART);
        }
        break;

    case TX_MODE_TX_FINISH:
        if (rf_inject_burst_active) {
            uint8_t promote_now = 0;
            if (rf_inject_burst_idx >= RF_BURST_PROMOTE_AFTER_SESSION_BURST) {
                promote_now = 1;
            }
            if (!promote_now && rf_inject_burst_idx < RF_BURST_TX_COUNT) {
                rf_inject_burst_idx++;
                hal_event_post_delayed(RF_EVT_TX_PAIR_15B,
                    RF_PAIR_ACK_BURST_GAP * HAL_TMOS_UNIT_TICKS);
            } else {
                rf_inject_burst_active = 0;
                rf_inject_burst_idx    = 0;
                if (rf_bond_tombstone) {
                    /* CODEREVIEW N06: the host cleared the bond while this fresh
                     * pair's burst was in flight. Abort the promote -- do NOT enter
                     * CONNECTED or persist a bond that would vanish at reset. Resume
                     * a clean listen where the accept gates reject new pairs until
                     * reset. (Transparent when not tombstoned.) */
#if RF_CONFIRM_BEFORE_PERSIST
                    rf_confirm_state = RF_CONFIRM_STATE_NONE;
                    hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
#endif
                    hal_event_post(RF_EVT_RX_RESTART);
                    break;
                }
                /* Burst landed; keyboard should now be in state==2. Promote
                 * to CONNECTED, reading the AA from rf_pair_ack15[0..3]
                 * (MSB-first) — that's the AA we actually advertised, which
                 * may differ from RF_SESSION_ACCESS_ADDR (the burst payload
                 * is hardcoded; the macro is only the default). The keyboard
                 * stored whatever we sent. */
                rf_access_addr = rf_protocol_pair_ack_session_aa(rf_pair_ack15);
                rf_last_conn_rx_tsys = hal_now();
                /* Seed the connected-hop helper from the pair-ACK type tag.
                 *
                 * Per stock RE (rf_task.c:1672 + PROTOCOL.md keyboard
                 * helper at 0x20000E06), the keyboard initializes its
                 * prev_idx from `rf_pair_ack15[4] % 5` (the type tag).
                 * `prev_idx` means previous poll index, not an absolute
                 * first-poll channel. The hop step advances from that seed;
                 * stock-dongle oracle work on 2026-07-03 observed the
                 * connected TX sequence starting at type_tag+1 after the
                 * pair/reconnect LEN-15 exchange. Keep OpenDongle stock-shaped
                 * here so it does not mask a controller that incorrectly
                 * listens on type_tag as the first connected channel.
                 *
                 * Previously fw-ch592f hardcoded idx 0 here, which
                 * with type_tag = 0x02 put us on ch=4 while the
                 * keyboard was on ch=20 — a fixed 2-channel phase
                 * offset, polls never landed on the keyboard's
                 * current listen channel, rx_count stayed 0.
                 * Found 2026-05-20 via keyboard SRAM dump (rx_count=0
                 * post-CONNECTED, even after the interval=28 fix).
                 * See analysis/fw-ch592f-vs-stock-audit-2026-05-20.md
                 * G3 follow-up. */
                rf_data_ch_idx           = rf_proto_hop_seed(rf_pair_ack15[4]);
                rf_channel               = rf_data_channels[rf_data_ch_idx];
                rf_supervision_ev10_active = 0;
                /* Use the interval we advertised in our 15-byte
                 * (rf_pair_ack15[5..6] LE), not the stock-default 28.
                 * The keyboard stored what we sent — TMR0 cadence has
                 * to match or polls drift out of its listen window. */
                rf_conn_interval         = rf_protocol_pair_ack_interval(rf_pair_ack15);
                /* Pair completed — snapshot the learned bond. CODEREVIEW N11
                 * (Issue #23: both executors): a FRESH pair RAM-commits now (so a
                 * same-boot reconnect has a target) but DEFERS the durable DataFlash
                 * write until a connected RX confirms the peer accepted the session
                 * — armed at the supervision site below. A KNOWN-DURABLE reconnect
                 * requests the persist directly (a no-op when already persisted). */
#if RF_CONFIRM_BEFORE_PERSIST
                if (rf_pair_is_fresh) {
                    rf_commit_bond_ram();
                } else {
                    rf_request_bond_persist();
                }
#else
                rf_request_bond_persist();
#endif
                /* Initialize the RTC32K-derived channel hop state.
                 *
                 * 2026-05-20 UPDATE: the hop anchor (rf_hop.prev_idx/.last)
                 * are now set at FIRST BURST TX (around line 2180)
                 * rather than here. The keyboard's anchor is at receipt
                 * of the FIRST valid 15-byte (which is the first burst),
                 * NOT at burst-promote ~200 ms later. Snapshotting here
                 * at promote produced a constant ~200 ms anchor offset
                 * vs the keyboard, which translated to consistent
                 * channel-miss in steady-state.
                 *
                 * We only ensure prev_idx is initialized in case the
                 * first-burst path didn't run (defensive — shouldn't
                 * happen under normal flow). last_rtc retains the
                 * value captured at burst#1. */
                if (rf_hop.last == 0) {
                    rf_hop.prev_idx = rf_proto_hop_seed(rf_pair_ack15[4]);
                    rf_hop.last = rf_hop_backdate(rf_hop_read(), 13u);
                    stock_seed_burst_applied = 0;
                }
#if RF_TASK_EXECUTOR_TMOS
                /* CH592/CH582: keep the historical promote-time re-anchor
                 * (byte-identical). On CH570 this promote-time overwrite (no
                 * backdate, ~bursts after burst#1) defeats the burst#1 anchor
                 * the comment above intends — fresh-pair tolerates the offset
                 * but bonded reconnect (keyboard anchors on the FIRST
                 * session-AA 15-byte, i.e. earlier) does not: it produces a
                 * fixed hop-phase offset -> deaf connected poll -> rx=0
                 * (bench + codex 2026-07-07). CH570 keeps the burst#1 anchor. */
                rf_hop.prev_idx = rf_proto_hop_seed(rf_pair_ack15[4]);
                rf_hop.last = rf_hop_read();
#endif
                stock_seed_burst_applied = 0xFE;
                /* Keep rf_poll_buf[0] stable (per stock: initialized
                 * to 0, only flipped on valid keyboard RX). */
                rf_poll_buf[0] = 0;
                
                /* Stock configures the session-AA radio path while still in
                 * state 1, then commits state 2. Keep that ordering so our
                 * rf_config entry context matches the stock caller trace. */
                rf_start_rx();
                rf_state = RF_STATE_CONNECTED;
#if DONGLE_RF_CRYPT
                /* On an encrypted bond, mint a fresh per-session nonce (in task
                 * context via RF_EVT_CRYPT_SESSION) and start announcing it. */
                if (rf_crypt_bond_enc) {
                    hal_event_post(RF_EVT_CRYPT_SESSION);
                }
#endif
                /* OQ7 Track 2f (2026-05-17): per stock event 0x40 disasm
                 * at runtime 0x20001174, TMR0 phase is anchored to the
                 * 15-byte pair-completion TX itself, NOT to a later
                 * LED3 fire. Anchoring TMR0 hundreds of ms after the
                 * accepted packet means our poll slots are phase-detached
                 * from whatever the keyboard's RX window is scheduled
                 * against. Start TMR0 HERE in the TX_FINISH-promotes-to-
                 * CONNECTED branch (state has just flipped to CONNECTED,
                 * so TMR0_IRQHandler will route fires to RF_EVT_POLL).
                 *
                 * LED3 (if defined) still queues at +0x140 but no longer
                 * gates the poll stream — polls run from this anchor
                 * point, and the next poll after +0x140 consumes the
                 * queue and sends [ctrl][0xA1][LED] in its slot. */
                /* Fresh-pair promote arms the poll cadence HERE, at the
                 * TX_FINISH-promotes-to-CONNECTED instant — the CH592's
                 * exact fresh-pair shape (its TMR0 also starts at THIS
                 * branch, not at the burst TX: only the EV10/re-key path
                 * inherits a TX-anchored timer). On the CH570 hardware grid
                 * (CH570_HW_POLL_GRID) this same call is a hardware
                 * auto-reload anchored at the promote instant: a
                 * burst-TX-start anchor was tried 2026-07-08 and the real
                 * Bridge75 flapped (promote+bond OK, polls missed its
                 * narrow RX window — a few hundred µs of sub-slot phase the
                 * awake harness cannot see); the promote-instant anchor is
                 * the production-validated CH592/v0.96 phase. */
                hal_timer_arm_periodic(HAL_TMR_SLOT_CONNECTED_POLL,
                              rf_connected_poll_delay_count(
                                  rf_conn_interval ? rf_conn_interval
                                                   : RF_TMR0_DEFAULT_INTERVAL),
                              rf_connected_poll_cb);
#if !RF_TASK_EXECUTOR_TMOS
                rf_ch570_acq_arm(1u);   /* seed+sweep arm (flash helper) */
#endif
                /* Re-sync the keyboard LED on this (re)connect: queue the relay
                 * directly (even 0x00) so the next poll carries the current host
                 * LED byte. Replaces the cancellable delayed LED3 event; the
                 * queue survives the supervision/EV10 rebind, and TMR0 is NOT
                 * restarted so the poll grid/phase-lock above is preserved. */
                rf_queue_led_relay(rf_led_state);
#if RF_CONFIRM_BEFORE_PERSIST
                if (rf_pair_is_fresh) {
                    /* CODEREVIEW N11: fresh pair is tentative until the peer
                     * confirms. Arm a dedicated one-shot confirmation deadline
                     * (not normal supervision, which any connected RX would
                     * refresh) and enter WAIT_RX; the durable bond write is armed
                     * only after a valid confirm RX + a successful RX re-arm.
                     * RESIDUAL (accepted, same class as P4): hal_event_post_delayed
                     * is void, so a failed TMOS task-timer allocation is not seen
                     * here -- WAIT_RX would then have no deadline. At a fresh promote
                     * the burst is done and only the hardware CONNECTED_POLL slot is
                     * active, so the TMOS timer pool has room; the poll still runs,
                     * so a present keyboard confirms regardless. */
                    rf_confirm_state = RF_CONFIRM_STATE_WAIT_RX;
                    hal_event_post_delayed(RF_EVT_CONFIRM_TIMEOUT,
                        RF_CONFIRM_TIMEOUT_TMOS * HAL_TMOS_UNIT_TICKS);
                    /* N11 fix (bench-proven 2026-07-13, Codex-reviewed): a fresh
                     * promote MUST arm normal supervision so the EV10-rekey
                     * reacquire runs and LOCKS the connected-poll RX phase.
                     * Skipping it (the original N11 shape) left the fresh pair
                     * deaf (0 connected RX) -> confirm never fired -> never
                     * persisted -> re-pair loop. The confirm deadline armed just
                     * above bounds the persist-deferral; supervision bounds link
                     * liveness. Baseline v0.96.6.4 (persist-at-promote) always
                     * armed this; it is load-bearing, not optional. */
                    rf_arm_connected_supervision(1);
                } else {
                    rf_confirm_state = RF_CONFIRM_STATE_NONE;
                    rf_arm_connected_supervision(1);
                }
#else
                rf_arm_connected_supervision(1);
#endif
            }
            break;
        }
        if (rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING) {
            if (rf_bond_tombstone) {
                /* CODEREVIEW N06: the host cleared the bond; do NOT reacquire
                 * (re-promote) the session via the EV10 rekey. Stand the whole
                 * reacquire machinery down before resuming a clean listen: the
                 * periodic pair-ACK timer self-maintains on CH570 and would
                 * otherwise fire at the connection cadence (~kHz) forever, and the
                 * EV10 watchdog / a queued pair-ACK send must not relaunch it. The
                 * accept gates reject new pairs until reset. (This whole branch is
                 * unreachable when not tombstoned.) */
                rf_supervision_ev10_active = 0;
                hal_timer_cancel(HAL_TMR_SLOT_PAIR_ACK);
                hal_timer_cancel(HAL_TMR_SLOT_EV10_REKEY);
                hal_event_cancel(RF_EVT_PAIR_PREP);
                hal_event_cancel(RF_EVT_SEND_PAIR_ACK);
#if RF_CONFIRM_BEFORE_PERSIST
                /* N11 confirm state: a fresh session's RF_EVT_CONFIRM_TIMEOUT can
                 * otherwise survive this abort and, once, re-arm EV10_REKEY or a
                 * bounded boot-window step (never re-persists / never restarts
                 * PAIR_ACK, but leaves a stale one-shot). Drop it for a clean
                 * teardown, matching rf_return_to_fresh_pair. */
                rf_confirm_state = RF_CONFIRM_STATE_NONE;
                hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
#endif
                hal_event_post(RF_EVT_RX_RESTART);
                break;
            }
            rf_access_addr = rf_protocol_pair_ack_session_aa(rf_pair_ack_buf);
            rf_conn_interval = rf_active_conn_interval();
            rf_conn_timeout = rf_active_conn_timeout();
            rf_hop.prev_idx = rf_proto_hop_seed(rf_pair_ack_buf[4]);
            rf_hop.last = rf_ev10_rekey_tx_hop ? rf_ev10_rekey_tx_hop
                                                : rf_hop_read();
            stock_seed_burst_applied = 0xF7;
            rf_data_ch_idx = rf_hop.prev_idx;
            rf_channel = rf_data_channels[rf_data_ch_idx];

            hal_event_cancel(RF_EVT_PAIR_PREP);
            rf_supervision_ev10_active = 0;

            /* Production shape: RF config happens while still in state 1;
             * TX_FINISH then promotes to state 2 and the already-running
             * TMR0 event-0x40 cadence becomes connected polling. */
            rf_start_rx();
            rf_state = RF_STATE_CONNECTED;
#if DONGLE_RF_CRYPT
            /* EV10 re-key: same peer/key, fresh per-session nonce. */
            if (rf_crypt_bond_enc) {
                hal_event_post(RF_EVT_CRYPT_SESSION);
            }
#endif
#if !RF_TASK_EXECUTOR_TMOS
#endif
            rf_last_conn_rx_tsys = hal_now();
            rf_arm_connected_supervision(1);
#if !RF_TASK_EXECUTOR_TMOS
            /* The CH592 phase-lock inherit, for real: rf_send_pair_ack armed
             * the PAIR_ACK slot as the hardware grid immediately before the
             * re-key TX (st_set_periodic ALL_CLEAR ≡ TX anchor), so the
             * running, TX-anchored auto-reload BECOMES the connected-poll
             * cadence by ownership transfer — zero hardware writes. First
             * poll = re-key TX + 1·interval, exactly the CH592 shape. Falls
             * back to a fresh arm if nothing is periodic (defensive). */
            hal_timer_promote_periodic(HAL_TMR_SLOT_PAIR_ACK,
                HAL_TMR_SLOT_CONNECTED_POLL,
                rf_connected_poll_delay_count(
                    rf_conn_interval ? rf_conn_interval
                                     : RF_TMR0_DEFAULT_INTERVAL),
                rf_connected_poll_cb);
            rf_ch570_acq_arm(0u);   /* sweep arm, no seed (EV10 inherits the
                                     * freshly-anchored pair-ACK grid). The
                                     * LOCAL per-poll sweep runs while
                                     * unanswered; it does NOT carry cumulative
                                     * cross-session phase (N10 — see
                                     * rf_ch570_acq_arm) */
#endif
            
            break;
        }

        /* Any other TX_FINISH re-arms RX via RX_RESTART -> rf_rearm_rx
         * (Shut+Rx only, no Config). RF_Config state persists across
         * RF_Shut and TX<->RX transitions on CH59x -- the validated
         * stock shape relies on the just-completed TX leaving AA /
         * channel / CRC primed (stock disassembly: NO RF_Config between
         * TX and RX; the connected poll does this ~1k times/sec). Paths
         * that change the AA reconfigure explicitly via rf_start_rx.
         * Next poll fires from TMR0 independently.
         *
         * OQ7 Track 2 (2026-05-16): EMU-Dongle exp59 showed stock may
         * re-enter rf_config during transient state==1 reconnect/teardown
         * windows, but not on every state==2 poll. */
        if (rf_state == RF_STATE_CONNECTED) {
            /* Stock-shaped: post the dedicated post-poll RX event so
             * the dispatcher arms RX via RF_Rx(rf_poll_buf, 10, ...)
             * without RF_Config. Mirrors stock callback's
             * TX_MODE_TX_FINISH state==2 → event 0x04 transition. */
            RF_CONNECTED_POST_POLL_RX_ARM();
            break;
        }
        hal_event_post(RF_EVT_RX_RESTART);
        break;

    case TX_MODE_TX_FAIL:
        if (rf_inject_burst_active) {
            /* Abort the pair-ACK burst chain cleanly. Without this the FSM
             * stays latched (rf_inject_burst_active=1, stale idx) with no TX
             * in flight: the chain never completes, and the NEXT TX_FINISH
             * from any later TX is misrouted into the burst branch above,
             * chaining spurious TX_PAIR_15B TXes on stale channels or running
             * burst-promote from an unrelated completion. The keyboard keeps
             * broadcasting, so the re-heard LEN-10 re-runs the accept path and
             * re-arms a fresh burst. */
            rf_abort_pair_burst();
#if RF_TASK_EXECUTOR_TMOS
            /* Codex full-batch review (merge blocker): a burst-active TX_FAIL
             * (rf_inject_burst_active is set from the FIRST pair-AA TX through the
             * session-AA chain, so this covers a failure of EITHER) leaves the
             * keyboard possibly still on the pair AA — either it never got a
             * pair-AA ACK, or the chain had switched to the session AA and it
             * missed the first one. Merely re-arming RX below would listen on the
             * CURRENT (session) AA only and strand it (dongle deaf, N11
             * supervision not yet armed). Defer the dual-AA fresh-pair relisten
             * to task context so we re-hear the keyboard on EITHER AA. (CH570
             * never raises TX_FAIL, so this is inert there even before the
             * #if — but gated for byte-identity.) A fresh-pair burst and an EV10
             * reacquire are mutually exclusive, but guard defensively so the flag
             * never leaks into the EV10 branch below. */
            if (!(rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING)) {
                rf_burst_txfail_fallback = 1u;
            }
#endif
        }
        if (rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING) {
            hal_timer_cancel(HAL_TMR_SLOT_PAIR_ACK);
            hal_event_post(RF_EVT_PAIR_PREP);
            
            break;
        }
        /* Re-arm RX (RX_RESTART -> rf_rearm_rx, Shut+Rx with no Config --
         * the radio keeps its RF_Config across the TX attempt; see the
         * TX_FINISH note above). If this was a failed pair-ACK the keyboard
         * keeps broadcasting and the re-heard LEN-10 re-runs the accept
         * path, whose TX handler does a full reconfigure anyway. TMR0
         * posts the next RF_EVT_POLL independently. */
        hal_event_post(RF_EVT_RX_RESTART);
        break;

    default:
        hal_event_post(RF_EVT_RX_RESTART);
        break;
    }
}

/* ---------- TMOS task event handler ---------- */

/* TMOS task counters retained for UART/status visibility. */

static void rf_arm_post_poll_rx(void)
{
    hal_rf_shut();
    {
        uint8_t post_status = hal_rf_start_rx_primed(
            HAL_RF_CHANNEL_CURRENT, 0,
            (const uint8_t *)rf_poll_buf, RF_POST_POLL_RX_LEN);
        (void)post_status;
    }
}

static uint16_t RF_ProcessEvent(uint8_t task_id, uint16_t events)
{
#if RF_TASK_EXECUTOR_TMOS
    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        if ((pMsg = tmos_msg_receive(task_id)) != NULL) {
            tmos_msg_deallocate(pMsg);
        }
        return events ^ SYS_EVENT_MSG;
    }
#else
    (void)task_id;   /* the pump passes 0; no TMOS message queue exists */
#endif

    /* Deferred bond persist, posted from the radio ISR at burst-promote. Handle
     * it before the high-rate RF_EVT_POLL (875 us cadence) so this one-shot
     * DataFlash write can't be starved by re-posted poll events. */
    if (events & RF_EVT_PERSIST_BOND) {
        rf_persist_bond_task();
        return events ^ RF_EVT_PERSIST_BOND;
    }

    /* Quiesced: consume everything. Nothing below may re-arm the radio once
     * the shutdown ran; late timer fires and re-posted bits land here. */
    if (rf_quiesced) {
        return 0;
    }

    /* Pre-reboot quiesce. Ordered after RF_EVT_PERSIST_BOND so a pending
     * bond write still reaches DataFlash before the radio dies (the update
     * flow promises the bond survives).
     *
     * ORDER MATTERS (codex checkpoint-2 finding): the latch is set FIRST.
     * These handlers run with IRQs on (hal_dispatch.h pump contract), so an
     * RF callback preempting between a shutdown and a later latch-write
     * could observe rf_quiesced == 0 and re-arm the shut radio. With the
     * latch written first, every IRQ-context arm (rf_connected_poll_cb /
     * rf_pair_ack_cb / the PHY sink) is already gated before any teardown
     * happens. A bond persist posted before the latch but not yet
     * dispatched is run inline here (this IS executor context) rather than
     * cancelled, so the cancel sweep below cannot silently drop it. */
    if (events & RF_EVT_QUIESCE) {
        uint8_t slot;
        uint16_t bit;
        rf_quiesced = 1;
        if (rf_bond_persist_pending) {
            rf_persist_bond_task();
        }
        for (slot = 0; slot < HAL_TMR_SLOT_COUNT; slot++) {
            hal_timer_cancel(slot);
        }
        for (bit = RF_EVT_START; bit <= RF_EVT_PERSIST_BOND; bit <<= 1) {
            hal_event_cancel(bit);
        }
#if DONGLE_RF_CRYPT
        /* The crypto events sit above RF_EVT_PERSIST_BOND, outside the sweep. */
        hal_event_cancel(RF_EVT_CRYPT_RX);
        hal_event_cancel(RF_EVT_CRYPT_SESSION);
        rf_crypt_announce_count = 0u;
        rf_crypt_fifo_head = rf_crypt_fifo_tail;
#endif
        hal_rf_shut();
        return 0;
    }

    if (events & RF_EVT_START) {
#if RF_CONFIRM_BEFORE_PERSIST
        rf_confirm_state = RF_CONFIRM_STATE_NONE;   /* N11: clean per-attempt state */
        hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
#endif
#if RF_TASK_EXECUTOR_TMOS
        rf_burst_txfail_fallback = 0u;
#endif
        
        rf_configure(rf_access_addr);
        rf_state = RF_STATE_PAIRING;
        rf_start_rx();
        if (rf_bond_valid) {
            /* Bonded: we are already listening on the session AA (reconnect
             * tried first). Open the ~3 s window; its timer alternates onto the
             * pair AA so a new keyboard can also pair during the window. */
            rf_boot_window_active = 1;
            rf_pair_window_open   = 1;
            rf_boot_window_step   = 0;
            hal_timer_arm(HAL_TMR_SLOT_BOOT_WINDOW, RF_BOOT_WINDOW_TICKS_TSYS,
                          rf_boot_window_cb);
        }
        /* P4: an UNBONDED dongle takes no boot-window timer above, so this cold
         * fresh-pair camp's only re-arm driver is the radio timeout — guard it
         * too. (Bonded already has the boot-window timer; the guard is a benign
         * faster backstop there.) */
        rf_arm_retry_if_failed();
        return events ^ RF_EVT_START;
    }

    if (events & RF_EVT_BOOT_WINDOW) {
        /* Stock-style boot reconnect/pair window. Runs only while bonded and
         * still un-connected. Each ~300 ms tick flips the listen AA between the
         * session AA (reconnect) and the pair AA (accept a new keyboard) for
         * RF_BOOT_WINDOW_STEPS ticks, then settles on the session AA
         * (reconnect-only) and closes the pair window. It never perturbs the
         * connected EV10 supervision: the moment the link promotes to CONNECTED
         * (or an EV10 cycle is in flight) this handler clears its flags and
         * stops re-posting, so it self-disables without touching any promote
         * site. */
        if (!rf_boot_window_active) {
            return events ^ RF_EVT_BOOT_WINDOW;
        }
        if (rf_state != RF_STATE_PAIRING || rf_supervision_ev10_active) {
            rf_boot_window_active = 0;
            rf_pair_window_open   = 0;
            return events ^ RF_EVT_BOOT_WINDOW;
        }
        if (++rf_boot_window_step >= RF_BOOT_WINDOW_STEPS) {
            /* Window done: reconnect-only on the session AA. Pairing a new
             * keyboard now needs a dongle replug to re-open the window. */
            rf_boot_window_active = 0;
            rf_pair_window_open   = 0;
            rf_access_addr = rf_bond_aa;
            rf_channel     = RF_PROTO_RECONNECT_CAMP_CHANNEL;
            rf_start_rx();
            rf_arm_retry_if_failed();   /* P4: closed window has no re-arm
                                         * timer — don't strand deaf */
        } else {
            /* Alternate: odd step = pair AA (new pair), even = session AA
             * (reconnect). rf_start_rx() does RF_Shut + reconfigure + RF_Rx so
             * the AA flip is clean (no deaf mid-flip gap). The initial ~300 ms
             * session-AA camp (before this first tick) already gives a bonded,
             * active keyboard reconnect priority: it re-broadcasts ~19 ms after
             * link loss (conn_timeout/20), well inside that camp (Codex A-priority
             * analysis). This camp NARROWS but does not close B1 (a keyboard slow
             * to reconnect -- sleep/key-wake -- during a bond-replacing re-pair):
             * B1 is a documented OWNER-ACCEPTED WONTFIX (see the classifier note
             * ~line 2246). The fresh candidate-AA path that would close it
             * structurally was designed but deliberately NOT implemented. */
            rf_access_addr = (rf_boot_window_step & 1u)
                                 ? RF_PAIR_ACCESS_ADDR
                                 : rf_bond_aa;
            rf_channel = RF_PROTO_RECONNECT_CAMP_CHANNEL;
            rf_start_rx();
            hal_timer_arm(HAL_TMR_SLOT_BOOT_WINDOW, RF_BOOT_WINDOW_TICKS_TSYS,
                          rf_boot_window_cb);
        }
        return events ^ RF_EVT_BOOT_WINDOW;
    }

#if RF_CONFIRM_BEFORE_PERSIST
    if (events & RF_EVT_CONFIRM_TIMEOUT) {
        /* CODEREVIEW N11 deadline. Claim the per-attempt state under a brief IRQ
         * mask (the RF sink advances WAIT_RX -> WAIT_REARM and can preempt this
         * task), then act OUTSIDE the mask. Placed before the high-rate RX_RESTART/
         * POLL events so repeated non-confirm RX cannot starve it.
         *   - WAIT_RX (never confirmed): abandon the unconfirmed session (its bond
         *     was never written to DataFlash) -> fresh-pair relisten. No connected
         *     RX was accepted while WAIT_RX (the FIRST valid one transitions to
         *     WAIT_REARM), so no HID was forwarded here -> no keys-up teardown is
         *     owed (unlike a confirmed-link loss).
         *   - WAIT_REARM (peer HAS confirmed, but the post-confirm RX rearm has not
         *     yet succeeded by the deadline): the pair IS accepted, so PROCEED --
         *     arm the durable write and hand off to normal supervision, which (unlike
         *     a fresh-pair teardown) owns the connected keys-up/reacquire path. This
         *     both gives WAIT_REARM a deadline (Codex B3) and avoids stranding a
         *     forwarded key-down without a keys-up (Codex fix re-review).
         *   - NONE: the RX_RESTART handler already claimed and posted the persist. */
        uint32_t irq = __risc_v_disable_irq();
        uint8_t st = rf_confirm_state;
        rf_confirm_state = RF_CONFIRM_STATE_NONE;
        if (st == RF_CONFIRM_STATE_WAIT_RX) {
            /* Codex fix re-review: atomically leave CONNECTED here, INSIDE the IRQ
             * mask, before the teardown. Otherwise a HID packet arriving between the
             * claim and rf_return_to_fresh_pair's later rf_state write would still
             * see CONNECTED, be forwarded as a key-down, and then be torn down with
             * no keys-up (stuck key). With rf_state left CONNECTED only while the
             * mask holds, the sink's connected branch can't run after the claim, so
             * no key is forwarded on this teardown -> no keys-up is owed. */
            rf_state = RF_STATE_PAIRING;
            /* Codex full-batch review: clear the EV10-active flag in the SAME
             * masked claim as the rf_state flip. Otherwise a pending EV10
             * TX_FINISH between here and rf_return_to_fresh_pair's later clear
             * (~line 1640) still satisfies the EV10 promote predicate and
             * transiently restores CONNECTED, reopening the HID-without-keys-up
             * window this critical section exists to close. */
            rf_supervision_ev10_active = 0;
        }
        (void)__risc_v_enable_irq(irq);
        if (st == RF_CONFIRM_STATE_WAIT_RX) {
            rf_bond_persisted = 0;   /* the tentative bond was never persisted */
            rf_return_to_fresh_pair();
        } else if (st == RF_CONFIRM_STATE_WAIT_REARM) {
            rf_arm_bond_persist();            /* confirmed -> make it durable */
            /* Codex fix re-review: only (re)arm the first-supervision timer if we
             * are still the CONNECTED owner of the radio. If a persistent post-
             * confirm RX-arm failure dropped us into EV10 reacquire (rf_state ==
             * PAIRING, rf_supervision_ev10_active) before this deadline fired,
             * arming the short ~ms first-supervision here would clobber the
             * 2.125 s reacquire watchdog and force a premature give-up. Leave the
             * reacquire watchdog intact in that case. */
            if (rf_state == RF_STATE_CONNECTED && !rf_supervision_ev10_active) {
                rf_arm_connected_supervision(1);  /* hand off to normal supervision */
            }
        }
        return events ^ RF_EVT_CONFIRM_TIMEOUT;
    }
#endif

    if (events & RF_EVT_RX_RESTART) {
#if RF_TASK_EXECUTOR_TMOS
        /* Codex full-batch review (merge blocker): a burst-active async TX_FAIL
         * asked (from the radio callback) for the dual-AA fresh-pair relisten
         * instead of a session-AA-only re-arm. Do it here in task context —
         * rf_return_to_fresh_pair re-camps the boot window alternating pair/
         * session AA, so a keyboard that missed the first pair-AA ACK (still on
         * the pair AA) is re-heard rather than stranded. Takes precedence over
         * the normal re-arm below. */
        if (rf_burst_txfail_fallback) {
            rf_burst_txfail_fallback = 0u;
            rf_return_to_fresh_pair();
            return events ^ RF_EVT_RX_RESTART;
        }
#endif
        /* Stock-match: during the EV10 pair-channel scan the single per-hop
         * RF_Rx holds RX for the full 30 ms dwell. Stock never re-arms
         * in-callback, so drop any RX_RESTART churn (from CRC/noise invalids
         * or non-catch valids) while scanning -- PAIR_PREP is the sole arm.
         * Re-arming here would RF_Shut+RF_Rx mid-dwell and gap the listen
         * window, dropping the occasional LEN-10. */
        if (rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING) {
            return events ^ RF_EVT_RX_RESTART;
        }
        /* RX-after-RX: fast path, no Config. Closes the "keyboard reply lands
         * while we're reconfiguring" dead window. (A TX→RX reconfigure flag was
         * declared for speculative builds but never had a writer, so it was
         * dead; the post-TX paths that genuinely need a full reconfigure call
         * rf_start_rx() directly -- connected poll via RF_EVT_POST_POLL_RX, EV10
         * and burst via explicit rf_start_rx. Removed.) */
#if RF_CONFIRM_BEFORE_PERSIST
        /* CODEREVIEW N11 (Codex impl-review): snapshot the confirm state at handler
         * ENTRY, BEFORE rf_rearm_rx. Persist only if we were ALREADY WAIT_REARM
         * here — that proves the rf_rearm_rx() below happens AFTER the confirm was
         * observed. Otherwise a confirm landing DURING rf_rearm_rx (entry was
         * WAIT_RX) would let us mistake this pre-confirm arm for the required
         * post-confirm arm; instead it is handled by the RX_RESTART the sink posts
         * with the confirm, which enters WAIT_REARM. */
        uint8_t n11_wait_rearm_at_entry =
            (rf_confirm_state == RF_CONFIRM_STATE_WAIT_REARM);
#endif
        rf_rearm_rx();
        /* P4: this handler IS the self-heal loop every camp/timeout re-arm
         * funnels through. If the re-arm fails, the radio timeout that would
         * re-drive us never fires, so reschedule ourselves off the guard. */
        rf_arm_retry_if_failed();
#if RF_CONFIRM_BEFORE_PERSIST
        /* Only NOW (after a SUCCESSFUL post-confirm arm) commit the durable bond,
         * so the flash erase/write never runs while RX is deaf (RF_EVT_PERSIST_BOND
         * outranks this event in the dispatch chain). If the re-arm FAILED, stay in
         * WAIT_REARM — the P4 retry loop re-enters here on the next successful arm;
         * and if the confirm deadline fires first, its WAIT_REARM branch persists
         * anyway (the pair is already confirmed) and hands off to supervision. */
        if (n11_wait_rearm_at_entry
            && rf_confirm_state == RF_CONFIRM_STATE_WAIT_REARM
            && rf_last_arm_status == 0u) {
            rf_confirm_state = RF_CONFIRM_STATE_NONE;
            hal_event_cancel(RF_EVT_CONFIRM_TIMEOUT);
            rf_arm_bond_persist();
            rf_arm_connected_supervision(1);   /* switch to normal supervision */
        }
#endif
        return events ^ RF_EVT_RX_RESTART;
    }

    if (events & RF_EVT_POST_POLL_RX) {
        /* Stock dispatcher event 0x04: RF_Shut + RF_Rx(buf, 10, 0xff,
         * 0xff), no RF_Config. We rely on the radio's just-completed
         * TX leaving AA / channel / CRC primed for RX. The 10-byte
         * buffer is the same gp-0x604-equivalent control byte storage
         * used for empty-poll TX, sized here for the stock length-10
         * RX arm. */
#if RF_CONFIRM_BEFORE_PERSIST
        /* CODEREVIEW N11 (Codex impl-review; widened to CH570 per review): stale-
         * event guard. A POST_POLL_RX co-dispatched with a confirm timeout that
         * fell back to PAIRING (via rf_return_to_fresh_pair, which camps a fresh
         * RX) must NOT re-prime the connected post-poll RX over that camp. Now
         * that CH570 runs the confirm-timeout fallback too, it needs this guard.
         * Only arm while still CONNECTED. */
        if (rf_state != RF_STATE_CONNECTED) {
            return events ^ RF_EVT_POST_POLL_RX;
        }
#endif
        rf_arm_post_poll_rx();
        return events ^ RF_EVT_POST_POLL_RX;
    }

#if DONGLE_RF_CRYPT
    if (events & RF_EVT_CRYPT_SESSION) {
        /* Task context: mint a fresh 32-bit session nonce, reset the replay
         * window and the silence guard, drop any stale queued frames, and
         * pre-build the authenticated session-nonce frame so the next few polls
         * announce it. On a build fault leave announce_count at 0 so no stale
         * frame is ever transmitted. */
        if (rf_crypt_bond_enc) {
            rf_crypt_announce_count = 0u;
            /* Discard stale frames. A frame the IRQ sink pushes concurrently with
             * this reset can survive it, but it predates the just-minted session,
             * so rf_crypt_rx computes the tag under the new session_id and drops it
             * on the MAC check -- the reset is an optimization, the tag is the
             * guarantee. (No new-session frame exists yet: the keyboard only learns
             * session_id from the announce that follows.) */
#if RF_CRYPT_DIAG_PREV_SESSION
            {
                /* Count what the flush destroys. Frames queued but never
                 * verified are indistinguishable from frames never sent in
                 * every other counter, and this path runs on EVERY mint. */
                uint8_t fh = rf_crypt_fifo_head, ft = rf_crypt_fifo_tail;
                rf_crypt_flush_drop +=
                    (uint32_t)((ft + RF_CRYPT_FIFO_N - fh) % RF_CRYPT_FIFO_N);
            }
#endif
            rf_crypt_fifo_head = rf_crypt_fifo_tail;
            rf_crypt_frames_since_ok = 0u;
            rf_crypt_new_session(rf_crypt_gen_session_id());
            if (rf_crypt_build_session_frame(rf_poll_buf[0], rf_crypt_session_tx)
                    == RF_CRYPT_OK) {
                rf_crypt_announce_count = RF_CRYPT_ANNOUNCE_POLLS;
            }
        }
        return events ^ RF_EVT_CRYPT_SESSION;
    }

    if (events & RF_EVT_CRYPT_RX) {
        /* Task context: drain the frame FIFO the IRQ sink filled, verifying and
         * decrypting each. Nothing reaches USB without a verified CCM tag. */
        while (rf_crypt_fifo_head != rf_crypt_fifo_tail) {
            uint8_t h = rf_crypt_fifo_head;
            uint8_t otag = 0u, obody[RF_CRYPT_MAX_BODY], on = 0u;
            rf_crypt_status_t st;
#if RF_CRYPT_DIAG_PREV_SESSION
            /* Bench: mark the CCM window so the RF status callback can count
             * BB/LLE interrupts that land mid-verify (they touch AES_STA). */
            rf_crypt_in_aes = 1u;
#endif
            st = rf_crypt_rx(rf_crypt_fifo_buf[h],
                             rf_crypt_fifo_len[h],
                             &otag, obody, &on);
#if RF_CRYPT_DIAG_PREV_SESSION
            rf_crypt_in_aes = 0u;
#endif
            rf_crypt_fifo_head = (uint8_t)((h + 1u) % RF_CRYPT_FIFO_N);

            if (st == RF_CRYPT_OK) {
                rf_crypt_ok_count++;
                rf_crypt_frames_since_ok = 0u;   /* authenticated liveness */
                rf_crypt_announce_count = 0u;    /* keyboard has the session nonce */
                if (rf_hid_callback) {
                    rf_hid_callback(otag, obody, on);
                }
            } else {
#if RF_CRYPT_DIAG_PREV_SESSION
                rf_crypt_drops++;
#endif
                if ((unsigned)st < 6u) {
                    rf_crypt_drop_reason[(unsigned)st]++;
                }
                if (st == RF_CRYPT_FAULT_ENGINE) {
                    /* hal_aes.h: an engine wedge is a fatal fault of the radio
                     * path. NEVER revert to the plaintext dispatch (that would let
                     * a forgery through) -- keep the bond encryption-required so
                     * every frame stays fail-closed, release the keyboard so a
                     * held key cannot stick, and let a reconnect re-mint and retry
                     * (the key stays installed; polls stay clear). */
                    rf_send_keys_up_on_link_loss();
                }
            }
        }
        return events ^ RF_EVT_CRYPT_RX;
    }
#endif /* DONGLE_RF_CRYPT */

    if (events & RF_EVT_PAIR_PREP) {
        rf_send_pair_prep();
        return events ^ RF_EVT_PAIR_PREP;
    }

    if (events & RF_EVT_SEND_PAIR_ACK) {
        /* P2.3(ii-d): dispatch through the seam's cb table (registered at the
         * PAIR_ACK arm sites). The armed gate suppresses an event the TMR0 ISR
         * posted before a cancel; rf_send_pair_ack keeps its own state guard. */
        hal_timing_ch592_dispatch(HAL_TMR_SLOT_PAIR_ACK);
        return events ^ RF_EVT_SEND_PAIR_ACK;
    }


    if (events & RF_EVT_POLL) {
        if (rf_state == RF_STATE_CONNECTED) {
            /* P2.3(ii-d): dispatch through the seam's cb table — invokes
             * rf_connected_poll_cb -> rf_send_poll. The armed gate suppresses
             * a POLL the TMR0 ISR posted just before a teardown cancel. */
            hal_timing_ch592_dispatch(HAL_TMR_SLOT_CONNECTED_POLL);



            /* No self-reschedule — TMR0 CYC_END is the sole pacer in
             * CONNECTED state. Posted event coalescing is benign: if
             * TMR0 fires during an in-flight TX/RX cycle, the next
             * RF_EVT_POLL bit is already set and we simply serve one
             * poll for that window. */
        }
        return events ^ RF_EVT_POLL;
    }

    if (events & RF_EVT_TX_PAIR_15) {
        /* Send the 15-byte pair-ACK on the channel we just heard the
         * keyboard's broadcast on. The full RF_Shut + rf_configure +
         * RF_Tx sequence mirrors the sniff-hop path that's known to
         * work cleanly across mode transitions — skipping the
         * rf_configure left the radio in a bad state and crashed the
         * board on the first TX. After TX_FINISH the regular
         * RF_EVT_RX_RESTART path restarts RX (also via the full
         * shut+configure+rx sequence so we don't crash there either). */
        uint8_t status;
        hal_rf_shut();
        rf_configure(rf_access_addr);
        status = hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                                 rf_pair_ack15, sizeof(rf_pair_ack15));
        /* P1': a failed StartTx here raises no TX_FINISH, so the pair-ACK chain
         * stalls with RX un-armed. Recover to the boot-window relisten. */
        if (rf_pair_tx_recover_if_failed(status)) {
            return events ^ RF_EVT_TX_PAIR_15;
        }
        
        return events ^ RF_EVT_TX_PAIR_15;
    }

    if (events & RF_EVT_TX_PAIR_15B) {
        /* Burst-mode chained TX. rf_inject_burst_idx was incremented by
         * the IRQ-side TX_FINISH before posting this event, so values
         * 1..5 here map to burst slots 0..4 (pair LUT, two rounds). The
         * radio config
         * switches from pair AA (where the first TX landed) to the
         * keyboard's negotiated session AA (encoded in the first 4
         * bytes of rf_pair_ack15, MSB-first). The keyboard's state==1
         * MAC-match check fires when LEN==15, type<5, and rxBuf[11..16]
         * matches the stored peer MAC. Same buffer = same MAC = match
         * (assuming the first TX successfully ran the first-pair branch
         * and stored that MAC).
         *
         * Guard on rf_inject_burst_active: a TX_FAIL or a teardown clears the
         * burst state and cancels this event, but a copy already dispatched
         * by TMOS must not resurrect the chain (it would flip rf_access_addr
         * to the session AA and TX on a stale channel while the FSM believes
         * it is camping on the pair AA). */
        uint8_t idx = (uint8_t)(rf_inject_burst_idx - 1);
        if (rf_inject_burst_active && idx < RF_BURST_TX_COUNT) {
            uint32_t session_aa = rf_protocol_pair_ack_session_aa(rf_pair_ack15);
            uint8_t status;
            hal_rf_shut();
            rf_access_addr = session_aa;
            rf_channel     = rf_pair_channels[idx % RF_PROTO_PAIR_CHANNEL_COUNT];
            rf_configure(rf_access_addr);
            /* Anchor the stock channel formula at FIRST burst TX time,
             * not at burst-promote (~200 ms later). The keyboard
             * anchors its hop helper at `gp+0x78 = RTC_at_state2 - 13`
             * — state-2 fires on receipt of the first valid 15-byte
             * (which is this very burst, as it lands on the
             * pair-broadcast channel). Mirror that exactly: snapshot
             * RTC right before RF_Tx and back-date by 13 ticks (the
             * same `(LEN >> 3) + 0xC` that the keyboard's
             * 0x20000D34 seed helper uses). Then DON'T re-snapshot
             * at burst-promote.
             *
             * Pattern copied from fw-ch582f rf_task.c:2212
             * (rf_stock_seed_pair_phase_delta(13)). Reviewer flagged
             * the original burst-promote snapshot as a ~200 ms anchor
             * mismatch on 2026-05-20 — see audit doc G3 and
             * fwch592f-bench-2026-05-20 memory. */
            if (rf_inject_burst_idx == 1) {
                /* 2026-05-20 final: stock RE (deeper_hop_trace.md:1134)
                 * shows stock keyboard initializes prev_idx = type_tag % 5
                 * = 2 (NOT 3). Tested both values on bench; neither
                 * restores connectivity, but the %5 form is correct per
                 * stock — keeping that as the documented default. */
                rf_hop.prev_idx = rf_proto_hop_seed(rf_pair_ack15[4]);
                rf_hop.last = rf_hop_backdate(rf_hop_read(), 13u);
                stock_seed_burst_applied = rf_inject_burst_idx;
            }
            status = hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                                     rf_pair_ack15, sizeof(rf_pair_ack15));
            /* P1': failed StartTx -> no TX_FINISH -> stalled burst; recover. */
            if (rf_pair_tx_recover_if_failed(status)) {
                return events ^ RF_EVT_TX_PAIR_15B;
            }
            
        }
        return events ^ RF_EVT_TX_PAIR_15B;
    }

    if (events & RF_EVT_TIMEOUT) {
#if DONGLE_RF_CRYPT
        /* Authenticated-HID silence guard fired (rf_send_poll): an active
         * encrypted bond saw a flood of unauthenticated frames, which keep
         * supervision's RX stamp fresh so the normal lapse checks below would
         * just re-arm and never release. FORCE the reacquire to tear the poisoned
         * session down and emit keys-up. */
        if (rf_crypt_force_release) {
            rf_crypt_force_release = 0u;
            if (rf_state == RF_STATE_CONNECTED && !rf_supervision_ev10_active) {
                rf_enter_stock_reacquire();
                return events ^ RF_EVT_TIMEOUT;
            }
        }
#endif
        if (rf_supervision_ev10_active) {
            rf_stock_reacquire_giveup();
            return events ^ RF_EVT_TIMEOUT;
        }
        if (rf_state == RF_STATE_CONNECTED) {
            /* P3a: the stock threshold is derived in protocol ticks and
             * scaled to Tsys ONCE here (exact x1875); the lapse measurement
             * below is plain 2^32-modular hal_now() arithmetic. */
            uint32_t threshold_tsys =
                rf_supervision_stock_confirmed_lapse_ticks(
                    rf_active_conn_timeout(),
                    CH592_STOCK_EV10_LAPSE_MARGIN_TICKS,
                    CH592_STOCK_EV10_RECOVERY_CONFIRM_WINDOWS)
                * HAL_TICKS_PER_PROTO_TICK;
            if (rf_stock_first_supervision_armed) {
                rf_stock_first_supervision_armed = 0;
                /* Snapshot the stamp BEFORE reading the clock (codex P3a
                 * #1): the radio IRQ-tail sink can preempt this task-context
                 * code and refresh the stamp between the two loads — with
                 * the clock read first that manufactures last > now and the
                 * unsigned delta underflows to ~2^32 (a false lapse). Stamp-
                 * first, a preempting RX only makes the sample AGE by the
                 * preemption window: idle is over-estimated by microseconds,
                 * lands in grace, and the fresh RX is seen next check. */
                uint32_t last_rx = rf_last_conn_rx_tsys;
                if (last_rx != 0) {
                    uint32_t now_tsys = hal_now();
                    uint32_t idle_tsys = now_tsys - last_rx;
                    if (idle_tsys < threshold_tsys) {
                        rf_arm_connected_supervision(0);
                        return events ^ RF_EVT_TIMEOUT;
                    }
                }
                rf_enter_stock_reacquire();
                return events ^ RF_EVT_TIMEOUT;
            }
            /* Same stamp-before-clock load order as the first-armed leg. */
            uint32_t last_rx = rf_last_conn_rx_tsys;
            if (last_rx != 0) {
                uint32_t now_tsys = hal_now();
                uint32_t idle_tsys = now_tsys - last_rx;
                if (idle_tsys < threshold_tsys) {
                    rf_arm_connected_supervision(0);
                    return events ^ RF_EVT_TIMEOUT;
                }
            }
            rf_enter_stock_reacquire();
            return events ^ RF_EVT_TIMEOUT;
        }
        return events ^ RF_EVT_TIMEOUT;
    }

    return 0;
}

/* ---------- Internal helpers ---------- */

/* BB IRQ entry counter — referenced from src/bb_irq_trampoline.S. Declared
 * here in C so the linker places it in our application BSS rather than
 * aliasing it with the BLE library's COMMON-allocated rfConfig (which lands
 * at the same address if bb_irq_count is declared inside .bss in assembly). */

static void rf_configure(uint32_t access_addr)
{
    /* P2.4: the rfConfig_t construction (minimal, stock-matching, FH fields
     * zero -- OQ7 Track 2 part 6) lives in hal_rf_ch592.c. The RF_Config ->
     * RF_SetChannel(rf_channel) ordering is preserved across the two seam
     * calls (stock-compatible A/B path: Channel=0 in the config, explicit
     * retune after). */
    hal_rf_configure(access_addr);
    hal_rf_set_channel(rf_channel);
}

__HIGH_CODE
static void rf_start_rx(void)
{
    /* Full reset sequence: Shut + reconfigure + Rx. NOTE the reconfigure is
     * belt-and-braces on this cold/AA-change path, NOT a radio requirement
     * for RX-after-TX: the validated stock-shape hot path (rf_rearm_rx after
     * every poll TX) does Shut+Rx with no Config ~1k×/s — RF_Config state
     * persists across TX. Callers that change rf_access_addr rely on the
     * reconfigure here. */
    hal_rf_shut();
    rf_configure(rf_access_addr);
    {
        uint8_t rx_status = hal_rf_start_rx(HAL_RF_CHANNEL_CURRENT, 0);
        rf_last_arm_status = rx_status;   /* P4: seam to the camp guard */
        (void)rx_status;
    }
}

__HIGH_CODE
static void rf_rearm_rx(void)
{
    /* Fast RX-after-RX re-arm. The radio's AA / channel / CRC state
     * is already primed; skipping rf_configure() removes the RF_Config
     * call (and its UART print), narrowing the post-RX dead window
     * from ~ms to tens of microseconds. Use this on the unchanged-state path
     * (RF_EVT_RX_RESTART). Keep rf_start_rx() for TX→RX transitions or
     * channel/AA changes. */
    hal_rf_shut();
    {
        uint8_t rx_status = hal_rf_start_rx(HAL_RF_CHANNEL_CURRENT, 0);
        rf_last_arm_status = rx_status;   /* P4: seam to the camp guard */
        (void)rx_status;
    }
}

/* ---------- TMR0 pacing helpers ---------- */

/* The TMR0 hardware primitive (rf_tmr0_start/stop) and the CYC_END ISR shell
 * moved into hal_timing_ch592.c (P2.3 ii-e) — the seam owns the timer
 * hardware. What stays here is the protocol decision: which TMOS event a
 * TMR0 fire posts for the current rf_state. Called from the seam's
 * TMR0_IRQHandler in IRQ context (post-only — the heavy lifting runs in task
 * context; CH59x RF calls cannot run from a timer ISR). */
#if RF_TASK_EXECUTOR_TMOS
/* The protocol half of the CH59x TMR0 CYC_END ISR (the hal_timing_ch592
 * shim's shell calls back here). The polled executor has no TMR0 shell —
 * its slot callbacks dispatch directly from the st_* mux. */
__HIGH_CODE
void rf_tmr0_isr_dispatch(void)
{
    switch (rf_state) {
    case RF_STATE_PAIRING:
        if (rf_supervision_ev10_active) {
            hal_event_post(RF_EVT_SEND_PAIR_ACK);
        } else {
            rf_tmr0_stop();
        }
        break;
    case RF_STATE_CONNECTED:
        hal_event_post(RF_EVT_POLL);
        break;
    default:
        /* IDLE / unexpected — stop the timer defensively so we don't keep
         * burning IRQs with nothing to post. PAIRING (non-EV10) should never
         * start TMR0 (no peer MAC, no pair-ACK buf populated); if it's
         * running here, we've drifted. */
        rf_tmr0_stop();
        break;
    }
}
#endif /* RF_TASK_EXECUTOR_TMOS */

/* conn_interval × 1875 Tsys counts per period. Falls back to the stock
 * default (28) before a pair-broadcast has populated rf_conn_interval,
 * so BOOT_CONNECTED and any pre-pair retry path still get a sane
 * steady-state rate instead of TMR0(0). */
static uint32_t rf_tmr0_steady_count(void)
{
    uint16_t iv = rf_conn_interval ? rf_conn_interval : RF_TMR0_DEFAULT_INTERVAL;
    return (uint32_t)iv * HAL_TICKS_PER_PROTO_TICK;
}

static uint32_t rf_connected_poll_delay_count(uint32_t delay_ticks)
{
    uint16_t iv = rf_conn_interval ? rf_conn_interval : RF_TMR0_DEFAULT_INTERVAL;
    uint32_t ticks = delay_ticks;

    if (ticks == 0u) {
        ticks = iv;
    }
    return ticks * HAL_TICKS_PER_PROTO_TICK;
}

/* Stock event 0x20 pair-window pre-phase (EV10 reacquire scan only since
 * R1b ii): hop the pair LUT and call
 * RF_Rx(...,6,...). The exact RF-side meaning of this RF_Rx(6) call is
 * still open, but the dispatcher shape is validated. Keep it as a
 * separate step from the 15-byte event-0x40 TX so we can test whether
 * the missing pre-phase was the reason our first 15-byte never stuck. */
__HIGH_CODE
static void rf_send_pair_prep(void)
{
    uint8_t status;

    if (!(rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING)) {
        return;
    }

    rf_channel = rf_pair_channels[rf_pair_prep_idx % RF_PROTO_PAIR_CHANNEL_COUNT];
    rf_pair_prep_idx = (uint8_t)((rf_pair_prep_idx + 1) % 3);  /* == RF_PROTO_PAIR_CHANNEL_COUNT; signed literal kept for byte-identical codegen */

    /* Hop-clock keepalive: pair-prep rotates throughout PAIRING and the EV10
     * scan, keeping consecutive rf_hop_read() calls far below the 71.6 s
     * hal_now() wrap while any hop timestamp is live (no-op cost on the RTC
     * fallback). */
    (void)rf_hop_read();

    hal_rf_shut();
    hal_rf_set_channel(rf_channel);
    status = hal_rf_start_rx_primed(HAL_RF_CHANNEL_CURRENT, 0,
                                    rf_pair_prep_buf,
                                    sizeof(rf_pair_prep_buf));
    (void)status;

    

    /* Match stock event 0x20's self-reschedule cadence while pairing
     * remains open. This keeps rotating the pair-window context
     * independently of TMR0's event-0x40 pacing. During an EV10 re-acquire
     * the dwell is the stock scan cadence (RF_SUPERVISION_STOCK_SCAN_TMOS),
     * separate from the initial-pairing scan. */
    if (rf_supervision_ev10_active) {
        hal_event_post_delayed(RF_EVT_PAIR_PREP,
            RF_SUPERVISION_STOCK_SCAN_TMOS * HAL_TMOS_UNIT_TICKS);
    } else
    hal_event_post_delayed(RF_EVT_PAIR_PREP,
                           RF_PAIR_PREP_TICKS * HAL_TMOS_UNIT_TICKS);
}

/* TX the 15-byte pair-completion packet. Matches stock event 0x40's
 * shape as documented in PROTOCOL.md §"Dongle event 0x40":
 *
 *   RF_Shut + TMR0(conn_interval * 1875) + RF_Tx(buf, 15, 0xff, 0xff)
 *
 * Critically, stock's event 0x40 does *not* call RF_SetChannel — it
 * TXes on whichever channel was last set. With rf_send_pair_prep()
 * wired in, that means the 15-byte now inherits the pair-LUT channel
 * chosen by the stock-shaped pre-phase rather than whatever channel
 * the broadcast RX happened to leave us on.
 *
 * Earlier versions of this helper rotated across {8, 17, 26} hop-style
 * (matching event 0x20's shape instead of event 0x40's), and layered a
 * synthetic SESSION_ACK sweep on top. Both were speculative; removing
 * them gets us back to the stock shape, which is the narrowest change
 * that might actually close the handshake. */
__HIGH_CODE
static void rf_send_pair_ack(void)
{
    /* Stale-event guard: SEND_PAIR_ACK can be posted by a TMR0 IRQ during an
     * EV10 cycle that ends before task context dispatches the event; without
     * this guard the stale event would TX a spurious 15-byte. The EV10
     * re-key is this function's only caller context. */
    if (!(rf_supervision_ev10_active && rf_state == RF_STATE_PAIRING)) {
        return;
    }

    /* Re-arm TMR0 to steady state (conn_interval × 1875 Tsys). Matches
     * stock event 0x40's inline TMR0_TimerInit call. First entry here
     * replaces the 300 µs bootstrap arm; subsequent entries keep TMR0
     * phase-locked to the TX. NOTE: the promote-to-CONNECTED at the
     * pair-completion TX_FINISH INHERITS this arm without re-arming
     * (phase-lock) — the PAIR_ACK slot's running timer becomes the
     * CONNECTED_POLL cadence by ownership transfer, never cancel/arm. */
    hal_timer_arm_periodic(HAL_TMR_SLOT_PAIR_ACK, rf_tmr0_steady_count(),
                  rf_pair_ack_cb);

    hal_rf_shut();
    (void)hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                          rf_pair_ack_buf, sizeof(rf_pair_ack_buf));
    if (rf_supervision_ev10_active) {
        /* Stamp the re-key TX instant so the EV10 restore can phase the first
         * resumed poll to rekey_TX + 1 interval. */
        rf_ev10_rekey_tx_hop = rf_hop_read();
    }

}

/* OQ7 Track 2d state lives at file scope above (R32_RTC_CNT_32K_ADDR
 * et al) so the burst-promote branch can initialize it. The formula
 * itself is implemented below in rf_send_poll. */

/* CONNECTED-mode LEN=1 poll TX using the stock hop formula on the hop clock
 * (HSE-derived protocol ticks). */
static void rf_send_poll(void)
{
    /* Defense-in-depth (v0.95 half-open finding): every legitimate caller is
     * CONNECTED-gated upstream, but this function fires a raw RF TX — a stale
     * event or timer dispatch reaching it in any other state must be a no-op,
     * not a rogue poll on a dead grid. */
    if (rf_state != RF_STATE_CONNECTED) {
        return;
    }

#if DONGLE_RF_CRYPT
    /* Authenticated-HID silence guard. rf_send_poll is the TMR ISR on CH570, so
     * do only IRQ-safe work here: if an active encrypted bond has seen a run of
     * connected frames with none verifying (a jammer keeping supervision alive
     * with unauthenticated traffic), post RF_EVT_TIMEOUT -- the existing
     * link-loss path releases the keyboard and re-enters reacquire in task
     * context. Genuine silence (no frames) is already handled by supervision. */
    if (rf_crypt_bond_enc && rf_crypt_frames_since_ok >= RF_CRYPT_SILENCE_FRAMES) {
        rf_crypt_frames_since_ok = 0u;
        rf_crypt_force_release = 1u;   /* make the timeout handler force reacquire */
        hal_event_post(RF_EVT_TIMEOUT);
    }
#endif

    {
        /* The shared stock hop formula (rf_protocol.h) -- the exact block
         * that used to live here (firmware.bin runtime 0x20000ff6..0x1016,
         * incl. the +elapsed repeat-correction anchor) now single-sourced
         * and host-tested; R3 ported CH570 onto the same call. */
        rf_proto_hop_t h = { rf_hop.last, rf_hop.prev_idx };
        uint32_t now = rf_hop_read();
        uint8_t idx;

        idx = rf_proto_hop_step(&h, now, rf_conn_interval);
        rf_hop.last = h.last;
        rf_hop.prev_idx = h.prev_idx;
        rf_channel = rf_data_channels[idx];
    }

    hal_rf_set_channel(rf_channel);
    hal_rf_shut();
    rf_stock_stage_poll_arena();
    /* RF_Tx actively transmits the poll. (An earlier experiment tried
     * RF_Rx here based on a misread of what the stock firmware's
     * 0x8c38 does, but CH58x RF_Rx arms receive mode with the buffer
     * as an auto-response payload — the opposite of what a master
     * poll needs. A master must TX first; the slave replies.) */
    /* Stock-shaped: consume the app-payload queue if present. Send
     * [ctrl][buf[0]][buf[1]] (3 bytes) and clear pending. Otherwise
     * send the default LEN=1 [ctrl] poll. Per 2026-05-17 user
     * feedback, this matches stock event 0x02 behaviour (queued app
     * packet OR LEN=1 poll, same slot, same cadence). */
    uint8_t status;
#if DONGLE_RF_CRYPT
    if (rf_crypt_announce_count) {
        /* Announce the fresh session nonce in this poll slot instead of a poll.
         * The 14-byte frame is pre-built in task context (RF_EVT_CRYPT_SESSION);
         * an authenticated RX clears the count early. Sent on the session AA where
         * the keyboard listens; the poll grid/cadence is otherwise untouched. */
        rf_crypt_announce_count--;
        status = hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                                 rf_crypt_session_tx, RF_CRYPT_LEN_SESSION);
        (void)status;
    } else
#endif
    if (rf_app_tx_pending) {
        /* static lifetime: RF_Tx() may read the payload after this function's
         * stack frame returns (codex); rf_send_poll runs only in TMOS task
         * context, so a function-static is safe and matches CH570's static
         * connected_app_payload[]. */
        static uint8_t buf3[3];
        buf3[0] = rf_poll_buf[0];
        buf3[1] = rf_app_tx_buf[0];
        buf3[2] = rf_app_tx_buf[1];
        status = hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                                 buf3, sizeof(buf3));
        /* Decrement UNCONDITIONALLY: the relay is sent exactly rf_app_tx_pending
         * times (bounded), then normal LEN=1 polls resume. The success-gated
         * form (CH570 parity) re-sends the LEN=3 relay on EVERY poll whenever
         * RF_Tx() returns non-zero -- which starves the connected poll exchange,
         * so supervision tears the link down (bench-observed 2026-06-14: the
         * relay fires once, then the link goes silent and never recovers). On
         * CH592 RF_Tx()'s return is not a clean per-frame ACK signal, so gating
         * on it is wrong here. A relay missed on a genuine TX failure is re-sent
         * by the next host LED change / reconnect re-sync anyway. */
        (void)status;
        rf_app_tx_pending--;
    } else {
        status = hal_rf_start_tx(HAL_RF_CHANNEL_CURRENT, rf_access_addr,
                                 rf_poll_buf, RF_POLL_TX_LEN);
    }

    /* Print the first few polls so we can confirm cadence + channel
     * rotation + RF_Tx status. After that, stay quiet (TMR0 at ~1140
     * Hz would flood UART1 at 115200 baud). */
    (void)status;
}

/* ---------- DataFlash bond persistence ---------- */

/* Master switch (default 1): load the bond identity from the DataFlash record
 * (bond.c) at boot, falling back to the compiled identity when absent. Set 0 to
 * force the pre-persistence behavior. */

/* When 1, persist the compiled identity to DataFlash on first boot if no record
 * exists. Default 0: provisioning is an explicit host step
 * (tools/provision_bond.py), and we avoid a DataFlash erase/write during boot. */

/* Auto-pair trigger: when 1 and no valid bond record is present at boot, listen
 * on the pair-broadcast access address so the first keyboard that broadcasts is
 * paired (write-bond-on-pair then persists it, so the next boot reconnects). A
 * provisioned dongle is unaffected. Needs the pair-ACK inject path. */
/* Per-pair session access address: when 1, a fresh pair advertises a newly
 * generated BLE-valid AA (stock pattern 0x6A____E6 / 0xAC____CE) instead of the
 * fixed compiled/bond AA, so two dongle/keyboard pairs don't share an AA. A
 * provisioned dongle keeps its bond AA (no generation). */

/* xorshift32 seeded lazily from the chip UID (unique per dongle) ^ RTC. */
static uint32_t rf_aa_rng_state;
static uint16_t rf_aa_rng16(void)
{
    uint32_t x = rf_aa_rng_state;
    if (x == 0) {
        uint8_t uid[8] __attribute__((aligned(4)));
        GET_UNIQUE_ID(uid);
        x = ((uint32_t)uid[0] | ((uint32_t)uid[1] << 8)
           | ((uint32_t)uid[2] << 16) | ((uint32_t)uid[3] << 24))
          ^ ((uint32_t)uid[4] | ((uint32_t)uid[5] << 8)
           | ((uint32_t)uid[6] << 16) | ((uint32_t)uid[7] << 24))
          ^ RF_ENTROPY_TICKS() ^ 0x9E3779B9u;
#if !RF_TASK_EXECUTOR_TMOS
        x ^= rf_ch570_boot_entropy;   /* cold-boot SRAM PUF entropy (main.c) */
#endif
        if (x == 0) x = 0xA5A5A5A5u;
    }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rf_aa_rng_state = x;
    return (uint16_t)x;
}

#if DONGLE_RF_CRYPT
/* Fresh 32-bit per-session nonce for the CCM construction. Not secret and not a
 * key -- it only needs to differ each connect so a captured transcript cannot be
 * replayed after a reconnect/reboot. Two draws of the same xorshift RNG that
 * seeds the session AA (chip UID ^ RTC ^ cold-boot SRAM entropy).
 *
 * RESIDUAL (documented, deferred with key establishment): 32 bits is a birthday
 * space -- a collision (repeated session_id with reset counters -> reused CCM
 * nonces) is ~1% after ~9,300 sessions under one long-lived key. This phase uses
 * a static host-provisioned key, so it is a real but slow-onset limit; the future
 * key-establishment handshake makes keys per-session and voids it. Widening the
 * session_id would change the wire nonce layout (and ccm_ref/tests), so it is a
 * deliberate follow-up, not a quiet change here. */
static uint32_t rf_crypt_gen_session_id(void)
{
    uint32_t hi = rf_aa_rng16();
    uint32_t lo = rf_aa_rng16();
    return (hi << 16) | lo;
}
#endif /* DONGLE_RF_CRYPT */

/* Generate a fresh BLE-valid session access address. Faithful port of the stock
 * dongle generator at firmware 0x8212: random 0x6A____E6 / 0xAC____CE with the
 * middle two bytes random, rejected if it has >24 bit transitions, any run of 6
 * identical bits, or if the low byte matches all three other bytes. */
static uint32_t rf_generate_session_aa(void)
{
    for (uint8_t attempt = 1; attempt != 0; attempt++) {
        uint32_t u7   = (uint32_t)rf_aa_rng16() << 8;
        uint32_t base = (u7 & 0x100u) ? 0xAC0000CEu : 0x6A0000E6u;
        uint32_t aa   = u7 | base;
        uint8_t trans = 0, bad = 0;
        for (uint32_t i = 0; i < 31; i++) {
            if (((aa >> (i + 1)) & 1u) != ((aa >> i) & 1u)) {
                if (++trans > 0x18u) { bad = 1; break; }
            }
        }
        if (bad) continue;
        for (uint32_t i = 0; i < 0x1Bu; i++) {
            uint32_t w = (aa >> i) & 0x3Fu;
            if (w == 0 || w == 0x3Fu) { bad = 1; break; }
        }
        if (bad) continue;
        uint32_t lo = base & 0xFFu;
        if (lo != ((u7 >> 8) & 0xFFu) || lo != ((u7 >> 16) & 0xFFu)
            || lo != (aa >> 24)) {
            return aa;
        }
    }
    return ((uint32_t)(0x10u + (rf_aa_rng16() % (0xF0u - 0x10u))) << 16)
           | 0x6A00ACE6u;
}

/* Override the runtime bond identity from a DataFlash record when present. Runs
 * after RF_TaskInit assigns the compiled defaults; with no valid record it is a
 * no-op, so an un-provisioned chip is byte-for-byte the pre-persistence build
 * (preserving the OQ7 reconnect soak). */
static void rf_load_persistent_bond(void)
{
    bond_record_t rec __attribute__((aligned(4)));

    /* bond_load() runs the structural + semantic checks (N08); the second
     * validator call adds the own-identity leg (peer must not equal the
     * record's effective dongle identity — the override when present, else
     * the factory MAC). Either failure boots the fresh-pair state. */
    if (!bond_load(&rec) || rec.session_aa == 0u
        || !bond_record_semantic_valid(&rec, rf_factory_mac)) {
        rf_bond_valid = 0;   /* fresh/invalid: pairing only, no boot window */
        
        /* No bond yet -> boot into PAIRING mode: listen on the pair-broadcast AA
         * so the first keyboard that broadcasts gets paired. write-bond-on-pair
         * then persists the bond, so the next boot takes the reconnect path. */
        rf_bond_default_aa = RF_PAIR_ACCESS_ADDR;
        rf_access_addr     = RF_PAIR_ACCESS_ADDR;
        rf_channel         = 8;   /* park on a pair-LUT channel {8,17,26} */
        
        /* Advertise a freshly generated session AA so this dongle/keyboard pair
         * gets a unique access address. The keyboard adopts it from our LEN-15;
         * write-bond-on-pair persists it, so reconnect uses the same AA. */
        rf_bond_aa = rf_generate_session_aa();
        rf_pair_ack15[0] = (uint8_t)((rf_bond_aa >> 24) & 0xFF);
        rf_pair_ack15[1] = (uint8_t)((rf_bond_aa >> 16) & 0xFF);
        rf_pair_ack15[2] = (uint8_t)((rf_bond_aa >>  8) & 0xFF);
        rf_pair_ack15[3] = (uint8_t)( rf_bond_aa        & 0xFF);

#if DONGLE_RF_CRYPT
        rf_crypt_peer_capable = 0u;
        rf_crypt_bond_enc = 0u;
        rf_crypt_clear();
#endif
        return;
    }

    /* Session access address — read by every reconnect/connected path. */
    rf_bond_aa = rec.session_aa;
    rf_bond_valid = 1;   /* enables the boot reconnect/pair window (Step 5) */
    /* A loaded bond is ALREADY verified-present in DataFlash, so mark it
     * persisted. CH570: this stops the FIRST reconnect promote re-running
     * rf_persist_bond_task (whose CX4 bond_save flash-write dance disrupts the
     * just-promoted connected RX -> rx=0 -> supervision lapse -> reacquire ->
     * the reconnect broadcast is then dropped; bench root-cause 2026-07-07,
     * CH592-as-sniffer). CH59x TMOS (CODEREVIEW N11): this is ALSO what makes the
     * known_durable classifier treat a loaded-bond reconnect as durable rather
     * than a fresh pair needing confirmation -- so it must be set for both
     * executors (it was CH570-only before N11). */
    rf_bond_persisted = 1;
    /* A loaded bond is a reconnect target: the boot listen AND the post-timeout
     * RF_EVT_TIMEOUT fallback (which re-listens on rf_bond_default_aa) must camp
     * on the bond's SESSION AA so the dongle hears the bonded keyboard's
     * reconnect. A fresh auto-pair generates a RANDOM session AA that differs
     * from the compile-time RF_DEFAULT_ACCESS_ADDR, so this is unconditional:
     * the old `#if RF_DEFAULT_ACCESS_ADDR == RF_SESSION_ACCESS_ADDR` guard was
     * false for the product build, leaving rf_bond_default_aa at the (wrong)
     * compiled AA -- after any drop the dongle camped on an AA that is neither
     * the session nor the pair AA (deaf -> no recovery). */
    rf_bond_default_aa = rec.session_aa;
    rf_access_addr = rf_bond_default_aa;

    if (rec.conn_interval) rf_conn_interval = rec.conn_interval;
    if (rec.conn_timeout) rf_conn_timeout = rec.conn_timeout;

    /* Dongle MAC defaults to the chip's factory MAC (set in RF_TaskInit). A
     * record overrides it only if it carries a non-zero MAC — e.g. to spoof a
     * specific dongle for a field test against a keyboard bonded elsewhere. The
     * supervision re-key (rf_pair_ack_buf) is rebuilt from rf_bond_dongle_mac
     * each EV10 cycle, so updating it here propagates. */
    {
        int have_dmac = 0;
        for (int i = 0; i < 6; i++) {
            if (rec.dongle_mac[i]) have_dmac = 1;
        }
        if (have_dmac) {
            tmos_memcpy(rf_bond_dongle_mac, rec.dongle_mac, 6);
            tmos_memcpy(&rf_pair_ack15[9], rec.dongle_mac, 6);
            rf_dongle_mac_overridden = 1;   /* N09: keep it across re-persists */
        }
    }

    /* Seed the peer (keyboard) MAC if the record carries one. */
    {
        int have_peer = 0;
        for (int i = 0; i < 6; i++) {
            if (rec.peer_mac[i]) have_peer = 1;
        }
        if (have_peer) tmos_memcpy(rf_peer_mac, rec.peer_mac, 6);
    }

    /* Patch the LEN-15 supervision re-key buffer: AA (MSB-first), interval,
     * timeout. The connected path re-derives rf_access_addr / hop seed /
     * interval from these bytes, so patching here propagates the identity
     * through the whole supervision cycle. (The dongle MAC at [9..14] is set
     * from the chip / record above.) */
    rf_pair_ack15[0] = (uint8_t)((rec.session_aa >> 24) & 0xFF);
    rf_pair_ack15[1] = (uint8_t)((rec.session_aa >> 16) & 0xFF);
    rf_pair_ack15[2] = (uint8_t)((rec.session_aa >>  8) & 0xFF);
    rf_pair_ack15[3] = (uint8_t)( rec.session_aa        & 0xFF);
    if (rec.conn_interval) {
        rf_pair_ack15[5] = (uint8_t)( rec.conn_interval       & 0xFF);
        rf_pair_ack15[6] = (uint8_t)((rec.conn_interval >> 8) & 0xFF);
    }
    if (rec.conn_timeout) {
        rf_pair_ack15[7] = (uint8_t)( rec.conn_timeout       & 0xFF);
        rf_pair_ack15[8] = (uint8_t)((rec.conn_timeout >> 8) & 0xFF);
    }

#if DONGLE_RF_CRYPT
    /* Install the link key ONCE at boot (the expensive CH570 schedule runs here,
     * off the poll grid). Encryption goes ACTIVE only for a negotiated+keyed bond;
     * the per-session nonce is minted later, at connect. */
    rf_crypt_peer_capable = (rec.flags & BOND_FLAG_ENC_CAPABLE) ? 1u : 0u;
    if (bond_enc_active(&rec)) {
        rf_crypt_install_key(rec.link_key);
        rf_crypt_bond_enc = 1u;
    } else {
        rf_crypt_clear();
        rf_crypt_bond_enc = 0u;
    }
#if DONGLE_CRYPT_BENCH_FORCE_KEY && !DONGLE_BENCH_PROFILE
    /* The tripwire behind the profile split: the force key is a shared,
     * in-the-source throwaway -- an image carrying it accepts forged
     * keystrokes from anyone who has read the repo. Only the bench profile
     * (PROFILE=bench, which defines DONGLE_BENCH_PROFILE=1) may compile it;
     * any other route to this block is a build-system regression, and the
     * release target's byte-scan backstops even that. */
#error "DONGLE_CRYPT_BENCH_FORCE_KEY outside the bench profile -- this must never ship (build with PROFILE=bench)"
#endif
#if DONGLE_CRYPT_BENCH_FORCE_KEY
    /* BENCH ONLY (this bench has no dongle USB, so provision_link_key.py
     * cannot reach the bond record): force link decryption ACTIVE for any
     * valid loaded bond, with the compiled-in throwaway key. RAM state only;
     * the DataFlash record is untouched. A fresh pair activates on the next
     * boot, when this load path runs again. The keyboard gets the same key
     * over its 0xAE bench command. Must never ship enabled. */
    if (!rf_crypt_bond_enc) {
        static const uint8_t bench_key[16] = DONGLE_CRYPT_BENCH_KEY_BYTES;
        rf_crypt_install_key(bench_key);
        rf_crypt_bond_enc = 1u;
    }
#endif
#endif
}

/* On a completed pair (burst-promote -> CONNECTED), persist the learned keyboard
 * (peer) MAC and the session AA / interval / timeout to the DataFlash bond
 * record so later boots reconnect without re-provisioning.
 *
 * The DataFlash erase/write masks all PFIC IRQs for tens of ms (CH59x
 * FLASH_EEPROM_CMD), so it MUST NOT run in the radio status callback (IRQ
 * context). rf_request_bond_persist() snapshots the record at promote and posts
 * RF_EVT_PERSIST_BOND; rf_persist_bond_task() does the write in TMOS task
 * context, reads it back, and only latches rf_bond_persisted on confirmed
 * success -- so a transient flash failure retries on a later promote instead of
 * silently shipping a paired-but-unpersisted dongle. The bond also becomes the
 * live reconnect target immediately (RAM) so a same-boot reacquire listens on
 * the learned session AA. dongle_mac: zero = "use chip MAC" unless a loaded
 * override set rf_dongle_mac_overridden, in which case the persisted record
 * carries the active advertised identity (CODEREVIEW N09).
 * (State declared above, ahead of RF_ProcessEvent.) */

/* Radio-IRQ context (burst-promote): snapshot the record and post the event.
 * No flash here. The non-volatile snapshot is published before the
 * tmos_set_event() call (a function-call barrier), and the task handler reads
 * it on a later TMOS pass. */
/* RAM commit + snapshot only (no flash post). Load-bearing for a same-boot reconnect
 * target (rf_bond_aa / rf_bond_default_aa), so it runs at promote for BOTH fresh and
 * bonded. Split out (CODEREVIEW N11) so the fresh-pair TMOS path can RAM-commit at promote
 * but defer the durable write until the peer confirms. */
static void rf_commit_bond_ram(void)
{
    if (rf_bond_tombstone) {
        return;   /* CODEREVIEW N06: host cleared the bond -> block a burst already
                   * in flight from RAM-committing a new bond that would look paired
                   * but vanish at the next reset. No new pair persists until reset. */
    }
    /* Issue #23 hardening: the durable record is keyed on the scalar rf_bond_aa,
     * but TX/promotion advertise the session AA read from the rf_pair_ack15
     * template. In normal flow they are kept in sync (fresh mint and reload both
     * write both), but nothing else enforces agreement here. If they ever diverge
     * the keyboard bonded to the advertised AA, so committing a record on a
     * different AA would mint a deaf reconnect -- skip the commit so nothing
     * durable is written. Graceful skip, NOT an assert: on CH570 this runs in the
     * radio-IRQ __HIGH_CODE sink, where a halting assert would strand the link. */
    if (rf_bond_aa != rf_protocol_pair_ack_session_aa(rf_pair_ack15)) {
        /* Review (CodeRabbit/Copilot): a bare early return would leave a STALE
         * rf_bond_pending_rec that a later confirm -> rf_arm_bond_persist could
         * still write. Zero the pending snapshot and drop rf_bond_valid so a
         * divergence commits and persists NOTHING (rf_persist_bond_task's
         * semantic readback also rejects the resulting zero-AA record). */
        tmos_memset(&rf_bond_pending_rec, 0, sizeof(rf_bond_pending_rec));
        rf_bond_valid = 0;
        return;
    }
    tmos_memset(&rf_bond_pending_rec, 0, sizeof(rf_bond_pending_rec));
#if DONGLE_RF_CRYPT
    /* Persist the negotiated capability so a later host key-provision activates
     * encryption. A fresh pair never carries a key (establishment is deferred),
     * so link_key stays zero and encryption stays off until provisioned. */
    if (rf_crypt_peer_capable) {
        rf_bond_pending_rec.flags |= BOND_FLAG_ENC_CAPABLE;
    }
#endif
    rf_bond_pending_rec.session_aa    = rf_bond_aa;
    rf_bond_pending_rec.conn_interval = rf_conn_interval;
    rf_bond_pending_rec.conn_timeout  = rf_protocol_pair_ack_timeout(rf_pair_ack15);
    tmos_memcpy(rf_bond_pending_rec.peer_mac, rf_peer_mac, 6);
    if (rf_dongle_mac_overridden) {
        /* N09: persist the configured identity the keyboard actually bonded
         * to. Without an override the field stays zero = chip-derived MAC. */
        tmos_memcpy(rf_bond_pending_rec.dongle_mac, rf_bond_dongle_mac, 6);
    }

    rf_bond_valid      = 1;
    rf_bond_default_aa = rf_bond_pending_rec.session_aa;
}

/* Post the deferred DataFlash write (task-context; guarded so it runs once). */
static void rf_arm_bond_persist(void)
{
    if (rf_bond_tombstone) {
        return;   /* CODEREVIEW N06: host cleared the bond -> do not re-persist until reset */
    }
    if (!rf_bond_persisted && !rf_bond_persist_pending) {
        rf_bond_persist_pending = 1;
        hal_event_post(RF_EVT_PERSIST_BOND);
    }
}

static void rf_request_bond_persist(void)
{
    rf_commit_bond_ram();
    rf_arm_bond_persist();
}

/* TMOS task context: the DataFlash write + readback verify. Latches
 * rf_bond_persisted only after the saved record reads back and matches the full
 * reconnect-critical tuple (session_aa, interval, timeout, peer_mac, and — N09
 * — dongle_mac; bond_tuple_equal() in bond.h is the single definition). */
static void rf_persist_bond_task(void)
{
    if (rf_bond_tombstone) {
        rf_bond_persist_pending = 0;   /* CODEREVIEW N06 (defense-in-depth): if a
                                        * persist event slipped past RF_TombstoneBond's
                                        * cancel, do not write the cleared record. */
        return;
    }
    if (rf_bond_persisted) {
        rf_bond_persist_pending = 0;
        return;
    }

    /* Copy the IRQ-published snapshot under a brief IRQ-masked critical section
     * so a concurrent rf_request_bond_persist() (radio IRQ) cannot tear the
     * bond record mid-read; clear the pending flag atomically with the copy.
     * (Brief, post-pair, well clear of boot -- no USB-enum interaction.) */
    bond_record_t want __attribute__((aligned(4)));
    uint32_t irq = __risc_v_disable_irq();
    tmos_memcpy(&want, &rf_bond_pending_rec, sizeof(want));
    rf_bond_persist_pending = 0;
    (void)__risc_v_enable_irq(irq);

    bond_record_t cur __attribute__((aligned(4)));
    if (bond_load(&cur) && bond_tuple_equal(&cur, &want)) {
        /* stored record already matches the full tuple (incl. the N09
         * dongle identity); no write */
        rf_bond_persisted = 1;
        return;
    }

#if !RF_TASK_EXECUTOR_TMOS
    /* CX4 (codex, hardware-forced CH570 delta), root-cause-revised 2026-07-07.
     * This runs ONCE per session — the first promote of a fresh pair, before
     * the bond is persisted. The ONLY safe shape when we are already CONNECTED
     * is the CH592's: write the bond WITHOUT disturbing the radio and stay
     * CONNECTED. The old shut+re-init+recamp path desynced from the connected
     * keyboard (which is in data-hop mode, not re-broadcasting on the pair
     * channel) and delivered ZERO connected RX — the very failure it aimed to
     * prevent. The disconnected case (defensive) keeps the reconnect camp. */
    uint8_t was_connected = (rf_state == RF_STATE_CONNECTED);
    if (!was_connected) {
        hal_timer_cancel(HAL_TMR_SLOT_PAIR_ACK);
        hal_timer_cancel(HAL_TMR_SLOT_EV10_REKEY);
        hal_timer_cancel(HAL_TMR_SLOT_BOOT_WINDOW);
        hal_timer_cancel(HAL_TMR_SLOT_CONNECTED_POLL);
        hal_rf_shut();
    }
#endif
    /* N08 defense-in-depth: never durably persist a tuple the next boot's
     * validator would reject (the accept-site guards are the primary fix; this
     * catches anything that slips a future path). Producer invariant worth
     * knowing: interval/timeout here always come from OUR pair-ACK template
     * (rf_pair_ack15, compiled 28/600) — the air-decoded broadcast values are
     * logged but never enter the durable tuple, so this check can only fire
     * on an identity-class escape. Leaving rf_bond_persisted=0 keeps the
     * session usable this boot; the record simply never becomes durable. */
    if (!bond_record_semantic_valid(&want, rf_factory_mac)) {
        
        return;
    }
    int save_rc = bond_save(&want);
#if !RF_TASK_EXECUTOR_TMOS
    if (was_connected) {
        /* CH592-style: the poll cadence kept running through the write; just
         * refresh supervision so the brief flash-write gap is not a lapse. */
        rf_last_conn_rx_tsys = hal_now();
    } else {
        hal_rf_init();
        rf_supervision_ev10_active = 0;
        rf_state = RF_STATE_PAIRING;
        rf_access_addr = want.session_aa;
        rf_channel = RF_PROTO_RECONNECT_CAMP_CHANNEL;
        rf_start_rx();
        rf_arm_retry_if_failed();   /* P4: this recamp arms no timer */
    }
#endif
    if (save_rc != 0) {
        
        return;                  /* leave rf_bond_persisted = 0 to retry */
    }

    bond_record_t back __attribute__((aligned(4)));
    if (bond_load(&back) && bond_tuple_equal(&back, &want)) {
        rf_bond_persisted = 1;
    }
}

/* ---------- Public API ---------- */

#if !RF_TASK_EXECUTOR_TMOS
void hal_timing_ch570_mux_init(void);      /* hal_timing_ch570.c */
#endif

void RF_TaskInit(void)
{
#if RF_TASK_EXECUTOR_TMOS
    rf_taskID = TMOS_ProcessEventRegister(RF_ProcessEvent);
#else
    /* Polled executor: bring the hal_timing mux's backing TMR IRQ to life
     * (the legacy body did this from its own init; the seam owns it now). */
    hal_timing_ch570_mux_init();
#endif

    /* Load the compiled defaults first; a valid DataFlash bond record overrides
     * the identity below (rf_load_persistent_bond). */
    rf_access_addr = rf_bond_default_aa;
    rf_channel = RF_PROTO_RECONNECT_CAMP_CHANNEL;
    tmos_memset(rf_peer_mac, 0, sizeof(rf_peer_mac));
    rf_state = RF_STATE_IDLE;
    rf_hid_callback = NULL;
    /* Pre-register the fixed TMR0-pair work callbacks (codex ii-d): events can
     * reach dispatch without a prior hal_timer_arm on their slot and must
     * never find a null cb. (The inherited-promote/pair-retry producers were
     * deleted at R1b ii; kept for the EV10 arms and defense in depth.) */
    hal_timing_ch592_set_cb(HAL_TMR_SLOT_PAIR_ACK, rf_pair_ack_cb);
    hal_timing_ch592_set_cb(HAL_TMR_SLOT_CONNECTED_POLL, rf_connected_poll_cb);
    /* P2.4: register the protocol sink with the RF PHY seam. The vendor
     * status callback (hal_rf_ch592.c) forwards one hal_rf event per radio
     * IRQ here. */
    hal_rf_set_event_cb(rf_phy_event_sink);
    rf_conn_timeout = RF_PROTO_DEFAULT_TIMEOUT;

    /* The advertised dongle MAC is the chip's own factory MAC: GET_UNIQUE_ID
     * returns bytes 0..5 from ROM_CFG_MAC_ADDR (bytes 6..7 are a checksum). This
     * matches the stock dongle, which derives its 2.4G MAC from silicon, so the
     * MAC never needs to be provisioned and the binary carries no hard-coded MAC.
     * A bond record may still override it (rf_load_persistent_bond) for spoofing. */
    {
        uint8_t chip_uid[8] __attribute__((aligned(4)));
        GET_UNIQUE_ID(chip_uid);
        tmos_memcpy(rf_bond_dongle_mac, chip_uid, 6);
        tmos_memcpy(&rf_pair_ack15[9], chip_uid, 6);
        /* N08: keep the factory identity separately — a record override
         * rewrites rf_bond_dongle_mac, but the semantic validator (and IAP
         * BondWrite via RF_FactoryMac) needs the CHIP identity, not the
         * live/overridden one (a record replacing an override must not be
         * judged against the override it replaces). */
        tmos_memcpy(rf_factory_mac, chip_uid, 6);
    }

#if DONGLE_RF_CRYPT
    /* Bring the AES backend up before the bond load can install a key. hal_rf_init
     * has already run in main() (the CH592 engine needs BLE_IPCoreInit), so this
     * is safe here. */
    rf_crypt_init();
#endif
    rf_load_persistent_bond();
    /* Runtime latch bits must not survive reset. */
    rf_supervision_ev10_active = 0;

    /* Kick off RF setup via TMOS event — mirrors the WCH RF_PHY sample's
     * pattern. All RF library calls (RF_Shut/RF_Config/RF_Rx/RF_Tx) are
     * done from RF_ProcessEvent in task context, NOT inline here. Doing
     * them inline (as a removed earlier "boot probe" did) wedges the
     * chip because the BLE library expects to be driven from its
     * internal event loop, not from main() before Main_Circulation runs. */
    hal_event_post(RF_EVT_START);

}

void RF_SetHIDCallback(rf_hid_cb_t cb)
{
    rf_hid_callback = cb;
}

/* CODEREVIEW N06: called from the IAP BondClear handler right after bond_clear()
 * erases the DataFlash record. Invalidate the in-RAM bond and cancel any in-flight
 * persist so nothing rewrites the record the host just wiped, and tombstone further
 * persists AND new-pair accept/promote until the next MCU reset (power-on/reset
 * zeroes .bss; deliberately NOT lifted at RF_EVT_START). Task context
 * (USB_PollEP6); the RAM-state writes are masked against the radio IRQ that also
 * touches rf_bond_valid / rf_bond_persisted / the persist latch. */
/* CODEREVIEW N08: the chip's factory identity for the IAP BondWrite semantic
 * check. Deliberately NOT the live rf_bond_dongle_mac — a record that
 * replaces or clears an override must be judged against the CHIP identity,
 * not the override it is replacing. */
const uint8_t *RF_FactoryMac(void)
{
    return rf_factory_mac;
}

void RF_TombstoneBond(void)
{
    uint32_t irq = __risc_v_disable_irq();
    rf_bond_tombstone = 1;
    rf_bond_valid = 0;
    rf_bond_persisted = 0;
    rf_bond_persist_pending = 0;
    (void)__risc_v_enable_irq(irq);
    hal_event_cancel(RF_EVT_PERSIST_BOND);
#if DONGLE_RF_CRYPT
    /* Zeroize the key schedule and drop the encrypted-path state (outside the
     * IRQ-masked section: rf_crypt_clear runs the CH570 zero-key schedule).
     * Deliberately DO NOT clear rf_crypt_bond_enc: BondClear can run while the
     * encrypted link is still live (iap.c keeps it forwarding until it drops), so
     * the "this bond is encryption-required" latch must stay set or the still-live
     * connection would revert to the plaintext dispatch and accept forged HID.
     * With the key gone every frame now fails closed (encrypted -> no key ->
     * dropped; plaintext -> dropped), so the cleared link goes dead, not open. */
    rf_crypt_peer_capable = 0u;
    rf_crypt_announce_count = 0u;
    rf_crypt_fifo_head = rf_crypt_fifo_tail;
    rf_crypt_frames_since_ok = 0u;
    rf_crypt_force_release = 0u;
    rf_crypt_clear();
#endif
}

void RF_SetLEDState(uint8_t led)
{
    /* Forward the host's HID LED output report (CapsLock/NumLock/ScrollLock) to
     * the keyboard. Called from the foreground (Main_Circulation) on a host LED
     * change. Latch the byte and, if connected, queue the relay DIRECTLY for the
     * next poll (rf_send_poll sends [ctrl][0xA1][led]). When not connected, the
     * burst-promote re-syncs the current rf_led_state on the next connect.
     *
     * NO IRQ critical section here -- but not because nothing preempts: the
     * burst-promote producer runs in the rf_phy_event_sink IRQ-tail callback,
     * which CAN preempt this task-context code. It is safe without a guard
     * because the shared state is all single volatile bytes, preemption is
     * one-way, and rf_led_state is latched below before the queue call (see
     * the contract note at rf_queue_led_relay). The earlier
     * __risc_v_disable_irq() guard (copied from CH570, whose poll runs in a timer
     * ISR) was not just unnecessary but HARMFUL: its csrrc/csrrs on CSR 0x800
     * (mask 0x88 = MIE|MPIE), executed in the boot loop right after CH59x_BLEInit
     * / USB_DevInit, could leave USB_IRQn masked and break USB enumeration (see
     * the CSR-0x800 warning near main.c:177). Bench-confirmed + codex-reviewed
     * 2026-06-14. */
    rf_led_state = (uint8_t)(led & 0x07u);
    if (rf_state == RF_STATE_CONNECTED) {
        rf_queue_led_relay(rf_led_state);
    }
}

uint8_t RF_GetState(void)
{
    return rf_state;
}

int8_t RF_GetRSSI(void)
{
    return rf_rssi;
}

uint8_t RF_GetConnectionStatus(void)
{
    if (rf_state == RF_STATE_CONNECTED) {
        return DONGLE_CONNECTION_CONNECTED;
    }
    if (rf_bond_valid) {
        return DONGLE_CONNECTION_WAITING;
    }
    return DONGLE_CONNECTION_PAIRING;
}

const uint8_t *RF_GetDongleMac(void)
{
    return rf_bond_dongle_mac;
}

void RF_QuiesceRequest(void)
{
    hal_event_post(RF_EVT_QUIESCE);
}

int RF_Quiesced(void)
{
    return rf_quiesced != 0;
}

#if !RF_TASK_EXECUTOR_TMOS
/* Polled executor (CH570): drain the hal_dispatch pending mask through the
 * shared event handler. One snapshot per pump call, processed to empty —
 * bits posted by IRQs mid-drain land in the mask for the next main-loop
 * pass (never lost; the coalescing contract holds). */
uint16_t hal_dispatch_ch570_drain(void);   /* hal_dispatch_ch570.c */

void RF_TaskPump(void)
{
    uint16_t ev = hal_dispatch_ch570_drain();

    while (ev) {
        ev = RF_ProcessEvent(0u, ev);
    }
}

#endif
