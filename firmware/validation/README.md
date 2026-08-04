# AES hardware validation

On-silicon validation of the `hal_aes.h` seam across every backend.

> **Flashing a validation image ERASES OPENBOOT.** These images are standalone
> and own the whole part. A device that has run one is not a dongle until it is
> re-flashed with a factory image. The runner will not write anything without
> `--confirm-erase 1`.

Nothing here runs during `make all` or `make test`. Both stay hardware-free and
toolchain-free; this is opt-in, and you run it when the cipher, the seam or a
backend changes.

## Why this exists

Roughly 45% of the AES code is hand-written RISC-V assembly in two variants.
The property the whole design rests on is that **every backend produces
identical ciphertext** — that is what lets an encrypted CH570↔CH592 link work
at all, and it is not something a unit test on one chip can establish.

## Arms

| arm | chip | backend | notes |
|---|---|---|---|
| `ch570-asm-a` | CH570 | `ASM_A` | the shipping default |
| `ch570-asm-f` | CH570 | `ASM_F` | **cannot build today** — see below |
| `ch570-c` | CH570 | portable C | the fallback any future chip gets first |
| `ch572-hw` | CH572 | hardware engine | same driver as CH592, different silicon |
| `ch592-hw` | CH592 | hardware engine | |

`ASM_F` requires `CORECFGR` bit 3 (`ROM_LOOP_ACC`). Production boots `0x25`,
which has it clear, so selecting that backend fails the build with an explicit
`#error`. **That refusal is correct behaviour, not a broken arm** — without it
the backend would be bit-exact but roughly 15× slower than its documented cost,
silently. The runner detects the condition from `ch570_corecfgr.h` and reports
the arm as skipped with the reason. It starts working by itself the day the
startup value changes.

CH572 is the control that matters historically: it is the same family as CH570
and *has* the AES engine at the same register block, which is how "CH570's
engine is absent" was distinguished from "CH570's engine is mis-driven".

## Running it

```sh
# Build every arm, touch no hardware. Proves the ASM_F guard still fires.
make aes-hw-build MRS_TOOLCHAIN=<gcc bin dir>

# See exactly what would happen, contact nothing.
make aes-hw-validate AES_HW_ARGS="--dry-run" MRS_TOOLCHAIN=<gcc bin dir>

# For real.
make aes-hw-validate MRS_TOOLCHAIN=<gcc bin dir> \
    AES_HW_CH570=<probe serial> AES_HW_CH592=<probe serial> \
    AES_HW_ARGS="--confirm-erase 1"
```

Probe serials are **required per chip and never guessed**. With several probes
attached, minichlink picks one arbitrarily and says so only in a warning; a
multi-probe bench routinely carries boards that must not be written. An arm
whose probe is not named is skipped and listed as skipped — a suite that
quietly tests three arms and reports success is worse than one that fails.

Find serials with `minichlink` (it lists them when more than one is attached).

Exit codes: `0` pass, `1` generic failure, `2` assertion failure, `3` probe or
infrastructure failure. `3` is kept distinct so a dead probe never reads as a
broken cipher. Note that `make` collapses any non-zero status to its own `2`,
so call `validation/run_aes_validation.py` directly when you need to act on the
distinction.

## What each arm checks

Six published known-answer vectors (FIPS-197 C.1, the all-zero vector, and the
four NIST SP 800-38A F.1.1 ECB blocks), five contract properties from
`hal_aes.h` (in-place, partial overlap `out = in+1`, repeated `set_key`, key
change and back, same block twice), and a 512-block differential with an
independent pseudorandom key and plaintext per block folded into one FNV-1a-32.

Independent keys matter: a fixed-key sweep never exercises the key schedule,
which on the assembly backends is a separate hand-written path from the cipher.

The differential fold is **`b106130c`**. It is not a magic number copied from a
bench log — `tests/test_aes_sw.py` re-derives it on the host from the portable
cipher, and `tests/test_aes_validate_harness.py` re-derives it again by running
this very harness on the host. A hardware arm only has to match a value two
independent implementations already agree on.

