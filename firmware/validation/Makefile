# OpenDongle AES hardware validation images.
#
# NOT part of `make all` or `make test`. These images are standalone diagnostics
# that own the whole part: flashing one ERASES OPENBOOT, and the device must be
# re-flashed with a factory image afterwards to be a dongle again.
#
# One source of truth per arm. `ARM` selects both the chip and the AES backend,
# because those two are not independent -- ch572-hw and ch592-hw run the same
# hardware driver on different silicon, and the three ch570 arms run different
# kernels on the same silicon. Naming the combination avoids the class of
# mistake where a run is labelled with a backend it was not built with.
#
#   make -C validation ARM=ch570-asm-a
#   make -C validation ARM=ch592-hw MRS_TOOLCHAIN=/path/to/gcc/bin
#
# Everything the runner needs comes back in build/$(ARM)/manifest.json.

SHELL := /bin/sh
.SHELLFLAGS := -eu -c
.DELETE_ON_ERROR:

ARMS := ch570-asm-a ch570-asm-f ch570-c ch572-hw ch572-asm-a ch592-hw
ARM ?= ch570-asm-a

COMMON := ../common
OPENWCH := $(abspath ../../third_party/openwch)
PYTHON ?= /usr/bin/python3 -I
MRS_TOOLCHAIN ?=
CROSS ?= riscv32-wch-elf

# ------------------------------------------------------------------- arm table
#
# BACKEND_ID values must match AES_LOG_BACKEND_* in aes_log_format.h; the
# harness logs the id and the reader names it, so a mismatch would mislabel an
# otherwise correct run.

ifeq ($(ARM),ch570-asm-a)
  CHIP := ch570
  AES_IMPL := DONGLE_AES_CH570_IMPL_ASM_A
  BACKEND_ID := 1
else ifeq ($(ARM),ch570-asm-f)
  CHIP := ch570
  AES_IMPL := DONGLE_AES_CH570_IMPL_ASM_F
  BACKEND_ID := 2
else ifeq ($(ARM),ch570-c)
  CHIP := ch570
  AES_IMPL := DONGLE_AES_CH570_IMPL_C
  BACKEND_ID := 3
else ifeq ($(ARM),ch572-hw)
  # CH572 is the same family as CH570 -- same SDK, same startup, same -march --
  # but unlike CH570 it HAS the hardware AES engine, at the same register block
  # CH592 uses. That is what makes it the control that proved CH570's engine is
  # absent rather than merely mis-driven.
  CHIP := ch570
  BACKEND_ID := 4
  USE_HW_AES := 1
else ifeq ($(ARM),ch572-asm-a)
  # The software kernel on the part that HAS the engine. Exists purely so the
  # software-vs-hardware comparison is same-silicon: comparing ASM_A on a CH570
  # against the engine on a CH572 compares two parts as much as two ciphers.
  CHIP := ch570
  AES_IMPL := DONGLE_AES_CH570_IMPL_ASM_A
  BACKEND_ID := 1
else ifeq ($(ARM),ch592-hw)
  CHIP := ch592
  BACKEND_ID := 4
  USE_HW_AES := 1
else
  $(error unknown ARM "$(ARM)"; choose one of: $(ARMS))
endif

# --------------------------------------------------------------- per-chip vars

