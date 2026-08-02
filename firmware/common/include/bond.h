/*
 * Bridge75 Open-Source Dongle Firmware
 * 2.4 GHz bond record - persistent pairing identity.
 *
 * The connected link needs five per-bond values to reconnect to a paired
 * keyboard: the session access address, the dongle MAC advertised in the
 * LEN-15 supervision re-key, the keyboard (peer) MAC matched on RX, the
 * connection interval, and the supervision timeout. RF_TaskInit() loads them
 * at boot, falling back to the compiled defaults when no valid record is
 * present. Storage goes through the dongle_nv_* platform seam; reads/writes
 * pass whole records, which the CH59x EEPROM_* API requires to be 4-byte
 * aligned in RAM (bond_record_t leads with a u32 so any instance is).
 */
#ifndef BOND_H
#define BOND_H

#include <stdint.h>

#include "rf_protocol.h"   /* MAC primitives + protocol defaults (N08) */

/* Logical bond-storage offset. CH59x maps this to DataFlash (physical
 * 0x75000 -- a page the stock firmware never touches: it uses 0x4000 AA /
 * 0x6000 BLE-bond / 0x7000 swap trigger, and our IAP End handler keeps
 * 0x7000); CH570 maps it to a reserved code-flash page because that part
 * has no CH59x-style DataFlash window. */
#define BOND_EEPROM_OFF   0x5000u

#define BOND_MAGIC        0x444E4F42u   /* 'B','O','N','D' little-endian */
#define BOND_VERSION      1u

/* 32-byte record. checksum is the final field and covers bytes 0..27. */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t conn_interval;
    uint32_t session_aa;
    uint16_t conn_timeout;
    uint16_t reserved0;
    uint8_t  dongle_mac[6];
    uint8_t  peer_mac[6];
    uint32_t checksum;
} bond_record_t;

/* The reconnect-critical persistence tuple, INCLUDING the advertised dongle
 * identity (CODEREVIEW N09: auto-persist used to compare/copy only
 * AA/interval/timeout/peer, silently reverting a configured dongle_mac to
 * zero on the next fresh-pair persist). Used by both firmware persist gates
 * (pre-write skip + readback verify) and host-testable (stdint-only). */
static inline int bond_tuple_equal(const bond_record_t *a,
                                   const bond_record_t *b)
{
    int i;

    if (a->session_aa != b->session_aa
        || a->conn_interval != b->conn_interval
        || a->conn_timeout != b->conn_timeout) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        if (a->peer_mac[i] != b->peer_mac[i]
            || a->dongle_mac[i] != b->dongle_mac[i]) {
            return 0;
        }
    }
    return 1;
}

/* CODEREVIEW N08 (+GB5, V2 record-side): ONE semantic validator for the
 * bond/session tuple, used at boot load (bond.c), at IAP BondWrite (iap.c,
 * reject status 0xB2), and — identity leg — at RF bond application
 * (rf_task.c). Bounds match the CURRENT shipped provisioning tool
 * (tools/provision_bond.py: interval 1..300, timeout 100..65535,
 * timeout > 3*interval — both fields in 1/32000 s protocol ticks; those tool
 * bounds date from 03af42a 2026-07-04 — an OLDER tool or raw probe write
 * could have stored an out-of-family record, which this validator now
 * boot-rejects into the RECOVERABLE fresh-pair state: re-pair or rewrite,
 * no brick), normalized through the zero = keep-compiled-default
 * convention BEFORE checking, so {interval=0, timeout=0} provisioning records
 * stay legal and a nonsense pair like {interval=300, timeout=100} is caught
 * relationally. AA: nonzero (structural, kept), not all-FF, and never the
 * fixed pair-broadcast AA. dongle_mac: all-zero = chip-derived (legal) or a
 * non-broadcast MAC. peer_mac: all-zero = learn-from-RX (legal, documented
 * provisioning mode) or a valid MAC that is not the dongle's own effective
 * identity (the record's override when present, else own_mac_or_null —
 * callers without an identity, e.g. bond.c/RF-off builds, pass NULL and the
 * own-identity leg is skipped; every firmware-generated tuple passes by
 * construction: generated AAs are 0x6A____E6/0xAC____CE-family or entropy
 * values checked nonzero, stock air tuples are 28/600). */
static inline int bond_record_semantic_valid(const bond_record_t *rec,
                                             const uint8_t *own_mac_or_null)
{
    uint16_t ivl = rec->conn_interval ? rec->conn_interval
                                      : (uint16_t)RF_PROTO_DEFAULT_INTERVAL;
    uint16_t to  = rec->conn_timeout  ? rec->conn_timeout
                                      : (uint16_t)RF_PROTO_DEFAULT_TIMEOUT;

    if (rec->session_aa == 0u || rec->session_aa == 0xFFFFFFFFu
        || rec->session_aa == RF_PROTO_PAIR_ACCESS_ADDR) {
        return 0;
    }
    if (ivl < 1u || ivl > 300u) {
        return 0;
    }
    if (to < 100u || to <= (uint32_t)ivl * 3u) {
        return 0;
    }
    if (rf_proto_mac_all_ff(rec->dongle_mac)) {
        return 0;
    }
    if (rf_proto_mac_nonzero(rec->peer_mac)) {
        const uint8_t *own = rf_proto_mac_nonzero(rec->dongle_mac)
                                 ? rec->dongle_mac : own_mac_or_null;
        if (rf_proto_mac_all_ff(rec->peer_mac)) {
            return 0;
        }
        if (own && rf_proto_mac_equal(rec->peer_mac, own)) {
            return 0;
        }
    }
    return 1;
}

uint32_t bond_checksum(const bond_record_t *rec);
int bond_load(bond_record_t *out);
int bond_save(bond_record_t *in);

/* Erase the bond page and confirm bond_load() now fails, returning the chip
 * to the un-provisioned fresh-pair state on the next boot. Does NOT touch the
 * running rf_task's in-RAM bond — reset the dongle after clearing. Returns 0
 * on success, 1 on erase failure, 2 if a valid record still reads back. */
int bond_clear(void);

#endif /* BOND_H */
