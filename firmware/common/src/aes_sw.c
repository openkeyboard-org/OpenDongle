/*
 * OpenKeyboard.org OpenDongle -- software AES-128 forward cipher.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Encrypt-only AES-128 per FIPS-197, used by the CH570 hal_aes backend because
 * that silicon has no hardware AES engine (see hal_aes.h for the measurement).
 *
 * READ THIS BEFORE COMPARING ANY NUMBER BELOW. Every bench figure in this file
 * is a SysTick TICK, and on this harness SysTick runs at HCLK/8, so one tick is
 * EIGHT core cycles. ch32fun writes SysTick->CTLR = 1 unless
 * FUNCONF_SYSTICK_USE_HCLK is defined, and the register reads 0x00000001 on
 * silicon. Proof the divisor is real: a chain of 1024 *dependent* addi -- which
 * a single-issue 3-stage core cannot execute faster than 1 cycle each --
 * measures 0.127 ticks/instruction from SRAM, i.e. 1.02 core cycles. Exactly
 * the floor. Ratios are unaffected; absolute costs are 8x these numbers.
 *
 * WHAT IT ACTUALLY COSTS, CH570 at 100 MHz, one AES-128 block, against the
 * 875 us connected-poll slot:
 *
 *   THIS, flash-resident             30721 cycles   307 us    35% of the slot
 *   THIS, if relocated into SRAM      1680 cycles    17 us   1.9% of the slot
 *
 * The flash-resident row is measured by firmware/validation on production-
 * faithful silicon: WCH GCC 15.2, -march=rv32imc_zba_zbb_zbc_zbs_xw, 100 MHz,
 * CORECFGR = 0x2D with the ROM loop buffer ON (the production value since the
 * 0x25 -> 0x2D flip; the same silicon measured 43,396 at 0x25). It replaces a
 * figure of 29,544 that was taken with SysTick counting HCLK/8, which nothing
 * production does. The SRAM row is still a bench estimate and has NOT been
 * re-measured; treat the ratio as indicative rather than as a result.
 *
 * A THIRD of the poll slot is not free. The SRAM figure is the same code, and
 * the gap is instruction fetch: 16.4 core cycles per instruction from flash
 * versus 1.02 from SRAM. If AES ever lands on the hot path, relocating it is
 * worth more (~17x) than every source-level optimisation here combined -- the
 * obstacle is that the hot path is ~2.3 KB against ~3 KB of free SRAM.
 *
 * WHY THIS SHAPE, in TICKS (multiply by 8 for core cycles), production
 * toolchain and ISA (MounRiver GCC 15.2, -march=rv32imc_zba_zbb_zbc_zbs_xw);
 * xPack-compiled numbers differ 5-8% and are not what ships:
 *
 *   byte-oriented, S-box in flash          7534   <- what this replaces
 *   THIS: row-major SWAR, S-box in RAM     3901   (1.93x, unrolled)
 *
 * and, measured with xPack so comparable only among themselves:
 *
 *   byte-oriented, S-box in flash          8070
 *   T-table (1 KB), tables in flash        5977
 *   row-major SWAR, S-box in RAM           4413
 *   T-table (1 KB), tables in RAM          3664   (costs 1280 B of 12 KB RAM)
 *
 * The dominant cost on this part is not instruction count, it is *flash data
 * reads*, and there are 160 table lookups per block. Measured directly with a
 * dependent pointer-chase over byte-identical 1 KB tables (same machine code,
 * only the base pointer differing): a flash read costs **~9 cycles more than a
 * RAM read** -- 9.4 vs 0.75 TICKS per dependent load (75 vs 6 core cycles). Running the same chase
 * from SRAM instead of flash changed it by under 4%, so instruction/data
 * contention is not a factor; prefetch already hides instruction fetch.
 *
 * (An earlier estimate of ~14 cycles was inferred from the AES-level delta
 * between a T-table in flash and the same table in RAM. That overstated it:
 * the delta also contained 40 round-key loads per block and placement-induced
 * codegen differences. The direct measurement above is the one to trust.)
 *
 * That is why the obvious answer -- a bigger table -- loses to this one. A
 * 1 KB table in flash pays the flash penalty on every lookup; this keeps only
 * the 256-byte S-box and puts it in RAM.
 *
 * The linear layer then costs no table at all:
 *   - state is four 32-bit words, one per ROW, held in registers throughout;
 *   - ShiftRows is one rotate per row -- GCC emits Zbb `rori` from the plain C
 *     idiom, which the previous byte-oriented form could never reach;
 *   - MixColumns does all four columns at once with a SWAR xtime, four GF(2^8)
 *     doublings per instruction group and no lookup.
 *
 * On the ISA extensions, all measured on the production compiler:
 *   - WCH's `xw` (four compressed byte/half load-stores) was worth 14.8% to the
 *     old byte-oriented code (8839 -> 7534), which was 40% lbu/sb, and is worth
 *     0.4% here (4692 -> 4674) now that the state lives in registers. It is
 *     emitted automatically with no intrinsics; nothing further to exploit.
 *   - Zba/Zbb are worth *exactly zero* to byte-at-a-time code (the old version
 *     compiled byte-identically with and without them), and their value here is
 *     a property of the COMPILER, not the silicon. On GCC 12.2 they were a 7.6%
 *     pessimisation (4674 with, 4344 without); on the now-pinned GCC 15.2 the
 *     sign flips and they HELP by 8.9% (3693 with, 4053 without). Deliberately
 *     never worked around with a per-file -march -- and that restraint paid off
 *     exactly as argued: the workaround would have been silently inverted by
 *     the pin move rather than merely made redundant.
 *
 * TOOLCHAIN NOTE, measured on silicon with this exact source:
 *
 *   GCC 12.2            3901 ticks/block, 3304 B
 *   GCC 15.2 (pinned)   3693 ticks/block, 3036 B   (-5.3% time, -8% size)
 *
 * The pin has since MOVED to GCC 15.2 (Toolchain/RISC-V Embedded GCC15, prefix
 * riscv32-wch-elf, accepts the full production -march including _xw), so this
 * source now gets that 5% and 270 B, and the bitmanip extensions are a win
 * rather than the pessimisation they were under 12.2. This note previously read
 * the other way round -- 12.2 pinned, the bump "recorded here, not taken" --
 * which the pin move made exactly wrong.
 *
 * There is no inverse cipher on purpose: counter mode encrypts in both
 * directions, so InvMixColumns and the inverse S-box would be dead weight.
 *
 * The S-box placement is load-bearing, not incidental, and it needs the
 * explicit section attribute below: merely dropping `const` is NOT enough,
 * because GCC proves the table is never written and promotes it back into
 * .rodata (flash) — silently undoing the optimisation and costing ~2240 ticks (~18k core cycles)
 * with nothing in the build output to say so. Both chips' linker scripts
 * collect `*(.data .data.*)` into RAM with a flash LMA, so this is portable
 * across the two targets. If you move this table, re-measure.
 */