ifeq ($(CHIP),ch570)
  SDK ?= $(OPENWCH)/ch570/EVT/EXAM
  SDK_REVISION := 1e34c9450cd6df390153952c097cd09dd8a91aed
  # Identical to firmware/ch570/Makefile. The xw extension and the bitmanip
  # subsets are what the hand-written kernels are compiled against; building
  # validation with anything else would not be validating what ships.
  ARCH := -march=rv32imc_zba_zbb_zbc_zbs_xw -mabi=ilp32 -mcmodel=medany
  ARCH += -msmall-data-limit=8 -mno-save-restore
  CLOCK_HZ := 100000000
  HAS_CORECFGR := 1
  # V3C SysTick: 32-bit CNT/CMP, no INIT bit.
  SYSTICK_64 := 0
  LINK := link-ch570.ld
  STARTUP_SRC := $(SDK)/SRC/Startup/startup_CH572.S
  STARTUP_SRC += ../ch570/src/reset_handler_ch570.S
  DRV_SRC := $(SDK)/SRC/StdPeriphDriver/CH57x_sys.c
  DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH57x_clk.c
  PLATFORM_SRC := validate_platform_ch570.c
  INCLUDES := -I . -I $(COMMON)/include -I ../ch570/src
  INCLUDES += -I $(SDK)/SRC/StdPeriphDriver/inc -I $(SDK)/SRC/RVMSIS
  INCLUDES += -I $(SDK)/SRC/Startup
else
  SDK ?= $(OPENWCH)/ch592/EVT/EXAM
  SDK_REVISION := a46e0086f1ffb5e5502703970bff94888e67f4cb
  # CH592's V4C core has neither the bitmanip subsets nor the xw extension that
  # the CH570 kernels use, so this arm gets its own -march. It runs the hardware
  # engine and never touches a software kernel.
  ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32
  ARCH += -msmall-data-limit=8 -mno-save-restore
  CLOCK_HZ := 60000000
  HAS_CORECFGR := 0
  # V4C SysTick: 64-bit CNT/CMP and an INIT bit the V3C parts lack.
  SYSTICK_64 := 1
  LINK := link-ch592.ld
  STARTUP_SRC := $(SDK)/SRC/Startup/startup_CH592.S
  DRV_SRC := $(SDK)/SRC/StdPeriphDriver/CH59x_sys.c
  DRV_SRC += $(SDK)/SRC/StdPeriphDriver/CH59x_clk.c
  PLATFORM_SRC := validate_platform_ch592.c
  INCLUDES := -I . -I $(COMMON)/include
  INCLUDES += -I $(SDK)/SRC/StdPeriphDriver/inc -I $(SDK)/SRC/RVMSIS
endif

# ------------------------------------------------------------------ AES backend

ASM_SRC :=
ifdef USE_HW_AES
  # The same driver on both chips: the register block CH592 uses is present and
  # identical on CH572.
  AES_SRC := ../ch592/src/hal_aes_ch592.c
else
  AES_SRC := ../ch570/src/hal_aes_ch570.c $(COMMON)/src/aes_sw.c
  ASM_SRC := ../ch570/src/hal_aes_ch570_asm.S
endif

C_SRCS := aes_validate.c $(PLATFORM_SRC) $(AES_SRC) $(DRV_SRC)
S_SRCS := $(STARTUP_SRC) $(ASM_SRC)

# ----------------------------------------------------------------- toolchain

ifeq ($(strip $(MRS_TOOLCHAIN)),)
  CC := $(CROSS)-gcc
  OBJCOPY := $(CROSS)-objcopy
  SIZE := $(CROSS)-size
  NM := $(CROSS)-nm
else
  CC := $(MRS_TOOLCHAIN)/$(CROSS)-gcc
  OBJCOPY := $(MRS_TOOLCHAIN)/$(CROSS)-objcopy
  SIZE := $(MRS_TOOLCHAIN)/$(CROSS)-size
  NM := $(MRS_TOOLCHAIN)/$(CROSS)-nm
endif

BUILD := build/$(ARM)
TARGET := aes-validate-$(ARM)
ELF := $(BUILD)/$(TARGET).elf
BIN := $(BUILD)/$(TARGET).bin
MANIFEST := $(BUILD)/manifest.json

