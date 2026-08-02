#!/usr/bin/env python3
"""Finalize an ELF's ODG2 section and emit a self-consistent ELF/BIN pair."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.fspath(Path(__file__).resolve().parent))

from dongle_image_id import IMAGE_ID_LEN, IMAGE_ID_OFF, finalize_image


def run(argv):
    subprocess.run(argv, check=True)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--objcopy", required=True)
    p.add_argument("--input-elf", required=True)
    p.add_argument("--output-elf", required=True)
    p.add_argument("--output-bin", required=True)
    args = p.parse_args()

    input_elf = Path(args.input_elf)
    output_elf = Path(args.output_elf)
    output_bin = Path(args.output_bin)
    output_elf.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output_elf.parent) as td_name:
        td = Path(td_name)
        raw_bin = td / "raw.bin"
        header = td / "odg2.bin"
        final_elf = td / "final.elf"
        final_bin = td / "final.bin"
        run([args.objcopy, "-O", "binary", os.fspath(input_elf), os.fspath(raw_bin)])
        finalized = finalize_image(raw_bin.read_bytes())
        header.write_bytes(finalized[IMAGE_ID_OFF:IMAGE_ID_OFF + IMAGE_ID_LEN])
        shutil.copyfile(input_elf, final_elf)
        run([args.objcopy, "--update-section", f".dongle_id={header}", final_elf])
        run([args.objcopy, "-O", "binary", final_elf, final_bin])
        if final_bin.read_bytes() != finalized:
            raise RuntimeError("finalized ELF does not reproduce the finalized BIN")
        os.replace(final_elf, output_elf)
        os.replace(final_bin, output_bin)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
