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
COMMON := ../common
OPENBOOT := $(abspath ../../third_party/openboot)
MINICHLINK ?= minichlink
WCHLINK_SERIAL ?=

CC := $(MRS_TOOLCHAIN)/riscv-wch-elf-gcc
AS := $(CC)
LD := $(CC)
OBJCOPY := $(MRS_TOOLCHAIN)/riscv-wch-elf-objcopy
SIZE := $(MRS_TOOLCHAIN)/riscv-wch-elf-size
PYTHON := /usr/bin/python3 -I

SDK_REVISION := 1e34c9450cd6df390153952c097cd09dd8a91aed
PROFILE_ID := ch570-product
TARGET := opendongle-ch570-product
BUILD := build/$(PROFILE_ID)

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
CFLAGS += -DOPENBOOT_CHIP_CH57X

LDFLAGS := $(ARCH) -T link.ld -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs

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

DRV_SRC := $(SDK)/SRC/StdPeriphDriver/CH57x_clk.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH57x_sys.c
DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH57x_flash.c
STARTUP_SRC := $(SDK)/SRC/Startup/startup_CH572.S
STARTUP_SRC += src/reset_handler_ch570.S src/fault_handler.S

C_SRCS := $(APP_SRC) $(DRV_SRC)
S_SRCS := $(STARTUP_SRC)
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

# App region under OpenBoot: [0x2000, 0x3A000) - the bond page sits at
# 0x3A000 and the OpenBoot boot record at 0x3B000, both outside the region
# (the OpenBoot board file clamps APP_END to the same bound).
APP_MAX_BYTES := 229376
COMPOSE_TOOL := ../compose_factory.py
FINALIZE_TOOL := ../finalize_image.py
CONFIG_STAMP := $(BUILD)/.config-stamp
BUILD_ID_STAMP := $(BUILD)/.build-id-stamp
BUILD_ID_TOOL := ../build_identity.py
FACTORY_BIN := $(BUILD)/$(TARGET)-factory.bin
OPENBOOT_BIN := $(BUILD)/$(TARGET)-openboot.bin
OPENBOOT_SRC_BIN := $(OPENBOOT)/firmware/build/ch570-usb+opendongle-ch570/openboot-ch570-usb.bin
RAW_ELF := $(BUILD)/$(TARGET)-unfinalized.elf

CONFIG_TEXT := schema=6||chip=ch570||profile=$(PROFILE_ID)||clock=100mhz||rf=1||usb=1||iap=1||boot=openboot-usb||fault=1||health=0||hw_poll_grid=1||rx_max=32||stack_floor=0x800||sdk=$(SDK_REVISION)||compiler_sha256=7f2d3c114b98fe9e48ac6abe6259a4574291a8e2aba960b21dce73528ece9ff2
BUILD_ID_INPUTS := $(APP_SRC) $(STARTUP_SRC) $(wildcard $(COMMON)/include/*.h)
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
.PHONY: all openboot-image factory flash-factory check-deps size size-check clean

all: check-deps $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin \
    $(FACTORY_BIN) size size-check

# OpenBoot bootloader image, built from the pinned submodule with the
# product board file (which clamps APP_END to 0x3A000). Recursive make every
# pass (OpenBoot's own build is incremental); the copy below is content-
# guarded so downstream targets do not rebuild unless the bytes changed.
openboot-image:
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch570 TRANSPORT=usb \
	    BOARD=opendongle-ch570 MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" image

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
#	-E erases the entire user code flash (0x00000..0x3BFFF on CH570), which
#	covers the app region, the bond page (0x3A000) and the OpenBoot boot
#	record page (0x3B000), but not the ROM bootloader or option bytes. A
#	factory install SHOULD clear the bond: a unit must not inherit the
#	pairing identity of a test fixture. Erasing the record page makes the
#	first boot land in OpenBoot deterministically (no record -> no launch),
#	so the bless step below is always required on CH570. -E performs its own
#	halt-and-reset, so no separate halt flag is needed.
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
	"$(MINICHLINK)" -C linke \
	    $(if $(strip $(WCHLINK_SERIAL)),-l "$(WCHLINK_SERIAL)") -b
	@echo "NOTE: the record page was erased, so this unit is now sitting in"
	@echo "      OpenBoot (1209:0001). To launch the app:"
	@echo "      openboot bless $(abspath $(BUILD)/$(TARGET).bin) && openboot boot"

check-deps:
	$(PYTHON) ../check_dependencies.py --sdk "$(SDK)" \
	    --sdk-revision $(SDK_REVISION) --toolchain "$(MRS_TOOLCHAIN)"

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

$(RAW_ELF): $(OBJS) $(SDK)/RF/LIB/libCH57xRF.a \
        $(SDK)/SRC/StdPeriphDriver/libISP572.a link.ld
	"$(LD)" $(LDFLAGS) $(OBJS) $(SDK)/RF/LIB/libCH57xRF.a \
	    $(SDK)/SRC/StdPeriphDriver/libISP572.a -o "$@"

$(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin &: $(RAW_ELF) $(FINALIZE_TOOL) \
        ../dongle_image_id.py
	$(PYTHON) $(FINALIZE_TOOL) --objcopy "$(OBJCOPY)" \
	    --input-elf "$(RAW_ELF)" --output-elf "$(BUILD)/$(TARGET).elf" \
	    --output-bin "$(BUILD)/$(TARGET).bin"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	"$(OBJCOPY)" --pad-to 0x3000 -O ihex "$<" "$@"

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
