/*
 * Bridge75 Open-Source Dongle Firmware
 * CH592 timing-HAL seam (hal_timing.h implementation) — task-dispatch shim.
 *
 * Unlike CH570's true HSE-TMR IRQ mux, CH59x cannot issue RF calls from a timer
 * ISR (spike 2026-06-07), so dispatch stays in TMOS task context: the periodic
 * TMR0 CYC_END ISR in rf_task.c posts RF_EVT_POLL and rf_send_poll() runs from
 * the task loop. This seam owns the per-slot bookkeeping and the arm/cancel of
 * the underlying timer primitive.
 *
 * Scope through increment P2.3(ii-c): ALL FOUR slots arm/cancel through the
 * seam — CONNECTED_POLL + PAIR_ACK on the one state-multiplexed TMR0,
 * EV10_REKEY + BOOT_WINDOW on rf_task-provided TMOS deadline primitives
 * (transitional 625 us TMOS units until P3a — see hal_timing_ch592.h).
 * Still in rf_task.c: the TMR0 primitive + its state-dispatch ISR, and the
 * native event dispatch (the stored slot cbs are not yet invoked via the
 * seam); both move here in the remaining P2.3(ii) increments.
 */
#include "hal_timing_ch592.h"
#include "dongle_target.h"   /* HAL_TMOS_UNIT_TICKS / HAL_TICKS_PER_US (P3a) */
#include "CH59x_common.h"   /* __HIGH_CODE — keep the arm/cancel wrappers in SRAM
                             * like the rf_tmr0_* primitives they front, so the
                             * seam adds no flash-XIP latency to the phase-locked
                             * pair-ACK / poll arm paths (codex, P2.3 ii-b). */

/* Per-slot callback table, set at arm time, dispatched in TMOS task context via
 * hal_timing_ch592_dispatch(). volatile: arms run from the radio-IRQ status
 * callback as well as task context, dispatch reads from task context; a slot's
 * cb is a single aligned word (atomic on RV32), and the arm/dispatch pairing is
 * ordered by the TMOS event post that follows the arm. */
static hal_timer_cb_t volatile slot_cb[HAL_TMR_SLOT_COUNT];

/* Armed latch for the TMR0-backed slot pair (PAIR_ACK + CONNECTED_POLL share
 * the one state-multiplexed TMR0, so they share one latch — which is exactly
 * what lets the inherited promote transfer ownership without re-arming). Set
 * on arm, cleared on cancel; a POLL/SEND_PAIR_ACK event posted by the TMR0 ISR
 * before a cancel is suppressed at dispatch by this gate (the protocol-level
 * rf_state guards in rf_task.c remain as the second line of defense). The
 * ISR-internal defensive rf_tmr0_stop() paths cannot clear the latch; that is
 * benign (the timer is stopped, so no further events post, and the next arm
 * re-sets the latch).
 *
 * EV10_REKEY / BOOT_WINDOW need no seam gate: their cancel path
 * (tmos_stop_task) also drains an already-posted TMOS event bit, and their
 * handlers keep protocol-level staleness guards (rf_boot_window_active, the
 * supervision state checks). */
static volatile uint8_t tmr0_armed;

/* ---- The TMR0 hardware primitive + CYC_END ISR shell (P2.3 ii-e) ----
 * Moved here from rf_task.c: the seam owns the timer hardware; rf_task owns
 * the protocol decision (rf_tmr0_isr_dispatch's rf_state switch, called from
 * the ISR shell below in IRQ context, post-only). */

/* Start/restart TMR0 as a periodic CYC_END timer with the given Tsys
 * count. Matches the stock helper at firmware.bin VA 0x5622 — count
 * write + ALL_CLEAR + COUNT_EN, no prescaler — plus the MCU-side
 * interrupt-enable glue (clear IF, set INTER_EN, enable PFIC line).
 * Called once per period change, not per fire (the counter auto-
 * reloads at CYC_END). */
__HIGH_CODE
void rf_tmr0_start(uint32_t count)
{
    PFIC_DisableIRQ(TMR0_IRQn);
    R8_TMR0_INTER_EN = 0;
    R32_TMR0_CNT_END = count;
    R8_TMR0_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;   /* W1C any latched flag */
    R8_TMR0_INTER_EN = RB_TMR_IF_CYC_END;
    R8_TMR0_CTRL_MOD = RB_TMR_COUNT_EN;
    PFIC_EnableIRQ(TMR0_IRQn);
}

__HIGH_CODE
void rf_tmr0_stop(void)
{
    PFIC_DisableIRQ(TMR0_IRQn);
    R8_TMR0_CTRL_MOD = 0;                   /* stop counter */
    R8_TMR0_INTER_EN = 0;                   /* disable CYC_END IT */
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;   /* clear pending */
}

