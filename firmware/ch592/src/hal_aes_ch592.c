/*
 * OpenKeyboard.org OpenDongle -- CH592 AES HAL (hardware backend).
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Implements hal_aes.h over the CH592's undocumented hardware AES-128 engine.
 *
 * WCH documents that the engine exists and nothing else: CH592DS1 section 19.5
 * "AES Module" reads, in full, "use it via the BLE protocol stack library", and
 * chapter 19 states the radio's registers are intentionally undocumented. The
 * map below was recovered by disassembling WCH's own aes.o out of LIBCH59xBLE.a
 * and then confirmed on silicon: the vendor's BLE_IPCoreInit() stores the base
 * into its gptrAESReg global, and reading that global from a running dongle
 * returns 0x4000C300. The sequence here passes FIPS-197 C.1, the all-zero
 * vector, a key-avalanche check and a decrypt round-trip on real hardware.
 *
 * Why not just call the vendor's LL_Encrypt(), which is already linked in
 * LIBCH59xBLE.a and declared in CH59xBLE_LIB.h: it calls phy_status_clear(13)
 * first, which ABORTS an in-flight radio operation. That is fine inside WCH's
 * BLE stack, which owns the radio's schedule, but this firmware drives the radio
 * itself on an 875 us poll grid and cannot have a crypto call reach in and
 * cancel a transmit. Driving the registers directly is the same engine without
 * that side effect. (LL_Encrypt also sets bit 7 of the LLE register at
 * 0x4000C250 every call; the engine was verified to work without it, and the
 * shipping firmware leaves that bit clear.)
 *
 * One trap worth recording: the vendor's own busy-wait polls STA bit 0, and
 * copying that verbatim HANGS FOREVER outside the BLE stack, because it is the
 * LLE interrupt that clears STA. On hardware, CFG bit 0 self-clears on
 * completion while STA bit 0 stays set. Poll CFG.
 */
#include "hal_aes.h"

#include <stdint.h>

/*
 * Overridable so a host test can point the register window at ordinary memory.
 * The HAL_AES_ENGINE_TIMEOUT branch below is unreachable on a healthy engine --
 * it needs the busy bit to stay set, which no working part will do on demand --
 * so a mocked register block is the only way that path is ever executed. See
 * tests/test_aes_ch592_mock.py. Nothing but a test should define this.
 */
#ifndef AES_BASE
#define AES_BASE 0x4000C300u
#endif
#define AES_REG(off) (*(volatile uint32_t *)(uintptr_t)(AES_BASE + (off)))

#define AES_CFG     AES_REG(0x00) /* bit0 start/busy, bit1 0=encrypt 1=decrypt */
#define AES_STA     AES_REG(0x04) /* bit0 armed (software-owned), bit1 done */
#define AES_DATA(i) AES_REG(0x18u + 4u * (i)) /* 16 bytes in/out, in place */
#define AES_KEY(i)  AES_REG(0x28u + 4u * (i)) /* 16 bytes */

/* Generous bound so a wedged engine degrades to a wrong answer rather than a
 * hung dongle. A real operation completes in tens of cycles. */
#define AES_SPIN_LIMIT 100000u

/* Per-block interrupt mask (see the note in hal_aes_encrypt_block). Host
 * builds (the mocked-register tests) have no mstatus; the mask is a no-op
 * there, which is also correct -- nothing preempts the host harness. */
#if defined(__riscv)
#define AES_IRQ_SAVE(mie) \
    __asm__ volatile("csrrci %0, mstatus, 0x8" : "=r"(mie) :: "memory")
#define AES_IRQ_RESTORE(mie) \
    do { if ((mie) & 0x8u) \
        __asm__ volatile("csrsi mstatus, 0x8" ::: "memory"); } while (0)
#else
#define AES_IRQ_SAVE(mie)    ((mie) = 0u)
#define AES_IRQ_RESTORE(mie) ((void)(mie))
#endif

static uint32_t aes_key_words[4];

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

