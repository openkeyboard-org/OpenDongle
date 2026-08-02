# OpenDongle production firmware entry point.

SHELL := /bin/sh
.SHELLFLAGS := -eu -c

MRS_TOOLCHAIN ?=
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
	check-deps test clean

all: ch570 ch592

ch570:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" all

ch592:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" all

ch570-factory:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" factory

ch592-factory:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" factory

ch570-factory-flash:
	$(MAKE) -C ch570 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH570_SDK)" \
	    MINICHLINK="$(MINICHLINK)" WCHLINK_SERIAL="$(WCHLINK_SERIAL)" \
	    flash-factory

ch592-factory-flash:
	$(MAKE) -C ch592 \
	    MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" SDK="$(CH592_SDK)" \
	    MINICHLINK="$(MINICHLINK)" WCHLINK_SERIAL="$(WCHLINK_SERIAL)" \
	    flash-factory

# OpenBoot bootloader images, built from the pinned submodule with the
# product board files (OpenBoot/firmware/boards/opendongle-*.mk). OpenBoot
# carries its own nested openwch submodules, pinned to the same revisions as
# ours; init with --recursive.
#
# What is NOT checked, so nobody has to discover it from a build: `check-deps`
# validates the two openwch SDKs and the compiler, and OpenBoot's own recursive
# build validates ITS openwch submodules - but nothing validates the OpenBoot
# checkout itself. Its revision is not compared against this repository's
# gitlink, its worktree is not checked for local edits, and no OpenBoot revision
# appears in CONFIG_TEXT or BUILD_ID_INPUTS. So a factory image can be composed
# from an unpinned or locally modified bootloader while every gate passes, and
# the build id will not record it. The application-side companion headers ARE
# hashed; the bootloader image is not. Tracked in TODO.md.
ch570-openboot:
	$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch570 TRANSPORT=usb \
	    BOARD=opendongle-ch570 MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" image

ch592-openboot:
	$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=usb \
	    BOARD=opendongle-ch592 MRS_TOOLCHAIN="$(MRS_TOOLCHAIN)" image

# OpenBoot's own host suites. Not part of `test`: run this when the
# submodule pin advances — the pin only moves onto OpenBoot commits whose
# full suite is green.
openboot-test:
	$(MAKE) -C $(OPENBOOT)/firmware test
	cargo test --manifest-path $(OPENBOOT)/tools/Cargo.toml

check-deps:
	$(PYTHON) check_dependencies.py \
	    --sdk "$(CH570_SDK)" \
	    --sdk-revision 1e34c9450cd6df390153952c097cd09dd8a91aed \
	    --toolchain "$(MRS_TOOLCHAIN)"
	$(PYTHON) check_dependencies.py \
	    --sdk "$(CH592_SDK)" \
	    --sdk-revision a46e0086f1ffb5e5502703970bff94888e67f4cb \
	    --toolchain "$(MRS_TOOLCHAIN)"

# Host-side suite: no cross toolchain, no hardware. Covers the build-identity
# contract and the factory composition. (OpenBoot's own host suite is separate
# - run it with `make openboot-test` when the submodule pin moves.)
test:
	$(PYTHON) -m unittest discover -s tests

clean:
	$(MAKE) -C ch570 clean
	$(MAKE) -C ch592 clean
