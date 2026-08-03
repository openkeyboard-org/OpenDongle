/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * IAP (In-Application Programming) command handler.
 *
 * Implements the minimum subset of the vendor's McuProgramIAP_* protocol
 * needed to make the dongle in-field reflashable by `flash_dongle.py`
 * (and, modulo the hard-coded VID/PID check, the stock Windows tool).
 *
 * Packet layout on EP 0x06 OUT / 0x86 IN (Interface 4):
 *
 *     [cmd][len][data...][cksum]
 *   where cksum = (cmd + len + Sum(data)) & 0xFF.
 *
 * Supported commands:
 *
 *     0x5A  Handshake        payload "WCH@HFD" (7 B) -> ack 0xA5
 *     0x84  GetDevInfo       [arg_LE32]              arms session (arg=1);
 *                            the former staging-geometry bytes report as 0
 *     0x85  EnterBootloader  4 B payload: OB_BOOTREQ_MAGIC (0xB007CA11) LE,
 *                            i.e. 11 CA 07 B0 -> status 0x00, then the main
 *                            loop's IAP_Service() reboots into OpenBoot
 *     0x87  BondWrite        32 B bond record        (extension)
 *     0x88  BondRead         no data                 (extension)
 *     0x89  BondClear        no data                 (extension)
 *     0x90  Version          no data -> FW string    (extension)
 *     0x91  Status           no data -> status v1    (extension)
 *     0x93  FaultRead        no data -> fault record (extension)
 *
 * Retired commands — firmware updates now happen inside the OpenBoot
 * bootloader over OBP, never in-app:
 *
 *     0x80  Program / 0x81 Erase / 0x82 Verify -> status 0xF8 when armed
 *     0x83  End                                -> status 0xF9 when armed
 *   (unarmed, all four are silently dropped, as before)
 *
 * 0x85 and the bond commands are gated by the session-armed flag set by
 * GetDevInfo with arg=1.
 */
#ifndef IAP_H
#define IAP_H

#include <stdint.h>

/* Hook this into the EP6-OUT receive path during USB init. */
void IAP_PacketHandler(const uint8_t *data, uint8_t len);

/* Call once per main-loop pass. Drives the EnterBootloader (0x85) reboot:
 * RF quiesce, EP6 reply drain (both with bounded waits), then IRQ-off and
 * openboot_request_update(). No-op until a reboot has been requested. */
void IAP_Service(void);

#endif /* IAP_H */
