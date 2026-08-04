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
 * Bit map (QingKe V3 processor manual V1.5; the CH570/CH572 datasheet's table is
 * lower fidelity and disagrees in several places):
 *
 *   [1:0] FETCH_MODE     01 = prefetch on. Measured: 01 and 11 are
 *                        indistinguishable on this silicon, and the datasheet
 *                        marks 11 Reserved for this part, so leave it at 01.
 *   2     ROM_JUMP_ACC   jump acceleration
 *   3     ROM_LOOP_ACC   128-byte ROM LOOP BUFFER -- not the general instruction
 *                        cache the datasheet calls it. Worth ~30x on a
 *                        flash-resident loop whose body is <=112 bytes, 1.44x on
 *                        the AES cipher, and it costs no memory. OFF today; see
 *                        the note below before turning it on.
 *   5     IE_REMAP_EN    REQUIRED. With this clear, CSR 0x800 is read-only, which
 *                        silently breaks __enable_irq/__disable_irq and this
 *                        firmware's own csrrs/csrrc on 0x800 (rf_task.c,
 *                        usb_device.c). This is why ch32fun's 0x0f/0x1f must NOT
 *                        be copied here.
 *
 * TURNING ON ROM_LOOP_ACC (0x25 -> 0x2D) is a firmware-wide behavioural change,
 * not a local one, and it is NOT yet done. Measured consequences and the two
 * known hazards -- a delay loop inside libCH57xRF.a at RFEND_DevInit+0x76 that
 * would collapse from ~50-100 us to ~3 us, and the fact that code CALLED FROM a
 * loop also speeds up ~2.1x so the hazard surface is wider than loop bodies --
 * are written up in bench/aes_spike/CORE-FINDINGS.md. Do not flip it without
 * running the RF end-to-end A/B described there.
 */
#ifndef CH570_CORECFGR_H
#define CH570_CORECFGR_H

#define CH570_CORECFGR_ROM_LOOP_ACC  0x08
#define CH570_CORECFGR_IE_REMAP_EN   0x20

/* The value reset_handler_ch570.S writes to CSR 0xBC0. Change it in ONE place. */
#define CH570_CORECFGR_VALUE         0x25

#endif /* CH570_CORECFGR_H */
