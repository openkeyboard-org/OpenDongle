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
| `ch570-asm-f` | CH570 | `ASM_F` | zero SRAM code; needs the loop buffer |
| `ch570-c` | CH570 | portable C | the fallback any future chip gets first |
| `ch572-hw` | CH572 | hardware engine | same driver as CH592, different silicon |
| `ch572-asm-a` | CH572 | `ASM_A` | same-silicon software-vs-engine control |
| `ch592-hw` | CH592 | hardware engine | |

`ASM_F` requires `CORECFGR` bit 3 (`ROM_LOOP_ACC`). Production now boots
`0x2D`, which has it set, so the arm builds and runs. If the startup value ever
returns to `0x25` the backend's `#error` guard resumes refusing the build, and
the runner reports the arm as skipped with the reason — **that refusal is
correct behaviour, not a broken arm**: without it the backend would be
bit-exact but roughly 15× slower than its documented cost, silently. The
runner derives the condition from `ch570_corecfgr.h`, so no configuration
accompanies the flip in either direction.

CH572 is the control that matters historically: it is the same family as CH570
and *has* the AES engine at the same register block, which is how "CH570's
engine is absent" was distinguished from "CH570's engine is mis-driven".

## Running it

```sh
# Build every arm, touch no hardware. Proves the ASM_F guard still fires.
make aes-hw-build MRS_TOOLCHAIN=<gcc bin dir>

# See exactly what would happen, contact nothing.
make aes-hw-validate AES_HW_ARGS="--dry-run" MRS_TOOLCHAIN=<gcc bin dir>

# For real. Name a probe per chip you have.
make aes-hw-validate MRS_TOOLCHAIN=<gcc bin dir> \
    AES_HW_CH570=<probe serial> AES_HW_CH572=<probe serial> \
    AES_HW_CH592=<probe serial> \
    AES_HW_ARGS="--confirm-erase 1 --allow-skips 1"
```

`--allow-skips 1` is needed for any run that does not cover every arm, and one
always does: `ch570-asm-f` cannot be built while CORECFGR bit 3 is clear. Without
it the run reports `INCOMPLETE` and exits 1 even though every arm that ran
passed. That default is deliberate -- a suite that quietly tests a subset and
prints PASS is worse than one that fails -- so pass the flag when a partial run
is what you meant, and leave it off in CI.

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

Production CORECFGR is `0x2D` (ROM loop buffer on). Every buildable arm passes
and produces the same differential fold — the property the suite exists to
prove:

| arm | chip | fold | cycles/block | key schedule |
|---|---|---|---|---|
| `ch570-asm-a` | V3C @100 MHz | `b106130c` | 1,672 | 7,647 |
| `ch570-asm-f` | V3C @100 MHz | `b106130c` | 3,960 | 7,405 |
| `ch570-c` | V3C @100 MHz | `b106130c` | 30,694 | 16,543 |
| `ch572-hw` | CH572 @100 MHz | `b106130c` | 2,967 | 322 |
| `ch572-asm-a` | CH572 @100 MHz | `b106130c` | 1,672 | 7,647 |
| `ch592-hw` | CH592 @60 MHz | `b106130c` | 871 | 838 |

The V3C rows measure identically on CH570 and CH572 silicon — same core, same
buffer — which is what lets either part stand in for the other on the bench,
and figures reproduce to the cycle across independent sweeps (key schedules
within ±2). At the old `0x25` (buffer off) the same silicon measured ASM_A
1,944 / 59,113, C 43,396 / 45,378, CH572 engine 4,011 / 5,126, with ASM_F
unbuildable; those flash-dominated figures moved ~3% with unrelated link
shifts, so read them to two significant figures.

`ch572-asm-a` exists so the software-vs-hardware comparison is same-silicon:
**the assembly kernel is 1.8× faster than the hardware engine** on the part
that has the engine (1,672 against 2,966). The engine core is not slow, its
driver is — it reloads the key and shuffles data through registers on every
block. The key schedule leans the other way (8,899 against 1,707), so the
engine wins below ~6 blocks per key and the software kernel above.

