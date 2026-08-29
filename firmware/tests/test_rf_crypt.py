"""The firmware AES-128-CCM RX path must match the reference and fail closed.

Compiles the real `common/src/rf_crypt.c` against `common/src/aes_sw.c` (through
a tiny hal_aes shim, the way CH570 wires the software backend) and drives it with
frames built by `ccm_ref`, which is itself anchored to RFC 3610. This proves the
device decrypts exactly what a conforming keyboard would send, and -- the part
that actually matters for a keyboard link -- that it drops everything else:
tampered ciphertext, tampered header, replays, stale sessions, and a wedged
engine. Nothing is ever forwarded without a verified tag.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import ccm_ref

ROOT = Path(__file__).resolve().parents[1]
RF_CRYPT_C = ROOT / "common" / "src" / "rf_crypt.c"
AES_SW_C = ROOT / "common" / "src" / "aes_sw.c"
INC = ROOT / "common" / "include"

TAG_CONSUMER, TAG_MOUSE, TAG_BOOT = 0xA3, 0xA8, 0xA1

# hal_aes shim over the software cipher, plus a line-driven harness so replay and
# session state persist across operations within one process.
HARNESS = r"""
#include "rf_crypt.h"
#include "hal_aes.h"
#include "aes_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aes_sw_ctx_t g_ctx;
static int g_fail_next = 0;

void hal_aes_init(void) {}
void hal_aes_set_key(const uint8_t key[16]) { aes_sw_expand_key(&g_ctx, key); }
hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    if (g_fail_next) { g_fail_next = 0; memset(out, 0, 16); return HAL_AES_ENGINE_TIMEOUT; }
    aes_sw_encrypt_block(&g_ctx, in, out);
    return HAL_AES_OK;
}

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
static int unhex(const char *s, uint8_t *out, int max){
    int n=0;
    while (s[0]&&s[1]&&hexval(s[0])>=0&&hexval(s[1])>=0&&n<max){
        out[n++]=(uint8_t)((hexval(s[0])<<4)|hexval(s[1])); s+=2;
    }
    return n;
}

static const char *st_name(rf_crypt_status_t s){ switch(s){
    case RF_CRYPT_OK: return "OK"; case RF_CRYPT_DROP_SHAPE: return "SHAPE";
    case RF_CRYPT_DROP_INACTIVE: return "INACTIVE"; case RF_CRYPT_DROP_MAC: return "MAC";
    case RF_CRYPT_DROP_REPLAY: return "REPLAY"; case RF_CRYPT_FAULT_ENGINE: return "ENGINE";
    default: return "?"; } }

