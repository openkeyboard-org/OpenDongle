# OpenDongle CH570D production firmware.

# Grouped targets (`a b &: prereqs`) need GNU Make 4.3. Older Make parses `&`
# as an ordinary target name and silently runs the recipe once per output, so
# the finalize and bundle steps would race instead of failing outright. Refuse
# up front rather than produce artifacts nobody can explain.
ifneq ($(shell printf '4.3\n%s\n' '$(MAKE_VERSION)' | sort -V -C && echo ok),ok)
  $(error GNU Make 4.3 or newer is required for grouped targets; this is $(MAKE_VERSION))
endif

SHELL := /bin/sh
.SHELLFLAGS := -eu -c
.DELETE_ON_ERROR:

SDK ?= $(abspath ../../third_party/openwch/ch570/EVT/EXAM)
MRS_TOOLCHAIN ?=
# MounRiver GCC15 renamed the tools: GCC12 shipped riscv-wch-elf-*, GCC15
# ships riscv32-wch-elf-*. One variable so the prefix lives in one place.
CROSS ?= riscv32-wch-elf
# OpenBoot builds with MounRiver GCC12, this application with GCC15. The
# factory image is two separately linked binaries that share no code, so mixing
# compilers is safe:
#   make ch570-factory MRS_TOOLCHAIN=<GCC15 bin> OPENBOOT_TOOLCHAIN=<GCC12 bin>
#
# This split is now PERMANENT, not a wait for OpenBoot's pin to catch up.
# OpenBoot auto-detects either tool prefix, so tool NAMES no longer force it -
# but GCC15 is explicitly unvalidated on ch57x and its own check went advisory,
# so the requirement is enforced by check-openboot-toolchain below.
OPENBOOT_TOOLCHAIN ?= $(MRS_TOOLCHAIN)
OPENBOOT_ALLOW_UNVALIDATED_GCC ?= 0
COMMON := ../common
OPENBOOT := $(abspath ../../third_party/openboot)
MINICHLINK ?= minichlink
WCHLINK_SERIAL ?=

CC := $(MRS_TOOLCHAIN)/$(CROSS)-gcc
AS := $(CC)
LD := $(CC)
OBJCOPY := $(MRS_TOOLCHAIN)/$(CROSS)-objcopy
SIZE := $(MRS_TOOLCHAIN)/$(CROSS)-size
PYTHON := /usr/bin/python3 -I

SDK_REVISION := 1e34c9450cd6df390153952c097cd09dd8a91aed
# The OpenBoot revision this build is pinned to. Declared rather than read from
# the submodule so it is a deterministic parse-time value, and asserted against
# the actual checkout by check-deps below. It also lands in CONFIG_TEXT, which
# is what finally puts the bootloader's identity into the build id - previously
# CONFIG_TEXT said only "boot=openboot-usb" and a locally modified bootloader
# composed into a factory image with every gate green.
OPENBOOT_REVISION := a3a492ccb0e1303731a8ce45e274d48c128e817e
PROFILE_ID := ch570-product
TARGET := opendongle-ch570-product

# OpenBoot's A/B slot geometry, read out of the pinned submodule rather than
# copied here. openboot_app.c derives its boot-record address as
# SLOT_BASE + SLOT_SIZE - erase block, so a local copy that drifted from the
# bootloader's own build would point the app's record read one slot away -
# silently, because nothing calls it yet. See ../openboot_geometry.py, which
# also cross-checks that the two slots are contiguous and fit the app region.
OPENBOOT_BOARD := opendongle-ch570
OPENBOOT_GEOM := $(shell $(PYTHON) ../openboot_geometry.py \
    --openboot "$(OPENBOOT)" --chip ch570 --transport usb \
    --board $(OPENBOOT_BOARD))
