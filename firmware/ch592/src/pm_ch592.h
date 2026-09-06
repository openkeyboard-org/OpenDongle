/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * CH592 Tier-1 power management: SEVONPEND-WFE main-loop idle, TMR3 heartbeat,
 * unused-peripheral clock gates and the GPIO park (pm_ch592.c).
 *
 * Every entry point exists only under its PM_* knob (firmware/ch592/Makefile),
 * so the all-off build is byte-identical to a tree without this file.
 */
#ifndef PM_CH592_H
#define PM_CH592_H

#include <stdint.h>

#if DONGLE_PM_IDLE
/* Per-iteration "work happened" latches (why: pm_ch592.c header comment).
 * Plain byte stores from every context; cleared only by pm_loop_top at the
 * top of Main_Circulation. */
extern volatile uint8_t dongle_pm_post;   /* set by every hal_event_post   */
extern volatile uint8_t dongle_pm_ran;    /* set at RF_ProcessEvent entry  */
/* The host LED state poll_usb_led_state (main.c) last relayed; the masked
 * admission re-check compares the live USB snapshot against it. */
extern volatile uint8_t dongle_usb_led_last;

/* PB15 pulls/DIR and CFG_DEBUG_EN as ROM/OpenBoot left them (page 6 [43]).
 * Call right after SetSysClock, before pm_gpio_park or anything else that
 * touches a pin. */
void pm_boot_snapshot(void);
/* Free-running TMR3 CYC_END heartbeat at DONGLE_PM_HEARTBEAT_US, PFIC
 * priority 0xF0. Call before RF_TaskInit. */
void pm_heartbeat_init(void);
/* Top of every Main_Circulation iteration: returns the entry work (a post
 * that landed after the previous masked observation, pm_ch592.c) and clears
 * the latches. Call before TMOS_SystemProcess. */
uint8_t pm_loop_top(void);
/* One idle attempt at the end of a Main_Circulation iteration, with the value
 * pm_loop_top returned for it. */
void pm_idle_try(uint8_t entry_work);
#endif

#if DONGLE_PM_GPIO_PARK
/* Park the CH592D no-connects; call right after SetSysClock, before
 * CH59x_BLEInit (section 4 of the plan). */
void pm_gpio_park(void);
#endif
#if DONGLE_PM_CLK_GATE
/* PWR_PeriphClkCfg(DISABLE, 0x4DF6); call after HAL_Init, before hal_rf_init. */
void pm_clock_gate(void);
#endif
#if DONGLE_PM_USB_DIGIN_OFF
/* Disable the PB10/PB11 digital input buffers; call after USB_DevInit. */
void pm_usb_digin_off(void);
#endif

#endif /* PM_CH592_H */
