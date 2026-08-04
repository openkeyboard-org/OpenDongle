/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CH570 AES backend selection.
 *
 * DEFINES ONLY — no types, no declarations. This header is included by
 * hal_aes_ch570_asm.S as well as by C, so anything the assembler cannot parse
 * must not appear here.
 *
 * Three backends, all producing identical ciphertext (512-vector differential
 * against a CH572 hardware AES engine, FNV-1a checksum b106130c, compared
 * byte-for-byte). Cycles measured on CH570 silicon at 100 MHz; SRAM and stack
 * margin measured from this firmware's own link, with the seam retained:
 *
 *   IMPL    cycles/block   key sched   SRAM added   .highcode   stack margin
 *   ASM_A        1,944      60,538        840 B       404 B        2,164 B
 *   ASM_F        3,797         n/m        432 B         0 B        2,572 B
 *   C           43,510      47,759        432 B         0 B        2,572 B
 *
 * Cycles are measured by firmware/validation on production-faithful silicon and
 * reproduce bit-identically between runs; ASM_F's is a bench figure (n/m here)
 * because that backend cannot be built while CORECFGR bit 3 is clear.
 *
 * ASM_A's KEY SCHEDULE IS SLOWER THAN THE PORTABLE ONE -- 60,538 against
 * 47,759 -- and that is deliberate, not a defect. Variant A holds the state as
 * four ROW words, so hal_aes_ch570.c transposes the schedule once at set_key
 * time, which is what makes AddRoundKey four plain word loads inside the
 * kernel. The extra ~12,800 cycles are repaid after 0.31 blocks, since each
 * block is 41,566 cycles cheaper than the portable backend. Both schedules are
 * ordinary C in flash; there is no hand-written assembly key schedule for any
 * backend, and adding one is not the lever here -- see the note below.
 *
 * The 875 us connected-poll slot is 87,500 cycles. The linker asserts a 2,048 B
 * stack floor (CH570_STACK_FLOOR), so ASM_A ships with 116 B to spare and
 * ASM_F with 524 B.
 *
 * NOTE THE C ROW. The portable backend is FLASH-resident, because aes_sw.c is
 * shared with CH592 and carries no chip-specific section attributes -- putting
 * __HIGH_CODE in common code to suit one part would be wrong. So C costs
 * 43,510 cycles/block, 435 us, 50% of a poll slot. It exists for PORTABILITY,
 * not performance: it is what a future chip gets before anyone writes assembly
 * for it, and it is what firmware/tests/test_aes_sw.py exercises on the host.
 * Do not select it on CH570 expecting the 2,133-cycle figure quoted in
 * bench/aes_spike/ -- that was a bench build with the cipher forced into
 * .highcode and the unroll pragma disabled, which is not what ships here.
 *
 * ON THE KEY SCHEDULE COSTING 69% OF A POLL SLOT. It is flash-resident C for
 * every backend, on purpose: it runs once per key, so making it fast buys
 * nothing per block, and keeping it out of SRAM is what lets the cipher fit.
 * Hand-writing it in assembly is the wrong lever -- it would spend scarce SRAM
 * and review effort on a once-per-session cost. The cheap lever, if that 605 us
 * ever needs to come down, is CORECFGR bit 3: the schedule is a tight
 * flash-resident loop, exactly the shape the ROM loop buffer accelerates, and
 * the bench measured this same schedule at 8,890 cycles with the buffer on
 * (6.8x). That costs no SRAM at all. It is not yet done and carries
 * firmware-wide hazards -- see the ASM_F note below and CORE-FINDINGS.md.
 *
 * What the number really constrains is WHEN set_key may be called, not how it
 * is written: 605 us does not fit inside an 875 us slot alongside anything
 * else, so re-key outside the poll grid.
 *
 * Cycles are NOT the axis that matters between ASM_A and ASM_F; SRAM is. The
 * part has 12,272 B and the firmware already commits 9,268 B before any cipher.
 *
 * ASM_A is the default because it is 25x faster than the portable C for 408 B
 * more SRAM, and because it depends on no unadvertised ISA extension. (misa
 * reads 0x40901106: RV32 I M B C U X, where ratified B is Zba+Zbb+Zbs and
 * excludes Zbc. clmul does work on this silicon, verified on both a CH570 and a
 * CH572, and two of the rejected variants needed it — ASM_A never does.)
 *
 * A "22% faster in 124 fewer bytes" comparison appears in
 * bench/aes_spike/DECISION.md. That is ASM_A against a BENCH build of the C
 * cipher, forced into .highcode with its unroll pragma disabled — a
 * configuration that does not ship. Against the three rows above the honest
 * numbers are 25x and +408 B. Do not carry the bench comparison into shipping
 * documentation.
 *
 * On the SRAM figure: 840 B is the measured delta from a real link of this
 * firmware. Its nominal components are 404 B .highcode + 256 B S-box + 176 B
 * schedule = 836 B; the remaining 4 B is alignment padding on .data. A bench
 * build of the same kernel reports 400 B of code rather than 404 because it
 * links with different alignment and relaxation — expect small differences
 * between bench and firmware numbers, and trust the firmware link.
 *
 * ASM_F is the alternate for when SRAM, not speed, is the binding constraint.
 * It puts ZERO bytes of code in SRAM: its 432 B is entirely data (256 B S-box +
 * 176 B round-key schedule), leaving the radio .highcode untouched.
 *
 *   !! ASM_F REQUIRES CORECFGR bit 3 (ROM_LOOP_ACC) TO BE SET !!
 *
 * Its kernel is a flash-resident loop shaped to fit the QingKe V3C 128-byte ROM
 * loop buffer. With bit 3 clear it stays bit-exact correct but degrades to an
 * estimated 42,000-68,000 cycles/block — no better than an unoptimised
 * flash-resident cipher and possibly worse. Production currently boots
 * CORECFGR = 0x25, which has bit 3 CLEAR. Do not select ASM_F until the startup
 * value is 0x2D. See bench/aes_spike/CORE-FINDINGS.md for the measurements and
 * for why 0x2D specifically (ch32fun's 0x0f/0x1f clear IE_REMAP_EN, which makes
 * CSR 0x800 read-only and silently breaks __enable_irq/__disable_irq).
 *
 * C is kept permanently as the portable fallback for any future chip, and is
 * what firmware/tests/test_aes_sw.py exercises on the host.
 */
