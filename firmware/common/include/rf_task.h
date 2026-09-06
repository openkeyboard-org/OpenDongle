/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Shared RF-task public API + event vocabulary (CH59x-family shape; P5).
 *
 * This is the merged rf_task.c's contract, shared by all three chips (CH592,
 * CH582, CH570) since the P6 legacy deletion.
 */
#ifndef RF_TASK_H
#define RF_TASK_H

#include <stdint.h>
#include "dongle_status.h"
#include "dongle_target.h"   /* a port may override RF_DIAG_PAGE_COUNT */

/* Deferred-event bits (hal_dispatch.h vocabulary; on CH59x these are TMOS
 * task events — SYS_EVENT_MSG remains 0x8000). */
#define RF_EVT_START             0x0001
#define RF_EVT_RX_RESTART        0x0002
#define RF_EVT_TIMEOUT           0x0004
#define RF_EVT_POLL              0x0008  /* CONNECTED: emit LEN=1 poll on next data-LUT channel */
#define RF_EVT_PAIR_PREP         0x0010  /* stock event-0x20-like pair-channel RF_Rx(6) pre-phase */
#define RF_EVT_SEND_PAIR_ACK     0x0020  /* TMR0 fire: emit 15-byte pair-completion TX */
#define RF_EVT_TX_PAIR_15        0x0040  /* LIVE fresh-pair entry: the FIRST burst 15-byte TX, posted by the pair-broadcast decoder (TX_FINISH then chains RF_EVT_TX_PAIR_15B) */
#define RF_EVT_TX_PAIR_15B       0x0080  /* chained 15-byte TX on session AA after the initial pair-AA TX */
#define RF_EVT_POST_POLL_RX      0x0100  /* stock-event-0x04-shaped post-poll-TX RX arm */
#define RF_EVT_CONFIRM_TIMEOUT   0x0200  /* CODEREVIEW N11 (RF_CONFIRM_BEFORE_PERSIST,
                                          * both executors as of Issue #23): fresh-pair
                                          * confirmation deadline — durable bond persist is
                                          * deferred until a connected RX confirms the peer
                                          * accepted the session; this fires the fresh-pair
                                          * fallback if it never does. */
#define RF_EVT_BOOT_WINDOW       0x0400  /* boot reconnect/pair listen-window timer */
#define RF_EVT_PERSIST_BOND      0x0800  /* deferred DataFlash bond write (task ctx, out of the radio ISR) */
#define RF_EVT_QUIESCE           0x1000  /* pre-reboot: shut the radio in executor context, stop re-arming */

/* Connection states */
#define RF_STATE_IDLE        0
#define RF_STATE_PAIRING     1
#define RF_STATE_CONNECTED   2

/* Callback for received HID data: the shared protocol contract
 * (rf_protocol.h). rf_hid_cb_t is a temporary compatibility alias while the
 * CH59x-family internals converge on the shared name. */
#include "rf_protocol.h"
typedef rf_hid_callback_t rf_hid_cb_t;

/* Initialize the RF 2.4G receiver TMOS task */
void RF_TaskInit(void);

/* Register a callback for received HID reports */
void RF_SetHIDCallback(rf_hid_cb_t cb);

/* CODEREVIEW N06: the IAP BondClear handler calls this right after erasing the
 * DataFlash bond, so the running RF task can't re-persist its still-live in-RAM
 * bond over the cleared record. Invalidates the in-RAM bond, cancels any pending
 * persist, and blocks further persists until the next reset. */
void RF_TombstoneBond(void);

/* CODEREVIEW N08: the chip's factory MAC (pre-override), for the IAP
 * BondWrite semantic validator's own-identity leg. */
const uint8_t *RF_FactoryMac(void);

/* Pre-reboot quiesce (the IAP EnterBootloader path): stops new RF work and
 * shuts the radio from the executor context the radio library requires
 * (TMOS task on CH59x, pump pass on CH570). Request from any context, then
 * poll RF_Quiesced() with a bounded wait before resetting — an unserviced
 * request (wedged executor) must not block the reboot forever. One-way
 * until the next reset. A pending bond persist dispatches first. */
