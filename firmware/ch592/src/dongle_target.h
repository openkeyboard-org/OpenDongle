#ifndef DONGLE_TARGET_H
#define DONGLE_TARGET_H

#include "version.h"

#define DONGLE_FW_VERSION FW_VERSION
#define DONGLE_CHIP_FAMILY_ID 0x92u

/* CODEREVIEW F01: the app carries the ODG2 identity header (family/kind/base/
 * magic at image offset 0x20; dongle_id.c stamps it). DONGLE_IAP_APP_BASE must
 * equal this chip's app ORIGIN(FLASH), which under OpenBoot's A/B slots is
 * the base of the slot THIS image was linked for, not a fixed address.
 * OpenBoot owns [0, 0x2000) and performs all flash updates in-bootloader
 * (linker-asserted in link.ld). */
#define DONGLE_IAP_IMAGE_ID 1
/* The app's load address IS its slot's base, so this must move with the slot:
 * dongle_id.c stamps it into the ODG2 header, and a slot-B image advertising
 * 0x2000 would be rejected by the host tooling that checks the two agree. */
#ifdef OPENBOOT_SLOT_BASE
#define DONGLE_IAP_APP_BASE ((uint32_t)(OPENBOOT_SLOT_BASE))
#else
/* Only reachable where openboot_app.c is NOT compiled: it #errors without
 * OPENBOOT_SLOT_BASE and is in APP_SRC for both chips, so no firmware build
 * can reach this line. Host/standalone compiles of individual sources can. */
#define DONGLE_IAP_APP_BASE 0x2000u
#endif

/* CH592 is the validated power-management target: it advertises AND services
 * USB remote wakeup (main loop calls USB_ServiceRemoteWake). */
#define DONGLE_USB_REMOTE_WAKEUP 1

/* N23 post-reset visibility: expose the read-only fault page. */
#define DONGLE_FAULT_ENABLE 1

/* CH570 alone needs a delayed pair-ACK transmit. */
#define RF_CH570_PAIR_ACK_PRE_TX_TMOS 0u

/* RF timing base (P3a): Tsys = 60 MHz (main.c SetSysClock(CLK_SOURCE_PLL_60MHz)).
 * HAL_TMOS_UNIT_TICKS converts one 625 us TMOS timer unit into Tsys ticks; the
 * hal_timing seam takes Tsys deltas and the CH592 shim divides back to TMOS
 * units for its TMOS-timer-backed slots (exact for every deadline in use). */
#define HAL_TICKS_PER_US    60u
#define HAL_TMOS_UNIT_TICKS (625u * HAL_TICKS_PER_US)
/* Tsys ticks per PROTOCOL TICK (1/32000 s — the on-air timing unit): exactly
 * 1875 at 60 MHz. The hop clock, the TMR0 poll cadence, and (P3a) the
 * supervision lapse measurement all convert through this one constant. */
#define HAL_TICKS_PER_PROTO_TICK (HAL_TICKS_PER_US * 125u / 4u)

/* RF-task executor model: 1 = TMOS (the CH59x-family task loop registers
 * RF_ProcessEvent and hal_dispatch rides tmos_set_event); 0 = the CH570
 * polled pump (RF_TaskPump drains a pending mask each main-loop pass). */
#define RF_TASK_EXECUTOR_TMOS 1

/* Gate the durable bond write on a confirmed connected RX from the peer (Issue
 * #23). Always on for CH59x — this is the executor that already shipped the
 * confirm-before-persist state machine; the macro now names the feature so the
 * CH570 build can share it. rf_task.c #errors if this is undefined. */
#define RF_CONFIRM_BEFORE_PERSIST 1

/* Optional AES-128-CCM link decryption (rf_crypt). Negotiated per bond and inert
 * until a key is provisioned, so it does not change plaintext behaviour. */
#define DONGLE_RF_CRYPT 1

/* CODEREVIEW P4 (RF-liveness parity, ported from CH570): reschedule delay for a
 * failed RF_Rx/RF_Tx arm at a terminal camp (rf_arm_retry_if_failed). Same
 * 80 x 625 us = 50 ms as CH570. A 0 would hot-spin the delayed RF_EVT_RX_RESTART
 * on a persistent failure, so rf_task.c #errors if this is undefined. */
#define RF_ARM_RETRY_TMOS 80u

_Static_assert(HAL_TICKS_PER_PROTO_TICK * 32000u
               == HAL_TICKS_PER_US * 1000000u,
               "protocol tick must divide the Tsys clock exactly");

#endif /* DONGLE_TARGET_H */
