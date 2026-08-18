"""The v2 bond record, its NV plumbing, and the v1 migration, on the host.

Compiles the real `common/src/bond.c` against a mock `dongle_nv_*` seam, so the
record layout, checksum coverage, key/flag canonical form, v1->v2 read migration,
and the 48-byte `bond_clear`/`bond_save` verification all run without a chip.

The migration and the NV cap are the load-bearing parts. Growing the record from
32 to 48 bytes silently breaks `bond_clear` on real hardware if the platform
`dongle_nv_is_erased` scratch buffer is not grown with it (it returns "not
erased" for any length over the buffer, and `bond_clear` treats that as a failed
clear). `clear_cap` reproduces exactly that at the seam boundary.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOND_C = ROOT / "common" / "src" / "bond.c"
INC = ROOT / "common" / "include"

# Minimal dongle_target.h so dongle_platform.h (pulled in by bond.c) is satisfied
# on the host. Placed first on the include path.
TARGET_STUB = r"""
#ifndef DONGLE_TARGET_H
#define DONGLE_TARGET_H
#define DONGLE_FW_VERSION      "host-test"
#define DONGLE_CHIP_FAMILY_ID  0u
#endif
"""

HARNESS = r"""
#include "bond.h"
#include "dongle_platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- mock NV seam: one 256-byte page, 0xFF = erased ---- */
#define NV_PAGE 256u
static unsigned char g_nv[NV_PAGE];
static uint32_t g_is_erased_cap = BOND_RECORD_MAX_NV;   /* models the platform buffer */

uint8_t dongle_nv_read(uint32_t off, void *out, uint32_t len) {
    if (off != BOND_EEPROM_OFF || len > NV_PAGE) return 1;
    memcpy(out, g_nv, len);
    return 0;
}
uint8_t dongle_nv_erase(uint32_t off, uint32_t len) {
    (void)len;
    if (off != BOND_EEPROM_OFF) return 0xFF;
    memset(g_nv, 0xFF, NV_PAGE);
    return 0;
}
uint8_t dongle_nv_write(uint32_t off, const void *data, uint32_t len) {
    if (off != BOND_EEPROM_OFF || len > NV_PAGE) return 1;
    memcpy(g_nv, data, len);
    return 0;
}
uint8_t dongle_nv_is_erased(uint32_t off, uint32_t len) {
    if (off != BOND_EEPROM_OFF) return 0;
    if (len > g_is_erased_cap) return 0;          /* the regression under test */
    for (uint32_t i = 0; i < len; i++) if (g_nv[i] != 0xFF) return 0;
    return 1;
}

static void fill_base(bond_record_t *r) {
    memset(r, 0, sizeof(*r));
    r->session_aa    = 0xAC1234CEu;
    r->conn_interval = 28u;
    r->conn_timeout  = 600u;
    r->peer_mac[0] = 0x10; r->peer_mac[1] = 0x20; r->peer_mac[2] = 0x30;
    r->peer_mac[3] = 0x40; r->peer_mac[4] = 0x50; r->peer_mac[5] = 0x60;
    /* dongle_mac all-zero = chip-derived (legal) */
}

/* Write a synthetic 32-byte v1 record into the mock page. */
static void put_v1(int good_checksum) {
    unsigned char b[32];
    memset(b, 0, sizeof(b));
    b[0]='B'; b[1]='O'; b[2]='N'; b[3]='D';
    b[4] = 1;                          /* version 1 */
    b[5] = 0;                          /* flags */
    b[6] = 28; b[7] = 0;               /* interval LE */
    b[8]=0xCE; b[9]=0x34; b[10]=0x12; b[11]=0xAC;   /* session_aa LE */
    b[12] = 0x58; b[13] = 0x02;        /* timeout 600 LE */
    /* reserved0 (14..15) zero, dongle_mac (16..21) zero */
    b[22]=0x10; b[23]=0x20; b[24]=0x30; b[25]=0x40; b[26]=0x50; b[27]=0x60;
    uint32_t sum = 0; for (int i = 0; i < 28; i++) sum += b[i];
    if (!good_checksum) sum ^= 0xFFu;
    b[28]=(unsigned char)sum; b[29]=(unsigned char)(sum>>8);
    b[30]=(unsigned char)(sum>>16); b[31]=(unsigned char)(sum>>24);
    memset(g_nv, 0xFF, NV_PAGE);
    memcpy(g_nv, b, sizeof(b));
}

