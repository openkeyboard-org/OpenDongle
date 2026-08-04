/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CH570D MVP port.
 */

/* CODEREVIEW N03: the Makefile's INT_SOFT guard checks only the make VARIABLE, so
 * EXTRA_CFLAGS=-DINT_SOFT=<anything> slips past it while the SDK's `#ifdef INT_SOFT`
 * still selects the software-ISR ABI (CH570 uses HPE/hardware stacking + live
 * __INTERRUPT handlers, same concern as CH59x). Fail the build LOUDLY on ANY
 * INT_SOFT definition (the shipping build leaves it undefined). */
#ifdef INT_SOFT
#error "INT_SOFT must be UNDEFINED for CH570: the software-ISR ABI breaks the WCH hardware-stacking interrupt path; build with the pinned MounRiver WCH toolchain and INT_SOFT off."
#endif

#include "CH57x_common.h"
#include "version.h"
#include "dongle_platform.h"
#include "sched.h"

#include "iap.h"
#include "usb_device.h"

#include "rf_task.h"
#include "hal_rf.h"

extern volatile uint8_t ch570_boot_reset_status;

/*
 * Default clock is 100 MHz: the CH570/CH572 parts are specified for it and WCH's
 * USB+RF examples run SetSysClock(CLK_SOURCE_HSE_PLL_100MHz). All RF timing is
 * expressed in CH570_SYSCLK_HZ Tsys units. 100 MHz is the only supported clock
 * (a build-time static assert below rejects anything else).
 */
#ifndef CH570_SYSCLK_SOURCE
#define CH570_SYSCLK_SOURCE CLK_SOURCE_HSE_PLL_100MHz
#endif

#ifndef CH570_SYSCLK_HZ
#define CH570_SYSCLK_HZ 100000000u
#endif

/*
 * Production board has ONLY the 32 MHz HSE crystal -- no external 32.768 kHz
 * low-speed crystal -- and is always bus-powered over USB. CH570_LSI_INTERNAL
 * explicitly sources the 32K low-speed domain from the internal LSI RC so the
 * firmware can never depend on an absent external crystal. CH570_DONGLE_NEVER_SLEEP
 * documents and guards the no-sleep policy: the CPU busy-polls and never enters
 * idle/deep-sleep. Both are on by default; USB and RF run off HSE/PLL and are
 * unaffected by the low-speed domain.
 */


/* The CH570 supports EXACTLY ONE clock: 100 MHz. */
_Static_assert(CH570_SYSCLK_SOURCE == CLK_SOURCE_HSE_PLL_100MHz,
               "CH570 only supports the 100 MHz PLL profile");
_Static_assert(CH570_SYSCLK_HZ == 100000000u,
               "CH570 only supports CH570_SYSCLK_HZ=100000000u");

/*
 * RB_PWR_EXTEND is a deep-sleep power-retention plan; it only does anything if
 * the part actually sleeps. Enabling it while claiming never-sleep is a
 * contradiction -- catch it at build time.
 */

static void usb_start(void)
{
    USB_DevInit();
    USB_SetEP6OutCallback(IAP_PacketHandler);
}

static uint8_t rf_started;

/* Deferred HID->USB forward (2026-07-08). The RF callback (radio-IRQ
 * context on CH570 — the shared sink runs in the true LLE/BB IRQ, unlike
 * the CH59x whose BLE-lib callbacks are IRQ-tail deferred) only LATCHES the
 * report here; the MAIN LOOP performs the USB arm in task context
 * (usb_hid_forward_pump). This matches the CH59x's effective dispatch
 * context and is the shape that was host-validated end to end (synthetic
 * radio-IRQ-origin reports: 171/172 delivered to the host hidraw; main-loop
 * EP1 arms measured 16/16). Direct-from-IRQ arming was the legacy v0.83
 * shape but has not been re-validated on the unified body, and USB_Send*
 * from the ~875 µs radio IRQ would also stretch the RF deadline — so the
 * deferral is both the validated and the conservative choice. Single-slot
 * newest-wins is correct for HID: last-state-wins is the USB interrupt-IN
 * model anyway (the old direct arm overwrote the EP buffer just the same),
 * and the main loop passes every few µs — far faster than the RF poll
 * cadence that spaces reports. */
static volatile uint8_t hid_fwd_pending;
static uint8_t hid_fwd_tag;
static uint8_t hid_fwd_len;
static uint8_t hid_fwd_body[8];

