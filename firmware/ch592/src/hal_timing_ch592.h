/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CH592 timing-HAL seam — private declarations.
 *
 * CH59x implements hal_timing.h as a TASK-DISPATCH SHIM (see the relaxed
 * callback-context contract in hal_timing.h): the periodic TMR0 CYC_END ISR
 * posts a TMOS event and the slot work runs in TMOS task context, where the WCH
 * BLE-lib RF calls are safe. (Spike 2026-06-07 proved RF_Tx issued from the
 * TMR0 ISR breaks the BLE-lib link — CH59x RF cannot run from a timer ISR.)
 *
 * Through increment P2.3(ii-c): all four slots arm/cancel through the seam —
 * CONNECTED_POLL + PAIR_ACK drive the shared periodic TMR0 (state-multiplexed
 * by rf_state); EV10_REKEY + BOOT_WINDOW drive rf_task-provided TMOS deadline
 * primitives. The TMR0 state-dispatch ISR and the event dispatch to the slot
 * callbacks are still served by rf_task.c directly; they move behind the seam
 * in later increments.
 */
#ifndef HAL_TIMING_CH592_H
#define HAL_TIMING_CH592_H

#include <stdint.h>
#include "hal_timing.h"
#include "dongle_target.h"  /* HAL_TMOS_UNIT_TICKS for the unit conversion below */

/* The periodic TMR0 pacer primitive (defined in hal_timing_ch592.c since
 * P2.3 ii-e — the seam owns the timer hardware). One physical TMR0 is
 * state-multiplexed across PAIR_ACK / CONNECTED_POLL / the EV10 pair-ACK
 * cadence by rf_state. */
void rf_tmr0_start(uint32_t count);
void rf_tmr0_stop(void);

/* The protocol half of the TMR0 ISR (defined in rf_task.c): posts the TMOS
 * event matching the current rf_state. Called by the seam's TMR0_IRQHandler
 * shell in IRQ context; post-only (CH59x RF calls cannot run from a timer
 * ISR). */
void rf_tmr0_isr_dispatch(void);

/* TMOS deadline primitives (defined in rf_task.c) backing the EV10_REKEY and
 * BOOT_WINDOW slots. CH59x supervision/boot-window deadlines ride TMOS timers,
 * which the seam cannot reach directly (rf_taskID + RF_EVT_* are rf_task
 * internals). These take 625 us TMOS units; the seam converts from the
 * contract's Tsys ticks (P3a, HAL_TMOS_UNIT_TICKS). */
void rf_ev10_deadline_start(uint32_t tmos_ticks);
void rf_ev10_deadline_stop(void);
void rf_boot_window_deadline_start(uint32_t tmos_ticks);
void rf_boot_window_deadline_stop(void);

/* hal_timer_arm_periodic (hal_timing.h): the TMR0 backing the two cadence
 * slots is a hardware auto-reload (CYC_END) timer, so the plain arm IS the
 * periodic arm on CH59x — every property of the periodic contract (HAL-side
 * cadence, no per-fire re-arm, grid immune to dispatch latency, ownership
 * transfer without re-arm) is what TMR0 + the shared tmr0_armed latch already
 * provide. Macro alias keeps converted call sites byte-identical (the P1e
 * lesson: an out-of-line or inline-fn indirection perturbs GCC12 -Os codegen). */
#define hal_timer_arm_periodic(slot, period_ticks, cb) \
    hal_timer_arm((slot), (period_ticks), (cb))


/* Convert a Tsys-tick delta to whole 625 us TMOS units for the TMOS-backed
 * timers, rounding UP and flooring at 1 (a truncating divide would arm 0
 * units — TMOS never-fire/fire-immediately — for sub-unit deltas). Exact for
 * whole-unit deltas, so seam call sites passing CONST * HAL_TMOS_UNIT_TICKS
 * constant-fold back to the original TMOS value (byte-identical). */
static inline uint32_t hal_tmos_units_from_tsys(uint32_t delta_ticks)
{
    uint32_t units =
        (delta_ticks + HAL_TMOS_UNIT_TICKS - 1u) / HAL_TMOS_UNIT_TICKS;
    return units ? units : 1u;
}

/* hal_dispatch.h backing (CH59x): deferred events ride TMOS. Macros, not
 * functions, so converted call sites stay byte-identical (P1e lesson), and
 * because they need rf_taskID — an rf_task file-static — in scope at the
 * expansion site. Coalescing is contractual and native: tmos_set_event on an
 * already-set bit is one dispatch. */
#define hal_event_post(bits)  tmos_set_event(rf_taskID, (bits))
#define hal_event_cancel(bit) tmos_stop_task(rf_taskID, (bit))
#define hal_event_post_delayed(bit, delta_ticks) \
    tmos_start_task(rf_taskID, (bit), hal_tmos_units_from_tsys(delta_ticks))

/* TMOS-task-context dispatch entry (defined in hal_timing_ch592.c): invokes
 * the slot's registered callback if the slot is still armed. rf_task's
 * RF_EVT_POLL / RF_EVT_SEND_PAIR_ACK handlers call this so the seam's cb
 * table is the live dispatch path. Returns 1 if the callback ran. */
uint8_t hal_timing_ch592_dispatch(uint8_t slot);

/* Register a slot callback without arming/cancelling the timer. Used to
 * pre-register the fixed work callbacks at RF_TaskInit and to make the
 * inherited-TMR0 promote's PAIR_ACK->CONNECTED_POLL ownership transfer
 * explicit (no cancel/re-arm — the phase-lock invariant). */
void hal_timing_ch592_set_cb(uint8_t slot, hal_timer_cb_t cb);

#endif /* HAL_TIMING_CH592_H */
