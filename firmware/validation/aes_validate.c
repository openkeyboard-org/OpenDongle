/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon validation of the hal_aes.h seam.
 *
 * ONE test source, linked against ANY backend. Nothing below names a backend:
 * it calls hal_aes.h and nothing else, so the CH570 builds (two assembly
 * kernels and the portable C cipher) and the CH592/CH572 builds (the hardware
 * engine) all run byte-identical code. That is what makes the cross-arm
 * comparison meaningful -- every arm must produce the same ciphertext, which is
 * exactly the property an encrypted CH570<->CH592 link depends on.
 *
 * These are the REAL firmware sources, not copies. What is measured is what
 * ships.
 *
 * RESULTS COME BACK THROUGH RAM, NOT A UART. printf-style output blocks at boot
 * when no debug terminal is attached, which silently stalls the probe -- a
 * failure mode that looks exactly like a hung cipher. The harness instead fills
 * `aes_log[]` and spins; the runner halts the core and reads the array out.
 * See aes_log_format.h for the record format and read_aes_log.py for the reader.
 */
#include "aes_log_format.h"
#include "hal_aes.h"

#if DONGLE_VALIDATE_HAS_CORECFGR
#include "ch570_corecfgr.h"
#endif

#include <stdint.h>

/*
 * Per-chip bring-up: set the system clock and anything else the part needs
 * before the cipher runs. Supplied by validate_platform_<chip>.c so this file
 * stays chip-agnostic.
 */
extern void aes_validate_platform_init(void);

/* --------------------------------------------------------- retained diagnosis */

/*
 * The retained section only means anything on a device, where it survives a
 * warm reset because the link script marks it NOLOAD and places it above
 * _ebss. A host build has no reset to survive and no such section in its
 * linker script -- naming one there lands the arrays in a section the host
 * linker does not make writable, and the first store segfaults -- so hosted
 * builds use ordinary .bss.
 */
#ifdef DONGLE_VALIDATE_HOSTED
#define VALIDATE_RETAINED
#else
#define VALIDATE_RETAINED __attribute__((section(".validate_keep")))
#endif

/*
 * Survives a warm reset (see .validate_keep in the link script). It exists to
 * answer one question the log cannot: when a run stops partway, did the part
 * HANG, or did it RESET and start over? Both leave an identical truncated log,
 * because a reset re-runs from the top and rewrites it.
 *
 *   [0] magic           - AES_LOG_KEEP_MAGIC once initialised
 *   [1] boot count      - >1 means the image restarted on its own
 *   [2] R8_RESET_STATUS - cause of the most recent reset
 *   [3] R8_RST_WDOG_CTRL and R8_WDOG_COUNT, packed
 *   [4] furthest stage reached, cumulative across boots
 */
VALIDATE_RETAINED volatile uint32_t validate_keep[8];

#define VK_MAGIC   0
#define VK_BOOTS   1
#define VK_RESET   2
#define VK_WDOG    3
#define VK_STAGE   4

/*
 * A snapshot of the last COMPLETED log, in retained memory.
 *
 * `aes_log` lives in .bss and is therefore cleared on every reset. On CH570
 * the part was observed rebooting repeatedly while the debug probe was
 * attached, so the live log almost never showed a finished run: a later boot
 * wiped the completed record and got partway through a fresh one before the
 * host could halt and read it. The stage counter proved the harness itself was
 * reaching the end every time.
 *
 * So the finished log is copied here, where a reset cannot reach it, and the
 * magic is written LAST so a copy interrupted partway is never mistaken for a
 * valid one.
 */
/* Defined below, in the logging section; declared here for vkeep_save(). */
extern volatile uint32_t aes_log[AES_LOG_WORDS];

VALIDATE_RETAINED volatile uint32_t validate_saved[AES_LOG_WORDS + 4];

