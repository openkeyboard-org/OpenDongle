/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * Target: WCH CH592F (QingKe RISC-V4F, 60MHz)
 *
 * Build requirements (verified 2026-05-17 via ch592f-irq-test/wch-rfphy-rebuild):
 *
 *   The CH59x BLE library (LIBCH59xBLE.a) is compiled with WCH's custom
 *   `WCH-Interrupt-fast` interrupt attribute and depends on the specific
 *   register-save/restore behavior it produces. Mainline GCC's plain
 *   `__attribute__((interrupt))` (what INT_SOFT=1 selects) produces a
 *   binary that wedges before any IRQ can be delivered. Only the MRS
 *   toolchain (riscv32-wch-elf-gcc 15.2; see firmware/README.md)
 *   supports `WCH-Interrupt-fast` natively, so this firmware MUST be
 *   built with MRS + INT_SOFT=0. The Makefile defaults reflect this.
 *
 * CH592F dongle product entry point: clock init, optional UART1 debug, and
 * TMOS/HAL/RF init, plus -- under RF_WITH_USB (the default product build) --
 * the USB HID composite device and IAP reflash interface. Forwards decoded RF
 * HID reports to USB and dispatches host LED / IAP traffic from the main loop.
 * (Originally an OQ7 RF-only SRAM-dump port; the USB HID + IAP product path is
 * now wired here behind #if RF_WITH_USB.)
 */

/* CODEREVIEW N03: the Makefile's INT_SOFT guard checks only the make VARIABLE, so
 * EXTRA_CFLAGS=-DINT_SOFT=<anything> (even =0) slips past it while the SDK's
 * `#ifdef INT_SOFT` still selects the software-ISR ABI that wedges the CH59x BLE/RF
 * paths (proven: the negative build changed TMR0_IRQHandler/USB handler prologues).
 * Fail the build LOUDLY on ANY INT_SOFT definition. The shipping build leaves it
 * undefined, so this never fires there. */
#ifdef INT_SOFT
#error "INT_SOFT must be UNDEFINED for CH592F: the software-ISR ABI wedges LIBCH59xBLE.a; build with the pinned MounRiver WCH toolchain and the native WCH-Interrupt-fast attribute."
#endif

#include "CONFIG.h"
#include "HAL.h"
#include "CH59x_pwr.h"
#include "rf_task.h"
#include "hal_rf.h"
#include "version.h"
#include "dongle_chip.h"
#include "dongle_platform.h"
#include "usb_device.h"
#include "iap.h"
#include "uart_diag.h"

/*
 * Production board has ONLY the 32 MHz HSE crystal -- no external 32.768 kHz.
 * The 32K low-speed domain must be the internal LSI; CLK_OSC32K==0 selects the
 * (dead) external-LSE branch in HAL_TimeInit, which would have nothing to drive
 * it on the production board. The Makefile pins CLK_OSC32K=1.
 */
#if (CLK_OSC32K == 0)
#error "CH592F dongle requires internal LSI (CLK_OSC32K != 0): production board has no 32.768 kHz crystal"
#endif

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if (defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif


/* Forward an RF HID report to USB, routed by the on-air report tag
 * (all bench-captured 2026-06-13 from a production keyboard):
 *   0xA1 boot keyboard  -> EP1 (8-byte [mod][rsv][k0..k5])
 *   0xA3 consumer/media -> EP3 composite, report ID 1 + 16-bit usage LE
 *                          (vol-up = [E9 00])
 *   0xA8 mouse          -> EP2 (5-byte [btn][X][Y][wheel][pan], verbatim)
 * System-control would be a further tag (not yet captured) -> dropped.
 * Runs in the RF task/callback context — the USB_Send* calls are a few
 * register writes and own endpoint readiness (configured/suspended). */
DONGLE_HIGHCODE_RF_HOT
static void usb_hid_callback(uint8_t tag, const uint8_t *data, uint8_t len)
{
    if (tag == RF_PROTO_HID_TAG) {
        USB_SendKeyboard(data);        /* 8-byte boot-keyboard body */
        return;
    }
    if (tag == RF_PROTO_HID_TAG_CONSUMER && len >= 2u) {
        /* Composite consumer report: [report-id 1][usage LE16]. */
        uint8_t r[3] = { RF_PROTO_USB_REPORT_ID_CONSUMER, data[0], data[1] };
        USB_SendComposite(r, sizeof(r));
        return;
    }
    if (tag == RF_PROTO_HID_TAG_MOUSE && len >= 5u) {
        /* Mouse report (EP2): 5-byte [btn][X][Y][wheel][pan] forwards
         * verbatim — the on-air body matches the if1 report layout. */
        USB_SendMouse(data);
        return;
    }
    /* Other tags (system control) are not yet wired — drop. Adding one is a
     * capture of its on-air tag + a dispatch case here. */
    (void)data;
    (void)len;
}

/* Stock parity: relay the host LED to the keyboard ONCE per change (and once at
 * connect/reconnect, handled by the rf_task promote re-sync). No periodic
 * heartbeat, no fast-resync burst, no RTC-epoch timing -- matching the production
 * dongle, which relays on-change + on-connect only; the keyboard latches the LED.
 * RF_SetLEDState stores the state always and queues a single LEN-3 relay only when
 * connected. last_led=0xFF (impossible 3-bit value) forces the first iteration
 * after RF start to sync the current state (matches the CH570 main loop). */
static __attribute__((noinline)) void poll_usb_led_state(void)
{
    static uint8_t last_led = 0xFFu;
    uint8_t led = (uint8_t)(USB_GetLEDState() & 0x07u);

    if (led != last_led) {
        last_led = led;
        RF_SetLEDState(led);
    }
}

DONGLE_HIGHCODE_RF_HOT
__attribute__((noinline))
void Main_Circulation(void)
{
    while (1) {
        TMOS_SystemProcess();
        /* Run any IAP command the USB ISR deferred (bond write, reboot
         * request) outside ISR context so it can't starve RF supervision. */
        USB_PollEP6();
        /* Drive a requested reboot into OpenBoot (0x85 EnterBootloader):
         * quiesce RF, drain the final EP6 reply, then reset. */
        IAP_Service();
        /* If a keypress arrived over RF while the host was suspended (and it
         * armed remote wakeup), drive the resume K-state here in foreground. */
        USB_ServiceRemoteWake();
        /* Forward host HID LED changes from foreground, outside the highcode loop
         * body so USB LED plumbing does not consume timing-critical SRAM. */
        poll_usb_led_state();
#if DONGLE_UART_DIAG
        /* Bench telemetry on UART1 PA9: non-blocking, at most one TX-FIFO
         * fill per pass, one 127-byte frame per second. */
        UartDiag_Service();
#endif
    }
}

int main(void)
{
    /* Capture reset status before clock/BLE bring-up can perturb it. */
    uint8_t reset_status = R8_RESET_STATUS;

    dongle_fault_boot(reset_status);
    SetSysClock(CLK_SOURCE_PLL_60MHz);

    CH59x_BLEInit();
    HAL_Init();
    hal_rf_init();   /* P2.4: RF_RoleInit via the RF PHY seam */

    /* CH59x BLE library does NOT enable BLEB/BLEL IRQ lines for raw
     * RF_Rx/RF_Tx mode (only for connection-mode via BLE_LibInit's
     * internal paths). Without these explicit enables, RF_Rx returns
     * success but no callbacks ever fire. Proven 2026-05-17 in
     * ch592f-irq-test/wch-rfphy-rebuild: enabling them turned 0
     * callbacks into 2 valid RX_MODE_RX_DATA receives on BLE ADV
     * ch37 in 8 s. */
    PFIC_EnableIRQ(BLEB_IRQn);
    PFIC_EnableIRQ(BLEL_IRQn);

    /* Do NOT call PFIC_EnableAllIRQ() here. On CH59x it writes CSR 0x800 = 0x88,
     * overwriting the QingKe interrupt-system-control config that CH59x_BLEInit
     * set up for the BLE/RF library's nested LLE/BB ISRs. Overwriting it delays
     * the timing-critical EV10 supervision and the keyboard drops at ~6 s with
     * USB active. Global IRQ dispatch is already on (the RF-only build's BLEB/
     * BLEL ISRs fire without this call), and USB_DevInit enables USB_IRQn
     * itself, so USB enumerates without disturbing the BLE interrupt config. */

    /* USB HID composite device (5 interfaces; the native USB_IRQHandler in
     * usb_device.c overrides the weak vector in startup_CH592_phased.S). */
    USB_DevInit();
    /* Route EP6 OUT (interface 4) into the IAP dispatcher so the dongle stays
     * in-field reflashable via flash_dongle.py / the stock Windows tool. */
    USB_SetEP6OutCallback(IAP_PacketHandler);
    USB_SetBusResetCallback(IAP_Reset);   /* reset cancels the IAP session */

    /* 2.4G RF receiver. */
    RF_TaskInit();
    RF_SetHIDCallback(usb_hid_callback);    /* forward keyboard reports to USB */

#if DONGLE_UART_DIAG
    UartDiag_Init();
#endif

    Main_Circulation();
}