static void usb_hid_callback(uint8_t tag, const uint8_t *data, uint8_t len)
{
    uint8_t n = (len < 8u) ? len : 8u;

    for (uint8_t i = 0u; i < n; i++) {
        hid_fwd_body[i] = data[i];
    }
    hid_fwd_tag = tag;
    hid_fwd_len = n;
    hid_fwd_pending = 1u;   /* written last; the pump snapshots under mask */
}

/* Main-loop half: route the latched report to USB by on-air tag (parity with
 * CH592): 0xA1 boot keyboard -> EP1 (8-byte, zero-padded/clamped),
 * 0xA3 consumer/media -> EP3 composite (report id 1 + usage LE16),
 * 0xA8 mouse -> EP2 (5-byte verbatim). The USB_Send* helpers own endpoint
 * readiness (configured/suspended/remote-wake stash). */
static void usb_hid_forward_pump(void)
{
    uint8_t tag, len;
    uint8_t body[8];

    if (!hid_fwd_pending) {
        return;
    }
    {
        uint32_t irq = __risc_v_disable_irq();
        tag = hid_fwd_tag;
        len = hid_fwd_len;
        for (uint8_t i = 0u; i < 8u; i++) {
            body[i] = hid_fwd_body[i];
        }
        hid_fwd_pending = 0u;
        (void)__risc_v_enable_irq(irq);
    }
    if (!USB_IsConfigured()) {
        return;
    }
    if (tag == RF_PROTO_HID_TAG) {
        uint8_t report[8] = {0};
        for (uint8_t i = 0u; i < len; i++) {
            report[i] = body[i];
        }
        USB_SendKeyboard(report);
    } else if (tag == RF_PROTO_HID_TAG_CONSUMER && len >= 2u) {
        uint8_t r[3] = { RF_PROTO_USB_REPORT_ID_CONSUMER, body[0], body[1] };
        USB_SendComposite(r, sizeof(r));
    } else if (tag == RF_PROTO_HID_TAG_MOUSE && len >= 5u) {
        USB_SendMouse(body);
    }
}

static void rf_start(void)
{
    hal_rf_init();   /* the shared body expects the radio brought up by main
                      * (the CH592 pattern) */
    RF_TaskInit();
    /* MUST come AFTER RF_TaskInit: the shared init clears rf_hid_callback as
     * part of its state reset (rf_task.c RF_TaskInit), so registering first
     * leaves the callback NULL and every received HID report is silently
     * dropped before USB. That exact ordering bug shipped in the v0.96
     * unification and made the CH570 receive-but-never-forward (bond/hid
     * tallies climbing, zero host reports) — root-caused on the bench
     * 2026-07-08. The CH592 main has always registered after RF_TaskInit. */
    RF_SetHIDCallback(usb_hid_callback);
    rf_started = 1u;
}

/*
 * v0.82 software-deadline timer MUX (ST_N slots, types in sched.h), shared by the
 * RF firmware (rf_task.c slots: 0 pair-ACK, 1 EV10-rekey, 2 boot-window, 3
 * connected-poll).
 *
 * N independently-active soft timers carry absolute deadlines (32-bit software
 * epoch in Tsys ticks); the single HSE TMR is armed one-shot to the NEAREST
 * active deadline; the IRQ dispatches expired timers in slot/priority order,
 * re-checking active after EACH callback (handlers may set or cancel timers).
 *
 * The CH57x TMR auto-clears at CNT_END (proven), so "now" = st_epoch +
 * R32_TMR_COUNT, and each arm re-bases st_epoch to the live now before ALL_CLEAR
 * so no elapsed time is lost. All compares are modular (deltas << 2^31). The
 * The dispatcher preserves nearest-deadline selection, cancellation,
 * same-tick expiry, stale suppression, and same-slot restart semantics.
 */
static volatile uint32_t st_epoch;
static volatile uint8_t  st_active[ST_N];
static volatile uint32_t st_deadline[ST_N];
static st_cb_t           st_cb[ST_N];
static volatile uint32_t st_armed_delta;
static volatile uint8_t  st_dispatch_burst;  /* callbacks run in the last service */