CFLAGS := $(ARCH) -O2 -g -std=gnu99
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffunction-sections -fdata-sections -fno-common
CFLAGS += --param=highcode-gen-section-name=1
CFLAGS += -DDONGLE_VALIDATE_CLOCK_HZ=$(CLOCK_HZ)u
CFLAGS += -DDONGLE_VALIDATE_BACKEND=$(BACKEND_ID)u
CFLAGS += -DDONGLE_VALIDATE_HAS_CORECFGR=$(HAS_CORECFGR)
CFLAGS += -DDONGLE_VALIDATE_SYSTICK_64=$(SYSTICK_64)
ifdef AES_IMPL
CFLAGS += -DDONGLE_AES_CH570_IMPL=$(AES_IMPL)
endif
CFLAGS += $(INCLUDES)
# One-off experiment hook, e.g. make ARM=... EXTRA_CFLAGS=-DFOO. Above the
# build-id computation on purpose, so an experiment gets its own identity and
# the flash-verify gate can tell experimental images from standard ones.
CFLAGS += $(EXTRA_CFLAGS)

LDFLAGS := $(ARCH) -T $(LINK) -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs
# Nothing in the image references the seam except the harness, and the harness
# is main() -- but the log array is only ever read by a debug probe, so keep it.
LDFLAGS += -Wl,--undefined=aes_log

