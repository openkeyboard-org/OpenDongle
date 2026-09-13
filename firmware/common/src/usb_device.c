/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * USB HID Composite Device Driver (shared CH592 / CH582 / CH570)
 *
 * 5-interface HID device: boot keyboard, boot mouse, composite
 * (consumer/NKRO/sysctl), vendor 32B IN-only, vendor 64B (IAP).
 *
 * Based on CH592 SDK CompoundDev example and the 8K mouse dongle project.
 * CH592 is the canonical, bench-validated base. The only chip-specific fork is
 * the analog USB pin/PHY seam behind dongle_usb_hal.h (hal_usb_pins_*), plus the
 * target-specific USB pin and PHY behavior.
 */

#include "dongle_chip.h"        /* per-tree SDK common header (CH59x / CH57x) */
#include "dongle_platform.h"    /* DONGLE_USB_REMOTE_WAKEUP */
#include "usb_descriptors.h"
#include "usb_device.h"
#include "dongle_usb_hal.h"     /* hal_usb_pins_predetach/enable/reattach */
#if DONGLE_PM_IDLE
#include "hal_timing.h"         /* hal_now: the EP0-quiet stamp for pm_ch592.c */
#endif

/* Production HID endpoints use the validated automatic-toggle mode. */
#define USB_HID_IN_TOG_MODE RB_UEP_AUTO_TOG

#ifndef USB_HID_SEND_HIGHCODE
#define USB_HID_SEND_HIGHCODE DONGLE_HIGHCODE_COLD
#endif

/* ---------- Endpoint DMA buffers ---------- */

__attribute__((aligned(4))) static uint8_t EP0_Buf[64]; /* EP0 only; EP4 unused
    (no descriptor, no ISR case) so no RX/TX halves are carved out for it */
__attribute__((aligned(4))) static uint8_t EP1_Buf[64];
__attribute__((aligned(4))) static uint8_t EP2_Buf[64];
__attribute__((aligned(4))) static uint8_t EP3_Buf[64];
__attribute__((aligned(4))) static uint8_t EP5_Buf[64];
__attribute__((aligned(4))) static uint8_t EP6_Buf[64 + 64];

/* EP buffer layout:
 * - EP1/EP2/EP3 and EP5 are descriptor-IN-only HID endpoints, configured
 *   TX-only, so WCH transmits from DMA offset 0.
 * - EP6 remains dual-buffered: [OUT 64B][IN 64B] for IAP command/response.
 * - Stock IF3/EP5 OUT was a proprietary vendor RF sideband (USB IRQ marker
 *   0x91 feeding a 5-slot RF TX queue); OpenDongle intentionally removes that
 *   OUT surface to save 64 B. Restore by re-adding IF3 OUT in descriptors,
 *   EP5 RX mode, and a 128 B EP5_Buf.
 */
#define EP_OUT(buf)  (buf)
#define EP1_IN()     (EP1_Buf)
#define EP2_IN()     (EP2_Buf)
#define EP3_IN()     (EP3_Buf)
#define EP_DUAL_IN(buf) ((buf) + 64)

/* EP0 max packet size. The CH59x EP0 hardware is designed for 64 and every
 * working CH592 SDK USB example uses 64 (DevEP0SIZE); an 8-byte EP0 multiplies
 * every control transfer into many more toggled packets. Must match
 * usb_device_desc[7] (bMaxPacketSize0 = 0x40). */
#define EP0_SIZE 64

/* ---------- State ---------- */

static uint8_t usb_dev_addr;
static volatile uint8_t usb_config;   /* set in USB ISR (SET_CONFIG/BusReset), read in foreground */
static uint8_t usb_setup_req;
/* Armed for exactly one SETUP: the boot-keyboard LED SET_REPORT (IF0, Output
 * type, report id 0, 1 byte). Reset on EVERY SETUP so a stale value can't route
 * an unrelated later EP0 OUT (or a same-bRequest SET_CONFIGURATION) into the LED
 * relay. Written + read only in USB ISR context. See CODEREVIEW F14. */
static uint8_t usb_ep0_out_is_led;
static uint16_t usb_setup_len;
static const uint8_t *usb_desc_ptr;
/* Written in USB_IRQHandler (SET_REPORT), read from the foreground via
 * USB_GetLEDState() to forward to the keyboard -- volatile for the cross-context
 * read. */
static volatile uint8_t usb_led_state;
static usb_ep6_out_cb_t ep6_out_cb;

/* USB suspend state. Set when the host stops SOF (RB_UIF_SUSPEND + bus idle),
 * cleared on resume and on bus reset. The dongle is bus-powered and keeps the
 * RF poll dispatching while suspended (on CH592 the core idles between polls
 * under pm_ch592.c's heartbeat-bounded WFE; see USB_IsSuspended for the history
 * of the older no-WFI rule), so the link survives host sleep. We also
 * suppress HID IN reports while suspended so a key that arrives over RF during
 * host sleep can't be delivered as a stale report the instant the host resumes;
 * the current key state is retained in the EP1 queue and delivered by the
 * resume path so the waking keystroke is not lost. */
static volatile uint8_t usb_suspended;
static volatile uint16_t usb_suspend_episodes;   /* IAP 0x92 page 4 */

/* ---- Report-delivery observability (IAP 0x92 page 8; read over USB, never SWD,
 * which would reset the dongle and drop the link). These locate a lost keystroke
 * at the dongle->USB hop: ep1_arms counts every boot-keyboard report armed onto
 * EP1; ep1_completions counts host IN completions (the delivery signal);
 * ep1_overwrites counts a report armed while the previous one had NOT completed
 * -- the proven single-slot overwrite. arms - completions - overwrites that stay
 * queued is the loss. All u32; wrap is irrelevant over a bench run. */
static volatile uint32_t usb_ep1_arms;
static volatile uint32_t usb_ep1_completions;
static volatile uint32_t usb_ep1_overwrites;
static volatile uint8_t  usb_ep1_armed;   /* set on arm, cleared on IN completion / reset */
#if DONGLE_DELIVERY_COUNTERS
static volatile uint16_t usb_ep1_arms_down;        /* EP1 armed with a non-zero (key-down) report */
static volatile uint16_t usb_ep1_completions_down; /* IN completed while the armed report was a key-down */
static volatile uint8_t  usb_ep1_down_inflight;    /* last-armed report is non-zero; awaiting its IN completion */
#endif

/* USB remote-wakeup feature. Armed/disarmed by the host via
 * SET/CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP) and reported in GET_STATUS(device).
 * USB spec: defaults OFF after every bus reset; the host sets it before
 * suspending if it wants the device to be able to wake it. Only when this is
 * set (and the bus is suspended) may a keypress over RF drive USB resume
 * signalling to wake the host (v0.90 remote wake). */
static volatile uint8_t usb_remote_wakeup;

/* Set when a HID report arrives over RF while suspended AND remote wakeup is
 * armed: the report itself is dropped (the host isn't listening), but the main
 * loop (USB_ServiceRemoteWake, foreground) then drives the ~2 ms resume
 * K-state. Cleared once serviced and on the resume/bus-reset edge. A flag (not
 * an inline drive) because USB_Send* run from the RF/TMR delivery context,
 * where a 2 ms busy K-state would stall the radio. */
static volatile uint8_t usb_wake_request;

/* One-shot "resume K-state in flight" latch: set when USB_ServiceRemoteWake
 * drives the K-state, cleared only on the resume/bus-reset edge. Prevents a
 * second RF report (while still suspended) from re-driving the K-state, and
 * keeps the drive to a single pulse per suspend episode. */
static volatile uint8_t usb_wake_inflight;

/* Set on a real suspend->resume transition so the main loop delivers one
 * boot-keyboard report right after resume: either the report stashed below (the
 * current key state at wake time) or, if none, an all-keys-up flush that closes
 * the stuck-key edge case. */
