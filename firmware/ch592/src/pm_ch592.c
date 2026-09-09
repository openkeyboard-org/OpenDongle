/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * CH592 Tier-1 power management: main-loop idle, heartbeat, clock gates, park.
 *
 * WHAT IDLES: the main loop only. The radio is never slept, the protocol bytes and
 * timer settings are unchanged (bench: poll cadence within 1 % of baseline; idle
 * adds up to one heartbeat of wake latency plus the foreground and scheduler passes
 * a timer always ran behind), the flash stays powered (no FLASH_ROM_SW_RESET: a ROM
 * call that masks every IRQ, and the first XIP fetch after each of ~1000
 * wakes/s would add re-enable jitter to the 875 us poll grid), and the DC-DC is
 * never enabled (VSW is tied to VDCID, there is no inductor).
 *
 * WHY SEVONPEND-WFE, NOT A MASKED WFI: on this silicon a plain WFI entered with
 * CSR 0x800 (MPIE|MIE) cleared never wakes on a pending source (OpenController
 * bench: it slept to the watchdog). The working form is WFITOWFE (SCTLR bit 3)
 * + SEVONPEND (bit 4): an interrupt GOING pending is a wake event even while
 * global delivery stays masked. SEVONPEND latches only a NEW pending edge, so a
 * source already pending when it is armed makes no event. Hence the order below
 * is load-bearing: arm, self-SEV + WFE to drain any stale event, THEN re-check
 * every wake source with SEVONPEND live (anything pending now is caught by the
 * check, anything that pends later is a fresh edge), THEN the one real WFE, then
 * disarm so OpenBoot and the fault paths see plain-WFI semantics again.
 *
 * WHY THE MASK: "nothing is queued" must be decided atomically against the
 * IRQ-tail work that posts TMOS events (the vendor status callback runs at
 * thread level in the BLEB/BLEL IRQ tail). The woken ISRs and that tail run at
 * the csrrs that ends the critical section. Taking the mask churns CSR 0x800,
 * a bench-confirmed USB enumeration hazard, so nothing sleeps while
 * unconfigured or within DONGLE_PM_EP0_QUIET_MS of a SETUP (a mitigation, not
 * a proof: the enumeration soak validates it and the knob tunes it).
 *
 * WHY A HEARTBEAT: TMOS software timers are polled by TMOS_SystemProcess
 * against the RTC; without HAL_SLEEP there is no timer IRQ, and HAL_SLEEP is
 * forbidden here (its idleCB would route the quiet passes into
 * tmos_proces_idle, which strobes RFEND and re-inits the LLE/BB). TMR3 CYC_END
 * at DONGLE_PM_HEARTBEAT_US bounds every timer-backed deadline to one heartbeat
 * of wake latency (plus the foreground the scheduler runs ahead of it, exactly
 * as today). Priority 0xF0 (lowest, same as BLEL) so TMR0/BLEB at 0 preempt it.
 *
 * WHY TWO LATCHES, entry_work AND TWO QUIET PASSES: TMOS_SystemProcess
 * dispatches ONE task per call, scanning by id from activeTaskID (0 or 2 at a
 * call boundary; after a task-0 dispatch it stays 0), and THREE tasks are
 * registered (HAL 0, the library's own RF task 1, the app 2), so an app event
 * can need three passes to be reached, and a handler may re-queue leftover
 * bits (`events ^ bit` returns). dongle_pm_post (every hal_event_post) and
 * dongle_pm_ran (every RF_ProcessEvent entry) record whether the iteration
 * did or queued app work; entry_work carries a post made after the previous
 * decision across the loop-top clear; two quiet passes plus the ordinary pass
 * equal the task count, so every app event queued at the top of an iteration
 * is dispatched within it. The residual - a timer expiring DURING the passes -
 * is what the heartbeat bounds, and veto_entry (page 6) is the alarm that the
 * scheduler-shape argument stopped holding (e.g. a fourth task).
 *
 * entry_work is ARMED only by the masked re-check, the one point that observes
 * both latches clear with interrupts off (pm_entry_armed, consumed by
 * pm_loop_top). A post latch that is still set at the loop top after a
 * work veto is usually stale - the post was dispatched in that very iteration
 * and the latch merely outlived it - and counting it as entry work vetoed one
 * idle attempt per work item (bench: ~285/s on a live link against ~4500
 * sleeps, which drowned the alarm). It is not PROVEN stale: a late ISR post
 * can still be queued, and that post is safe for the same reason as any
 * other queued at the top of an iteration - the next iteration's three passes
 * dispatch it and dongle_pm_ran vetoes - not because of the carry-over.
 * Armed, a set latch means the post landed after the masked observation,
 * which is exactly the IRQ-tail case the carry-over exists for; three passes
 * then either dispatch it (veto_work) or the alarm is real. What the stale
 * veto used to catch by accident - a timer expiring during the final pass -
 * is the heartbeat-bounded residual stated above.
 *
 * 2026-06-13 ("no WFI while suspended", usb_device.h): that failure was a plain
 * WFI with the RF poll unbounded and no TMOS-quiet test, on a driver that predates
 * this tree; its mechanism was never isolated. The two candidates the tree supports
 * (a masked WFI that never wakes; sleeping on a queued TMOS event) are addressed
 * here, so idle while suspended is on by default behind DONGLE_PM_IDLE_IN_SUSPEND
 * and validated by the S scenario (bench 2026-09-06: link held, 0 lapses).
 *
 * Never call RF_FrequencyHopping*: the library's only WFI/SCTLR writer
 * (ll_tx_wait_finish) is reachable from rf_fh.o solely through the hopping
 * state those calls enable; on the basic RF_Rx/RF_Tx path this file is the
 * only runtime SCTLR owner.
 */
#include "CONFIG.h"        /* TMOS_SystemProcess; the SDK HAL_SLEEP default; TRUE */
#include "HAL.h"

/* Value tests belong AFTER CONFIG.h: before it neither the SDK default nor TRUE
 * is visible, so -DHAL_SLEEP=TRUE would preprocess to 0 and slip past. */
#if defined(HAL_SLEEP) && HAL_SLEEP
#error "HAL_SLEEP is forbidden on the dongle: its idleCB routes the TMOS idle path into tmos_proces_idle (LLE/BB re-init); pm_ch592.c owns idle"
#endif

#include "CH59x_gpio.h"
#include "CH59x_pwr.h"
#include "dongle_platform.h"   /* DONGLE_PM_IDLE default; dongle_pm_diag_fill */
#include "dongle_target.h"     /* HAL_TICKS_PER_US */
#include "hal_timing.h"        /* hal_now: SysTick keeps counting in Idle */
#include "rf_task.h"           /* RF_IdleClass + RF_IDLE_CLASS_* */
#include "usb_device.h"        /* USB_PmSnapshot + USB_PM_* */
#include "pm_ch592.h"

#if DONGLE_PM_IDLE_LEVEL && !DONGLE_PM_IDLE
#error "PM_IDLE_LEVEL needs PM_IDLE=1"
#endif
#if DONGLE_PM_IDLE_LEVEL < 0 || DONGLE_PM_IDLE_LEVEL > 3
#error "PM_IDLE_LEVEL must be 0..3"
#endif
#if DONGLE_PM_HEARTBEAT_US < 100 || DONGLE_PM_HEARTBEAT_US > 10000
#error "PM_HEARTBEAT_US must be 100..10000"
#endif
#if DONGLE_PM_EP0_QUIET_MS < 0 || DONGLE_PM_EP0_QUIET_MS > 5000
#error "PM_EP0_QUIET_MS must be 0..5000"
#endif

#if DONGLE_PM_IDLE

/* Curated wake set in PFIC->IPR[0]: TMR0 16, BLEB 20, BLEL 21, USB 22. SysTick
 * (12) is deliberately NOT in it: it has no enabled ISR, so a pending SysTick
 * would never clear and would be a permanent, unnamed veto; as an alien it is
 * counted and its mask recorded on page 6. TMR3 (32) is IPR[1] bit 0. */
#define PM_WAKE_IPR0    0x00710000u
#define PM_RADIO_IPR0   0x00300000u     /* BLEB | BLEL */
#define PM_TMR0_IPR0    (1u << 16)
#define PM_USB_IPR0     (1u << 22)
#define PM_TMR3_IPR1    1u
/* ADC (IRQ 29, shared with TouchKey): never enabled (the weak trap vector), but
 * the BLE library's 1 s TMOS_TempSample -> HAL_GetInterTempValue (MCU.c) polls
 * RB_ADC_START and leaves RB_ADC_IF_EOC set. That flag is RO and clears on a
 * write to R8_ADC_CONVERT (or R8_TKEY_CONVERT, or an automatic conversion);
 * the SDK's own ADC_ClearITFlag() uses the same write. Bench 2026-09-06, R2a
 * negative control: IPR0 bit 29 stayed set between samples, 100 % alien vetoes,
 * idle duty 0. Whether a held ADC request re-pends after a PFIC clear or needs a
 * fresh edge is not documented (codex review), so pm_irq_pending clears BOTH the
 * source (a START-clear write: START=0 is "stop", no conversion starts, PGA bits
 * preserved) and the PFIC pending bit, counts it (page 6 [50]), and re-reads
 * IPR so a bit that is still set falls through to the alien veto. The sample
 * runs in TMOS task context, never concurrently with this main-loop path. */
#define PM_ADC_IPR0     (1u << 29)
/* Registered TMOS tasks minus one (HAL 0, lib RF 1, app 2). Sufficient only
 * while the HAL task never has more than one event outstanding (today:
 * HAL_REG_INIT_EVENT alone; LED/KEY/TEST are compile-time off). */
#define PM_QUIET_PASSES 2u
#define PM_EP0_QUIET_TICKS (DONGLE_PM_EP0_QUIET_MS * 1000u * HAL_TICKS_PER_US)

enum { PM_V_NONE = 0, PM_V_USB, PM_V_EP0 };

/* Every access is a relaxed atomic builtin (the ISR/task store in
 * hal_event_post, the loads below, the exchange in pm_loop_top), so the
 * object is never mixed between atomic and plain access. Relaxed on this
 * single core is one lw/sw/amoswap.w; the only cost is that GCC addresses
 * the object explicitly instead of gp-relative, one extra instruction at
 * each post site (owner's call: absorbed for the simpler code). */
volatile uint32_t dongle_pm_post;
volatile uint8_t dongle_pm_ran;
/* Set under the mask when the re-check saw both latches clear; consumed (and
 * cleared) by pm_loop_top. Thread context only. */
static uint8_t pm_entry_armed;

/* dongle_pm_ran is written and read by task context only (RF_ProcessEvent
 * runs under TMOS_SystemProcess in this loop), so its plain accesses race
 * nothing; the post latch is read through the builtin like every access. */
#define PM_POST_PENDING()  (__atomic_load_n(&dongle_pm_post, __ATOMIC_RELAXED) != 0u)

/* Counters for IAP 0x92 pages 5 and 6. Written on the idle path at thread level
 * except pm_hb_irqs (TMR3 ISR); read by dongle_pm_diag_fill. */
static volatile uint32_t pm_wfe_count, pm_idle_tsys, pm_hb_irqs;
static volatile uint32_t pm_wake_tmr3, pm_wake_radio, pm_wake_both, pm_wake_tmr0;
static volatile uint32_t pm_wake_usb, pm_wake_other, pm_wake_none;
static volatile uint32_t pm_veto_work, pm_veto_pending, pm_veto_usb, pm_veto_state;
static volatile uint32_t pm_veto_ep0, pm_veto_alien, pm_veto_entry, pm_stale_tmr0;
static volatile uint32_t pm_quiet_passes, pm_stale_adc;

#if DONGLE_PM_EXACT_DEADLINE
/* ---- Exact-deadline heartbeat (plan section 12, third revision) ----
 * One word per RF_EVT_* bit: the RTC 32 kHz count at which that app timer
 * is due, or PM_DEADLINE_EMPTY. TMOS keeps its timers on the same counter
 * (HAL_TimeInit hands it SYS_GetClockValue and RTC_MAX_COUNT), so the table
 * shares TMOS's clock and modulus: 1 unit = 625 us = 20 ticks at CAB_LSIFQ
 * 32000, every sum and distance is taken modulo RTC_MAX_COUNT (0xA8C00000,
 * the counter's wrap every 24.576 h).
 *
 * NO masks at the writers and NO compare-and-swap anywhere: this QingKe V4
 * executes LR/SC as plain loads/stores (SC always succeeds), so a compiler
 * CAS is not atomic against the IRQ-tail sink; the atomic tools here are
 * single-instruction AMOs and the idle mask pm_idle_try already holds. The
 * writers are ORDERED instead, so every cross-context interleaving of a
 * same-bit start and stop leaves the table consistent or holding a PHANTOM
 * entry (floor wakes until the stuck-entry guard drops it), never missing a
 * live timer for longer than the cap: start = TMOS call first, publish only
 * on TRUE; stop = EMPTY first, then the TMOS call. The one bit both contexts
 * restart (supervision, per RX) rewrites its entry within 875 us anyway.
 * Retirement is decided in pm_tmr3_plan and applied in pm_tmr3_arm on
 * dispatch evidence that carries TIME without a clock read on the dispatch
 * path (a 1.3 us RTC read per dispatch measurably cost poll replies):
 * RF_ProcessEvent ORs the dispatched bits into pm_evid_bits (task context
 * only), and pm_evid_since is the RTC count at which that evidence WINDOW
 * opened - the moment the previous plan was applied. An entry is consumed
 * only if its bit was dispatched inside the window AND its deadline was
 * already due when the window opened (2 ticks of tolerance), because then
 * every dispatch inside the window happened after the deadline. A timer that
 * became due inside the window is ambiguous (the dispatch may be an
 * immediate post of the same bit from before the deadline): it is left in
 * place, arms the floor, and the next window - which opens after this
 * apply, when it is already due - retires it on the expiry's own dispatch,
 * one short wake later. Evidence from an earlier occurrence of the same
 * timer can never retire the next one (the adversarial review's sequence:
 * a PAIR_PREP dispatched 30 ms before its restart's expiry, the expiry
 * landing in the last quiet pass where TMOS queues the event internally,
 * and a 200 ms EP0 veto keeping a sticky bit alive used to admit a
 * cap-long sleep over the queued event). The window closes only when a
 * plan is APPLIED (pm_tmr3_arm), so a veto keeps the evidence. An entry
 * overdue by more than 100 ms with no such dispatch is a phantom (or a
 * refused start): dropped and counted in hb_stale_drop, the alarm. The guard bounds how long an entry
 * stays OVERDUE, checked at the next admitted sleep, not a phantom's whole
 * life: a phantom with a far deadline first produces deadline-timed wakes
 * (no worse than the cap), then floor wakes for up to 100 ms; the TMR3 ISR's
 * fallback to the cap keeps even that from running while idle is vetoed. */
#define PM_RTC_MOD           ((uint32_t)RTC_MAX_COUNT)
#define PM_DEADLINE_EMPTY    0xFFFFFFFFu            /* never a valid count (< PM_RTC_MOD) */
#define PM_UNIT_RTC          20u                    /* 625 us */
#define PM_DUE_WINDOW_RTC    2u                     /* 62 us: "due" for a dispatched bit */
#define PM_DEADLINE_MIN_RTC  4u                     /* 125 us: the floor, chosen explicitly */
#define PM_STALE_RTC         3200u                  /* 100 ms overdue with no dispatch: phantom */
#define PM_CAP_RTC           ((uint32_t)DONGLE_PM_DEADLINE_CAP_US * 32u / 1000u)   /* us -> ticks at 32000 Hz */
#define PM_TMR3_PER_RTC      1875u                  /* 60e6 / 32000, exact */
#if RTC_MAX_COUNT != 0xA8C00000
#error "pm_ch592.c assumes the CH59x RTC modulus 0xA8C00000 (RTC_MAX_COUNT)"
#endif
#if CAB_LSIFQ != 32000
#error "pm_ch592.c assumes the LSI at 32000 Hz (CAB_LSIFQ): 1 TMOS unit = 20 ticks, 1 tick = 1875 TMR3 ticks"
#endif
static volatile uint32_t pm_deadline[16];
static uint32_t pm_evid_bits;                 /* bits dispatched since the window opened; task context only */
static uint32_t pm_evid_since;                /* RTC count at which the window opened (last applied plan) */
static volatile uint32_t pm_hb_arm_deadline, pm_hb_arm_cap;
static uint8_t pm_hb_stale_drop;              /* saturating: page 5 [59] high nibble */

__HIGH_CODE
static inline uint32_t pm_rtc_now(void)
{
    uint32_t i;
    do {                                  /* SYS_GetClockValue's double read */
        i = R32_RTC_CNT_32K;
    } while (i != R32_RTC_CNT_32K);
    return i;
}
/* now + d, modulo the RTC wrap (d < PM_RTC_MOD). */
static inline uint32_t pm_rtc_add(uint32_t now, uint32_t d)
{
    uint32_t s = now + d;
    return (s >= PM_RTC_MOD) ? s - PM_RTC_MOD : s;
}
/* Ticks from now to a deadline, modulo the wrap: < PM_RTC_MOD/2 (12 h) means
 * that many ticks in the future; anything larger means the deadline is past,
 * and (PM_RTC_MOD - dist) is how far past. */
static inline uint32_t pm_rtc_dist(uint32_t deadline, uint32_t now)
{
    return (deadline >= now) ? deadline - now : deadline + PM_RTC_MOD - now;
}
#define PM_RTC_PAST(dist)  ((dist) >= (PM_RTC_MOD / 2u))

__HIGH_CODE
void pm_tmos_start(uint8_t task, uint16_t bit, uint32_t units)
{
    uint32_t now = pm_rtc_now();          /* before the call: never later than TMOS's own start */
    if (tmos_start_task(task, bit, units)) {
        pm_deadline[__builtin_ctz((uint32_t)bit)] = pm_rtc_add(now, units * PM_UNIT_RTC);
    }                                     /* refused (no timer available): keep the previous entry */
}

__HIGH_CODE
void pm_tmos_stop(uint8_t task, uint16_t bit)
{
    pm_deadline[__builtin_ctz((uint32_t)bit)] = PM_DEADLINE_EMPTY;   /* EMPTY first, then TMOS */
    tmos_stop_task(task, bit);
}

/* RF_ProcessEvent entry, task context only: one OR, no clock read (the
 * dispatch path arms the post-poll receive; every microsecond there costs
 * replies). Read and cleared by the plan/apply pair in the same context. */
__HIGH_CODE
void pm_deadline_dispatched(uint16_t events)
{
    pm_evid_bits |= events;
}

/* Was `deadline` already due when the evidence window opened (2 ticks of
 * tolerance)? Then a dispatch of its bit inside the window came after it. */
static inline uint8_t pm_due_at_window_open(uint32_t deadline)
{
    uint32_t d = pm_rtc_dist(deadline, pm_evid_since);   /* future = deadline after the window opened */
    return (uint8_t)(PM_RTC_PAST(d) || d <= PM_DUE_WINDOW_RTC);
}

/* The arm is planned UNMASKED (after the quiet passes, before the mask) and
 * only applied under the mask: the masked window before the WFE is on the
 * poll critical path - a TX-done interrupt that lands inside it waits for it,
 * and that wait delays the post-poll receive arm; the bench measured ~0.8 %
 * of poll replies missed with the RTC read and the 16-entry scan inside the
 * mask, none with them outside. What the plan reads can change before the
 * WFE only through an IRQ-tail timer start, which cannot make an entry
 * earlier than one unit and rarely shorter than the cap: a deadline that
 * appears between plan and sleep is cap-bounded once (and in CONNECTED TMR0
 * wakes the core every 875 us anyway). Retirement is decided in the plan and
 * applied under the mask only if the entry still holds the value the plan
 * saw, so a fresh deadline written by the sink in between is never erased. */
static uint32_t pm_plan_now;            /* the plan's RTC read: the next window opens here */
static uint32_t pm_plan_best;           /* ticks to arm, PM_DEADLINE_MIN_RTC..cap */
static uint32_t pm_plan_clear;          /* bit i: retire entry i if unchanged */
static uint32_t pm_plan_stale;          /* subset of pm_plan_clear retired as stuck, not consumed */
static uint32_t pm_plan_seen[16];       /* the value the plan saw in a to-clear entry */
static uint8_t  pm_plan_hit;

__HIGH_CODE
static void pm_tmr3_plan(void)
{
    uint32_t now = pm_rtc_now();
    /* The window closes only when a plan is applied. Should one stay open
     * past a quarter of the RTC modulus (6 h of vetoed sleeps), its opening
     * time can no longer be placed on the wrapping counter, so its evidence
     * is ignored and the next apply reopens it (codex: aged evidence). */
    uint32_t evid = (pm_rtc_dist(now, pm_evid_since) < PM_RTC_MOD / 4u) ? pm_evid_bits : 0u;
    uint32_t best = PM_CAP_RTC;
    uint32_t clear = 0u, stale = 0u;
    uint8_t  hit = 0u;
    uint32_t i;
    pm_plan_now = now;
    for (i = 0u; i < 16u; i++) {
        uint32_t d = pm_deadline[i];
        uint32_t dist, cand;
        if (d == PM_DEADLINE_EMPTY) continue;
        dist = pm_rtc_dist(d, now);
        if (PM_RTC_PAST(dist)) {
            uint32_t overdue = PM_RTC_MOD - dist;
            if ((evid & (1u << i)) && pm_due_at_window_open(d)) {   /* dispatched after it was due */
                clear |= 1u << i; pm_plan_seen[i] = d;
                continue;
            }
            if (overdue > PM_STALE_RTC) {                 /* phantom or refused start */
                clear |= 1u << i; stale |= 1u << i; pm_plan_seen[i] = d;
                continue;
            }
            cand = PM_DEADLINE_MIN_RTC;                   /* awaiting its dispatch: one short wake */
        } else if (dist <= PM_DUE_WINDOW_RTC) {
            /* Due now: TMOS's expiry test accepts equality, so its event may
             * already be queued (codex: dist == 0 arms the floor, not a unit). */
            if ((evid & (1u << i)) && pm_due_at_window_open(d)) {
                clear |= 1u << i; pm_plan_seen[i] = d;   /* and dispatched after it was due */
                continue;
            }
            cand = PM_DEADLINE_MIN_RTC;
        } else {
            cand = dist + PM_UNIT_RTC;                    /* land just after TMOS's own expiry */
        }
        if (cand < best) { best = cand; hit = 1u; }
    }
    if (best < PM_DEADLINE_MIN_RTC) best = PM_DEADLINE_MIN_RTC;
    pm_plan_best = best; pm_plan_clear = clear; pm_plan_stale = stale; pm_plan_hit = hit;
}

/* Under the mask, right before the real WFE: apply the plan. A handful of
 * stores; nothing is read from the RTC or scanned here. Only this path
 * re-arms; a veto leaves the last period running, so TMR3 keeps cycling at
 * <= cap whenever the idle path is not reached. */
__HIGH_CODE
static void pm_tmr3_arm(void)
{
    uint32_t clear = pm_plan_clear;
    pm_evid_bits = 0u;                    /* the window closes here, not at the plan: a veto keeps it */
    pm_evid_since = pm_plan_now;
    while (clear != 0u) {
        uint32_t i = __builtin_ctz(clear);
        clear &= clear - 1u;
        if (pm_deadline[i] == pm_plan_seen[i]) {          /* unchanged since the plan */
            pm_deadline[i] = PM_DEADLINE_EMPTY;
            if ((pm_plan_stale & (1u << i)) && pm_hb_stale_drop < 15u) pm_hb_stale_drop++;
        }
    }
    if (pm_plan_hit) pm_hb_arm_deadline++; else pm_hb_arm_cap++;
    R32_TMR3_CNT_END = pm_plan_best * PM_TMR3_PER_RTC;   /* <= 3.0M at a 50 ms cap: fits the 26 bits */
    R8_TMR3_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R8_TMR3_CTRL_MOD = RB_TMR_COUNT_EN;
}
#endif /* DONGLE_PM_EXACT_DEADLINE */
static volatile uint32_t pm_alien_ipr0, pm_alien_ipr1;
static volatile uint32_t pm_last_wake_ipr0, pm_last_wake_ipr1;
static volatile uint8_t  pm_last_wake_flags;

/* PB15 (the CH592D ISP strap) as ROM/OpenBoot left it, plus who owns the pad:
 * b0 PU, b1 PD, b2 DIR, b3 CFG_DEBUG_EN. Page 6 [43]; recorded from the first
 * production unit (not bench-measurable: the WeAct PB15 is SWCLK). */
static uint8_t pm_pb15_boot;

/* Snapshot BEFORE anything touches a pin: main() calls this right after
 * SetSysClock, ahead of pm_gpio_park (whose PB14/PB15 writes are inert while
 * CFG_DEBUG_EN=1, pin-table note 3). Taken here rather than inside the park so
 * page 6 [43] is valid on every PM_IDLE build, park on or off; a park-off
 * image would otherwise report a zero byte that reads as a definite
 * "debug disabled, no pull" and invite a wrong PB15-ownership call. */
void pm_boot_snapshot(void)
{
    pm_pb15_boot = (uint8_t)(((R32_PB_PU >> 15) & 1u)
                 | (((R32_PB_PD_DRV >> 15) & 1u) << 1)
                 | (((R32_PB_DIR >> 15) & 1u) << 2)
                 | ((R8_GLOB_CFG_INFO & RB_CFG_DEBUG_EN) ? 8u : 0u));
}

/* Level policy over RF_IdleClass() bits: 1 = the terminal PAIRING camp exactly;
 * 2 = every PAIRING sub-mode + IDLE; 3 = + CONNECTED between the ~875 us polls.
 * Never sleep, at any level: a reboot quiesce, a pending bond persist, and the
 * fresh-pair ACK burst / confirm-before-persist phase. The last one is a bench
 * finding (2026-09-06, R2b): with idle allowed there, 3 of 15 fresh pairs lost
 * the confirming RX and deferred the durable bond write (confirm_to_waitrx),
 * while a never-idle control persisted 10/10. Both phases last well under a
 * second and only occur on a fresh pair, so vetoing them costs nothing. */
#define PM_NEVER_IDLE (RF_IDLE_CLASS_QUIESCED | RF_IDLE_CLASS_PERSIST \
                       | RF_IDLE_CLASS_BURST | RF_IDLE_CLASS_CONFIRM)
