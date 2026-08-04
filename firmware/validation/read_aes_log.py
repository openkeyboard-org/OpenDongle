#!/usr/bin/env python3
"""Decode the AES validation log the on-device harness leaves in RAM.

Copyright 2026 Eric Molitor (EMulator)
SPDX-License-Identifier: Apache-2.0

Every result from this campaign so far was decoded BY HAND out of a hex dump.
That is slow, and worse, it is the step where a tired reader confirms the number
they expected to see. This module is the machine-checked replacement.

Two design rules, both learned the hard way:

  - MARKER CONSTANTS ARE PARSED FROM `aes_log_format.h`, never redeclared here.
    A reader carrying its own copy of the wire format is a reader that will one
    day disagree with the firmware and be believed anyway.

  - THE DEVICE'S OWN PASS FLAG IS NOT TRUSTED. The harness compares ciphertext
    on-device and logs a boolean, but it also logs the ciphertext itself, and
    `verify()` re-checks the bytes against expectations supplied by the caller.
    A backend whose comparison is broken reports success in that boolean; only
    an independent check catches it. This is what the plan means by comparing
    byte-for-byte rather than by checksum.

A desync is reported, never worked around. If an unexpected word turns up where
a marker belongs, parsing stops with the offset -- resynchronising by scanning
forward would eventually land on payload data that happens to look like a
marker and produce a confident, wrong report.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

HEADER = Path(__file__).resolve().parent / "aes_log_format.h"


def _load_markers(path=HEADER):
    """Parse the plain scalar `#define`s out of the shared format header."""
    text = path.read_text()
    found = {}
    for m in re.finditer(
        r"^\s*#define\s+(AES_LOG_\w+)\s+(0[xX][0-9a-fA-F]+|\d+)\s*(?:/\*|//|$)",
        text,
        re.MULTILINE,
    ):
        found[m.group(1)] = int(m.group(2), 0)
    missing = [
        n for n in (
            "AES_LOG_MAGIC", "AES_LOG_VERSION", "AES_LOG_WORDS",
            "AES_LOG_HEADER_WORDS", "AES_LOG_START", "AES_LOG_VECTOR_BASE",
            "AES_LOG_VECTOR_MASK", "AES_LOG_VECTOR_WORDS", "AES_LOG_KAT_END",
            "AES_LOG_DIFF_ENTER", "AES_LOG_DIFF_EXIT", "AES_LOG_TIMING_BASE",
            "AES_LOG_TIMING_MASK", "AES_LOG_TIMING_WORDS", "AES_LOG_CORECFGR",
            "AES_LOG_END", "AES_LOG_OVERFLOW", "AES_LOG_RESULT_BYTES_OK",
            "AES_LOG_FAULT_WORDS", "AES_LOG_FAULT_MAGIC", "AES_LOG_FAULT_MASK",
            "AES_LOG_KEEP_MAGIC", "AES_LOG_SAVED_MAGIC",
            "AES_LOG_RESULT_STATUS_OK", "AES_LOG_DIFF_COUNT",
            "AES_LOG_DIFF_CHECKPOINT",
        )
        if n not in found
    ]
    if missing:
        raise RuntimeError(
            f"{path.name} is missing scalar definitions for: {', '.join(missing)}. "
            "They must stay plain integer #defines -- this reader parses them."
        )
    # FAULT_SLOT is deliberately an expression in the header (WORDS - FAULT_WORDS)
    # and so cannot be parsed; derive it here from the two scalars it is made of,
    # rather than relaxing the parser to evaluate arithmetic it should not trust.
    found["AES_LOG_FAULT_SLOT"] = (found["AES_LOG_WORDS"]
                                   - found["AES_LOG_FAULT_WORDS"])
    return found


M = _load_markers()

BACKEND_NAMES = {
    M.get("AES_LOG_BACKEND_UNKNOWN", 0): "unknown",
    M.get("AES_LOG_BACKEND_ASM_A", 1): "ASM_A",
    M.get("AES_LOG_BACKEND_ASM_F", 2): "ASM_F",
    M.get("AES_LOG_BACKEND_C", 3): "C",
    M.get("AES_LOG_BACKEND_HW", 4): "hardware",
}

VECTOR_NAMES = {
    1: "FIPS-197 C.1",
    2: "all-zero key and plaintext",
    3: "SP800-38A F.1.1 block 1",
    4: "SP800-38A F.1.1 block 2",
    5: "SP800-38A F.1.1 block 3",
    6: "SP800-38A F.1.1 block 4",
    7: "in-place (out == in)",
    8: "partial overlap (out == in + 1)",
    9: "repeated set_key is idempotent",
    10: "key change K1->K2->K1",
    11: "same block twice (no chaining state)",
}

TIMING_NAMES = {0: "measurement overhead", 1: "key expansion", 2: "block"}


class LogError(Exception):
    """The record cannot be parsed. Never raised for a merely failing test."""


@dataclass
class Vector:
    vec_id: int
    ciphertext: bytes
    bytes_ok: bool
    status_ok: bool

    @property
    def name(self):
        return VECTOR_NAMES.get(self.vec_id, f"vector {self.vec_id}")


@dataclass
class Differential:
    entered: bool = False
    exited: bool = False
    checkpoints: list = field(default_factory=list)
    count: int | None = None
    fold: str | None = None
    last_ciphertext: bytes | None = None

    @property
    def hung(self):
        """Entered but never exited: the harness died inside the loop."""
        return self.entered and not self.exited


@dataclass
class Record:
    version: int = 0
    clock_hz: int = 0
    backend: int = 0
    build_id: int = 0
    vectors: dict = field(default_factory=dict)
    kat_all: int | None = None
    differential: Differential = field(default_factory=Differential)
    timings: dict = field(default_factory=dict)
    corecfgr: int | None = None
    fault: dict | None = None
    complete: bool = False
    overflow: bool = False

    @property
    def backend_name(self):
        return BACKEND_NAMES.get(self.backend, f"backend {self.backend}")


def words_from_bytes(data: bytes):
    """Little-endian words. Trailing partial word is an error, not a truncation."""
    if len(data) % 4:
        raise LogError(f"{len(data)} bytes is not a whole number of 32-bit words")
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def words_from_text(text: str):
    """Accept whitespace/comma separated hex, with or without 0x, plus the
    `addr: w0 w1 w2 w3` layout that memory-dump tools emit."""
    words = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        # Drop a leading "0x20001000:" style address column.
        line = re.sub(r"^[0-9a-fA-Fx]+\s*:\s*", "", line)
        for tok in re.split(r"[\s,]+", line):
            if not tok:
                continue
            try:
                words.append(int(tok, 16) & 0xFFFFFFFF)
            except ValueError as exc:
                raise LogError(f"not a hex word: {tok!r}") from exc
    return words


def _le_bytes(words):
    return b"".join(struct.pack("<I", w) for w in words)


def parse(words) -> Record:
    """Walk a word list into a Record. Raises LogError on a malformed log."""
    if not words:
        raise LogError("empty log: nothing was read back from the device")
    if words[0] != M["AES_LOG_MAGIC"]:
        raise LogError(
            f"bad magic {words[0]:#010x}, expected {M['AES_LOG_MAGIC']:#010x} "
            '("SAE1"). The harness did not run, or the wrong address was read.'
        )

    hdr = M["AES_LOG_HEADER_WORDS"]
    if len(words) < hdr:
        raise LogError(f"truncated header: {len(words)} words, need {hdr}")

    rec = Record(version=words[1])
    if rec.version != M["AES_LOG_VERSION"]:
        raise LogError(
            f"log format version {rec.version}, this reader understands "
            f"{M['AES_LOG_VERSION']}. Reflash the harness or use a matching reader; "
            "guessing at an unknown layout risks reporting a pass that never happened."
        )
    if words[2] != M["AES_LOG_START"]:
        raise LogError(
            f"missing start sentinel at word 2 ({words[2]:#010x}): the header "
            "looks like a log but the run never began"
        )
    rec.clock_hz, rec.backend, rec.build_id = words[3], words[4], words[5]

    # The last four words are a fatal-fault record written by the NMI/HardFault
    # vectors. Checked before the marker walk, because a fault explains why the
    # walk is about to run out of log.
    slot = M["AES_LOG_FAULT_SLOT"]
    if len(words) > slot + 3 and \
            (words[slot] & M["AES_LOG_FAULT_MASK"]) == M["AES_LOG_FAULT_MAGIC"]:
        kind = words[slot] & ~M["AES_LOG_FAULT_MASK"] & 0xFF
        rec.fault = {
            "vector": {1: "NMI", 2: "HardFault"}.get(kind, f"kind {kind}"),
            "mcause": words[slot + 1],
            "mepc": words[slot + 2],
            "mtval": words[slot + 3],
        }

    i = hdr
    n = len(words)
    while i < n:
        w = words[i]

        if w == M["AES_LOG_END"]:
            rec.complete = True
            break

        if w == M["AES_LOG_OVERFLOW"]:
            rec.overflow = True
            break

        if (w & M["AES_LOG_VECTOR_MASK"]) == M["AES_LOG_VECTOR_BASE"]:
            width = M["AES_LOG_VECTOR_WORDS"]
            if i + width > n:
                raise LogError(f"truncated vector record at word {i}")
            vec_id = w & ~M["AES_LOG_VECTOR_MASK"] & 0xFF
            result = words[i + 5]
            rec.vectors[vec_id] = Vector(
                vec_id=vec_id,
                ciphertext=_le_bytes(words[i + 1:i + 5]),
                bytes_ok=bool(result & M["AES_LOG_RESULT_BYTES_OK"]),
                status_ok=bool(result & M["AES_LOG_RESULT_STATUS_OK"]),
            )
            i += width
            continue

        if w == M["AES_LOG_KAT_END"]:
            if i + 1 >= n:
                raise LogError("KAT section end with no summary word")
            rec.kat_all = words[i + 1]
            i += 2
            continue

        if w == M["AES_LOG_DIFF_ENTER"]:
            rec.differential.entered = True
            i += 1
            # Checkpoints are raw sums, not markers: consume until EXIT.
            while i < n and words[i] != M["AES_LOG_DIFF_EXIT"]:
                if words[i] == M["AES_LOG_END"] or words[i] == M["AES_LOG_OVERFLOW"]:
                    # Ran off the end of the differential without an exit.
                    break
                rec.differential.checkpoints.append(words[i])
                i += 1
            if i < n and words[i] == M["AES_LOG_DIFF_EXIT"]:
                rec.differential.exited = True
                if i + 7 > n:
                    raise LogError(f"truncated differential summary at word {i}")
                rec.differential.count = words[i + 1]
                rec.differential.fold = f"{words[i + 2]:08x}"
                rec.differential.last_ciphertext = _le_bytes(words[i + 3:i + 7])
                i += 7
            continue

        if (w & M["AES_LOG_TIMING_MASK"]) == M["AES_LOG_TIMING_BASE"]:
            width = M["AES_LOG_TIMING_WORDS"]
            if i + width > n:
                raise LogError(f"truncated timing record at word {i}")
            rec.timings[w & ~M["AES_LOG_TIMING_MASK"] & 0xFF] = words[i + 1]
            i += width
            continue

        if w == M["AES_LOG_CORECFGR"]:
            if i + 1 >= n:
                raise LogError("CORECFGR marker with no value")
            rec.corecfgr = words[i + 1]
            i += 2
            continue

        raise LogError(
            f"unrecognised word {w:#010x} at offset {i}: the log is out of sync "
            "with this reader. Not skipping it -- scanning forward would "
            "eventually resynchronise onto payload data and report confident "
            "nonsense."
        )

    return rec


def verify(rec: Record, expectations: dict, expected_fold: str | None = None):
    """Independently re-check a parsed record. Returns a list of failure strings.

    `expectations` maps vector id -> expected ciphertext (hex str or bytes).
    Vector ids absent from it are checked only via the device's own flags --
    the contract properties (in-place, overlap, ...) all reduce to a published
    vector, so in practice the caller supplies every one.
    """
    failures = []

    if rec.fault is not None:
        f = rec.fault
        failures.append(
            f"the harness took a fatal {f['vector']} at mepc={f['mepc']:#010x} "
            f"(mcause={f['mcause']:#010x}, mtval={f['mtval']:#010x}); "
            "everything after that point is missing because the code stopped, "
            "not because the cipher was wrong"
        )
    elif not rec.complete:
        failures.append(
            "record is incomplete: the harness never reached its end marker, so "
            "it hung or reset partway through"
        )
    if rec.overflow:
        failures.append(
            "the harness overflowed its log buffer; raise AES_LOG_WORDS"
        )

    missing = sorted(set(expectations) - set(rec.vectors))
    if missing:
        names = ", ".join(VECTOR_NAMES.get(v, str(v)) for v in missing)
        failures.append(f"vectors never ran: {names}")

    for vec_id, vec in sorted(rec.vectors.items()):
        if not vec.status_ok:
            failures.append(
                f"{vec.name}: the seam did not return HAL_AES_OK "
                "(on CH592 that means the hardware engine timed out)"
            )
        want = expectations.get(vec_id)
        if want is None:
            if not vec.bytes_ok:
                failures.append(f"{vec.name}: device reported a mismatch")
            continue
        want = bytes.fromhex(want) if isinstance(want, str) else want
        if vec.ciphertext != want:
            failures.append(
                f"{vec.name}: ciphertext {vec.ciphertext.hex()} != "
                f"expected {want.hex()}"
            )
        elif not vec.bytes_ok:
            # Bytes are right but the device said they were wrong. That is a
            # broken on-device comparison, which would mask a real failure.
            failures.append(
                f"{vec.name}: ciphertext is correct but the device reported a "
                "mismatch -- the harness's own comparison is broken"
            )

    d = rec.differential
    if not d.entered:
        failures.append("the 512-block differential never ran")
    elif d.hung:
        failures.append(
            f"the differential hung after {len(d.checkpoints)} checkpoints "
            f"(~{len(d.checkpoints) * M['AES_LOG_DIFF_CHECKPOINT']} blocks)"
        )
    else:
        if d.count != M["AES_LOG_DIFF_COUNT"]:
            failures.append(
                f"differential ran {d.count} blocks, expected "
                f"{M['AES_LOG_DIFF_COUNT']}"
            )
        if expected_fold is not None and d.fold != expected_fold:
            failures.append(
                f"differential fold {d.fold} != expected {expected_fold}: at "
                "least one of 512 ciphertexts differs from every other backend"
            )
    return failures


def format_report(rec: Record, failures=None) -> str:
    out = []
    out.append(f"backend      {rec.backend_name}")
    out.append(f"build id     {rec.build_id:08X}")
    out.append(f"core clock   {rec.clock_hz:,} Hz")
    if rec.corecfgr is not None:
        loop = "on" if rec.corecfgr & 0x08 else "off"
        out.append(f"CORECFGR     {rec.corecfgr:#04x} (ROM_LOOP_ACC {loop})")
    out.append("")

    out.append("vectors")
    for vec_id in sorted(rec.vectors):
        v = rec.vectors[vec_id]
        mark = "pass" if (v.bytes_ok and v.status_ok) else "FAIL"
        out.append(f"  [{mark}] {v.name:<38} {v.ciphertext.hex()}")
    if rec.kat_all is not None:
        out.append(f"  device summary: {'all passed' if rec.kat_all else 'FAILURES'}")
    if rec.fault is not None:
        f = rec.fault
        out.append(f"FAULT        {f['vector']} at mepc={f['mepc']:#010x} "
                   f"mcause={f['mcause']:#010x} mtval={f['mtval']:#010x}")
    out.append("")

    d = rec.differential
    out.append("differential")
    if not d.entered:
        out.append("  did not run")
    elif d.hung:
        out.append(f"  HUNG after {len(d.checkpoints)} checkpoints")
    else:
        out.append(f"  {d.count} blocks, fold {d.fold}")
        out.append(f"  last ciphertext {d.last_ciphertext.hex()}")
        out.append(f"  {len(d.checkpoints)} checkpoints recorded")
    out.append("")

    if rec.timings:
        out.append("timing (core cycles)")
        overhead = rec.timings.get(M.get("AES_LOG_TIME_OVERHEAD", 0), 0)
        for t in sorted(rec.timings):
            name = TIMING_NAMES.get(t, f"timing {t}")
            out.append(f"  {name:<24} {rec.timings[t]:>8,}")
        if overhead:
            out.append(f"  (measurement overhead {overhead} cycles, not subtracted)")
        out.append("")

    if failures is not None:
        if failures:
            out.append(f"RESULT: FAIL ({len(failures)})")
            out.extend(f"  - {f}" for f in failures)
        else:
            out.append("RESULT: PASS")
    return "\n".join(out)


def load_expectations():
    """Expected ciphertexts per vector id, from the shared vector table.

    `aes_vectors.py` lives in `firmware/tests/` because the host suite is its
    primary consumer; it is imported here rather than duplicated so the device
    is always judged against the same constants the host proves. Vectors 7-11
    are contract properties (in-place, overlap, re-key, ...) that all reduce to
    the FIPS-197 C.1 answer.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
    import aes_vectors  # noqa: E402

    fips = aes_vectors.VECTORS[0][3]
    exp = {i + 1: aes_vectors.VECTORS[i][3] for i in range(6)}
    exp.update({7: fips, 8: fips, 9: fips, 10: fips, 11: fips})
    return exp, aes_vectors.EXPECTED_FOLD


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path", nargs="?", default="-",
                    help="file of raw little-endian words, or hex text; - for stdin")
    ap.add_argument("--binary", action="store_true",
                    help="input is raw bytes rather than hex text")
    ap.add_argument("--decode-only", action="store_true",
                    help="print the record without judging it")
    args = ap.parse_args(argv)

    raw = sys.stdin.buffer.read() if args.path == "-" else Path(args.path).read_bytes()

    try:
        words = words_from_bytes(raw) if args.binary else words_from_text(
            raw.decode("utf-8", "replace"))
        rec = parse(words)
    except LogError as exc:
        # Exit 3 is the infrastructure/parse code, distinct from a test failure.
        # Keeping them apart is what stops a dead probe reading as a broken cipher.
        print(f"RESULT: FAIL\n  - {exc}", file=sys.stderr)
        return 3

    if args.decode_only:
        print(format_report(rec))
        return 0

    expectations, fold = load_expectations()
    failures = verify(rec, expectations, fold)
    print(format_report(rec, failures))
    return 2 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