static void vkeep_save(uint32_t n)
{
    validate_saved[0] = 0u;              /* invalidate while copying */
    validate_saved[1] = n;
    /*
     * Which boot produced this snapshot. Retained memory survives reflashing,
     * so without this a snapshot left by a PREVIOUS run of the SAME build --
     * same build id, so the identity check passes -- would be accepted as this
     * run's result. Re-run a build that passed, have it fail the second time,
     * and the rig would report the first run's PASS. The host records the boot
     * count before flashing and refuses any snapshot not taken after it.
     */
    validate_saved[2] = validate_keep[VK_BOOTS];
    /*
     * Which EXECUTION produced this snapshot. The runner writes a fresh random
     * word into flash at AES_LOG_NONCE_ADDR each run, after the image; echoing
     * it here is what lets the host tell this run's snapshot from one left by
     * any earlier run of the same build in surviving retained RAM. Hosted
     * builds have no flash to read and no runner to satisfy.
     */
#ifdef DONGLE_VALIDATE_HOSTED
    validate_saved[3] = 0u;
#else
    validate_saved[3] = *(volatile const uint32_t *)(uintptr_t)AES_LOG_NONCE_ADDR;
#endif
    for (uint32_t i = 0; i < n && i < AES_LOG_WORDS; i++)
        validate_saved[4u + i] = aes_log[i];
    validate_saved[0] = AES_LOG_SAVED_MAGIC;     /* publish only once complete */
}

static void vkeep_boot(void)
{
    if (validate_keep[VK_MAGIC] != AES_LOG_KEEP_MAGIC) {
        /* First boot after a cold start: RAM is undefined, so initialise. */
        validate_keep[VK_MAGIC] = AES_LOG_KEEP_MAGIC;
        validate_keep[VK_BOOTS] = 0u;
        validate_keep[VK_STAGE] = 0u;
    }
    validate_keep[VK_BOOTS] += 1u;
#ifndef DONGLE_VALIDATE_HOSTED
    /* R8_RESET_STATUS, R8_RST_WDOG_CTRL, R8_WDOG_COUNT. Device-only: these are
     * MMIO addresses, and dereferencing them in a host build segfaults. */
    validate_keep[VK_RESET] = *(volatile uint8_t *)(uintptr_t)0x40001044u;
    validate_keep[VK_WDOG] =
        (uint32_t)(*(volatile uint8_t *)(uintptr_t)0x40001046u)
        | ((uint32_t)(*(volatile uint8_t *)(uintptr_t)0x40001043u) << 8);
#endif
}

static void vkeep_stage(uint32_t s)
{
    if (s > validate_keep[VK_STAGE])
        validate_keep[VK_STAGE] = s;
}

/* ------------------------------------------------------------------ logging */

/*
 * Exported, and deliberately NOT at a hard-coded address. The bench original
 * wrote to a fixed 0x20001000, which is correct only for as long as nothing
 * else in the image grows into it -- and nothing checks that, so the day it
 * breaks the symptom is a corrupted log rather than an error. A symbol cannot
 * collide with the linker's own allocation.
 *
 * volatile because the only consumer is a debug probe reading RAM; nothing in
 * this program ever reads it back, and the compiler is entitled to assume that.
 */
volatile uint32_t aes_log[AES_LOG_WORDS];

static uint32_t log_n;
static uint32_t log_overflowed;

/* One word is always held in reserve so the terminator can be written even
 * when the body filled the buffer. A truncated log that says it is truncated
 * beats one that silently loses its own ending. */
static void put(uint32_t v)
{
    /* One word held in reserve for the terminator, and the last four for the
     * fatal-fault record, which the body must never tread on. */
    if (log_n < (AES_LOG_FAULT_SLOT - 1u))
        aes_log[log_n++] = v;
    else
        log_overflowed = 1u;
}

static void put_block(const uint8_t *b)
{
    for (uint32_t i = 0; i < 4u; i++)
        put((uint32_t)b[4u * i] | ((uint32_t)b[4u * i + 1u] << 8) |
            ((uint32_t)b[4u * i + 2u] << 16) | ((uint32_t)b[4u * i + 3u] << 24));
}

/* ------------------------------------------------------------ cycle counter */