/*
 * Hardware auto-reload periodic mode (sched.h contract; the CH592 TMR0
 * phase-lock mirror, ported from the ch570-freerun-timer prototype + the
 * coexistence dispatch that prototype lacked). While `st_periodic_slot_p1`
 * is nonzero (slot+1), that slot owns the TMR as a true auto-reload periodic;
 * st_now() switches to the SysTick formula (continuous, never freezes);
 * st_advance() is a NO-OP (the CYC_END flag is ISR-owned in this mode — the
 * prototype's mutator-context sync corrupted the missed-reload accounting);
 * st_rearm() must not touch the hardware; the other slots dispatch from the
 * periodic ISR at <=1-period granularity. 0 = one-shot mode (slot+1 encoding
 * keeps this .bss). st_last_sync carries the sub-period remainder across
 * syncs (advance by N*period, never to `now`).
 */
static volatile uint8_t  st_periodic_slot_p1;
static volatile uint32_t st_period;          /* auto-reload period (Tsys)      */
static volatile uint32_t st_last_sync;       /* SysTick CNT at last epoch sync */
static volatile uint32_t st_nudge_req;       /* stretch REQUEST (cb-set)       */
static volatile uint32_t st_nudge_applied;   /* stretch applied to LIVE cycle  */

__HIGH_CODE
static inline uint32_t st_systick(void)
{
    return (uint32_t)SysTick->CNT;
}

/* SysTick is the independent free-running timebase for the missed-reload
 * accounting. Ensure it runs (free-running, CMP=max, sysclk) before periodic mode
 * relies on it — otherwise st_systick() reads a frozen counter and the
 * accounting silently does nothing (freerun proof S10). Idempotent.
 * Flash-resident: mode-entry only, not per-fire (2 KB stack-floor budget). */
static inline void st_systick_ensure(void)
{
    if ((SysTick->CTLR & SysTick_CTLR_STE) == 0u) {
        SysTick->CMP  = SysTick_LOAD_RELOAD_Msk;
        SysTick->CNT  = 0u;
        SysTick->CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK;
    }
}

/* Advance the epoch across N elapsed periods using SysTick (reconstructs the
 * delayed-IRQ multi-reload the TMR alone cannot). Carries the sub-period
 * remainder by advancing st_last_sync by N*period (NOT to `now`) so latency
 * never drags the grid. ISR-ONLY in this port (see st_advance). */
__HIGH_CODE
static inline void st_periodic_sync(void)
{
    uint32_t now_st  = st_systick();
    uint32_t elapsed = now_st - st_last_sync;     /* modular, wrap-safe < 2^32 */
    uint32_t n       = elapsed / st_period;
    uint32_t advance;

    if (n == 0u) {
        n = 1u;                                   /* defensive (real CYC_END)  */
    }
    advance = n * st_period;
    st_epoch += advance;
    st_last_sync += advance;                      /* carry remainder           */
}
/* Apply at most ONE pending stretch request to the just-begun cycle — a
 * single CNT_END write at a fixed, early post-reload offset (called from the
 * ISR before the cb). Mid-cycle CNT_END writes RE-PHASE the TMR
 * (bench-measured 2026-07-08), so the write must happen at this constant
 * point in the cycle, never from the cb. When the previous cycle was
 * stretched and no new request is pending, one restore write brings CNT_END
 * back to the flat period. Flash-resident + noinline (SRAM budget); the XIP
 * fetch latency is constant, preserving the fixed-offset argument. */
__attribute__((noinline))
static void st_periodic_apply_nudge(void)
{
    if (st_nudge_req != 0u) {
        st_nudge_applied = st_nudge_req;
        st_nudge_req = 0u;
        R32_TMR_CNT_END = st_period + st_nudge_applied;
    } else if (R32_TMR_CNT_END != st_period) {
        R32_TMR_CNT_END = st_period;
    }
}

__HIGH_CODE
uint32_t st_now(void)   /* exposed for hal_timing_ch570.c (hal_now) */
{
    /* Periodic mode: SysTick-exact, independent of the auto-reloading TMR (a
     * delayed/multi-reload IRQ cannot make st_now() undercount) — and
     * CONTINUOUS: it never freezes while the grid runs. Read the three fields
     * under an IRQ mask so an ISR sync (which advances st_epoch AND
     * st_last_sync together) can't be observed half-applied — callers may be
     * unmasked. */
    if (st_periodic_slot_p1 != 0u) {
        uint32_t irq = __risc_v_disable_irq();
        uint32_t v = st_epoch + (st_systick() - st_last_sync);
        (void)__risc_v_enable_irq(irq);
        return v;
    }
    /* Pending-expiry aware: if the TMR reached CNT_END and auto-cleared but the
     * expiry has not been synced yet (read inside a critical section before
     * st_advance runs), the whole armed interval has already elapsed. Call with
     * the TMR IRQ masked so the flag is stable. */
    if (TMR_GetITFlag(TMR_IT_CYC_END)) {
        return st_epoch + st_armed_delta + R32_TMR_COUNT;
    }
    return st_epoch + R32_TMR_COUNT;
}