ifneq ($(words $(OPENBOOT_GEOM)),6)
$(error cannot read OpenBoot slot geometry - run: git submodule update --init --recursive)
endif
OPENBOOT_SLOT_A_BASE := $(word 1,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_SIZE := $(word 2,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_CAP := $(word 3,$(OPENBOOT_GEOM))
OPENBOOT_RECORD_SZ := $(word 4,$(OPENBOOT_GEOM))
OPENBOOT_SRC_BIN := $(word 5,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_B_BASE := $(word 6,$(OPENBOOT_GEOM))

# Which slot this build targets. OpenBoot alternates its write target after
# every COMMIT, so a release needs BOTH: an update is refused outright unless
# the image's base equals the device's current write_base. Objects are compiled
# per slot rather than linked twice from one set, because the slot base reaches
# the compiler - openboot_app.c derives its record address from it, and
# dongle_id.c stamps it into the ODG2 header.
SLOT ?= A
ifeq ($(SLOT),A)
OPENBOOT_SLOT_BASE := $(OPENBOOT_SLOT_A_BASE)
else ifeq ($(SLOT),B)
OPENBOOT_SLOT_BASE := $(OPENBOOT_SLOT_B_BASE)
else
$(error SLOT must be A or B, got '$(SLOT)')
endif

# Per-slot object and artifact tree. PROFILE_ID and TARGET stay slot-agnostic:
# the two builds are the same firmware at different link bases, and they are
# already distinguished by DONGLE_BUILD_ID, since CFLAGS (which carries
# -DOPENBOOT_SLOT_BASE) is hashed into it.
BUILD := build/$(PROFILE_ID)-slot$(SLOT)

# Factory composition is slot A ONLY, and that is not an arbitrary policy: the
# factory image places the application at slot A's base and carries slot A's
# boot record, so a slot-B-linked APP would be programmed at the wrong address
# with a record describing bytes that are not there. Left to run, SLOT=B here
# composes exactly that image before the size check rejects it with a message
# about a missing record, which is not the real problem. Refuse up front.
FACTORY_SLOT_B := 0
ifeq ($(SLOT),B)
FACTORY_SLOT_B := 1
endif

ARCH := -march=rv32imc_zba_zbb_zbc_zbs_xw -mabi=ilp32 -mcmodel=medany
ARCH += -msmall-data-limit=8 -mno-save-restore
CFLAGS := $(ARCH) -O2 -g -std=gnu99
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffunction-sections -fdata-sections -fno-common
CFLAGS += --param=highcode-gen-section-name=1
CFLAGS += -DCH570_SYSCLK_SOURCE=CLK_SOURCE_HSE_PLL_100MHz
CFLAGS += -DCH570_SYSCLK_HZ=100000000u
CFLAGS += -DDONGLE_IMAGE_HAS_IAP=1
# OpenBoot app companion (openboot_request_update / openboot_get_record).
# openboot_app.c #errors without the two slot defines; they must match the
# bootloader's own generated openboot_config.h, which is why they are derived
# above rather than written out. Both are per-slot: SLOT selects which.
CFLAGS += -DOPENBOOT_CHIP_CH57X
CFLAGS += -DOPENBOOT_SLOT_BASE=$(OPENBOOT_SLOT_BASE)
CFLAGS += -DOPENBOOT_SLOT_SIZE=$(OPENBOOT_SLOT_SIZE)
# Select the CH570 AES backend: DONGLE_AES_CH570_IMPL_{ASM_A,ASM_F,C}.
# Default (ASM_A) is set in src/hal_aes_ch570_impl.h; override here, e.g.
#   make AES_IMPL=DONGLE_AES_CH570_IMPL_ASM_F
# ASM_F additionally REQUIRES CORECFGR bit 3 -- see that header.
ifdef AES_IMPL
CFLAGS += -DDONGLE_AES_CH570_IMPL=$(AES_IMPL)
endif

LDFLAGS := $(ARCH) -T link.ld -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs
LDFLAGS += -L $(BUILD)          # so link.ld's `INCLUDE memory.ld` resolves
LDFLAGS += $(EXTRA_LDFLAGS)

INCLUDES := -I src -I $(COMMON)/include
INCLUDES += -I $(SDK)/RF/LIB
INCLUDES += -I $(SDK)/SRC/StdPeriphDriver/inc
INCLUDES += -I $(SDK)/SRC/RVMSIS -I $(SDK)/SRC/Startup
INCLUDES += -I $(OPENBOOT)/firmware/app

APP_SRC := src/main.c src/platform_ch570.c src/fault_record.c
APP_SRC += $(COMMON)/src/bond.c $(COMMON)/src/dongle_id.c
APP_SRC += $(COMMON)/src/rf_task.c src/hal_dispatch_ch570.c
APP_SRC += src/hal_rf_ch570.c src/hal_timing_ch570.c
APP_SRC += $(COMMON)/src/iap.c $(COMMON)/src/usb_descriptors.c
APP_SRC += $(COMMON)/src/usb_device.c src/hal_usb_ch570.c
APP_SRC += $(OPENBOOT)/firmware/app/openboot_app.c
# AES-128 forward cipher behind hal_aes.h. hal_aes_ch570.c supplies
# hal_aes_init/hal_aes_set_key for every backend; the block kernel comes
# from hal_aes_ch570_asm.S (ASM_A default, ASM_F alternate) or from
# aes_sw.c when DONGLE_AES_CH570_IMPL is ..._C. See hal_aes_ch570_impl.h.
APP_SRC += src/hal_aes_ch570.c $(COMMON)/src/aes_sw.c

DRV_SRC := $(SDK)/SRC/StdPeriphDriver/CH57x_clk.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH57x_sys.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH57x_flash.c
STARTUP_SRC := $(SDK)/SRC/Startup/startup_CH572.S
STARTUP_SRC += src/reset_handler_ch570.S src/fault_handler.S
ASM_SRC := src/hal_aes_ch570_asm.S

C_SRCS := $(APP_SRC) $(DRV_SRC)
S_SRCS := $(STARTUP_SRC) $(ASM_SRC)
C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS)))
S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(notdir $(S_SRCS)))
OBJS := $(C_OBJS) $(S_OBJS)
DEPS := $(OBJS:.o=.d)