#include "aes_sw.h"

/*
 * Rotate right. GCC folds this to a single Zbb `rori` on the production -march.
 *
 * Deliberately a macro, and deliberately guarded. The plain shift form is
 * undefined for n == 0 (it would shift a uint32_t by 32) and for n >= 32.
 * Neither can happen at today's call sites, which pass only the literals 8, 16
 * and 24 -- but "no caller does that yet" is not a safety property, so the
 * sizeof guard below turns an out-of-range rotate into a COMPILE error instead
 * of silent UB. It costs nothing at runtime and, because it needs a
 * constant-foldable n, a caller passing a computed rotate fails to build rather
 * than quietly misbehaving.
 *
 * Why not an inline function, which would also fix the double evaluation of x:
 * measured, it costs 3.5% (4118 vs 3980 ticks/block) even with
 * always_inline, because GCC schedules the macro expansion better inside the
 * unrolled round. The double evaluation is safe here only because every call
 * site passes a plain local -- keep it that way.
 */
#define ROR32(x, n)                                                        \
    ((void)sizeof(char[1 - 2 * !((n) >= 1u && (n) <= 31u)]),               \
     ((x) >> (n)) | ((x) << (32u - (n))))

__attribute__((section(".data"))) static uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint32_t sub4(uint32_t w)
{
    return (uint32_t)sbox[w & 0xff] | ((uint32_t)sbox[(w >> 8) & 0xff] << 8) |
           ((uint32_t)sbox[(w >> 16) & 0xff] << 16) |
           ((uint32_t)sbox[(w >> 24) & 0xff] << 24);
}

/*
 * Four GF(2^8) doublings at once. The alternative formulation
 * `((x << 1) & 0xfefefefe) ^ (((x >> 7) & 0x01010101) * 0x1b)` was measured and
 * is a wash -- 3980 ticks either way, same code size, because GCC folds both
 * to equivalent instructions. Do not re-litigate it.
 */
static uint32_t xt4(uint32_t x)
{
    return ((x & 0x7f7f7f7fu) << 1) ^ (((x & 0x80808080u) >> 7) * 0x1bu);
}

