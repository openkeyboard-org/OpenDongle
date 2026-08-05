/*
 * OpenKeyboard.org OpenDongle -- CH570 RF PHY HAL.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Implements hal_rf.h directly over the CH570 RFIP radio
 * (RFRole_BasicInit / RFIP_SetRx / RFIP_StartTx). This is the PHY seam: the
 * shared rf_task drives the radio through hal_rf_*, and the RFIP status
 * callback here is reduced to pure PHY-event extraction -- it maps the RFIP
 * `sta` status bits onto the chip-agnostic hal_rf_event_t set and forwards
 * each to the rf_task-registered sink. All protocol/FSM logic stays in
 * rf_task.c.
 *
 * Ownership moved here from rf_task.c: the
 * rx_param/tx_param RFIP descriptors, the RX DMA buffer, the TX-DMA framing,
 * rf_properties(), rf_frequency_for_channel(), RFRole_BasicInit, and the
 * RF_ProcessCallBack vendor entry.
 */
#include "hal_rf.h"
#include "rf_protocol.h"

#include "CH57x_common.h"
#include "CH572rf.h"
#include "ch570_corecfgr.h"

/*
 * --- Production RF PHY configuration -----------------------------------------
 */
#define RF_PAIR_ACCESS_ADDR RF_PROTO_PAIR_ACCESS_ADDR

#define CH570_RF_CRC_INIT                       0x555555u
#define CH570_RF_CRC_POLY                       0x80032du
#define CH570_RF_RX_MAX_LEN                     32u
#define CH570_RF_RX_DMA_SIZE                    (CH570_RF_RX_MAX_LEN + 16u)
#define CH570_RF_TX_POWER                       LL_TX_PWR_0_DBM

/*
 * --- PHY state ---------------------------------------------------------------
 * rx_param/tx_param are the RFIP descriptors; static fields are programmed once
 * in hal_rf_init(), per-arm fields in start_rx/start_tx. rx_buf is the RX DMA
 * region (RFIP writes its small header before the rxMaxLen payload).
 * pair_ack_tx_dma holds the framed TX payload.
 */
static rfipRx_t rx_param;
static rfipTx_t tx_param;
__attribute__((aligned(4))) static uint8_t rx_buf[CH570_RF_RX_DMA_SIZE];
__attribute__((aligned(4))) static uint8_t pair_ack_tx_dma[17];

static hal_rf_event_cb_t rf_event_cb;

static uint32_t rf_properties(void)
{
    return PHY_MODE_PHY_2M | BB_WHITENING_CH;
}

static uint32_t rf_frequency_for_channel(uint8_t channel)
{
    return channel;
}

void hal_rf_set_event_cb(hal_rf_event_cb_t cb)
{
    rf_event_cb = cb;
}

const uint8_t *hal_rf_rx_buf(void)
{
    return rx_buf;
}

void hal_rf_configure(uint32_t access_addr)
{
    rx_param.accessAddress = access_addr;
}

/* HAL_RF_CHANNEL_CURRENT support (P4): the unified rf_task keeps the CH59x
 * stock idiom — retune via hal_rf_set_channel, then arm on "current" without
 * a retune (the CH59x radio's persistent channel state). The RFIP has no
 * persistent tune, so this shadow byte replicates it: set_channel and every
 * explicit-channel arm refresh it; a CHANNEL_CURRENT arm resolves through it
 * (one compare + load in the hot paths — measured within the SRAM budget of
 * the unified build). Legacy-body arms always pass real channels; behavior
 * there is unchanged. */
static uint8_t rf_tuned_channel = 8u;   /* pair LUT[0] until first tune */

static inline uint8_t rf_resolve_channel(uint8_t channel)
{
    return (channel == HAL_RF_CHANNEL_CURRENT) ? rf_tuned_channel : channel;
}

void hal_rf_set_channel(uint8_t channel)
{
    rf_tuned_channel = channel;
}

