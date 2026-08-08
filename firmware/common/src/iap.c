/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * Common IAP command handler.
 */

#include "iap.h"

#include "bond.h"
#include "dongle_chip.h"        /* __risc_v_disable_irq (same intrinsic as usb_device.c) */
#include "dongle_platform.h"
#include "dongle_target.h"      /* HAL_TICKS_PER_US (IAP_Service deadlines) */
#include "hal_timing.h"         /* hal_now (IAP_Service deadlines) */
#include "dongle_image_id.h"
#include "openboot_app.h"       /* openboot_request_update (noreturn) */
#include "usb_device.h"
#if DONGLE_HAS_RF
#include "rf_task.h"            /* RF_TombstoneBond (N06), RF_Quiesce* (0x85) */
#endif

#include <string.h>

#define CMD_HANDSHAKE   0x5A
#define CMD_PROGRAM     0x80    /* retired: OpenBoot owns flashing */
#define CMD_ERASE       0x81    /* retired */
#define CMD_VERIFY      0x82    /* retired */
#define CMD_END         0x83    /* retired */
#define CMD_GETDEVINFO  0x84
#define CMD_ENTER_BOOT  0x85
#define CMD_BOND_WRITE  0x87
#define CMD_BOND_READ   0x88
#define CMD_BOND_CLEAR  0x89
#define CMD_VERSION     0x90
#define CMD_STATUS      0x91
#define CMD_FAULT       0x93

#define ACK_OK          0x0F
#define ACK_HANDSHAKE   0xA5
#define ACK_DEVINFO     0x04
#define ACK_BOND_READ   0x88
#define ACK_STATUS      0x91
#define ACK_FAULT       0x93

static uint8_t iap_armed;
static uint8_t iap_err_count;

/* Set by a valid 0x85 EnterBootloader; consumed by IAP_Service(), which
 * drives the actual reboot into OpenBoot from the main loop (never inline
 * in the packet handler, so the status reply can still go out). */
static uint8_t iap_reboot_pending;

static void send_reject(void)
{
    /* Match the stock dispatcher: malformed or disallowed packets time out. */
}