/* TMR0 CYC_END ISR shell. Clears the flag and hands the state-multiplex
 * decision to rf_task (rf_tmr0_isr_dispatch posts the TMOS event matching
 * rf_state — post-only, the heavy lifting runs in task context, mirroring the
 * stock pattern where TMR0 is the sole pacer for both connected polling
 * (event 0x02) and pair-completion TX (event 0x40)). */
__INTERRUPT
__HIGH_CODE
void TMR0_IRQHandler(void)
{
    if (!(R8_TMR0_INT_FLAG & RB_TMR_IF_CYC_END)) {
        return;
    }
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;   /* W1C */
    rf_tmr0_isr_dispatch();
}

/* Monotonic Tsys tick from the QingKe SysTick free-running at HCLK == Tsys
 * (60 MHz). In the product build the SDK already configured it exactly this
 * way (CH59x_BLEInit -> SysTick_Config: up-count, STCLK=HCLK, CMP~=2^64,
 * SysTick IRQ masked; srandCB reads CNT once at init -- codex P3a #1), so this
 * normally just reads the running counter; the lazy start below only covers a
 * build that skips CH59x_BLEInit. The double-init race is benign (identical
 * programming). Wraps at 2^32 (~71.6 s) per the hal_timing.h contract --
 * callers compute modular deltas. This is also the hop-anchor clock: rf_task
 * derives its protocol-tick hop counter from hal_now() (P3b; the RTC32K
 * fallback was retired in unify-rf-task P1b). RTC32K remains only as the
 * supervision timebase. */
__HIGH_CODE
uint32_t hal_now(void)
{
    if ((SysTick->CTLR & SysTick_CTLR_STE) == 0) {
        SysTick->CMP  = SysTick_LOAD_RELOAD_Msk;     /* free-run, no compare */
        SysTick->CNT  = 0;
        SysTick->CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK;  /* HCLK, up */
    }
    /* Low word only: a 64-bit read is two loads on RV32 (torn across the
     * carry); the contract is a 32-bit wrapping tick anyway. */
    return *(volatile uint32_t *)&SysTick->CNT;
}


__HIGH_CODE
void hal_timer_arm(uint8_t slot, uint32_t delta_ticks, hal_timer_cb_t cb)
{
    if (slot >= HAL_TMR_SLOT_COUNT) {
        return;
    }

    if (delta_ticks < HAL_TMR_MIN_DELTA) {
        delta_ticks = HAL_TMR_MIN_DELTA;
    }

    switch (slot) {
    case HAL_TMR_SLOT_PAIR_ACK:
    case HAL_TMR_SLOT_CONNECTED_POLL:
        /* ONE physical TMR0 backs both slots, state-multiplexed by rf_state in
         * rf_task.c's rf_tmr0_isr_dispatch (PENDING_ACK→SEND_PAIR_ACK,
         * CONNECTED→POLL; called from this file's ISR shell) — so PAIR_ACK and CONNECTED_POLL are mutually
         * exclusive by construction, and arming either slot programs the same
         * periodic CYC_END timer. delta_ticks is already in Tsys (the 300 µs
         * pair bootstrap, conn_interval * RF_TMR0_INTERVAL_MUL, or an EV10
         * phase-align count). rf_tmr0_start clears any latched CYC_END flag
         * under a masked PFIC line, satisfying the contract's same-slot
         * stale-expiry suppression.
         *
         * INVARIANT (phase-lock): the promote-to-CONNECTED at the 15-byte
         * pair-completion TX_FINISH inherits the running PAIR_ACK arm as the
         * CONNECTED_POLL cadence by OWNERSHIP TRANSFER — rf_task.c flips
         * rf_state without calling cancel/arm, and this seam must never
         * insert a re-arm into that transition (the shared tmr0_armed latch
         * survives the transfer untouched). */
        /* Arm/cancel of this shared TMR0 runs from BOTH RF_ProcessEvent (TMOS
         * task) AND rf_phy_event_sink, which the CH59x BLE lib can run as an
         * IRQ-tail deferred controller callback (TMOS_SysRegister/tmosSign ECALL
         * trampoline) that PREEMPTS the task -- so the publish is not naturally
         * serialized. Mask interrupts across the whole publish (callback word +
         * armed latch + the multi-write rf_tmr0_start register sequence) so a
         * preempting arm/cancel cannot observe a half-updated timer and dispatch
         * a stale/mismatched poll or pair-ACK. Save/restore form; this slot arms
         * only during active pairing/connection (never at boot), so it does not
         * gate USB enumeration. Honors the hal_timing.h "safe from any context"
         * contract. */
        {
            uint32_t irq = __risc_v_disable_irq();
            slot_cb[slot] = cb;
            tmr0_armed = 1;
            rf_tmr0_start(delta_ticks);
            (void)__risc_v_enable_irq(irq);
        }
        break;
    case HAL_TMR_SLOT_EV10_REKEY:
        /* CH59x supervision/EV10 deadline rides a TMOS timer (RF_EVT_TIMEOUT).
         * P3a: delta arrives in Tsys ticks per the hal_timing.h contract and
         * is converted back to the 625 us TMOS units the backing timer wants
         * (round-up, floor 1 -- see hal_tmos_units_from_tsys).
         * tmos_start_task restarting an already-armed timer replaces its
         * deadline, matching the same-slot-restart contract. */
        slot_cb[slot] = cb;   /* TMOS-backed: no shared-TMR0 race, no critsec */
        rf_ev10_deadline_start(hal_tmos_units_from_tsys(delta_ticks));
        break;
    case HAL_TMR_SLOT_BOOT_WINDOW:
        /* Boot reconnect/pair window tick (RF_EVT_BOOT_WINDOW), same TMOS
         * backing and Tsys->TMOS-unit conversion as EV10_REKEY. */
        slot_cb[slot] = cb;   /* TMOS-backed: no shared-TMR0 race, no critsec */
        rf_boot_window_deadline_start(hal_tmos_units_from_tsys(delta_ticks));
        break;
    default:
        break;
    }
}