void hal_aes_init(void)
{
    /* The engine needs no bring-up of its own: it is already reachable in the
     * shipping configuration (hal_rf_init() -> RF_RoleInit() -> BLE_IPCoreInit()
     * has run), and it answers correctly from a cold reset on this family. */
}

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    /* The engine has no persistent key register across a CFG reset pulse, so
     * the key is cached here and reloaded per block.
     *
     * "A few stores" at the source level -- but GCC compiles this loop into a
     * tail call to newlib-nano's one-byte-loop memcpy, so what it costs is
     * dominated by where the KEY BYTES live and where this code sits, not by
     * the work: with the key in flash it measured 1,359 cycles on CH592 and
     * up to ~5,100 on CH572 (per-access flash data penalties plus flash
     * instruction fetch), against ~120-800 with the key in SRAM. Production
     * keys come from the bond record in RAM, so the SRAM regime is the real
     * one. See validation/README.md, "the key-schedule numbers". */
    for (uint32_t i = 0; i < 4u; i++)
        aes_key_words[i] = load_le32(&key[4u * i]);
}

hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES])
{
    uint32_t mie;

    /* Mask interrupts across this ONE block (~15 us), output read included.
     * Root cause (OpenController docs/TODO.md section 0, bench 2026-08-15/16):
     * a BLEB preempt mid-operation silently aborts the engine -- CFG bit 0
     * reads back clear as if complete, but the computation never ran and the
     * DATA read path still holds the PREVIOUS block's output (separate in/out
     * latches; the mocked-register test sees the input instead, which is why
     * this failure mode stayed invisible to it). The linked BB_IRQLibHandler
     * reads/clears AES_STA, the register this sequence arms.
     *
     * mstatus.MIE gates WCH's fast-vectored (VTF/HPE) interrupts on QingKe
     * V4C -- WCH's own RTOS ports use csrrci mstatus,8 as their global
     * primitive. Mask per block, never across a whole CCM: ~15 us is 1.7% of
     * the 875 us poll grid, and pending IRQs are serviced between blocks.
     * Restore only if MIE was set on entry. Do NOT gate on AES_STA bit 1 as
     * completion evidence -- the engine never sets it outside the vendor IRQ
     * flow, and a bit-1 gate rejects every healthy block (fail-closed link
     * collapse, measured on the keyboard side). */
    AES_IRQ_SAVE(mie);

    AES_CFG = 0x100u; /* reset/prepare pulse, as the vendor driver does */
    __asm__ volatile("nop; nop; nop; nop");
    AES_CFG = 0u;     /* bit1 clear = encrypt */

    for (uint32_t i = 0; i < 4u; i++)
        AES_KEY(i) = aes_key_words[i];
    for (uint32_t i = 0; i < 4u; i++)
        AES_DATA(i) = load_le32(&in[4u * i]);

    AES_STA &= ~2u;
    AES_STA |= 1u;
    AES_CFG |= 1u; /* start */

    uint32_t spins = 0;
    while ((AES_CFG & 1u) != 0u && ++spins < AES_SPIN_LIMIT)
        ;

    /*
     * Distinguish completion from expiry. This previously fell straight into
     * the copy below in both cases, so a wedged engine returned the contents of
     * the data registers with no indication anything was wrong.
     *
     * Measured, not assumed: with the check removed, this returns THE PLAINTEXT
     * (tests/test_aes_ch592_mock.py reverts the branch and observes it). The
     * input is written into the data registers just above, so if the engine
     * never runs, reading them back yields exactly what was written. In counter
     * mode the caller XORs that "keystream" with the same plaintext and
     * transmits the result -- which is not encryption at all.
     *
     * Zero the output instead. A caller that ignores the status then transmits
     * plaintext, which is equally broken but obvious in a capture rather than
     * silently self-cancelling. See the contract note in hal_aes.h for why that
     * is the lesser evil and not a safe one.
     */
    if ((AES_CFG & 1u) != 0u) {
        AES_IRQ_RESTORE(mie);
        for (uint32_t i = 0; i < HAL_AES_BLOCK_BYTES; i++)
            out[i] = 0u;
        return HAL_AES_ENGINE_TIMEOUT;
    }

    for (uint32_t i = 0; i < 4u; i++)
        store_le32(&out[4u * i], AES_DATA(i));

    AES_IRQ_RESTORE(mie);

    return HAL_AES_OK;
}
