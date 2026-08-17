"""Run the on-device validation harness on the host, end to end.

`validation/aes_validate.c` is the program that decides whether a chip passes.
It normally only runs on silicon, which makes it the least-tested code in the
suite -- and a harness with a bug in it reports whatever it reports, on every
arm, convincingly.

It is chip-agnostic by construction: it calls `hal_aes.h` and nothing else, and
its only two hardware touchpoints are a SysTick base (overridable, like
`AES_BASE` in the CH592 driver) and a platform init hook. So the REAL harness
compiles and runs here against the portable cipher, and its log goes through the
REAL reader. That closes the loop on everything except the silicon itself:

  - the harness emits a record the reader accepts,
  - every marker the harness writes is one the reader knows,
  - the record is uniformly framed, so the marker walk stays in sync,
  - and the 512-block differential computed by on-device code yields the same
    `b106130c` the host derives independently in test_aes_sw.py.

That last point is the whole cross-arm assertion, proven before any hardware is
touched. A device arm then only has to match a number two independent host
implementations already agree on.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import aes_vectors

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "validation"))

import read_aes_log as R  # noqa: E402

VALIDATION = ROOT / "validation"
INC = ROOT / "common" / "include"
# rf_crypt.c includes dongle_target.h; -I{VALIDATION} supplies the neutral
# validation shim, so this harness compiles rf_crypt in exactly the shape the
# on-silicon validation arms build -- the shipping configuration, no bench
# diagnostics. (test_rf_crypt.py covers the bench shape.)
AES_SW = ROOT / "common" / "src" / "aes_sw.c"
RF_CRYPT = ROOT / "common" / "src" / "rf_crypt.c"

# Mirrors what read_aes_log's CLI builds; vectors 7-11 are contract properties
# that all reduce to the FIPS-197 C.1 answer.
FIPS = aes_vectors.VECTORS[0][3]
EXPECTATIONS = {i + 1: aes_vectors.VECTORS[i][3] for i in range(6)}
EXPECTATIONS.update({7: FIPS, 8: FIPS, 9: FIPS, 10: FIPS, 11: FIPS})

SHIM = r"""
/*
 * Host stand-ins for the two things aes_validate.c needs from a chip: a
 * hal_aes backend and a platform bring-up hook. The backend is the portable
 * cipher, wired exactly as ch570/src/hal_aes_ch570.c wires it for the C
 * variant, so this exercises a real backend rather than a fake one.
 */
#include "hal_aes.h"
#include "aes_sw.h"
#include "aes_log_format.h"

#include <stdint.h>
#include <stdio.h>

static aes_sw_ctx_t ctx;

void hal_aes_init(void) {}

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    aes_sw_expand_key(&ctx, key);
}

hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES])
{
    aes_sw_encrypt_block(&ctx, in, out);
    return HAL_AES_OK;
}

void aes_validate_platform_init(void) {}

/* Stands in for the SysTick register block. Reads return zero, so the timing
 * figures come out as zero -- which is correct here: timings are recorded as
 * regression signals, never as pass/fail, so a host run has nothing to say
 * about them and says nothing. */
uint32_t mock_systick[8];

extern volatile uint32_t aes_log[AES_LOG_WORDS];
int aes_validate_main(void);

