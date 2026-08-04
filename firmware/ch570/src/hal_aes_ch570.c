/*
 * OpenKeyboard.org OpenDongle -- CH570 AES HAL.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Implements hal_aes.h in software, because the CH570 has no hardware AES
 * engine. That is measured, not assumed: the address space at 0x4000C300 that
 * carries a working engine on CH592 and CH572 is not decoded on this part --
 * writes to the plain key/data registers do not latch and every word reads
 * zero, while neighbouring registers in the same cluster are live and writable.
 * See hal_aes.h for the full statement of the evidence.
 *
 * This file owns hal_aes_init() and hal_aes_set_key() for all three backends.
 * hal_aes_encrypt_block() comes from hal_aes_ch570_asm.S for the two assembly
 * backends, and from aes_sw.c for the portable one. The backend is chosen by
 * DONGLE_AES_CH570_IMPL -- see hal_aes_ch570_impl.h, which carries the measured
 * cost of each and the reason ASM_A is the default.
 *
 * WHY THE CIPHER IS IN SRAM NOW, having previously been deliberately left in
 * flash: the earlier decision rested on a figure of ~8,000 cycles/block that
 * was wrong twice over. SysTick was counting HCLK/8, so every measurement was
 * 8x low; and the bench harness ran CORECFGR = 0x0f with the ROM loop buffer
 * enabled, while production boots 0x25 with it clear. The true flash-resident
 * cost is 43,510 cycles -- 435 us, or 50% of the 875 us connected-poll slot,
 * not the ~9% previously recorded here. Relocating the cipher is worth 25x,
 * which changes the trade completely. See bench/aes_spike/CORE-FINDINGS.md.
 *
 * The key schedule stays in FLASH on purpose for every backend. It runs once
 * per key, so making it fast buys nothing, and keeping it out of SRAM is what
 * lets the cipher fit. Only hal_aes_encrypt_block() and the data it touches are
 * charged against the SRAM budget.
 */
#include "hal_aes.h"
#include "hal_aes_ch570_impl.h"

#if DONGLE_AES_CH570_IMPL == DONGLE_AES_CH570_IMPL_C
#include "aes_sw.h"
#endif

#include <stdint.h>

void hal_aes_init(void)
{
    /* Nothing to bring up: no engine, no clock gate, no register block. The
     * entry point exists so the seam is identical on both chips. */
}

#if DONGLE_AES_CH570_IMPL == DONGLE_AES_CH570_IMPL_ASM_A

/* Variant A keeps the state as four ROW words, so the schedule is stored
 * transposed -- rk[4r + row] -- and doing that once here is what makes
 * AddRoundKey four plain word loads inside the kernel. */
extern uint8_t  aes_a_sbox[256];   /* SRAM, defined in hal_aes_ch570_asm.S */
extern uint32_t aes_a_rk[44];      /* SRAM, defined in hal_aes_ch570_asm.S */

static uint32_t aes_a_sub4(uint32_t w)
{
    return (uint32_t)aes_a_sbox[w & 0xffu] |
           ((uint32_t)aes_a_sbox[(w >> 8) & 0xffu] << 8) |
           ((uint32_t)aes_a_sbox[(w >> 16) & 0xffu] << 16) |
           ((uint32_t)aes_a_sbox[(w >> 24) & 0xffu] << 24);
}

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    static const uint8_t rcon[10] = { 0x01u, 0x02u, 0x04u, 0x08u, 0x10u,
                                      0x20u, 0x40u, 0x80u, 0x1bu, 0x36u };
    uint32_t w[44];
    unsigned i, r, row;

    for (i = 0u; i < 4u; i++) {
        w[i] = (uint32_t)key[4u * i] |
               ((uint32_t)key[4u * i + 1u] << 8) |
               ((uint32_t)key[4u * i + 2u] << 16) |
               ((uint32_t)key[4u * i + 3u] << 24);
    }
    for (i = 4u; i < 44u; i++) {
        uint32_t t = w[i - 1u];
        if ((i & 3u) == 0u) {
            t = aes_a_sub4((t >> 8) | (t << 24)) ^ (uint32_t)rcon[(i >> 2) - 1u];
        }
        w[i] = w[i - 4u] ^ t;
    }
    for (r = 0u; r < 11u; r++) {
        for (row = 0u; row < 4u; row++) {
            aes_a_rk[4u * r + row] =
                ((w[4u * r + 0u] >> (8u * row)) & 0xffu) |
                (((w[4u * r + 1u] >> (8u * row)) & 0xffu) << 8) |
                (((w[4u * r + 2u] >> (8u * row)) & 0xffu) << 16) |
                (((w[4u * r + 3u] >> (8u * row)) & 0xffu) << 24);
        }
    }
}

#elif DONGLE_AES_CH570_IMPL == DONGLE_AES_CH570_IMPL_ASM_F

/* Variant F reads the schedule as plain bytes: round r at bytes 16r..16r+15,
 * column c of round r as the little-endian word at 16r + 4c. That matches
 * FIPS-197's w[] exactly when w[i] is stored least-significant-byte-first,
 * which is the natural in-memory order of the key bytes, so nothing is
 * byte-swapped anywhere. */
extern const uint8_t aes_f_sbox[256];  /* SRAM, defined in hal_aes_ch570_asm.S */
extern uint8_t       aes_f_rk[176];    /* SRAM, defined in hal_aes_ch570_asm.S */

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    uint8_t rcon = 1u;
    int i;

    for (i = 0; i < 16; i++) {
        aes_f_rk[i] = key[i];
    }
    for (i = 16; i < 176; i += 4) {
        uint8_t a = aes_f_rk[i - 4];
        uint8_t b = aes_f_rk[i - 3];
        uint8_t c = aes_f_rk[i - 2];
        uint8_t d = aes_f_rk[i - 1];

        if ((i & 15) == 0) {
            uint8_t t = a;
            /* RotWord, then SubWord, then ^ Rcon */
            a = (uint8_t)(aes_f_sbox[b] ^ rcon);
            b = aes_f_sbox[c];
            c = aes_f_sbox[d];
            d = aes_f_sbox[t];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80u) ? 0x1bu : 0u));
        }
        aes_f_rk[i + 0] = (uint8_t)(aes_f_rk[i - 16] ^ a);
        aes_f_rk[i + 1] = (uint8_t)(aes_f_rk[i - 15] ^ b);
        aes_f_rk[i + 2] = (uint8_t)(aes_f_rk[i - 14] ^ c);
        aes_f_rk[i + 3] = (uint8_t)(aes_f_rk[i - 13] ^ d);
    }
}

#else /* DONGLE_AES_CH570_IMPL_C -- portable fallback */

static aes_sw_ctx_t aes_ctx;

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    aes_sw_expand_key(&aes_ctx, key);
}

hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES])
{
    /* A software cipher has no failure mode: no engine to wedge, no timeout.
     * Always HAL_AES_OK, so a caller written against the seam behaves
     * identically on both chips. Only the CH592 hardware backend can fail. */
    aes_sw_encrypt_block(&aes_ctx, in, out);
    return HAL_AES_OK;
}

#endif /* DONGLE_AES_CH570_IMPL */
