/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CH570 platform hooks for common bond/IAP code.
 */

#include "CH57x_common.h"
#include "bond.h"
#include "dongle_platform.h"
#include "fault_record.h"
#include "rf_task.h"
#include "sched.h"
#include "usb_device.h"

volatile uint8_t ch570_boot_reset_status;


static uint8_t ch570_fault_record_valid(void)
{
    return ch570_fault_record.magic == CH570_FAULT_MAGIC
        && ch570_fault_record.magic_inverse == CH570_FAULT_MAGIC_INVERSE
        && ch570_fault_record.version == CH570_FAULT_RECORD_VERSION;
}

static uint32_t ch570_sat_inc(uint32_t value)
{
    return value == 0xffffffffu ? value : value + 1u;
}

static void ch570_fault_invalidate(void)
{
    ch570_fault_record.magic = 0u;
    __asm volatile("fence iorw, iorw" ::: "memory");
}

static void ch570_fault_publish(void)
{
    ch570_fault_record.magic_inverse = CH570_FAULT_MAGIC_INVERSE;
    __asm volatile("fence iorw, iorw" ::: "memory");
    ch570_fault_record.magic = CH570_FAULT_MAGIC;
    __asm volatile("fence iorw, iorw" ::: "memory");
}

static void ch570_fault_record_reset(uint8_t reset_status,
                                     uint8_t reset_keep,
                                     uint8_t action,
                                     uint8_t flags)
{
    ch570_fault_invalidate();
    ch570_fault_record.magic_inverse = 0u;
    ch570_fault_record.version = CH570_FAULT_RECORD_VERSION;
    ch570_fault_record.kind = CH570_FAULT_KIND_NONE;
    ch570_fault_record.action = action;
    ch570_fault_record.flags = flags;
    ch570_fault_record.event_count = 0u;
    ch570_fault_record.reset_request_count = 0u;
    ch570_fault_record.repeat_count = 0u;
    ch570_fault_record.mepc = 0u;
    ch570_fault_record.mcause = 0u;
    ch570_fault_record.mtval = 0u;
    ch570_fault_record.boot_count = 1u;
    ch570_fault_record.last_reset_status = reset_status;
    ch570_fault_record.reset_keep = reset_keep;
    ch570_fault_record.reserved[0] = 0u;
    ch570_fault_record.reserved[1] = 0u;
    ch570_fault_publish();
}

/* Reconcile the hardware-cleared reset epoch with retained SRAM. The keeper is
 * authoritative: a consumed token is never silently rearmed by a warm reset. */
static void ch570_fault_boot(uint8_t reset_status)
{
    uint8_t reset_keep = R8_GLOB_RESET_KEEP;

    ch570_boot_reset_status = reset_status;
    if (reset_keep != CH570_FAULT_RESET_KEEP_ARMED
        && reset_keep != CH570_FAULT_RESET_KEEP_CONSUMED) {
        ch570_fault_invalidate();
        R8_GLOB_RESET_KEEP = CH570_FAULT_RESET_KEEP_ARMED;
        reset_keep = CH570_FAULT_RESET_KEEP_ARMED;
        ch570_fault_record_reset(reset_status, reset_keep,
                                 CH570_FAULT_ACTION_NONE, 0u);
        return;
    }

    if (reset_keep == CH570_FAULT_RESET_KEEP_ARMED) {
        if (!ch570_fault_record_valid()) {
            ch570_fault_record_reset(reset_status, reset_keep,
                                     CH570_FAULT_ACTION_NONE, 0u);
            return;
        }
        ch570_fault_invalidate();
        ch570_fault_record.boot_count =
            ch570_sat_inc(ch570_fault_record.boot_count);
        ch570_fault_record.last_reset_status = reset_status;
        ch570_fault_record.reset_keep = reset_keep;
        ch570_fault_publish();
        return;
    }

    if (!ch570_fault_record_valid()) {
        ch570_fault_record_reset(reset_status, reset_keep,
                                 CH570_FAULT_ACTION_GUARD_ONLY,
                                 CH570_FAULT_FLAG_REBUILT);
        return;
    }

    ch570_fault_invalidate();
    if (ch570_fault_record.action == CH570_FAULT_ACTION_RESET_REQUESTED) {
        if ((reset_status & RB_RESET_FLAG) == RST_FLAG_SW) {
            ch570_fault_record.action = CH570_FAULT_ACTION_RECOVERED;
        } else {
            ch570_fault_record.action = CH570_FAULT_ACTION_RESET_MISMATCH;
            ch570_fault_record.flags |= CH570_FAULT_FLAG_RESET_MISMATCH;
        }
    }
    ch570_fault_record.boot_count =
        ch570_sat_inc(ch570_fault_record.boot_count);
    ch570_fault_record.last_reset_status = reset_status;
    ch570_fault_record.reset_keep = reset_keep;
    ch570_fault_publish();
}

