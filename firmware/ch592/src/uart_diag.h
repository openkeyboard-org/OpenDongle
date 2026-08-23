/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * BENCH ONLY: periodic link-encryption telemetry over UART1 (chip-default
 * pins: TX = PA9, RX = PA8), for a bench where the dongle's USB is not
 * connected and CMD_CRYPT_DIAG is unreachable. TX-only, non-blocking: the
 * service call stuffs at most a FIFO's worth of bytes per main-loop pass and
 * never waits, so it cannot stall the RF task.
 *
 * Wire format, one frame per second:
 *   [0x5E][len=124][payload][chk]
 *   chk = (0x5E + len + sum(payload)) & 0xFF
 *
 * payload (all multi-byte fields LE):
 *   off 0   ok(4)  drop[6](24)                       -- rf_crypt_status_t order
 *   off 28  conn_rx(4) enc_shape(4) fifo_full(4) flush_drop(4) plain_drop(4)
 *   off 48  len_max(1) len_max_tag(1)
 *   off 50  session_mint_count(4) mac_same_ok(4) last_mac_ctr(4)
 *   off 62  mac_prev_ok(4) same_differs(4) bb_during_aes(4)
 *   off 74  kat_run(1) kat_fail(1)
 *   off 76  fail_latched(1) fail_len(1) fail_session(4) fail_counter(4)
 *   off 86  fail_expect1(8) fail_expect2(8) fail_frame(22)
 *   = 124 bytes
 */
#ifndef UART_DIAG_H
#define UART_DIAG_H

void UartDiag_Init(void);
void UartDiag_Service(void);

#endif /* UART_DIAG_H */
