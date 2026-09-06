/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdint.h>

/* Callback for data received on EP6 OUT (IAP interface) */
typedef void (*usb_ep6_out_cb_t)(const uint8_t *data, uint8_t len);

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

/* True while the USB bus is suspended (host stopped SOF). History: a 2026-06-13
 * bench run with a plain LowPower_Idle/__WFI in the loop starved the connected RF
 * poll while suspended and dropped the keyboard on supervision timeout; that
 * driver predates this tree and the mechanism was never isolated (a masked plain
 * WFI never wakes on this silicon, and nothing bounded the TMOS timers). The
 * CH592 idle path (ch592/src/pm_ch592.c) is a SEVONPEND WFE bounded by a 1 ms
 * heartbeat and woken by the radio and TMR0, so it idles while suspended by
 * default (DONGLE_PM_IDLE_IN_SUSPEND; bench 2026-09-06: link survived, 0 lapses,
 * remote wake armed) and consults this flag only for the USB veto set. */
uint8_t USB_IsSuspended(void);
/* Number of host suspend episodes seen since boot (diagnostics). */
uint16_t USB_SuspendEpisodes(void);

/* Call from the main loop (foreground). When a HID report arrived over RF
 * while the bus was suspended and remote wakeup is armed, this drives the
 * ~2 ms USB resume K-state to wake the host. No-op otherwise. Kept out of the
 * RF/TMR HID-delivery path because the K-state busy-waits. */
void USB_ServiceRemoteWake(void);

/* Get current keyboard LED state (from host SET_REPORT)
 * bit0=NumLock, bit1=CapsLock, bit2=ScrollLock */
uint8_t USB_GetLEDState(void);

#if DONGLE_PM_IDLE
/* CH592 main-loop idle (ch592/src/pm_ch592.c) inputs: one SRAM-resident
 * snapshot of every USB admission condition, readable under the IRQ mask. */
#define USB_PM_CONFIGURED 0x01u
#define USB_PM_SUSPENDED  0x02u
#define USB_PM_PENDING    0x04u   /* iap_pkt_pending || usb_resume_clear_kbd */
#define USB_PM_WAKE_REQ   0x08u   /* usb_wake_request && !usb_wake_inflight:
                                   * unclaimed -> spin; claimed -> nothing to do
                                   * until the resume edge */
#define USB_PM_LED_SHIFT  4u      /* bits 4-6: usb_led_state & 7 */
#define USB_PM_RW_ARMED   0x80u   /* DEVICE_REMOTE_WAKEUP armed by the host */
uint8_t USB_PmSnapshot(void);
extern volatile uint32_t usb_last_setup_tsys;   /* hal_now() at the last SETUP */
extern volatile uint16_t usb_rw_arm_count;      /* SET_FEATURE(REMOTE_WAKEUP) count */
#endif

#endif