int main(void)
{
    int n = aes_validate_main();
    for (int i = 0; i < n; i++)
        printf("%08x\n", aes_log[i]);
    return 0;
}
"""


def _find_cc():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


class HarnessRunsOnTheHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = _find_cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls._tmp = tempfile.TemporaryDirectory()
        d = Path(cls._tmp.name)
        (d / "shim.c").write_text(SHIM)
        # Redirect the harness's SysTick window onto ordinary memory. Same
        # approach as tests/test_aes_ch592_mock.py: an -include header, so the
        # harness itself is compiled exactly as it ships.
        (d / "stk_shim.h").write_text(
            "extern unsigned int mock_systick[8];\n"
            "#define STK_BASE ((unsigned long)(void *)mock_systick)\n"
        )

        common = [cc, "-O1", "-std=gnu99", "-Wall", "-Wextra",
                  "-Wno-unused-parameter", f"-I{INC}", f"-I{VALIDATION}"]

        def compile_one(src, extra=()):
            obj = d / (Path(src).stem + ".o")
            proc = subprocess.run(
                [*common, *extra, "-c", "-o", str(obj), str(src)],
                capture_output=True, text=True)
            if proc.returncode != 0:
                raise AssertionError(
                    f"{Path(src).name} does not compile for the host:\n"
                    + proc.stderr[-4000:])
            return obj

        objs = [
            # Only aes_validate.c gets the entry-point rename and the SysTick
            # redirect; applying -Dmain to the shim too would rename ITS main
            # and turn the call below into infinite recursion.
            compile_one(VALIDATION / "aes_validate.c", (
                "-DDONGLE_VALIDATE_HOSTED=1",
                "-Dmain=aes_validate_main",
                "-include", str(d / "stk_shim.h"),
                "-DDONGLE_VALIDATE_CLOCK_HZ=100000000u",
                f"-DDONGLE_VALIDATE_BACKEND={R.M['AES_LOG_BACKEND_C']}u",
                "-DDONGLE_VALIDATE_BUILD_ID=0xABCD1234u",
                # CSR 0xBC0 is CORECFGR only on V3C; no such CSR here.
                "-DDONGLE_VALIDATE_HAS_CORECFGR=0",
            )),
            compile_one(d / "shim.c"),
            compile_one(AES_SW),
            # aes_validate.c now also exercises the CCM mode (rf_crypt) on each
            # arm, so the host harness links it too, over the same shim backend.
            compile_one(RF_CRYPT),
        ]

        binary = d / "validate"
        proc = subprocess.run(
            [cc, "-o", str(binary), *[str(o) for o in objs]],
            capture_output=True, text=True)
        if proc.returncode != 0:
            raise AssertionError(
                "the validation harness does not link:\n" + proc.stderr[-4000:])
        run = subprocess.run([str(binary)], check=True, capture_output=True,
                             text=True)
        cls.words = R.words_from_text(run.stdout)
        cls.record = R.parse(cls.words)

    @classmethod
    def tearDownClass(cls):
        tmp = getattr(cls, "_tmp", None)
        if tmp is not None:
            tmp.cleanup()

    def test_the_reader_accepts_what_the_harness_writes(self):
        """The two sides of the wire format agree.

        Both derive their constants from aes_log_format.h, so this is really a
        check that the framing is consistent: a record the harness writes in one
        width and the reader consumes in another desyncs here.
        """
        self.assertTrue(self.record.complete)
        self.assertFalse(self.record.overflow)
        self.assertEqual(self.record.backend_name, "C")
        self.assertEqual(self.record.build_id, 0xABCD1234)

    def test_every_vector_ran_and_passed(self):
        self.assertEqual(sorted(self.record.vectors), sorted(EXPECTATIONS))
        for vec_id, vec in sorted(self.record.vectors.items()):
            with self.subTest(vector=vec.name):
                self.assertTrue(vec.status_ok)
                self.assertTrue(vec.bytes_ok)
                self.assertEqual(vec.ciphertext.hex(), EXPECTATIONS[vec_id])

    def test_the_device_differential_matches_the_host_derivation(self):
        """The cross-arm gate, proven without hardware.

        `b106130c` is computed here by the on-device harness code, and in
        test_aes_sw.py by an independent host loop over the same cipher. A
        hardware arm then has to match a number two implementations agree on.
        """
        d = self.record.differential
        self.assertTrue(d.entered and d.exited, "differential did not complete")
        self.assertEqual(d.count, aes_vectors.DIFFERENTIAL_COUNT)
        self.assertEqual(d.fold, aes_vectors.EXPECTED_FOLD)

    def test_checkpoints_localise_a_divergence(self):
        d = self.record.differential
        self.assertEqual(
            len(d.checkpoints),
            aes_vectors.DIFFERENTIAL_COUNT // aes_vectors.CHECKPOINT_EVERY,
        )

    def test_the_run_verifies_clean(self):
        failures = R.verify(self.record, EXPECTATIONS, aes_vectors.EXPECTED_FOLD)
        self.assertEqual(failures, [], "\n".join(failures))

    def test_the_log_fits_with_room_to_spare(self):
        """If a new section ever pushes this over AES_LOG_WORDS the harness
        sets its overflow flag, but catching it here is cheaper than catching
        it on a device."""
        self.assertLess(len(self.words), R.M["AES_LOG_WORDS"])

    def test_corecfgr_is_absent_when_the_core_has_no_such_csr(self):
        """CSR 0xBC0 is CORECFGR on V3C only.

        On the V4C core in CH592 that address is a different register, so
        reading it would log a plausible-looking number that means something
        else. The section is compiled out rather than read and ignored.
        """
        self.assertIsNone(self.record.corecfgr)


if __name__ == "__main__":
    unittest.main()
