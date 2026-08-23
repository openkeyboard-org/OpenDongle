#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdint.h>

/* Callback for data received on EP6 OUT (IAP interface) */
typedef void (*usb_ep6_out_cb_t)(const uint8_t *data, uint8_t len);

/* Callback invoked from the USB ISR on a bus reset, after the deferred EP6
 * packet latch is dropped and before EP6 OUT is re-enabled -- the IAP layer
 * cancels its session state here (IAP_Reset). ISR context: byte writes only. */
typedef void (*usb_bus_reset_cb_t)(void);

/* Initialize USB device with all 5 HID interfaces */
void USB_DevInit(void);

/* Send boot keyboard report on EP1 (Interface 0)
 * report: 8 bytes [modifier][reserved][key0..key4][vendor] */
void USB_SendKeyboard(const uint8_t report[8]);

/* Drop any stashed remote-wake keyboard report so it can't be replayed on the
 * next resume. Call on RF link loss (see CODEREVIEW P2). No wake, no send. */
void USB_ClearPendingKeyboard(void);

/* Send boot mouse report on EP2 (Interface 1)
 * report: 5 bytes [buttons][X][Y][wheel][pan] */
void USB_SendMouse(const uint8_t report[5]);

/* Send composite report on EP3 (Interface 2)
 * First byte must be the Report ID (1=consumer, 2=sysctl, 3=NKRO) */
void USB_SendComposite(const uint8_t *report, uint8_t len);

/* Send data on EP6 IN (Interface 4, for IAP responses) */
void USB_SendEP6(const uint8_t *data, uint8_t len);

/* True once EP6 IN has no armed report: USB_SendEP6 sets T_RES=ACK and the
 * ISR's IN-token completion returns it to NAK when the host has taken the
 * bytes. Used to drain the final IAP reply before a requested reboot. A
 * detached/suspended host never takes the packet — pair with a bounded wait. */
int USB_EP6InIdle(void);

/* Register callback for EP6 OUT data */
void USB_SetEP6OutCallback(usb_ep6_out_cb_t cb);

/* Register the bus-reset cancellation hook (see usb_bus_reset_cb_t) */
void USB_SetBusResetCallback(usb_bus_reset_cb_t cb);

/* Drain a deferred IAP command (EP6 OUT) from the main loop. The USB ISR latches
 * the packet and NAKs EP6; this runs the registered callback outside ISR context
 * and re-ACKs EP6. Call once per Main_Circulation iteration when USB is built in. */
void USB_PollEP6(void);

/* True once the host has issued SET_CONFIGURATION (config != 0). The CH570 main
 * loop gates RF start-up on this; always present so the shared driver exports it. */
uint8_t USB_IsConfigured(void);

/* True while main-loop USB work is pending. Used by the idle power-management
 * path to avoid WFI while EP6 is flow-controlled for a deferred IAP command. */
uint8_t USB_HasPendingWork(void);

/* True while the USB bus is suspended (host stopped SOF). The idle path must
 * NOT enter LowPower_Idle/__WFI while suspended: with no 1 ms SOF interrupt to
 * keep waking the CPU, the connected RF poll starves and the keyboard drops on
 * supervision timeout (~3 s) -- bench-proven 2026-06-13. Staying out of WFI
 * keeps the poll dispatching so the link survives host sleep (v0.90 keep-alive
 * + remote wake). */
uint8_t USB_IsSuspended(void);

/* Call from the main loop (foreground). When a HID report arrived over RF
 * while the bus was suspended and remote wakeup is armed, this drives the
 * ~2 ms USB resume K-state to wake the host. No-op otherwise. Kept out of the
 * RF/TMR HID-delivery path because the K-state busy-waits. */
void USB_ServiceRemoteWake(void);

/* Get current keyboard LED state (from host SET_REPORT)
 * bit0=NumLock, bit1=CapsLock, bit2=ScrollLock */
uint8_t USB_GetLEDState(void);

#endif
