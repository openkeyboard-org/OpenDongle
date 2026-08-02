/*
 * Bridge75 Open-Source Dongle Firmware
 * Dispatch HAL seam — deferred-event posting for the shared RF task.
 *
 * The RF task defers non-timing-critical work out of IRQ context (bond
 * persist, RX re-arm requests, pair-prep rotation, HID handoff, maintenance)
 * as EVENT BITS dispatched later in task/foreground context. The two chips
 * back this differently:
 *
 *   CH59x/CH58x: TMOS — hal_event_post is tmos_set_event(rf_taskID, bits),
 *     dispatch happens when TMOS_SystemProcess() runs the registered task.
 *     (The chip headers alias these symbols onto the tmos_* calls as macros,
 *     byte-identical at every converted call site — the P1e/P2a pattern.)
 *   CH570: a volatile pending mask — hal_event_post is an atomic OR; the
 *     foreground pump (RF_TaskPump) drains it each main-loop iteration.
 *     (Implementation lands with the P2d/P4 CH570 batch.)
 *
 * CONTRACT (behavioral, both backings):
 *
 *  - COALESCING: posting a bit that is already pending results in ONE
 *    dispatch. The seam is a bitmask, never a counted queue — this preserves
 *    the measured ~12% RF_EVT_POLL coalescing baseline (HANDOFF), which a
 *    counted queue would "fix" into double-polling.
 *  - Post context: hal_event_post/cancel are safe from any context (IRQ,
 *    timer callback, task).
 *  - TIMING: dispatch latency is bounded only by the backing loop (TMOS pass
 *    / one main-loop iteration) and, on the CH570 pump, can stretch to
 *    MILLISECONDS under EP6 IAP flash traffic. The rule is therefore about
 *    PRESERVING VALIDATED CONTEXT, not one fixed shape: no RF arm may move
 *    from the context its chip bench-validated onto this seam's slow path as
 *    part of the merge. Concretely — CH570's radio sink keeps its
 *    IRQ-synchronous pair-ACK scheduling and post-poll RX arm [Codex CX1];
 *    CH59x's sink keeps its validated mix (synchronous rf_start_rx at the
 *    promote sites, post-poll RX deferred via the TMOS pass it was always
 *    validated at [Codex CX2] — TMOS latency is not the pump's EP6-exposed
 *    latency). Cadence stays on hal_timing either way.
 *  - Delayed post: hal_event_post_delayed(bit, delta_ticks) fires the bit
 *    once after delta_ticks (Tsys, hal_timing.h domain). Re-posting a
 *    pending delayed bit restarts its deadline. Consumers are the pair-prep
 *    scan cadence and the pair-ACK burst gap (~tens of ms — pump latency is
 *    noise at that scale).
 *  - hal_event_cancel(bit) clears a pending (immediate or delayed) bit that
 *    has not yet dispatched.
 *
 * PUMP RE-ENTRANCY SPEC (the CH570 side; here because it is the contract the
 * shared handlers are written against) [Codex CX3]:
 *  - The pump snapshots-and-clears the pending mask under a brief IRQ-mask,
 *    then runs handlers with IRQs ON: handlers CAN be preempted by radio/
 *    timer IRQs that mutate RF state and post new bits (dispatched next
 *    pump pass, never lost).
 *  - Therefore every handler must re-validate its state on entry (stale-
 *    event guard) — the discipline the TMOS body already has, kept by the
 *    shared rf_task_process_events.
 */
#ifndef HAL_DISPATCH_H
#define HAL_DISPATCH_H

#include <stdint.h>

/* The chip headers may provide these as macros (CH59x: tmos_* aliases that
 * require rf_task scope); the declarations below cover implementations that
 * provide real functions (CH570). */
#ifndef hal_event_post
void hal_event_post(uint16_t evt_bits);
#endif
#ifndef hal_event_post_delayed
void hal_event_post_delayed(uint16_t evt_bit, uint32_t delta_ticks);
#endif
#ifndef hal_event_cancel
void hal_event_cancel(uint16_t evt_bit);
#endif

#endif /* HAL_DISPATCH_H */