__HIGH_CODE
static void st_rearm(void)
{
    uint32_t now = st_now();
    int8_t best = -1;
    uint32_t best_dl = 0u;
    uint8_t i;
    int32_t d;

    /* While a slot is periodic it OWNS the TMR: no hardware touch here. The
     * other slots' deadlines stay bookkeeping (dispatched from the periodic
     * ISR); the periodic-exit paths clear st_periodic_slot_p1 before their
     * own st_rearm, so a one-shot clobber of the auto-reload is structurally
     * impossible. */
    if (st_periodic_slot_p1 != 0u) {
        return;
    }
    for (i = 0u; i < ST_N; i++) {
        if (st_active[i] &&
            (best < 0 || (int32_t)(st_deadline[i] - best_dl) < 0)) {
            best = (int8_t)i;
            best_dl = st_deadline[i];
        }
    }
    if (best < 0) {                 /* nothing active: stop the timer */
        TMR_ITCfg(DISABLE, TMR_IT_CYC_END);
        R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
        TMR_ClearITFlag(TMR_IT_CYC_END);   /* match production stop; no stale flag */
        return;
    }
    d = (int32_t)(best_dl - now);
    if (d < (int32_t)ST_MIN_DELTA) {
        d = (int32_t)ST_MIN_DELTA;  /* due/past -> fire ASAP, never CNT_END=0 */
    }
    st_epoch = now;                 /* re-base; ALL_CLEAR below resets count */
    st_armed_delta = (uint32_t)d;
    R32_TMR_CNT_END = (uint32_t)d;
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    TMR_ClearITFlag(TMR_IT_CYC_END);
    TMR_ITCfg(ENABLE, TMR_IT_CYC_END);
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
}

/*
 * Epoch sync ONLY (no callbacks): if a CNT_END expiry is pending, clear the flag
 * and advance the epoch by the armed interval so st_now() is not left a whole
 * interval behind. Safe to call from ANY context (RF callback, main, IRQ) -- it
 * runs no user callbacks, so it cannot re-enter the RF state machine. MUST run
 * before any non-IRQ st_now()/st_set/st_cancel/st_rearm (the pending-expiry race).
 * The expired soft timer(s) stay active; st_rearm() then arms them at ST_MIN_DELTA
 * so their callbacks fire from the TMR IRQ shortly after (see st_dispatch).
 */
__HIGH_CODE
static void st_advance(void)
{
    /* Periodic mode: NO-OP. The CYC_END flag and the epoch sync are owned
     * exclusively by the ISR (st_periodic_sync); st_now() is SysTick-exact
     * without any flag handling. (The freerun prototype routed mutator-context
     * calls into st_periodic_sync with n forced to 1 — under the ~1250/s
     * connected supervision-refresh mutations that would corrupt the
     * missed-reload accounting. Fixed here by making this a pure no-op.) */
    if (st_periodic_slot_p1 != 0u) {
        return;
    }
    if (TMR_GetITFlag(TMR_IT_CYC_END)) {
        TMR_ClearITFlag(TMR_IT_CYC_END);
        st_epoch += st_armed_delta;
    }
}

/*
 * Run expired callbacks in slot/priority order, re-checking active after each
 * (a callback may set/cancel timers). Called ONLY from the TMR IRQ, so callbacks
 * always run in IRQ context -- matching the existing RF handler assumption.
 */
__HIGH_CODE
static void st_dispatch(void)
{
    uint8_t guard = 0u;
    uint8_t burst = 0u;

    for (;;) {
        uint32_t now = st_now();
        int8_t hit = -1;
        uint8_t i;

        for (i = 0u; i < ST_N; i++) {   /* slot index == priority */
            if ((uint8_t)(i + 1u) == st_periodic_slot_p1) {
                continue;               /* periodic slot fires on auto-reload,
                                         * not via its (stale) one-shot deadline
                                         * — a cb may enter periodic
                                         * mid-dispatch (one-shot -> periodic
                                         * handoff, freerun S12) */
            }
            if (st_active[i] && (int32_t)(now - st_deadline[i]) >= 0) {
                hit = (int8_t)i;
                break;
            }
        }
        if (hit < 0) {
            break;
        }
        st_active[hit] = 0u;            /* clear before cb; cb may re-set */
        burst++;
        if (st_cb[hit] != 0) {
            st_cb[hit]((uint8_t)hit);
        }
        if (++guard > 64u) {           /* safety against a runaway re-set */
            break;
        }
    }
    st_dispatch_burst = burst;
}

