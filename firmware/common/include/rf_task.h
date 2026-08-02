/*
 * Bridge75 Open-Source Dongle Firmware
 * Shared RF-task public API + event vocabulary (CH59x-family shape; P5).
 *
 * This is the merged rf_task.c's contract, shared by all three chips (CH592,
 * CH582, CH570) since the P6 legacy deletion.
 */
#ifndef RF_TASK_H
#define RF_TASK_H

#include <stdint.h>
#include "dongle_status.h"

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
#define RF_EVT_CONFIRM_TIMEOUT   0x0200  /* CODEREVIEW N11 (CH59x TMOS only): fresh-pair
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

#endif
