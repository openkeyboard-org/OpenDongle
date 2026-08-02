/*
 * dongle_chip.h — per-tree SDK common-header shim (CH592F / CH59x).
 *
 * The shared USB device driver (fw-common/src/usb_device.c) includes
 * "dongle_chip.h" instead of a chip-specific SDK header so one source file can
 * build against either the CH59x or CH57x SDK. Each target tree provides its own
 * copy of this shim pointing at that chip's SDK common header.
 */
#ifndef DONGLE_CHIP_H
#define DONGLE_CHIP_H

#include "CH59x_common.h"

/* Semantic SRAM-code placement classes. Keep IRQ/RF/flash-operation paths in
 * SRAM, but allow explicitly cold helpers to stay in flash. */
#define DONGLE_HIGHCODE_IRQ      __HIGH_CODE
#define DONGLE_HIGHCODE_RF_HOT   __HIGH_CODE
#define DONGLE_HIGHCODE_FLASH_OP __HIGH_CODE
#define DONGLE_HIGHCODE_COLD

/* USB ISR code placement. CH592F has 26 KB SRAM and deliberately runs the USB
 * ISR from SRAM (.highcode) for lower interrupt latency — the validated v0.80
 * behavior. (CH570 with only 12 KB SRAM overrides this to keep the ISR in
 * flash.) */
#define USB_ISR_HIGHCODE DONGLE_HIGHCODE_IRQ

#endif /* DONGLE_CHIP_H */
