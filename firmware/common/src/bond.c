/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * 2.4 GHz bond record load/save.
 */

#include "bond.h"
#include "dongle_platform.h"

#include <stddef.h>

uint32_t bond_checksum(const bond_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t sum = 0;

    for (size_t i = 0; i < offsetof(bond_record_t, checksum); i++) {
        sum += p[i];
    }
    return sum;
}

int bond_load(bond_record_t *out)
{
    if (dongle_nv_read(BOND_EEPROM_OFF, out, sizeof(*out)) != 0) {
        return 0;
    }
    if (out->magic != BOND_MAGIC) {
        return 0;
    }
    if (out->version == 1u) {
        /* Read-side v1->v2 migration (no forced re-pair). A v1 record read into
         * a v2-sized buffer has its 28 leading fields at unchanged offsets, the
         * v1 checksum at bytes 28..31, and erased flash beyond. Accept it on the
         * v1 checksum, then normalise to a keyless v2 record in RAM so the rest
         * of this function and every caller see one shape; the page rewrites to
         * v2 on the next bond_save(). Encryption stays off until the peer is
         * re-paired as capable and a key is provisioned. */
        const uint8_t *raw = (const uint8_t *)out;
        uint32_t v1_sum = 0u;
        uint32_t v1_stored;
        size_t i;

        for (i = 0; i < 28u; i++) {
            v1_sum += raw[i];
        }
        v1_stored = (uint32_t)raw[28] | ((uint32_t)raw[29] << 8)
                  | ((uint32_t)raw[30] << 16) | ((uint32_t)raw[31] << 24);
        if (v1_sum != v1_stored) {
            return 0;
        }
        out->version = BOND_VERSION;
        out->flags = 0u;
        for (i = 0; i < sizeof(out->link_key); i++) {
            out->link_key[i] = 0u;
        }
        out->checksum = bond_checksum(out);
    }
    if (out->version != BOND_VERSION) {
        return 0;
    }
    if (bond_checksum(out) != out->checksum) {
        return 0;
    }
    if (out->session_aa == 0u) {
        return 0;
    }
    /* CODEREVIEW N08: semantic validation (bounds/relational/MAC hygiene).
     * No identity available at this layer — the own-MAC leg runs at the RF
     * bond-application site with the factory identity. A stored record that
     * fails here reads back as "no record": the dongle boots into the
     * fresh-pair state instead of camping on an unusable tuple. */
    if (!bond_record_semantic_valid(out, 0)) {
        return 0;
    }
    /* V2: the key/flag pair must be in canonical form (key present iff flagged).
     * A record that fails this reads back as "no record" too, failing closed. */
    if (!bond_key_flags_valid(out)) {
        return 0;
    }
    return 1;
}

int bond_clear(void)
{
    if (dongle_nv_erase(BOND_EEPROM_OFF, 256) != 0) {
        return 1;
    }
    /* Verify the clear took by confirming the record bytes read back fully
     * erased. Checking the erased state directly (rather than "bond_load() no
     * longer validates") means a verify READ failure is reported as a failed
     * clear -- bond_load() returns 0 for both "no valid record" and "read
     * failed", so using it here would misreport an unverifiable clear as
     * success. dongle_nv_is_erased() returns 0 (not erased) on a read error. */
    if (!dongle_nv_is_erased(BOND_EEPROM_OFF, sizeof(bond_record_t))) {
        return 2;
    }
    return 0;
}

int bond_save(bond_record_t *in)
{
    in->magic     = BOND_MAGIC;
    in->version   = BOND_VERSION;
    in->reserved0 = 0;
    /* Canonical form: no provisioned key => zero the field, so a caller that
     * cleared the flag but left stale key bytes cannot persist an inconsistent
     * record that bond_load()/BondWrite would then reject (bricking the bond). */
    if (!(in->flags & BOND_FLAG_ENC_KEY)) {
        size_t i;
        for (i = 0; i < sizeof(in->link_key); i++) {
            in->link_key[i] = 0u;
        }
    }
    in->checksum  = bond_checksum(in);

    if (!dongle_nv_is_erased(BOND_EEPROM_OFF, sizeof(*in))) {
        if (dongle_nv_erase(BOND_EEPROM_OFF, 256) != 0) {
            return 1;
        }
    }
    if (dongle_nv_write(BOND_EEPROM_OFF, in, sizeof(*in)) != 0) {
        return 2;
    }
    return 0;
}
