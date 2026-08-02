# OpenDongle production firmware

This directory builds the two supported production images:

- `make ch570` builds the CH570D RF + USB HID + IAP image.
- `make ch592` builds the CH592F RF + USB HID + IAP image.
- `make ch570-openboot` or `make ch592-openboot` builds only the OpenBoot
  bootloader from the pinned submodule (product board files).
- `make ch570-factory` / `make ch592-factory` build one raw factory image:
  OpenBoot at flash address `0x0000`, `0x00` pad to `0x2000`, then the
  finalized application at `0x2000`.
- `make` or `make all` builds both images.
- `make check-deps` verifies the pinned SDKs and toolchain; `make test` runs the
  host unit tests (no toolchain, no hardware); `make clean` removes build output.

The dongle boots via the OpenBoot bootloader (`third_party/openboot`,
pinned submodule) and updates in-bootloader over its OBP USB protocol. See
[BOOT.md](BOOT.md) for the flash maps, boot-record semantics, and the
update/factory/recovery flows; OpenBoot's own design and wire protocol are
documented in the submodule.

## CH570D factory flashing

The single image produced by `make ch570-factory` is:

```text
ch570/build/ch570-product/opendongle-ch570-product-factory.bin
```

It is a raw code-flash image based at address zero. To build and program it
through the debug pin with a WCH-LinkE and `minichlink`, run:

```sh
make ch570-factory-flash \
  MINICHLINK=/path/to/minichlink \
  WCHLINK_SERIAL=probe-serial
```

`WCHLINK_SERIAL` is **required**: the target refuses to guess which probe to
drive, because a whole-chip erase aimed at the wrong target is unrecoverable and
a multi-probe bench may carry boards that must never be written. **Use the make target rather than raw `minichlink` calls.** minichlink ignores
the return values of its CH5xx erase and write calls and its byte-for-byte
verify is disabled, so a zero exit status alone does not prove the image
landed. The target exists because it does the two things a hand-typed sequence
does not: it selects the probe explicitly, and it reads the image back and
byte-compares it before reporting success.

What it does, and why each step matters:

1. `-kt` turns off the WCH-LinkE target supply.
2. `-E` erases the entire user code flash. **The erase is required, not
   optional.** It clears the bond page (`0x3A000`) so a unit cannot inherit a
   test fixture's pairing identity, and it clears the OpenBoot boot-record page
   (`0x3B000`) so the first boot deterministically lands in the bootloader.
3. The factory image is written at address 0, read back, and compared.
4. On CH592 only, the recipe additionally refuses to finish if a bond record
   survived in DataFlash, which `-E` cannot reach (`ALLOW_BONDED_FLASH=1`
   overrides this for a development unit).

Finish with `openboot bless <app.bin>` + `openboot boot` over USB: with the
record page erased, nothing attests the application yet (see BOOT.md).

For a target placed in the WCH ROM USB bootloader instead of using the debug
pin, the same raw image can be programmed with:

```sh
wchisp erase
wchisp flash \
  ch570/build/ch570-product/opendongle-ch570-product-factory.bin
```

The explicit `wchisp erase` matters for the same reason it does above:
`wchisp flash` erases only enough sectors to hold the image, so it would leave
the old boot-record page behind. Erasing makes the post-flash state
deterministic instead of dependent on whatever the unit held before.

Be precise about what a surviving record does and does not guarantee. It stores
a length and a CRC-32, and the bootloader recomputes that CRC over exactly that
many bytes from the app base — so it validates any image whose first `img_len`
bytes match the recorded ones, which in practice means re-flashing the same
build. It is not a whole-image identity check, and it does not by itself
guarantee the unit cannot launch an image other than the recorded one. Anything
that does not match that prefix fails the check and the unit stays in the
bootloader. `BOOT.md` states the rule in full.

The factory target enforces two invariants before exposing the image: the
application fits its chip's `APP_MAX_BYTES` window (`size-check`), and the
OpenBoot binary fits `[0, 0x2000)` so the application lands at exactly the
address it links at (`compose_factory.py`). The application's own ODG2 header is
written earlier, by `finalize_image.py` during the app build.