/* Cold-boot SRAM entropy for the session-AA RNG seed. Hashes the pristine
 * power-on state of the free RAM above the heap base (not-yet-used stack/heap
 * gap) into rf_ch570_boot_entropy. MUST be called as main()'s first act.
 * Reserves nothing — it reads RAM that is used normally afterward, keeping
 * only a one-word hash. Measured on
 * this part: ~17% of SRAM cells are metastable at power-on (15/15 cold boots
 * distinct), so hashing ~1.5 KB yields far more than the ~32 bits the seed needs.
 * Cold-boot only: a warm/software reset preserves RAM, but a fresh pair follows
 * a replug, so the AA reseeds at the right granularity. */
#define CH570_ENTROPY_SKIP 64u     /* margin over any early heap word above _end */
#define CH570_ENTROPY_LEN  1536u   /* bytes of power-on RAM to hash */
/* The read window [_end+SKIP, _end+SKIP+LEN) must stay within the RAM the linker
 * guarantees free above _end. The 0x800 floor keeps this window below RAM top.
 * Keep both assertions in sync if the window changes. */
_Static_assert(CH570_ENTROPY_SKIP + CH570_ENTROPY_LEN <= 0x640u,
    "boot-entropy read must fit the minimum linker-guaranteed stack-floor gap");
void ch570_capture_boot_entropy(void)
{
    extern uint32_t _end;   /* heap base (.bss/.noinit end), linker-provided */
    const volatile uint8_t *p =
        (const volatile uint8_t *)((uintptr_t)&_end + CH570_ENTROPY_SKIP);
    uint32_t h = 0x811C9DC5u;   /* FNV-1a-32 */
    uint32_t i;
    for (i = 0u; i < CH570_ENTROPY_LEN; i++) {
        h = (h ^ p[i]) * 0x01000193u;
    }
    rf_ch570_boot_entropy = h;
}

/* Fold clock jitter into the session-AA entropy word as a WEAK but INDEPENDENT
 * secondary source on top of the SRAM hash. Enables a free-running SysTick
 * (sysclk/HCLK, 100 MHz) and samples the HCLK-cycle count per LSI period
 * (R32_RTC_CNT_LSI edges) over 32 periods, FNV-folding the deltas + phase into
 * rf_ch570_boot_entropy. Measured behaviour (bench): within a boot the LSI-vs-HCLK
 * beat is nearly locked (the cross-domain read synchronizer masks fine RC
 * jitter), but its phase/period drift across cold boots contributes ~3-5
 * independent bits. Mixed by FNV (bijective per step), so it can only ADD/permute
 * entropy — a stuck LSI folds in harmlessly and the SRAM hash survives intact.
 * Call after the LSI is powered (main.c), following ch570_capture_boot_entropy().
 * Cost ~1 ms once the RC is oscillating (32 LSI periods at ~32 kHz); the priming
 * wait absorbs cold RC startup. If the LSI never ticks (hardware fault), the
 * priming wait times out ONCE (~tens of ms) and the sample is skipped. */
void ch570_mix_jitter_entropy(void)
{
    uint32_t h = rf_ch570_boot_entropy;   /* start from the SRAM hash */
    uint32_t prev, now, rc, spin;
    uint8_t i;
    if ((SysTick->CTLR & SysTick_CTLR_STE) == 0u) {
        SysTick->CMP  = SysTick_LOAD_RELOAD_Msk;
        SysTick->CNT  = 0u;
        SysTick->CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK;
    }
    /* Prime on the first LSI edge (also absorbs RC startup). If the counter
     * never advances the LSI is dead — skip the loop so a hardware fault costs
     * one timeout, not 32, and leave the SRAM hash untouched. */
    rc = R32_RTC_CNT_LSI; spin = 0u;
    while (R32_RTC_CNT_LSI == rc && spin < 400000u) { spin++; }
    if (R32_RTC_CNT_LSI == rc) {
        return;
    }
    prev = (uint32_t)SysTick->CNT;
    for (i = 0u; i < 32u; i++) {
        rc = R32_RTC_CNT_LSI; spin = 0u;
        while (R32_RTC_CNT_LSI == rc && spin < 400000u) { spin++; }
        now = (uint32_t)SysTick->CNT;
        h = (h ^ (uint8_t)(now - prev)) * 0x01000193u;   /* HCLK cycles per LSI period */
        h = (h ^ (uint8_t)now) * 0x01000193u;            /* + absolute sampling phase */
        prev = now;
    }
    rf_ch570_boot_entropy = h;
}


static void fault_put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/* CH570 fault page v5 (40 B), matching the CH592 fatal-fault schema:
 *   [0] family, [1] page version, [2] record valid, [3] record version,
 *   [4] kind, [5] action, [6] flags, [7] current reset keeper,
 *   [8..11] event_count, [12..15] reset_request_count,
 *   [16..19] repeat_count, [20..23] boot_count,
 *   [24..27] mcause, [28..31] mepc, [32..35] mtval,
 *   [36] last reset status, [37] startup marker,
 *   [38] startup-captured reset status, [39] record keeper snapshot. */