/*
 * SEMANTICS NOTE (Codex edge): st_advance() syncs the epoch but does NOT dispatch.
 * If this slot had already expired and its callback had not run yet, re-setting it
 * here overwrites the old deadline -- intentionally SUPPRESSING the stale callback
 * (restart-the-timer semantics; what the RF re-arm paths want). A DIFFERENT slot's
 * expiry is left active and still fires from the TMR IRQ (st_rearm arms it at
 * ST_MIN_DELTA). So "never drop" holds across slots; same-slot restart deliberately
 * drops the superseded callback. Proven by S8. (st_cancel suppresses likewise.)
 */
/* Bounded one-shot drain at grid granularity (the coexistence tick):
 * dispatch every due non-periodic slot, highest priority first, at most
 * ST_N dispatches per tick (a cb that re-arms itself due cannot monopolize
 * the ISR — Codex hardening). One-shot deadlines and the periodic st_now()
 * share one continuous domain, so the compares hold across mode
 * transitions. Called from the TMR IRQ AFTER the poll cb — flash-resident
 * (the XIP fetch only delays one-shot dispatch by µs; SRAM budget).
 * noinline: single-call statics get -Os-inlined back into the SRAM ISR. */
__attribute__((noinline))
static void st_periodic_drain(void)
{
    uint8_t guard;

    for (guard = 0u; guard < (uint8_t)ST_N; guard++) {
        uint32_t now = st_epoch + (st_systick() - st_last_sync);
        int8_t hit = -1;
        uint8_t i;

        for (i = 0u; i < ST_N; i++) {   /* index == priority */
            if ((uint8_t)(i + 1u) == st_periodic_slot_p1) {
                continue;
            }
            if (st_active[i] && (int32_t)(now - st_deadline[i]) >= 0) {
                hit = (int8_t)i;
                break;
            }
        }
        if (hit < 0) {
            break;
        }
        st_active[hit] = 0u;            /* clear before cb; cb may re-set */
        if (st_cb[hit] != 0) {
            st_cb[hit]((uint8_t)hit);
        }
        if (st_periodic_slot_p1 == 0u) {
            return;                     /* a drained cb exited periodic */
        }
    }
}

/* Exit periodic -> one-shot. IRQ-masked, caller verified the slot owns the
 * grid. ORDER MATTERS (freerun S11): read the exact SysTick-domain now WHILE
 * still periodic, rebase the one-shot epoch onto it, stop the auto-reload,
 * clear any latched CYC_END, THEN clear the ownership flag so
 * the one-shot st_now() formula (st_epoch + R32_TMR_COUNT) reads `now`. The
 * caller re-arms (st_rearm) after its own mutation. Flash-resident: runs
 * only on teardown/claim transitions, never per-fire (2 KB stack budget).
 * noinline: keep out of the SRAM-resident mutator callers. */
__attribute__((noinline))
static void st_periodic_exit_locked(void)
{
    uint32_t now = st_epoch + (st_systick() - st_last_sync);

    st_epoch = now;
    st_armed_delta = 0u;
    TMR_ITCfg(DISABLE, TMR_IT_CYC_END);
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    TMR_ClearITFlag(TMR_IT_CYC_END);
    /* F11: drop any pending/applied grid stretch so it can't leak across the
     * teardown and get replayed onto the first cycle of the next periodic
     * session. The TMR is stopped and CYC_END acked above, so no ISR consumes
     * these after this point. */
    st_nudge_req = 0u;
    st_nudge_applied = 0u;
    st_periodic_slot_p1 = 0u;           /* now one-shot domain */
}

__HIGH_CODE
void st_set(uint8_t slot, uint32_t delta, st_cb_t cb)
{
    uint32_t s = __risc_v_disable_irq();
    if ((uint8_t)(slot + 1u) == st_periodic_slot_p1) {
        st_periodic_exit_locked();      /* claim-from-grid: one-shot takes over */
    }
    st_advance();                       /* sync epoch (no cbs) before reading now */
    st_deadline[slot] = st_now() + delta;
    st_cb[slot] = cb;
    st_active[slot] = 1u;
    st_rearm();
    (void)__risc_v_enable_irq(s);
}