__HIGH_CODE
uint8_t hal_rf_start_rx(uint8_t channel, uint16_t timeout)
{
    uint32_t frequency;

    channel = rf_resolve_channel(channel);
    frequency = rf_frequency_for_channel(channel);
    rf_tuned_channel = channel;
    rx_param.frequency = frequency;
    rx_param.whiteChannel = channel;
    /* Seam semantic: timeout 0 = camp indefinitely (the CH59x RF_Rx shape).
     * The RFIP's documented 0 = "no timeout" proved deaf on the bench: an
     * infinite-arm camp never raised RX_DONE, while the
     * legacy finite-timeout + re-arm shape hears everything). Translate the
     * camp to the longest practical window; the expiry raises
     * HAL_RF_EV_RX_TIMEOUT and the body's defensive RX_RESTART re-arms —
     * behaviorally an infinite camp. */
    rx_param.timeOut = timeout ? timeout : 60000u;   /* 30 ms @ 0.5 us */
    return RFIP_SetRx(&rx_param);
}

__HIGH_CODE
uint8_t hal_rf_start_tx(uint8_t channel, uint32_t access_addr,
                        const uint8_t *buf, uint8_t len)
{
    uint32_t frequency;

    channel = rf_resolve_channel(channel);
    frequency = rf_frequency_for_channel(channel);
    rf_tuned_channel = channel;

    if ((uint32_t)len + 2u > sizeof(pair_ack_tx_dma)) {
        return 0xffu;
    }

    tx_param.accessAddress = access_addr;
    tx_param.accessAddressEx = 0;
    tx_param.frequency = frequency;
    tx_param.whiteChannel = channel;
    /* Reserved in PHY2M; kept for 2G4 nondpl trials. */
    tx_param.txLen = len;
    pair_ack_tx_dma[0] = 0u;
    pair_ack_tx_dma[1] = len;
    for (uint8_t i = 0; i < len; i++) {
        pair_ack_tx_dma[i + 2u] = buf[i];
    }
    tx_param.txDMA = (uint32_t)pair_ack_tx_dma;

    return RFIP_StartTx(&tx_param);
}

/* RFIP has no auto-ACK prime concept: this is identical to hal_rf_start_rx().
 * IMPORTANT: despite living in flash, this IS on the timing-critical TX->RX
 * turnaround. On !RF_TASK_EXECUTOR_TMOS (CH570), RF_CONNECTED_POST_POLL_RX_ARM()
 * -> rf_arm_post_poll_rx() (rf_task.c) calls it SYNCHRONOUSLY from the
 * __HIGH_CODE radio-IRQ sink on every connected-poll TX_FINISH (~1143/s), and
 * the EV10 pair-prep path calls it too. It is deliberately left flash-resident
 * (NOT __HIGH_CODE) to respect the 2 KB stack-floor / flash budget -- promoting
 * it would change codegen. A future flash-wait-state change could narrow the
 * reply-capture margin, so start any TX->RX timing-regression hunt here. */
uint8_t hal_rf_start_rx_primed(uint8_t channel, uint16_t timeout,
                               const uint8_t *prime_buf, uint8_t prime_len)
{
    (void)prime_buf;
    (void)prime_len;
    return hal_rf_start_rx(channel, timeout);
}

void hal_rf_shut(void)
{
    (void)RFRole_Shut();
}

__INTERRUPT
__HIGH_CODE
void LLE_IRQHandler(void)
{
    LLE_LibIRQHandler();
}

__INTERRUPT
__HIGH_CODE
void BB_IRQHandler(void)
{
    BB_LibIRQHandler();
}

/*
 * RFIP status callback (IRQ context). Pure PHY-event extraction: map each set
 * status bit onto a hal_rf_event_t and forward it to the registered sink, in
 * the priority order RX -> RX_CRCERR -> TIMEOUT -> TX_FINISH. Each bit is an
 * independent `if` (NOT mutually exclusive) so a multi-bit status dispatches
 * every event, matching the legacy callback's independent-if chain. On RX the
 * frame base is rx_buf and len is rx_buf[1].
 */
__HIGH_CODE
void RF_ProcessCallBack(rfRole_States_t sta, uint8_t id)
{
    (void)id;

    if (sta & RF_STATE_RX) {
        if (rf_event_cb) {
            rf_event_cb(HAL_RF_EV_RX_DONE, rx_buf, rx_buf[1]);
        }
    }
    if (sta & RF_STATE_RX_CRCERR) {
        if (rf_event_cb) {
            rf_event_cb(HAL_RF_EV_RX_CRCERR, rx_buf, 0u);
        }
    }
    if (sta & RF_STATE_TIMEOUT) {
        if (rf_event_cb) {
            rf_event_cb(HAL_RF_EV_RX_TIMEOUT, rx_buf, 0u);
        }
    }
    if (sta & RF_STATE_TX_FINISH) {
        if (rf_event_cb) {
            rf_event_cb(HAL_RF_EV_TX_DONE, rx_buf, 0u);
        }
    }
}

