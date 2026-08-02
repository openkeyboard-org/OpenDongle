#ifndef DONGLE_TARGET_H
#define DONGLE_TARGET_H

#include "version.h"

#define DONGLE_FW_VERSION FW_VERSION
#define DONGLE_CHIP_FAMILY_ID 0x70u

/* CODEREVIEW F01: the app carries the ODG2 identity header (family/kind/base/
 * magic at image offset 0x20; dongle_id.c stamps it). DONGLE_IAP_APP_BASE must
 * equal this chip's app ORIGIN(FLASH) — 0x2000 under OpenBoot, which owns
 * [0, 0x2000) and performs all flash updates in-bootloader (linker-asserted
 * in link.ld). */
#define DONGLE_IAP_IMAGE_ID 1
#define DONGLE_IAP_APP_BASE 0x2000u

/* The Bridge75 needs a short PAIR_BCAST-to-ACK turnaround gap. */
#define RF_CH570_PAIR_ACK_PRE_TX_TMOS 4u    /* 4 x 625 us = 2500 us */

/* P4 deaf-camp guard retry cadence. A terminal camp (give-up reacquire,
 * closed boot window) and the connected self-heal loop arm RF_Rx and then
 * lean on the radio's 30 ms camp timeout to re-drive RF_EVT_RX_RESTART; a
 * FAILED arm never starts the radio, so no timeout fires and the camp is
 * silently deaf forever. On a failed arm the guard schedules an independent
 * RF_EVT_RX_RESTART this many TMOS units out — comfortably above the 30 ms
 * camp window so retries don't stack, ~20/s to pace a persistent failure.
 * CAVEAT: hal_event_post_delayed() has only two delay slots; if both are
 * already held (both pair-prep PAIR_PREP and a pair-ACK burst in flight during
 * an active pairing attempt) the post DEGRADES to immediate and the retry
 * paces at the main-loop rate until a slot frees. That window is bounded (the
 * held deadlines mature in tens of ms) and self-limiting (the radio is already
 * broken and PAIR_PREP re-drives the scan there), so it thrashes rather than
 * hot-locks. The terminal camps that rely on the guard as their SOLE recovery
 * (give-up, closed boot window) first cancel PAIR_PREP and abort the burst, so
 * both slots are free there. See rf_arm_retry_if_failed() in rf_task.c. */
#define RF_ARM_RETRY_TMOS 80u               /* 80 x 625 us = 50 ms */
#define CH570_RF_START_AFTER_USB_CONFIG_DELAY_MS 1000u

/* CH570 remote wake: advertise DEVICE_REMOTE_WAKEUP and let the shared USB
 * driver arm/drive wake via the CH570 main-loop service call. */
#define DONGLE_USB_REMOTE_WAKEUP 1

/* Tsys timing constants for the shared rf_task (P4). The CH570 Tsys clock is
 * CH570_SYSCLK_HZ (fixed 100 MHz — the only supported CH570 clock) —
 * both divide the microsecond and the 1/32000 s protocol tick exactly. */
#define HAL_TICKS_PER_US        (CH570_SYSCLK_HZ / 1000000u)
#define HAL_TMOS_UNIT_TICKS     (625u * HAL_TICKS_PER_US)
#define HAL_TICKS_PER_PROTO_TICK (HAL_TICKS_PER_US * 125u / 4u)
_Static_assert(HAL_TICKS_PER_PROTO_TICK * 32000u
               == HAL_TICKS_PER_US * 1000000u,
               "protocol tick must divide the Tsys clock exactly");
_Static_assert((CH570_SYSCLK_HZ % 1000000u) == 0u,
               "CH570 Tsys clock must be a whole MHz");

/* RF-task executor model: the CH570 has no TMOS — the shared rf_task is
 * driven by the polled pump (hal_dispatch_ch570 + RF_TaskPump). */
#define RF_TASK_EXECUTOR_TMOS 0

#define DONGLE_HAS_RF 1

/* OpenBoot's boot record owns 0x3B000, so the bond moved to the ex-spare
 * page at 0x3A000 — still outside the OBP-clamped app region, which ends at
 * 0x3A000 (link.ld). */
#define CH570_BOND_FLASH_ADDR 0x3A000u
#define CH570_BOND_FLASH_SIZE 0x1000u

/* What erased flash reads as through the XIP/FLASH_ROM descrambler (the
 * fixed per-4-byte XOR key 56 42 06 0C over physical 0xFF cells). Used by
 * dongle_nv_is_erased() to controller-VERIFY the bond page really erased
 * (F26: the XIP view can serve stale bytes after a controller write). */
#define CH570_FLASH_ERASED_WORD 0xF3F9BDA9u

#endif /* DONGLE_TARGET_H */
