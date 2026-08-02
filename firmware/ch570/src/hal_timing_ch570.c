/*
 * Bridge75 Open-Source Dongle Firmware -- CH570 timing HAL.
 *
 * Implements hal_timing.h directly over the CH570 HSE-TMR software-deadline mux
 * (sched.h `st_*`, implemented in main.c). This is the convergence target: the
 * shared rf_task schedules all deadlines through hal_timer_*, and on CH570 those
 * map 1:1 onto the proven S1-S8 mux. hal_timer_cb_t and st_cb_t are the same
 * `void(*)(uint8_t)` signature; the named HAL_TMR_SLOT_* indices match the slot
 * numbering the mux dispatches by (index == priority).
 */
#include "hal_timing.h"
#include "hal_timing_ch592.h"   /* the CH59x-named side entries stubbed below */
#include "sched.h"
#include "CH57x_common.h"        /* __HIGH_CODE / __risc_v_*_irq */

uint32_t hal_now(void)
{
    /* st_now() reads {CYC_END flag, st_epoch, st_armed_delta, R32_TMR_COUNT}
     * non-atomically and its contract requires the TMR IRQ masked (main.c).
     * The st_* mutators all honor that; hal_now() is the one seam entry that
     * did not, so a task-context caller (the RF_EVT_TIMEOUT supervision-lapse
     * legs run from the polled pump, IRQ-unmasked) could be preempted by the
     * connected-poll TMR IRQ mid-read and read a torn value — a backward jump
     * of ~one poll interval underflows idle_tsys past the supervision
     * threshold and forces a spurious reacquire. Mask here so every hal_now()
     * caller, task or IRQ, is safe (redundant in radio/TMR-IRQ context, where
     * the co-priority IRQ already can't preempt — harmless). CH570-local. */
    uint32_t irq = __risc_v_disable_irq();
    uint32_t now = st_now();
    (void)__risc_v_enable_irq(irq);
    return now;
}

/* Enable the software-deadline mux's backing TMR IRQ line (P4). The legacy
 * CH570 rf_task did this from its own init; the shared rf_task drives all
 * timing through this seam, so the seam owns bringing the mux to life. Same
 * priority as the RF radio IRQs (0x80 level via PFIC pri 1) so the poll grid
 * is never delayed by a USB ISR. Without this the st slots arm but never
 * dispatch (the P4 burst-gap stall). */
void hal_timing_ch570_mux_init(void)
{
    PFIC_SetPriority(TMR_IRQn, 1);
    PFIC_EnableIRQ(TMR_IRQn);
}


/* ---- Periodic cadence backing: the HARDWARE auto-reload grid ----
 * hal_timer_arm_periodic maps onto st_set_periodic (main.c): the one TMR
 * auto-reloads at the period with the ALL_CLEAR at the arm = the phase
 * anchor — every arm_periodic caller in rf_task.c sits immediately before
 * its anchoring RF TX (pair-ACK bootstrap/steady, burst#1), so the grid is
 * TX-anchored by construction, the CH592 rf_tmr0_start-before-RF_Tx shape.
 * hal_timer_promote_periodic is the promote: grid ownership transfers with
 * ZERO hardware writes (st_periodic_transfer); on a cold path (nothing
 * periodic) it falls back to a fresh anchor-at-now arm so the link still
 * polls. The software chaining trampoline below (#else) is the legacy
 * knob=0 backing — the measured +3380 ppm re-arm-churn grid
 * (analysis/ch570-hw-poll-grid/baseline.md). */

void hal_timer_arm_periodic(uint8_t slot, uint32_t period_ticks,
                            hal_timer_cb_t cb)
{
    if (slot >= HAL_TMR_SLOT_COUNT) {
        return;
    }
    if (period_ticks < HAL_TMR_MIN_DELTA) {
        period_ticks = HAL_TMR_MIN_DELTA;
    }
    st_set_periodic(slot, period_ticks, cb);
}

/* Phase-acquisition nudge passthrough (sched.h st_periodic_nudge): rf_task
 * stays on hal_* seam calls only. */
uint8_t hal_timer_grid_nudge(uint32_t ticks)
{
    return st_periodic_nudge(ticks);
}

void hal_timer_promote_periodic(uint8_t from_slot, uint8_t to_slot,
                                uint32_t fallback_period,
                                hal_timer_cb_t cb)
{
    hal_timer_cb_t eff = cb;

    if (from_slot >= HAL_TMR_SLOT_COUNT || to_slot >= HAL_TMR_SLOT_COUNT) {
        return;
    }
    if (!st_periodic_transfer(from_slot, to_slot, eff)) {
        /* Nothing periodic to inherit (cold path): fresh anchor-at-now arm
         * so the link still polls — the phase then relies on the keyboard's
         * servo capture, as before this change. */
        hal_timer_arm_periodic(to_slot, fallback_period, cb);
    }
}


/* ---- CH59x-named side entries (registration-only on this executor) ----
 * The shared body pre-registers slot work callbacks and, on the TMOS shim,
 * dispatches posted slot events through the seam table. On CH570 the mux
 * invokes callbacks directly in the TMR IRQ, so registration is bookkeeping
 * and the event-path dispatch can only see a stale post — honor the same
 * contract (invoke the registered cb if the slot is live) for safety. */
static hal_timer_cb_t volatile hal_slot_cb_shadow[HAL_TMR_SLOT_COUNT];

void hal_timing_ch592_set_cb(uint8_t slot, hal_timer_cb_t cb)
{
    if (slot < HAL_TMR_SLOT_COUNT) {
        hal_slot_cb_shadow[slot] = cb;
    }
}

uint8_t hal_timing_ch592_dispatch(uint8_t slot)
{
    hal_timer_cb_t cb;

    if (slot >= HAL_TMR_SLOT_COUNT) {
        return 0u;
    }
    cb = hal_slot_cb_shadow[slot];
    if (cb == 0) {
        return 0u;
    }
    cb(slot);
    return 1u;
}

void hal_timer_arm(uint8_t slot, uint32_t delta_ticks, hal_timer_cb_t cb)
{
    uint32_t irq = __risc_v_disable_irq();
    /* Grid mode: st_set itself claims the slot from the hardware grid when
     * the slot owns it (claim-from-grid, proof S15); other slots' one-shots
     * are bookkeeping-only while the grid runs (dispatched from the periodic
     * ISR at <=1-period granularity — the sched.h coexistence contract). */
    st_set(slot, delta_ticks, cb);
    (void)__risc_v_enable_irq(irq);
}

void hal_timer_cancel(uint8_t slot)
{
    uint32_t irq = __risc_v_disable_irq();
    st_cancel(slot);
    (void)__risc_v_enable_irq(irq);
}
