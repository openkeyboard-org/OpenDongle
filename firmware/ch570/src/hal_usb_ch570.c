/*
 * Bridge75 Open-Source Dongle Firmware
 * CH570 (CH57x) USB pin/PHY HAL.
 *
 * CH570 shares PA0/PA1 between USB and two-wire debug. USB-capable images must
 * release the debug mux (RB_PIN_DEBUG_EN) before bringing up the USB analog IO,
 * and the analog enable + D+ pull-up live in R16_PIN_ALTERNATE (RB_PIN_USB_EN /
 * RB_UDP_PU_EN) rather than R16_PIN_ANALOG_IE. The validated v0.83 init does NOT
 * perform an end-of-init D+ re-attach dance, so hal_usb_pins_reattach() is a
 * no-op here.
 */
#include "dongle_chip.h"
#include "dongle_usb_hal.h"

void hal_usb_pins_predetach(void)
{
}

void hal_usb_pins_enable(void)
{
    R16_PIN_ALTERNATE &= (uint16_t)~RB_PIN_DEBUG_EN;
    R16_PIN_ALTERNATE |= RB_PIN_USB_EN | RB_UDP_PU_EN;
}

void hal_usb_pins_reattach(void)
{
    /* CH570: no end-of-init re-attach dance (validated v0.83). */
}

void hal_usb_remote_wakeup(void)
{
    /* Device remote-wakeup K-state, CH570 flavour: the D+ pull-up is
     * RB_UDP_PU_EN in R16_PIN_ALTERNATE (not R16_PIN_ANALOG_IE as on
     * CH59x/CH58x). Drop it, drive K via low-speed for ~2 ms, restore.
     * Untested on the bench: CH570's product main loop never sleeps, so its
     * own port is not host-suspended in normal use; provided so the shared
     * driver's remote-wake path links and behaves consistently. */
    R16_PIN_ALTERNATE &= ~RB_UDP_PU_EN;
    R8_UDEV_CTRL |= RB_UD_LOW_SPEED;
    DelayMs(2);
    R8_UDEV_CTRL &= ~RB_UD_LOW_SPEED;
    R16_PIN_ALTERNATE |= RB_UDP_PU_EN;
}
