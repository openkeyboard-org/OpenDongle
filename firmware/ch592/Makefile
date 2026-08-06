# OpenDongle CH592F production firmware.

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

SDK ?= $(abspath ../../third_party/openwch/ch592/EVT/EXAM)
MRS_TOOLCHAIN ?=
# MounRiver GCC15 renamed the tools: GCC12 shipped riscv-wch-elf-*, GCC15
# ships riscv32-wch-elf-*. One variable so the prefix lives in one place.
CROSS ?= riscv32-wch-elf
# OpenBoot builds with MounRiver GCC12, this application with GCC15. The
# factory image is two separately linked binaries that share no code, so mixing
# compilers is safe:
#   make ch592-factory MRS_TOOLCHAIN=<GCC15 bin> OPENBOOT_TOOLCHAIN=<GCC12 bin>
#
# This split is now PERMANENT, not a wait for OpenBoot's pin to catch up.
# OpenBoot auto-detects either tool prefix, so tool NAMES no longer force it -
# but GCC15 is explicitly unvalidated and its own check went advisory, so the
# requirement is enforced by check-openboot-toolchain below.
OPENBOOT_TOOLCHAIN ?= $(MRS_TOOLCHAIN)
OPENBOOT_ALLOW_UNVALIDATED_GCC ?= 0
COMMON := ../common
BLE_LIB_DIR := $(SDK)/BLE/LIB
OPENBOOT := $(abspath ../../third_party/openboot)

CC := $(MRS_TOOLCHAIN)/$(CROSS)-gcc
AS := $(CC)
LD := $(CC)
OBJCOPY := $(MRS_TOOLCHAIN)/$(CROSS)-objcopy
SIZE := $(MRS_TOOLCHAIN)/$(CROSS)-size
MINICHLINK ?= minichlink
WCHLINK_SERIAL ?=

PYTHON := /usr/bin/python3 -I

SDK_REVISION := a46e0086f1ffb5e5502703970bff94888e67f4cb
# The OpenBoot revision this build is pinned to. Declared rather than read from
# the submodule so it is a deterministic parse-time value, and asserted against
# the actual checkout by check-deps below. It also lands in CONFIG_TEXT, which
# is what finally puts the bootloader's identity into the build id - previously
# CONFIG_TEXT said only "boot=openboot-usb" and a locally modified bootloader
# composed into a factory image with every gate green.
OPENBOOT_REVISION := a3a492ccb0e1303731a8ce45e274d48c128e817e
PROFILE_ID := ch592-product
TARGET := opendongle-ch592-product
# Per-slot object and artifact tree. PROFILE_ID and TARGET stay slot-agnostic:
# the two builds are the same firmware at different link bases, and they are
# already distinguished by DONGLE_BUILD_ID, since CFLAGS (which carries
# -DOPENBOOT_SLOT_BASE) is hashed into it.
BUILD := build/$(PROFILE_ID)-slot$(SLOT)

