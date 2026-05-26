SUBDIRS := protocol opcd vhlctl

CC  ?= aarch64-linux-gnu-gcc
AR  ?= aarch64-linux-gnu-ar
export CC AR

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

opcd vhlctl: protocol

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