/*
 * SysTick, driven directly rather than through a HAL, because the HAL is a
 * timer abstraction and this needs a free-running cycle count.
 *
 * STCLK IS THE WHOLE BALLGAME. Clear, SysTick counts HCLK/8 and every figure
 * this harness reports is 8x low; set, it counts core cycles. An entire
 * measurement campaign was invalidated by exactly this bit, and the resulting
 * numbers were plausible enough to be published in a header before anyone
 * noticed. Do not "simplify" the initialisation below.
 *
 * The register block is identical on V3C (CH570/CH572) and V4C (CH592) for the
 * first three words; CH592's CNT is 64-bit but its low half sits at the same
 * offset, so a 32-bit read is correct on both.
 */
/*
 * Overridable for the same reason AES_BASE is: it lets the host suite run this
 * exact harness against ordinary memory, so the log format, the differential
 * and the reader can all be exercised end-to-end without a device
 * (tests/test_aes_validate_harness.py). Nothing but a test should define it.
 */
#ifndef STK_BASE
#define STK_BASE        0xE000F000u
#endif
#define STK_CTLR        (*(volatile uint32_t *)(uintptr_t)(STK_BASE + 0x00u))
#define STK_SR          (*(volatile uint32_t *)(uintptr_t)(STK_BASE + 0x04u))
#define STK_CNTL        (*(volatile uint32_t *)(uintptr_t)(STK_BASE + 0x08u))
#define STK_CMPL        (*(volatile uint32_t *)(uintptr_t)(STK_BASE + 0x10u))
#define STK_CMPH        (*(volatile uint32_t *)(uintptr_t)(STK_BASE + 0x14u))
#define STK_CTLR_STE    (1u << 0) /* counter enable */
#define STK_CTLR_STCLK  (1u << 2) /* 1 = HCLK (core cycles), 0 = HCLK/8 */
#define STK_CTLR_MODE   (1u << 4) /* 0 = count up */
#define STK_CTLR_INIT   (1u << 5) /* V4C only: load the counter on enable */

/*
 * CMP must be pushed out of the way before the counter is enabled.
 *
 * Leaving it at its reset value of 0 means the counter reaches its compare
 * target immediately and repeatedly, and what CNT then reads is not a
 * monotonic cycle count. That produced a plainly impossible first measurement
 * on CH592 -- 1 cycle of overhead for two volatile MMIO reads, and 1,359
 * cycles to cache a 16-byte key -- which is the only reason it was caught.
 * The figures LOOKED like ordinary numbers; only knowing what the code does
 * made them absurd.
 *
 * CH592's V4C SysTick also has an INIT bit that the V3C parts do not, and
 * WCH's own CH592 SysTick_Config sets it. Its CNT/CMP are 64-bit, so the
 * high compare word needs pushing out too; on V3C that offset is reserved and
 * is left alone.
 */
static void cycles_init(void)
{
    STK_CTLR = 0u;
    STK_SR = 0u;
    STK_CMPL = 0xFFFFFFFFu;
#if DONGLE_VALIDATE_SYSTICK_64
    STK_CMPH = 0xFFFFFFFFu;
#endif
    STK_CNTL = 0u;
#if DONGLE_VALIDATE_SYSTICK_64
    STK_CTLR = STK_CTLR_INIT | STK_CTLR_STE | STK_CTLR_STCLK;
#else
    STK_CTLR = STK_CTLR_STE | STK_CTLR_STCLK;
#endif
}

static uint32_t cycles(void)
{
    return STK_CNTL;
}

/* Keep the optimiser from moving work across a timing boundary. */
#define BARRIER() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ vectors */

static const uint8_t k_fips[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                   0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
                                   0x0e, 0x0f};
static const uint8_t p_fips[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                   0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
                                   0xee, 0xff};
static const uint8_t c_fips[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04,
                                   0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4,
                                   0xc5, 0x5a};

static const uint8_t k_zero[16] = {0};
static const uint8_t c_zero[16] = {0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c,
                                   0x3b, 0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34,
                                   0x2b, 0x2e};

/* NIST SP 800-38A F.1.1 ECB-AES128, one key and four plaintext blocks. */
static const uint8_t k_nist[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2,
                                   0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf,
                                   0x4f, 0x3c};
