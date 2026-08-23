/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * BENCH ONLY: see uart_diag.h for the wire format and why this exists.
 *
 * UART1 register recipe mirrors OpenBoot's ports/ch59x (default PA8/PA9
 * mapping, DL = round(Fsys/8/baud), DIV = 1, RB_IER_TXD_EN gates the TXD pin
 * driver). Counter reads are word loads (atomic on RV32); a frame snapshots
 * them field-by-field, so cross-field tearing is possible and acceptable for
 * telemetry.
 */
#include "CONFIG.h"

#include "dongle_target.h"   /* gates, before rf_crypt.h */
#include "rf_crypt.h"
#include "uart_diag.h"

#if DONGLE_UART_DIAG

/* Pre-verify sink counters live in rf_task.c; iap.c declares them the same
 * way (there is no shared header for them, deliberately -- IAP is their
 * canonical reader and this module is a bench mirror of it). */
extern uint32_t rf_crypt_drop_reason[6];
extern uint32_t rf_crypt_ok_count;
extern uint32_t rf_crypt_conn_rx;
extern uint32_t rf_crypt_enc_shape;
extern uint32_t rf_crypt_fifo_full;
extern uint32_t rf_crypt_flush_drop;
extern uint32_t rf_crypt_plain_drop;
extern uint8_t  rf_crypt_len_max;
extern uint8_t  rf_crypt_len_max_tag;

#define UART_DIAG_PAYLOAD 124u
#define UART_DIAG_FRAME   (UART_DIAG_PAYLOAD + 3u)
#define UART_DIAG_PERIOD_TMOS 1600u   /* 1600 x 625 us = 1.0 s */

/* FCR per the EVT IAP: RX trigger 4 bytes, TX+RX FIFO reset, FIFO enable. */
#define UART_DIAG_FCR ((2u << 6) | 0x04u | 0x02u | 0x01u)

static uint8_t  ud_buf[UART_DIAG_FRAME];
static uint8_t  ud_idx;      /* next byte to send; == ud_len when idle */
static uint8_t  ud_len;
static uint32_t ud_last_tmos;

void UartDiag_Init(void)
{
    /* TX must idle high before the direction flip (OpenBoot ports/ch59x).
     * PA9 = TXD1 out, PA8 = RXD1 in with pull-up (unused, kept sane), and
     * PA8's digital-input-disable cleared per the vendor GPIOA_PinCfg. */
    const uint32_t tx = (1u << 9);
    const uint32_t rx = (1u << 8);

    R32_PA_OUT    |= tx;
    R32_PA_PD_DRV &= ~(tx | rx);
    R32_PA_DIR    |= tx;
    R32_PA_PU     |= rx;
    R32_PA_DIR    &= ~rx;
    R32_PIN_CONFIG2 &= ~(1u << 8);
    R16_PIN_ALTERNATE &= (uint16_t)~RB_PIN_UART1;   /* default mapping */

    R8_UART1_FCR = UART_DIAG_FCR;
    R8_UART1_LCR = RB_LCR_WORD_SZ;                  /* 8N1 */
    R16_UART1_DL = (uint16_t)(((10u * 60000000u / 8u) / 115200u + 5u) / 10u);
    R8_UART1_DIV = 1;
    R8_UART1_IER = RB_IER_TXD_EN;

    ud_idx = 0;
    ud_len = 0;
    ud_last_tmos = TMOS_GetSystemClock();
}

static void ud_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void ud_build(void)
{
    uint8_t *p = &ud_buf[2];
    uint8_t  i;
    uint32_t chk;

    ud_buf[0] = 0x5Eu;
    ud_buf[1] = UART_DIAG_PAYLOAD;

    ud_le32(&p[0], rf_crypt_ok_count);
    for (i = 0; i < 6u; i++) {
        ud_le32(&p[4 + 4u * i], rf_crypt_drop_reason[i]);
    }
    ud_le32(&p[28], rf_crypt_conn_rx);
    ud_le32(&p[32], rf_crypt_enc_shape);
    ud_le32(&p[36], rf_crypt_fifo_full);
    ud_le32(&p[40], rf_crypt_flush_drop);
    ud_le32(&p[44], rf_crypt_plain_drop);
    p[48] = rf_crypt_len_max;
    p[49] = rf_crypt_len_max_tag;
#if RF_CRYPT_DIAG_PREV_SESSION
    ud_le32(&p[50], rf_crypt_session_mint_count);
    ud_le32(&p[54], rf_crypt_mac_same_ok);
    ud_le32(&p[58], rf_crypt_last_mac_ctr);
    ud_le32(&p[62], rf_crypt_mac_prev_ok);
    ud_le32(&p[66], rf_crypt_same_differs);
    ud_le32(&p[70], rf_crypt_bb_during_aes);
    p[74] = rf_crypt_kat_run;
    p[75] = rf_crypt_kat_fail;
    p[76] = rf_crypt_fail_latched;
    p[77] = rf_crypt_fail_len;
    ud_le32(&p[78], rf_crypt_fail_session);
    ud_le32(&p[82], rf_crypt_fail_counter);
    for (i = 0; i < 8u; i++) {
        p[86 + i] = rf_crypt_fail_expect1[i];
        p[94 + i] = rf_crypt_fail_expect2[i];
    }
    for (i = 0; i < 22u; i++) {
        p[102 + i] = rf_crypt_fail_frame[i];
    }
#else
    for (i = 50u; i < UART_DIAG_PAYLOAD; i++) {
        p[i] = 0u;
    }
#endif

    chk = 0u;
    for (i = 0; i < (uint8_t)(UART_DIAG_PAYLOAD + 2u); i++) {
        chk += ud_buf[i];
    }
    ud_buf[UART_DIAG_PAYLOAD + 2u] = (uint8_t)chk;

    ud_idx = 0;
    ud_len = UART_DIAG_FRAME;
}

void UartDiag_Service(void)
{
    /* Drain first: stuff the TX FIFO with whatever fits, never wait. */
    while (ud_idx < ud_len && R8_UART1_TFC < UART_FIFO_SIZE) {
        R8_UART1_THR = ud_buf[ud_idx++];
    }
    if (ud_idx < ud_len) {
        return;                     /* frame still going out */
    }

    {
        uint32_t now = TMOS_GetSystemClock();
        if ((uint32_t)(now - ud_last_tmos) < UART_DIAG_PERIOD_TMOS) {
            return;
        }
        ud_last_tmos = now;
    }
    ud_build();
}

#endif /* DONGLE_UART_DIAG */
