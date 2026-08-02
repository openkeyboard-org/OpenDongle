/*
 * BB_IRQHandler trampoline for the CH59x BLE library.
 *
 * The CH59x SDK's ble_task_scheduler.S only provides a strong override for
 * LLE_IRQHandler (which dispatches through the g_LLE_IRQLibHandlerLocation
 * function pointer); BB_IRQHandler is left as the weak empty default in
 * startup_CH592.S.
 *
 * Without a real BB handler, baseband interrupts from the radio (RX-complete,
 * TX-complete, CRC events) are silently dropped. The result is that
 * `RF_2G4StatusCallBack` never fires, even though the radio is configured
 * via RF_Config + RF_Rx — measured during OQ7 SRAM-diff bench-test
 * (2026-05-17): the (since-retired) RX callback counters[0..5] all zero after a full pair attempt.
 *
 * LIBCH59xBLE.a exports BB_IRQLibHandler as a direct symbol (unlike the
 * LLE handler which uses the function-pointer indirection). We wire BB
 * with the same software-save-restore prologue/epilogue as the SDK's LLE
 * trampoline, then jalr into BB_IRQLibHandler. The lib handler is a
 * regular C function (extern void X(void) in CH59xBLE_LIB.h), not an ISR,
 * so caller-saved registers may be clobbered without our prologue.
 *
 * Software save/restore is required even though startup sets CSR 0x804=0x3
 * (HPE on) because the hardware-stacked region is only restored on mret —
 * any user register the C-style library function clobbers would still be
 * trashed when control returns to whatever was interrupted.
 */

    .extern BB_IRQLibHandler

    .global BB_IRQHandler

    .section .highcode.BB_IRQHandler, "ax", @progbits
    .align 2
    .func BB_IRQHandler
BB_IRQHandler:

    addi sp, sp, -32*4

    sw x1, 1 * 4( sp )
    sw x4, 2 * 4( sp )
    sw x5, 3 * 4( sp )
    sw x6, 4 * 4( sp )
    sw x7, 5 * 4( sp )
    sw x8, 6 * 4( sp )
    sw x9, 7 * 4( sp )
    sw x10, 8 * 4( sp )
    sw x11, 9 * 4( sp )
    sw x12, 10 * 4( sp )
    sw x13, 11 * 4( sp )
    sw x14, 12 * 4( sp )
    sw x15, 13 * 4( sp )
    sw x16, 14 * 4( sp )
    sw x17, 15 * 4( sp )
    sw x18, 16 * 4( sp )
    sw x19, 17 * 4( sp )
    sw x20, 18 * 4( sp )
    sw x21, 19 * 4( sp )
    sw x22, 20 * 4( sp )
    sw x23, 21 * 4( sp )
    sw x24, 22 * 4( sp )
    sw x25, 23 * 4( sp )
    sw x26, 24 * 4( sp )
    sw x27, 25 * 4( sp )
    sw x28, 26 * 4( sp )
    sw x29, 27 * 4( sp )
    sw x30, 28 * 4( sp )
    sw x31, 29 * 4( sp )

    la a0, BB_IRQLibHandler
    jalr x1, 0(a0)

    lw x1, 1 * 4( sp )
    lw x4, 2 * 4( sp )
    lw x5, 3 * 4( sp )
    lw x6, 4 * 4( sp )
    lw x7, 5 * 4( sp )
    lw x8, 6 * 4( sp )
    lw x9, 7 * 4( sp )
    lw x10, 8 * 4( sp )
    lw x11, 9 * 4( sp )
    lw x12, 10 * 4( sp )
    lw x13, 11 * 4( sp )
    lw x14, 12 * 4( sp )
    lw x15, 13 * 4( sp )
    lw x16, 14 * 4( sp )
    lw x17, 15 * 4( sp )
    lw x18, 16 * 4( sp )
    lw x19, 17 * 4( sp )
    lw x20, 18 * 4( sp )
    lw x21, 19 * 4( sp )
    lw x22, 20 * 4( sp )
    lw x23, 21 * 4( sp )
    lw x24, 22 * 4( sp )
    lw x25, 23 * 4( sp )
    lw x26, 24 * 4( sp )
    lw x27, 25 * 4( sp )
    lw x28, 26 * 4( sp )
    lw x29, 27 * 4( sp )
    lw x30, 28 * 4( sp )
    lw x31, 29 * 4( sp )

    addi sp, sp, 32*4

    mret
    .endfunc
