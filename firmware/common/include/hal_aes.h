/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * AES-128 block-cipher HAL seam.
 *
 * One seam, two backends, identical output. CH592 has a hardware AES-128 engine
 * in its radio register cluster and uses it; CH570 does NOT have that engine and
 * uses a software forward cipher. Ciphertext is ciphertext, so an encrypted RF
 * mode built on this seam interoperates between the two chips bit-for-bit and
 * the wire format never has to know which backend produced a block.
 *
 * That asymmetry is a measured hardware fact, not a porting shortcut. On CH570
 * the address space at 0x4000C300 is not decoded at all: writes to the plain
 * key/data registers do not latch and every word reads zero, in every state
 * tested (reset, radio brought up, AES_CCM clock explicitly enabled, and with
 * the LLE CTRL_MOD enable bit set), while neighbouring registers in the same
 * cluster are live and writable. A CH572 on the same bench — same family, very
 * likely the same lot, and reporting the identical CTRL_MOD value in the same
 * reset state — passes the full known-answer battery from a cold reset with the
 * program counter still at zero. The block is absent from CH570 silicon; no
 * driver can recover it. WCH's own CH570 RF library agrees: it never references
 * 0x4000C300, while their CH592 non-BLE library does.
 *
 * CONTRACT (both implementations MUST honor this):
 *
 *  - AES-128 ONLY. The key is 16 bytes and the block is 16 bytes.
 *  - FORWARD CIPHER ONLY. There is deliberately no decrypt entry point. The
 *    intended construction is counter mode, which derives a keystream by
 *    ENCRYPTING a nonce block and XORing it with the payload — both directions
 *    of a CTR link use the forward cipher. Omitting decrypt keeps the CH570
 *    software backend to one S-box and no inverse MixColumns. If a mode that
 *    genuinely needs the inverse cipher is ever adopted, add it deliberately
 *    rather than assuming this seam already provides it.
 *  - KEY IS SCHEDULED SEPARATELY from encryption. hal_aes_set_key() is the
 *    expensive half on CH570 (it expands the 176-byte round-key schedule once)
 *    and near-free on CH592 (it just stores the key). Callers MUST set the key
 *    once per key change, NOT once per block, or the software backend pays the
 *    expansion on every block.
 *  - hal_aes_encrypt_block() is PURE with respect to the key: same key, same
 *    input, same output, with no cross-block chaining state. Counter mode is
 *    built by the caller.
 *  - IN-PLACE IS ALLOWED, and any overlap is safe, not just exact aliasing:
 *    both backends read all 16 input bytes before writing any output byte. The
 *    contract previously claimed only exact aliasing was permitted, which was
 *    needlessly strict and would have pushed callers into pointless copies.
 *  - COST, so a caller can budget honestly. On CH570 this depends on which
 *    backend is selected (see ch570/src/hal_aes_ch570_impl.h).
 *
 *    CH570 backends, at 100 MHz. The slot is the 875 us connected poll, so
 *    87,500 cycles:
 *
 *      backend         cycles/block   % slot   key schedule   % slot
 *      ASM_A (default)      1,944      2.2%         60,538    69.2%
 *      ASM_F                3,797      4.3%            n/m      n/m
 *      C (portable)        43,510     49.7%         47,759    54.6%
 *
 *    The hardware engines, measured on their own silicon and clock:
 *
 *      CH572 hardware, 100 MHz:  4,139 cycles/block,  41.4 us,  4.7% of slot
 *      CH592 hardware,  60 MHz:    885 cycles/block,  14.8 us,  1.7% of slot
 *
 *    Every figure above is a true core-cycle count measured by
 *    firmware/validation on production-faithful silicon -- same startup, same
 *    CORECFGR, same clock as ships -- and reproduced bit-identically across
 *    repeated runs. They are in-loop measurements and include call overhead, so
 *    they sit slightly above the kernel-only costs quoted in bench material.
 *
 *    NOTE THE KEY SCHEDULE COLUMN. It is new, and it is the number that
 *    actually constrains a caller on CH570: ASM_A's schedule is 31x its block
 *    cost and eats 69% of a poll slot on its own. Publishing only cycles/block
 *    made the backend look nearly free and hid that entirely. The schedule runs
 *    once per key, so this is a scheduling constraint rather than a throughput
 *    one -- but do not call hal_aes_set_key() inside a poll slot.
 *
 *    Three earlier figures in this file were wrong and are corrected here. The
 *    "~29,500 cycles" once quoted for the portable backend was taken with
 *    SysTick counting HCLK/8 AND under a bench harness that enabled the ROM
 *    loop buffer, while production boots with it disabled. The "2,700 cycles,
 *    CH592 hardware" row was measured on a CH572, not a CH592 -- the two differ
 *    by 4.7x -- and is replaced above by a measurement of each part.
 *
 *    ASM_F's 3,797 is NOT MEASURED here (n/m) and remains a bench figure. That
 *    backend cannot be built while CORECFGR bit 3 is clear, which production
 *    leaves clear; since it only ever runs with the ROM loop buffer enabled, a
 *    loop-buffer-enabled bench measurement is the right one for it, but nothing
 *    in this repository has re-confirmed it.
 *
 *    The intended construction still precomputes keystream in task context and
 *    leaves only the XOR in the radio interrupt. With the default backend a
 *    block is genuinely cheap, but that is a property of the chosen backend,
 *    not of the seam — do not design a caller that assumes it.
 *  - NOT REENTRANT. Both backends keep module state (the round-key schedule on
 *    CH570; a single shared hardware engine on CH592), so a call from interrupt
 *    context that preempts one in task context corrupts the result. Callers on
 *    the RF hot path should precompute keystream in task context and leave only
 *    the XOR in the IRQ-context receive path.
 *  - NOT CONSTANT-TIME. The software backend is table-driven and its timing
 *    depends on key and data. This is a considered trade: the threat model for
 *    this device is an attacker over the air, who cannot observe instruction
 *    timing. Do NOT reuse this seam anywhere a local attacker can measure it.
 */
