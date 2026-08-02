/*
 * Bridge75 Open-Source Dongle Firmware
 * Timing HAL seam — the RF deadline scheduler.
 *
 * The shared RF task schedules all of its time-based work through this seam so
 * one rf_task.c serves every SoC. CH570 implements it directly over its
 * HSE-TMR software-deadline mux (the proven sched.h `st_*` core); CH59x/CH58x
 * implement it as a compatibility shim over their existing TMR0 + RTC32K + TMOS
 * machinery until that family is converged onto the same HSE-TMR mux.
 *
 * CONTRACT (all implementations MUST honor this — it is a behavioral interface,
 * not just a set of signatures):
 *
 *  - Slots: there are HAL_TMR_SLOT_COUNT independently-active one-shot timers,
 *    indexed 0..N-1. The index IS the dispatch priority: on simultaneous expiry,
 *    lower index fires first.
 *  - Callback context: PLATFORM-DEFINED. A slot's callback runs either in
 *    timer-IRQ context (CH570's true HSE-TMR mux) or in TMOS task context
 *    (CH59x/CH58x, via a task-dispatch shim: the timer ISR posts a TMOS event and
 *    the callback runs from the task loop). This was relaxed from IRQ-only after
 *    the 2026-06-07 spike proved the CH59x BLE-lib RF calls (RF_Tx/RF_Shut/...)
 *    CANNOT run from a timer ISR — they break the link. The shared rf_task is
 *    therefore written to the IRQ-safe subset that is correct under EITHER
 *    context: it must not assume a callback preempts task code, and must keep
 *    callbacks short (the radio work is fine in task context but must not block).
 *  - One-shot: a slot fires at most once per arm. Periodic cadence is achieved by
 *    re-arming the slot from inside its own callback.
 *  - Same-slot restart: hal_timer_arm() on an already-active slot REPLACES its
 *    deadline+callback; any pending-but-not-yet-dispatched expiry of that slot is
 *    SUPPRESSED (no stale callback). This is the documented sched.h S8 semantics.
 *  - Cancellation: hal_timer_cancel() deactivates a slot; a not-yet-dispatched
 *    expiry is suppressed. Safe to cancel an inactive slot.
 *  - Call sites: hal_timer_arm()/cancel() are safe from any context (they sync
 *    the epoch under a brief IRQ-masked critical section before mutating).
 *  - Clock domain: deltas and hal_now() are in Tsys ticks (the CPU/HSE clock
 *    rate). rf_task converts microseconds/intervals to ticks via the per-chip
 *    constants in <chip>/target.h (e.g. HAL_TICKS_PER_US). Ticks are NOT the
 *    32.768 kHz RTC domain.
 */
#ifndef HAL_TIMING_H
#define HAL_TIMING_H

#include <stdint.h>

/* Named slots — identical across chips so the shared rf_task is chip-agnostic.
 * Index == dispatch priority (lower fires first on simultaneous expiry). */
enum {
    HAL_TMR_SLOT_PAIR_ACK = 0,
    HAL_TMR_SLOT_EV10_REKEY = 1,
    HAL_TMR_SLOT_BOOT_WINDOW = 2,
    HAL_TMR_SLOT_CONNECTED_POLL = 3,
    HAL_TMR_SLOT_COUNT = 4,
};

/* Arm floor: deltas below this are clamped up to avoid arming 0 / a past
 * deadline (matches sched.h ST_MIN_DELTA). */
#define HAL_TMR_MIN_DELTA 20u

typedef void (*hal_timer_cb_t)(uint8_t slot);

/* Monotonic software epoch in Tsys ticks. Wraps at 2^32; callers use modular
 * (wrap-safe) subtraction for deltas. */
uint32_t hal_now(void);

/* Arm `slot` to fire `delta_ticks` from now, invoking cb(slot) in the
 * PLATFORM-DEFINED callback context (see the contract above: timer-IRQ on
 * CH570; TMOS task via the posted-event shim on CH59x). Re-arming an active
 * slot restarts it (old deadline/callback dropped, stale expiry suppressed). */
void hal_timer_arm(uint8_t slot, uint32_t delta_ticks, hal_timer_cb_t cb);

/* Arm `slot` as a PERIODIC cadence: cb(slot) fires every `period_ticks` on a
 * phase-stable grid anchored at the arm instant (first fire one period from
 * now). CONTRACT (the connected-poll grid both chips bench-validated):
 *  - The implementation maintains the cadence itself — the callback must NOT
 *    re-arm per fire, and per-fire dispatch latency must not accumulate into
 *    the grid (CH59x: hardware auto-reload TMR0; CH570: HAL-side absolute-
 *    deadline chaining over the st_* mux).
 *  - Missed periods are SKIPPED to the next future grid point — never a
 *    catch-up burst (e.g. after a flash op masked IRQs).
 *  - Same-slot restart (either arm form) re-anchors the grid; cancel stops it.
 *  - OWNERSHIP TRANSFER (CH59x phase-lock invariant): a state transition may
 *    take over a running cadence by swapping the dispatch callback only —
 *    the implementation must never require a cancel/re-arm across such a
 *    transition (the promote-to-CONNECTED inherits the running pair-ACK
 *    cadence this way).
 *  - Currently defined for the poll/pair-ACK cadence slots (PAIR_ACK,
 *    CONNECTED_POLL); the deadline slots (EV10_REKEY, BOOT_WINDOW) are
 *    one-shot by nature and use hal_timer_arm.
 * NOTE (CH59x transitional): on CH59x the TMR0 backing both cadence slots is
 * inherently periodic, so hal_timer_arm on those slots already behaves as
 * arm_periodic — its chip header aliases this symbol onto hal_timer_arm
 * (byte-identical by construction). The distinct name exists so the shared
 * rf_task states its intent and CH570 can back it with deadline chaining. */
void hal_timer_arm_periodic(uint8_t slot, uint32_t period_ticks,
                            hal_timer_cb_t cb);

/* Deactivate `slot`; a not-yet-dispatched expiry is suppressed. */
void hal_timer_cancel(uint8_t slot);

#endif /* HAL_TIMING_H */