SOURCE_BASENAMES := $(notdir $(C_SRCS) $(S_SRCS))
ifneq ($(words $(SOURCE_BASENAMES)),$(words $(sort $(SOURCE_BASENAMES))))
$(error duplicate source basename in the flattened object directory)
endif

VPATH := $(COMMON)/src:src:$(SDK)/SRC/StdPeriphDriver:$(SDK)/SRC/Startup
VPATH := $(VPATH):$(OPENBOOT)/firmware/app

# The app gets its slot's capacity - the slot minus the erase block holding
# that slot's boot record. Derived, for the same reason the -D flags are.
APP_MAX_BYTES := $(shell printf '%d' $$(($(OPENBOOT_SLOT_CAP))))

# link.ld's MEMORY block is GENERATED per slot into $(BUILD)/memory.ld and
# pulled in with INCLUDE, following the same pattern OpenBoot uses for its own
# link script. That retires the previous arrangement, where link.ld carried a
# literal LENGTH and the Makefile asserted it matched the derived capacity: the
# literal is gone, so there is nothing left to drift.
MEMORY_LD := $(BUILD)/memory.ld

# Release artifact: ONE file carrying both per-slot builds. OpenBoot picks the
# variant matching the device's current write_base, which alternates after every
# COMMIT - so shipping a bare .bin means being right about the slot by luck.
SLOT_A_BIN := build/$(PROFILE_ID)-slotA/$(TARGET).bin
SLOT_B_BIN := build/$(PROFILE_ID)-slotB/$(TARGET).bin
BUNDLE_BIN := build/$(PROFILE_ID).obb
OPENBOOT_CLI := $(OPENBOOT)/tools/target/release/openboot