__HIGH_CODE
static uint8_t pm_level_allows(uint8_t cls)
{
#if DONGLE_PM_IDLE_LEVEL == 0
    (void)cls;
    return 0u;
#elif DONGLE_PM_IDLE_LEVEL == 1
    return (uint8_t)(cls == 0u);
#elif DONGLE_PM_IDLE_LEVEL == 2
    return (uint8_t)((cls & (RF_IDLE_CLASS_CONNECTED | PM_NEVER_IDLE)) == 0u);
#else
    return (uint8_t)((cls & PM_NEVER_IDLE) == 0u);
#endif
}

/* EVERY USB admission condition in one RAM-resident predicate, evaluated
 * unmasked before the quiet passes AND again on a snapshot taken under the
 * mask after the drain: a USB ISR in between can un-configure (USB_BusReset),
 * set/clear suspend, or take a SETUP and then W1C its flag, so the raw pending
 * re-check alone would not see it. Returns a reason so the unmasked caller can
 * count veto_usb / veto_ep0 and the masked caller counts veto_pending. */
__HIGH_CODE
static uint8_t pm_usb_veto(uint8_t usb)
{
    if (!(usb & USB_PM_CONFIGURED)) {
        return PM_V_USB;                /* no CSR churn during enumeration */
    }
#if !DONGLE_PM_IDLE_IN_SUSPEND
    if (usb & USB_PM_SUSPENDED) {
        return PM_V_USB;                /* today's spin-while-suspended */
    }
#endif
#if DONGLE_PM_EP0_QUIET_MS > 0
    /* Mitigation window after every SETUP. hal_now() wraps every 71.6 s, so
     * once per wrap with no SETUP this reads spuriously small: one 200 ms spin
     * per 71.6 s (0.3 % duty), accepted. PM_EP0_QUIET_MS=0 means "window off"
     * and is compiled out (the comparison would be an always-false `< 0u`). */
    if ((uint32_t)(hal_now() - usb_last_setup_tsys) < PM_EP0_QUIET_TICKS) {
        return PM_V_EP0;
    }
#endif
    if (usb & (USB_PM_PENDING | USB_PM_WAKE_REQ)) {
        return PM_V_USB;                /* USB_PollEP6 / USB_ServiceRemoteWake owe work */
    }
    if (((usb >> USB_PM_LED_SHIFT) & 7u) != dongle_usb_led_last) {
        return PM_V_USB;                /* SET_REPORT landed after poll_usb_led_state */
    }
    return PM_V_NONE;
}