/* schedule stored transposed into row words: rk[4r+row] */
void aes_sw_expand_key(aes_sw_ctx_t *ctx, const uint8_t key[16])
{
    uint32_t w[44];
    for (unsigned i = 0; i < 4; i++)
        w[i] = (uint32_t)key[4*i] | ((uint32_t)key[4*i+1] << 8) |
               ((uint32_t)key[4*i+2] << 16) | ((uint32_t)key[4*i+3] << 24);
    for (unsigned i = 4; i < 44; i++) {
        uint32_t t = w[i-1];
        if ((i & 3u) == 0u)
            t = sub4(ROR32(t, 8)) ^ (uint32_t)rcon[(i >> 2) - 1];
        w[i] = w[i-4] ^ t;
    }
    /* transpose each 4-word round key into 4 row words */
    for (unsigned r = 0; r < 11; r++)
        for (unsigned row = 0; row < 4; row++)
            ctx->rk[4*r + row] =
                ((w[4*r+0] >> (8*row)) & 0xff) |
                (((w[4*r+1] >> (8*row)) & 0xff) << 8) |
                (((w[4*r+2] >> (8*row)) & 0xff) << 16) |
                (((w[4*r+3] >> (8*row)) & 0xff) << 24);
}

void aes_sw_encrypt_block(const aes_sw_ctx_t *ctx, const uint8_t in[16],
                          uint8_t out[16])
{
    const uint32_t *rk = ctx->rk;
    uint32_t r0, r1, r2, r3;

    r0 = (uint32_t)in[0] | ((uint32_t)in[4] << 8) | ((uint32_t)in[8] << 16) | ((uint32_t)in[12] << 24);
    r1 = (uint32_t)in[1] | ((uint32_t)in[5] << 8) | ((uint32_t)in[9] << 16) | ((uint32_t)in[13] << 24);
    r2 = (uint32_t)in[2] | ((uint32_t)in[6] << 8) | ((uint32_t)in[10] << 16) | ((uint32_t)in[14] << 24);
    r3 = (uint32_t)in[3] | ((uint32_t)in[7] << 8) | ((uint32_t)in[11] << 16) | ((uint32_t)in[15] << 24);
    r0 ^= rk[0]; r1 ^= rk[1]; r2 ^= rk[2]; r3 ^= rk[3];

    /*
     * Unrolled deliberately: measured 4674 -> 3980 ticks (15%) on CH570 for
     * +2132 B of flash, which is cheap here. This contradicts the published
     * guidance -- Adomnicai measured unrolling as 2.9% *slower* on a SiFive
     * E31 -- and that result simply does not transfer to a 3-stage QingKe
     * fetching from flash, where each taken back-edge restarts the prefetcher.
     * Note `-funroll-loops` globally is worse than this targeted pragma (4499
     * ticks, and it wrecks the key schedule); unroll this loop and no other.
     */
    _Pragma("GCC unroll 9")
    for (unsigned i = 1; i < 10; i++) {
        uint32_t t, n0, n1, n2, n3;
        /*
         * Rotate the INPUT of sub4, not its output. The S-box is applied
         * byte-wise so the two commute, but this form measured 79 cycles
         * faster and 120 B smaller. Note the near miss: specialising sub4 into
         * four rotate-folded variants to remove the rotate entirely is 37%
         * SLOWER (5435), because one shared sub4 lets GCC share work that four
         * expansions cannot. Same transformation, opposite result -- measure
         * any change here, do not reason about it.
         */
        r0 = sub4(r0); r1 = sub4(ROR32(r1, 8));
        r2 = sub4(ROR32(r2, 16)); r3 = sub4(ROR32(r3, 24));
        t = r0 ^ r1 ^ r2 ^ r3;
        n0 = r0 ^ t ^ xt4(r0 ^ r1);
        n1 = r1 ^ t ^ xt4(r1 ^ r2);
        n2 = r2 ^ t ^ xt4(r2 ^ r3);
        n3 = r3 ^ t ^ xt4(r3 ^ r0);
        r0 = n0 ^ rk[4*i+0]; r1 = n1 ^ rk[4*i+1];
        r2 = n2 ^ rk[4*i+2]; r3 = n3 ^ rk[4*i+3];
    }
    r0 = sub4(r0); r1 = sub4(ROR32(r1, 8));
    r2 = sub4(ROR32(r2, 16)); r3 = sub4(ROR32(r3, 24));
    r0 ^= rk[40]; r1 ^= rk[41]; r2 ^= rk[42]; r3 ^= rk[43];

    out[0]=(uint8_t)r0;  out[4]=(uint8_t)(r0>>8);  out[8]=(uint8_t)(r0>>16);  out[12]=(uint8_t)(r0>>24);
    out[1]=(uint8_t)r1;  out[5]=(uint8_t)(r1>>8);  out[9]=(uint8_t)(r1>>16);  out[13]=(uint8_t)(r1>>24);
    out[2]=(uint8_t)r2;  out[6]=(uint8_t)(r2>>8);  out[10]=(uint8_t)(r2>>16); out[14]=(uint8_t)(r2>>24);
    out[3]=(uint8_t)r3;  out[7]=(uint8_t)(r3>>8);  out[11]=(uint8_t)(r3>>16); out[15]=(uint8_t)(r3>>24);
}
