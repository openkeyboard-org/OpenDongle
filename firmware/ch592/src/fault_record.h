#ifndef CH592_FAULT_RECORD_H
#define CH592_FAULT_RECORD_H

/* N23 retained-fault contract. The first four bytes remain owned by the
 * phased startup sentinel; the versioned record begins immediately after it.
 * Keep these offsets synchronized with fault_handler.S and link.ld. */
#define CH592_FAULT_KEEP_BASE              0x20005800
#define CH592_FAULT_RECORD_BASE            0x20005804
#define CH592_FAULT_KEEP_END               0x20005830

#define CH592_FAULT_MAGIC                  0x3332464E
#define CH592_FAULT_MAGIC_INVERSE          0xCCCDB9B1
#define CH592_FAULT_RECORD_VERSION         1
/* R8_GLOB_RESET_KEEP state machine. Hardware clears the byte on RPOR/GRWSM;
 * early C boot publishes ARMED so a pre-main fault cannot mistake stale SRAM
 * from the prior powered epoch for the current epoch. CONSUMED is written
 * before the first reset request and survives that software reset. */
#define CH592_FAULT_RESET_KEEP_ARMED        0x5C
#define CH592_FAULT_RESET_KEEP_CONSUMED     0xA3

#define CH592_FAULT_KIND_NONE              0x00
#define CH592_FAULT_KIND_HARDFAULT         0xDE
#define CH592_FAULT_KIND_NMI               0xDF

#define CH592_FAULT_ACTION_NONE            0
#define CH592_FAULT_ACTION_RESET_REQUESTED 1
#define CH592_FAULT_ACTION_RECOVERED       2
#define CH592_FAULT_ACTION_REPEAT_FAILSTOP 3
#define CH592_FAULT_ACTION_GUARD_ONLY      4
#define CH592_FAULT_ACTION_RESET_MISMATCH  5

#define CH592_FAULT_FLAG_REBUILT           0x01
#define CH592_FAULT_FLAG_RESET_MISMATCH    0x02

#define CH592_FAULT_OFF_MAGIC              0
#define CH592_FAULT_OFF_MAGIC_INVERSE      4
#define CH592_FAULT_OFF_VERSION            8
#define CH592_FAULT_OFF_KIND               9
#define CH592_FAULT_OFF_ACTION             10
#define CH592_FAULT_OFF_FLAGS              11
#define CH592_FAULT_OFF_EVENT_COUNT        12
#define CH592_FAULT_OFF_RESET_REQUEST_COUNT 16
#define CH592_FAULT_OFF_REPEAT_COUNT       20
#define CH592_FAULT_OFF_MEPC               24
#define CH592_FAULT_OFF_MCAUSE             28
#define CH592_FAULT_OFF_MTVAL              32
#define CH592_FAULT_OFF_BOOT_COUNT         36
#define CH592_FAULT_OFF_LAST_RESET_STATUS  40
#define CH592_FAULT_OFF_RESET_KEEP         41
#define CH592_FAULT_OFF_RESERVED_0         42
#define CH592_FAULT_OFF_RESERVED_1         43
#define CH592_FAULT_RECORD_SIZE            44

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint8_t version;
    uint8_t kind;
    uint8_t action;
    uint8_t flags;
    uint32_t event_count;
    uint32_t reset_request_count;
    uint32_t repeat_count;
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t boot_count;
    uint8_t last_reset_status;
    uint8_t reset_keep;
    uint8_t reserved[2];
} ch592_fault_record_t;

extern volatile ch592_fault_record_t ch592_fault_record;

_Static_assert(sizeof(ch592_fault_record_t) == CH592_FAULT_RECORD_SIZE,
               "CH592 retained-fault record size changed");
#define CH592_FAULT_ASSERT_OFFSET(field, offset) \
    _Static_assert(offsetof(ch592_fault_record_t, field) == (offset), \
                   "CH592 retained-fault " #field " offset changed")
CH592_FAULT_ASSERT_OFFSET(magic, CH592_FAULT_OFF_MAGIC);
CH592_FAULT_ASSERT_OFFSET(magic_inverse, CH592_FAULT_OFF_MAGIC_INVERSE);
CH592_FAULT_ASSERT_OFFSET(version, CH592_FAULT_OFF_VERSION);
CH592_FAULT_ASSERT_OFFSET(kind, CH592_FAULT_OFF_KIND);
CH592_FAULT_ASSERT_OFFSET(action, CH592_FAULT_OFF_ACTION);
CH592_FAULT_ASSERT_OFFSET(flags, CH592_FAULT_OFF_FLAGS);
CH592_FAULT_ASSERT_OFFSET(event_count, CH592_FAULT_OFF_EVENT_COUNT);
CH592_FAULT_ASSERT_OFFSET(reset_request_count,
                          CH592_FAULT_OFF_RESET_REQUEST_COUNT);
CH592_FAULT_ASSERT_OFFSET(repeat_count, CH592_FAULT_OFF_REPEAT_COUNT);
CH592_FAULT_ASSERT_OFFSET(mepc, CH592_FAULT_OFF_MEPC);
CH592_FAULT_ASSERT_OFFSET(mcause, CH592_FAULT_OFF_MCAUSE);
CH592_FAULT_ASSERT_OFFSET(mtval, CH592_FAULT_OFF_MTVAL);
CH592_FAULT_ASSERT_OFFSET(boot_count, CH592_FAULT_OFF_BOOT_COUNT);
CH592_FAULT_ASSERT_OFFSET(last_reset_status,
                          CH592_FAULT_OFF_LAST_RESET_STATUS);
CH592_FAULT_ASSERT_OFFSET(reset_keep, CH592_FAULT_OFF_RESET_KEEP);
CH592_FAULT_ASSERT_OFFSET(reserved, CH592_FAULT_OFF_RESERVED_0);
#undef CH592_FAULT_ASSERT_OFFSET

#endif /* !__ASSEMBLER__ */

#endif /* CH592_FAULT_RECORD_H */
