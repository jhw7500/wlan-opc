SUBDIRS := protocol opcd vhlctl

CC  = aarch64-linux-gnu-gcc
AR  = aarch64-linux-gnu-ar
export CC AR

# Default platform backend for opcd. Override at the command line with
# `make PLATFORM=nxp`. opcd's own Makefile defaults to stub when no value
# is forwarded.
PLATFORM ?= stub

.PHONY: all clean check $(SUBDIRS)

all: $(SUBDIRS)

opcd vhlctl: protocol

$(SUBDIRS):
	$(MAKE) -C $@ PLATFORM=$(PLATFORM)

# `make check` runs host-side codec round-trip tests with the native compiler
# so they can be invoked on the build machine without a target board.
# Also runs the platform-independent opcd unit tests (inventory loader,
# timesyncd parser).
check:
	$(MAKE) -C protocol CC=cc AR=ar
	$(MAKE) -C protocol/tests CC=cc check
	$(MAKE) -C opcd      CC=cc PLATFORM=stub inventory.o json_util.o ntp_parse.o \
	                          snapshot.o handler.o store.o indication.o platform_stub.o
	$(MAKE) -C opcd/tests CC=cc check
	$(MAKE) -C vhlctl    CC=cc fielddump.o
	$(MAKE) -C vhlctl/tests CC=cc check

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
	$(MAKE) -C protocol/tests clean
	$(MAKE) -C opcd/tests    clean
	$(MAKE) -C vhlctl/tests  clean