# ---------------------------------------------------------------- build identity
#
# Computed the same way production computes it, and for the same reason: the
# runner verifies the id it reads back from the device against the id of the
# image it meant to flash, BEFORE trusting a single result. Skipping exactly
# that check is what once turned one clean failure into two ambiguous ones.
#
# This must stay ABOVE the -D that injects it, or the id would feed its own input.
# The compiler is hashed in because it is part of what produced the binary:
# two toolchains generate different cipher code from identical sources, and
# without this both would log the SAME build id -- so a retained result from
# one could pass the identity gate for the other.
COMPILER_ID := $(shell "$(CC)" -dumpversion 2>/dev/null)-$(shell "$(CC)" -dumpmachine 2>/dev/null)
CONFIG_TEXT := schema=2||validation||arm=$(ARM)||chip=$(CHIP)||clock=$(CLOCK_HZ)||backend=$(BACKEND_ID)||corecfgr=$(HAS_CORECFGR)||sdk=$(SDK_REVISION)||cc=$(COMPILER_ID)
BUILD_ID_TOOL := ../build_identity.py
BUILD_ID_INPUTS := $(C_SRCS) $(S_SRCS) $(LINK) aes_log_format.h
BUILD_ID_INPUTS += $(wildcard $(COMMON)/include/*.h) $(wildcard ../ch570/src/*.h)
BUILD_ID := $(shell $(PYTHON) $(BUILD_ID_TOOL) --config-text '$(CONFIG_TEXT)' \
    --flags 'CFLAGS=$(CFLAGS)' --flags 'LDFLAGS=$(LDFLAGS)' \
    $(BUILD_ID_INPUTS))
ifeq ($(BUILD_ID),)
$(error failed to calculate validation build identity)
endif
CFLAGS += -DDONGLE_VALIDATE_BUILD_ID=0x$(BUILD_ID)u

# ---------------------------------------------------------------------- rules

OBJS := $(addprefix $(BUILD)/,$(notdir $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)))

# Objects depend on the exact flags they were built with, not just on the
# Makefile. A command-line override (`make ARM=... HAS_CORECFGR=0`) changes
# CFLAGS without touching any file, so without this the objects stay stale
# while BUILD_ID is recomputed from the new flags -- producing an image whose
# recorded identity does not match its own code. That is precisely the failure
# the build id exists to make impossible, and it was hit for real during
# development.
CONFIG_STAMP := $(BUILD)/.config-stamp
VPATH := .:../ch570/src:../ch592/src:$(COMMON)/src
VPATH := $(VPATH):$(SDK)/SRC/StdPeriphDriver:$(SDK)/SRC/Startup

.PHONY: all clean arms FORCE

FORCE:
.DEFAULT_GOAL := all

all: $(BIN) $(MANIFEST)
	@"$(SIZE)" $(ELF)

arms:
	@printf '%s\n' $(ARMS)

$(BUILD):
	mkdir -p $(BUILD)

# FORCE, not `Makefile`: the recipe has to RUN on every invocation to notice a
# command-line override, which changes no file at all. It rewrites the stamp
# only when the flag text actually differs, so mtime moves -- and objects
# rebuild -- exactly when the configuration changed and never otherwise.
$(CONFIG_STAMP): FORCE | $(BUILD)
	@tmp="$@.$$$$"; trap 'rm -f "$$tmp"' 0 1 2 15; \
	 printf '%s\n' 'CFLAGS=$(CFLAGS)' 'LDFLAGS=$(LDFLAGS)' \
	     'CONFIG=$(CONFIG_TEXT)' > "$$tmp"; \
	 if cmp -s "$$tmp" "$@" 2>/dev/null; then rm -f "$$tmp"; \
	 else mv -f "$$tmp" "$@"; fi; \
	 trap - 0 1 2 15

$(OBJS): $(CONFIG_STAMP)

$(BUILD)/%.o: %.c | $(BUILD)
	"$(CC)" $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	"$(CC)" $(CFLAGS) -c -o $@ $<

$(ELF): $(OBJS) $(LINK) | $(BUILD)
	"$(CC)" $(OBJS) $(LDFLAGS) -o $@

$(BIN): $(ELF)
	"$(OBJCOPY)" -O binary $< $@

# The runner reads this instead of re-deriving anything. `log_addr` is resolved
# from the ELF rather than hard-coded: aes_log is an ordinary .bss array
# precisely so it cannot collide with the linker's own allocation, which means
# only the link knows where it ended up.
$(MANIFEST): $(ELF)
	@set -e; \
	log_addr=$$("$(NM)" $(ELF) | awk '$$3 == "aes_log" { print $$1 }'); \
	saved_addr=$$("$(NM)" $(ELF) | awk '$$3 == "validate_saved" { print $$1 }'); \
	keep_addr=$$("$(NM)" $(ELF) | awk '$$3 == "validate_keep" { print $$1 }'); \
	if [ -z "$$log_addr" ]; then \
	    echo "aes_log symbol missing from $(ELF)" >&2; exit 1; \
	fi; \
	log_words=$$(grep -E '^#define[[:space:]]+AES_LOG_WORDS' aes_log_format.h \
	    | awk '{ print $$3 }'); \
	printf '{\n' > $@; \
	printf '  "arm": "%s",\n' '$(ARM)' >> $@; \
	printf '  "chip": "%s",\n' '$(CHIP)' >> $@; \
	printf '  "backend_id": %s,\n' '$(BACKEND_ID)' >> $@; \
	printf '  "clock_hz": %s,\n' '$(CLOCK_HZ)' >> $@; \
	printf '  "build_id": "%s",\n' '$(BUILD_ID)' >> $@; \
	printf '  "log_addr": "0x%s",\n' "$$log_addr" >> $@; \
	printf '  "log_words": %s,\n' "$$log_words" >> $@; \
	if [ -n "$$saved_addr" ]; then \
	    printf '  "saved_addr": "0x%s",\n' "$$saved_addr" >> $@; \
	else printf '  "saved_addr": null,\n' >> $@; fi; \
	if [ -n "$$keep_addr" ]; then \
	    printf '  "keep_addr": "0x%s",\n' "$$keep_addr" >> $@; \
	else printf '  "keep_addr": null,\n' >> $@; fi; \
	printf '  "elf": "%s",\n' '$(abspath $(ELF))' >> $@; \
	printf '  "bin": "%s"\n' '$(abspath $(BIN))' >> $@; \
	printf '}\n' >> $@; \
	echo "$(ARM): build id $(BUILD_ID), aes_log at 0x$$log_addr"

clean:
	rm -rf build
