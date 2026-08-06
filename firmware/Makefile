# OpenDongle production firmware entry point.

SHELL := /bin/sh
.SHELLFLAGS := -eu -c

MRS_TOOLCHAIN ?=
# OpenBoot is a pinned submodule with its OWN toolchain pin, still on MounRiver
# GCC12 and its riscv-wch-elf-* tool names. Every target that reaches into the
# submodule has to route through this, not MRS_TOOLCHAIN, or it is unbuildable
# under a GCC15 pin with no way to override short of changing MRS_TOOLCHAIN
# itself. The chip Makefiles declare the same default for the same reason.
OPENBOOT_TOOLCHAIN ?= $(MRS_TOOLCHAIN)
# The application tool prefix. GCC12 shipped riscv-wch-elf-*, GCC15 ships
# riscv32-wch-elf-*. Passed to check_dependencies.py below so it validates the
# prefix the build actually uses rather than a hardcoded one.
CROSS ?= riscv32-wch-elf
OPENWCH_ROOT ?= $(abspath ../third_party/openwch)
CH570_SDK ?= $(OPENWCH_ROOT)/ch570/EVT/EXAM
CH592_SDK ?= $(OPENWCH_ROOT)/ch592/EVT/EXAM
OPENBOOT ?= $(abspath ../third_party/openboot)
MINICHLINK ?= minichlink
WCHLINK_SERIAL ?=
# The -I is load-bearing, not decoration: the build tools insert their own
# directory onto sys.path deliberately (see finalize_image.py), which only
# behaves predictably when the interpreter starts isolated from the invoking
# user's site-packages and PYTHONPATH. Override the interpreter if you must,
# but keep -I, or a stray module on the host can change what the build imports.
PYTHON ?= /usr/bin/python3 -I

.DEFAULT_GOAL := all

.PHONY: all ch570 ch592 ch570-factory ch592-factory \
	ch570-factory-flash ch592-factory-flash \
	ch570-openboot ch592-openboot openboot-test \
	ch570-bundle ch592-bundle bundles \
	aes-hw-build aes-hw-validate \
	check-deps test clean

all: ch570 ch592

ch570:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" all

ch592:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" all

ch570-factory:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" factory

ch592-factory:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" factory

# Release artifacts: one .obb per chip carrying BOTH per-slot builds. OpenBoot
# alternates its write target after every COMMIT and refuses an image whose base
# is not the current write_base, so a bare .bin is only ever right for one slot
# by luck. `openboot flash app.obb` picks the matching variant.
ch570-bundle:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" bundle

ch592-bundle:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" bundle

bundles: ch570-bundle ch592-bundle

# OPENBOOT_TOOLCHAIN must be forwarded here too: flash-factory builds the
# factory image, which reaches into the submodule. Without it the variable
# falls back to MRS_TOOLCHAIN (GCC15) and check-openboot-toolchain refuses.
ch570-factory-flash:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" \
	    MINICHLINK="$(MINICHLINK)" WCHLINK_SERIAL="$(WCHLINK_SERIAL)" \
	    flash-factory

ch592-factory-flash:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" \
	    OPENBOOT_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" \
	    MINICHLINK="$(MINICHLINK)" WCHLINK_SERIAL="$(WCHLINK_SERIAL)" \
	    flash-factory

# OpenBoot bootloader images, built from the pinned submodule with the
# product board files (OpenBoot/firmware/boards/opendongle-*.mk). OpenBoot
# carries its own nested openwch submodules, pinned to the same revisions as
# ours; init with --recursive.
#
# The OpenBoot checkout IS validated now, in three places, because one was not
# enough:
#   - each chip's check-deps runs OpenBoot's check_dependencies.py
#     --expect-revision, which fails on a wrong revision OR a dirty worktree;
#   - OPENBOOT_REVISION is in CONFIG_TEXT, so the bootloader's identity is part
#     of the application's build id rather than the bare literal
#     "boot=openboot-usb" that used to stand in for it;
#   - the factory rule re-checks the revision and dirty flag from the .manifest
#     OpenBoot emits beside the image. That last one is not redundant: under -j,
#     check-deps can finish AFTER the submodule was read, and the manifest is
#     written by the same build that produced the bytes being packaged.
#
# What is still NOT checked: the bootloader BINARY is not hashed into
# BUILD_ID_INPUTS. Deliberately - the manifest does not exist at parse time on
# a clean checkout, and it carries a sha256 that depends on which GCC12 install
# built OpenBoot, which would make DONGLE_BUILD_ID differ between developers
# for byte-identical application images. That id must identify the app.
ch570-openboot:
	$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch570 TRANSPORT=usb \
	    BOARD=opendongle-ch570 MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

