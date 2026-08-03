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

#define AES_BASE 0x4000C300u
#define AES_REG(off) (*(volatile uint32_t *)(uintptr_t)(AES_BASE + (off)))

#define AES_CFG     AES_REG(0x00) /* bit0 start/busy, bit1 0=encrypt 1=decrypt */
#define AES_STA     AES_REG(0x04) /* bit0 armed (software-owned), bit1 done */
#define AES_DATA(i) AES_REG(0x18u + 4u * (i)) /* 16 bytes in/out, in place */
#define AES_KEY(i)  AES_REG(0x28u + 4u * (i)) /* 16 bytes */

/* Generous bound so a wedged engine degrades to a wrong answer rather than a
 * hung dongle. A real operation completes in tens of cycles. */
#define AES_SPIN_LIMIT 100000u

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
     * the key is cached here and reloaded per block. That is a few stores. */
    for (uint32_t i = 0; i < 4u; i++)
        aes_key_words[i] = load_le32(&key[4u * i]);
}

void hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                           uint8_t out[HAL_AES_BLOCK_BYTES])
{
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

    for (uint32_t i = 0; i < 4u; i++)
        store_le32(&out[4u * i], AES_DATA(i));
}