/* Under the mask, after the drain: is any wake source already pending? A source
 * pending BEFORE SEVONPEND was armed makes no edge, so the raw flags are read
 * too. Stale TMR0: a CYC_END that landed between hal_timer_cancel's mask and
 * rf_tmr0_stop's PFIC_DisableIRQ leaves IRQ 16 pending with the line disabled
 * and the flag clear (whether the PFIC latches or levels pending is not
 * documented; this handles both) - cleared and counted, not a veto. Any IPR bit
 * outside the curated set vetoes and is recorded (veto_alien, page 6). */
__HIGH_CODE
static uint8_t pm_irq_pending(void)
{
    uint32_t p0 = PFIC->IPR[0];
    uint32_t p1 = PFIC->IPR[1];

    if ((p0 & PM_TMR0_IPR0) && !(PFIC->ISR[0] & PM_TMR0_IPR0)
            && !(R8_TMR0_INT_FLAG & RB_TMR_IF_CYC_END)) {
        PFIC_ClearPendingIRQ(TMR0_IRQn);
        pm_stale_tmr0++;
        p0 &= ~PM_TMR0_IPR0;
    }
    if (p0 & PM_ADC_IPR0) {                     /* library temp-sample residue (see PM_ADC_IPR0) */
        R8_ADC_CONVERT = (uint8_t)(R8_ADC_CONVERT & (uint8_t)~RB_ADC_START);   /* clears IF_EOC */
        PFIC_ClearPendingIRQ(ADC_IRQn);
        pm_stale_adc++;
        p0 = PFIC->IPR[0];                      /* re-read: still set -> a real alien below */
    }
    if ((p0 & ~PM_WAKE_IPR0) || (p1 & ~PM_TMR3_IPR1)) {
        pm_alien_ipr0 = p0;
        pm_alien_ipr1 = p1;
        pm_veto_alien++;
        return 1u;
    }
    return (uint8_t)((p0 & PM_WAKE_IPR0) != 0u || (p1 & PM_TMR3_IPR1) != 0u
        || (R8_TMR0_INT_FLAG & RB_TMR_IF_CYC_END)
        || (R8_TMR3_INT_FLAG & RB_TMR_IF_CYC_END)
        || (R8_USB_INT_FG & (RB_UIF_TRANSFER | RB_UIF_SUSPEND | RB_UIF_BUS_RST)));
}

