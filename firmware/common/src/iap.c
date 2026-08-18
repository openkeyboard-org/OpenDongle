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
#include "stack_watermark.h"    /* measurement builds only (0x96) */
#include "hal_timing.h"         /* hal_now (IAP_Service deadlines) */
#include "dongle_image_id.h"
#include "openboot_app.h"       /* openboot_request_update (noreturn) */
#include "usb_device.h"
#if DONGLE_HAS_RF
#include "rf_task.h"            /* RF_TombstoneBond (N06), RF_Quiesce* (0x85) */
#if DONGLE_RF_CRYPT
#include "rf_crypt.h"           /* RF_CRYPT_DIAG_PREV_SESSION + its counters */
#endif
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
#define CMD_CRYPT_DIAG  0x94    /* read-only link-encryption counters */
#define ACK_CRYPT_DIAG  0x94
#define CMD_CRYPT_LAST_FAIL 0x95 /* read-only latched first-DROP_MAC fingerprint */
#define CMD_STACK_WATERMARK 0x96 /* read-only; measurement builds only
                                  * (DONGLE_STACK_WATERMARK) */

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

/* Bus-reset cancellation (USB_SetBusResetCallback; 2026-08-16 review,
 * finding 13): a reset tears down the host session, so an armed mutation
 * window, the error tally, and a NOT-yet-started EnterBootloader must not
 * survive into the next one -- with live BondWrite activation, a stale
 * deferred command now rewires running crypto state, not just flash. A
 * reboot IAP_Service has already begun stays begun, deliberately: its RF
 * quiesce is one-way, so completing the reboot is the only sane exit (the
 * service state machine latched past the flag it consumed). ISR context:
 * byte writes only. */
void IAP_Reset(void)
{
    iap_armed = 0;
    iap_err_count = 0;
    iap_reboot_pending = 0;
}

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
        send_status4(status);   /* 0x01 erase / 0x02 write NV failure */
        return;
    }

    /* Read-back verification -- the same discipline the RF task applies to
     * its own persists (rf_persist_bond_task) and BondClear applies to the
     * erase. bond_save returning 0 only says the flash driver reported
     * success; the record the NEXT BOOT will load is what must match. 0xB4 =
     * written-but-verify-failed: nothing is applied to the running task, so
     * RAM keeps the previous coherent state. */
    {
        bond_record_t back __attribute__((aligned(4)));
        if (!bond_load(&back) || !bond_tuple_equal(&back, &rec)) {
            iap_err_count++;
            send_status4(0xB4);
            return;
        }
    }

#if DONGLE_HAS_RF
    /* Install into the RUNNING task (finding-2 P0: BondWrite used to persist
     * and report success while the live crypto state ran the old key until a
     * reset -- the freshly provisioned peer then required encryption the
     * dongle would not speak, and "reboot the dongle" was an undocumented
     * required step). 0xB5 = saved+verified but apply DEFERRED: a key
     * removal landed while the encrypted link is live; it takes effect when
     * the link drops (fail-closed until then). */
    if (RF_ApplyBondRecord(&rec)) {
        send_status4(0xB5);
        return;
    }
#endif
    send_status4(0x00);
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

#if DONGLE_RF_CRYPT
/* Read-only: verified-frame count then the per-reason drop tally, in
 * rf_crypt_status_t order (OK, SHAPE, INACTIVE, MAC, REPLAY, ENGINE). A single
 * drop total cannot tell a failed tag from a replayed counter from a malformed
 * frame, and those point at completely different bugs on the transmitting end.
 * Exposed unarmed because it reveals nothing secret -- counts only, no key or
 * session material. */
extern uint32_t rf_crypt_drop_reason[6];
extern uint32_t rf_crypt_ok_count;
#if RF_CRYPT_AES_DOUBLE
extern uint32_t rf_crypt_aes_redo;        /* rf_crypt.c: double-compute catches */
extern uint32_t rf_crypt_announce_retry;  /* rf_task.c: announce-seal rebuilds  */
#endif
#if RF_CRYPT_BOOT_KAT
extern uint8_t  rf_crypt_boot_kat_run;
extern uint8_t  rf_crypt_boot_kat_fail;
#endif
#if RF_CRYPT_DIAG_PREV_SESSION
extern uint32_t rf_crypt_conn_rx;
extern uint32_t rf_crypt_enc_shape;
extern uint32_t rf_crypt_fifo_full;
extern uint32_t rf_crypt_flush_drop;
extern uint32_t rf_crypt_plain_drop;
extern uint8_t  rf_crypt_len_max;
extern uint8_t  rf_crypt_len_max_tag;
#endif

