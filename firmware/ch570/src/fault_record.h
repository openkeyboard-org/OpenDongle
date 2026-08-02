#ifndef CH570_FAULT_RECORD_H
#define CH570_FAULT_RECORD_H

/* N23 retained-fault contract. The linker owns a four-byte startup prefix
 * followed by this versioned record. Keep these offsets synchronized with
 * fault_handler.S, reset_handler_ch570.S, and link.ld. */
#define CH570_FAULT_STARTUP_SIZE            4
#define CH570_FAULT_KEEP_SIZE              48

#define CH570_FAULT_MAGIC                  0x3037464E
#define CH570_FAULT_MAGIC_INVERSE          0xCFC8B9B1
#define CH570_FAULT_RECORD_VERSION         1

/* R8_GLOB_RESET_KEEP state machine. Hardware clears the byte on RPOR/GRWSM;
 * early C boot publishes ARMED. The first fatal event publishes CONSUMED
 * before requesting its one permitted software reset. */
#define CH570_FAULT_RESET_KEEP_ARMED        0x5C
#define CH570_FAULT_RESET_KEEP_CONSUMED     0xA3

#define CH570_FAULT_KIND_NONE              0x00
#define CH570_FAULT_KIND_HARDFAULT         0xDE
#define CH570_FAULT_KIND_NMI               0xDF

#define CH570_FAULT_ACTION_NONE            0
#define CH570_FAULT_ACTION_RESET_REQUESTED 1
#define CH570_FAULT_ACTION_RECOVERED       2
#define CH570_FAULT_ACTION_REPEAT_FAILSTOP 3
#define CH570_FAULT_ACTION_GUARD_ONLY      4
#define CH570_FAULT_ACTION_RESET_MISMATCH  5

#define CH570_FAULT_FLAG_REBUILT           0x01
#define CH570_FAULT_FLAG_RESET_MISMATCH    0x02

#define CH570_FAULT_OFF_MAGIC               0
#define CH570_FAULT_OFF_MAGIC_INVERSE       4
#define CH570_FAULT_OFF_VERSION             8
#define CH570_FAULT_OFF_KIND                9
#define CH570_FAULT_OFF_ACTION             10
#define CH570_FAULT_OFF_FLAGS              11
#define CH570_FAULT_OFF_EVENT_COUNT        12
#define CH570_FAULT_OFF_RESET_REQUEST_COUNT 16
#define CH570_FAULT_OFF_REPEAT_COUNT       20
#define CH570_FAULT_OFF_MEPC               24
#define CH570_FAULT_OFF_MCAUSE             28
#define CH570_FAULT_OFF_MTVAL              32
#define CH570_FAULT_OFF_BOOT_COUNT         36
#define CH570_FAULT_OFF_LAST_RESET_STATUS  40
#define CH570_FAULT_OFF_RESET_KEEP         41
#define CH570_FAULT_OFF_RESERVED_0         42
#define CH570_FAULT_OFF_RESERVED_1         43
#define CH570_FAULT_RECORD_SIZE            44

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

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
} ch570_fault_record_t;

extern volatile ch570_fault_record_t ch570_fault_record;
extern uint8_t _fault_keep_start[];

_Static_assert(sizeof(ch570_fault_record_t) == CH570_FAULT_RECORD_SIZE,
               "CH570 retained-fault record size changed");
#define CH570_FAULT_ASSERT_OFFSET(field, offset) \
    _Static_assert(offsetof(ch570_fault_record_t, field) == (offset), \
                   "CH570 retained-fault " #field " offset changed")
CH570_FAULT_ASSERT_OFFSET(magic, CH570_FAULT_OFF_MAGIC);
CH570_FAULT_ASSERT_OFFSET(magic_inverse, CH570_FAULT_OFF_MAGIC_INVERSE);
CH570_FAULT_ASSERT_OFFSET(version, CH570_FAULT_OFF_VERSION);
CH570_FAULT_ASSERT_OFFSET(kind, CH570_FAULT_OFF_KIND);
CH570_FAULT_ASSERT_OFFSET(action, CH570_FAULT_OFF_ACTION);
CH570_FAULT_ASSERT_OFFSET(flags, CH570_FAULT_OFF_FLAGS);
CH570_FAULT_ASSERT_OFFSET(event_count, CH570_FAULT_OFF_EVENT_COUNT);
CH570_FAULT_ASSERT_OFFSET(reset_request_count,
                          CH570_FAULT_OFF_RESET_REQUEST_COUNT);
CH570_FAULT_ASSERT_OFFSET(repeat_count, CH570_FAULT_OFF_REPEAT_COUNT);
CH570_FAULT_ASSERT_OFFSET(mepc, CH570_FAULT_OFF_MEPC);
CH570_FAULT_ASSERT_OFFSET(mcause, CH570_FAULT_OFF_MCAUSE);
CH570_FAULT_ASSERT_OFFSET(mtval, CH570_FAULT_OFF_MTVAL);
CH570_FAULT_ASSERT_OFFSET(boot_count, CH570_FAULT_OFF_BOOT_COUNT);
CH570_FAULT_ASSERT_OFFSET(last_reset_status,
                          CH570_FAULT_OFF_LAST_RESET_STATUS);
CH570_FAULT_ASSERT_OFFSET(reset_keep, CH570_FAULT_OFF_RESET_KEEP);
CH570_FAULT_ASSERT_OFFSET(reserved, CH570_FAULT_OFF_RESERVED_0);
#undef CH570_FAULT_ASSERT_OFFSET

#endif /* !__ASSEMBLER__ */

#endif /* CH570_FAULT_RECORD_H */
