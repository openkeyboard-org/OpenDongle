/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * The single definition of CH570's CORECFGR (CSR 0xBC0) startup value.
 *
 * DEFINES ONLY — included by reset_handler_ch570.S as well as by C, so nothing
 * the assembler cannot parse may appear here.
 *
 * This exists so that code which DEPENDS on a particular CORECFGR bit can check
 * the value the startup actually writes, rather than a comment asserting what it
 * ought to be. An acknowledgement macro disconnected from startup would prevent
 * accidents but could still lie.
 *
 * Bit map, from the QingKe V3 processor manual V1.5. The CH570/CH572 datasheet
 * has a table for this register too and it is lower fidelity -- it marks bit 6
 * Reserved where the manual documents INT_FENCE, marks FETCH_MODE=11 Reserved
 * where the manual documents it as a valid mode, and calls bit 3 a general
 * instruction cache when it is not one. Prefer the manual:
 *
 *   [1:0] FETCH_MODE     01 = prefetch on. Measured: 01 and 11 are
 *                        indistinguishable on this silicon, and the datasheet
 *                        marks 11 Reserved for this part, so leave it at 01.
 *   2     ROM_JUMP_ACC   jump acceleration
 *   3     ROM_LOOP_ACC   128-byte ROM LOOP BUFFER, not a cache: a flash loop
 *                        whose body is <=112 bytes (120 with the head at
 *                        .balign 128) runs at ~1.10 cycles/instruction instead
 *                        of ~33, while straight-line flash code is unaffected.
 *                        Never a slowdown at any loop count -- the first pass
 *                        fills the buffer at unbuffered speed, so even a
 *                        once-through loop breaks even. Costs no SRAM and no
 *                        flash (8 KB of SRAM verified byte-for-byte across
 *                        enabling, hammering and disabling it). Measured on
 *                        the AES paths: 1.16x-1.41x per block, 6.6x on the key
 *                        schedule. ON in production (bit 3 of the 0x2D below);
 *                        see the rules above.
 *   5     IE_REMAP_EN    REQUIRED. With this clear, CSR 0x800 is read-only, which
 *                        silently breaks __enable_irq/__disable_irq and this
 *                        firmware's own csrrs/csrrc on 0x800 (rf_task.c,
 *                        usb_device.c). This is why ch32fun's 0x0f/0x1f must NOT
 *                        be copied here.
 *
 * ROM_LOOP_ACC IS ON IN PRODUCTION (0x25 -> 0x2D), validated on silicon:
 *
 *  - AES: every backend still folds b106130c; ASM_A 1,944 -> 1,672
 *    cycles/block and its key schedule 59,113 -> 8,899 (6.6x, 68% of a poll
 *    slot down to 10%); ASM_F becomes buildable and measures ~3,960-3,992.
 *    (Those key-schedule figures are the flip sweep's, timed with a flash-
 *    resident key; the field has since moved to SRAM-key timing and reads
 *    7,647 -- see validation/README.md. The ratio is what mattered.) Figures
 *    reproduce to the cycle across independent sweeps (firmware/validation).
 *  - RF end-to-end at 0x2D on a production keyboard: pairing, reconnect,
 *    typing, indicators, media keys and sleep/wake all pass; bench pairing
 *    verified down to the bond write and HID reports on the host. The known
 *    hazard -- the calibration settle in libCH57xRF.a's RFEND_DevInit
 *    collapsing ~99 us -> ~3.3 us -- did not manifest on any functional path.
 *    Residual risk is RF margin at range, which bench-distance tests cannot
 *    rule out.
 *
 * TWO RULES THIS VALUE MUST OBEY, both measured the hard way:
 *
 *  1. WRITE IT ONCE, AT RESET, AND NEVER TOUCH IT AGAIN. A guard that dropped
 *     to 0x25 across the vendor RF bring-up and restored 0x2D after broke
 *     pairing outright (0/2, bond never written) while both consistent
 *     configurations pass (all-0x25 and all-0x2D). The vendor init appears to
 *     derive timing-dependent values consumed at runtime; init and runtime
 *     must therefore run under the SAME core configuration. See the note at
 *     the RFRole_BasicInit call in hal_rf_ch570.c.
 *  2. NEVER READ IT. CSR 0xBC0 is write-only on this silicon; csrr
 *     destabilises the part (A/B: 3/3 failures with the read, 2/2 passes
 *     without, changing nothing else).
 */
#ifndef CH570_CORECFGR_H
#define CH570_CORECFGR_H

#define CH570_CORECFGR_ROM_LOOP_ACC  0x08
#define CH570_CORECFGR_IE_REMAP_EN   0x20

/* The value reset_handler_ch570.S writes to CSR 0xBC0. Change it in ONE place,
 * and only here -- see the two rules above. */
#define CH570_CORECFGR_VALUE         0x2D

#endif /* CH570_CORECFGR_H */