static void handle_crypt_diag(void)
{
#if RF_CRYPT_DIAG_PREV_SESSION
    /* Bench layout, v4: ok(4) + reason[6](24) + pre-verify sink counters(20) +
     * len_max(1) + len_max_tag(1) + prev-session diagnostic(12) = 62 B, which
     * with the 2 B header is exactly the 64 B EP6 report -- no room left, so
     * anything further needs a selector rather than another append. Purely
     * additive: a v1/v2 reader that stops early still parses. */
    uint8_t resp[2u + 4u + 24u + 20u + 2u + 12u] = {0};
    uint8_t i;

    resp[0] = ACK_CRYPT_DIAG;
    resp[1] = 4u + 24u + 20u + 2u + 12u;
    put_le32(&resp[2], rf_crypt_ok_count);
    for (i = 0; i < 6u; i++) {
        put_le32(&resp[6 + 4u * i], rf_crypt_drop_reason[i]);
    }
    put_le32(&resp[30], rf_crypt_conn_rx);
    put_le32(&resp[34], rf_crypt_enc_shape);
    put_le32(&resp[38], rf_crypt_fifo_full);
    put_le32(&resp[42], rf_crypt_flush_drop);
    put_le32(&resp[46], rf_crypt_plain_drop);
    resp[50] = rf_crypt_len_max;
    resp[51] = rf_crypt_len_max_tag;
    put_le32(&resp[52], rf_crypt_session_mint_count);
    /* v4: offset 56 now carries the same-session re-verify count. mac_prev_ok
     * answered its question (0/34) and still counts internally; reporting it
     * here would let a prev-session rescue masquerade as a same-session one,
     * inverting the experiment's conclusion. (The re-verify experiment is
     * written up in OpenController firmware/docs/TODO.md section 4.) */
    put_le32(&resp[56], rf_crypt_mac_same_ok);
    put_le32(&resp[60], rf_crypt_last_mac_ctr);
    USB_SendEP6(resp, sizeof(resp));
#else
    /* Product layout: the health signal -- verified frames, the per-reason
     * drops, and the stale-abort hardening's own telemetry: ok(4) reason[6](24)
     * aes_redo(4) announce_retry(4) boot_kat_run(1) boot_kat_fail(1) = 38 B
     * payload. The hardening fields read zero on a chip without the hardware
     * engine (CH570); the pre-verify sink forensics are bench-profile
     * scaffolding (rf_task.c). The two layouts are told apart by their length
     * and by the status profile byte. Additive appends only. */
    uint8_t resp[2u + 4u + 24u + 4u + 4u + 1u + 1u] = {0};
    uint8_t i;

    resp[0] = ACK_CRYPT_DIAG;
    resp[1] = 4u + 24u + 4u + 4u + 1u + 1u;
    put_le32(&resp[2], rf_crypt_ok_count);
    for (i = 0; i < 6u; i++) {
        put_le32(&resp[6 + 4u * i], rf_crypt_drop_reason[i]);
    }
#if RF_CRYPT_AES_DOUBLE
    put_le32(&resp[30], rf_crypt_aes_redo);
    put_le32(&resp[34], rf_crypt_announce_retry);
#endif
#if RF_CRYPT_BOOT_KAT
    resp[38] = rf_crypt_boot_kat_run;
    resp[39] = rf_crypt_boot_kat_fail;
#endif
    USB_SendEP6(resp, sizeof(resp));
#endif
}

#if RF_CRYPT_DIAG_PREV_SESSION
/* CMD 0x95: the latched first-failure fingerprint plus the secondary
 * diagnostic counters that no longer fit in the full 0x94 report. Read-only,
 * served unarmed (counts and already-public frame bytes; the tag proves
 * nothing without the key, which never leaves the device). */
static void handle_crypt_last_fail(void)
{
    /* [ack][len] latched(1) fail_len(1) session(4) counter(4) expect1(8)
     * expect2(8) frame(22) same_differs(4) bb_during_aes(4) kat_run(1)
     * kat_fail(1) = 57-byte payload. */
    uint8_t resp[2u + 1u + 1u + 4u + 4u + 8u + 8u + 22u + 4u + 4u + 1u + 1u] = {0};
    uint8_t i;

    resp[0] = 0x95u;
    resp[1] = (uint8_t)(sizeof(resp) - 2u);
    resp[2] = rf_crypt_fail_latched;
    resp[3] = rf_crypt_fail_len;
    put_le32(&resp[4], rf_crypt_fail_session);
    put_le32(&resp[8], rf_crypt_fail_counter);
    for (i = 0; i < 8u; i++) {
        resp[12 + i] = rf_crypt_fail_expect1[i];
        resp[20 + i] = rf_crypt_fail_expect2[i];
    }
    for (i = 0; i < 22u; i++) {
        resp[28 + i] = rf_crypt_fail_frame[i];
    }
    put_le32(&resp[50], rf_crypt_same_differs);
    put_le32(&resp[54], rf_crypt_bb_during_aes);
    resp[58] = rf_crypt_kat_run;
    resp[59] = rf_crypt_kat_fail;
    USB_SendEP6(resp, sizeof(resp));
}
#endif
#endif

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

#if DONGLE_STACK_WATERMARK
/* CMD 0x96: the stack low-water mark since boot (stack_watermark.h). Payload:
 * low_water(4) end(4) eusrstack(4), LE -- the host computes depth
 * (eusrstack - low_water) and slack (low_water - end). Read-only, unarmed
 * (addresses only, and never in a product build). */
static void handle_stack_watermark(void)
{
    uint8_t resp[2u + 12u] = {0};

    resp[0] = CMD_STACK_WATERMARK;
    resp[1] = 12u;
    put_le32(&resp[2], stack_watermark_low());
    put_le32(&resp[6], stack_watermark_floor());   /* true stack floor per chip */
    put_le32(&resp[10], (uint32_t)(uintptr_t)&_eusrstack);
    USB_SendEP6(resp, sizeof(resp));
}
#endif

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
#if DONGLE_RF_CRYPT
    case CMD_CRYPT_DIAG:  handle_crypt_diag(); break;
#if RF_CRYPT_DIAG_PREV_SESSION
    case CMD_CRYPT_LAST_FAIL: handle_crypt_last_fail(); break;
#endif
#endif
#if DONGLE_STACK_WATERMARK
    case CMD_STACK_WATERMARK: handle_stack_watermark(); break;
#endif
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
