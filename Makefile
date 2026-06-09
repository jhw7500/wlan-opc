# wlan-opc top-level build.
#
# Artifacts are kept per-architecture under build/<arch>/ so the native (host)
# and arm64 (target) builds coexist without clobbering each other:
#
#   build/native/{protocol,opcd,vhlctl}/   (cc / ar)
#   build/arm64/{protocol,opcd,vhlctl}/    (aarch64-linux-gnu-*)
#
# Targets:
#   make            # = arm64 (target-deploy cross build)
#   make arm64      # cross build  -> build/arm64
#   make native     # host build   -> build/native
#   make both       # arm64 + native, side by side
#   make check      # native build + host-side unit tests
#   make clean      # remove build/ entirely
#
# PLATFORM (stub|nxp) is orthogonal to ARCH and only selects opcd's platform
# backend (vhlctl is platform-independent). Default stub; cross-deploy uses
# `make PLATFORM=nxp` (= `make arm64 PLATFORM=nxp`).

ROOT := $(abspath $(CURDIR))

PLATFORM ?= stub

# Per-arch toolchains.
NATIVE_CC := cc
NATIVE_AR := ar
ARM64_CC  := aarch64-linux-gnu-gcc
ARM64_AR  := aarch64-linux-gnu-ar

.PHONY: all arm64 native both check clean

all: arm64

# Build every subdir for one arch into build/<arch>/.
#   $(1)=arch  $(2)=cc  $(3)=ar
define build_arch
	$(MAKE) -C protocol BUILDROOT=$(ROOT)/build/$(1) CC=$(2) AR=$(3)
	$(MAKE) -C opcd     BUILDROOT=$(ROOT)/build/$(1) CC=$(2) AR=$(3) PLATFORM=$(PLATFORM)
	$(MAKE) -C vhlctl   BUILDROOT=$(ROOT)/build/$(1) CC=$(2) AR=$(3)
endef

arm64:
	$(call build_arch,arm64,$(ARM64_CC),$(ARM64_AR))

native:
	$(call build_arch,native,$(NATIVE_CC),$(NATIVE_AR))

both: arm64 native

# Host-side codec round-trip + opcd/vhlctl unit tests. Always native (the
# binaries must run on the build machine) and always PLATFORM=stub so the
# handler test can link platform_stub.o regardless of the caller's PLATFORM.
check:
	$(MAKE) native PLATFORM=stub
	$(MAKE) -C protocol/tests BUILDROOT=$(ROOT)/build/native CC=$(NATIVE_CC) check
	$(MAKE) -C opcd/tests     BUILDROOT=$(ROOT)/build/native CC=$(NATIVE_CC) check
	$(MAKE) -C vhlctl/tests   BUILDROOT=$(ROOT)/build/native CC=$(NATIVE_CC) check

clean:
	rm -rf $(ROOT)/build
