/*
 * dongle_chip.h — per-tree SDK common-header shim (CH570 / CH57x).
 *
 * The shared USB device driver (fw-common/src/usb_device.c) includes
 * "dongle_chip.h" instead of a chip-specific SDK header so one source file can
 * build against either the CH59x or CH57x SDK. Each target tree provides its own
 * copy of this shim pointing at that chip's SDK common header.
 */
#ifndef DONGLE_CHIP_H
#define DONGLE_CHIP_H

#include "CH57x_common.h"

/* Semantic SRAM-code placement classes. CH570 keeps cold/shared USB code out of
 * .highcode by default, but still marks RF/IRQ/flash-operation paths with the
 * same intent labels used by the larger chips. */
#define DONGLE_HIGHCODE_IRQ      __HIGH_CODE
#define DONGLE_HIGHCODE_RF_HOT   __HIGH_CODE
#define DONGLE_HIGHCODE_FLASH_OP __HIGH_CODE
#define DONGLE_HIGHCODE_COLD

/* USB hot-path code placement. WCH's RF_UartDongle keeps its USB ISR in
 * .highcode; CH570 product defaults keep the composite-HID USB path in flash to
 * preserve the 12 KB SRAM budget. Test builds can opt into SRAM placement for
 * latency A/B runs. */
#define CH570_USB_ISR_HIGHCODE 0

#define CH570_USB_HID_SEND_HIGHCODE 0

#define USB_ISR_HIGHCODE DONGLE_HIGHCODE_COLD

#define USB_HID_SEND_HIGHCODE DONGLE_HIGHCODE_COLD

#endif /* DONGLE_CHIP_H */