/*
 * Like st_set(), but arm to an ABSOLUTE software-epoch deadline (st_now()
 * domain) instead of now+delta. A caller that advances `deadline` by a fixed
 * period each fire holds a phase-stable cadence immune to per-call dispatch
 * latency (the connected-poll grid), unlike now+delta which folds the callback
 * latency into every period. Same same-slot restart semantics as st_set().
 */
__HIGH_CODE
void st_set_at(uint8_t slot, uint32_t deadline, st_cb_t cb)
{
    uint32_t s = __risc_v_disable_irq();
    if ((uint8_t)(slot + 1u) == st_periodic_slot_p1) {
        st_periodic_exit_locked();      /* claim-from-grid: one-shot takes over */
    }
    st_advance();                       /* sync epoch (no cbs) before reading now */
    st_deadline[slot] = deadline;       /* absolute grid deadline, not now+delta */
    st_cb[slot] = cb;
    st_active[slot] = 1u;
    st_rearm();
    (void)__risc_v_enable_irq(s);
}

__HIGH_CODE
void st_cancel(uint8_t slot)
{
    uint32_t s = __risc_v_disable_irq();
    if ((uint8_t)(slot + 1u) == st_periodic_slot_p1) {
        /* Exit periodic -> one-shot; remaining active one-shot slots get the
         * hardware back at full precision. */
        st_active[slot] = 0u;
        st_periodic_exit_locked();
        st_rearm();
        (void)__risc_v_enable_irq(s);
        return;
    }
    st_advance();                       /* sync epoch (no cbs) before mutating */
    st_active[slot] = 0u;
    st_rearm();
    (void)__risc_v_enable_irq(s);
}

__INTERRUPT
__HIGH_CODE
void TMR_IRQHandler(void)
{
    if (st_periodic_slot_p1 != 0u) {
        uint8_t slot = (uint8_t)(st_periodic_slot_p1 - 1u);

        if (st_nudge_applied != 0u) {
            /* The just-ended cycle ran stretched: consume exactly
             * period+applied into the epoch/baseline. */
            uint32_t adv = st_period + st_nudge_applied;

            st_epoch += adv;
            st_last_sync += adv;
            st_nudge_applied = 0u;
        } else {
            st_periodic_sync();          /* epoch += N*period (SysTick-exact);
                                          * ONE cb per ISR entry even if N>1 —
                                          * missed periods are counted, never
                                          * replayed as a catch-up burst */
        }
        TMR_ClearITFlag(TMR_IT_CYC_END); /* ack the auto-reload */
        st_periodic_apply_nudge();       /* one CNT_END write per cycle at
                                          * this fixed early offset (flash;
                                          * constant XIP latency) */
        if (st_active[slot] && st_cb[slot] != 0) {
            /* Fire the grid owner (the poll TX) FIRST — phase-critical. The
             * cb may exit periodic (st_cancel / claim-from-grid arm inside):
             * those paths leave the TMR correctly re-armed for the one-shot
             * mux, so just return. If it stays periodic the hardware
             * auto-reloads — no software re-arm, the phase-lock INVARIANT. */
            st_cb[slot](slot);
        }
        if (st_periodic_slot_p1 != 0u) {
            st_periodic_drain();         /* flash-resident: runs AFTER the
                                          * phase-critical poll TX */
        }
        return;                          /* still periodic: hardware
                                          * auto-reloads; exited: the exit
                                          * path already re-armed one-shot */
    }
    st_advance();      /* sync epoch for this expiry */
    st_dispatch();     /* run expired callbacks (IRQ context only) */
    st_rearm();
}

/* Flash-resident: mode entry runs once per pair/re-key TX, not per fire.
 * The flash-XIP fetch adds a fixed few-us skew between the ALL_CLEAR anchor
 * and the RF TX — same shape/sign as the CH592's rf_tmr0_start->RF_Tx gap;
 * the keyboard servo absorbs it (SRAM stack-floor budget). */
