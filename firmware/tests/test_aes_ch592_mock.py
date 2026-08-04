"""The CH592 hardware AES backend's failure path, which hardware cannot reach.

`hal_aes_encrypt_block()` on CH592 starts a hardware engine and polls it. If the
engine wedges, the wait expires. That branch is unreachable on a healthy part --
it needs the busy bit to stay set, and no working engine will do that on demand
-- so it can only be executed against a mocked register block.

This compiles the REAL `ch592/src/hal_aes_ch592.c` for the host with `AES_BASE`
pointed at ordinary memory. Plain memory never clears the busy bit, so the driver
always takes the timeout path -- which is exactly, and only, what needs covering
here. The success path is covered on real silicon by the hardware suite
(`tests-hardware/`), where the engine is real; simulating it here would require a
second test hook in production code to buy coverage that hardware already gives.

WHY THIS PATH MATTERS. The seam's output is a keystream block in the intended
counter-mode construction. The driver originally copied the engine's data
registers out unconditionally, so a wedged engine returned a PREVIOUS keystream
block with no error indication -- and reusing a keystream across two messages
breaks the confidentiality of both. The fix zeroes the output and reports
`HAL_AES_ENGINE_TIMEOUT`. These tests pin both halves.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "ch592" / "src" / "hal_aes_ch592.c"
INC = ROOT / "common" / "include"

KEY = "000102030405060708090a0b0c0d0e0f"
PLAINTEXT = "00112233445566778899aabbccddeeff"

# A recognisable "previous keystream block" preloaded into the mock data
# registers. If the driver returns this after a timeout, that is precisely the
# keystream-reuse bug and the test must fail.
STALE = "deadbeef" * 4

HARNESS = r"""
/*
 * Host mock of the CH592 AES register window.
 *
 * `mock_regs` stands in for the block at 0x4000C300; AES_BASE is redirected
 * onto it at compile time via -include shim.h. Because it is plain memory,
 * nothing ever clears CFG bit 0, so the driver's wait always expires -- which
 * is the state under test.
 */
#include "hal_aes.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t mock_regs[64];

#define DATA_IDX (0x18u / 4u)

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = (uint8_t)((hexval(s[2 * i]) << 4) | hexval(s[2 * i + 1]));
}

/* argv: <key-hex> <plaintext-hex> <stale-hex>; prints "<status> <out-hex>" */
int main(int argc, char **argv)
{
    uint8_t key[16], in[16], out[16], stale[16];

    if (argc < 4) return 2;
    unhex(argv[1], key, 16);
    unhex(argv[2], in, 16);
    unhex(argv[3], stale, 16);

    /* Preload the data registers with a recognisable previous result, so a
     * driver that copies them out after a timeout is caught red-handed. */
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) v |= (uint32_t)stale[4 * i + j] << (8 * j);
        mock_regs[DATA_IDX + i] = v;
    }

    /* Fill the caller's buffer with a third pattern: if the driver never wrote
     * to `out` at all, that is also a failure and must not read as zeroed. */
    memset(out, 0x5A, sizeof out);

    hal_aes_set_key(key);
    hal_aes_status_t st = hal_aes_encrypt_block(in, out);

    printf("%d ", (int)st);
    for (int i = 0; i < 16; i++) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
"""

# Redirect the driver's register window onto the mock array. Only AES_BASE is
# overridden; the driver is otherwise compiled exactly as it ships.
SHIM = r"""
extern unsigned int mock_regs[64];
#define AES_BASE ((unsigned long)(void *)mock_regs)
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class Ch592WedgedEngine(unittest.TestCase):
    """Compile the real CH592 driver against a register block that never
    completes, and pin what it hands back."""

    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls._tmp = tempfile.TemporaryDirectory()
        d = Path(cls._tmp.name)
        (d / "harness.c").write_text(HARNESS)
        (d / "shim.h").write_text(SHIM)
        cls.binary = d / "ch592mock"
        proc = subprocess.run(
            [
                cc, "-O1", "-std=gnu99", "-Wall", "-Wextra",
                f"-I{INC}", f"-I{d}",
                "-include", "shim.h",
                # The shipping bound is 100000 spins; shrink it so the test is
                # instant. The driver reads it from a macro precisely so a bound
                # this coarse never has to be hard-coded in two places.
                "-DAES_SPIN_LIMIT=1000u",
                "-o", str(cls.binary), str(d / "harness.c"), str(SRC),
            ],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            # An AssertionError, not SkipTest: if the shipping driver stops
            # compiling that is a failure, and skipping would hide it behind a
            # green run. SkipTest is reserved for "no compiler on this host",
            # which is a property of the machine rather than of the code.
            raise AssertionError(
                "CH592 driver does not compile for the host mock:\n"
                + proc.stderr[-4000:]
            )
        cls._run = subprocess.run(
            [str(cls.binary), KEY, PLAINTEXT, STALE],
            check=True, capture_output=True, text=True,
        )
        cls.status, cls.out = cls._run.stdout.split()
        cls.status = int(cls.status)

    @classmethod
    def tearDownClass(cls):
        tmp = getattr(cls, "_tmp", None)
        if tmp is not None:
            tmp.cleanup()

    def test_reports_timeout(self):
        """HAL_AES_ENGINE_TIMEOUT is 1; HAL_AES_OK is 0."""
        self.assertEqual(
            self.status, 1,
            "a wedged engine must report HAL_AES_ENGINE_TIMEOUT, not success",
        )

    def test_does_not_return_the_stale_keystream(self):
        """The bug this file exists for.

        A previous keystream block sitting in the data registers must never
        reach the caller: in counter mode that is keystream reuse, which breaks
        two messages at once.
        """
        self.assertNotEqual(
            self.out, STALE, "returned the engine's stale data registers"
        )

    def test_zeroes_the_output(self):
        """Stronger than 'not stale'.

        'Not stale' would also accept the plaintext, or the 0x5A fill the
        harness put there, or any other junk. The contract in hal_aes.h says
        zeroed, so that is what is asserted.
        """
        self.assertEqual(self.out, "00" * 16)

    def test_output_was_actually_written(self):
        """Guards the guard: the harness pre-fills `out` with 0x5A, so a driver
        that returned early without touching it would fail the zero check above
        for the right reason rather than passing by accident."""
        self.assertNotIn("5a", self.out)


if __name__ == "__main__":
    unittest.main()
