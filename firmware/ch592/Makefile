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
# OpenBoot is a pinned submodule with its OWN toolchain pin, still on MounRiver
# GCC12 and its riscv-wch-elf-* tool names. The factory image is
# OpenBoot || pad || application -- two separately linked binaries that share no
# code -- so building them with different compilers is safe. Until OpenBoot's
# pin moves, point this at the GCC12 bin directory:
#   make ch592-factory MRS_TOOLCHAIN=<GCC15 bin> OPENBOOT_TOOLCHAIN=<GCC12 bin>
OPENBOOT_TOOLCHAIN ?= $(MRS_TOOLCHAIN)
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
PROFILE_ID := ch592-product
TARGET := opendongle-ch592-product
BUILD := build/$(PROFILE_ID)

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
# SNVAddr == 0 and nothing in the application writes that page. Under OpenBoot
# the SAME DataFlash block (logical 0x7000) holds the OBR1 boot record, so the
# guard matters even more than it did for the OSW2 state it originally
# protected. This product uses raw RF_Rx/RF_Tx and never BLE bonding, so SNV
# has no other purpose here. If BLE features are ever enabled they must be
# given their own DataFlash page, not this one.
CFLAGS += -DBLE_SNV=FALSE
# OpenBoot app companion (openboot_request_update / openboot_get_record).
CFLAGS += -DOPENBOOT_CHIP_CH59X

LDFLAGS := $(ARCH) -T link.ld -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs

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

# App region under OpenBoot: [0x2000, 0x70000), matching link.ld and the
# manifest's CHIP_LAYOUTS.
APP_MAX_BYTES := 450560
COMPOSE_TOOL := ../compose_factory.py
FINALIZE_TOOL := ../finalize_image.py
CONFIG_STAMP := $(BUILD)/.config-stamp
BUILD_ID_STAMP := $(BUILD)/.build-id-stamp
BUILD_ID_TOOL := ../build_identity.py
FACTORY_BIN := $(BUILD)/$(TARGET)-factory.bin
OPENBOOT_BIN := $(BUILD)/$(TARGET)-openboot.bin
OPENBOOT_SRC_BIN := $(OPENBOOT)/firmware/build/ch592-usb+opendongle-ch592/openboot-ch592-usb.bin
RAW_ELF := $(BUILD)/$(TARGET)-unfinalized.elf

CONFIG_TEXT := schema=8||chip=ch592||profile=$(PROFILE_ID)||clock=60mhz||osc32k=lsi||rf=1||usb=1||iap=1||boot=openboot-usb||fault=1||pm_idle=0||ble_snv=0||access_addr=0xAC1649CE||sdk=$(SDK_REVISION)||compiler_sha256=9527827d2004aaddfeb3ecac030d0a0ec19678e9601e3ffdb18f9a3100b9bd99
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
.PHONY: all openboot-image factory flash-factory check-deps size size-check clean

all: check-deps $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin \
    $(FACTORY_BIN) size size-check

# OpenBoot bootloader image, built from the pinned submodule with the
# product board file. Recursive make every pass (OpenBoot's own build is
# incremental); the copy below is content-guarded so downstream targets do
# not rebuild unless the bytes changed.
openboot-image:
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=usb \
	    BOARD=opendongle-ch592 MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

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
#	not from eeprom_offset/eeprom_size, so the OpenBoot boot record at 0x77000
#	and the bond at 0x75000 both survive. A survivor record is HARMLESS here
#	(unlike the old OSW2 state): OpenBoot CRC-checks the record's recorded
#	length of app bytes at every boot (OB_BOOT_IMAGE_CRC), so a stale record
#	either still validates (only possible when the flashed bytes contain the
#	previously blessed image as a prefix - re-flashing the identical build is
#	the normal case) or mismatches and the device stays in the bootloader
#	until `openboot bless` re-attests. Never write DataFlash over SWD -
#	bench-confirmed, minichlink leaves garbage there.
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
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") -b
	@echo "NOTE: first boot lands in OpenBoot unless a stale record matches"
	@echo "      these exact app bytes. If the device enumerates as 1209:0001,"
	@echo "      run: openboot bless $(abspath $(BUILD)/$(TARGET).bin) && openboot boot"

check-deps:
	$(PYTHON) ../check_dependencies.py --sdk "$(SDK)" \
	    --sdk-revision $(SDK_REVISION) --toolchain "$(MRS_TOOLCHAIN)" \
	    --tool-prefix "$(CROSS)"

$(BUILD):
	mkdir -p "$@"

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
        $(SDK)/SRC/StdPeriphDriver/libISP592.a link.ld
	"$(LD)" $(LDFLAGS) $(OBJS) $(BLE_LIB_DIR)/LIBCH59xBLE.a \
	    $(SDK)/SRC/StdPeriphDriver/libISP592.a -o "$@"

$(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin &: $(RAW_ELF) $(FINALIZE_TOOL) \
        ../dongle_image_id.py
	$(PYTHON) $(FINALIZE_TOOL) --objcopy "$(OBJCOPY)" \
	    --input-elf "$(RAW_ELF)" --output-elf "$(BUILD)/$(TARGET).elf" \
	    --output-bin "$(BUILD)/$(TARGET).bin"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	"$(OBJCOPY)" -O ihex "$<" "$@"

$(FACTORY_BIN): $(OPENBOOT_BIN) $(BUILD)/$(TARGET).bin $(COMPOSE_TOOL)
	$(PYTHON) $(COMPOSE_TOOL) --openboot "$(OPENBOOT_BIN)" \
	    --app "$(BUILD)/$(TARGET).bin" --output "$@"

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