/* Wake attribution. Runs right after the real WFE and BEFORE __risc_v_enable_irq,
 * so no ISR has run and no flag has been W1C'd: what is pending now is what
 * ended the WFE. Only an exit with a radio IRQ pending and NOTHING ELSE
 * pending proves the radio ended it (wake_radio). With any other source
 * co-pending (wake_both: TMR3, TMR0, USB or an alien) the snapshot cannot
 * order them - a radio IRQ that never woke the core sits pending until the
 * next TMR3/TMR0/USB does, but a radio wake with a heartbeat landing inside
 * this read window looks identical. So the R2a proof is a protocol, not a
 * ratio: the keyboard-absent negative control must read wake_radio ==
 * wake_both == 0, then every reconnect/pair must advance wake_radio; a radio
 * that never ends the WFE shows as wake_both advancing with wake_radio flat. */
__HIGH_CODE
static void pm_attribute_wake(void)
{
    uint32_t p0 = PFIC->IPR[0];
    uint32_t p1 = PFIC->IPR[1];
    uint8_t  f  = (uint8_t)(((R8_TMR0_INT_FLAG & RB_TMR_IF_CYC_END) ? 0x01u : 0u)
                | ((R8_TMR3_INT_FLAG & RB_TMR_IF_CYC_END) ? 0x02u : 0u)
                | ((R8_USB_INT_FG & RB_UIF_TRANSFER) ? 0x04u : 0u)
                | ((R8_USB_INT_FG & RB_UIF_SUSPEND)  ? 0x08u : 0u)
                | ((R8_USB_INT_FG & RB_UIF_BUS_RST)  ? 0x10u : 0u));
    uint8_t radio = (p0 & PM_RADIO_IPR0) != 0u;
    uint8_t tmr3  = (p1 & PM_TMR3_IPR1) != 0u || (f & 0x02u) != 0u;
    uint8_t tmr0  = (p0 & PM_TMR0_IPR0) != 0u || (f & 0x01u) != 0u;
    uint8_t usb   = (p0 & PM_USB_IPR0) != 0u || (f & 0x1Cu) != 0u;
    uint8_t other = (p0 & ~PM_WAKE_IPR0) != 0u || (p1 & ~PM_TMR3_IPR1) != 0u;

    if (radio) {
        if (tmr3 | tmr0 | usb | other) {
            pm_wake_both++;             /* co-pending: unordered, not a radio proof */
        } else {
            pm_wake_radio++;            /* radio alone: it ended the WFE */
        }
    }
    if (tmr3)  pm_wake_tmr3++;
    if (tmr0)  pm_wake_tmr0++;
    if (usb)   pm_wake_usb++;
    if (other) pm_wake_other++;
    if (!(radio | tmr3 | tmr0 | usb | other)) pm_wake_none++;
    pm_last_wake_ipr0  = p0;
    pm_last_wake_ipr1  = p1;
    pm_last_wake_flags = f;
}