void st_set_periodic(uint8_t slot, uint32_t period, st_cb_t cb)
{
    uint32_t s = __risc_v_disable_irq();
    uint32_t now;

    st_advance();                   /* sync any pending one-shot expiry (no-op
                                     * if already periodic) */
    now = st_now();                 /* whichever domain is live: continuity */
    st_systick_ensure();            /* the missed-reload timebase MUST run */
    if (st_periodic_slot_p1 != 0u &&
        st_periodic_slot_p1 != (uint8_t)(slot + 1u)) {
        /* Takeover from a different owner (protocol keeps the cadence slots
         * mutually exclusive; this is defensive): deactivate the old owner so
         * its stale deadline can't be drained as a due one-shot. */
        st_active[st_periodic_slot_p1 - 1u] = 0u;
    }
    st_cb[slot] = cb;
    st_active[slot] = 1u;
    st_period = period;
    st_epoch = now;                 /* periodic now == prior now */
    st_last_sync = st_systick();    /* independent-timebase baseline */
    st_periodic_slot_p1 = (uint8_t)(slot + 1u);
    /* F11: a fresh periodic session (or a re-anchor of the owning slot) starts
     * with no pending/applied stretch — the flat CNT_END/ALL_CLEAR anchor below
     * physically discards any carried-over stretch, so the accounting must too. */
    st_nudge_req = 0u;
    st_nudge_applied = 0u;
    /* Arm the TMR to AUTO-RELOAD at `period`. The ALL_CLEAR here IS the grid
     * phase anchor — the caller sits immediately before the anchoring RF TX
     * (the CH592 rf_tmr0_start-before-RF_Tx shape). Re-entrant call on the
     * owning slot lands here too and RE-ANCHORS (stock event-0x40). */
    st_armed_delta = period;
    R32_TMR_CNT_END = period;
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    TMR_ClearITFlag(TMR_IT_CYC_END);
    TMR_ITCfg(ENABLE, TMR_IT_CYC_END);
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
    (void)__risc_v_enable_irq(s);
}

/* Stretch the CURRENT grid period by `delta` ticks, ONCE: CNT_END is raised
 * for this cycle and restored by the ISR after the stretched reload; the
 * epoch/baseline accounting consumes the exact stretch, so the grid PHASE
 * shifts +delta permanently with zero cumulative error. This is the
 * phase-ACQUISITION primitive (hw-poll-grid): the v0.96 software timer's
 * +3380 ppm drift accidentally swept the poll phase across the slot until
 * the sleeping keyboard's narrow RX window was found (its ±1-tick/packet
 * servo then locks); the crystal-flat grid needs the sweep made deliberate.
 * Positive delta only (CNT_END may only be raised while counting); refused
 * if a stretch is already pending, a CYC_END is latched, or the cycle is
 * past its midpoint (the stretch must provably apply to THIS cycle so the
 * ISR's +period+delta accounting is exact). Callers run right after a fire
 * (the poll cb), where COUNT is tiny. Flash-resident (SRAM budget). */
uint8_t st_periodic_nudge(uint32_t delta)
{
    uint32_t s = __risc_v_disable_irq();
    uint8_t accepted = 0u;

    /* REQUEST-only: no hardware write here. Bench-measured 2026-07-08
     * (diag7): a mid-cycle R32_TMR_CNT_END write RE-PHASES the TMR (the
     * count restarts at the write instant) — the original write-from-cb
     * nudge therefore re-anchored the grid to the cb's own time every
     * cycle and the phase never actually swept (deltas sat at
     * period + cb-latency, spread 1.5 us). The ISR applies one request
     * per cycle at a fixed early offset after the reload, which is
     * phase-stable under either write semantic. */
    if (st_periodic_slot_p1 != 0u && st_nudge_req == 0u && delta != 0u) {
        st_nudge_req = delta;
        accepted = 1u;
    }
    (void)__risc_v_enable_irq(s);
    return accepted;      /* callers count ACCEPTED sweep steps (Codex) */
}

/* Flash-resident: the promote runs once per session (SRAM budget). */
uint8_t st_periodic_transfer(uint8_t from, uint8_t to, st_cb_t cb)
{
    uint32_t s = __risc_v_disable_irq();

    if ((uint8_t)(from + 1u) != st_periodic_slot_p1) {
        (void)__risc_v_enable_irq(s);
        return 0u;                  /* caller arms fresh as fallback */
    }
    /* The PROMOTE: move grid ownership and install the new dispatch target
     * with ZERO hardware writes — the running, TX-anchored auto-reload IS the
     * new cadence (the CH592 phase-lock INVARIANT, enforced structurally).
     * from == to just swaps the callback. */
    if (from != to) {
        st_active[from] = 0u;
    }
    st_cb[to] = cb;
    st_active[to] = 1u;
    st_periodic_slot_p1 = (uint8_t)(to + 1u);
    (void)__risc_v_enable_irq(s);
    return 1u;
}