Cycles per block, key-schedule cycles and the configured `CORECFGR` are recorded
as regression signals, never as pass/fail. A slow cipher is something to
investigate, not a failure.

## Measured results

All four buildable arms pass, and all four produce the same differential fold —
which is the property the suite exists to prove:

| arm | chip | fold | cycles/block | key schedule |
|---|---|---|---|---|
| `ch570-asm-a` | CH570 @100 MHz | `b106130c` | 1,944 | 60,538 |
| `ch570-c` | CH570 @100 MHz | `b106130c` | 43,510 | 47,759 |
| `ch572-hw` | CH572 @100 MHz | `b106130c` | 4,139 | 5,158 |
| `ch592-hw` | CH592 @60 MHz | `b106130c` | 885 | 1,359 |

Cycle figures are measured in-loop and include call overhead, so they run a
little above the kernel-only costs quoted in `hal_aes.h`. Two of them are not
yet reconciled with that header and should not be copied into it as-is: the
hardware arms report a key-schedule cost far above what caching four words can
plausibly take, and the header's "CH592 hardware, 2,700 cycles" row was in fact
measured on a CH572.

## How results come back

The harness cannot print: `printf` blocks at boot when no debug terminal is
attached, which stalls the probe and looks exactly like a hung cipher. Instead
it fills `aes_log[]` in RAM and spins; the runner halts the core and reads the
array out.

`aes_log` is an ordinary exported `.bss` array and the build resolves its
address into `build/<arm>/manifest.json`. It is deliberately **not** at a fixed
address — the original bench harness used a hard-coded `0x20001000`, which is
correct only until something else in the image grows into it, and nothing would
have caught that.

| file | role |
|---|---|
| `aes_log_format.h` | the wire format, shared by harness and reader |
| `aes_validate.c` | the harness; calls `hal_aes.h` and nothing else |
| `read_aes_log.py` | decodes and judges a record; usable standalone |
| `run_aes_validation.py` | builds, flashes, verifies, runs, reads, reports |
| `validate_platform_ch5*.c` | clock bring-up and fatal vectors |
| `link-ch5*.ld` | standalone link, at 0x0 |

`read_aes_log.py` parses its marker constants **out of `aes_log_format.h`**
rather than declaring its own copies, so the reader cannot drift from the
firmware and be believed anyway. Records are fixed-width per marker and the
reader walks them; on a desync it stops and reports the offset rather than
scanning forward, which would eventually resynchronise onto payload data and
produce a confident, wrong report.

Decode a dump by hand with:

```sh
python3 validation/read_aes_log.py record.hex     # judges it
python3 validation/read_aes_log.py --decode-only record.hex
```

## Two traps worth knowing

**SysTick's `STCLK` bit.** Clear, SysTick counts HCLK/8 and every cycle figure
is 8× low. `cycles_init()` sets it explicitly. An entire measurement campaign
was invalidated by this once, and the wrong numbers were plausible enough to
reach a published header before anyone noticed.

**`CORECFGR` must never be READ on CH570.** CSR `0xBC0` is `CORECFGR` on
CH570/CH572, and production only ever writes it. A `csrr 0xbc0` **resets the
part** — and the symptom is thoroughly misleading: the device reboots, `.bss`
is cleared, and the log comes back looking as though the cipher hung partway
through the differential at a deterministic block count. It cost a long
debugging session, and the read existed purely to record the configuration a
measurement was taken under, so it destroyed the measurement it was there to
qualify. The harness now logs `CH570_CORECFGR_VALUE` — the same constant the
reset handler writes, from the same header, so it still cannot drift. On
CH592's V4C core that CSR address is a different register entirely, so the
section is compiled out there.

## Afterwards

Every device written is left running a standalone image with no bootloader:

```sh
make ch570-factory-flash WCHLINK_SERIAL=<serial>
```
