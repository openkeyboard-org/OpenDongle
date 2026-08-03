/*
 * OpenKeyboard.org OpenDongle -- CH570 AES HAL (software backend).
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Implements hal_aes.h with the portable software forward cipher, because the
 * CH570 has no hardware AES engine. That is measured, not assumed: the address
 * space at 0x4000C300 that carries a working engine on CH592 and CH572 is not
 * decoded on this part — writes to the plain key/data registers do not latch and
 * every word reads zero, while neighbouring registers in the same cluster are
 * live and writable. See hal_aes.h for the full statement of the evidence.
 *
 * Deliberately NOT __HIGH_CODE, and that is a measured decision rather than an
 * assumption. On CH5xx silicon at the production 100 MHz, running from flash:
 * key expansion 1638 cycles (16.4 us), one block 8072 cycles (80.7 us). The
 * 875 us poll slot is about 87,500 cycles, so a block is ~9.2% of a slot.
 * Relocating the cipher and its S-box into SRAM was measured too and buys only
 * 8072 -> 6843 cycles (~15%) for roughly 1.3 KB of a 12 KB SRAM budget. That is
 * a bad trade while the intended caller precomputes keystream in task context
 * and leaves only the XOR in the radio interrupt. Revisit only if a design ever
 * genuinely needs a block inside the hot path.
 */
#include "hal_aes.h"
#include "aes_sw.h"

static aes_sw_ctx_t aes_ctx;

void hal_aes_init(void)
{
    /* Nothing to bring up: no engine, no clock gate, no register block. The
     * entry point exists so the seam is identical on both chips. */
}

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    aes_sw_expand_key(&aes_ctx, key);
}

void hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                           uint8_t out[HAL_AES_BLOCK_BYTES])
{
    aes_sw_encrypt_block(&aes_ctx, in, out);
}