ch592-openboot:
	$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=usb \
	    BOARD=opendongle-ch592 MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

# OpenBoot's own host suites. Not part of `test`: run this when the
# submodule pin advances — the pin only moves onto OpenBoot commits whose
# full suite is green.
#
# This is deliberately a SUPERSET of OpenBoot's own CI, not a subset. Its
# workflow runs fmt and clippy as a separate job from the test job; a pin bump
# taken from a green branch can still carry lint debt that only shows up the
# next time someone builds the CLI with -D warnings. The pin gate is the right
# place to catch that, since the pin is the thing being accepted.
#
# --expect-revision is per-chip (it needs OPENBOOT_REVISION, which the chip
# Makefiles declare), so it runs from check-deps rather than here.
openboot-test:
	$(MAKE) -C $(OPENBOOT)/firmware test
	cargo fmt --manifest-path $(OPENBOOT)/tools/Cargo.toml --check
	cargo clippy --manifest-path $(OPENBOOT)/tools/Cargo.toml \
	    --all-targets -- -D warnings
	cargo test --manifest-path $(OPENBOOT)/tools/Cargo.toml

check-deps:
	$(PYTHON) check_dependencies.py \
	    --sdk "$(CH570_SDK)" \
	    --sdk-revision 1e34c9450cd6df390153952c097cd09dd8a91aed \
	    --toolchain "$(MRS_TOOLCHAIN)" --tool-prefix "$(CROSS)"
	$(PYTHON) check_dependencies.py \
	    --sdk "$(CH592_SDK)" \
	    --sdk-revision a46e0086f1ffb5e5502703970bff94888e67f4cb \
	    --toolchain "$(MRS_TOOLCHAIN)" --tool-prefix "$(CROSS)"

# Host-side suite: no cross toolchain, no hardware. Covers the build-identity
# contract and the factory composition. (OpenBoot's own host suite is separate
# - run it with `make openboot-test` when the submodule pin moves.)
test:
	$(PYTHON) -m unittest discover -s tests

# AES backend validation images, built but not run. Useful on its own: it is
# what proves the ASM_F guard still fires, since that arm is EXPECTED to fail
# to compile while CORECFGR bit 3 is clear. Needs the cross toolchain, so it is
# not part of `test`.
#	A failed arm is only acceptable when it failed for the ONE expected reason:
#	the ASM_F CORECFGR guard firing. Anything else is a real build break. An
#	earlier version treated every non-zero exit as the expected refusal, which
#	would have let a genuinely broken arm pass this target silently. The guard
#	is identified by its own #error text rather than by arm name, so the day
#	CORECFGR bit 3 is set and ASM_F starts building, a failure there becomes a
#	failure again with no change here.
aes-hw-build:
	@set -e; fail=0; \
	for arm in $$($(MAKE) -s -C validation arms); do \
	    printf '\n=== %s ===\n' "$$arm"; \
	    log="$$(mktemp)"; \
	    if $(MAKE) -C validation ARM="$$arm" MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" \
	           >"$$log" 2>&1; then \
	        tail -3 "$$log"; \
	    elif grep -q 'requires ROM_LOOP_ACC' "$$log"; then \
	        echo "$$arm: build refused by the CORECFGR guard, as expected"; \
	    else \
	        cat "$$log"; echo "$$arm: BUILD FAILED" >&2; fail=1; \
	    fi; \
	    rm -f "$$log"; \
	done; \
	if [ $$fail -ne 0 ]; then \
	    echo "one or more validation arms failed to build" >&2; exit 1; \
	fi

# Hardware AES validation across CH570/CH572/CH592. NOT part of `test`: it
# flashes and drives real targets, and the image it writes is standalone, so it
# ERASES OPENBOOT on whatever it touches. Requires an explicit probe serial per
# chip - it refuses to guess, for the same reason flash-factory does, and it
# will not write at all without AES_HW_ARGS containing --confirm-erase 1.
#
#   make aes-hw-validate AES_HW_CH570=<serial> AES_HW_ARGS="--confirm-erase 1"
#
# Add --dry-run to AES_HW_ARGS to build every arm and print the probe commands
# without contacting a device.
aes-hw-validate:
	$(PYTHON) validation/run_aes_validation.py \
	    --minichlink "$(MINICHLINK)" --toolchain "$(MRS_TOOLCHAIN)" \
	    --ch570-probe "$(AES_HW_CH570)" \
	    --ch572-probe "$(AES_HW_CH572)" \
	    --ch592-probe "$(AES_HW_CH592)" \
	    $(AES_HW_ARGS)

clean:
	$(MAKE) -C ch570 clean
	$(MAKE) -C ch592 clean
	$(MAKE) -C validation clean