static const uint8_t p_nist[4][16] = {
    {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
     0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a},
    {0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
     0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51},
    {0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
     0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef},
    {0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
     0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10},
};
static const uint8_t c_nist[4][16] = {
    {0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
     0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97},
    {0xf5, 0xd3, 0xd5, 0x85, 0x03, 0xb9, 0x69, 0x9d,
     0xe7, 0x85, 0x89, 0x5a, 0x96, 0xfd, 0xba, 0xaf},
    {0x43, 0xb1, 0xcd, 0x7f, 0x59, 0x8e, 0xce, 0x23,
     0x88, 0x1b, 0x00, 0xe3, 0xed, 0x03, 0x06, 0x88},
    {0x7b, 0x0c, 0x78, 0x5e, 0x27, 0xe8, 0xad, 0x3f,
     0x82, 0x23, 0x20, 0x71, 0x04, 0x72, 0x5d, 0xd4},
};

static uint32_t same16(const uint8_t *a, const uint8_t *b)
{
    uint32_t diff = 0;
    for (uint32_t i = 0; i < 16u; i++)
        diff |= (uint32_t)(a[i] ^ b[i]);
    return diff == 0u;
}

/*
 * Every vector logs the SAME six words: tag, four ciphertext words, result.
 * The bench original varied its record width between vectors, which desynced
 * any reader that walked the log instead of hard-coding offsets.
 *
 * The result word separates "wrong bytes" from "the seam reported a failure",
 * because on CH592 those are completely different findings: a wrong ciphertext
 * is a broken cipher, a bad status is a wedged hardware engine.
 */
static uint32_t log_vector(uint32_t id, const uint8_t *got, const uint8_t *want,
                           hal_aes_status_t st)
{
    uint32_t result = 0;
    if (same16(got, want))
        result |= AES_LOG_RESULT_BYTES_OK;
    if (st == HAL_AES_OK)
        result |= AES_LOG_RESULT_STATUS_OK;

    put(AES_LOG_VECTOR_BASE | id);
    put_block(got);
    put(result);
    return result == (AES_LOG_RESULT_BYTES_OK | AES_LOG_RESULT_STATUS_OK);
}

static uint32_t vector(uint32_t id, const uint8_t *k, const uint8_t *p,
                       const uint8_t *want)
{
    uint8_t out[16];
    hal_aes_set_key(k);
    hal_aes_status_t st = hal_aes_encrypt_block(p, out);
    return log_vector(id, out, want, st);
}

/* ------------------------------------------------------- contract properties */

/* hal_aes.h promises `out` may alias `in`; CTR callers depend on it. */
static uint32_t check_in_place(void)
{
    uint8_t b[16];
    for (uint32_t i = 0; i < 16u; i++)
        b[i] = p_fips[i];
    hal_aes_set_key(k_fips);
    hal_aes_status_t st = hal_aes_encrypt_block(b, b);
    return log_vector(AES_LOG_VEC_IN_PLACE, b, c_fips, st);
}

/*
 * The contract promises ANY overlap, not just exact aliasing: both backends
 * read all 16 input bytes before writing any output byte. `out = in + 1` is
 * the case that catches an implementation which writes as it goes -- exact
 * aliasing alone would not.
 */
static uint32_t check_overlap(void)
{
    uint8_t b[17];
    for (uint32_t i = 0; i < 16u; i++)
        b[i] = p_fips[i];
    hal_aes_set_key(k_fips);
    hal_aes_status_t st = hal_aes_encrypt_block(b, b + 1);
    return log_vector(AES_LOG_VEC_OVERLAP, b + 1, c_fips, st);
}

/* Setting the same key twice must not corrupt the schedule. */
static uint32_t check_set_key_twice(void)
{
    uint8_t out[16];
    hal_aes_set_key(k_fips);
    hal_aes_set_key(k_fips);
    hal_aes_status_t st = hal_aes_encrypt_block(p_fips, out);
    return log_vector(AES_LOG_VEC_SET_KEY_TWICE, out, c_fips, st);
}

/* K1 -> K2 -> K1 must reproduce the K1 answer: expand_key fully replaces the
 * schedule rather than mixing into whatever was there. */