void hal_rf_init(void)
{
    uint32_t props = rf_properties();
    rfRoleConfig_t conf = {0};

    sys_safe_access_enable();
    R32_MISC_CTRL = (R32_MISC_CTRL & (~(0x3fu << 24))) | (0x0eu << 24);
    sys_safe_access_disable();

    conf.rfProcessCB = RF_ProcessCallBack;
    conf.processMask = RF_STATE_RX | RF_STATE_RX_CRCERR |
                       RF_STATE_TX_FINISH | RF_STATE_TIMEOUT;

    /*
     * DO NOT suspend ROM_LOOP_ACC (CORECFGR bit 3) around this call.
     *
     * It was tried -- a write-only guard that dropped CORECFGR to 0x25 across
     * RFRole_BasicInit and restored 0x2D after, to protect the ~99 us
     * calibration settle inside RFEND_DevInit from collapsing to ~3.3 us under
     * the ROM loop buffer. Measured result, same bench, same day, byte-exact
     * control image:
     *
     *   unguarded 0x2D: pairing 2/2, bond written, HID traffic flows
     *   guarded   0x2D: pairing 0/2, bond page still blank
     *
     * The guard did not protect the radio; it broke it -- and backwards from
     * the settle-duration theory (production-length settle FAILED, collapsed
     * settle WORKS). The economical explanation is that the vendor init
     * derives timing-dependent values consumed later at runtime: calibrate at
     * 0x25, run at 0x2D, and every such value is ~30x wrong. Whatever the
     * mechanism, the CONSISTENT configurations both work -- all-0x25 (the old
     * production value) and all-0x2D each pass pairing, reconnect and traffic;
     * mixing them fails. Keep CORECFGR at one value for the whole runtime:
     * whatever reset_handler_ch570.S writes, leave it alone here.
     *
     * (Related: never READ CORECFGR to "save" it -- csrr 0xbc0 destabilises
     * this part outright. It is a write-only register on this silicon.)
     */
    (void)RFRole_BasicInit(&conf);

    tx_param.accessAddress = RF_PAIR_ACCESS_ADDR;
    tx_param.accessAddressEx = 0;
    tx_param.crcInit = CH570_RF_CRC_INIT;
    tx_param.crcPoly = CH570_RF_CRC_POLY;
    tx_param.properties = props;
    tx_param.frequency = rf_frequency_for_channel(8u);
    tx_param.whiteChannel = 8u;
    tx_param.waitTime = 80 * 2;
    tx_param.txPowerVal = CH570_RF_TX_POWER;
    tx_param.txLen = 15;

    rx_param.accessAddress = RF_PAIR_ACCESS_ADDR;
    rx_param.accessAddressEx = 0;
    rx_param.crcInit = CH570_RF_CRC_INIT;
    rx_param.crcPoly = CH570_RF_CRC_POLY;
    rx_param.properties = props;
    rx_param.rxDMA = (uint32_t)rx_buf;
    rx_param.rxMaxLen = CH570_RF_RX_MAX_LEN;

    /* RF radio IRQs must PREEMPT USB. On CH570, USB_IRQn and the RF radio IRQs
     * share the default PFIC priority, so an in-progress USB ISR (control
     * transfer, suspend/resume, transfer completion) delays the time-critical
     * RF RX/poll IRQs -> a connected poll fires late, misses the keyboard's RX
     * window, and the link drops on supervision. (RF-only builds have no USB
     * ISR, which is why they stay connected.) Raise the RF radio IRQs to the
     * high pre-emption level (IPRIOR bit7 = 0x80); interrupt nesting is enabled
     * in startup (intsyscr=0x3). The connected-poll TMR is raised to the SAME
     * level at its enable site in rf_task, so the RF IRQs stay co-equal among
     * themselves (no change to RF-internal timing) but collectively preempt USB. */
    PFIC_SetPriority(BLEB_IRQn, 1);
    PFIC_SetPriority(BLEL_IRQn, 1);
    PFIC_EnableIRQ(BLEB_IRQn);
    PFIC_EnableIRQ(BLEL_IRQn);
}
