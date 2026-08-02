/*
 * Bridge75 Open-Source Dongle Firmware
 * CH592F (CH59x) USB pin/PHY HAL.
 *
 * On CH59x the USB analog IO and D+ pull-up live in R16_PIN_ANALOG_IE
 * (RB_PIN_USB_IE / RB_PIN_USB_DP_PU) and are NOT muxed with the two-wire debug
 * pins, so there is no pre-detach to do. The end-of-init re-attach briefly drops
 * and re-asserts the D+ pull-up so a host still holding the prior WCH-ISP/SWD
 * session re-probes our descriptors (mirrors the SDK DevWakeup() toggle).
 */
#include "dongle_chip.h"
#include "dongle_usb_hal.h"

void hal_usb_pins_predetach(void)
{
    /* CH59x: USB IO is not muxed with the debug pins — nothing to release. */
}

void hal_usb_pins_enable(void)
{
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;
}

void hal_usb_pins_reattach(void)
{
    /* Force a disconnect-then-reconnect so the host sees a fresh plug event and
     * starts enumeration from scratch. Without this the host's USB-C controller
     * may still regard this port as the persistent WCH-ISP/SWD session and never
     * re-probe our descriptors. */
    R16_PIN_ANALOG_IE &= ~RB_PIN_USB_DP_PU;
    /* Hold D+ low long enough for the host to debounce a real detach. The old
     * `for (i < 100000) nop` spin was ~8-10 ms at 60 MHz but its duration was
     * an accident of clock and -O level; a named-duration delay expresses the
     * electrical requirement directly and survives ports/flag changes. */
    DelayMs(8);
    R16_PIN_ANALOG_IE |= RB_PIN_USB_DP_PU;
}

void hal_usb_remote_wakeup(void)
{
    /* WCH device remote-wakeup K-state (SDK USB/Device DevWakeup): drop the D+
     * pull-up, switch the port to low-speed so it drives the opposite line
     * (K-state vs full-speed idle J), hold ~2 ms (USB resume is 1-15 ms), then
     * restore full speed and the pull-up. */
    R16_PIN_ANALOG_IE &= ~RB_PIN_USB_DP_PU;
    R8_UDEV_CTRL |= RB_UD_LOW_SPEED;
    DelayMs(2);
    R8_UDEV_CTRL &= ~RB_UD_LOW_SPEED;
    R16_PIN_ANALOG_IE |= RB_PIN_USB_DP_PU;
}