static uint32_t check_key_change(void)
{
    uint8_t out[16], other[16];
    for (uint32_t i = 0; i < 16u; i++)
        other[i] = (uint8_t)(k_fips[i] ^ 0xFFu);

    hal_aes_set_key(k_fips);
    (void)hal_aes_encrypt_block(p_fips, out);
    hal_aes_set_key(other);
    (void)hal_aes_encrypt_block(p_fips, out);
    hal_aes_set_key(k_fips);
    hal_aes_status_t st = hal_aes_encrypt_block(p_fips, out);
    return log_vector(AES_LOG_VEC_KEY_CHANGE, out, c_fips, st);
}

/* The seam promises purity: no cross-block chaining state. */
static uint32_t check_twice(void)
{
    uint8_t first[16], second[16];
    hal_aes_set_key(k_fips);
    hal_aes_status_t st = hal_aes_encrypt_block(p_fips, first);
    hal_aes_status_t st2 = hal_aes_encrypt_block(p_fips, second);
    if (st == HAL_AES_OK && st2 != HAL_AES_OK)
        st = st2;
    /* Logging `second` checks both that it is right AND that it matches the
     * first, since both are compared against the same constant. */
    if (!same16(first, second))
        st = HAL_AES_ENGINE_TIMEOUT; /* force a visible failure */
    return log_vector(AES_LOG_VEC_TWICE, second, c_fips, st);
}

/* ------------------------------------------------------------- differential */

/*
 * 512 blocks with an INDEPENDENT pseudorandom key and plaintext each, folded
 * into one FNV-1a-32.
 *
 * Fixed vectors only prove the points they test. Independent keys matter too:
 * a fixed-key sweep would never exercise the key schedule, which on the
 * assembly backends is a separate hand-written path from the cipher itself.
 *
 * Every constant here is load-bearing -- seed, count, the interleaving of key
 * and plaintext bytes from ONE stream, and the fold. Change any of them and
 * the expected checksum changes, silently invalidating comparison against
 * every result ever recorded. The host suite recomputes the same value from
 * the portable cipher (tests/test_aes_sw.py), so this is not folklore.
 */
static void differential(void)
{
    put(AES_LOG_DIFF_ENTER);

    uint32_t st = AES_LOG_DIFF_SEED;
    uint32_t sum = AES_LOG_DIFF_FNV_BASIS;
    uint32_t status_ok = 1u;
    uint8_t k[16], p[16], c[16];

    for (uint32_t n = 0; n < AES_LOG_DIFF_COUNT; n++) {
        for (uint32_t i = 0; i < 16u; i++) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  k[i] = (uint8_t)st;
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;  p[i] = (uint8_t)st;
        }
        hal_aes_set_key(k);
        if (hal_aes_encrypt_block(p, c) != HAL_AES_OK)
            status_ok = 0u;
        for (uint32_t i = 0; i < 16u; i++) {
            sum ^= c[i];
            sum *= AES_LOG_DIFF_FNV_PRIME;
        }
        /* Checkpoint the running fold so a divergence localises to a window
         * instead of only surfacing as a wrong final number. */
        if (((n + 1u) % AES_LOG_DIFF_CHECKPOINT) == 0u)
            put(sum);
    }

    put(AES_LOG_DIFF_EXIT);
    put(status_ok ? AES_LOG_DIFF_COUNT : 0u);
    put(sum);
    put_block(c); /* last ciphertext, to localise a mismatch */
}

/* ------------------------------------------------------------------- timing */

static void timings(void)
{
    uint8_t buf[16];
    uint32_t t0, t1;

    /* Calibrate the measurement's own cost so it can be judged, not guessed. */
    t0 = cycles();
    t1 = cycles();
    put(AES_LOG_TIMING_BASE | AES_LOG_TIME_OVERHEAD);
    put(t1 - t0);

    hal_aes_set_key(k_fips);
    BARRIER();
    t0 = cycles();
    for (uint32_t i = 0; i < 64u; i++)
        hal_aes_set_key(k_fips);
    BARRIER();
    t1 = cycles();
    put(AES_LOG_TIMING_BASE | AES_LOG_TIME_KEY_EXPAND);
    put((t1 - t0) / 64u);

    for (uint32_t i = 0; i < 16u; i++)
        buf[i] = p_fips[i];
    hal_aes_set_key(k_fips);
    BARRIER();
    t0 = cycles();
    for (uint32_t i = 0; i < 256u; i++)
        (void)hal_aes_encrypt_block(buf, buf);
    BARRIER();
    t1 = cycles();
    put(AES_LOG_TIMING_BASE | AES_LOG_TIME_BLOCK);
    put((t1 - t0) / 256u);
}

