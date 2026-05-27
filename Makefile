SUBDIRS := protocol opcd vhlctl

CC  = aarch64-linux-gnu-gcc
AR  = aarch64-linux-gnu-ar
export CC AR

.PHONY: all clean check $(SUBDIRS)

all: $(SUBDIRS)

opcd vhlctl: protocol

$(SUBDIRS):
	$(MAKE) -C $@

# `make check` runs host-side codec round-trip tests with the native compiler
# so they can be invoked on the build machine without a target board.
check:
	$(MAKE) -C protocol CC=cc AR=ar
	$(MAKE) -C protocol/tests CC=cc check

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
	$(MAKE) -C protocol/tests clean