Cycle figures are measured in-loop and include call overhead, so they run a
little above kernel-only costs quoted in bench material. The key-schedule
column is timed with the key in SRAM -- the production regime, since real keys
come from the bond record in RAM. It was previously timed with the flash-
resident KAT constant, which inflated the hardware arms 2-10x and looked like
an anomaly; the anatomy below is what that investigation found.


## The key-schedule numbers: an anatomy

The hardware arms once reported a key-schedule cost "far above what caching
four words can plausibly take" (CH572 5,126 at CORECFGR 0x25, 1,707 at 0x2D;
CH592 1,359), with a CH572/CH592 ratio that did not match the clock ratio.
Investigated to closure; three stacked causes, none of them the driver's logic
or the engine:

1. **The code is not what the source says.** GCC 15.2 at -O2 compiles the
   driver's "cache four words" loop into a tail call to newlib-nano's memcpy —
   a one-byte-per-iteration loop, ~110 flash-resident instructions per call.
2. **The measured key lived in flash.** The KAT constant is `.rodata`, so every
   call did 16 flash *data* reads. Measured on CH572 @0x2D with matched loops:
   the identical 16-byte copy costs 118 cycles from an SRAM source and ~1,760
   from a flash source. A discriminator (same bytes as 4 aligned word loads:
   182 cycles) shows the penalty is **per access**, roughly 10x worse for byte
   loads than word loads; source alignment is a ~5% effect, no cliff.
3. **On CH592, instruction fetch itself.** The V4C core has no loop buffer and
   its 60 MHz flash fetch measured ~4.1 cycles/instruction even straight-line,
   so the ~110-instruction loop dominates: ~800 of the 1,359 was fetch.

The clock-ratio "anomaly" dissolves accordingly: the two parts differ in fetch
and data-path architecture, not just clock.

Two lessons the investigation itself paid for, worth keeping:

- **Micro-benchmarks under the loop buffer are placement-sensitive.** An empty
  call+ret probe measured 160 cycles in one layout and 13 in the next; the
  set_key field moved 12% from unrelated harness edits. Individual small
  figures at 0x2D are not stable; only deltas between matched loops in the
  same image are. (This is also why the flash-resident block figures carry a
  two-significant-figures caveat.)
- **Time the regime that ships.** The field now measures set_key with an SRAM
  key; the flash-key worst case and the full decomposition remain available by
  building an arm with `EXTRA_CFLAGS=-DDONGLE_VALIDATE_TIMING_PROBE`, which
  emits timing tags 3-9.

The switch to SRAM-key semantics also lowered the software arms' key-schedule
figures ~10-15% (e.g. ASM_A 8,899 to 7,647) — their real expansion work had the
same 16 flash key-byte reads buried in it.

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

## CORECFGR bit 3 (ROM_LOOP_ACC): how 0x2D became production

Measured on V3C silicon at 100 MHz; the AES table above is the 0x2D result.
The flip's headline is the key schedule, not the block: ASM_A's went from
67.6% of an 875 µs poll slot to 10.2% (59,113 → 8,899, 6.6×) for zero SRAM,
and ASM_F became buildable and measurable at all (3,992 against its unverified
bench figure of 3,797).

RF at `0x2D`, end to end on a production keyboard: pairing, reconnect, typing,
indicators, media keys and sleep/wake all pass; bench pairing was verified down
to the bond write at `0x3A000` and HID reports arriving on the host. That
exercises both sites that reach the vendor bring-up (boot, and the
post-bond-save reconnect in `rf_task.c`), so the known delay-loop hazard — the
calibration settle in `RFEND_DevInit` collapsing ~99 µs → ~3.3 µs — did not
manifest on any functional path.

Two rules were established the hard way and live in `ch570_corecfgr.h`:

- **Write CORECFGR once, at reset, never again.** A guard that dropped to
  `0x25` across the vendor RF init and restored `0x2D` after broke pairing
  outright (0/2, bond never written) against a byte-exact passing control.
  Both *consistent* configurations work; mixing them fails — the vendor init
  appears to derive timing-dependent values that runtime consumes.
- **Never read it.** CSR `0xBC0` is write-only on this silicon; `csrr`
  destabilises the part (A/B: 3/3 failures with the read, 2/2 passes without).

Residual risk, named rather than buried: RF **margin**. An under-settled
calibration is exactly the fault that works at bench range and degrades at
distance or under interference; functional passes do not retire it.
