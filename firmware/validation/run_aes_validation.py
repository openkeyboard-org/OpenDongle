#!/usr/bin/env python3
"""Run the AES validation suite across real CH570 / CH572 / CH592 hardware.

Copyright 2026 Eric Molitor (EMulator)
SPDX-License-Identifier: Apache-2.0

Builds each arm, flashes it, verifies the device is running THAT image, lets it
run, halts it, reads the log out of RAM and judges it. The point of the whole
exercise is the cross-arm assertion at the end: every backend must produce the
same 512-block checksum, because that interoperability is exactly what
`hal_aes.h` promises and what an encrypted CH570<->CH592 link depends on.

  !! THIS ERASES OPENBOOT. The validation image is standalone and owns the
  !! whole part. A device that has run it is not a dongle until it is
  !! re-flashed with a factory image. Nothing is written without --confirm-erase.

Operating rules, each of which is here because of a specific way an earlier
campaign went wrong:

  - REFUSES TO GUESS A PROBE. A multi-probe bench carries boards that must
    never be written -- including, at times, a keyboard in daily use. Probes are
    named per chip or the arm is skipped, and minichlink is never allowed to
    pick one arbitrarily (it will, silently, and say so only in a warning).
  - NEVER TRUSTS A WRITE. minichlink ignores the return values of its CH5xx
    erase/write calls and its byte-for-byte verify is disabled, so exit status 0
    is not evidence the image landed. Every write is read back and compared.
  - GATES EVERY RUN ON IDENTITY. The harness logs its own build id and the
    reader extracts it; it is checked against the manifest before any result is
    believed. Skipping exactly this check is what once turned one clean failure
    into two ambiguous ones -- an unattended reflash into an unresponsive device.
  - NAMES WHAT IT SKIPPED. A suite that quietly tests three arms and reports
    success is worse than one that fails, so skipped arms are listed in the
    summary and in the final verdict.

Exit codes: 0 pass, 1 generic failure, 2 assertion failure, 3 probe or
infrastructure failure. Keeping 3 distinct from 2 is what stops a dead probe
reading as a broken cipher.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
# Both paths are explicit because the build tools run under `python3 -I`, and
# isolated mode does NOT prepend the script's own directory to sys.path. Relying
# on that prepend works when invoked directly and breaks under `make`.
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(FIRMWARE / "tests"))

import read_aes_log as R  # noqa: E402
import aes_vectors  # noqa: E402

CORECFGR_H = FIRMWARE / "ch570" / "src" / "ch570_corecfgr.h"

# arm -> which --<chip>-probe it needs. ch572-hw builds from the CH570 SDK but
# runs on different silicon, so it needs its own probe.
ARMS = {
    "ch570-asm-a": "ch570",
    "ch570-asm-f": "ch570",
    "ch570-c": "ch570",
    "ch572-hw": "ch572",
    "ch572-asm-a": "ch572",
    "ch592-hw": "ch592",
}

EXIT_OK, EXIT_FAIL, EXIT_ASSERT, EXIT_INFRA = 0, 1, 2, 3


def expectations():
    fips = aes_vectors.VECTORS[0][3]
    exp = {i + 1: aes_vectors.VECTORS[i][3] for i in range(6)}
    exp.update({7: fips, 8: fips, 9: fips, 10: fips, 11: fips})
    return exp


def define_value(path, name):
    m = re.search(rf"^\s*#define\s+{name}\s+(0[xX][0-9a-fA-F]+|\d+)\b",
                  path.read_text(), re.MULTILINE)
    if m is None:
        raise RuntimeError(f"{name} not defined in {path}")
    return int(m.group(1), 0)


def asm_f_buildable():
    """ASM_F needs CORECFGR bit 3; production boots with it clear.

    Derived from the header rather than hard-coded, so this starts returning
    True by itself on the day the startup value changes.
    """
    return bool(define_value(CORECFGR_H, "CH570_CORECFGR_VALUE")
                & define_value(CORECFGR_H, "CH570_CORECFGR_ROM_LOOP_ACC"))


class Probe:
    """One WCH-LinkE, addressed by serial. Never by 'whichever is plugged in'."""

    def __init__(self, minichlink, serial, dry_run=False):
        self.minichlink = minichlink
        self.serial = serial
        self.dry_run = dry_run

    def run(self, *args, timeout=120):
        cmd = [self.minichlink, "-C", "linke", "-l", self.serial, *map(str, args)]
        if self.dry_run:
            print("    [dry-run] " + " ".join(cmd))
            return subprocess.CompletedProcess(cmd, 0, "", "")
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)

    def check(self, *args, what="", timeout=120):
        proc = self.run(*args, timeout=timeout)
        if proc.returncode != 0:
            raise InfraError(
                f"{what or ' '.join(map(str, args))} failed on probe "
                f"{self.serial}: {(proc.stderr or proc.stdout).strip()[-500:]}")
        return proc

    def info(self):
        return self.run("-i").stdout


class InfraError(Exception):
    """Probe/tooling failure -- exit 3, never scored as a cipher failure."""


def build_arm(arm, toolchain, verbose=False):
    """Build one arm and return its manifest. A build failure is not a test
    failure: ASM_F deliberately refuses to compile while CORECFGR bit 3 is
    clear, and that refusal is a passing behaviour of the guard."""
    cmd = ["make", "-C", str(HERE), f"ARM={arm}"]
    if toolchain:
        cmd.append(f"MRS_TOOLCHAIN={toolchain}")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired as exc:
        raise InfraError(f"build for {arm} timed out after 600s") from exc
    if proc.returncode != 0:
        raise InfraError(f"build failed for {arm}:\n{proc.stdout[-1500:]}\n"
                         f"{proc.stderr[-1500:]}")
    if verbose:
        print(proc.stdout)
    return json.loads((HERE / "build" / arm / "manifest.json").read_text())


def flash_and_run(probe, manifest, settle_s):
    """Erase, write, read back, compare, then start the device running."""
    binary = Path(manifest["bin"])
    size = binary.stat().st_size

    # -E does its own halt-and-reset. Erasing first means a failed write cannot
    # leave the previous arm's image running and be mistaken for this one.
    probe.check("-E", what="chip erase")
    probe.check("-w", str(binary), "0", what="flash write")

    readback = binary.with_suffix(".readback.bin")
    probe.check("-r", str(readback), "0", str(size), what="flash read-back")
    if not probe.dry_run:
        if readback.read_bytes() != binary.read_bytes():
            raise InfraError(
                f"flash read-back differs from {binary.name}: the write did not "
                "land. minichlink reports success regardless, which is why this "
                "is checked rather than assumed.")

    # Per-run nonce: the authority on which EXECUTION a retained snapshot
    # belongs to (see AES_LOG_NONCE_ADDR in aes_log_format.h). Random, but
    # never the all-0/all-F words a blank or shorted line could produce.
    # Written to FLASH because probe writes to RAM silently fail on this
    # silicon (measured: "Image written", readback unchanged) -- and verified
    # by read-back like every other write, for the same reason.
    nonce = 0
    if not probe.dry_run:
        import secrets
        while nonce in (0x00000000, 0xFFFFFFFF):
            nonce = int.from_bytes(secrets.token_bytes(4), "little")
        nfile = Path(manifest["bin"]).with_suffix(".nonce.bin")
        nfile.write_bytes(nonce.to_bytes(4, "little"))
        naddr = f"0x{R.M['AES_LOG_NONCE_ADDR']:08X}"
        probe.check("-w", str(nfile), naddr, what="nonce write")
        nread = Path(manifest["bin"]).with_suffix(".nonce-rb.bin")
        probe.check("-r", str(nread), naddr, "4", what="nonce read-back")
        if nread.read_bytes() != nfile.read_bytes():
            raise InfraError("nonce read-back mismatch: the flash write did "
                             "not land")

    probe.check("-b", what="reboot out of halt")
    if not probe.dry_run:
        time.sleep(settle_s)
    return nonce


def read_keep(probe, manifest):
    """Retained diagnosis block: boots, reset cause, watchdog, furthest stage."""
    addr = manifest.get("keep_addr")
    if not addr or probe.dry_run:
        return None
    out = Path(manifest["bin"]).with_suffix(".keep.bin")
    # 32 bytes = 8 words, the whole block. Reading only the first five silently
    # hid every field beyond VK_STAGE, which defeated a later addition without
    # any error -- the parse just saw a short list.
    probe.check("-r", str(out), addr, "32", what="retained diagnosis read")
    # A short or odd-length read is a probe problem, not a device verdict. Left
    # unguarded it raised LogError out of run_arm, which only catches InfraError,
    # and the whole suite died with a traceback instead of reporting one bad arm.
    try:
        w = R.words_from_bytes(out.read_bytes())
    except R.LogError as exc:
        raise InfraError(f"malformed retained-diagnosis read: {exc}") from exc
    # The whole 8-word block or nothing: a short-but-word-aligned read is a
    # transport fault, and quietly accepting 5-7 words would let truncated
    # retained data pass as trustworthy -- exactly what the 32-byte request
    # exists to prevent. Only a magic mismatch means "no baseline yet".
    if len(w) != 8:
        raise InfraError(
            f"retained-diagnosis read returned {len(w)} words, expected 8")
    if w[0] != R.M["AES_LOG_KEEP_MAGIC"]:
        return None
    return {"boots": w[1], "reset_status": w[2],
            "wdog_ctrl": w[3] & 0xFF, "wdog_count": (w[3] >> 8) & 0xFF,
            "stage": w[4]}


def read_log(probe, manifest, boots_before=0, nonce=0):
    """Halt the device and read the log back.

    PREFERS THE RETAINED SNAPSHOT. `aes_log` is in .bss and a reset clears it,
    so on a part that reboots the live array shows a fresh partial run and the
    completed one is gone. The harness copies each finished record into
    retained memory, which a reset cannot reach; that copy is authoritative
    when present, and the live array is only a fallback for a run that never
    finished (where a partial log is exactly what you want to see).
    """
    nbytes = int(manifest["log_words"]) * 4
    # -A halts WITHOUT resetting. A reset here would clear the very RAM being
    # read; the log only exists because the part is still where it stopped.
    probe.check("-A", what="halt")

    saved_addr = manifest.get("saved_addr")
    if saved_addr and not probe.dry_run:
        out = Path(manifest["bin"]).with_suffix(".saved.bin")
        probe.check("-r", str(out), saved_addr, str(nbytes + 16),
                    what="retained log read")
        try:
            w = R.words_from_bytes(out.read_bytes())
        except R.LogError as exc:
            raise InfraError(f"malformed retained-log read: {exc}") from exc
        # The whole block or a transport error -- same rule as read_keep. A
        # short word-aligned read previously indexed off the end (traceback,
        # not a verdict) or quietly fell through to the live log.
        if len(w) != int(manifest["log_words"]) + 4:
            raise InfraError(
                f"retained-log read returned {len(w)} words, expected "
                f"{int(manifest['log_words']) + 4}")
        # w[3] is the run nonce -- the authority on which EXECUTION wrote this
        # snapshot. The runner wrote a fresh random word into flash after the
        # image; only a snapshot echoing it can be from this run, which closes
        # every staleness path at once: retained RAM survives power cycles and
        # reflashing, and the build id only distinguishes builds, so without
        # the nonce a snapshot from an earlier run of the SAME build could be
        # reported as this run's result. The boot-count check stays as belt
        # and braces for the baseline-known case; it costs nothing.
        if (w[0] == R.M["AES_LOG_SAVED_MAGIC"] and 0 < w[1] <= len(w) - 4
                and nonce and w[3] == nonce and w[2] > boots_before):
            return list(w[4:4 + w[1]]), True

    out = Path(manifest["bin"]).with_suffix(".log.bin")
    probe.check("-r", str(out), manifest["log_addr"], str(nbytes),
                what="live log read")
    if probe.dry_run:
        return [], False
    try:
        return R.words_from_bytes(out.read_bytes()), False
    except R.LogError as exc:
        raise InfraError(f"malformed live-log read: {exc}") from exc


def run_arm(arm, probe, manifest, settle_s, exp, fold):
    """Returns (failures, record). Raises InfraError for tooling problems."""
    # Before writing anything: how many times has this part booted so far? Any
    # retained snapshot at or below this count predates the run and is stale.
    before = read_keep(probe, manifest)
    # An absent baseline is stated, not collapsed to 0: with a 0 sentinel the
    # snapshot freshness gate degenerates to "any snapshot" and the reboot
    # delta counts history as if it were this run. (Cross-build snapshot reuse
    # is separately blocked by verify(): the snapshot content embeds the build
    # id of the image that wrote it.)
    baseline_known = before is not None
    boots_before = before["boots"] if baseline_known else 0

    nonce = flash_and_run(probe, manifest, settle_s)
    words, retained = read_log(probe, manifest, boots_before, nonce)
    keep = read_keep(probe, manifest)
    if probe.dry_run:
        return ["dry run: no device was contacted"], None

    try:
        rec = R.parse(words)
    except R.LogError as exc:
        raise InfraError(f"could not decode the log from {arm}: {exc}") from exc

    failures = []
    # Identity gate. Checked against the RUNNING image's own logged id, which is
    # stronger than comparing flash contents: it proves the code that produced
    # these results is the code we meant to test.
    want_id = int(manifest["build_id"], 16)
    if rec.build_id != want_id:
        failures.append(
            f"device reports build id {rec.build_id:08X}, expected "
            f"{want_id:08X}: it is not running the image just flashed")
    if rec.backend != manifest["backend_id"]:
        failures.append(
            f"device reports backend {rec.backend_name}, expected id "
            f"{manifest['backend_id']}")
    if rec.clock_hz != manifest["clock_hz"]:
        failures.append(
            f"device ran at {rec.clock_hz:,} Hz, expected "
            f"{manifest['clock_hz']:,}: cycle figures are not comparable")

    failures.extend(R.verify(rec, exp, fold))

    # Report reboots even when the run itself passed: a part that reboots is a
    # finding, and the retained snapshot deliberately hides it from the log.
    if keep:
        rec.keep = keep
        # CUMULATIVE, not per-run: retained RAM survives reflashing, so this
        # counter climbs across runs. Reporting it raw once read "booted 50
        # times during this run" when the run accounted for about five -- the
        # difference between a part in a reset loop and a part behaving
        # normally, and it sent a whole investigation down the wrong path. Two
        # or three of those are the erase and the reboot this runner performs
        # itself, so a healthy arm shows a small delta.
        booted = keep["boots"] - boots_before if baseline_known else None
        if booted is not None and booted > 4:
            note = (f"the part booted {booted} times during this run "
                    f"(cumulative {keep['boots']}, reset status "
                    f"{keep['reset_status']:#04x}, furthest stage "
                    f"{keep['stage']})")
            if retained:
                print(f"    note: {note}; results are from the retained "
                      "snapshot of a completed run")
            else:
                failures.append(note + " and no completed run was retained")
    return failures, rec


def write_summary(path_json, path_md, results, skipped, fold):
    """Written after EVERY arm, so a killed run still leaves a valid report."""
    payload = {
        "expected_fold": fold,
        "arms": results,
        "skipped": skipped,
    }
    path_json.write_text(json.dumps(payload, indent=2) + "\n")

    md = ["# AES hardware validation", ""]
    md.append(f"Expected differential fold: `{fold}`")
    md.append("")
    md.append("| arm | result | fold | cycles/block | CORECFGR |")
    md.append("|---|---|---|---|---|")
    for r in results:
        md.append(
            f"| {r['arm']} | {r['result']} | `{r.get('fold') or '-'}` | "
            f"{r.get('cycles_per_block') or '-'} | {r.get('corecfgr') or '-'} |")
    if skipped:
        md.append("")
        md.append("## Skipped")
        for s in skipped:
            md.append(f"- **{s['arm']}** — {s['reason']}")
    for r in results:
        if r["failures"]:
            md.append("")
            md.append(f"## {r['arm']} failures")
            md.extend(f"- {f}" for f in r["failures"])
    path_md.write_text("\n".join(md) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ch570-probe", default="", help="WCH-LinkE serial for the CH570")
    ap.add_argument("--ch572-probe", default="", help="WCH-LinkE serial for the CH572")
    ap.add_argument("--ch592-probe", default="", help="WCH-LinkE serial for the CH592")
    ap.add_argument("--arm", action="append", dest="arms",
                    help="run only this arm (repeatable); default is all")
    ap.add_argument("--minichlink", default="minichlink")
    ap.add_argument("--toolchain", default="", help="MRS_TOOLCHAIN bin directory")
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to let the harness run before halting")
    ap.add_argument("--report-dir", default="", help="where to write the report")
    ap.add_argument("--confirm-erase", default="",
                    help='must be exactly "1". Flashing a validation image '
                         "ERASES OPENBOOT on the target.")
    ap.add_argument("--allow-skips", default="",
                    help='must be exactly "1" to let a run with skipped arms '
                         "report PASS. Off by default: a suite that tests a "
                         "subset and reports success hides the untested arms.")
    ap.add_argument("--dry-run", action="store_true",
                    help="build and print every probe command without running it")
    args = ap.parse_args(argv)

    probes = {"ch570": args.ch570_probe.strip(),
              "ch572": args.ch572_probe.strip(),
              "ch592": args.ch592_probe.strip()}

    # Compared against "1" rather than tested for non-emptiness, so
    # --confirm-erase 0 fails safe instead of reading as consent.
    if not args.dry_run and args.confirm_erase != "1":
        print("refusing to write: pass --confirm-erase 1.\n"
              "The validation image is standalone and ERASES OPENBOOT on the\n"
              "target; the device must be re-flashed with a factory image\n"
              "afterwards before it is a dongle again.", file=sys.stderr)
        return EXIT_INFRA

    if not any(probes.values()) and not args.dry_run:
        print("no probes named. Set at least one of --ch570-probe, "
              "--ch572-probe, --ch592-probe.\nThis tool will not scan for a "
              "probe: a multi-probe bench carries boards that must never be "
              "written.", file=sys.stderr)
        return EXIT_INFRA

    if shutil.which(args.minichlink) is None and not args.dry_run:
        print(f"minichlink not found: {args.minichlink}", file=sys.stderr)
        return EXIT_INFRA

    selected = args.arms or list(ARMS)
    for arm in selected:
        if arm not in ARMS:
            print(f"unknown arm {arm!r}; choose from {', '.join(ARMS)}",
                  file=sys.stderr)
            return EXIT_INFRA

    report_dir = Path(args.report_dir) if args.report_dir else HERE / "reports" / "latest"
    report_dir.mkdir(parents=True, exist_ok=True)
    summary_json = report_dir / "summary.json"
    summary_md = report_dir / "summary.md"

    exp = expectations()
    fold = aes_vectors.EXPECTED_FOLD
    results, skipped = [], []
    infra_failed = False

    for arm in selected:
        chip = ARMS[arm]
        serial = probes.get(chip, "")

        if arm == "ch570-asm-f" and not asm_f_buildable():
            skipped.append({"arm": arm, "reason":
                            "ASM_F requires CORECFGR bit 3 (ROM_LOOP_ACC); "
                            f"startup writes "
                            f"{define_value(CORECFGR_H, 'CH570_CORECFGR_VALUE'):#04x}, "
                            "which has it clear, so the backend refuses to build"})
            print(f"[skip] {arm}: CORECFGR bit 3 is clear")
            write_summary(summary_json, summary_md, results, skipped, fold)
            continue

        if not serial and not args.dry_run:
            skipped.append({"arm": arm,
                            "reason": f"no --{chip}-probe given"})
            print(f"[skip] {arm}: no --{chip}-probe given")
            write_summary(summary_json, summary_md, results, skipped, fold)
            continue

        print(f"[run ] {arm} on probe {serial or '(dry-run)'}")
        entry = {"arm": arm, "chip": chip, "probe": serial}
        try:
            manifest = build_arm(arm, args.toolchain)
            entry["build_id"] = manifest["build_id"]
            probe = Probe(args.minichlink, serial or "dry-run", args.dry_run)
            failures, rec = run_arm(arm, probe, manifest, args.settle, exp, fold)
            entry["failures"] = failures
            entry["result"] = "PASS" if not failures else "FAIL"
            if rec is not None:
                entry["fold"] = rec.differential.fold
                entry["cycles_per_block"] = rec.timings.get(2)
                entry["key_expand_cycles"] = rec.timings.get(1)
                entry["corecfgr"] = (f"{rec.corecfgr:#04x}"
                                     if rec.corecfgr is not None else None)
                print(R.format_report(rec, failures))
        except InfraError as exc:
            infra_failed = True
            entry["result"] = "INFRA"
            entry["failures"] = [str(exc)]
            print(f"[infra] {arm}: {exc}", file=sys.stderr)

        results.append(entry)
        write_summary(summary_json, summary_md, results, skipped, fold)

    # ---- the cross-arm assertion, which is the point of the whole suite ----
    folds = {r["arm"]: r.get("fold") for r in results if r.get("fold")}
    cross_arm_ok = True
    if folds:
        distinct = set(folds.values())
        if len(distinct) > 1 or distinct != {fold}:
            cross_arm_ok = False
            print("\nCROSS-ARM MISMATCH: backends did not agree.", file=sys.stderr)
            for a, f in folds.items():
                print(f"  {a}: {f}", file=sys.stderr)
            print(f"  expected: {fold}", file=sys.stderr)
    # Agreement between backends is the entire point of the suite, and one arm
    # cannot agree with anything. Treat an un-exercised comparison like a
    # skipped arm rather than a pass: --allow-skips 1 accepts it, the default
    # does not.
    cross_arm_exercised = len(folds) >= 2
    if not cross_arm_exercised:
        print(f"\nNote: {len(folds)} arm(s) produced a checksum, so the cross-arm "
              "comparison\n(the property this suite exists to prove) was not "
              "actually exercised.")

    print("\n" + "=" * 60)
    for r in results:
        print(f"  {r['result']:<5} {r['arm']}")
    for s in skipped:
        print(f"  SKIP  {s['arm']} — {s['reason']}")
    print(f"report: {summary_md}")

    if skipped:
        print(f"\n{len(skipped)} arm(s) were NOT tested; see the report.")
    if any(r["arm"] for r in results) and not args.dry_run:
        print("\nEvery device written is now running a standalone validation "
              "image and\nhas NO bootloader. Re-flash it before use:\n"
              "  make -C .. ch570-factory-flash WCHLINK_SERIAL=<serial>")

    if infra_failed:
        print("RESULT: FAIL (infrastructure)")
        return EXIT_INFRA
    if any(r["failures"] for r in results) or not cross_arm_ok:
        print("RESULT: FAIL")
        return EXIT_ASSERT
    if not results:
        print("RESULT: FAIL (nothing ran)")
        return EXIT_INFRA
    # Compared against "1" so --allow-skips 0 fails safe rather than reading as
    # consent, same rule as --confirm-erase.
    if (skipped or not cross_arm_exercised) and args.allow_skips != "1":
        n = len(skipped)
        why = []
        if n:
            why.append(f"{n} arm{'' if n == 1 else 's'} "
                       f"{'was' if n == 1 else 'were'} not tested")
        if not cross_arm_exercised:
            why.append("the cross-arm comparison was not exercised")
        print("RESULT: INCOMPLETE -- every arm that ran passed, but "
              + " and ".join(why) + ".\n"
              "Pass --allow-skips 1 to accept a partial run as success.")
        return EXIT_FAIL
    print("RESULT: PASS")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
