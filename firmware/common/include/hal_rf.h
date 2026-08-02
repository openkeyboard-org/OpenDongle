/*
 * Bridge75 Open-Source Dongle Firmware
 * RF PHY HAL seam — the 2.4 GHz radio access layer.
 *
 * The shared RF task drives the radio entirely through this seam so one
 * rf_task.c serves every SoC. CH570 implements it directly over its RFIP
 * (`RFIP_SetRx`/`RFIP_StartTx`/`RFRole_BasicInit`); CH59x/CH58x implement the
 * same interface over the LIBCH59xBLE radio (`RF_Config`/`RF_SetChannel`/
 * `RF_Rx`/`RF_Tx`/`RF_Shut`). The vendor status callback
 * (CH570 `RF_ProcessCallBack`, CH59x `RF_2G4StatusCallBack`) lives inside the
 * per-chip implementation and is reduced to pure PHY-event extraction: it maps
 * the radio's raw status into the chip-agnostic events below and forwards them
 * to the registered callback. All protocol/FSM logic stays in rf_task.c.
 *
 * CONTRACT (all implementations MUST honor this — it is a behavioral interface,
 * not just a set of signatures):
 *
 *  - Event model: the registered hal_rf_event_cb_t is invoked once per PHY
 *    event. When the radio raises several status bits in one interrupt, the
 *    implementation dispatches one event per bit (no event is dropped) in the
 *    priority order RX_DONE, RX_CRCERR, RX_TIMEOUT, TX_DONE.
 *  - Callback context: the event callback runs in the radio's IRQ context (the
 *    same context the vendor status callback runs in). rf_task is written to
 *    this contract.
 *  - RX frame base layout (identical on both radios): rx[0] = RSSI,
 *    rx[1] = payload length, rx[2..] = payload. On HAL_RF_EV_RX_DONE the
 *    callback's `rx` points at the frame base and `len` equals rx[1].
 *    hal_rf_rx_buf() returns that same frame base for telemetry/decoding.
 *  - hal_rf_configure() sets the access address used by the next
 *    hal_rf_start_rx(); it is NOT timing-critical and MAY live in flash.
 *  - hal_rf_start_rx()/hal_rf_start_tx() are on the timing-critical
 *    connected-poll path and run from SRAM on CH570 (__HIGH_CODE). They return
 *    the radio's raw start status (0 = success) unchanged.
 *  - timeout is passed straight to the radio's RX timeout field unchanged
 *    (CH570: rfipRx_t.timeOut, units N*0.5us).
 */
#ifndef HAL_RF_H
#define HAL_RF_H

#include <stdint.h>

/* Chip-agnostic PHY events. Dispatch priority on a multi-bit status is the
 * declaration order: RX_DONE first, then RX_CRCERR, RX_TIMEOUT, TX_DONE,
 * TX_FAIL. TX_FAIL is raised only by radios that report a distinct
 * transmit-failure state (CH59x TX_MODE_TX_FAIL, which carries real EV10
 * restore + pending-ACK retry semantics in rf_task — it must NOT be folded
 * into TX_DONE). CH570's RFIP has no fail state and never raises it. */
typedef enum {
    HAL_RF_EV_RX_DONE = 0,
    HAL_RF_EV_RX_CRCERR,
    HAL_RF_EV_RX_TIMEOUT,
    HAL_RF_EV_TX_DONE,
    HAL_RF_EV_TX_FAIL,
} hal_rf_event_t;

/* Channel sentinel: arm RX/TX on whatever channel the radio is currently
 * tuned to, without a retune. Used by the CH59x stock-shape paths that
 * deliberately skip RF_SetChannel (pair-ACK TX inherits the pair-prep
 * channel; post-poll RX re-arms without reconfigure). CH59x/CH58x resolve it
 * through the radio's persistent channel state; CH570 (P4) replicates that
 * state with a HAL-side shadow byte refreshed by set_channel and every
 * explicit-channel arm — the sentinel is now uniform across chips. */
#define HAL_RF_CHANNEL_CURRENT 0xFFu

/* On HAL_RF_EV_RX_DONE: rx = RX frame base (rx[0]=RSSI, rx[1]=len, rx[2..]=
 * payload), len = rx[1]. On the other events rx/len are unspecified. */
typedef void (*hal_rf_event_cb_t)(hal_rf_event_t ev, const uint8_t *rx,
                                  uint8_t len);

/* Bring the radio up (vendor BasicInit + static TX/RX param config). */
void hal_rf_init(void);

/* Register the PHY-event sink. Set before the radio can raise events. */
void hal_rf_set_event_cb(hal_rf_event_cb_t cb);

/* Select the access address used by the next hal_rf_start_rx(). */
void hal_rf_configure(uint32_t access_addr);

/* Tune the radio's channel WITHOUT (re)arming RX/TX. CH59x maps this to
 * RF_SetChannel (the stock shapes sequence it independently of the arm —
 * e.g. poll TX is SetChannel, Shut, Tx). CH570 records it as the channel a
 * subsequent HAL_RF_CHANNEL_CURRENT arm resolves to. */
void hal_rf_set_channel(uint8_t channel);

/* Arm RX on `channel` (frequency derived per-chip; HAL_RF_CHANNEL_CURRENT =
 * no retune) with `timeout`, using the access address from the most recent
 * hal_rf_configure(). Returns the radio's raw start status (0 = success). */
uint8_t hal_rf_start_rx(uint8_t channel, uint16_t timeout);

/* RX arm variant carrying a radio-specific prime payload. CH59x's RF_Rx takes
 * a (txBuf, txLen) pair — the auto-mode ACK payload, passed through verbatim
 * for stock-shape faithfulness even in basic mode (stock arms its connected
 * post-poll RX with the poll buffer and the pair-prep RX with a 6-byte
 * buffer). CH570's RFIP has no such concept: prime args are ignored and this
 * is identical to hal_rf_start_rx(). */
uint8_t hal_rf_start_rx_primed(uint8_t channel, uint16_t timeout,
                               const uint8_t *prime_buf, uint8_t prime_len);

/* Transmit `len` bytes from `buf` on `channel` at `access_addr`. The per-chip
 * implementation owns the on-air framing of the payload. Returns the radio's
 * raw start status (0 = success). */
uint8_t hal_rf_start_tx(uint8_t channel, uint32_t access_addr,
                        const uint8_t *buf, uint8_t len);

/* Power the radio down. */
void hal_rf_shut(void);

/* RX DMA frame base (rx[0]=RSSI, rx[1]=len, rx[2..]=payload) for the rf_task
 * RX decoder. Stable for the life of the program. */
const uint8_t *hal_rf_rx_buf(void);

#endif /* HAL_RF_H */