__HIGH_CODE
uint8_t pm_loop_top(void)
{
    /* Capture and clear the post latch in ONE instruction (amoswap.w):
     * an ISR post lands either before the swap, and is captured, or after
     * it, and survives for the next loop top. A separate read then clear
     * could erase a post that landed in between (review of PR #36); the
     * event itself would still sit in TMOS and be dispatched by this
     * iteration's three passes, but the latch is what the entry alarm and
     * the quiet-pass veto read. Not a mask: taking CSR 0x800 on every loop
     * iteration is the enumeration hazard the USB_ServiceRemoteWake comment
     * in usb_device.c records. The other two stores race nothing:
     * dongle_pm_ran is written by task context only, pm_entry_armed by this
     * loop only.
     *
     * The capture counts only if the previous decision armed it: a post that
     * landed after the masked observation must survive the clear (the radio
     * sink runs at thread level in the IRQ tail, so its post lands before
     * __risc_v_enable_irq returns); one left over from a work veto is stale
     * and must not. */
    uint32_t post = __atomic_exchange_n(&dongle_pm_post, 0u, __ATOMIC_RELAXED);
    uint8_t entry_work = (uint8_t)((post != 0u) & pm_entry_armed);

    dongle_pm_ran   = 0u;
    pm_entry_armed  = 0u;
    return entry_work;
}