static volatile uint8_t usb_resume_clear_kbd;
/* Set with it on the same edge, one per change-driven endpoint without a
 * stash: EP2 (mouse) and EP3 (consumer), and EP1 (keyboard) for its stash /
 * keys-up delivery. USB_PollEP6 sends the reconciliation report for each flag
 * still set; a FRESH report on that endpoint (one that is actually armed)
 * clears its flag under the sender's IRQ mask first, because a fresh report
 * carries the device's whole current state and is the better reconciliation.
 * The check and the arm happen under one mask in usb_reconcile_keyboard /
 * usb_arm_mouse / usb_arm_composite, so a fresh press can neither be
 * overwritten by the flush nor released by it (codex and CodeRabbit reviews of
 * the suspend fix). For EP1 this also means a report that arrived after resume
 * outranks the stash captured during suspend: newest wins. */
static volatile uint8_t usb_reconcile_ep1;
static volatile uint8_t usb_reconcile_ep2;
static volatile uint8_t usb_reconcile_ep3;

/* One-slot stash of the newest boot-keyboard report that arrived over RF while
 * the host was suspended and remote-wakeup was armed. HID reports are
 * change-driven (not repeated per poll), so a key pressed to wake the host is
 * never re-sent after resume — without this the waking keystroke is lost. The
 * resume path delivers this instead of the all-keys-up flush; newest-wins means
 * a key released before the host actually resumes is stored as all-up, so no
 * key sticks. Keyboard (EP1) only; mouse/consumer stay drop-on-suspend: like
 * EP1, a report armed when SOF stops is NAKed by USB_SuspendResume so it
 * cannot replay, and on resume USB_PollEP6 sends all-up / no-key reports on
 * EP2/EP3 so nothing the host last saw stays pressed. */
/* ---- EP1 boot-keyboard queue -------------------------------------------
 * EP1 used to hold exactly ONE report: any new report overwrote the previous one
 * whenever the host had not yet collected it, destroying a real transition.
 * Measured on the bench once the module stopped losing the key upstream: with a
 * tap's down and up drained on consecutive RF polls (~0.87 ms apart) the up
 * replaced the down inside the host's 1 ms poll interval, losing 4 of 21
 * key-downs (arms_down 21, completions_down 17, overwrites 4).
 * Reports are now held in queue-owned storage with exactly ONE transfer in
 * flight; the head is retired only by its own IN completion. This also replaces
 * the single-slot suspend stash -- suspend-time arrivals simply enter the queue,
 * so a whole tap survives a suspend instead of the up clobbering the down.
 * Capacity is per-chip: CH570 links with only ~32 B above its stack floor. */
#ifndef DONGLE_EP1_QUEUE_SLOTS
#define DONGLE_EP1_QUEUE_SLOTS 4u
#endif
#if DONGLE_EP1_QUEUE_SLOTS < 2u
#error "EP1 queue needs >= 2 slots: the overflow path coalesces onto the newest entry, which must never be the in-flight head"
#endif
static uint8_t          usb_kbd_q[DONGLE_EP1_QUEUE_SLOTS][8];
static volatile uint8_t usb_kbd_q_head;    /* retired by the IN completion */
static volatile uint8_t usb_kbd_q_tail;    /* advanced by the enqueue */
static volatile uint8_t usb_kbd_q_owned;   /* a transfer is in flight and owns the head */
static void usb_ep1_service(void);   /* the one place EP1 is armed (defined below) */

/* Cancel the in-flight EP1 transfer WITHOUT retiring its entry. The report was
 * never collected, so it stays at the head and usb_ep1_service() re-arms it once
 * the endpoint is usable again. Required EVERYWHERE we force EP1 to NAK: no
 * completion can arrive for a NAKed endpoint, so leaving ownership set wedges the
 * queue permanently (codex). If the hardware does complete a transfer we already
 * cancelled, the completion handler will NOT retire it -- retirement is gated on
 * the ownership flag this clears -- so the entry is simply re-armed and delivered
 * again. That is safe: these are absolute key-state reports, so a repeat conveys
 * the same state and produces no extra key transition. */
static void usb_ep1_cancel_owned(void)
{
    usb_kbd_q_owned = 0;
    usb_ep1_armed   = 0;
}

/* IAP EP6-OUT packets are deferred out of the USB ISR. The ISR latches the
 * packet length, NAKs EP6 for flow control, and USB_PollEP6() (called from the
 * main loop) runs the IAP command and re-ACKs EP6. Flash erase/program block
 * for ms (and the SDK's FLASH_EEPROM_CMD masks every IRQ for that whole span);
 * running them in the ISR would starve TMOS_SystemProcess() and the RF EV10
 * supervision. One slot is enough — IAP is strict request/response, so the host
 * waits for our EP6-IN reply before sending the next OUT. EP6 OUT stays NAKed
 * until after the callback, so the command can be read in place from EP6_Buf's
 * OUT half instead of copying to a second 64-byte buffer. */
static volatile uint8_t iap_pkt_pending;
static volatile uint8_t iap_pkt_len;   /* set in USB ISR with iap_pkt_pending, read in foreground */

/* Per-interface idle and protocol values */
static uint8_t usb_idle[USB_NUM_INTERFACES];
static uint8_t usb_protocol[USB_NUM_INTERFACES];

static uint8_t usb_effective_suspended(void)
{
    return (uint8_t)((usb_suspended || (R8_USB_MIS_ST & RB_UMS_SUSPEND)) ? 1u : 0u);
}

#if DONGLE_PM_IDLE
/* CH592 main-loop idle (ch592/src/pm_ch592.c) inputs. usb_last_setup_tsys is
 * hal_now() at the last SETUP - the start of the EP0-quiet window during which
 * the idle never takes the IRQ mask (CSR-0x800 churn is an enumeration hazard
 * on this silicon). usb_rw_arm_count counts SET_FEATURE(DEVICE_REMOTE_WAKEUP)
 * for the page-6 suspend/resume oracle. */
volatile uint32_t usb_last_setup_tsys;
volatile uint16_t usb_rw_arm_count;

/* One SRAM-resident snapshot of every idle-admission input, so nothing the
 * idle path evaluates under the IRQ mask fetches flash (USB_IsConfigured,
 * USB_IsSuspended and USB_GetLEDState are XIP). Taken unmasked before the
 * quiet passes and again under the mask, where no USB ISR can move it. */
DONGLE_HIGHCODE_RF_HOT
uint8_t USB_PmSnapshot(void)
{
    uint8_t s = 0u;

    if (usb_config != 0u)                                   s |= USB_PM_CONFIGURED;
    if (usb_suspended || (R8_USB_MIS_ST & RB_UMS_SUSPEND))  s |= USB_PM_SUSPENDED;
    if (iap_pkt_pending || usb_resume_clear_kbd)            s |= USB_PM_PENDING;
    if (usb_wake_request && !usb_wake_inflight)             s |= USB_PM_WAKE_REQ;
    if (usb_remote_wakeup)                                  s |= USB_PM_RW_ARMED;
    s |= (uint8_t)((usb_led_state & 7u) << USB_PM_LED_SHIFT);
    return s;
}
#endif /* DONGLE_PM_IDLE */

/* ---------- HID report descriptor lookup ---------- */

static const uint8_t *hid_report_descs[USB_NUM_INTERFACES] = {
    usb_hid_report_desc_if0,
    usb_hid_report_desc_if1,
    usb_hid_report_desc_if2,
    usb_hid_report_desc_if3,
    usb_hid_report_desc_if4,
};

static const uint16_t hid_report_sizes[USB_NUM_INTERFACES] = {
    HID_REPORT_DESC_SIZE_IF0,
    HID_REPORT_DESC_SIZE_IF1,
    HID_REPORT_DESC_SIZE_IF2,
    HID_REPORT_DESC_SIZE_IF3,
    HID_REPORT_DESC_SIZE_IF4,
};

/* HID descriptor offsets within the config descriptor (byte offset of each 0x21 descriptor) */
static const uint8_t hid_desc_offsets[USB_NUM_INTERFACES] = {
    18,   /* IF0 */
    43,   /* IF1 */
    68,   /* IF2 */
    93,   /* IF3 */
    118,  /* IF4 */
};

/* ---------- USB Setup Request Processing ---------- */

