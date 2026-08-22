# Stack-watermark measurement

Puts a number on the worst-case stack depth. Two decisions are gated on it:

- **CH570 stack floor.** The encrypted image no longer fits under
  `CH570_STACK_FLOOR = 0x800` (`ch570/link.ld`) — it is ~100 bytes over
  after the bench-counter gating. The floor was selected without depth data;
  the candidate change is `0x800 → 0x700` (+256 B, still ≥ the `0x640` the
  cold-boot entropy window demands), taken **only if this measurement
  passes**. Fallback if it fails: shrink `rf_crypt_fifo_buf` to 1 slot
  (+22 B, costs stall-drops) and revisit moving `aes_f_sbox` to flash
  (+256 B, ~+35 % per AES block) with the measured number in hand.
- **CH592 true-stack budget** (2026-08-16 review, finding 18): 1,824 B
  between `_susrstack` and `_eusrstack`, also never measured; M1 added
  ~32 B to the deepest crypto frame.

## Build

The instrumented image gets its own build id (EXTRA_CFLAGS is hashed), so it
can never pass for the standard bytes.

```bash
# CH570 -- the candidate configuration measured AS ITSELF: counters gated
# (already in-tree) + the candidate floor via --defsym (PROVIDE yields to it;
# link.ld is NOT edited until the measurement passes).
make -C firmware/ch570 MRS_TOOLCHAIN=... OPENBOOT_TOOLCHAIN=... \
     EXTRA_CFLAGS=-DDONGLE_STACK_WATERMARK=1 \
     "EXTRA_LDFLAGS=-Wl,--defsym=CH570_STACK_FLOOR=0x700"

# CH592 (either profile)
make -C firmware/ch592 MRS_TOOLCHAIN=... OPENBOOT_TOOLCHAIN=... \
     EXTRA_CFLAGS=-DDONGLE_STACK_WATERMARK=1
```

Mechanics (`common/include/stack_watermark.h`): main() paints free RAM with
`0xC5C5C5C5` — on CH570 strictly as its **second** act, after
`ch570_capture_boot_entropy()` reads the pristine power-on RAM the paint
would destroy. On CH592 the paint runs before `CH59x_BLEInit`, whose arena
overwrites the low span, so the scan lands at the true stack
(`_susrstack.._eusrstack`) by construction.

## Read

IAP command `0x96` (unarmed, read-only, measurement builds only) returns
`low_water(4) _end(4) _eusrstack(4)` LE:

- `max_depth = _eusrstack − low_water`
- `slack     = low_water − _end` (CH570; on CH592 read it against
  `_susrstack` from the map)

Cross-check with an offline SRAM dump (`minichlink` read of the same span,
scanning for the first non-`C5` word from `_end` up). On the USB-less bench
the dump is the only readout.

## Exercise matrix (run all before reading)

- cold boot; pairing from scratch
- ≥ 1 h connected typing soak
- encrypted verify soak with a provisioned key (CH592: product profile,
  USB-provisioned; CH570: once it links)
- EV10 re-keys; forced link loss + reacquire
- bond-persist DataFlash write (the IRQ-masked deep path)
- concurrent EP6 IAP traffic while connected
- USB suspend/resume
- a forced fault-handler pass

## Acceptance (CH570 floor cut)

`max_depth + 128 ≤ 0x700` → edit `ch570/link.ld` `CH570_STACK_FLOOR` to
`0x700` permanently (that edit moves the build id; it batches with this
campaign's matrix run and digest re-pin). Otherwise: fallbacks above, and
keep the measured number with the record.

**Result (2026-08-18):** on-silicon run over the encrypted-link exercise (pair
+ USB provision + soak) measured **max_depth 548 B, slack 1372 B** at the
0x700 candidate (`low 0x20002DCC`, `_end 0x20002870`, `top 0x20002FF0`; 1920 B
physical gap). 548 + 128 = 676 ≪ 1792, so the floor was cut to `0x700` and the
plain production CH570 image now links with no `--defsym` override (RAM
10352/12272 B). The reading is from the encrypted-link paths only, not the
full matrix above; the ~1.24 KB of unused reserve absorbs the unmeasured
deep paths (fault frame, nested RF/USB/AES) with wide margin, but a full-matrix
re-measure is still the belt-and-braces before the final release build.

**CH592 result (2026-08-18):** measured **max_depth 516 B, slack 1308 B** of
the 1824 B true stack (`_susrstack 0x200060D0 .. _eusrstack 0x200067F0`),
steady across pair + provision + a 150 s / ~6.2k-frame encrypted soak with
forced reconnect. Resolves review #18 (the 1824 B budget was never measured):
ample headroom, not tight. NOTE the CH592 scan starts at `_susrstack`, not
`_end` -- an RF/BLE arena, the heap, and the retained fault record sit between
`_end` and the stack and are all written at boot, so a from-`_end` scan (the
CH570 shape) stopped at the fault record and reported a bogus ~4 KB depth.
`stack_watermark.h` picks the floor per chip via `RF_TASK_EXECUTOR_TMOS`.