__HIGH_CODE
void pm_idle_try(uint8_t entry_work)
{
    uint32_t irq, t0;
    uint8_t usb, v, n;

    if (PM_POST_PENDING() || dongle_pm_ran) {
        pm_veto_work++;
        return;
    }
    if (!pm_level_allows(RF_IdleClass())) {
        pm_veto_state++;
        return;
    }
    v = pm_usb_veto(USB_PmSnapshot());
    if (v == PM_V_USB) {
        pm_veto_usb++;
        return;
    }
    if (v == PM_V_EP0) {
        pm_veto_ep0++;
        return;
    }
    /* Exhaust the scheduler (three passes = three tasks, see the header). */
    for (n = 0u; n < PM_QUIET_PASSES; n++) {
        TMOS_SystemProcess();
        pm_quiet_passes++;
        if (PM_POST_PENDING() || dongle_pm_ran) {
            pm_veto_work++;
            return;
        }
    }
    if (entry_work) {
        /* Posted after the previous masked observation and STILL not
         * dispatched by three passes: never sleep on it. A bounded count is
         * benign: hal_event_cancel does not clear
         * the post latch (and must not - that could erase a real post of
         * another bit), so a post that a teardown cancels in the same
         * iteration (the TMR0 ISR's POLL / SEND_PAIR_ACK swept by
         * rf_return_to_fresh_pair or rf_enter_stock_reacquire) costs one
         * extra spin here, of the order of the link-loss teardowns. The
         * scheduler-shape alarm is a RATE comparable to wfe_count or
         * quiet_passes, not a nonzero count. */
        pm_veto_entry++;
        return;
    }
    if (!pm_level_allows(RF_IdleClass())) {
        pm_veto_state++;                /* a quiet pass may have moved the state */
        return;
    }
#if DONGLE_PM_EXACT_DEADLINE
    pm_tmr3_plan();                                     /* unmasked: the RTC read and the scan */
#endif

    irq = __risc_v_disable_irq();                       /* csrrc 0x800, 0x88 */
    PFIC->SCTLR &= ~(1u << 2);                          /* SLEEPDEEP = 0 */
    PFIC->SCTLR |= (1u << 4) | (1u << 3) | (1u << 5);   /* SEVONPEND | WFITOWFE | SETEVENT */
    __asm__ volatile ("wfi");                           /* self-SEV: drains any stale event */
    PFIC->SCTLR |= (1u << 3);                           /* re-arm WFE mode */
    usb = USB_PmSnapshot();                             /* re-read under the mask */
    /* Both latches clear, observed with interrupts off: anything the next
     * loop top finds posted came after this point (see pm_loop_top). */
    pm_entry_armed = (uint8_t)!(PM_POST_PENDING() || dongle_pm_ran);
    if (!pm_entry_armed
            || !pm_level_allows(RF_IdleClass())         /* IRQ-tail promote since the unmasked check */
            || pm_irq_pending()
            || pm_usb_veto(usb) != PM_V_NONE) {
        pm_veto_pending++;
    } else {
        pm_wfe_count++;
#if DONGLE_PM_EXACT_DEADLINE
        pm_tmr3_arm();                                  /* apply the plan: a few stores */
#endif
        t0 = hal_now();
        __asm__ volatile ("wfi");                       /* the real WFE; flash stays on */
        pm_idle_tsys += hal_now() - t0;
        pm_attribute_wake();                            /* before the csrrs */
    }
    PFIC->SCTLR &= ~((1u << 4) | (1u << 3));            /* disarm: plain-WFI semantics for OpenBoot/fault paths */
    (void)__risc_v_enable_irq(irq);                     /* woken ISRs (and the IRQ-tail sink) run here */
}