void RF_QuiesceRequest(void);
int RF_Quiesced(void);

/* Forward the host's HID LED output report (CapsLock/NumLock/ScrollLock, low 3
 * bits) to the keyboard. Call from the main loop on a host LED change. */
void RF_SetLEDState(uint8_t led);

/* Polled-executor pump (RF_TASK_EXECUTOR_TMOS=0 chips): call once per
 * main-loop pass to drain deferred events. TMOS chips never define it. */
void RF_TaskPump(void);

/* Cold-boot SRAM entropy word: main() hashes the pristine power-on RAM into this
 * before it is painted/used, and the session-AA RNG mixes it into its seed for
 * a per-cold-boot-fresh, unpredictable access address. CH570 only. */
extern volatile uint32_t rf_ch570_boot_entropy;

/* Get current connection state */
uint8_t RF_GetState(void);

/* Get last received RSSI */
int8_t RF_GetRSSI(void);

/* Stable user-facing state for the production status command. */
uint8_t RF_GetConnectionStatus(void);

/* Effective on-air identity, including any persistent bond override. */
const uint8_t *RF_GetDongleMac(void);

/* IAP 0x92 RF diagnostics: fill one read-only page (RF_DIAG_PAGE_LEN bytes) of
 * runtime state and wrapping counters. Returns the bytes written, 0 for an
 * unknown page or a too-small buffer. Page layouts are documented at
 * RF_DiagFill in rf_task.c and mirrored by tools/src/rfdiag.rs. */
#define RF_DIAG_PAGE_VERSION 1u
#define RF_DIAG_PAGE_LEN     62u
/* Pages 0-4 are common; a port may add pages (CH592 PM_IDLE=1: 5 and 6, see
 * dongle_target.h). Compatibility runs one way: a tool that knows more pages
 * than the firmware skips the ones RF_DiagFill answers with length 0
 * (tools/src/rfdiag.rs REQUIRED_PAGES = 4); an older tool never requests
 * them. */
#ifndef RF_DIAG_PAGE_COUNT
#define RF_DIAG_PAGE_COUNT   5u
#endif
uint8_t RF_DiagFill(uint8_t page, uint8_t *out, uint8_t max);

#if DONGLE_PM_IDLE
/* Idle-admission class of the RF task for the CH592 main-loop idle
 * (pm_ch592.c): one byte of state bits read under the IRQ mask without any
 * XIP call. 0 means the terminal PAIRING camp (bonded reconnect search or the
 * fresh-pair listen) with nothing else in flight. */
#define RF_IDLE_CLASS_CONNECTED 0x01u   /* rf_state == CONNECTED            */
#define RF_IDLE_CLASS_EV10      0x02u   /* supervision EV10 reacquire scan   */
#define RF_IDLE_CLASS_BOOTWIN   0x04u   /* boot reconnect/pair window timer  */
#define RF_IDLE_CLASS_BURST     0x08u   /* fresh-pair ACK burst in flight    */
#define RF_IDLE_CLASS_CONFIRM   0x10u   /* confirm-before-persist pending    */
#define RF_IDLE_CLASS_QUIESCED  0x20u   /* IAP reboot quiesce done           */
#define RF_IDLE_CLASS_IDLE      0x40u   /* rf_state == IDLE                  */
#define RF_IDLE_CLASS_PERSIST   0x80u   /* bond persist posted, not written  */
uint8_t RF_IdleClass(void);
#endif

/* IAP 0x94 RF intervention ladder (armed, task context, CH570 only): 1 =
 * re-arm RX from task context (heals a lost radio completion event), 2 = shut
 * + vendor re-init + re-arm (heals a PHY that stayed deaf through shut/arm).
 * The state predicate is checked and the action taken under one IRQ mask.
 * 0 = done; 0xE0 unknown rung; 0xE1 not in the exact terminal camp (CONNECTED,
 * EV10 scan, boot window/relisten or pair burst active); 0xE2 quiescing for a
 * reboot; 0xE3 unsupported on this radio (CH59x). */
uint8_t RF_DiagIntervene(uint8_t rung);

#endif
