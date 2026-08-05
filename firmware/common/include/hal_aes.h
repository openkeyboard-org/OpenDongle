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
 *    CH570 backends, at 100 MHz, with the production CORECFGR value 0x2D
 *    (ROM loop buffer ON -- see ch570/src/ch570_corecfgr.h). The slot is the
 *    875 us connected poll, so 87,500 cycles:
 *
 *      backend         cycles/block   % slot   key schedule   % slot
 *      ASM_A (default)      1,672      1.9%          7,647     8.7%
 *      ASM_F                3,992      4.6%          7,405     8.5%
 *      C (portable)        30,721       35%         16,543    18.9%
 *
 *    Key schedules are timed with the key in SRAM -- the production regime
 *    (keys come from the bond record in RAM). Earlier figures used the
 *    flash-resident KAT key, which inflated them 10-15% here and up to 5x on
 *    the hardware engines; see validation/README.md, "the key-schedule
 *    numbers", for the anatomy.
 *
 *    The hardware engines, measured on their own silicon and clock:
 *
 *      CH572 hardware, 100 MHz:  2,966 cycles/block,  29.7 us,  3.4% of slot
 *      CH592 hardware,  60 MHz:    865 cycles/block,  14.4 us,  1.6% of slot
 *      (key schedule: 322 cycles on CH572, 838 on CH592, SRAM-key regime)
 *
 *    Every figure is a true core-cycle count measured by firmware/validation on
 *    silicon under the production startup, and reproduced to the cycle across
 *    independent flash-and-run sweeps (key schedules within +/-2). They are
 *    in-loop measurements and include call overhead, so they sit slightly above
 *    kernel-only costs quoted in bench material. The V3C rows measure
 *    identically on CH570 and CH572 -- same core, same buffer -- which is what
 *    lets either part stand in for the other on the bench.
 *
 *    THE SOFTWARE KERNEL BEATS THE HARDWARE ENGINE on the part that has one:
 *    ASM_A 1,672 against the engine's 2,966 on the same CH572, 1.8x. The
 *    engine core is not slow; its driver reloads the key and shuffles four
 *    data words each way on EVERY block (the engine keeps no key across its
 *    reset pulse). The key schedule leans the other way -- 7,647 against
 *    322 -- so the engine wins below ~6 blocks per key and the software
 *    kernel above; a CTR link re-keying per session is far above that.
 *
 *    THE KEY SCHEDULE COLUMN is the number that used to constrain callers
 *    hard: at the old CORECFGR 0x25 it was 59,113 cycles -- 68% of a poll slot
 *    -- and the seam had to forbid re-keying inside one. At 0x2D it is 89 us,
 *    10.2%. Re-keying outside the poll grid remains the right design (it is
 *    still 5.3x a block), but it is no longer prohibitive if a future path
 *    needs it mid-session.
 *
 *    HISTORY, kept so nobody resurrects a stale number. At CORECFGR 0x25
 *    (loop buffer off) the same silicon measured: ASM_A 1,944 / 59,113,
 *    C 43,396 / 45,378, CH572 engine 4,011 / 5,126; ASM_F cannot build there
 *    (its selector #errors, deliberately). Those 0x25 figures moved ~3% with
 *    unrelated link shifts because flash fetch dominated them; at 0x2D the
 *    sweep-to-sweep spread observed so far is zero. Two figures that predate
 *    the validation suite were wrong and stay dead: "~29,500" for portable C
 *    (SysTick counting HCLK/8 under a bench harness) and "2,700, CH592
 *    hardware" (actually measured on a CH572).
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
 *  - TREAT AS NOT CONSTANT-TIME. Do NOT reuse this seam anywhere a local
 *    attacker can measure it. That rule is unchanged by the measurements
 *    below, which are stated so this note is accurate rather than merely
 *    cautious -- an earlier version claimed the timing "depends on key and
 *    data", which the bench falsified, and a false claim invites someone to
 *    falsify it and then discard the caution with it.
 *
 *    What is actually known, per backend:
 *      - ASM_A keeps its S-box and schedule in SRAM, where loads cost a fixed
 *        2 cycles on this core regardless of address, and the kernel has no
 *        data-dependent branches -- so it is plausibly constant-time BY
 *        CONSTRUCTION on this silicon, and 512 random key/plaintext blocks
 *        measured zero cycle spread. That is one backend, one compiler, one
 *        configuration; it is corroboration, not a verified property.
 *      - The portable C backend's tables live in flash, whose read cost is
 *        measurably address- and alignment-sensitive on this part, and its
 *        codegen is the compiler's to change -- assume data-dependent timing.
 *      - The hardware engines are opaque; nothing is known about their
 *        internal timing either way.
 *
 *    The considered trade stands: the threat model for this device is an
 *    attacker over the air, who cannot observe instruction timing (keystream
 *    is precomputed in task context, so cipher timing does not gate radio
 *    responses). Anything more hostile than that needs its own analysis, not
 *    this note.
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