/* Heartbeat (OpenController main.c literal, period from the knob, lowest
 * priority so it never delays BB/TMR0 entry beyond nesting overhead). The
 * handler only clears the flag and counts: waking IS the point. */
void pm_heartbeat_init(void)
{
#if DONGLE_PM_EXACT_DEADLINE
    uint32_t i;
    for (i = 0u; i < 16u; i++) pm_deadline[i] = PM_DEADLINE_EMPTY;
    pm_evid_bits = 0u;
    pm_evid_since = pm_rtc_now();
#endif
    R8_TMR3_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R32_TMR3_CNT_END = (GetSysClock() / 1000000u) * DONGLE_PM_HEARTBEAT_US;   /* 60000 @ 1000 us; the exact-deadline mode re-arms per sleep */
    R8_TMR3_INT_FLAG = RB_TMR_IF_CYC_END;
    R8_TMR3_INTER_EN = RB_TMR_IE_CYC_END;
    R8_TMR3_CTRL_MOD = RB_TMR_COUNT_EN;
    PFIC_SetPriority(TMR3_IRQn, 0xF0);
    PFIC_EnableIRQ(TMR3_IRQn);
}

__INTERRUPT
__HIGH_CODE
void TMR3_IRQHandler(void)
{
    R8_TMR3_INT_FLAG = RB_TMR_IF_CYC_END;
    pm_hb_irqs++;
#if DONGLE_PM_EXACT_DEADLINE
    /* Fall back to the cap period after any fire: a short arm (a deadline
     * that was due, the 125 us floor) must fire once and then settle, even
     * if the idle path - the only other place that re-arms - is vetoed for
     * a long time (the 200 ms window after a USB SETUP, an unconfigured
     * bus). Writing CNT_END restarts the count; the next admitted sleep
     * re-arms precisely anyway (codex design review). */
    R32_TMR3_CNT_END = PM_CAP_RTC * PM_TMR3_PER_RTC;
#endif
}

static void pm_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* IAP 0x92 pages 5 "power" and 6 "power detail" (u32 LE unless noted; the
 * caller zeroed the page and wrote [0] version, [1] page). Mirrored by
 * tools/src/rfdiag.rs PAGE5_NAMES / PAGE6_NAMES.
 * Page 5: [2] wfe_count, [6] idle_tsys (SysTick ticks in WFE; rate/60e6 = idle
 * duty), [10] hb_irqs, [14] hal_now(), [18] wake_tmr3, [22] wake_radio (radio
 * the ONLY source pending: it ended the WFE), [26] wake_both (radio plus
 * another source pending: unordered), [30] wake_tmr0, [34] wake_usb, [38]
 * wake_other, [42] veto_work, [46] veto_pending, [50] veto_usb, [54] veto_state,
 * [58] flags (b0 TMR0 counting, b1 TMR3 counting, b2 usb configured, b3 usb
 * suspended, b4 idle-in-suspend, b5 debug_en, b6 remote wake armed), [59]
 * level, [60..61] heartbeat us (u16).
 * Page 6: [2] R32_SLEEP_CONTROL, [6] alien IPR0 last seen, [10] alien IPR1,
 * [14] veto_ep0, [18] stale_tmr0, [22] veto_alien, [26] veto_entry, [30]
 * quiet_passes, [34] last-wake IPR0, [38] last-wake IPR1, [42] last-wake raw
 * flags (b0 TMR0 CYC_END, b1 TMR3 CYC_END, b2 USB TRANSFER, b3 USB SUSPEND,
 * b4 USB BUS_RST), [43] pb15_boot (b0 PU, b1 PD, b2 DIR, b3 DEBUG_EN),
 * [44..45] usb_rw_arm_count (u16), [46] wake_none, [50] stale_adc, [54] hb_arm_deadline,
 * [58] hb_arm_cap (exact-deadline mode; 0 otherwise). */
void dongle_pm_diag_fill(uint8_t page, uint8_t *out)
{
    if (page == 5u) {
        uint8_t usb = USB_PmSnapshot();
        uint8_t f = 0u;

        pm_put32(&out[2],  pm_wfe_count);
        pm_put32(&out[6],  pm_idle_tsys);
        pm_put32(&out[10], pm_hb_irqs);
        pm_put32(&out[14], hal_now());
        pm_put32(&out[18], pm_wake_tmr3);
        pm_put32(&out[22], pm_wake_radio);
        pm_put32(&out[26], pm_wake_both);
        pm_put32(&out[30], pm_wake_tmr0);
        pm_put32(&out[34], pm_wake_usb);
        pm_put32(&out[38], pm_wake_other);
        pm_put32(&out[42], pm_veto_work);
        pm_put32(&out[46], pm_veto_pending);
        pm_put32(&out[50], pm_veto_usb);
        pm_put32(&out[54], pm_veto_state);
        if (R8_TMR0_CTRL_MOD & RB_TMR_COUNT_EN) f |= 0x01u;
        if (R8_TMR3_CTRL_MOD & RB_TMR_COUNT_EN) f |= 0x02u;
        if (usb & USB_PM_CONFIGURED)            f |= 0x04u;
        if (usb & USB_PM_SUSPENDED)             f |= 0x08u;
#if DONGLE_PM_IDLE_IN_SUSPEND
        f |= 0x10u;
#endif
        if (R8_GLOB_CFG_INFO & RB_CFG_DEBUG_EN) f |= 0x20u;
        if (usb & USB_PM_RW_ARMED)              f |= 0x40u;
#if DONGLE_PM_EXACT_DEADLINE
        f |= 0x80u;                              /* exact-deadline mode: [60..61] is the cap */
        out[58] = f;
        out[59] = (uint8_t)(DONGLE_PM_IDLE_LEVEL | (pm_hb_stale_drop << 4));   /* level | stale drops (saturating nibble) */
        out[60] = (uint8_t)DONGLE_PM_DEADLINE_CAP_US;
        out[61] = (uint8_t)(DONGLE_PM_DEADLINE_CAP_US >> 8);
#else
        out[58] = f;
        out[59] = (uint8_t)DONGLE_PM_IDLE_LEVEL;
        out[60] = (uint8_t)DONGLE_PM_HEARTBEAT_US;
        out[61] = (uint8_t)(DONGLE_PM_HEARTBEAT_US >> 8);
#endif
    } else if (page == 6u) {
        /* One 16-bit load: the USB ISR increments it, and two byte-wise
         * volatile reads could tear across a 0x00FF -> 0x0100 carry. */
        uint16_t arms = usb_rw_arm_count;

        pm_put32(&out[2],  R32_SLEEP_CONTROL);
        pm_put32(&out[6],  pm_alien_ipr0);
        pm_put32(&out[10], pm_alien_ipr1);
        pm_put32(&out[14], pm_veto_ep0);
        pm_put32(&out[18], pm_stale_tmr0);
        pm_put32(&out[22], pm_veto_alien);
        pm_put32(&out[26], pm_veto_entry);
        pm_put32(&out[30], pm_quiet_passes);
        pm_put32(&out[34], pm_last_wake_ipr0);
        pm_put32(&out[38], pm_last_wake_ipr1);
        out[42] = pm_last_wake_flags;
        out[43] = pm_pb15_boot;
        out[44] = (uint8_t)arms;
        out[45] = (uint8_t)(arms >> 8);
        pm_put32(&out[46], pm_wake_none);
        pm_put32(&out[50], pm_stale_adc);
#if DONGLE_PM_EXACT_DEADLINE
        pm_put32(&out[54], pm_hb_arm_deadline);
        pm_put32(&out[58], pm_hb_arm_cap);
#endif
    }
}

