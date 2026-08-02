/*
 * Bridge75 Open-Source Dongle Firmware -- CH592 RF PHY HAL.
 *
 * Implements hal_rf.h over the LIBCH59xBLE radio (RF_Config / RF_SetChannel /
 * RF_Rx / RF_Tx / RF_Shut). This is the PHY seam: rf_task drives the radio
 * through hal_rf_*, and the vendor status callback here is reduced to pure
 * PHY-event extraction -- it maps the (sta, rsr) pair onto the chip-agnostic
 * hal_rf_event_t set and forwards one event per invocation to the
 * rf_task-registered sink (CH59x sta is a single enumerated state, never a
 * bitmask, so one callback == one event). All protocol/FSM logic stays in
 * rf_task.c.
 *
 * CH59x mapping quirks (vs the CH570 RFIP impl):
 *  - The access address and CRC live in RF_Config (hal_rf_configure);
 *    start_tx's access_addr argument is accepted for contract symmetry but
 *    the radio transmits with the configured AA.
 *  - Channel is persistent radio state set by RF_SetChannel; the
 *    HAL_RF_CHANNEL_CURRENT sentinel skips the retune, preserving the stock
 *    no-channel-set shapes (pair-ACK TX inherits the pair-prep channel;
 *    post-poll RX re-arms untouched).
 *  - RF_Rx has no hardware timeout in this basic-mode usage; the `timeout`
 *    argument is ignored (link supervision is TMOS-timer based).
 *  - RF_Rx's (txBuf, txLen) prime pair is passed through verbatim by
 *    hal_rf_start_rx_primed for stock-shape faithfulness.
 */
#include "CONFIG.h"
#include "hal_rf_ch592.h"

/* Mirrors the rf_task.c values exactly (PROTOCOL.md RF PHY parameters). */
#define HAL_RF_CH592_CRC_INIT  0x555555
#define HAL_RF_CH592_LLE_MODE  (LLE_MODE_BASIC | LLE_WHITENING_ON | LLE_MODE_PHY_2M)

static hal_rf_event_cb_t rf_event_cb;

/* Last RX frame base handed to the status callback. The BLE library owns the
 * buffer; the pointer is stable across a session in practice (hal_rf_rx_buf
 * contract: telemetry/decoding only). */
static const uint8_t *volatile rf_last_rx_frame;

void RF_2G4StatusCallBack(uint8_t sta, uint8_t rsr, uint8_t *rxBuf);

void hal_rf_init(void)
{
    RF_RoleInit();
}

void hal_rf_set_event_cb(hal_rf_event_cb_t cb)
{
    rf_event_cb = cb;
}

const uint8_t *hal_rf_rx_buf(void)
{
    return rf_last_rx_frame;
}

void hal_rf_configure(uint32_t access_addr)
{
    rfConfig_t cfg;

    tmos_memset(&cfg, 0, sizeof(cfg));
    cfg.LLEMode       = HAL_RF_CH592_LLE_MODE;
    cfg.Channel       = 0;    /* RF_SetChannel selects the live channel later. */
    cfg.accessAddress = access_addr;
    cfg.CRCInit       = HAL_RF_CH592_CRC_INIT;
    cfg.rfStatusCB    = RF_2G4StatusCallBack;
    (void)RF_Config(&cfg);
}

void hal_rf_set_channel(uint8_t channel)
{
    RF_SetChannel(channel);
}

__HIGH_CODE
uint8_t hal_rf_start_rx(uint8_t channel, uint16_t timeout)
{
    (void)timeout;   /* no hardware RX timeout in basic mode; TMOS supervises */
    if (channel != HAL_RF_CHANNEL_CURRENT) {
        RF_SetChannel(channel);
    }
    return RF_Rx(NULL, 0, 0xFF, 0xFF);
}

__HIGH_CODE
uint8_t hal_rf_start_rx_primed(uint8_t channel, uint16_t timeout,
                               const uint8_t *prime_buf, uint8_t prime_len)
{
    (void)timeout;
    if (channel != HAL_RF_CHANNEL_CURRENT) {
        RF_SetChannel(channel);
    }
    return RF_Rx((uint8_t *)prime_buf, prime_len, 0xFF, 0xFF);
}

__HIGH_CODE
uint8_t hal_rf_start_tx(uint8_t channel, uint32_t access_addr,
                        const uint8_t *buf, uint8_t len)
{
    (void)access_addr;   /* AA comes from RF_Config (hal_rf_configure) */
    if (channel != HAL_RF_CHANNEL_CURRENT) {
        RF_SetChannel(channel);
    }
    return RF_Tx((uint8_t *)buf, len, 0xFF, 0xFF);
}

void hal_rf_shut(void)
{
    RF_Shut();
}

/*
 * Vendor status callback. CONTEXT: the BLE controller invokes this as an
 * IRQ-tail DEFERRED callback (TMOS_SysRegister/tmosSign trampoline), not a raw
 * BB/LLE ISR; it can preempt the foreground/TMOS task. Pure PHY-event extraction:
 * (sta, rsr) -> one hal_rf_event_t per invocation. rsr (bit0 CRC, bit1 type
 * match) collapses to the RX_DONE/RX_CRCERR split.
 */
__HIGH_CODE
void RF_2G4StatusCallBack(uint8_t sta, uint8_t rsr, uint8_t *rxBuf)
{
    if (rxBuf) {
        rf_last_rx_frame = rxBuf;
    }
    if (rf_event_cb == 0) {
        return;
    }
    switch (sta) {
    case RX_MODE_RX_DATA:
        if (rsr == 0) {
            rf_event_cb(HAL_RF_EV_RX_DONE, rxBuf, rxBuf ? rxBuf[1] : 0u);
        } else {
            rf_event_cb(HAL_RF_EV_RX_CRCERR, rxBuf, 0u);
        }
        break;
    case TX_MODE_TX_FINISH:
        rf_event_cb(HAL_RF_EV_TX_DONE, rxBuf, 0u);
        break;
    case TX_MODE_TX_FAIL:
        rf_event_cb(HAL_RF_EV_TX_FAIL, rxBuf, 0u);
        break;
    default:
        /* Auto-mode states this basic-mode firmware never arms. The legacy
         * callback's switch default posted a defensive RX restart for them;
         * forward as RX_TIMEOUT (CH59x has no real RX-timeout state, so the
         * slot is free) and the sink replicates the legacy default. */
        rf_event_cb(HAL_RF_EV_RX_TIMEOUT, rxBuf, 0u);
        break;
    }
}