#ifndef HAL_AES_CH570_IMPL_H
#define HAL_AES_CH570_IMPL_H

#include "ch570_corecfgr.h"

#define DONGLE_AES_CH570_IMPL_ASM_A  1
#define DONGLE_AES_CH570_IMPL_ASM_F  2
#define DONGLE_AES_CH570_IMPL_C      3

#ifndef DONGLE_AES_CH570_IMPL
#define DONGLE_AES_CH570_IMPL DONGLE_AES_CH570_IMPL_ASM_A
#endif

#if DONGLE_AES_CH570_IMPL != DONGLE_AES_CH570_IMPL_ASM_A && \
    DONGLE_AES_CH570_IMPL != DONGLE_AES_CH570_IMPL_ASM_F && \
    DONGLE_AES_CH570_IMPL != DONGLE_AES_CH570_IMPL_C
#error "DONGLE_AES_CH570_IMPL must be one of DONGLE_AES_CH570_IMPL_{ASM_A,ASM_F,C}"
#endif

/*
 * ASM_F's advertised cost is only true with ROM_LOOP_ACC set. Enforce that
 * against the value the startup ACTUALLY writes, not against a comment or an
 * acknowledgement macro that could drift from it.
 *
 * Without this, selecting ASM_F today would produce a bit-exact but roughly
 * 15x slower cipher, silently, with the header still claiming 3,797 cycles.
 * That is precisely the class of trap this whole investigation kept hitting.
 */
#if DONGLE_AES_CH570_IMPL == DONGLE_AES_CH570_IMPL_ASM_F && \
    ((CH570_CORECFGR_VALUE) & (CH570_CORECFGR_ROM_LOOP_ACC)) == 0
#error "DONGLE_AES_CH570_IMPL_ASM_F requires ROM_LOOP_ACC (CORECFGR bit 3). \
CH570_CORECFGR_VALUE in ch570_corecfgr.h has it clear, so this build would be \
correct but ~15x slower than ASM_F's documented cost. Set CORECFGR to 0x2D -- \
which is a firmware-wide change with its own hazards, see \
bench/aes_spike/CORE-FINDINGS.md -- or select ASM_A."
#endif

#endif /* HAL_AES_CH570_IMPL_H */