int main(void){
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        char *arg = strtok(NULL, " \t\r\n");
        if (!strcmp(cmd, "install")) {
            uint8_t k[16]; unhex(arg, k, 16); rf_crypt_install_key(k);
        } else if (!strcmp(cmd, "session")) {
            uint32_t sid = (uint32_t)strtoul(arg, NULL, 16); rf_crypt_new_session(sid);
        } else if (!strcmp(cmd, "clear")) {
            rf_crypt_clear();
        } else if (!strcmp(cmd, "active")) {
            printf("active %d\n", rf_crypt_active());
        } else if (!strcmp(cmd, "failnext")) {
            g_fail_next = 1;
        } else if (!strcmp(cmd, "rx")) {
            uint8_t f[64]; int len = unhex(arg, f, sizeof(f));
            uint8_t tag=0, body[RF_CRYPT_MAX_BODY], n=0;
            rf_crypt_status_t s = rf_crypt_rx(f, (uint8_t)len, &tag, body, &n);
            if (s == RF_CRYPT_OK) {
                printf("rx OK %02x ", tag);
                for (int i=0;i<n;i++) printf("%02x", body[i]);
                printf("\n");
            } else printf("rx %s\n", st_name(s));
        } else if (!strcmp(cmd, "build")) {
            uint8_t ctrl = (uint8_t)strtoul(arg, NULL, 16);
            uint8_t out[RF_CRYPT_LEN_SESSION];
            rf_crypt_status_t s = rf_crypt_build_session_frame(ctrl, out);
            if (s == RF_CRYPT_OK) {
                printf("build OK ");
                for (int i=0;i<(int)RF_CRYPT_LEN_SESSION;i++) printf("%02x", out[i]);
                printf("\n");
            } else printf("build %s\n", st_name(s));
        }
        fflush(stdout);
    }
    return 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class CcmRxPath(unittest.TestCase):
    KEY = bytes(range(1, 17))
    SID = 0x11223344

    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls._tmp = tempfile.TemporaryDirectory()
        d = Path(cls._tmp.name)
        (d / "harness.c").write_text(HARNESS)
        cls.binary = d / "rfcrypt"
        subprocess.run(
            [cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             f"-I{INC}", "-o", str(cls.binary),
             str(d / "harness.c"), str(RF_CRYPT_C), str(AES_SW_C)],
            check=True, capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        tmp = getattr(cls, "_tmp", None)
        if tmp is not None:
            tmp.cleanup()

    def drive(self, script):
        proc = subprocess.run(
            [str(self.binary)], input=script, check=True,
            capture_output=True, text=True,
        )
        return proc.stdout.splitlines()

    def frame(self, counter, ctrl, tag, body, key=None, sid=None):
        return ccm_ref.build_frame(
            key or self.KEY, sid if sid is not None else self.SID,
            counter, ctrl, tag, body,
        ).hex()

    def session_prefix(self):
        return [f"install {self.KEY.hex()}", f"session {self.SID:08x}"]

    def test_good_frame_decrypts(self):
        body = bytes([0xE9, 0x00])
        script = "\n".join(self.session_prefix()
                           + [f"rx {self.frame(1, 0x5A, TAG_CONSUMER, body)}"]) + "\n"
        self.assertEqual(self.drive(script), [f"rx OK {TAG_CONSUMER:02x} {body.hex()}"])

    def test_all_three_report_shapes(self):
        cases = [(TAG_CONSUMER, bytes([0xE9, 0x00])),
                 (TAG_MOUSE, bytes([0x00, 0x05, 0x00, 0x00, 0x00])),
                 (TAG_BOOT, bytes([0x02, 0, 0x04, 0, 0, 0, 0, 0]))]
        lines, ctr = self.session_prefix(), 1
        expect = []
        for tag, body in cases:
            lines.append(f"rx {self.frame(ctr, 0x00, tag, body)}")
            expect.append(f"rx OK {tag:02x} {body.hex()}")
            ctr += 1
        self.assertEqual(self.drive("\n".join(lines) + "\n"), expect)

    def test_ciphertext_tamper_is_dropped(self):
        f = bytearray.fromhex(self.frame(1, 0x5A, TAG_MOUSE,
                                         bytes([0, 1, 2, 3, 4])))
        f[6] ^= 0x01  # flip a ciphertext byte
        script = "\n".join(self.session_prefix() + [f"rx {f.hex()}"]) + "\n"
        self.assertEqual(self.drive(script), ["rx MAC"])

    def test_header_tamper_is_dropped(self):
        f = bytearray.fromhex(self.frame(1, 0x5A, TAG_CONSUMER, b"\x01\x02"))
        f[0] ^= 0x20  # ctrl is AAD; tampering must fail the tag
        script = "\n".join(self.session_prefix() + [f"rx {f.hex()}"]) + "\n"
        self.assertEqual(self.drive(script), ["rx MAC"])

    def test_replayed_counter_is_dropped(self):
        f = self.frame(5, 0x5A, TAG_CONSUMER, b"\x01\x02")
        script = "\n".join(self.session_prefix() + [f"rx {f}", f"rx {f}"]) + "\n"
        out = self.drive(script)
        self.assertEqual(out[0], f"rx OK {TAG_CONSUMER:02x} 0102")
        self.assertEqual(out[1], "rx REPLAY")

    def test_counter_must_increase(self):
        script = "\n".join(self.session_prefix() + [
            f"rx {self.frame(10, 0, TAG_CONSUMER, b'aa')}",
            f"rx {self.frame(7,  0, TAG_CONSUMER, b'bb')}",   # older -> replay
            f"rx {self.frame(11, 0, TAG_CONSUMER, b'cc')}",   # newer -> ok
        ]) + "\n"
        out = self.drive(script)
        self.assertEqual(out[0].split()[1], "OK")
        self.assertEqual(out[1], "rx REPLAY")
        self.assertEqual(out[2].split()[1], "OK")

    def test_counter_zero_is_rejected(self):
        script = "\n".join(self.session_prefix()
                           + [f"rx {self.frame(0, 0, TAG_CONSUMER, b'zz')}"]) + "\n"
        self.assertEqual(self.drive(script), ["rx REPLAY"])

    def test_wrong_session_fails_the_tag(self):
        # frame built under SID, device switched to a different session.
        f = self.frame(1, 0x5A, TAG_CONSUMER, b"\x01\x02")
        script = "\n".join([f"install {self.KEY.hex()}", "session 99887766",
                            f"rx {f}"]) + "\n"
        self.assertEqual(self.drive(script), ["rx MAC"])

    def test_inactive_without_key_or_session(self):
        f = self.frame(1, 0, TAG_CONSUMER, b"\x01\x02")
        # key installed but no session yet
        script = f"install {self.KEY.hex()}\nrx {f}\n"
        self.assertEqual(self.drive(script), ["rx INACTIVE"])

    def test_bad_shape_is_dropped(self):
        # A valid-tag frame padded to a non-encrypted length.
        f = self.frame(1, 0, TAG_CONSUMER, b"\x01\x02") + "abcd"
        script = "\n".join(self.session_prefix() + [f"rx {f}"]) + "\n"
        self.assertEqual(self.drive(script), ["rx SHAPE"])

    def test_engine_fault_is_terminal_not_forwarded(self):
        script = "\n".join(self.session_prefix() + [
            "failnext",
            f"rx {self.frame(1, 0, TAG_CONSUMER, b'\x01\x02')}",
        ]) + "\n"
        self.assertEqual(self.drive(script), ["rx ENGINE"])

    def test_clear_deactivates(self):
        script = "\n".join(self.session_prefix()
                           + ["clear", "active",
                              f"rx {self.frame(1, 0, TAG_CONSUMER, b'xy')}"]) + "\n"
        out = self.drive(script)
        self.assertEqual(out[0], "active 0")
        self.assertEqual(out[1], "rx INACTIVE")

    def test_session_frame_matches_reference(self):
        ctrl, session_tag = 0x40, 0xA5
        nonce = ccm_ref.build_nonce(self.SID, ccm_ref.DIR_DONGLE_TO_KB, 0)
        aad = bytes([ctrl, session_tag])
        tag = ccm_ref.ccm_encrypt(self.KEY, nonce, aad, b"", ccm_ref.CCM_TAG_BYTES)
        expected = (bytes([ctrl, 0xA5]) + self.SID.to_bytes(4, "little") + tag).hex()
        script = "\n".join(self.session_prefix() + [f"build {ctrl:02x}"]) + "\n"
        self.assertEqual(self.drive(script), [f"build OK {expected}"])


if __name__ == "__main__":
    unittest.main()
