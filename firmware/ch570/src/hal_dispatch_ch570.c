/*
 * OpenKeyboard.org OpenDongle — CH570 hal_dispatch backing.
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Deferred events are a volatile pending bitmask drained by RF_TaskPump()
 * once per main-loop pass (hal_dispatch.h contract: coalescing bitmask,
 * never a counted queue; nothing timing-critical rides this seam — the
 * radio sink's CONNECTED-state arms are IRQ-synchronous per CX1).
 *
 * Delayed posts ride two dedicated low-priority st_* slots (sched.h
 * ST_SLOT_EVT_DELAY_*): the deadline callback just sets the pending bit in
 * TMR-IRQ context; the pump dispatches it in the foreground. Re-posting a
 * pending delayed bit restarts its deadline (st_set same-slot restart).
 */
#include <stdint.h>
#include "hal_dispatch.h"
#include "sched.h"
#include "CH57x_common.h"   /* __HIGH_CODE / __risc_v_*_irq */
#include "rf_task.h"        /* RF_EVT_* vocabulary for the delayed-slot map */

static volatile uint16_t hal_evt_pending;

/* Fixed bit->slot map for the two delayed-post consumers. */
static uint16_t hal_evt_delay_bit[2];

__HIGH_CODE
void hal_event_post(uint16_t evt_bits)
{
    uint32_t irq = __risc_v_disable_irq();
    hal_evt_pending |= evt_bits;
    (void)__risc_v_enable_irq(irq);
}

__HIGH_CODE
static void hal_evt_delay_cb(uint8_t slot)
{
    uint8_t idx = (uint8_t)(slot - ST_SLOT_EVT_DELAY_0);

    hal_event_post(hal_evt_delay_bit[idx]);
    hal_evt_delay_bit[idx] = 0u;
}

void hal_event_post_delayed(uint16_t evt_bit, uint32_t delta_ticks)
{
    uint32_t irq = __risc_v_disable_irq();
    uint8_t idx;

    /* Re-posting a bit already scheduled restarts its deadline; otherwise
     * take the first free delay slot. Delayed posters: pair-prep cadence,
     * pair-ACK burst gap, and the P4 deaf-camp guard's RX_RESTART retry — up
     * to three bits contend for two slots, so exhaustion is possible. On
     * exhaustion the post degrades to immediate (paced by the pump) rather
     * than being dropped: the deadline is lost but the event is not. The P4
     * guard tolerates this (see RF_ARM_RETRY_TMOS in dongle_target.h);
     * the terminal camps that depend on it free both slots first. */
    for (idx = 0u; idx < 2u; idx++) {
        if (hal_evt_delay_bit[idx] == evt_bit) {
            break;
        }
    }
    if (idx == 2u) {
        for (idx = 0u; idx < 2u; idx++) {
            if (hal_evt_delay_bit[idx] == 0u) {
                break;
            }
        }
    }
    if (idx == 2u) {
        hal_evt_pending |= evt_bit;   /* degraded: no free slot */
        (void)__risc_v_enable_irq(irq);
        return;
    }
    hal_evt_delay_bit[idx] = evt_bit;
    st_set((uint8_t)(ST_SLOT_EVT_DELAY_0 + idx), delta_ticks, hal_evt_delay_cb);
    (void)__risc_v_enable_irq(irq);
}

void hal_event_cancel(uint16_t evt_bit)
{
    uint32_t irq = __risc_v_disable_irq();
    uint8_t idx;

    hal_evt_pending &= (uint16_t)~evt_bit;
    for (idx = 0u; idx < 2u; idx++) {
        if (hal_evt_delay_bit[idx] == evt_bit) {
            hal_evt_delay_bit[idx] = 0u;
            st_cancel((uint8_t)(ST_SLOT_EVT_DELAY_0 + idx));
        }
    }
    (void)__risc_v_enable_irq(irq);
}

/* Snapshot-and-clear for RF_TaskPump (hal_dispatch.h pump contract). */
__HIGH_CODE
uint16_t hal_dispatch_ch570_drain(void)
{
    uint32_t irq = __risc_v_disable_irq();
    uint16_t ev = hal_evt_pending;

    hal_evt_pending = 0u;
    (void)__risc_v_enable_irq(irq);
    return ev;
}