# OpenBoot's A/B slot geometry, read out of the pinned submodule rather than
# copied here. openboot_app.c derives its boot-record address as
# SLOT_BASE + SLOT_SIZE - erase block, so a local copy that drifted from the
# bootloader's own build would point the app's record read one slot away -
# silently, because nothing calls it yet. See ../openboot_geometry.py, which
# also cross-checks that the two slots are contiguous and fit the app region.
OPENBOOT_BOARD := opendongle-ch592
OPENBOOT_GEOM := $(shell $(PYTHON) ../openboot_geometry.py \
    --openboot "$(OPENBOOT)" --chip ch592 --transport usb \
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

ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32
ARCH += -msmall-data-limit=8 -mno-save-restore
CFLAGS := $(ARCH) -O2 -g -std=gnu99
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffunction-sections -fdata-sections -fno-common
CFLAGS += --param=highcode-gen-section-name=1
CFLAGS += -DCLK_OSC32K=1
CFLAGS += -DDONGLE_IMAGE_HAS_IAP=1
# BLE SNV must stay OFF: its default address is BLE_SNV_ADDR =
# 0x77000-FLASH_ROM_MAX_SIZE, i.e. DataFlash offset 0x7000 - the SAME block as
# the stage-1 transaction state (CH592_IAP_STATE_EEPROM_OFF).
# CH59x_BLEInit -> BLE_LibInit -> tmos_snv_init reads the first word there and,
# whenever it is not the SNV marker, rewrites the SNV blocks through
# Lib_Write_Flash (an EEPROM erase followed by a write). An OSW2 record starts
# 0x3257534F, so any application boot reached with a live record destroyed it.
# Bench-confirmed: a normally-running dongle held 00 70 00 00 ff ff ... at
# 0x77000 - the SNV marker written as its own magic, not an erased page.
#
# Ordinary updates were unaffected, since stage-1 runs first and consumes the
# record before the application starts. What was lost is a record stage-1 leaves
# armed ON PURPOSE for a later boot - most clearly the path where the START
# marker will not persist, which boots the untouched old image and keeps the
# transaction so a later boot can retry it. A cleanup erase that failed would
# strand one the same way, and SNV hid that too.
#
# With BLE_SNV=FALSE the cfg.SNVAddr/readFlashCB/writeFlashCB assignments are
# compiled out of CH59x_BLEInit - as are Lib_Read_Flash and Lib_Write_Flash,
# which live under the same guard - so tmos_snv_init returns immediately on
# SNVAddr == 0 and nothing in the application writes that page.
#
# BOTH collisions above are now HISTORICAL, not live. The stage-1 OSW2 state
# went away with the OpenBoot cutover, and OpenBoot's boot record moved out of
# DataFlash into the top erase block of each code-flash slot, so nothing of
# ours sits at logical 0x7000 any more - all 32 KiB of DataFlash is the
# application's again. The bench evidence above is kept because it is what
# established that the library really does rewrite that page unbidden.
#
# The guard stays for the reasons that survive: this product uses raw
# RF_Rx/RF_Tx and never BLE bonding, so SNV has no purpose here; the flag is
# hashed into the build id and the whole CH592 matrix was validated with it
# off; and re-enabling would put unaudited library writes into the DataFlash
# that still holds the RF bond. If BLE features are ever enabled they must be
# given their own DataFlash page.
CFLAGS += -DBLE_SNV=FALSE
# OpenBoot app companion (openboot_request_update / openboot_get_record).
# openboot_app.c #errors without the two slot defines; they must match the
# bootloader's own generated openboot_config.h, which is why they are derived
# above rather than written out. Phase 1 builds slot A only.
CFLAGS += -DOPENBOOT_CHIP_CH59X
CFLAGS += -DOPENBOOT_SLOT_BASE=$(OPENBOOT_SLOT_BASE)
CFLAGS += -DOPENBOOT_SLOT_SIZE=$(OPENBOOT_SLOT_SIZE)

LDFLAGS := $(ARCH) -T link.ld -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs
LDFLAGS += -L $(BUILD)          # so link.ld's `INCLUDE memory.ld` resolves

INCLUDES := -I src -I $(COMMON)/include -I $(BLE_LIB_DIR)
INCLUDES += -I $(SDK)/BLE/HAL/include
INCLUDES += -I $(SDK)/SRC/StdPeriphDriver/inc -I $(SDK)/SRC/RVMSIS
INCLUDES += -I $(OPENBOOT)/firmware/app

APP_SRC := src/main.c $(COMMON)/src/rf_task.c
APP_SRC += src/hal_timing_ch592.c src/hal_rf_ch592.c src/platform_ch592.c
APP_SRC += $(COMMON)/src/bond.c $(COMMON)/src/dongle_id.c
APP_SRC += $(COMMON)/src/usb_device.c src/hal_usb_ch592.c
APP_SRC += $(COMMON)/src/usb_descriptors.c $(COMMON)/src/iap.c
APP_SRC += $(OPENBOOT)/firmware/app/openboot_app.c
# AES-128 forward cipher behind hal_aes.h. CH592 has a hardware engine, so this
# is the whole backend -- no software cipher is linked here, unlike CH570.
# Listing it in APP_SRC also puts it in BUILD_ID_INPUTS (which expands
# $(APP_SRC) below), so editing the driver moves the build id. It was in
# neither before, meaning the file was dead code that no build compiled.
APP_SRC += src/hal_aes_ch592.c

HAL_SRC := $(SDK)/BLE/HAL/MCU.c $(SDK)/BLE/HAL/RTC.c
DRV_SRC := $(SDK)/SRC/StdPeriphDriver/CH59x_clk.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH59x_sys.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH59x_flash.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH59x_adc.c
STARTUP_SRC := src/startup_CH592_phased.S
STARTUP_SRC += $(BLE_LIB_DIR)/ble_task_scheduler.S
STARTUP_SRC += src/bb_irq_trampoline.S src/fault_handler.S

C_SRCS := $(APP_SRC) $(HAL_SRC) $(DRV_SRC)
S_SRCS := $(STARTUP_SRC)
C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS)))
S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(notdir $(S_SRCS)))
OBJS := $(C_OBJS) $(S_OBJS)
DEPS := $(OBJS:.o=.d)

