/*
 * Bridge75 Open-Source Dongle Firmware
 * USB pin/PHY HAL seam.
 *
 * The shared USB device driver (fw-common/src/usb_device.c) is chip-agnostic
 * except for the analog-pin enable path, which differs by SoC family:
 *   - CH59x/CH58x: USB IO + D+ pull-up live in R16_PIN_ANALOG_IE
 *     (RB_PIN_USB_IE / RB_PIN_USB_DP_PU).
 *   - CH570 (CH57x): USB shares PA0/PA1 with two-wire debug; enable lives in
 *     R16_PIN_ALTERNATE (RB_PIN_USB_EN / RB_UDP_PU_EN) and the debug mux must be
 *     released first (RB_PIN_DEBUG_EN) before the analog USB IO comes up.
 *
 * Each target provides exactly one implementation (hal_usb_<chip>.c).
 */
#ifndef DONGLE_USB_HAL_H
#define DONGLE_USB_HAL_H

#include <stdint.h>

/* Called once near the start of USB_DevInit, before endpoint setup. On CH570
 * this releases the PA0/PA1 debug mux for USB (and runs any pull-up dance);
 * on CH59x/CH58x it is a no-op. */
void hal_usb_pins_predetach(void);

/* Called after the USB controller/endpoints are configured to enable the analog
 * USB IO and assert the D+ pull-up so the host enumerates the device. */
void hal_usb_pins_enable(void);

/* Called once at the end of USB_DevInit. On CH59x/CH58x this performs a brief
 * D+ disconnect/reconnect so a host that still holds the prior WCH-ISP/SWD
 * session re-probes our descriptors. On CH570 it is a no-op (the validated
 * v0.83 init does no end-of-init re-attach dance). */
void hal_usb_pins_reattach(void);

/* Drive USB resume (K-state) signalling to request that the host wake from
 * suspend. Blocks for the ~2 ms K-state per the WCH DevWakeup sequence, so the
 * shared driver only calls it from the main loop (foreground), never from the
 * RF/TMR HID-delivery path. Caller guarantees the bus is suspended and the host
 * armed DEVICE_REMOTE_WAKEUP. CH59x/CH58x drop RB_PIN_USB_DP_PU around the
 * K-state; CH570 drops RB_UDP_PU_EN. */
void hal_usb_remote_wakeup(void);

#endif /* DONGLE_USB_HAL_H */