/* Runs from flash: EP0 control transfers only happen during enumeration /
 * occasional GET/SET, never concurrently with a flash erase or program (IAP
 * streams on EP6; the one-shot bond write happens at pair time). Keep noinline
 * so the parser cannot be folded back into the SRAM USB IRQ shell. */
DONGLE_HIGHCODE_COLD
static __attribute__((noinline)) void USB_EP0_Setup(void)
{
    uint8_t  bmReqType = EP0_Buf[0];
    uint8_t  bRequest  = EP0_Buf[1];
    uint16_t wValue    = EP0_Buf[2] | (EP0_Buf[3] << 8);
    uint16_t wIndex    = EP0_Buf[4] | (EP0_Buf[5] << 8);
    uint16_t wLength   = EP0_Buf[6] | (EP0_Buf[7] << 8);

    uint8_t desc_type  = wValue >> 8;
    uint8_t desc_index = wValue & 0xFF;

    usb_setup_len = wLength;
    usb_desc_ptr = NULL;
    usb_setup_req = bRequest;
    usb_ep0_out_is_led = 0;   /* F14: only the exact keyboard LED SET_REPORT below re-arms it */
    uint16_t len = 0;
    uint8_t  err = 0;

    if ((bmReqType & 0x60) == 0x00) {
        /* Standard request */
        switch (bRequest) {
        case 0x06: /* GET_DESCRIPTOR */
            switch (desc_type) {
            case 0x01: /* Device */
                usb_desc_ptr = usb_device_desc;
                len = sizeof(usb_device_desc);
                break;
            case 0x02: /* Configuration */
                usb_desc_ptr = usb_config_desc;
                len = sizeof(usb_config_desc);
                break;
            case 0x03: /* String */
                switch (desc_index) {
                case 0:
                    usb_desc_ptr = usb_lang_desc;
                    len = sizeof(usb_lang_desc);
                    break;
                case 2:
                    usb_desc_ptr = usb_product_desc;
                    len = sizeof(usb_product_desc);
                    break;
                default:
                    err = 0xFF;
                    break;
                }
                break;
            case 0x22: /* HID Report Descriptor */
                if (wIndex < USB_NUM_INTERFACES) {
                    usb_desc_ptr = hid_report_descs[wIndex];
                    len = hid_report_sizes[wIndex];
                } else {
                    err = 0xFF;
                }
                break;
            case 0x21: /* HID Descriptor */
                if (wIndex < USB_NUM_INTERFACES) {
                    usb_desc_ptr = &usb_config_desc[hid_desc_offsets[wIndex]];
                    len = 9;
                } else {
                    err = 0xFF;
                }
                break;
            default:
                err = 0xFF;
                break;
            }
            if (len > usb_setup_len)
                len = usb_setup_len;
            usb_setup_len = len;
            break;

        case 0x05: /* SET_ADDRESS */
            usb_dev_addr = wValue & 0x7F;
            break;

        case 0x09: /* SET_CONFIGURATION */
            /* Only config 0 (unconfigured) and our single advertised config 1
             * exist (usb_config_desc bNumConfigurations = 1). STALL anything
             * else instead of silently accepting it. */
            if ((wValue & 0xFF) <= 1) {
                usb_config = wValue & 0xFF;
            } else {
                err = 0xFF;
            }
            break;

        case 0x08: /* GET_CONFIGURATION */
            EP0_Buf[0] = usb_config;
            usb_setup_len = 1;
            break;

        case 0x0A: /* GET_INTERFACE (bRequest 10 — CODEREVIEW N19: this and
                    * SET_INTERFACE were swapped, so a real GET_INTERFACE fell
                    * through with usb_setup_len still = wLength and shipped
                    * the stale SETUP bmRequestType byte from EP0_Buf[0]).
                    * Exact spec tuple; full 16-bit wIndex (0x0100 must not
                    * alias interface 0). Only alt setting 0 exists. */
            if (bmReqType == 0x81u && wValue == 0u
                && wIndex < USB_NUM_INTERFACES && wLength == 1u) {
                EP0_Buf[0] = 0;
                usb_setup_len = 1;
            } else {
                err = 0xFF;
            }
            break;

        case 0x0B: /* SET_INTERFACE (bRequest 11): no alternate settings —
                    * accept only the exact alt-0 tuple, STALL the rest. */
            if (bmReqType == 0x01u && wValue == 0u
                && wIndex < USB_NUM_INTERFACES && wLength == 0u) {
                /* nothing to do: alt 0 is the only setting */
            } else {
                err = 0xFF;
            }
            break;

        case 0x00: /* GET_STATUS */
            /* 2-byte status. Device: bit0=self-powered (0, bus-powered),
             * bit1=remote-wakeup (reflects the armed feature, only on chips
             * that advertise it). Interface: reserved 0. Endpoint: bit0=halt
             * (0, we never halt an endpoint). Validate it is a well-formed
             * IN GET_STATUS (wValue=0, wLength=2) to a device/iface/endpoint
             * recipient; STALL anything else. */
            if ((bmReqType & 0x80) && wValue == 0x0000 && wLength == 2u
                && (bmReqType & 0x1F) <= 0x02) {
                EP0_Buf[0] = (uint8_t)(((bmReqType & 0x1F) == 0x00
                                        && usb_remote_wakeup) ? 0x02 : 0x00);
                EP0_Buf[1] = 0x00;
                usb_setup_len = 2;
            } else {
                err = 0xFF;
            }
            break;

        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
            /* Honored features: DEVICE_REMOTE_WAKEUP (only on chips that
             * advertise it) and CLEAR_FEATURE(ENDPOINT_HALT). Both are OUT,
             * wLength=0; STALL malformed/unsupported (incl. SET_FEATURE
             * (ENDPOINT_HALT), TEST_MODE) rather than silently ACK a no-op. */
            if (bmReqType & 0x80) {
                err = 0xFF;                 /* must be host->device (OUT)     */
            } else if (wLength != 0u) {
                err = 0xFF;                 /* FEATURE carries no data stage   */
#if DONGLE_USB_REMOTE_WAKEUP
            } else if ((bmReqType & 0x1F) == 0x00 && wValue == 0x0001
                       && wIndex == 0x0000) {
                /* SET/CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP) — the host arms or
                 * disarms our ability to wake it. bRequest 0x03=SET, 0x01=CLEAR. */
                usb_remote_wakeup = (uint8_t)(bRequest == 0x03);
#if DONGLE_PM_IDLE
                if (bRequest == 0x03) {
                    usb_rw_arm_count++;   /* page 6: the host armed remote wake */
                }
#endif
#endif
            } else if ((bmReqType & 0x1F) == 0x02 && wValue == 0x0000 && bRequest == 0x01) {
                /* CLEAR_FEATURE(ENDPOINT_HALT) */
                uint8_t ep = wIndex & 0x0F;
                switch (ep) {
                case 1:
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                    usb_ep1_cancel_owned();   /* NAK + DATA0 restart: nothing will complete */
                    /* Re-arm immediately: unlike suspend, the endpoint is usable
                     * again right now, and without this a queued report waits for
                     * the next arrival that may never come. */
                    usb_ep1_service();
                    break;
                case 2: R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                case 3: R8_UEP3_CTRL = (R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                case 5: R8_UEP5_CTRL = (R8_UEP5_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                case 6: R8_UEP6_CTRL = (R8_UEP6_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK; break;
                default: err = 0xFF; break;
                }
            } else {
                err = 0xFF;
            }
            break;

        default:
            err = 0xFF;
            break;
        }
    } else if ((bmReqType & 0x60) == 0x20) {
        /* HID Class request */
        uint8_t iface = wIndex & 0xFF;
        switch (bRequest) {
        case 0x01: /* GET_REPORT */
            /* Return zeros for now. STALL a request for a non-existent iface. */
            if (iface < USB_NUM_INTERFACES) {
                for (uint16_t i = 0; i < wLength && i < 64; i++)
                    EP0_Buf[i] = 0;
                usb_setup_len = wLength < 64 ? wLength : 64;
            } else {
                err = 0xFF;
            }
            break;

        case 0x02: /* GET_IDLE */
            if (iface < USB_NUM_INTERFACES) {
                EP0_Buf[0] = usb_idle[iface];
                usb_setup_len = 1;
            } else {
                err = 0xFF;
            }
            break;

        case 0x03: /* GET_PROTOCOL */
            if (iface < USB_NUM_INTERFACES) {
                EP0_Buf[0] = usb_protocol[iface];
                usb_setup_len = 1;
            } else {
                err = 0xFF;
            }
            break;

        case 0x09: /* SET_REPORT */
            /* Data arrives in the EP0 OUT phase. STALL a bad iface. Only the
             * boot keyboard's LED Output report (IF0, report type Output, report
             * id 0, 1 byte) may drive usb_led_state -- arm the OUT sink only for
             * that exact tuple so a SET_REPORT to any other interface (e.g. a
             * control-transfer fallback write toward the vendor IF4, whose
             * normal hidraw write path is its interrupt-OUT EP6) can't corrupt
             * the RF LED relay (CODEREVIEW F14). Other well-formed interfaces
             * are still ACK'd-and-ignored. */
            if (iface >= USB_NUM_INTERFACES)
                err = 0xFF;
            else if (bmReqType != 0x21u          /* OUT|class|interface only  */
                     || wLength > EP0_SIZE       /* can't buffer more anyway  */
                     || ((wValue >> 8) != 2u && (wValue >> 8) != 3u))
                /* GA2 (narrowed): type/length hygiene only — DEVICE POLICY:
                 * we STALL SET_REPORT(Input) (HID 1.11 permits a device to
                 * treat it as meaningless; we reject rather than fake-ACK)
                 * and anything whose data stage exceeds EP0. Full descriptor-
                 * aware per-interface tuple validation stays open under N22. */
                err = 0xFF;
            else if (bmReqType == 0x21u && wIndex == 0x0000u &&
                     wValue == 0x0200u && wLength == 1u)
                usb_ep0_out_is_led = 1u;
            break;

        case 0x0A: /* SET_IDLE — GA1: the idle DURATION is the wValue HIGH
                    * byte; the low byte is the Report ID (per-report idle
                    * stays unimplemented — fine for single-report ifaces).
                    * The old code stored the Report ID, so GET_IDLE echoed
                    * the wrong byte back. */
            if (iface < USB_NUM_INTERFACES)
                usb_idle[iface] = desc_type;
            else
                err = 0xFF;
            break;

        case 0x0B: /* SET_PROTOCOL */
            if (iface < USB_NUM_INTERFACES)
                usb_protocol[iface] = wValue & 0xFF;
            else
                err = 0xFF;
            break;

        default:
            err = 0xFF;
            break;
        }
    } else {
        err = 0xFF;
    }

    if (err) {
        R8_UEP0_T_LEN = 0;
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
    } else {
        /* Stage the control transfer the way the canonical CH59x SDK does
         * (CompoundDev USB_DevTransProcess): for a device-to-host (IN) request,
         * send the first <=EP0_SIZE chunk of the prepared response; for a
         * host-to-device (OUT) request, arm a 0-length IN — the data stage (if
         * any) arrives via the OUT token and its status is this 0-length IN.
         * Both directions reset the toggles to DATA1 and ACK. */
        uint16_t tx_len = 0;
        if (bmReqType & 0x80) {
            tx_len = usb_setup_len > EP0_SIZE ? EP0_SIZE : usb_setup_len;
            /* Direct single-packet responses (GET_STATUS, GET_CONFIGURATION,
             * GET_REPORT, …) write straight into EP0_Buf and leave usb_desc_ptr
             * NULL; multi-packet descriptors set usb_desc_ptr to the source. */
            if (usb_desc_ptr) {
                for (uint16_t i = 0; i < tx_len; i++)
                    EP0_Buf[i] = usb_desc_ptr[i];
                usb_desc_ptr += tx_len;
            }
            usb_setup_len -= tx_len;
        }
        R8_UEP0_T_LEN = tx_len;
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
    }
}

DONGLE_HIGHCODE_COLD
static __attribute__((noinline)) void USB_EP0_IN(void)
{
    if (usb_setup_req == 0x05) {
        /* SET_ADDRESS: apply after status stage */
        R8_USB_DEV_AD = usb_dev_addr;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    } else if (usb_setup_len > 0 && usb_desc_ptr) {
        /* More data: send next <=EP0_SIZE chunk, advance, toggle DATA0/1. */
        uint16_t tx_len = usb_setup_len > EP0_SIZE ? EP0_SIZE : usb_setup_len;
        for (uint16_t i = 0; i < tx_len; i++)
            EP0_Buf[i] = usb_desc_ptr[i];
        usb_desc_ptr += tx_len;
        usb_setup_len -= tx_len;
        R8_UEP0_T_LEN = tx_len;
        R8_UEP0_CTRL ^= RB_UEP_T_TOG;
    } else {
        /* Data stage complete: enter the status stage -- NAK further IN and
         * accept the host's 0-length OUT status, re-arming EP0 for the next
         * SETUP. */
        R8_UEP0_T_LEN = 0;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    }
}

DONGLE_HIGHCODE_COLD
static __attribute__((noinline)) void USB_BusReset(void)
{
    R8_USB_DEV_AD = 0;
    usb_config = 0;
    usb_ep1_armed = 0;
    /* The host has dropped all key state and will re-enumerate, so a queued
     * backlog describes a session that no longer exists; ownership is released
     * because no in-flight transfer survives a reset. Nothing can be stuck: the
     * host starts with no keys held. */
    usb_kbd_q_owned = 0;
    usb_kbd_q_head  = 0;
    usb_kbd_q_tail  = 0;
#if DONGLE_DELIVERY_COUNTERS
    usb_ep1_down_inflight = 0;
#endif
    usb_dev_addr = 0;

    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP2_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP3_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP5_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP6_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;

    usb_suspended = 0;     /* a bus reset always leaves the device active */
    usb_remote_wakeup = 0; /* USB spec: feature defaults off after reset */
    usb_wake_request = 0;
    usb_wake_inflight = 0;
    usb_reconcile_ep1 = 0;
    usb_reconcile_ep2 = 0;
    usb_reconcile_ep3 = 0;
}

DONGLE_HIGHCODE_COLD
static __attribute__((noinline)) void USB_SuspendResume(void)
{
    /* Track suspend/resume only. Bus-powered, no sleep: leave USB and RF fully
     * powered (RF_WITH_USB keeps the radio running so a keyboard wake
     * reconnects), keep the DP pull-up and endpoints as-is. The flag just gates
     * HID IN reports (see USB_Send*). */
    if (R8_USB_MIS_ST & RB_UMS_SUSPEND) {
        usb_suspended = 1;   /* host stopped SOF -- bus is idle */
        usb_suspend_episodes++;   /* diag: correlate RF loss with host sleep */
        /* NAK any HID report that was armed (T_RES=ACK) but not polled before
         * SOF stopped, so the host's first IN poll on resume can't return it as
         * a stale event: for EP1 ahead of the USB_PollEP6 keys-up flush
         * (CODEREVIEW F12); for EP2 (mouse) and EP3 (consumer) because their
         * reports are change-driven, so a press armed at suspend whose
         * release arrived (and was dropped) during suspend would otherwise
         * resume as a stuck button or held media key. The mirror case - a
         * RELEASE armed here and NAKed - is what the all-up flush in
         * USB_PollEP6 covers on resume.
         * Mask only T_RES so the data toggle is preserved: the host never
         * received the NAK'd report, so its expected toggle is unchanged and
         * the next arm continues the sequence. EP5 is never armed; EP6 (IAP)
         * is outside the HID suspend policy on purpose. */
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
        usb_ep1_cancel_owned();   /* the NAKed transfer can never complete */
        R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
        R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
    } else {
        /* Resume signalling -- bus active again. On a real suspend->resume
         * edge, ask the main loop to flush an all-keys-up report (see
         * usb_resume_clear_kbd). */
        if (usb_suspended) {
            usb_resume_clear_kbd = 1;
            usb_reconcile_ep1 = 1;
            usb_reconcile_ep2 = 1;
            usb_reconcile_ep3 = 1;
        }
        usb_suspended = 0;
        usb_wake_request = 0;   /* host is up -- the wake request is done */
        usb_wake_inflight = 0;  /* end of the wake episode; allow re-arm */
    }
}

/* ---------- USB Interrupt Handler ---------- */

__INTERRUPT
USB_ISR_HIGHCODE
void USB_IRQHandler(void)
{
    uint8_t intflag = R8_USB_INT_FG;
    if (intflag & RB_UIF_TRANSFER) {
        uint8_t raw_st = R8_USB_INT_ST;
        /* SETUP is flagged by RB_UIS_SETUP_ACT and always targets EP0. On CH592
         * the endpoint bits in R8_USB_INT_ST do NOT read as 0 for a SETUP (they
         * carry stale endp state — observed as raw 0xB2 = SETUP_ACT|SETUP|ep2),
         * so matching on the token field (UIS_TOKEN_SETUP|0 == 0x30) misses it
         * and enumeration never starts. Gate on SETUP_ACT instead — the
         * canonical WCH CH59x pattern. */
        if (raw_st & RB_UIS_SETUP_ACT) {
#if DONGLE_PM_IDLE
            usb_last_setup_tsys = hal_now();   /* EP0-quiet window starts here */
#endif
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            USB_EP0_Setup();
        } else
        switch (raw_st & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
        /* ---- EP0 IN ---- */
        case UIS_TOKEN_IN | 0:
            USB_EP0_IN();
            break;

        /* ---- EP0 OUT ---- */
        case UIS_TOKEN_OUT | 0:
            if (usb_ep0_out_is_led) {
                /* SET_REPORT(Output) to IF0 only: boot-keyboard LED state from
                 * the host (armed at SETUP for the exact tuple; F14). */
                usb_led_state = EP0_Buf[0];
            }
            /* Do NOT rewrite R8_UEP0_CTRL here. The SETUP stage armed EP0 with
             * T_LEN=0 / T_RES=ACK and the toggles set, so this OUT is either the
             * data stage of a SET_* (whose 0-length IN status is already armed)
             * or the status OUT of a control-read (transfer done; next is a fresh
             * SETUP). Writing UEP_T_RES_NAK here would NAK the SET_* status IN and
             * clear the data toggles — the canonical CH59x handler leaves it be. */
            break;

        /* ---- EP1 IN (boot keyboard) ---- */
        case UIS_TOKEN_IN | 1:
            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            R8_UEP1_CTRL ^= RB_UEP_T_TOG;
            usb_ep1_armed = 0;             /* host consumed the report */
            usb_ep1_completions++;
#if DONGLE_DELIVERY_COUNTERS
            if (usb_ep1_down_inflight) { usb_ep1_completions_down++; usb_ep1_down_inflight = 0; }
#endif
            /* This completion belongs to the entry that was in flight: retire
             * exactly that one, then arm the next queued state. */
            if (usb_kbd_q_owned) {
                usb_kbd_q_owned = 0;
                if (usb_kbd_q_head != usb_kbd_q_tail) {
                    usb_kbd_q_head++;
                }
            }
            usb_ep1_service();
            break;

        /* ---- EP2 IN (boot mouse) ---- */
        case UIS_TOKEN_IN | 2:
            R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            R8_UEP2_CTRL ^= RB_UEP_T_TOG;
            break;

        /* ---- EP3 IN (composite HID) ---- */
        case UIS_TOKEN_IN | 3:
            R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            R8_UEP3_CTRL ^= RB_UEP_T_TOG;
            break;

        /* ---- EP5 IN (vendor) ---- */
        case UIS_TOKEN_IN | 5:
            R8_UEP5_CTRL = (R8_UEP5_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            R8_UEP5_CTRL ^= RB_UEP_T_TOG;
            break;

        /* ---- EP6 IN (IAP response) ---- */
        case UIS_TOKEN_IN | 6:
            R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            R8_UEP6_CTRL ^= RB_UEP_T_TOG;
            break;

        /* ---- EP6 OUT (IAP command) ---- */
        case UIS_TOKEN_OUT | 6:
            /* Defer the IAP command out of ISR context: latch the packet, flag
             * it, and leave EP6 OUT NAK'd so the host can't send the next OUT
             * until USB_PollEP6() (main loop) has run the command and re-ACK'd.
             * IAP flash erase/program block for ms with every IRQ masked (the
             * SDK's FLASH_EEPROM_CMD disables PFIC IRER for the whole op);
             * running them here would starve the ~1 Hz RF EV10 supervision and
             * drop the keyboard link. One slot suffices — IAP is strict
             * request/response, so the host waits for our EP6-IN reply. */
            if ((raw_st & RB_UIS_TOG_OK) && !iap_pkt_pending) {
                uint8_t rx_len = R8_USB_RX_LEN;
                if (rx_len > 64u) rx_len = 64u;
                iap_pkt_len = rx_len;
                iap_pkt_pending = 1;
                /* Latched: NAK until USB_PollEP6 has run it and re-ACKs. */
                R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
            } else if (iap_pkt_pending) {
                /* Defensive: an OUT completed while a command is pending. The
                 * SIE's auto-busy (RB_UC_INT_BUSY) NAKs until UIF_TRANSFER is
                 * cleared below, and the explicit NAK then holds until the
                 * poll re-ACKs, so ordinary traffic cannot get here; only the
                 * bus-reset path, which re-ACKs EP6 OUT without cancelling a
                 * pending command, can admit one, and its DMA has then already
                 * overwritten EP6_Buf under the old iap_pkt_len (a pre-existing
                 * exposure, codex). Keep the NAK. */
                R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
            } else {
                /* Toggle mismatch with nothing pending: the host retried a
                 * packet whose ACK it lost after we had already taken and run
                 * it (USB 2.0 8.6.4), or a bus error. The duplicate is
                 * dropped without advancing the toggle and the endpoint must
                 * stay live. Before this branch the unconditional NAK left EP6
                 * OUT NAKed with nothing to re-ACK it, so the vendor interface
                 * was wedged until a bus reset or a power cycle (TODO defect).
                 * The toggle is judged from the same status sample as the
                 * token, not a re-read. */
                R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
            }
            break;

        default:
            break;
        }

        R8_USB_INT_FG = RB_UIF_TRANSFER;
    }

    if (intflag & RB_UIF_BUS_RST) {
        /* Bus reset */
        USB_BusReset();
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }

    if (intflag & RB_UIF_SUSPEND) {
        USB_SuspendResume();
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
}

/* ---------- Initialization ---------- */

void USB_DevInit(void)
{
    /* Mask USB interrupts while we reconfigure the controller (CH570 init order;
     * harmless on CH592 since the PFIC IRQ isn't enabled until the end). */
    R8_USB_INT_EN = 0x00;

    /* Optional SIE soft-reset before configuring (CH570 bring-up knob). */
    R8_USB_CTRL = RB_UC_RESET_SIE;
    DelayUs(10);

    /* Reset the USB controller to a known state before configuring (matches the
     * SDK USB_DeviceInit and the validated CH582F port). */
    R8_USB_CTRL = 0x00;
    R8_UDEV_CTRL = 0x00;

    /* Chip-specific pin/PHY pre-detach. On CH570 this releases the PA0/PA1 debug
     * mux for USB (and runs any pull-up dance); on CH59x/CH58x it is a no-op. */
    hal_usb_pins_predetach();

    /* (The SDK's pEPn_RAM_Addr globals are unused here — we program the EP DMA
     * registers R16_UEPn_DMA directly below rather than through the SDK's USB
     * device layer, so we don't link CH59x_usbdev.c.) */

    /* EP1/EP2/EP3 are HID IN-only in the descriptor, so keep them TX-only and
     * write their report buffers at DMA offset 0. EP4 has no descriptor and no
     * ISR case, so its RX/TX enables stay off (its CTRL below still parks it
     * at NAK defensively). EP5 is TX-only; EP6 stays dual-buffered for IAP
     * OUT traffic. */
    R8_UEP4_1_MOD = RB_UEP1_TX_EN;
    R8_UEP2_3_MOD = RB_UEP2_TX_EN | RB_UEP3_TX_EN;
    R8_UEP567_MOD = RB_UEP5_TX_EN |
                    RB_UEP6_TX_EN | RB_UEP6_RX_EN;

    /* Set DMA addresses for EP0-EP3 */
    R16_UEP0_DMA = (uint16_t)(uint32_t)EP0_Buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)EP1_Buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)EP2_Buf;
    R16_UEP3_DMA = (uint16_t)(uint32_t)EP3_Buf;

    /* Set DMA addresses for EP5/EP6 */
    R16_UEP5_DMA = (uint16_t)(uint32_t)EP5_Buf;
    R16_UEP6_DMA = (uint16_t)(uint32_t)EP6_Buf;

    /* Endpoint control: data endpoints start with T_RES=NAK (nothing to send
     * yet). OUT-capable endpoints start with R_RES=ACK; TX-only HID endpoints
     * leave R_RES=NAK. IN handlers flip T_RES to ACK when they queue data. */
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP2_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP3_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP4_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;  /* defensive; EP4 unused */
    R8_UEP5_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK | USB_HID_IN_TOG_MODE;
    R8_UEP6_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;

    /* Set TX lengths to 0 */
    R8_UEP0_T_LEN = 0;
    R8_UEP1_T_LEN = 0;
    R8_UEP2_T_LEN = 0;
    R8_UEP3_T_LEN = 0;
    R8_UEP5_T_LEN = 0;
    R8_UEP6_T_LEN = 0;

    /* USB device address = 0 */
    R8_USB_DEV_AD = 0x00;

    /* Enable USB device, DMA, interrupt-on-busy */
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;

    /* Enable USB port with internal pull-up, then clear-and-enable interrupts.
     * Order matches SDK: pin-enable -> INT_FG clear -> UDEV_CTRL -> INT_EN. The
     * analog USB IO + D+ pull-up enable is chip-specific (CH59x: R16_PIN_ANALOG_IE;
     * CH570: R16_PIN_ALTERNATE), so it lives behind the pin HAL. */
    hal_usb_pins_enable();
    R8_USB_INT_FG = 0xFF;
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;

    /* Chip-specific end-of-init re-attach. On CH59x/CH58x this performs a brief
     * D+ disconnect/reconnect so a host still holding the prior WCH-ISP/SWD
     * session re-probes our descriptors; on CH570 it is a no-op. */
    hal_usb_pins_reattach();

    /* Enable USB IRQ in PFIC */
    PFIC_EnableIRQ(USB_IRQn);

    /* Init protocol to report mode (1) for boot interfaces */
    usb_protocol[0] = 1;
    usb_protocol[1] = 1;

    
}

/* ---------- Report sending ---------- */

/* EP1/EP2/EP3 IN endpoints are armed from multiple contexts (an RF/TMR ISR
 * delivering a decoded report, plus the foreground resume-clear) while the USB
 * ISR also updates EP state. Keep each endpoint buffer/control update single-copy
 * w.r.t. the USB/RF/TMR interrupt contexts. Gate on "configured and not
 * suspended": before SET_CONFIGURATION there is no host listening, and while
 * suspended a queued report would deliver as a stale keypress on resume.
 * (Originally the CH570 path; harmless and strictly safer on CH59x/CH58x, whose
 * RF callback context differs.)
 *
 * DESIGN NOTE (low; not a beta blocker): this gates only on
 * configured/not-suspended, NOT on per-endpoint pending/consumed state. Two
 * reports to the SAME endpoint within one ~1 ms USB frame (the RF poll cadence
 * is ~875 us, faster than the 1 ms IN poll) coalesce/replace rather than queue,
 * and rewriting an already-ACK-armed buffer can in principle race the SIE DMA.
 * Benign at current rates and bench-validated; revisit if mouse/report rate
 * rises or true queued HID semantics are added (a per-EP busy flag + NAK before
 * rewrite, or double-buffering). */
static uint8_t usb_hid_in_ready(void)
{
    /* Use an "effective suspended" that also consults the hardware suspend bit,
     * not just the usb_suspended flag: a report can arrive over RF in the small
     * window after the bus enters suspend but before the suspend ISR sets the
     * flag. Without this the report would arm an IN endpoint while the host is
     * asleep (a stale keypress on resume) and miss requesting the wake. */
    uint8_t suspended = usb_effective_suspended();

    /* A report wants to go out but the host is asleep and has armed remote
     * wakeup: ask the main loop to drive the resume K-state. The report is
     * still dropped here (host not listening); once the host resumes, the
     * keyboard's continued reports deliver normally. Side-effect lives here so
     * all three USB_Send* paths request the wake from one place. */
    if (usb_config != 0u && suspended && usb_remote_wakeup) {
        usb_wake_request = 1;
    }
    return (uint8_t)(usb_config != 0u && !suspended);
}

void USB_ServiceRemoteWake(void)
{
    uint8_t do_drive = 0;

    /* Fast path, read UNMASKED on purpose: with nothing pending there is nothing
     * to make atomic. This matters because USB_ServiceRemoteWake runs every
     * foreground Main_Circulation pass -- taking the IRQ mask below on every
     * iteration would churn CSR 0x800 (MIE/MPIE) and can leave USB_IRQn masked
     * during enumeration, a bench-confirmed enumeration hazard on this silicon
     * (see the RF_SetLEDState note in rf_task.c and the CSR-0x800 warning near
     * main.c). usb_wake_request is only ever set while suspended, so the masked
     * decision runs at most a handful of times per suspend episode, never in the
     * enumeration loop. A request posted right after this check is serviced on
     * the next pass. */
    if (!usb_wake_request) {
        return;
    }

    /* Run the whole decision under one IRQ mask so the USB ISR (which clears
     * usb_wake_inflight, and on resume usb_suspended) cannot interleave
     * (CODEREVIEW F13). Two races otherwise: (1) an ISR clear of
     * usb_wake_inflight=0 gets overwritten by our stale =1, stranding the latch
     * and disabling remote wake for the whole next suspend; (2) a stale "awake"
     * snapshot lets us clear a wake request that an RF keypress set just after a
     * suspend edge, losing the wake with no guaranteed retry (reports are
     * change-driven). Reading the suspend state, clearing a stale request, and
     * claiming the drive must therefore be atomic. The ~2 ms K-state drive
     * itself stays OUTSIDE the mask. */
    uint32_t irq = __risc_v_disable_irq();
    if (!usb_effective_suspended()) {
        usb_wake_request = 0;   /* resumed (by us or the host) - drop stale req */
    } else if (usb_wake_request && usb_remote_wakeup && !usb_wake_inflight) {
        /* Drive the K-state at most once per suspend episode: the inflight latch
         * is cleared only on the resume/bus-reset edge, so further RF reports
         * while still suspended cannot re-pulse the bus. */
        usb_wake_request = 0;
        usb_wake_inflight = 1;
        do_drive = 1;
    }
    (void)__risc_v_enable_irq(irq);

    /* Re-check immediately before the drive: if a resume ISR fired since the
     * mask released, do not pulse an already-active bus. */
    if (do_drive && usb_effective_suspended()) {
        hal_usb_remote_wakeup();   /* foreground ~2 ms K-state */
    }
}

/* All queue helpers run with IRQs already masked by the caller. */
static uint8_t usb_kbd_q_count(void)
{
    return (uint8_t)(usb_kbd_q_tail - usb_kbd_q_head);
}

/* Queue one complete keyboard state. Deduped ONLY against the newest queued
 * state, so down -> up -> same-down is preserved; with the queue empty we do not
 * dedup at all (re-sending a state the host already has is idempotent, and
 * keeping a last-completed copy costs SRAM CH570 does not have). A full queue
 * coalesces onto the newest entry rather than dropping the report: the source
 * only sends changes, so a discarded all-keys-up would strand a key forever. */
static void usb_kbd_q_push(const uint8_t report[8])
{
    uint8_t n = usb_kbd_q_count();
    uint8_t *slot;

    if (n != 0u) {
        const uint8_t *newest = usb_kbd_q[(uint8_t)(usb_kbd_q_tail - 1u) % DONGLE_EP1_QUEUE_SLOTS];
        uint8_t same = 1u;
        for (int i = 0; i < 8; i++) {
            if (newest[i] != report[i]) { same = 0u; break; }
        }
        if (same) {
            return;
        }
    }
    if (n < DONGLE_EP1_QUEUE_SLOTS) {
        slot = usb_kbd_q[usb_kbd_q_tail % DONGLE_EP1_QUEUE_SLOTS];
        for (int i = 0; i < 8; i++) {
            slot[i] = report[i];
        }
        usb_kbd_q_tail++;
    } else {
        /* Never the in-flight head while full (>= 2 slots, checked above). */
        slot = usb_kbd_q[(uint8_t)(usb_kbd_q_tail - 1u) % DONGLE_EP1_QUEUE_SLOTS];
        for (int i = 0; i < 8; i++) {
            slot[i] = report[i];
        }
        /* Surfaced through the existing public overwrite metric: with the queue
         * in place this is the only path that still discards a transition. */
        usb_ep1_overwrites++;
    }
}

/* The ONE place EP1 is armed. Arms the head only when no transfer is in flight,
 * so a queued report can never overwrite one the host has not collected. */
static void usb_ep1_service(void)
{
    const uint8_t *e;

    if (usb_kbd_q_owned) {
        return;                       /* the head is in flight; it must complete first */
    }
    if (usb_kbd_q_head == usb_kbd_q_tail) {
        return;                       /* nothing queued */
    }
    if (usb_config == 0u || usb_effective_suspended()) {
        return;                       /* host is not collecting; the queue retains it */
    }
    e = usb_kbd_q[usb_kbd_q_head % DONGLE_EP1_QUEUE_SLOTS];
    for (int i = 0; i < 8; i++) {
        EP1_IN()[i] = e[i];
    }
    R8_UEP1_T_LEN = 8;
    usb_kbd_q_owned = 1;
    usb_ep1_armed = 1;
    usb_ep1_arms++;
#if DONGLE_DELIVERY_COUNTERS
    {   uint8_t nz = e[0]|e[1]|e[2]|e[3]|e[4]|e[5]|e[6]|e[7];
        usb_ep1_down_inflight = nz ? 1u : 0u;
        if (nz) usb_ep1_arms_down++; }
#endif
    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
}

/* A fresh boot-keyboard report. Once it is actually armed it clears the
 * resume reconciliation flag (it carries the whole current key state); the
 * suspend-time stash below does not, since nothing reached the host. */
USB_HID_SEND_HIGHCODE
void USB_SendKeyboard(const uint8_t report[8])
{
    uint32_t irq_state = __risc_v_disable_irq();

    /* Keeps the remote-wake side effect in one place: a report offered while the
     * host is suspended and has armed remote wakeup asks the main loop to drive
     * the K-state. Remote-wakeup permission governs SIGNALLING only -- queue
     * admission below is unconditional, so the report itself is never lost. */
    (void)usb_hid_in_ready();

    if (usb_config != 0u) {
        usb_kbd_q_push(report);
        /* A queued report carries the whole current key state, so the resume
         * all-keys-up flush must not also fire. */
        usb_reconcile_ep1 = 0;
    }
    usb_ep1_service();
    (void)__risc_v_enable_irq(irq_state);
}

/* EP1 resume delivery: the key state stashed at wake time (the waking
 * keystroke, or all-up if it was released before resume), else keys-up so a
 * pre-suspend held key cannot stay logically stuck. ONE IRQ mask covers the
 * whole decision - the fresh-report gate, the suspend check, the stash
 * consumption and the arm - because the pieces interact (codex review):
 *  - a fresh report armed since the resume edge (usb_reconcile_ep1 clear)
 *    carries the whole key state and outranks the stash: deliver nothing, and
 *    LEAVE the stash alone - if the bus has suspended again meanwhile, that
 *    stash belongs to the new episode and its resume edge (which re-arms the
 *    flag and usb_resume_clear_kbd) delivers it;
 *  - the bus already suspended again: same deferral, nothing consumed, and no
 *    call into usb_hid_in_ready, whose side effect would request a remote
 *    wake for a flush that is not a keystroke;
 *  - otherwise consume the stash (or take keys-up), clear the flag and arm.
 * Called from the main loop only. */
static void usb_reconcile_keyboard(void)
{
    static const uint8_t keys_up[8] = { 0 };
    uint32_t irq_state = __risc_v_disable_irq();

    if (!usb_reconcile_ep1 || usb_config == 0u || usb_effective_suspended()) {
        (void)__risc_v_enable_irq(irq_state);
        return;
    }
    usb_reconcile_ep1 = 0;
    /* The queue IS the retained history across the suspend, and draining it
     * delivers the real transitions in order. Only when nothing was retained do
     * we synthesise a release, so a key held before the suspend cannot stay
     * logically stuck. A fresh report that arrived since the resume edge has
     * already cleared usb_reconcile_ep1, so it is never overridden here. */
    if (usb_kbd_q_head == usb_kbd_q_tail) {
        usb_kbd_q_push(keys_up);
    }
    usb_ep1_service();
    (void)__risc_v_enable_irq(irq_state);
}

/* Invalidate the remote-wake keyboard stash WITHOUT requesting a wake or
 * sending anything. Called on an RF link loss: if a wake-time key-DOWN was
 * stashed (USB_SendKeyboard's suspend path above, valid=1) and the keyboard
 * then dies, the resume flush in USB_PollEP6 would otherwise replay that
 * key-down with no keyboard left to ever send the matching key-up -- a
 * logically stuck key (CODEREVIEW P2). Clearing valid=0 makes that flush take
 * its explicit all-keys-up branch instead. Deliberately does NOT touch
 * usb_wake_request (a dead keyboard must never spuriously wake a suspended
 * host) and does NOT go through usb_hid_in_ready(). IRQ-masked: atomic vs the
 * USB_SendKeyboard stash writer and the USB_PollEP6 resume consumer. */
void USB_ClearPendingKeyboard(void)
{
    static const uint8_t keys_up[8] = { 0 };
    uint32_t irq_state = __risc_v_disable_irq();
    /* Link loss. Do NOT discard the queue -- those are real transitions the host
     * still needs. Instead append the release through the queue so it lands in
     * order behind them. This must happen even while suspended: the RF layer
     * skips its own all-keys-up in that state, which would otherwise resume with
     * a key-down and no key-up (a stuck key). Deliberately does not go through
     * USB_SendKeyboard, so losing the link never requests a remote wake. */
    if (usb_config != 0u) {
        usb_kbd_q_push(keys_up);
    }
    usb_ep1_service();
    (void)__risc_v_enable_irq(irq_state);
}

/* Arm EP2 with a 5-byte mouse report. reconcile=0 is a fresh report: it
 * clears the resume reconciliation flag, since it carries the device's whole
 * current state. reconcile=1 is the resume all-up flush: it is sent only if
 * no fresh report has cleared the flag since the resume edge. One IRQ mask
 * covers the flag decision and the arm, so the two orderings cannot cross. */
USB_HID_SEND_HIGHCODE
static void usb_arm_mouse(const uint8_t report[5], uint8_t reconcile)
{
    uint32_t irq_state = __risc_v_disable_irq();
    if (reconcile) {
        /* The flush: skipped if a fresh report already reconciled, deferred
         * (flag kept for the next resume edge) if the bus suspended again or
         * is unconfigured - checked directly, not through usb_hid_in_ready,
         * whose side effect would request a remote wake for a flush. */
        if (!usb_reconcile_ep2 || usb_config == 0u || usb_effective_suspended()) {
            (void)__risc_v_enable_irq(irq_state);
            return;
        }
    } else if (!usb_hid_in_ready()) {
        (void)__risc_v_enable_irq(irq_state);
        return;                     /* dropped: the flag stays, nothing reached the host */
    }
    usb_reconcile_ep2 = 0;          /* only a report that is actually armed reconciles */
    for (int i = 0; i < 5; i++)
        EP2_IN()[i] = report[i];
    R8_UEP2_T_LEN = 5;
    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    (void)__risc_v_enable_irq(irq_state);
}

USB_HID_SEND_HIGHCODE
void USB_SendMouse(const uint8_t report[5])
{
    usb_arm_mouse(report, 0u);
}

/* EP3 twin of usb_arm_mouse. Only a consumer report (report ID 1) counts as
 * fresh reconciliation of the consumer state; the other IDs the composite
 * interface declares (2 sysctl, 3 NKRO, usb_device.h) leave the flag alone. */
USB_HID_SEND_HIGHCODE
static void usb_arm_composite(const uint8_t *report, uint8_t len, uint8_t reconcile)
{
    uint32_t irq_state = __risc_v_disable_irq();
    if (reconcile) {
        /* Same three-way gate as usb_arm_mouse. */
        if (!usb_reconcile_ep3 || usb_config == 0u || usb_effective_suspended()) {
            (void)__risc_v_enable_irq(irq_state);
            return;
        }
    } else if (!usb_hid_in_ready()) {
        (void)__risc_v_enable_irq(irq_state);
        return;                     /* dropped: the flag stays, nothing reached the host */
    }
    if (len != 0u && report[0] == 1u)
        usb_reconcile_ep3 = 0;      /* only a consumer report that is actually armed reconciles */
    if (len > 16) len = 16;
    for (int i = 0; i < len; i++)
        EP3_IN()[i] = report[i];
    R8_UEP3_T_LEN = len;
    R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    (void)__risc_v_enable_irq(irq_state);
}

USB_HID_SEND_HIGHCODE
void USB_SendComposite(const uint8_t *report, uint8_t len)
{
    usb_arm_composite(report, len, 0u);
}

void USB_SendEP6(const uint8_t *data, uint8_t len)
{
    /* Interface 4's IN report is 64 bytes. Linux hidraw (and the Windows
     * HFD_IAPTool) buffer short IN packets until a full-size report arrives, so
     * every IAP response must be padded to the full 64 bytes or the host never
     * sees it (flash_dongle.py --info would time out on the handshake even
     * though the bytes went out). Stock firmware pads too; matches CH582F. */
    if (len > 64) len = 64;
    for (int i = 0; i < len; i++)
        EP_DUAL_IN(EP6_Buf)[i] = data[i];
    for (int i = len; i < 64; i++)
        EP_DUAL_IN(EP6_Buf)[i] = 0x00;
    R8_UEP6_T_LEN = 64;
    /* R8_UEP6_CTRL is a single byte holding both the OUT (R_RES/R_TOG) and IN
     * (T_RES/T_TOG) fields; the USB ISR's EP6-IN completion RMWs the same byte.
     * Mask IRQs around this read-modify-write so a concurrent IN completion
     * can't be lost (the keyboard/mouse/composite senders do the same). */
    uint32_t irq_state = __risc_v_disable_irq();
    R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    (void)__risc_v_enable_irq(irq_state);
}

int USB_EP6InIdle(void)
{
    /* Single-byte register read; the T_RES field is written only by
     * USB_SendEP6 (to ACK) and the ISR's EP6-IN completion (back to NAK),
     * so no IRQ masking is needed to sample it. */
    return (R8_UEP6_CTRL & MASK_UEP_T_RES) == UEP_T_RES_NAK;
}

void USB_SetEP6OutCallback(usb_ep6_out_cb_t cb)
{
    ep6_out_cb = cb;
}

void USB_PollEP6(void)
{
    /* On a suspend->resume edge, flush one all-keys-up boot-keyboard report so a
     * key-up dropped during suspend can't leave a key logically stuck. Done from
     * the main loop (not the ISR) so we don't race the RF-IRQ USB_SendKeyboard
     * on EP1; usb_suspended is already 0 here so the send goes through.
     * Gate on configured (V5, v0.96 review): a resume edge that lands during
     * pre-SET_CONFIGURATION enumeration would otherwise call USB_SendKeyboard
     * with usb_config==0, doing pointless IRQ-masked work in the enum window
     * (harmless only because usb_hid_in_ready re-drops it). A bus reset — the
     * only thing that clears usb_config — also empties the EP1 queue, so
     * have_pending implies configured; the gate never drops a real flush. */
    if (usb_resume_clear_kbd && usb_config != 0u) {
        usb_resume_clear_kbd = 0;
        /* The keyboard: the stash captured at wake time or keys-up, gated on
         * no fresh report since the resume edge and deferred if the bus has
         * suspended again (usb_reconcile_keyboard). */
        usb_reconcile_keyboard();
        /* The same reconciliation for the change-driven mouse (EP2) and
         * consumer (EP3) reports, which have no stash: a report armed when
         * SOF stopped was NAKed by USB_SuspendResume and anything that
         * arrived during suspend was dropped, so the host's last-seen state
         * for either can be a press whose release it will never get (a
         * stuck button or held media key); Linux also keeps the last
         * consumer report and would suppress the next identical press. All
         * buttons up with zero motion, and consumer usage 0 under report ID
         * 1 (the composite interface's consumer report, usb_device.h), put
         * the host back to the released state; the next real report
         * re-asserts whatever is still held. Each flush is skipped if a
         * fresh report on that endpoint has already gone out since the
         * resume edge (usb_reconcile_ep2/ep3): that report IS the current
         * state, and flushing after it would drop or release a real press
         * that landed in the window between the resume ISR and this loop.
         * Residual, stated: in either ordering within the resume window
         * (a fresh report cancelling the flush before it arms, or replacing
         * it before the host polls it) the host never receives the no-key
         * report. Harmless for the mouse (absolute button state); for the
         * consumer, a host that compares against its retained report (Linux)
         * would then suppress a press of the SAME usage it last saw before
         * suspend, until any other consumer report arrives. Same readiness
         * gate as any report. */
        {
            static const uint8_t mouse_up[5] = { 0, 0, 0, 0, 0 };
            static const uint8_t consumer_none[3] = { 1u, 0, 0 };
            usb_arm_mouse(mouse_up, 1u);
            usb_arm_composite(consumer_none, sizeof(consumer_none), 1u);
        }
    }

    /* Drain a deferred IAP command from the main loop (see the EP6-OUT case in
     * USB_IRQHandler). The callback runs the IAP command and sends its EP6-IN
     * reply via USB_SendEP6; it may block for ms (flash erase/program), which
     * is fine here — we are not in ISR context. Clear the pending flag before
     * re-ACK'ing EP6 OUT so a fresh command that arrives right after the ACK
     * latches cleanly. */
    if (!iap_pkt_pending)
        return;
    if (ep6_out_cb)
        ep6_out_cb(EP_OUT(EP6_Buf), iap_pkt_len);
    iap_pkt_pending = 0;
    /* Re-ACK EP6 OUT. Same shared-register hazard as USB_SendEP6: the ISR's
     * EP6-IN completion RMWs R8_UEP6_CTRL, so mask IRQs around this RMW. */
    uint32_t irq_state = __risc_v_disable_irq();
    R8_UEP6_CTRL = (R8_UEP6_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
    (void)__risc_v_enable_irq(irq_state);
}

uint8_t USB_HasPendingWork(void)
{
    return (uint8_t)(iap_pkt_pending || usb_resume_clear_kbd);
}

uint8_t USB_IsConfigured(void)
{
    return (uint8_t)(usb_config != 0u);
}

uint8_t USB_IsSuspended(void)
{
    return usb_effective_suspended();
}

uint16_t USB_SuspendEpisodes(void)
{
    return usb_suspend_episodes;
}

uint32_t USB_Ep1Arms(void)        { return usb_ep1_arms; }
uint32_t USB_Ep1Completions(void) { return usb_ep1_completions; }
uint32_t USB_Ep1Overwrites(void)  { return usb_ep1_overwrites; }

uint8_t USB_GetLEDState(void)
{
    return usb_led_state;
}