#endif /* DONGLE_PM_IDLE */

#if DONGLE_PM_GPIO_PARK
/* Explicit CH592D no-connect list (plan section 4), never GPIO_Pin_All: the
 * bench units are WeAct CH592F modules whose extra pads must not be biased
 * into loads. PB15 is the production ISP strap (tied to GND): INPUT PULL-DOWN,
 * never pulled up, never driven - 0 uA in every ownership case (while
 * CFG_DEBUG_EN=1 the debug block owns PB14/PB15 and these writes are inert,
 * which is why the state is snapshotted first). PB10/PB11 belong to the USB
 * PHY and are never touched here. Never set RB_DEBUG_EN. */
void pm_gpio_park(void)
{
    /* The pre-park PB15 / debug-ownership snapshot (page 6 [43]) is taken by
     * pm_boot_snapshot(), which main() calls before this on PM_IDLE builds. */
    GPIOA_ModeCfg(GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13
                  | GPIO_Pin_14 | GPIO_Pin_15, GPIO_ModeIN_PU);              /* 0xFC00 */
    GPIOB_ModeCfg(GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14, GPIO_ModeIN_PU);  /* 0x7000 */
    GPIOB_ModeCfg(GPIO_Pin_15, GPIO_ModeIN_PD);                              /* 0x8000 */
    /* CH592D-unbonded pads -> datasheet Table 7-9 "Only analog input":
     * DIR=0, PU=0, PD_DRV=0 FIRST (GPIO pin masks: a bare CONFIG2 write would
     * inherit whatever pull or drive ROM/OpenBoot left), THEN the CONFIG2
     * digital-input-disable bits (CONFIG2 masks: PB22/PB23 sit at bits 24/25
     * there, not 22/23). */
    GPIOA_ModeCfg(GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7
                  | GPIO_Pin_8 | GPIO_Pin_9, GPIO_ModeIN_Floating);           /* 0x03F0 */
    GPIOB_ModeCfg(GPIO_Pin_0 | GPIO_Pin_4 | GPIO_Pin_6 | GPIO_Pin_7
                  | GPIO_Pin_22 | GPIO_Pin_23, GPIO_ModeIN_Floating);         /* 0x00C000D1 */
    R32_PIN_CONFIG2 |= 0x03F0u /* PA4-PA9 */ | RB_PIN_PB0_DIS | RB_PIN_PB4_DIS
                     | RB_PIN_PB6_7_DIS | RB_PIN_PB22_23_DIS;
}
#endif /* DONGLE_PM_GPIO_PARK */

#if DONGLE_PM_CLK_GATE
#ifdef DEBUG
#error "UART1 is clock-gated by pm_clock_gate(); a DEBUG/PRINT build would hang in _write (CH59x_sys.c)"
#endif
#if (RB_SLP_CLK_I2C | RB_SLP_CLK_LCD) > 0xFF
#error "byte-1 SFR bits must be byte-local for the <<8 shift"
#endif
/* The datasheet describes every R8_SLP_CLK_OFF0/1 bit as an unconditional
 * clock gate, not a sleep-only one, so the R3 delta is expected in EVERY
 * scenario; safe because nothing linked here addresses a gated block (the BLE
 * library never touches TMR1-3/UART/SPI/I2C/PWMX). */
#define PM_CLK_GATE_FULL \
    (BIT_SLP_CLK_TMR1 | BIT_SLP_CLK_TMR2 \
     | BIT_SLP_CLK_UART0 | BIT_SLP_CLK_UART1 | BIT_SLP_CLK_UART2 | BIT_SLP_CLK_UART3 \
     | BIT_SLP_CLK_SPI0 | BIT_SLP_CLK_PWMX \
     | (uint16_t)((uint16_t)(RB_SLP_CLK_I2C | RB_SLP_CLK_LCD) << 8))
_Static_assert(PM_CLK_GATE_FULL == 0x4DF6u, "clock-gate set drifted from the validated set");
/* The mask actually applied comes from the build (PM_CLK_GATE_MASK, decimal;
 * default the full set) so the bench can bisect it; never a bit outside the set. */
#ifdef DONGLE_PM_CLK_GATE_MASK
#define PM_CLK_GATE_MASK ((uint16_t)(DONGLE_PM_CLK_GATE_MASK))
#else
#define PM_CLK_GATE_MASK PM_CLK_GATE_FULL
#endif
_Static_assert((PM_CLK_GATE_MASK & (uint16_t)~PM_CLK_GATE_FULL) == 0u,
               "PM_CLK_GATE_MASK contains a bit outside the validated 0x4DF6 set");
_Static_assert((PM_CLK_GATE_MASK & (BIT_SLP_CLK_TMR0 | BIT_SLP_CLK_TMR3
                                    | BIT_SLP_CLK_USB | BIT_SLP_CLK_BLE)) == 0u,
               "never gate TMR0 (pacer), TMR3 (heartbeat), USB or BLE");
void pm_clock_gate(void)
{
    PWR_PeriphClkCfg(DISABLE, PM_CLK_GATE_MASK);    /* 0x4DF6 by default */
}
#endif /* DONGLE_PM_CLK_GATE */

#if DONGLE_PM_USB_DIGIN_OFF
/* PB10/PB11 digital input buffers off (the datasheet recommends it for
 * analog-function pins; D+/D- are the only pins toggling at 12 MHz). Pulls,
 * DIR and the remote-wake K-state drive are untouched. */
void pm_usb_digin_off(void)
{
    R32_PIN_CONFIG2 |= (3u << 26);
}
#endif /* DONGLE_PM_USB_DIGIN_OFF */

/* Keep the translation unit non-empty for the all-off build (no code, no data). */
typedef int pm_ch592_all_off_marker_t;
