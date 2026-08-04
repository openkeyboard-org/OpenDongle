/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * CH592 bring-up for the AES validation image.
 *
 * Unlike the CH570 arm this uses the VENDOR's stock startup rather than the
 * production one. Production's startup_CH592_phased.S carries the BLE task
 * scheduler and the baseband IRQ trampoline, none of which the cipher needs,
 * and CH592's QingKe V4C core has no CORECFGR to preserve -- CSR 0xBC0 is a
 * different register on V4C, which is why the harness compiles its CORECFGR
 * read out entirely on this chip. So there is nothing to be gained here by
 * dragging in the production startup, and a good deal of RF machinery to go
 * wrong in ways that would look like a cipher fault.
 *
 * The AES engine itself needs no bring-up: it answers correctly from a cold
 * reset on this family, which is what made the CH572 known-answer run possible
 * with the program counter still at zero.
 */
#include "CH59x_common.h"

#include "aes_log_format.h"

#include <stdint.h>

extern volatile uint32_t aes_log[AES_LOG_WORDS];

#ifndef DONGLE_VALIDATE_SYSCLK_SOURCE
#define DONGLE_VALIDATE_SYSCLK_SOURCE CLK_SOURCE_PLL_60MHz
#endif

/* Production CH592 runs at 60 MHz; see the CH570 platform file for why the
 * validation image must match production's clock rather than pick its own. */
_Static_assert(DONGLE_VALIDATE_SYSCLK_SOURCE == CLK_SOURCE_PLL_60MHz,
               "validation must run at the production 60 MHz core clock, or "
               "its cycle figures cannot be compared with production's");

void aes_validate_platform_init(void)
{
    SetSysClock(DONGLE_VALIDATE_SYSCLK_SOURCE);
}

/* See the CH570 platform file for why these record rather than spin. */
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
    /*
     * Stamp a minimal header first. A fault before main() has written the log
     * header leaves aes_log[0] zero, and the reader rejects the whole dump on
     * bad magic -- so the fault record four words later, the only evidence of
     * what happened, is never even looked at. Exactly the early faults hardest
     * to debug are the ones that would be invisible. These are the same values
     * main() writes, so stamping them here is idempotent on a normal run.
     */
    aes_log[0] = AES_LOG_MAGIC;
    aes_log[1] = AES_LOG_VERSION;
    aes_log[2] = AES_LOG_START;

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
