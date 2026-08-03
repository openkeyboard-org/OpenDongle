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

/* Encrypt one 16-byte block under the key from the last hal_aes_set_key().
 * `out` may alias `in`. Behaviour is undefined if no key has been set. */
void hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                           uint8_t out[HAL_AES_BLOCK_BYTES]);

#endif /* HAL_AES_H */