static int fail(const char *why) { printf("FAIL:%s\n", why); return 1; }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    memset(g_nv, 0xFF, NV_PAGE);

    if (!strcmp(mode, "roundtrip_plain")) {
        bond_record_t w, r;
        fill_base(&w);
        if (bond_save(&w) != 0) return fail("save");
        if (!bond_load(&r)) return fail("load");
        if (r.version != BOND_VERSION) return fail("version");
        if (r.session_aa != 0xAC1234CEu) return fail("aa");
        if (r.flags != 0) return fail("flags");
        if (!bond_key_is_zero(r.link_key)) return fail("key");
        if (bond_enc_active(&r)) return fail("active");
        printf("OK\n"); return 0;
    }
    if (!strcmp(mode, "roundtrip_enc")) {
        bond_record_t w, r;
        fill_base(&w);
        w.flags = BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY;
        for (int i = 0; i < 16; i++) w.link_key[i] = (unsigned char)(i + 1);
        if (bond_save(&w) != 0) return fail("save");
        if (!bond_load(&r)) return fail("load");
        if (!bond_enc_active(&r)) return fail("active");
        for (int i = 0; i < 16; i++)
            if (r.link_key[i] != (unsigned char)(i + 1)) return fail("key");
        printf("OK\n"); return 0;
    }
    if (!strcmp(mode, "save_zeroes_stale_key")) {
        /* flags clear ENC_KEY but a stale key in the struct: bond_save must
         * zero it so the persisted record is canonical and reloads. */
        bond_record_t w, r;
        fill_base(&w);
        w.flags = 0;
        for (int i = 0; i < 16; i++) w.link_key[i] = 0xAB;
        if (bond_save(&w) != 0) return fail("save");
        if (!bond_load(&r)) return fail("load-rejected-stale-key");
        if (!bond_key_is_zero(r.link_key)) return fail("key-not-zeroed");
        printf("OK\n"); return 0;
    }
    if (!strcmp(mode, "migrate_ok")) {
        bond_record_t r;
        put_v1(1);
        if (!bond_load(&r)) return fail("load");
        if (r.version != BOND_VERSION) return fail("version");
        if (r.flags != 0) return fail("flags");
        if (!bond_key_is_zero(r.link_key)) return fail("key");
        if (r.session_aa != 0xAC1234CEu) return fail("aa");
        if (r.peer_mac[0] != 0x10 || r.peer_mac[5] != 0x60) return fail("peer");
        printf("OK\n"); return 0;
    }
    if (!strcmp(mode, "migrate_badsum")) {
        bond_record_t r;
        put_v1(0);
        printf(bond_load(&r) ? "ACCEPTED\n" : "REJECTED\n");
        return 0;
    }
    if (!strcmp(mode, "clear_cap")) {
        bond_record_t w;
        g_is_erased_cap = (uint32_t)strtoul(argv[2], NULL, 10);
        fill_base(&w);
        if (bond_save(&w) != 0) return fail("save");
        printf("%d\n", bond_clear());
        return 0;
    }
    if (!strcmp(mode, "keyflags")) {
        bond_record_t r; int ok = 1;
        fill_base(&r);
        r.flags = BOND_FLAG_ENC_KEY;
        for (int i = 0; i < 16; i++) r.link_key[i] = (unsigned char)(i + 1);
        ok &= bond_key_flags_valid(&r) == 1;               /* key set, real   */
        memset(r.link_key, 0, 16);
        ok &= bond_key_flags_valid(&r) == 0;               /* key set, zero    */
        memset(r.link_key, 0xFF, 16);
        ok &= bond_key_flags_valid(&r) == 0;               /* key set, all-FF  */
        r.flags = 0; memset(r.link_key, 0, 16);
        ok &= bond_key_flags_valid(&r) == 1;               /* no key, zero     */
        r.link_key[3] = 0x01;
        ok &= bond_key_flags_valid(&r) == 0;               /* no key, nonzero  */
        printf(ok ? "OK\n" : "FAIL\n");
        return 0;
    }
    if (!strcmp(mode, "tuple")) {
        bond_record_t a, b; int ok = 1;
        fill_base(&a); fill_base(&b);
        a.flags = b.flags = BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY;
        for (int i = 0; i < 16; i++) a.link_key[i] = b.link_key[i] = (unsigned char)i;
        ok &= bond_tuple_equal(&a, &b) == 1;               /* identical        */
        b.link_key[7] ^= 0x01;
        ok &= bond_tuple_equal(&a, &b) == 0;               /* key differs      */
        b.link_key[7] ^= 0x01;
        b.flags = BOND_FLAG_ENC_CAPABLE;                   /* KEY bit differs  */
        ok &= bond_tuple_equal(&a, &b) == 0;
        printf(ok ? "OK\n" : "FAIL\n");
        return 0;
    }
    return 2;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class BondRecordV2(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls._tmp = tempfile.TemporaryDirectory()
        d = Path(cls._tmp.name)
        (d / "dongle_target.h").write_text(TARGET_STUB)
        harness = d / "harness.c"
        harness.write_text(HARNESS)
        cls.binary = d / "bondkat"
        subprocess.run(
            [
                cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                f"-I{d}", f"-I{INC}", "-o", str(cls.binary),
                str(harness), str(BOND_C),
            ],
            check=True,
            capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        tmp = getattr(cls, "_tmp", None)
        if tmp is not None:
            tmp.cleanup()

    def run_mode(self, *args):
        proc = subprocess.run(
            [str(self.binary), *args], check=True, capture_output=True, text=True
        )
        return proc.stdout.strip()

    def test_plaintext_roundtrip(self):
        self.assertEqual(self.run_mode("roundtrip_plain"), "OK")

    def test_encrypted_roundtrip_preserves_key(self):
        self.assertEqual(self.run_mode("roundtrip_enc"), "OK")

    def test_save_zeroes_a_stale_key(self):
        self.assertEqual(self.run_mode("save_zeroes_stale_key"), "OK")

    def test_v1_record_migrates_keyless(self):
        self.assertEqual(self.run_mode("migrate_ok"), "OK")

    def test_v1_bad_checksum_is_rejected(self):
        self.assertEqual(self.run_mode("migrate_badsum"), "REJECTED")

    def test_clear_succeeds_at_the_record_size(self):
        # sizeof(bond_record_t) == 48; a 48-byte cap verifies the clear.
        self.assertEqual(self.run_mode("clear_cap", "48"), "0")

    def test_clear_fails_if_the_nv_cap_is_too_small(self):
        # The exact regression the platform buffer widening prevents: a 32-byte
        # cap makes bond_clear report failure (2) after a real erase.
        self.assertEqual(self.run_mode("clear_cap", "32"), "2")

    def test_key_flag_canonical_form(self):
        self.assertEqual(self.run_mode("keyflags"), "OK")

    def test_tuple_equal_covers_key_and_flags(self):
        self.assertEqual(self.run_mode("tuple"), "OK")


if __name__ == "__main__":
    unittest.main()