__HIGH_CODE
void hal_timer_cancel(uint8_t slot)
{
    if (slot >= HAL_TMR_SLOT_COUNT) {
        return;
    }

    switch (slot) {
    case HAL_TMR_SLOT_PAIR_ACK:
    case HAL_TMR_SLOT_CONNECTED_POLL:
        /* Shared TMR0: cancelling either slot stops the one physical timer.
         * Safe because the slots are mutually exclusive by rf_state (see
         * hal_timer_arm); rf_task.c's stop sites only fire for the state that
         * currently owns the timer. Clearing the latch suppresses any
         * POLL/SEND_PAIR_ACK event the ISR posted before this cancel. */
        /* Same dual-context (task + IRQ-tail BLE callback) concern as
         * hal_timer_arm: mask interrupts across the latch clear + the multi-write
         * rf_tmr0_stop sequence. */
        {
            uint32_t irq = __risc_v_disable_irq();
            tmr0_armed = 0;
            rf_tmr0_stop();
            (void)__risc_v_enable_irq(irq);
        }
        break;
    case HAL_TMR_SLOT_EV10_REKEY:
        rf_ev10_deadline_stop();
        break;
    case HAL_TMR_SLOT_BOOT_WINDOW:
        rf_boot_window_deadline_stop();
        break;
    default:
        break;
    }
}

/* TMOS-task-context dispatch entry (P2.3 ii-d): rf_task's RF_EVT_POLL /
 * RF_EVT_SEND_PAIR_ACK handlers route here instead of calling the work
 * functions directly, so the slot_cb table is the live dispatch path the
 * shared rf_task.c (P2.5) will rely on. Returns 1 if a registered, still-armed
 * callback ran. */
/* Register a slot callback WITHOUT touching the timer (P2.3 ii-d, codex fix).
 * Two uses: (1) RF_TaskInit pre-registers the fixed CH592 work callbacks for
 * the TMR0-backed pair, so event posts that arrive without a preceding
 * hal_timer_arm on that slot (the inherited-promote ownership transfer, the
 * PENDING_ACK pair-retry tmos_set_event) can never hit a null cb; (2) the
 * inherited promote calls it explicitly to make the PAIR_ACK->CONNECTED_POLL
 * ownership transfer visible at the seam without a cancel/re-arm. */
void hal_timing_ch592_set_cb(uint8_t slot, hal_timer_cb_t cb)
{
    if (slot < HAL_TMR_SLOT_COUNT) {
        slot_cb[slot] = cb;
    }
}

uint8_t hal_timing_ch592_dispatch(uint8_t slot)
{
    hal_timer_cb_t cb;

    if (slot >= HAL_TMR_SLOT_COUNT) {
        return 0;
    }
    switch (slot) {
    case HAL_TMR_SLOT_PAIR_ACK:
    case HAL_TMR_SLOT_CONNECTED_POLL:
        if (!tmr0_armed) {
            return 0;   /* cancelled after the TMR0 ISR posted the event */
        }
        break;
    default:
        break;
    }
    cb = slot_cb[slot];
    if (cb == 0) {
        return 0;
    }
    cb(slot);
    return 1;
}