/* --------------------------------------------------------------------- main */

int main(void)
{
    /* Before anything else, and before the clock is touched. */
    vkeep_boot();
    aes_validate_platform_init();
    cycles_init();

    log_n = 0;
    log_overflowed = 0;
    aes_log[log_n++] = AES_LOG_MAGIC;
    aes_log[log_n++] = AES_LOG_VERSION;
    aes_log[log_n++] = AES_LOG_START;
    aes_log[log_n++] = DONGLE_VALIDATE_CLOCK_HZ;
    aes_log[log_n++] = DONGLE_VALIDATE_BACKEND;
    aes_log[log_n++] = DONGLE_VALIDATE_BUILD_ID;

    vkeep_stage(1u);
    hal_aes_init();

    uint32_t all = 1u;
    all &= vector(AES_LOG_VEC_FIPS197_C1, k_fips, p_fips, c_fips);
    all &= vector(AES_LOG_VEC_ALL_ZERO, k_zero, k_zero, c_zero);
    all &= vector(AES_LOG_VEC_SP80038A_1, k_nist, p_nist[0], c_nist[0]);
    all &= vector(AES_LOG_VEC_SP80038A_2, k_nist, p_nist[1], c_nist[1]);
    all &= vector(AES_LOG_VEC_SP80038A_3, k_nist, p_nist[2], c_nist[2]);
    all &= vector(AES_LOG_VEC_SP80038A_4, k_nist, p_nist[3], c_nist[3]);
    all &= check_in_place();
    all &= check_overlap();
    all &= check_set_key_twice();
    all &= check_key_change();
    all &= check_twice();

    put(AES_LOG_KAT_END);
    put(all);

    vkeep_stage(2u);
    differential();
    vkeep_stage(3u);
    timings();
    vkeep_stage(4u);

    /*
     * Record the core configuration these numbers were taken under. Bit 3
     * (ROM_LOOP_ACC) changes the flash-resident backends' cost by roughly 15x,
     * so a cycle count is not interpretable without it.
     *
     * THIS IS THE VALUE STARTUP WRITES, NOT A VALUE READ BACK, because on this
     * silicon CORECFGR IS WRITE-ONLY. A `csrr 0xbc0` destabilises a CH570; it
     * is not a readable register. Confirmed twice: an A/B against an otherwise
     * identical harness fails 3/3 with the read and passes 2/2 without it,
     * changing nothing else.
     *
     * The symptom is vicious, which is worth recording because it cost a long
     * debugging session: the part reboots, .bss is cleared, and the log comes
     * back looking like the cipher hung partway through the differential at a
     * deterministic block count -- nothing points at the CSR. And the read
     * existed only to record the configuration a measurement was taken under,
     * so it destroyed the very measurement it was there to qualify.
     *
     * CH570_CORECFGR_VALUE is the same constant reset_handler_ch570.S writes, so
     * this still cannot drift from what the core is actually running -- it is
     * one header, included by both. What it can no longer do is detect someone
     * changing CORECFGR at runtime, which nothing in this firmware does.
     */
#if DONGLE_VALIDATE_HAS_CORECFGR
    put(AES_LOG_CORECFGR);
    put(CH570_CORECFGR_VALUE);
#endif

    /* Terminator, using the word held in reserve by put(). */
    aes_log[log_n++] = log_overflowed ? AES_LOG_OVERFLOW : AES_LOG_END;

    /* Preserve the finished record where a reset cannot clear it. */
    vkeep_save(log_n);
    vkeep_stage(5u);

#ifdef DONGLE_VALIDATE_HOSTED
    /* Host suite only: return so the caller can dump the log. On a device
     * there is nowhere to return TO -- the runner halts the core and reads
     * `aes_log` out of RAM, so the harness must still be here when it does. */
    return (int)log_n;
#else
    for (;;)
        BARRIER();
#endif
}