int main(void)
{
    uint8_t reset_status;

    /* MUST be first: hash the pristine power-on SRAM into the session-AA seed. */
    ch570_capture_boot_entropy();


    reset_status = R8_RESET_STATUS;
    ch570_boot_reset_status = reset_status;
    /* N23: reconcile the retained fatal record and hardware-cleared reset
     * epoch before clock/peripheral initialization. The assembly handler owns
     * the narrower pre-hook window without trusting stale SRAM. */
    dongle_fault_boot(reset_status);


#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(CH570_SYSCLK_SOURCE);


    /*
     * Source the 32K low-speed domain from the internal LSI RC. The production
     * board has only the 32 MHz HSE crystal (no external 32.768 kHz), and this
     * CH57x SDK has no external-LSE path -- the 32K domain is internal-LSI by
     * construction -- so this is a defensive, self-documenting power-on: if any
     * future code consults the low-speed clock it finds the internal RC, never
     * an absent crystal. High-speed (CPU/USB/RF) stays on HSE/PLL, unaffected.
     */
    sys_safe_access_enable();
    R8_LSI_CONFIG |= RB_CLK_LSI_PON;
    sys_safe_access_disable();

    /* Fold a weak independent clock-jitter sample into the session-AA entropy
     * (the SRAM hash was captured first, above). The jitter source IS the LSI
     * RC, so this only runs when the LSI is powered — no LSI, no jitter, and no
     * wasted spin. */
    ch570_mix_jitter_entropy();

    /*
     * usb_start() asserts the D+ pull-up, which is what triggers the intended
     * full-speed attach. Keep it last.
     *
     * Moving it above the two calls before it was tried on the bench and failed
     * enumeration in all five trials taken - the host saw the attach and then
     * failed its control transfers -
     *
     *   usb 7-1.1: device descriptor read/64, error -32
     *   usb 7-1.1: device not accepting address, error -71
     *   usb 7-1-port1: unable to enumerate USB device
     *
     * The MECHANISM IS NOT ESTABLISHED. The obvious guess - that the LSI
     * cold-start wait in ch570_mix_jitter_entropy() starves enumeration - looks
     * unlikely, though not impossible: USB is interrupt-driven (USB_IRQHandler,
     * enabled inside USB_DevInit), a busy foreground does not stop the ISR, the
     * USB path never touches SysTick, and the safe-access sequence masks
     * interrupts only around its own register writes. Distinguishing "SETUP
     * never serviced" from corrupted signalling needs a bus analyser or an
     * ISR-entry GPIO pulse, and neither has been done. What is recorded here is
     * the observation, not a cause.
     *
     * The move was made to shorten the detached interval ahead of a stage-1
     * change that has since been reverted for want of any evidence of benefit
     * (see VALIDATION.md, "CH570 attaches as LOW SPEED"), so nothing now argues
     * for it at all. Do not reorder this without re-testing enumeration on
     * hardware.
     */
    usb_start();

    /*
     * Bus-powered dongle: the CPU busy-polls forever and NEVER sleeps. There is
     * deliberately no LowPower_Idle()/__WFI()/deep-sleep here -- adding one is a
     * conscious opt-out of CH570_DONGLE_NEVER_SLEEP, not an accident. Deep sleep
     * would also reintroduce a low-speed-clock wake dependency the production
     * 32-MHz-HSE-only board is built to avoid.
     */
    while (1) {
        USB_PollEP6();
        /* Drive a requested reboot into OpenBoot (0x85 EnterBootloader):
         * quiesce RF, drain the final EP6 reply, then reset. */
        IAP_Service();
        USB_ServiceRemoteWake();
        usb_hid_forward_pump();
        if (!rf_started && USB_IsConfigured()) {
            DelayMs(CH570_RF_START_AFTER_USB_CONFIG_DELAY_MS);
            if (!USB_IsConfigured()) {
                continue;
            }
            rf_start();
        }
        if (rf_started) {
            /* Forward the host's HID LED output report (CapsLock/NumLock/
             * ScrollLock) to the keyboard on change. SET_REPORT latches
             * usb_led_state in the USB ISR; we read it here (foreground) and
             * queue the relay via RF_SetLEDState (RF sends it on the connected
             * poll cadence). last_led starts 0xFF (impossible 3-bit value) so the
             * first iteration after RF start syncs the current state. */
            {
                static uint8_t last_led = 0xFFu;
                uint8_t led = (uint8_t)(USB_GetLEDState() & 0x07u);
                if (led != last_led) {
                    last_led = led;
                    RF_SetLEDState(led);
                }
            }
            RF_TaskPump();
        }
    }
}