Two independent checksums are in play, and it is worth not confusing them.
`finalize_image.py` stamps a CRC inside the ODG2 header; that is a host-side
identity check, used by `opendongle` and by the build. OpenBoot never parses
ODG2. What it verifies at every boot is its own `img_crc32`, computed over the
committed bytes at bless time and stored out of band in the OBR1 boot record. A
raw OBP client can therefore bless and boot an image whose ODG2 CRC is invalid —
consistent with wrong-family protection also being host-side only (see
`BOOT.md`).

## Dependencies

The OpenWCH CH570/CH572 and CH592 SDKs are pinned Git submodules. Initialize
them once from the repository root:

```sh
git submodule update --init --recursive
```

MounRiver Studio's GCC 12 toolchain is an external dependency. It is not
redistributed or made a submodule because its packaging and license are
independent of the OpenWCH SDK repositories. Set `MRS_TOOLCHAIN` to the
directory containing `riscv-wch-elf-gcc`, `riscv-wch-elf-objcopy`, and
`riscv-wch-elf-size`:

```sh
make MRS_TOOLCHAIN=/path/to/MounRiver_Studio/toolchain/RISC-V_Embedded_GCC12/bin
```

`MRS_TOOLCHAIN`, `OPENWCH_ROOT`, `CH570_SDK`, and `CH592_SDK` use Make's `?=`
assignment, so they may be supplied by the environment, command line, or a
developer-local make wrapper. `make check-deps` verifies the SDK revisions,
clean SDK worktrees, compiler version, compiler digest, and required tools
before compilation.

### The toolchain pin: what it covers, and what it costs

`check_dependencies.py` pins the **`riscv-wch-elf-gcc` binary** by SHA-256, and
it is an order-only prerequisite of every object file — so the check is not
advisory: a mismatch stops the build.

That strictness is deliberate. The build id reported by `opendongle --info`
covers the compiler digest, so the id is only an honest answer to "is the
running build the expected one?" while something actually enforces that digest.
Two different builds of the same compiler version can produce different
binaries; without the pin they would share a build id.

Two honest limits, both worth knowing before you rely on the pin:

- **It covers the driver, not the whole toolchain.** `gcc` delegates to `cc1`,
  the assembler and the linker, and `objcopy` produces the released binary.
  None of those are hashed, and the version check only interrogates the driver.
  A mixed or partially replaced toolchain directory can therefore pass this
  gate and still produce different firmware under the same build id. Verifying
  a complete toolchain archive would close that gap.
- **It covers one platform's one release.** A contributor on a different host
  platform or a different MounRiver build is blocked until the pin is updated.
  If you hit that, the honest options are to obtain the pinned toolchain, or to
  change the pin here and accept that your artifacts are not the pinned bytes —
  in which case the build id no longer implies them.

Both are known and accepted for now rather than overlooked; a future change may
add an explicit opt-out that states the consequence at build time, and widen the
pin to the components that actually determine the output.

## Verifying a build

The build is reproducible: `build_identity.py` hashes the sources, the linker
script, the compiler and linker flag sets, and the pinned SDK/toolchain
revisions into a 32-bit build id that is compiled into the image and reported by
the host tool (`opendongle --info`). Two builds of the same tree on the same
host produce byte-identical artifacts, so a rebuild-and-compare is a meaningful
check:

```sh
make ch592 MRS_TOOLCHAIN=...
sha256sum ch592/build/ch592-product/opendongle-ch592-product.bin
make clean && make ch592 MRS_TOOLCHAIN=...
sha256sum ch592/build/ch592-product/opendongle-ch592-product.bin   # identical
```

**What that check does and does not establish.** It compares the *application*
image, and it is only as strong as the inputs the build id actually covers. Two
gaps are worth stating rather than leaving implied, both documented above and in
`TODO.md`:

- The toolchain pin covers the `riscv-wch-elf-gcc` driver, not `cc1`, the
  assembler, the linker or `objcopy`. A partially replaced toolchain directory
  can produce different bytes under the same build id, so "the same pinned
  toolchain" means the whole directory unchanged, not merely a matching driver
  digest.
- No OpenBoot revision appears in `CONFIG_TEXT` or `BUILD_ID_INPUTS`. The
  **factory** image is OpenBoot ‖ pad ‖ application, so its bytes depend on a
  checkout the build id says nothing about. Comparing factory images across
  hosts is only meaningful with `third_party/openboot` at the same commit and
  clean — which nothing currently enforces.

So: same tree, same complete toolchain, same OpenBoot checkout → identical
artifacts. A matching build id alone does not imply a matching factory image.

`make clean` removes generated output for both production targets.
