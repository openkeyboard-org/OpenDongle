/*
 * Bridge75 Open-Source Dongle Firmware
 * CH592F platform hooks for common bond/IAP code.
 */

#include "CH59x_common.h"
#include "dongle_platform.h"
#include "fault_record.h"

static uint8_t ch592_boot_reset_status;

/* Linker-pinned NOLOAD record shared with the no-software-sp assembly bodies. */
volatile ch592_fault_record_t ch592_fault_record
    __attribute__((section(".noinit.ch592_fault"), used, aligned(4)));

static uint8_t ch592_fault_record_valid(void)
{
    return ch592_fault_record.magic == CH592_FAULT_MAGIC
        && ch592_fault_record.magic_inverse == CH592_FAULT_MAGIC_INVERSE
        && ch592_fault_record.version == CH592_FAULT_RECORD_VERSION;
}

static uint32_t ch592_sat_inc(uint32_t value)
{
    return value == 0xffffffffu ? value : value + 1u;
}

/* End the old record transaction before changing any payload. The I/O bits
 * also order retained SRAM against the reset-keeper MMIO state machine. */
static void ch592_fault_invalidate(void)
{
    ch592_fault_record.magic = 0u;
    __asm volatile("fence iorw, iorw" ::: "memory");
}

/* Publish magic last, matching fault_handler.S. This makes any interrupted
 * healthy-boot repair or handler update fail closed as an invalid record. */
static void ch592_fault_publish(void)
{
    ch592_fault_record.magic_inverse = CH592_FAULT_MAGIC_INVERSE;
    __asm volatile("fence iorw, iorw" ::: "memory");
    ch592_fault_record.magic = CH592_FAULT_MAGIC;
    __asm volatile("fence iorw, iorw" ::: "memory");
}

static void ch592_fault_record_reset(uint8_t reset_status,
                                     uint8_t reset_keep,
                                     uint8_t action,
                                     uint8_t flags)
{
    ch592_fault_invalidate();
    ch592_fault_record.magic_inverse = 0u;
    ch592_fault_record.version = CH592_FAULT_RECORD_VERSION;
    ch592_fault_record.kind = CH592_FAULT_KIND_NONE;
    ch592_fault_record.action = action;
    ch592_fault_record.flags = flags;
    ch592_fault_record.event_count = 0u;
    ch592_fault_record.reset_request_count = 0u;
    ch592_fault_record.repeat_count = 0u;
    ch592_fault_record.mepc = 0u;
    ch592_fault_record.mcause = 0u;
    ch592_fault_record.mtval = 0u;
    ch592_fault_record.boot_count = 1u;
    ch592_fault_record.last_reset_status = reset_status;
    ch592_fault_record.reset_keep = reset_keep;
    ch592_fault_record.reserved[0] = 0u;
    ch592_fault_record.reserved[1] = 0u;
    ch592_fault_publish();
}


uint8_t dongle_nv_read(uint32_t off, void *out, uint32_t len)
{
    return (uint8_t)EEPROM_READ(off, out, len);
}

uint8_t dongle_nv_is_erased(uint32_t off, uint32_t len)
{
    uint8_t buf[32];

    if (len > sizeof(buf)) {
        return 0u;
    }
    if (EEPROM_READ(off, buf, len) != 0) {
        return 0u;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != 0xffu) {
            return 0u;
        }
    }
    return 1u;
}

uint8_t dongle_nv_erase(uint32_t off, uint32_t len)
{
    return (uint8_t)EEPROM_ERASE(off, len);
}