uint8_t dongle_fault_fill(uint8_t *out, uint8_t max)
{
    const volatile uint8_t *startup =
        (const volatile uint8_t *)_fault_keep_start;
    uint8_t valid = ch570_fault_record_valid();
    uint8_t i;

    if (max < 40u) {
        return 0u;
    }
    for (i = 0u; i < 40u; i++) {
        out[i] = 0u;
    }
    out[0] = DONGLE_CHIP_FAMILY_ID;
    out[1] = 5u;
    out[2] = valid;
    out[3] = valid ? ch570_fault_record.version : 0u;
    out[7] = R8_GLOB_RESET_KEEP;
    out[37] = startup[0];
    out[38] = startup[1];
    if (valid) {
        out[4] = ch570_fault_record.kind;
        out[5] = ch570_fault_record.action;
        out[6] = ch570_fault_record.flags;
        fault_put32(&out[8], ch570_fault_record.event_count);
        fault_put32(&out[12], ch570_fault_record.reset_request_count);
        fault_put32(&out[16], ch570_fault_record.repeat_count);
        fault_put32(&out[20], ch570_fault_record.boot_count);
        fault_put32(&out[24], ch570_fault_record.mcause);
        fault_put32(&out[28], ch570_fault_record.mepc);
        fault_put32(&out[32], ch570_fault_record.mtval);
        out[36] = ch570_fault_record.last_reset_status;
        out[39] = ch570_fault_record.reset_keep;
    }
    return 40u;
}

void dongle_fault_boot(uint8_t reset_status)
{
    ch570_fault_boot(reset_status);
}

static uint8_t nv_range_ok(uint32_t off, uint32_t len)
{
    uint32_t rel;

    if (off < BOND_EEPROM_OFF) {
        return 0u;
    }
    rel = off - BOND_EEPROM_OFF;
    return (uint8_t)(rel <= CH570_BOND_FLASH_SIZE &&
                     len <= (CH570_BOND_FLASH_SIZE - rel));
}

static uint32_t nv_flash_addr(uint32_t off)
{
    return CH570_BOND_FLASH_ADDR + (off - BOND_EEPROM_OFF);
}

uint8_t dongle_nv_read(uint32_t off, void *out, uint32_t len)
{
    if (!nv_range_ok(off, len)) {
        return 0xFF;
    }
    FLASH_ROM_READ(nv_flash_addr(off), out, len);
    return 0;
}

uint8_t dongle_nv_is_erased(uint32_t off, uint32_t len)
{
    uint32_t erased_buf[8];
    uint8_t erased;

    if (!nv_range_ok(off, len) ||
        len > sizeof(erased_buf) ||
        (len & (sizeof(erased_buf[0]) - 1u)) != 0u) {
        return 0u;
    }
    /* CODEREVIEW N16: controller-VERIFY erased, do NOT FLASH_ROM_READ + compare
     * (a plain XIP word copy that F26 proved can serve stale data). A bond page
     * read earlier this boot could read back its pre-erase (non-erased) bytes
     * here and falsely fail a bond_clear that actually erased -- N06's clear
     * treats a failed verify as a failed clear. An erased cell reads back
     * through the controller as CH570_FLASH_ERASED_WORD; the compare buffer
     * must live in 4-byte-aligned RAM (SDK contract). */
    for (uint32_t i = 0; i < (len / sizeof(erased_buf[0])); i++) {
        erased_buf[i] = CH570_FLASH_ERASED_WORD;
    }
    erased = (uint8_t)(FLASH_ROM_VERIFY(nv_flash_addr(off), erased_buf, len) == 0);
    return erased;
}

uint8_t dongle_nv_erase(uint32_t off, uint32_t len)
{
    uint8_t status;

    if (!nv_range_ok(off, len)) {
        return 0xFF;
    }
    status = (uint8_t)FLASH_ROM_ERASE(CH570_BOND_FLASH_ADDR,
                                      CH570_BOND_FLASH_SIZE);
    return status;
}

uint8_t dongle_nv_write(uint32_t off, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t status;

    if (!nv_range_ok(off, len)) {
        return 0xFF;
    }
    if ((len & 3u) != 0u) {
        return 0xFE;
    }

    for (uint32_t pos = 0; pos < len; pos += 4u) {
        status = (uint8_t)FLASH_ROM_WRITE(nv_flash_addr(off) + pos,
                                          (void *)(p + pos),
                                          4u);
        if (status != 0u) {
            return 1u;
        }
    }
    return 0u;
}

uint8_t dongle_unique_id_fill(uint8_t *out, uint8_t max)
{
    uint8_t uid[DONGLE_UID_LEN] __attribute__((aligned(4)));

    if (max < DONGLE_UID_LEN) {
        return 0u;
    }
    GET_UNIQUE_ID(uid);
    memcpy(out, uid, DONGLE_UID_LEN);
    return DONGLE_UID_LEN;
}