static void send_status4(uint8_t status)
{
    uint8_t resp[4] = { ACK_OK, 0x01, status, 0x00 };
    USB_SendEP6(resp, sizeof(resp));
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void handle_handshake(const uint8_t *data, uint8_t data_len)
{
    static const uint8_t expected[7] = { 'W','C','H','@','H','F','D' };
    uint8_t resp[7];

    if (data_len != sizeof(expected)) {
        send_reject();
        return;
    }
    for (uint8_t i = 0; i < sizeof(expected); i++) {
        if (data[i] != expected[i]) {
            send_reject();
            return;
        }
    }

    resp[0] = ACK_HANDSHAKE;
    resp[1] = 0x04;
    resp[2] = 0x02;
    resp[3] = 0x00;
    resp[4] = 0x00;
    resp[5] = 0x00;
    resp[6] = 0xAB;
    USB_SendEP6(resp, sizeof(resp));
}

static void handle_getdevinfo(const uint8_t *data, uint8_t data_len)
{
    if (data_len != 4) {
        send_reject();
        return;
    }

    uint32_t arg = (uint32_t)data[0]
                 | ((uint32_t)data[1] << 8)
                 | ((uint32_t)data[2] << 16)
                 | ((uint32_t)data[3] << 24);

    if (arg == 1) {
        uint8_t resp[12];

        iap_armed = 1;
        iap_err_count = 0;

        resp[0] = ACK_DEVINFO;
        resp[1] = 0x08;
        /* Bytes 2..5 carried the in-app staging offset (LE32) and bytes 6..7
         * the flash block size (LE16). In-app staging is gone -- updates run
         * inside OpenBoot over OBP -- so the geometry reports as zero while
         * the 12-byte response shape stays intact for host compatibility. */
        put_le32(&resp[2], 0u);
        put_le16(&resp[6], 0u);
        resp[8] = DONGLE_CHIP_FAMILY_ID;
        resp[9] = 0x00;
        resp[10] = 0x00;
        resp[11] = 0x00;
        USB_SendEP6(resp, sizeof(resp));
        return;
    }

    iap_armed = 0;
    uint8_t resp[8] = { ACK_OK, 0x01, 0xF3, 0x03, 0x00, 0x00, 0xAB, 0x00 };
    USB_SendEP6(resp, sizeof(resp));
}

/* 0x80 Program / 0x81 Erase / 0x82 Verify / 0x83 End are retired: in-app
 * staging is gone, all flashing happens inside OpenBoot over OBP. An armed
 * session gets an explicit terminal status (0xF8 unsupported; End keeps its
 * historical 0xF9 no-commit) so a legacy updater fails fast instead of
 * appearing to stage an image; unarmed they are silently dropped, matching
 * the pre-existing unarmed behavior of the flash commands. */
static void handle_retired(uint8_t status)
{
    if (!iap_armed) {
        send_reject();
        return;
    }
    iap_err_count++;
    send_status4(status);
}

/* 0x85 EnterBootloader (armed-only): body must be exactly OB_BOOTREQ_MAGIC
 * (0xB007CA11) little-endian. On success replies status 0x00 and latches
 * iap_reboot_pending; IAP_Service() then quiesces RF, drains the reply, and
 * reboots into OpenBoot. Never resets inline here. */
static void handle_enter_bootloader(const uint8_t *data, uint8_t data_len)
{
    /* Derived from the protocol header rather than spelled out, so it cannot
     * drift from OpenBoot. Kept as a byte array compared with memcmp: `data`
     * points into the 64-byte HID report at offset 2, so it is at best 2-byte
     * aligned and a uint32_t load through it would be misaligned. */
    static const uint8_t magic[4] = {
        (uint8_t)(OB_BOOTREQ_MAGIC        & 0xFFu),
        (uint8_t)((OB_BOOTREQ_MAGIC >> 8)  & 0xFFu),
        (uint8_t)((OB_BOOTREQ_MAGIC >> 16) & 0xFFu),
        (uint8_t)((OB_BOOTREQ_MAGIC >> 24) & 0xFFu),
    };

    if (!iap_armed) {
        send_reject();
        return;
    }
    if (data_len != sizeof(magic) || memcmp(data, magic, sizeof(magic)) != 0) {
        iap_err_count++;
        send_status4(DONGLE_IAP_STATUS_UNSUPPORTED);
        return;
    }
    send_status4(0x00);
    iap_reboot_pending = 1;
}

static void handle_bond_write(const uint8_t *data, uint8_t data_len)
{
    if (!iap_armed) {
        send_reject();
        return;
    }

    if (data_len != sizeof(bond_record_t)) {
        iap_err_count++;
        send_status4(0xB0);
        return;
    }

    bond_record_t rec;
    for (uint8_t i = 0; i < sizeof(rec); i++) {
        ((uint8_t *)&rec)[i] = data[i];
    }

    if (rec.magic != BOND_MAGIC || rec.version != BOND_VERSION
        || bond_checksum(&rec) != rec.checksum || rec.session_aa == 0) {
        iap_err_count++;
        send_status4(0xB1);
        return;
    }

    /* CODEREVIEW N08: semantic validation (bounds, relational
     * interval/timeout, MAC hygiene, peer != effective dongle identity).
     * 0xB2 = semantically-rejected (0xB1 stays structural). The identity leg
     * uses the FACTORY MAC (not a live override — a record replacing an
     * override is judged against the chip identity). */
    if (!bond_record_semantic_valid(&rec,
#if DONGLE_HAS_RF
                                    RF_FactoryMac()
#else
                                    0
#endif
                                    )) {
        iap_err_count++;
        send_status4(0xB2);
        return;
    }

    /* V2: key/flag canonical form. 0xB3 = a provisioned key that is blank/erased,
     * or a key left in the record without BOND_FLAG_ENC_KEY set. Also rejects a
     * redacted BondRead view written straight back (ENC_KEY set, key zeroed). */
    if (!bond_key_flags_valid(&rec)) {
        iap_err_count++;
        send_status4(0xB3);
        return;
    }

    uint8_t status = (uint8_t)bond_save(&rec);
    if (status) {
        iap_err_count++;
    }
    send_status4(status);
}

static void handle_bond_clear(void)
{
    if (!iap_armed) {
        send_reject();
        return;
    }

    /* Firmware-side erase + readback verification (the only trustworthy
     * clear: host-side DataFlash writes around the running RF firmware
     * corrupt the page). On success, RF_TombstoneBond() (below) invalidates the
     * in-RAM bond and blocks re-persist + new accept/promote until reset so the
     * clear survives; a currently-live link keeps forwarding until it drops (it
     * cannot reacquire or re-persist), and is gone at the next reset. */
    uint8_t status = (uint8_t)bond_clear();
    if (status == 0) {
#if DONGLE_HAS_RF
        /* CODEREVIEW N06: the DataFlash record is now verified-erased. Tell the
         * running RF task to drop its in-RAM bond + persist latch and stop
         * re-persisting (and stop accepting/promoting a new pair) until reset, so
         * the clear actually survives to the next boot (otherwise the live in-RAM
         * bond, or an already-posted persist, re-writes the record just erased). */
        RF_TombstoneBond();
#endif
    } else {
        iap_err_count++;
    }
    send_status4(status);
}

static void handle_bond_read(void)
{
    if (!iap_armed) {
        send_reject();
        return;
    }

    /* Zero-init: bond_load() returns early without filling rec on an NV read
     * error, and the response always copies sizeof(rec) regardless of validity
     * -- without this, a failed read would leak uninitialized stack over USB. */
    bond_record_t rec = {0};
    int valid = bond_load(&rec);
    uint8_t redacted = 0u;

    /* Never expose the link key over USB: BondRead is reachable by any host
     * process that can open the vendor HID, so a readable key would be a
     * disclosure oracle. Zero it, recompute the checksum so the view stays
     * self-consistent for display, and flag it in status bit 1. The resulting
     * ENC_KEY-set / zero-key combination is also non-importable -- BondWrite
     * rejects it with 0xB3 -- so a redacted view cannot be written back. */
    if (valid && (rec.flags & BOND_FLAG_ENC_KEY)) {
        for (uint8_t i = 0; i < sizeof(rec.link_key); i++) {
            rec.link_key[i] = 0u;
        }
        rec.checksum = bond_checksum(&rec);
        redacted = 0x02u;
    }

    uint8_t resp[3 + sizeof(bond_record_t)];
    resp[0] = ACK_BOND_READ;
    resp[1] = (uint8_t)sizeof(bond_record_t);
    resp[2] = (uint8_t)((valid ? 0x00u : 0x01u) | redacted);
    for (uint8_t i = 0; i < sizeof(rec); i++) {
        resp[3 + i] = ((const uint8_t *)&rec)[i];
    }
    USB_SendEP6(resp, sizeof(resp));
}

static void handle_version(void)
{
    uint8_t resp[64] = {0};
    uint8_t len = (uint8_t)strlen(DONGLE_FW_VERSION) + 1u;
    if (len > sizeof(resp)) {
        len = sizeof(resp);
    }
    memcpy(resp, DONGLE_FW_VERSION, len);
    USB_SendEP6(resp, sizeof(resp));
}

static void handle_status(void)
{
    uint8_t resp[2u + DONGLE_STATUS_PAYLOAD_LEN] = {0};
    uint8_t *payload = &resp[2];
    uint8_t capabilities = DONGLE_CAP_IAP | DONGLE_CAP_BOND
                         | DONGLE_CAP_FAULT;

#if DONGLE_HAS_RF
    capabilities |= DONGLE_CAP_RF;
#endif
    resp[0] = ACK_STATUS;
    resp[1] = DONGLE_STATUS_PAYLOAD_LEN;
    payload[DONGLE_STATUS_OFF_SCHEMA] = DONGLE_STATUS_SCHEMA;
    payload[DONGLE_STATUS_OFF_FAMILY] = DONGLE_CHIP_FAMILY_ID;
#if DONGLE_HAS_RF
    payload[DONGLE_STATUS_OFF_CONNECTION] = RF_GetConnectionStatus();
    /* Signed dBm in an unsigned byte; the host sign-extends. Reported so the
     * pair-RSSI floor can be measured rather than guessed - it was previously
     * only observable by flashing diagnostic builds. */
    payload[DONGLE_STATUS_OFF_LAST_RSSI] = (uint8_t)RF_GetRSSI();
    memcpy(&payload[DONGLE_STATUS_OFF_DONGLE_MAC], RF_GetDongleMac(),
           DONGLE_MAC_LEN);
#else
    payload[DONGLE_STATUS_OFF_CONNECTION] = DONGLE_CONNECTION_UNAVAILABLE;
#endif
    /* Updates run inside OpenBoot now; the app never holds a staged or
     * pending image of its own, so the update state is always clean. */
    payload[DONGLE_STATUS_OFF_UPDATE] = DONGLE_UPDATE_CLEAN;
    payload[DONGLE_STATUS_OFF_CAPABILITIES] = capabilities;
    payload[DONGLE_STATUS_OFF_PROFILE] = DONGLE_BUILD_PROFILE;
    payload[DONGLE_STATUS_OFF_UID_LEN] =
        dongle_unique_id_fill(&payload[DONGLE_STATUS_OFF_UID], DONGLE_UID_LEN);
    put_le32(&payload[DONGLE_STATUS_OFF_BUILD_ID], DONGLE_BUILD_ID);
#if DONGLE_IAP_IMAGE_ID
    put_le32(&payload[DONGLE_STATUS_OFF_IMAGE_LEN], dongle_image_id.image_len);
#endif
    USB_SendEP6(resp, sizeof(resp));
}

/* Read-only target fault page. It is deliberately unarmed: retained fatal
 * evidence must remain observable after the recovery boot. */
static void handle_fault(void)
{
    uint8_t resp[64] = {0};

    resp[0] = ACK_FAULT;
    resp[1] = dongle_fault_fill(&resp[2], (uint8_t)(sizeof(resp) - 2u));
    USB_SendEP6(resp, sizeof(resp));
}

void IAP_PacketHandler(const uint8_t *pkt, uint8_t rx_len)
{
    /* Once a reboot is latched, every further command is dropped without a
     * reply: USB_SendEP6 has no busy check, so answering would overwrite
     * the pending status-0 reply the host has not taken yet, and nothing
     * may cancel the reboot anyway (disarm clears only the session flag). */
    if (iap_reboot_pending) {
        return;
    }
    if (rx_len < 3) {
        return;
    }

    uint8_t cmd = pkt[0];
    uint8_t len = pkt[1];
    if ((uint32_t)len + 3 > rx_len) {
        return;
    }

    uint8_t sum = cmd + len;
    for (uint8_t i = 0; i < len; i++) {
        sum += pkt[2 + i];
    }
    if (sum != pkt[2 + len]) {
        return;
    }

    const uint8_t *data = &pkt[2];

    switch (cmd) {
    case CMD_HANDSHAKE:   handle_handshake(data, len); break;
    case CMD_GETDEVINFO:  handle_getdevinfo(data, len); break;
    case CMD_PROGRAM:
    case CMD_ERASE:
    case CMD_VERIFY:      handle_retired(DONGLE_IAP_STATUS_UNSUPPORTED); break;
    case CMD_END:         handle_retired(DONGLE_IAP_STATUS_NO_COMMIT); break;
    case CMD_ENTER_BOOT:  handle_enter_bootloader(data, len); break;
    case CMD_BOND_WRITE:  handle_bond_write(data, len); break;
    case CMD_BOND_READ:   handle_bond_read(); break;
    case CMD_BOND_CLEAR:  handle_bond_clear(); break;
    case CMD_VERSION:     handle_version(); break;
    case CMD_STATUS:      handle_status(); break;
    case CMD_FAULT:       handle_fault(); break;
    default:              break;
    }
}

/* Reboot-into-OpenBoot state machine, stepped once per main-loop pass.
 *
 * IDLE until 0x85 latches iap_reboot_pending, then:
 *   RF_QUIESCE — request the RF task to stop new work and shut the radio
 *                (RF_QuiesceRequest, once), and wait until RF_Quiesced() or
 *                the deadline passes (a wedged executor must not block the
 *                reboot forever);
 *   USB_DRAIN  — wait until the final EP6 IN status reply has been taken by
 *                the host (USB_EP6InIdle) or the deadline passes (a
 *                detached/suspended host never takes it);
 * then disable interrupts and enter the bootloader via
 * openboot_request_update() (writes OB_BOOTREQ_MAGIC to the reserved
 * top-of-RAM word and software-resets; noreturn).
 *
 * Deadlines are WALL time via hal_now() (Tsys ticks), not pass counts: the
 * main loops run hundreds of passes per millisecond, so a pass bound could
 * expire before the host's next 1 ms EP6 IN token ever arrived (codex
 * checkpoint-2 finding). u32 tick arithmetic is modular, so the deltas
 * survive hal_now() wrap. */
#define IAP_SVC_RF_QUIESCE_TICKS (500000u * HAL_TICKS_PER_US) /* 500 ms */
#define IAP_SVC_USB_DRAIN_TICKS  (250000u * HAL_TICKS_PER_US) /* 250 ms */

#define IAP_SVC_IDLE        0u
#define IAP_SVC_RF_QUIESCE  1u
#define IAP_SVC_USB_DRAIN   2u

void IAP_Service(void)
{
    static uint8_t svc_state = IAP_SVC_IDLE;
    static uint32_t svc_start;

    switch (svc_state) {
    case IAP_SVC_IDLE:
        if (!iap_reboot_pending) {
            return;
        }
#if DONGLE_HAS_RF
        RF_QuiesceRequest();
#endif
        svc_start = hal_now();
        svc_state = IAP_SVC_RF_QUIESCE;
        return;

    case IAP_SVC_RF_QUIESCE:
#if DONGLE_HAS_RF
        if (!RF_Quiesced()
                && (uint32_t)(hal_now() - svc_start) < IAP_SVC_RF_QUIESCE_TICKS) {
            return;
        }
#endif
        svc_start = hal_now();
        svc_state = IAP_SVC_USB_DRAIN;
        return;

    default: /* IAP_SVC_USB_DRAIN */
        if (!USB_EP6InIdle()
                && (uint32_t)(hal_now() - svc_start) < IAP_SVC_USB_DRAIN_TICKS) {
            return;
        }
        (void)__risc_v_disable_irq();
        openboot_request_update();      /* noreturn */
    }
}
