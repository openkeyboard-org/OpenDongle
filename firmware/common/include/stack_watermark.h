/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Stack watermark instrumentation, measurement builds only
 * (EXTRA_CFLAGS=-DDONGLE_STACK_WATERMARK=1).
 *
 * Purpose: put a number on the worst-case stack depth. The CH570 stack floor
 * (link.ld CH570_STACK_FLOOR) and the CH592 true-stack budget (2026-08-16
 * review, finding 18) were both chosen without one; lowering the CH570 floor
 * to relink the encrypted image is gated on this measurement (finding 4).
 *
 * Method: paint the free RAM between the static image (_end) and the current
 * stack pointer with a pattern at boot, run the exercise matrix, then read
 * the low-water mark -- the first still-painted address scanning UP from
 * _end is the deepest the stack (interrupt frames included) ever reached.
 *
 * Ordering contract (CH570): ch570_capture_boot_entropy() reads the PRISTINE
 * power-on RAM in [_end+64, _end+1600) as main()'s first act; painting is
 * main()'s SECOND act, never earlier, or the entropy seed is destroyed.
 *
 * Readout: IAP command 0x96 (iap.c), plus an offline SRAM dump cross-check
 * (minichlink). Reading is safe at any time: the scan only loads from below
 * the live stack.
 */

#ifndef STACK_WATERMARK_H
#define STACK_WATERMARK_H

#include <stdint.h>
#include "dongle_target.h"      /* RF_TASK_EXECUTOR_TMOS discriminates the layout */

#ifndef DONGLE_STACK_WATERMARK
#define DONGLE_STACK_WATERMARK 0
#endif

#if DONGLE_STACK_WATERMARK

#define STACK_WATERMARK_PATTERN 0xC5C5C5C5u

/* Linker-provided. _end = end of the static image; _eusrstack = top of stack. */
extern uint32_t _end;
extern uint32_t _eusrstack;
#if RF_TASK_EXECUTOR_TMOS
/* CH592: an RF/BLE arena, the heap, and the retained fault record all sit
 * between _end and the stack, and every one of them is written at boot. So
 * the SCAN must start at the guarded stack floor (_susrstack = _rf_arena_end),
 * not _end -- scanning from _end stops at the first of those regions (the
 * fault record) and reports a bogus depth spanning arena+heap+stack. CH570
 * has none of that: the stack sits directly above _end, so it scans from _end. */
extern uint32_t _susrstack;
#define STACK_WM_FLOOR ((uintptr_t)&_susrstack)
#else
#define STACK_WM_FLOOR (((uintptr_t)&_end + 3u) & ~(uintptr_t)3u)
#endif

/* Address the scan measures depth from: the stack floor for this chip. */
static inline uint32_t stack_watermark_floor(void)
{
    return (uint32_t)STACK_WM_FLOOR;
}

/* Paint [floor, sp-64) -- from the true stack floor up to just below the live
 * frame, so the painter cannot eat its own stack. (On CH592 the region below
 * _susrstack is arena/heap/fault, not stack, and is left alone.) */
static inline void stack_watermark_paint(void)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    volatile uint32_t *p   = (volatile uint32_t *)STACK_WM_FLOOR;
    volatile uint32_t *top = (volatile uint32_t *)((sp - 64u) & ~(uintptr_t)3u);

    while (p < top) {
        *p++ = STACK_WATERMARK_PATTERN;
    }
}

/* Lowest address the stack has reached since the paint: the first word up
 * from the stack floor whose pattern is broken. Returns _eusrstack when the
 * paint never ran. */
static inline uint32_t stack_watermark_low(void)
{
    volatile const uint32_t *p = (volatile const uint32_t *)STACK_WM_FLOOR;
    volatile const uint32_t *top = (volatile const uint32_t *)&_eusrstack;

    while (p < top && *p == STACK_WATERMARK_PATTERN) {
        p++;
    }
    return (uint32_t)(uintptr_t)p;
}

#endif /* DONGLE_STACK_WATERMARK */

#endif /* STACK_WATERMARK_H */