#ifndef HAL_AES_H
#define HAL_AES_H

#include <stdint.h>

#define HAL_AES_BLOCK_BYTES 16u
#define HAL_AES_KEY_BYTES   16u

/* Bring the backend up. Safe to call more than once. CH592 points the engine at
 * its register block; CH570 has nothing to do. Call before hal_aes_set_key(). */
void hal_aes_init(void);

/* Install the 16-byte key. Expensive on CH570 (expands the round-key schedule),
 * trivial on CH592. Call once per key, not once per block. */
void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES]);

typedef enum {
    HAL_AES_OK = 0,             /* `out` holds the ciphertext */
    HAL_AES_ENGINE_TIMEOUT = 1  /* CH592 only: the engine did not complete */
} hal_aes_status_t;

/* Encrypt one 16-byte block under the key from the last hal_aes_set_key().
 * `out` may alias `in`. Behaviour is undefined if no key has been set.
 *
 * CHECK THE RETURN VALUE. It is not decoration, and this is a case where
 * ignoring it is worse than a normal unchecked error.
 *
 * The CH570 backends cannot fail and always return HAL_AES_OK. The CH592
 * hardware engine can wedge; if it does, this returns HAL_AES_ENGINE_TIMEOUT
 * and `out` is ZEROED rather than left holding whatever the engine's data
 * registers contained.
 *
 * The zeroing is deliberate and is the lesser of two bad outcomes, not a safe
 * one. In the intended counter-mode construction the output is a keystream
 * block. Without the check, a wedged engine returns THE PLAINTEXT — measured,
 * not assumed: the driver writes the input into the engine's data registers
 * before starting it, so if the engine never runs, reading them back yields
 * what was just written (tests/test_aes_ch592_mock.py demonstrates this by
 * reverting the branch). A caller would then XOR that "keystream" with the
 * same plaintext and transmit the result, which is not encryption at all.
 * Zeroing instead degrades to transmitting plaintext outright, which is
 * equally broken but loud: immediately visible in a capture rather than
 * silently self-cancelling.
 *
 * Neither is acceptable in shipping code. A caller that gets
 * HAL_AES_ENGINE_TIMEOUT must not transmit the block — treat it as a fatal
 * fault of the radio path, not a retryable condition. */
hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES]);

#endif /* HAL_AES_H */