FINALIZE_TOOL := ../finalize_image.py
CONFIG_STAMP := $(BUILD)/.config-stamp
BUILD_ID_STAMP := $(BUILD)/.build-id-stamp
BUILD_ID_TOOL := ../build_identity.py
# Composed by OpenBoot's own `make factory`, which also writes slot A's boot
# record into the image - so a blank part boots the app on first power-on.
OPENBOOT_FACTORY_BIN := $(patsubst %.bin,%-factory.bin,$(OPENBOOT_SRC_BIN))
OPENBOOT_MANIFEST := $(patsubst %.bin,%.manifest,$(OPENBOOT_SRC_BIN))
FACTORY_BIN := $(BUILD)/$(TARGET)-factory.bin
FACTORY_MANIFEST := $(BUILD)/$(TARGET)-factory.manifest
OPENBOOT_BIN := $(BUILD)/$(TARGET)-openboot.bin
RAW_ELF := $(BUILD)/$(TARGET)-unfinalized.elf

CONFIG_TEXT := schema=7||chip=ch570||profile=$(PROFILE_ID)||clock=100mhz||rf=1||usb=1||iap=1||boot=openboot-usb||openboot=$(OPENBOOT_REVISION)||fault=1||health=0||hw_poll_grid=1||rx_max=32||stack_floor=0x800||sdk=$(SDK_REVISION)||compiler_sha256=9527827d2004aaddfeb3ecac030d0a0ec19678e9601e3ffdb18f9a3100b9bd99
# $(ASM_SRC) belongs here too: without it, editing either AES kernel changes
# the firmware without changing its build identity (CodeRabbit/Codex review).
BUILD_ID_INPUTS := $(APP_SRC) $(STARTUP_SRC) $(ASM_SRC) $(wildcard $(COMMON)/include/*.h)
BUILD_ID_INPUTS += $(wildcard src/*.h) link.ld ../dongle_image_id.py
BUILD_ID_INPUTS += $(OPENBOOT)/firmware/app/openboot_app.h
BUILD_ID_INPUTS += $(OPENBOOT)/protocol/openboot_protocol.h $(FINALIZE_TOOL)
#	CFLAGS and LDFLAGS are hashed too. Without them an optimisation change or a
#	new -D produced a different binary under the SAME build id unless someone
#	remembered to bump CONFIG_TEXT by hand - and that id is what every "is the
#	running build the expected one?" check compares.
#
#	This must stay ABOVE the -DDONGLE_BUILD_ID line below, or the id would feed
#	its own input. Every codegen-affecting flag is already set by this point;
#	only the build-id define comes after.
BUILD_ID := $(shell $(PYTHON) $(BUILD_ID_TOOL) --config-text '$(CONFIG_TEXT)' \
    --flags 'CFLAGS=$(CFLAGS)' --flags 'LDFLAGS=$(LDFLAGS)' \
    $(BUILD_ID_INPUTS))
ifeq ($(BUILD_ID),)
$(error failed to calculate firmware build identity)
endif
CFLAGS += -DDONGLE_BUILD_PROFILE=1u -DDONGLE_BUILD_ID=0x$(BUILD_ID)u

.DEFAULT_GOAL := all
.PHONY: all openboot-image factory flash-factory bundle check-deps FORCE \
	check-openboot-toolchain size size-check clean

all: check-factory-slot check-deps $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin \
    $(FACTORY_BIN) size size-check

# The bootloader on its own, built from the pinned submodule with the product
# board file (which clamps OB_APP_END to 0x3A000, keeping OBP off the bond
# page). The factory image no longer goes through this - OpenBoot's own
# `factory` target builds and composes it - but a standalone bootloader binary
# is what the SWD recovery path writes, so it stays available on demand.
# Recursive make every pass (OpenBoot's build is incremental); the copy below
# is content-guarded so downstream targets do not rebuild unless bytes changed.
openboot-image: check-openboot-toolchain
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch570 TRANSPORT=usb \
	    BOARD=$(OPENBOOT_BOARD) MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

$(OPENBOOT_BIN): openboot-image | $(BUILD)
	@if ! cmp -s "$(OPENBOOT_SRC_BIN)" "$@"; then \
	  cp "$(OPENBOOT_SRC_BIN)" "$@"; echo "openboot image updated: $@"; fi

factory: check-factory-slot check-deps $(FACTORY_BIN) size-check
	@echo "factory image: $(abspath $(FACTORY_BIN))"

flash-factory: factory
	@command -v "$(MINICHLINK)" >/dev/null 2>&1 || { \
	  echo "minichlink not found: $(MINICHLINK)" >&2; exit 1; }
#	An empty WCHLINK_SERIAL lets minichlink pick a probe arbitrarily; on a
#	multi-probe bench that includes one driving a keyboard that must never
#	be written. Whole-chip erase demands an explicit target.
	@test -n "$(strip $(WCHLINK_SERIAL))" || { \
	  echo "set WCHLINK_SERIAL=<probe serial> - flash-factory refuses to guess the target" >&2; exit 1; }
	"$(MINICHLINK)" -kt -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)")
#	-E erases the entire user code flash (0x00000..0x3BFFF on CH570), which
#	covers both slots, slot A's boot record (0x1D000) and the bond page
#	(0x3A000), but not the ROM bootloader or option bytes. A factory install
#	SHOULD clear the bond: a unit must not inherit the pairing identity of a
#	test fixture. -E performs its own halt-and-reset, so no separate halt
#	flag is needed.
#
#	The erase no longer implies a bless step: the factory image carries slot
#	A's record, so the write below restores it along with the application.
	"$(MINICHLINK)" -E -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)")
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") \
	    -w "$(abspath $(FACTORY_BIN))" 0
#	minichlink ignores the return values of its CH5xx erase/write calls and its
#	byte-for-byte verify is disabled, so a zero exit status is NOT evidence the
#	image landed. Read it back and compare (the factory image's 0x00 pad
#	between OpenBoot and the app is programmed, so the compare covers the
#	whole file).
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") \
	    -r "$(BUILD)/.flash-readback.bin" 0 $$(stat -c%s "$(FACTORY_BIN)")
	cmp "$(BUILD)/.flash-readback.bin" "$(FACTORY_BIN)"
	@echo "factory image read back OK"
#	Finish with a REAL power cycle, not -b. "-b" reboots out of halt: it
#	resumes the core rather than resetting through the boot path, so the
#	boot decision does not re-run from a cold start. That matters on CH57x
#	specifically - errata F26 means the XIP view can serve stale data after a
#	controller write within the same power cycle, which is exactly why
#	OpenBoot's own BOOT command resets rather than jumping. Ending on -b
#	leaves the unit in a state that has not demonstrated the claim printed
#	below, and a bench part flashed that way did not come up on USB.
	"$(MINICHLINK)" -kt -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)")
	"$(MINICHLINK)" -k3 -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)")
	@echo "NOTE: this image is factory-blessed - it carries slot A's boot"
	@echo "      record - so the unit boots the application on first power-on."
	@echo "      No 'openboot bless' step, and none is possible: the device"
	@echo "      comes up active=A/write=B, and bless resolves against the"
	@echo "      write slot, so a slot-A image is refused there by design."

# check-openboot-toolchain is deliberately NOT a prerequisite here. check-deps
# gates every object build, so requiring GCC12 from it would make the
# application-only .elf/.bin targets unbuildable for anyone who is not
# composing a factory image - which is the opposite of the split's purpose.
# The gate belongs on the paths that actually invoke OPENBOOT_TOOLCHAIN:
# openboot-image and the factory rule.
check-factory-slot:
	@test "$(FACTORY_SLOT_B)" = 0 || { \
	  echo "factory and all compose slot A only, so SLOT=B cannot be used here." >&2; \
	  echo "Run make bundle to build both slots into one .obb instead." >&2; \
	  exit 1; }

check-deps:
	$(PYTHON) ../check_dependencies.py --sdk "$(SDK)" \
	    --sdk-revision $(SDK_REVISION) --toolchain "$(MRS_TOOLCHAIN)" \
	    --tool-prefix "$(CROSS)"
#	Assert the submodule IS the pinned revision and its worktree is clean.
#	--skip-toolchain because OpenBoot's own recursive build checks its
#	compiler; requiring GCC12 to be installed merely to verify a SHA would
#	make `make ch570` unbuildable for anyone not composing a factory image.
	$(PYTHON) $(OPENBOOT)/firmware/check_dependencies.py \
	    --expect-revision $(OPENBOOT_REVISION) --skip-toolchain

# OpenBoot's own compiler gate went ADVISORY at this pin: a compiler-hash
# mismatch and an unvalidated GCC major both warn() and continue. Combined with
# its new tool-prefix auto-detection, a routine `make ch570-factory` carrying a
# single GCC15 toolchain now silently ships a GCC15 bootloader.
#
# That is not hypothetical. Upstream pins SUPPORTED_GCC_MAJORS = (12,) on the
# strength of a CH572 bench where GCC15 fired the idle auto-boot at
# 1.51/1.71/4.75 s against a configured 10 s, and failed to return from 4 of 32
# software resets (GCC12: 0/32) - with the generated code for the timing
# functions instruction-identical between the two. Cause unresolved. A dongle
# that leaves the bootloader after 1.5 s instead of 10 does not look broken; it
# just loses its update window.
#
# So OpenDongle keeps a HARD gate where upstream now warns. Override only to
# reproduce that investigation:
#   make ... OPENBOOT_ALLOW_UNVALIDATED_GCC=1
check-openboot-toolchain:
	@if [ "$(OPENBOOT_ALLOW_UNVALIDATED_GCC)" = "1" ]; then \
	  echo "openboot toolchain: check bypassed (OPENBOOT_ALLOW_UNVALIDATED_GCC=1)"; \
	  exit 0; \
	fi; \
	gcc=""; \
	for p in riscv-wch-elf-gcc riscv32-wch-elf-gcc; do \
	  if [ -x "$(OPENBOOT_TOOLCHAIN)/$$p" ]; then \
	    gcc="$(OPENBOOT_TOOLCHAIN)/$$p"; break; \
	  fi; \
	done; \
	test -n "$$gcc" || { \
	  echo "no riscv-wch-elf-gcc or riscv32-wch-elf-gcc under OPENBOOT_TOOLCHAIN='$(OPENBOOT_TOOLCHAIN)'" >&2; \
	  exit 1; }; \
	major=$$("$$gcc" -dumpversion | cut -d. -f1); \
	test "$$major" = "12" || { \
	  echo "OpenBoot must be built with GCC 12; $$gcc is GCC $$major" >&2; \
	  echo "  GCC15 is not validated on ch57x - upstream benched erratic idle" >&2; \
	  echo "  auto-boot (1.5-4.8 s against a configured 10 s) and 4/32 failed" >&2; \
	  echo "  software-reset returns on CH572. Point OPENBOOT_TOOLCHAIN at a" >&2; \
	  echo "  GCC12 bin directory, or set OPENBOOT_ALLOW_UNVALIDATED_GCC=1." >&2; \
	  exit 1; }; \
	echo "openboot toolchain: GCC $$major ($$gcc)"

$(BUILD):
	mkdir -p "$@"

# The MEMORY block, generated per slot so link.ld holds no literal addresses.
# FORCE plus a content guard: it re-evaluates on every build (the geometry can
# move when the submodule pin does) but only rewrites the file when the bytes
# actually change, so an unchanged slot does not relink.
$(MEMORY_LD): FORCE | $(BUILD)
	@{ echo "/* Generated by ch570/Makefile for slot $(SLOT). DO NOT EDIT. */"; \
	   echo "MEMORY"; \
	   echo "{"; \
	   echo "    FLASH (rx)  : ORIGIN = $(OPENBOOT_SLOT_BASE), LENGTH = $(OPENBOOT_SLOT_CAP)"; \
	   echo "    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x2FF0"; \
	   echo "}"; \
	   echo "DONGLE_SLOT_A_BASE = $(OPENBOOT_SLOT_A_BASE);"; \
	   echo "DONGLE_SLOT_B_BASE = $(OPENBOOT_SLOT_B_BASE);"; \
	 } > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv -f "$@.tmp" "$@"; fi

FORCE:

$(CONFIG_STAMP): Makefile | $(BUILD)
	@tmp="$@.$$$$"; trap 'rm -f "$$tmp"' 0 1 2 15; \
	 printf '%s\n' '$(CONFIG_TEXT)' > "$$tmp"; \
	 if cmp -s "$$tmp" "$@" 2>/dev/null; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi; \
	 trap - 0 1 2 15

# Makefile is a prerequisite because CFLAGS and LDFLAGS are hashed into the
# build id but are DEFINED here. Without it, a flags-only edit moved the id
# while this stamp stayed up to date, so nothing recompiled: the objects, the
# ELF, the ODG2 record and the factory image all kept the old flags and the
# old id baked in, while the build system advertised the new one. The content
# guard below still blocks propagation when the id has not actually moved.
$(BUILD_ID_STAMP): $(BUILD_ID_INPUTS) $(BUILD_ID_TOOL) Makefile | $(BUILD)
	@tmp="$@.$$$$"; trap 'rm -f "$$tmp"' 0 1 2 15; \
	 printf '%s\n' '$(BUILD_ID)' > "$$tmp"; \
	 if cmp -s "$$tmp" "$@" 2>/dev/null; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi; \
	 trap - 0 1 2 15

$(OBJS): $(CONFIG_STAMP) $(BUILD_ID_STAMP) | check-deps

$(BUILD)/%.o: %.c | $(BUILD)
	"$(CC)" $(CFLAGS) $(INCLUDES) -MMD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o "$@"

$(BUILD)/%.o: %.S | $(BUILD)
	"$(AS)" $(CFLAGS) $(INCLUDES) -MMD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o "$@"

$(RAW_ELF): $(OBJS) $(SDK)/RF/LIB/libCH57xRF.a \
        $(SDK)/SRC/StdPeriphDriver/libISP572.a link.ld $(MEMORY_LD)
	"$(LD)" $(LDFLAGS) $(OBJS) $(SDK)/RF/LIB/libCH57xRF.a \
	    $(SDK)/SRC/StdPeriphDriver/libISP572.a -o "$@"

$(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin &: $(RAW_ELF) $(FINALIZE_TOOL) \
        ../dongle_image_id.py
	$(PYTHON) $(FINALIZE_TOOL) --objcopy "$(OBJCOPY)" \
	    --input-elf "$(RAW_ELF)" --output-elf "$(BUILD)/$(TARGET).elf" \
	    --output-bin "$(BUILD)/$(TARGET).bin"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	"$(OBJCOPY)" --pad-to 0x3000 -O ihex "$<" "$@"

# Factory composition lives in OpenBoot now. It is not a concatenation any
# more: the image is bootloader || 0x00 pad || app || zeros || slot A's OBR2
# boot record, so a blank part boots the application on FIRST POWER-ON with no
# host and no `openboot bless`. Reproducing that here would mean reimplementing
# the record layout and its CRC against a format that just changed.
#
# FACTORY_BLESS=1 is passed explicitly even though OpenBoot defaults it: if
# that default ever flips, an unblessed image looks identical until a unit
# reaches a customer and sits in the bootloader.
$(FACTORY_BIN) $(FACTORY_MANIFEST) &: $(BUILD)/$(TARGET).bin \
        | $(BUILD) check-openboot-toolchain
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch570 TRANSPORT=usb \
	    BOARD=$(OPENBOOT_BOARD) MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" \
	    FACTORY_BLESS=1 APP="$(abspath $(BUILD)/$(TARGET).bin)" factory
#	The manifest is written by the same OpenBoot build that produced these
#	bytes, so checking it here - rather than only in check-deps - is what
#	closes the parallel-make hole: under -j, check-deps can finish AFTER the
#	submodule was read.
	@rev=$$(sed -n 's/^openboot_revision=//p' "$(OPENBOOT_MANIFEST)"); \
	 dirty=$$(sed -n 's/^openboot_dirty=//p' "$(OPENBOOT_MANIFEST)"); \
	 test "$$rev" = "$(OPENBOOT_REVISION)" || { \
	   echo "OpenBoot built from $$rev, expected $(OPENBOOT_REVISION)" >&2; exit 1; }; \
	 test "$$dirty" = "0" || { \
	   echo "OpenBoot worktree is dirty; the image is not the pinned revision's bytes" >&2; \
	   exit 1; }
#	Size is a cheap, decisive check that the record is actually present:
#	bootloader region + slot capacity + one record. A short image means
#	FACTORY_BLESS did not take effect and the unit would not boot unattended.
	@want=$$(( $(OPENBOOT_SLOT_BASE) + $(OPENBOOT_SLOT_CAP) + $(OPENBOOT_RECORD_SZ) )); \
	 got=$$(stat -c%s "$(OPENBOOT_FACTORY_BIN)"); \
	 test "$$got" -eq "$$want" || { \
	   echo "factory image is $$got B, expected $$want B (blessed record missing?)" >&2; \
	   exit 1; }
	@cp "$(OPENBOOT_FACTORY_BIN)" "$(FACTORY_BIN)"
	@cp "$(OPENBOOT_MANIFEST)" "$(FACTORY_MANIFEST)"
	@echo "factory image: $$(stat -c%s "$(FACTORY_BIN)") B (blessed, slot A)"

size: $(BUILD)/$(TARGET).elf
	"$(SIZE)" "$<"

size-check: $(BUILD)/$(TARGET).bin
	@size=$$(stat -c%s "$<"); \
	 test "$$size" -le $(APP_MAX_BYTES) || { \
	   echo "firmware is $$size B, exceeds $(APP_MAX_BYTES) B" >&2; exit 1; }; \
	 echo "firmware: $$size / $(APP_MAX_BYTES) B"

# Both slots plus the bundle. Recursive because SLOT selects the object tree,
# the link base and the -D values, so the two builds cannot share a pass.
bundle:
	+$(MAKE) SLOT=A $(SLOT_A_BIN)
	+$(MAKE) SLOT=B $(SLOT_B_BIN)
	@test -x "$(OPENBOOT_CLI)" || { \
	  echo "build the OpenBoot CLI first: cargo build --release --manifest-path $(OPENBOOT)/tools/Cargo.toml" >&2; \
	  exit 1; }
	"$(OPENBOOT_CLI)" bundle create -o "$(BUNDLE_BIN)" --chip ch570 \
	    "$(SLOT_A_BIN)@$(OPENBOOT_SLOT_A_BASE)" \
	    "$(SLOT_B_BIN)@$(OPENBOOT_SLOT_B_BASE)"
	@echo "bundle: $(abspath $(BUNDLE_BIN)) ($$(stat -c%s "$(BUNDLE_BIN)") B)"

clean:
	rm -rf "$(abspath build)"

-include $(DEPS)