uint8_t dongle_nv_write(uint32_t off, const void *data, uint32_t len)
{
    return (uint8_t)EEPROM_WRITE(off, (void *)data, len);
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

void dongle_fault_boot(uint8_t reset_status)
{
    uint8_t reset_keep = R8_GLOB_RESET_KEEP;

    ch592_boot_reset_status = reset_status;

    /* R8_GLOB_RESET_KEEP is cleared only by RPOR/GRWSM. Claim a freshly
     * cleared/unknown value with the ARMED token before publishing the new
     * record. This distinguishes the current epoch from stale retained SRAM
     * if a fatal exception arrives after the early vectors are installed but
     * before this C hook runs. */
    if (reset_keep != CH592_FAULT_RESET_KEEP_ARMED
        && reset_keep != CH592_FAULT_RESET_KEEP_CONSUMED) {
        /* Invalidate stale SRAM before publishing ARMED. Otherwise a fault in
         * the few instructions between the keeper write and record_reset()
         * could accept a valid-looking record from the prior powered epoch. */
        ch592_fault_invalidate();
        R8_GLOB_RESET_KEEP = CH592_FAULT_RESET_KEEP_ARMED;
        reset_keep = CH592_FAULT_RESET_KEEP_ARMED;
        ch592_fault_record_reset(reset_status, reset_keep,
                                 CH592_FAULT_ACTION_NONE, 0u);
        return;
    }

    /* An initialized, still-armed epoch retains a genuine boot count across
     * manual/watchdog resets. An invalid record cannot hide a consumed fault:
     * the handler writes CONSUMED before publishing its record. */
    if (reset_keep == CH592_FAULT_RESET_KEEP_ARMED) {
        if (!ch592_fault_record_valid()) {
            ch592_fault_record_reset(reset_status, reset_keep,
                                     CH592_FAULT_ACTION_NONE, 0u);
            return;
        }
        ch592_fault_invalidate();
        ch592_fault_record.boot_count =
            ch592_sat_inc(ch592_fault_record.boot_count);
        ch592_fault_record.last_reset_status = reset_status;
        ch592_fault_record.reset_keep = reset_keep;
        ch592_fault_publish();
        return;
    }

    /* A consumed guard with an invalid SRAM record is still authoritative:
     * preserve the no-more-auto-resets policy and expose that evidence had to
     * be rebuilt rather than silently manufacturing fault CSRs. */
    if (!ch592_fault_record_valid()) {
        ch592_fault_record_reset(reset_status, reset_keep,
                                 CH592_FAULT_ACTION_GUARD_ONLY,
                                 CH592_FAULT_FLAG_REBUILT);
        return;
    }

    /* If NMI lands after this invalidation, the assembly handler sees the
     * authoritative CONSUMED keeper and emits a REBUILT repeat-failstop
     * record instead of accepting this partially reconciled payload. */
    ch592_fault_invalidate();
    if (ch592_fault_record.action == CH592_FAULT_ACTION_RESET_REQUESTED) {
        if ((reset_status & RB_RESET_FLAG) == RST_FLAG_SW) {
            ch592_fault_record.action = CH592_FAULT_ACTION_RECOVERED;
        } else {
            ch592_fault_record.action = CH592_FAULT_ACTION_RESET_MISMATCH;
            ch592_fault_record.flags |= CH592_FAULT_FLAG_RESET_MISMATCH;
        }
    }
    ch592_fault_record.boot_count =
        ch592_sat_inc(ch592_fault_record.boot_count);
    ch592_fault_record.last_reset_status = reset_status;
    ch592_fault_record.reset_keep = reset_keep;
    ch592_fault_publish();
}

static void fault_put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/* N23 CH592 fault page v1 (40 B):
 *   [0] family, [1] page version, [2] record valid, [3] record version,
 *   [4] kind, [5] action, [6] flags, [7] current reset keeper,
 *   [8..11] event_count, [12..15] reset_request_count,
 *   [16..19] repeat_count, [20..23] boot_count,
 *   [24..27] mcause, [28..31] mepc, [32..35] mtval,
 *   [36] last reset status, [37] startup marker,
 *   [38] startup-captured reset status, [39] record's keeper snapshot. */
uint8_t dongle_fault_fill(uint8_t *out, uint8_t max)
{
    const volatile uint8_t *startup =
        (const volatile uint8_t *)CH592_FAULT_KEEP_BASE;
    uint8_t valid = ch592_fault_record_valid();
    uint8_t i;

    if (max < 40u) {
        return 0u;
    }
    for (i = 0u; i < 40u; i++) {
        out[i] = 0u;
    }

    out[0] = DONGLE_CHIP_FAMILY_ID;
    out[1] = 1u;
    out[2] = valid;
    out[3] = valid ? ch592_fault_record.version : 0u;
    out[7] = R8_GLOB_RESET_KEEP;
    out[37] = startup[0];
    out[38] = startup[1];
    if (!valid) {
        return 40u;
    }

    out[4] = ch592_fault_record.kind;
    out[5] = ch592_fault_record.action;
    out[6] = ch592_fault_record.flags;
    fault_put32(&out[8], ch592_fault_record.event_count);
    fault_put32(&out[12], ch592_fault_record.reset_request_count);
    fault_put32(&out[16], ch592_fault_record.repeat_count);
    fault_put32(&out[20], ch592_fault_record.boot_count);
    fault_put32(&out[24], ch592_fault_record.mcause);
    fault_put32(&out[28], ch592_fault_record.mepc);
    fault_put32(&out[32], ch592_fault_record.mtval);
    out[36] = ch592_fault_record.last_reset_status;
    out[39] = ch592_fault_record.reset_keep;
    return 40u;
}
