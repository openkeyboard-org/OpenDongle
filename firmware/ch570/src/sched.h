#ifndef CH570_SCHED_H
#define CH570_SCHED_H

#include <stdint.h>

/*
 * Software-deadline timer mux over the single CH57x HSE TMR.
 *
 * N independently-active soft timers ("slots") carry absolute deadlines on a
 * 32-bit software epoch (Tsys ticks). The single hardware TMR is armed one-shot
 * to the NEAREST active deadline; the TMR IRQ dispatches every expired slot in
 * slot/priority order (lower index == higher priority), re-checking active after
 * each callback. st_set/st_cancel are safe from any context (they sync the epoch
 * under a brief IRQ-masked critical section before mutating); expired callbacks
 * run ONLY from TMR_IRQHandler (st_dispatch), so they always execute in IRQ
 * context. Same-slot st_set/st_cancel after an undispatched expiry intentionally
 * suppresses the stale callback (restart/cancel semantics). Implementation,
 * semantics, and the S1-S8 hardware proof live in main.c / HANDOFF.md.
 */
#define ST_N            6u      /* slots; index == dispatch priority.
                                 * 0..3 = the hal_timing.h named slots;
                                 * 4..5 = hal_dispatch delayed-event posts
                                 * (pair-prep cadence, pair-ACK burst gap) —
                                 * lowest priority by construction. */
#define ST_SLOT_EVT_DELAY_0 4u
#define ST_SLOT_EVT_DELAY_1 5u
#define ST_MIN_DELTA    20u     /* arm floor (avoid 0 / past CNT_END) */

typedef void (*st_cb_t)(uint8_t slot);

/* Arm slot to fire `delta` Tsys ticks from now, calling cb(slot) from the TMR
 * IRQ. Re-setting an active slot restarts it (old deadline/callback dropped). */
void st_set(uint8_t slot, uint32_t delta, st_cb_t cb);

/* Like st_set, but arm to an ABSOLUTE software-epoch deadline (st_now() domain)
 * instead of now+delta. Advancing the deadline by a fixed period each fire holds
 * a phase-stable cadence immune to per-call dispatch latency. Same restart
 * semantics. */
void st_set_at(uint8_t slot, uint32_t deadline, st_cb_t cb);

/* Deactivate slot. A same-slot expiry that has not yet dispatched is suppressed. */
void st_cancel(uint8_t slot);

/* Current software epoch in Tsys ticks (exposed for hal_timing_ch570.c). */
uint32_t st_now(void);

/*
 * Hardware auto-reload periodic mode (the CH592 TMR0 phase-lock mirror).
 *
 * st_set_periodic(slot, period, cb) hands the ONE hardware TMR to `slot` as a
 * true auto-reload periodic: CNT_END = period, ALL_CLEAR at the call = the
 * grid's PHASE ANCHOR (call it immediately before the anchoring RF TX, the
 * CH592 rf_tmr0_start-before-RF_Tx shape). No per-fire software re-arm — the
 * hardware reload keeps the grid, so dispatch latency and IRQ-masked windows
 * never accumulate into it (SysTick accounts missed periods; the cb fires at
 * most ONCE per ISR entry, never a catch-up burst). Re-calling on the owning
 * slot RE-ANCHORS (stock event-0x40 shape). Only one slot may be periodic.
 *
 * st_periodic_transfer(from, to, cb) is the PROMOTE: move grid ownership
 * from->to and install `cb`, with ZERO hardware writes (the phase-lock
 * INVARIANT — a re-arm would ALL_CLEAR and destroy the TX anchor). Returns 0
 * if `from` does not own the grid (caller arms fresh as fallback). from == to
 * is legal and just swaps the callback.
 *
 * WHILE A SLOT IS PERIODIC (the coexistence contract):
 *  - st_now() is SysTick-backed: continuous, never freezes, same 100 MHz
 *    clock tree as the TMR (one frequency domain).
 *  - The OTHER slots' st_set/st_set_at/st_cancel are bookkeeping-only (no
 *    hardware touch, no epoch rebase — cheap at any call rate); their
 *    callbacks dispatch from the periodic ISR at <=1-period granularity:
 *    "fires not earlier than the deadline, on the next grid tick". Fine for
 *    the seconds-scale supervision watchdog; do NOT give a sub-period
 *    one-shot deadline to a slot while the grid runs and expect precision.
 *  - st_set/st_set_at ON the owning slot exit periodic mode first (claim-
 *    from-grid), then arm one-shot; st_cancel on it exits back to the
 *    one-shot mux with full precision for the remaining slots.
 */
void st_set_periodic(uint8_t slot, uint32_t period, st_cb_t cb);
uint8_t st_periodic_transfer(uint8_t from, uint8_t to, st_cb_t cb);

/* Phase acquisition: stretch the CURRENT grid period by `delta` ticks once
 * (the grid phase shifts +delta permanently, accounting exact). Positive
 * delta; call right after a fire (e.g. from the grid owner's own callback).
 * No-op if not periodic, a stretch is pending, or the cycle is past its
 * midpoint. See the v0.96 drift-as-accidental-sweep note in main.c. */
uint8_t st_periodic_nudge(uint32_t delta);   /* 1 = stretch accepted */
#endif /* CH570_SCHED_H */
