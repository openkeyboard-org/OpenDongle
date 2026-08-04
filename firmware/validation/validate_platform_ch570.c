/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * CH570/CH572 bring-up for the AES validation image.
 *
 * The validation image reuses the PRODUCTION startup chain -- the vendor
 * startup_CH572.S plus this firmware's own reset_handler_ch570.S -- rather than
 * a bespoke one, because that reset handler is what writes CORECFGR. Bit 3
 * (ROM_LOOP_ACC) changes the flash-resident backends' cost by roughly 15x, so a
 * validation image that set up the core its own way would be measuring a core
 * production never runs. The harness reads CORECFGR back and logs it, and the
 * reader prints it, so the config each number was taken under is never in doubt.
 *
 * What this file does NOT pull in is deliberate: no fault_record.c, no RF, no
 * USB, no OpenBoot companion. The image only has to run the cipher and be read
 * out over the debug probe, and every extra dependency is another thing that
 * can fail in a way that looks like a cipher fault.
 */
#include "CH57x_common.h"

#include "aes_log_format.h"

#include <stdint.h>

extern volatile uint32_t aes_log[AES_LOG_WORDS];

#ifndef DONGLE_VALIDATE_SYSCLK_SOURCE
#define DONGLE_VALIDATE_SYSCLK_SOURCE CLK_SOURCE_HSE_PLL_100MHz
#endif

/*
 * Production runs the radio on a 100 MHz core clock and every published cycle
 * figure was taken there. Cycle COUNTS are clock-independent, but flash read
 * latency in core cycles is not -- and the flash-resident backends are
 * dominated by exactly that -- so the clock has to match or the numbers are not
 * comparable. The harness logs the value it was told, and the reader prints it.
 */
_Static_assert(DONGLE_VALIDATE_SYSCLK_SOURCE == CLK_SOURCE_HSE_PLL_100MHz,
               "validation must run at the production 100 MHz core clock, or "
               "its cycle figures cannot be compared with production's");

void aes_validate_platform_init(void)
{
    /*
     * Configure the HSE load capacitance BEFORE switching to the PLL, exactly
     * as production does (ch570/src/main.c). Omitting this was a real defect in
     * the first version of this file, and its signature was nasty: the known-
     * answer vectors all passed, then the 512-block differential stopped
     * partway through. A few hundred microseconds of work is not enough to
     * expose a marginal 32 MHz crystal feeding a 100 MHz PLL; sustained work is.
     *
     * The general lesson for this image: any bring-up step production performs
     * before the cipher runs is part of the environment being validated. A
     * validation image that sets the core up its own way measures a core that
     * does not ship.
     */
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(DONGLE_VALIDATE_SYSCLK_SOURCE);
}

/*
 * Strong overrides for the vendor's weak fatal vectors. reset_handler_ch570.S
 * installs both before it copies .highcode, so they must resolve.
 *
 * These used to spin silently, on the reasoning that a fault would show up as
 * a log with no end marker and that was detail enough. That reasoning was
 * wrong, and it cost a debugging session: when both CH570 arms stopped partway
 * through the differential, "incomplete" could not tell a fault from a genuine
 * hang, and the 32-block checkpoint spacing gave no better resolution. The
 * record below costs four reserved words and names the faulting instruction.
 */
void NMI_Handler(void) __attribute__((interrupt()));
void HardFault_Handler(void) __attribute__((interrupt()));

/*
 * Record the fault where the host can see it, then stop.
 *
 * CSRs are addressed numerically (mepc 0x341, mcause 0x342, mtval 0x343) so
 * this does not depend on the assembler knowing QingKe's CSR names.
 */
static void record_fault(uint32_t kind)
{
    uint32_t mcause, mepc, mtval;
    __asm__ volatile("csrr %0, 0x342" : "=r"(mcause));
    __asm__ volatile("csrr %0, 0x341" : "=r"(mepc));
    __asm__ volatile("csrr %0, 0x343" : "=r"(mtval));
    aes_log[AES_LOG_FAULT_SLOT + 0u] = AES_LOG_FAULT_MAGIC | kind;
    aes_log[AES_LOG_FAULT_SLOT + 1u] = mcause;
    aes_log[AES_LOG_FAULT_SLOT + 2u] = mepc;
    aes_log[AES_LOG_FAULT_SLOT + 3u] = mtval;
    for (;;)
        ;
}

void NMI_Handler(void)
{
    record_fault(AES_LOG_FAULT_NMI);
}

void HardFault_Handler(void)
{
    record_fault(AES_LOG_FAULT_HARD);
}