SOURCE_BASENAMES := $(notdir $(C_SRCS) $(S_SRCS))
ifneq ($(words $(SOURCE_BASENAMES)),$(words $(sort $(SOURCE_BASENAMES))))
$(error duplicate source basename in the flattened object directory)
endif

VPATH := $(COMMON)/src:src:$(SDK)/BLE/HAL:$(SDK)/SRC/StdPeriphDriver:$(BLE_LIB_DIR)
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

CONFIG_TEXT := schema=9||chip=ch592||profile=$(PROFILE_ID)||clock=60mhz||osc32k=lsi||rf=1||usb=1||iap=1||boot=openboot-usb||openboot=$(OPENBOOT_REVISION)||fault=1||pm_idle=0||ble_snv=0||access_addr=0xAC1649CE||sdk=$(SDK_REVISION)||compiler_sha256=9527827d2004aaddfeb3ecac030d0a0ec19678e9601e3ffdb18f9a3100b9bd99
BUILD_ID_INPUTS := $(APP_SRC) $(STARTUP_SRC) $(wildcard $(COMMON)/include/*.h)
BUILD_ID_INPUTS += $(wildcard src/*.h) link.ld ../dongle_image_id.py
BUILD_ID_INPUTS += $(OPENBOOT)/firmware/app/openboot_app.h
BUILD_ID_INPUTS += $(OPENBOOT)/protocol/openboot_protocol.h $(FINALIZE_TOOL)
#	CFLAGS and LDFLAGS are hashed too. Without them an optimisation change or a
#	new -D produced a different binary under the SAME build id unless someone
#	remembered to bump CONFIG_TEXT by hand - which is exactly what -DBLE_SNV=FALSE
#	above depended on. That id is what every "is the running build the expected
#	one?" check compares.
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
.PHONY: all openboot-image factory flash-factory check-deps FORCE \
	check-openboot-toolchain size size-check clean

all: check-deps $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin \
    $(FACTORY_BIN) size size-check

# The bootloader on its own, built from the pinned submodule with the product
# board file. The factory image no longer goes through this - OpenBoot's own
# `factory` target builds and composes it - but a standalone bootloader binary
# is what the SWD recovery path writes, so it stays available on demand.
# Recursive make every pass (OpenBoot's build is incremental); the copy below
# is content-guarded so downstream targets do not rebuild unless bytes changed.
openboot-image: check-openboot-toolchain
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=usb \
	    BOARD=$(OPENBOOT_BOARD) MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

$(OPENBOOT_BIN): openboot-image | $(BUILD)
	@if ! cmp -s "$(OPENBOOT_SRC_BIN)" "$@"; then \
	  cp "$(OPENBOOT_SRC_BIN)" "$@"; echo "openboot image updated: $@"; fi

factory: check-deps $(FACTORY_BIN) size-check
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
#	-E erases the whole user CODE flash. It does NOT touch DataFlash:
#	ch5xx.c CH5xxErase() takes its whole-chip extent from flash_offset/flash_size,
#	not from eeprom_offset/eeprom_size.
#
#	The boot record is no longer among the survivors. It moved out of
#	DataFlash into the top erase block of each code-flash slot with the A/B
#	change, so -E DOES clear boot state on this part now - the earlier
#	"a stale record either still validates or the device stays in the
#	bootloader" analysis no longer applies, and the write below restores
#	slot A's record from the factory image anyway.
#
#	What still survives -E is the bond at 0x75000, which is why the
#	ALLOW_BONDED_FLASH guard below is unchanged. Never write DataFlash over
#	SWD - bench-confirmed, minichlink leaves garbage there.
	"$(MINICHLINK)" -E -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)")
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") \
	    -w "$(abspath $(FACTORY_BIN))" 0
#	minichlink ignores the return values of its CH5xx erase/write calls and its
#	byte-for-byte verify is disabled, so a zero exit status is NOT evidence the
#	image landed. Read it back and compare (the factory image's 0x00 pad between
#	OpenBoot and the app is programmed, so the compare covers the whole file).
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") \
	    -r "$(BUILD)/.flash-readback.bin" 0 $$(stat -c%s "$(FACTORY_BIN)")
	cmp "$(BUILD)/.flash-readback.bin" "$(FACTORY_BIN)"
#	The BOND survives -E, and cannot be cleared here.
#	CH570's recipe erases it deliberately - "a unit must not inherit the pairing
#	identity of a test fixture" - and CH592 cannot honour that with -E alone.
#
#	FAIL by default: a shipped unit must not carry a test fixture's pairing
#	identity, and CH592 cannot clear it from the probe, so the safe default is to
#	stop and make the operator clear it over IAP. Re-flashing a bonded DEVELOPMENT
#	unit is legitimate; that is the explicit ALLOW_BONDED_FLASH=1 opt-out, which
#	downgrades this to a warning. The test below compares against exactly 1
#	rather than testing for a non-empty value: ALLOW_BONDED_FLASH=0 arriving
#	from a script must fail safe, and -n would have taken the override branch
#	and flashed a unit still carrying a test fixture's pairing identity.
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") \
	    -r "$(BUILD)/.flash-bond.bin" 0x75000 4
	@if head -c 4 "$(BUILD)/.flash-bond.bin" | od -An -tx1 | grep -qi "42 4f 4e 44"; then \
	  if [ "$(strip $(ALLOW_BONDED_FLASH))" = "1" ]; then \
	    echo "WARNING: a bond record survives at 0x75000 - -E does not reach CH592" >&2; \
	    echo "         DataFlash. ALLOW_BONDED_FLASH is set, so continuing; clear it" >&2; \
	    echo "         over IAP before shipping this unit." >&2; \
	  else \
	    echo "ERROR: a bond record survives at 0x75000 - -E does not reach CH592" >&2; \
	    echo "       DataFlash, so this unit still carries a pairing identity. Clear" >&2; \
	    echo "       it over IAP (bond-clear command 0x89), or pass" >&2; \
	    echo "       ALLOW_BONDED_FLASH=1 to reflash a development unit deliberately." >&2; \
	    exit 1; \
	  fi; fi
	@echo "factory image read back OK"
#	A REAL power cycle, not -b. "-b" reboots out of halt: it resumes the core
#	rather than resetting through the boot path, so the boot decision does not
#	re-run from a cold start and the unit has not actually demonstrated the
#	claim printed below. CH59x has no F26 stale-XIP errata, so the stakes are
#	lower here than on CH57x - but a factory install whose whole promise is
#	"boots unattended on first power-on" should end by doing exactly that.
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
check-deps:
	$(PYTHON) ../check_dependencies.py --sdk "$(SDK)" \
	    --sdk-revision $(SDK_REVISION) --toolchain "$(MRS_TOOLCHAIN)" \
	    --tool-prefix "$(CROSS)"
#	Assert the submodule IS the pinned revision and its worktree is clean.
#	--skip-toolchain because OpenBoot's own recursive build checks its
#	compiler; requiring GCC12 to be installed merely to verify a SHA would
#	make `make ch592` unbuildable for anyone not composing a factory image.
	$(PYTHON) $(OPENBOOT)/firmware/check_dependencies.py \
	    --expect-revision $(OPENBOOT_REVISION) --skip-toolchain

# OpenBoot's own compiler gate went ADVISORY at this pin: a compiler-hash
# mismatch and an unvalidated GCC major both warn() and continue. Combined with
# its new tool-prefix auto-detection, a routine `make ch592-factory` carrying a
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
	  echo "  GCC15 is not validated - upstream benched erratic idle auto-boot" >&2; \
	  echo "  (1.5-4.8 s against a configured 10 s) and 4/32 failed" >&2; \
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
	@{ echo "/* Generated by ch592/Makefile for slot $(SLOT). DO NOT EDIT. */"; \
	   echo "MEMORY"; \
	   echo "{"; \
	   echo "    FLASH (rx)  : ORIGIN = $(OPENBOOT_SLOT_BASE), LENGTH = $(OPENBOOT_SLOT_CAP)"; \
	   echo "    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x67F0"; \
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

$(RAW_ELF): $(OBJS) $(BLE_LIB_DIR)/LIBCH59xBLE.a \
        $(SDK)/SRC/StdPeriphDriver/libISP592.a link.ld $(MEMORY_LD)
	"$(LD)" $(LDFLAGS) $(OBJS) $(BLE_LIB_DIR)/LIBCH59xBLE.a \
	    $(SDK)/SRC/StdPeriphDriver/libISP592.a -o "$@"

$(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin &: $(RAW_ELF) $(FINALIZE_TOOL) \
        ../dongle_image_id.py
	$(PYTHON) $(FINALIZE_TOOL) --objcopy "$(OBJCOPY)" \
	    --input-elf "$(RAW_ELF)" --output-elf "$(BUILD)/$(TARGET).elf" \
	    --output-bin "$(BUILD)/$(TARGET).bin"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	"$(OBJCOPY)" -O ihex "$<" "$@"

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
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=usb \
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

clean:
	rm -rf "$(abspath build)"

-include $(DEPS)
