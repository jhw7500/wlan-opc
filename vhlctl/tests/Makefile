CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I.. -I../../protocol

# Out-of-tree: test objects + binaries land under build/<arch>/vhlctl/tests/.
# Reuses fielddump.o already compiled into build/<arch>/vhlctl/ and the
# protocol library from build/<arch>/protocol/. Defaults to native.
BUILDROOT ?= $(abspath ../../build/native)
VHLOBJ    := $(BUILDROOT)/vhlctl
PROTODIR  := $(BUILDROOT)/protocol
TESTDIR   := $(BUILDROOT)/vhlctl/tests

LDFLAGS += -L$(PROTODIR)
LDLIBS  += -lopcproto

TESTS := test_fielddump
BINS  := $(addprefix $(TESTDIR)/,$(TESTS))

.PHONY: all check clean
all: $(BINS)

$(TESTDIR)/test_fielddump: $(TESTDIR)/test_fielddump.o $(VHLOBJ)/fielddump.o $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_fielddump.o $(VHLOBJ)/fielddump.o $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/%.o: %.c | $(TESTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTDIR):
	mkdir -p $(TESTDIR)

check: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; $$t || exit 1; done
	@echo "all vhlctl unit tests passed"

clean:
	rm -rf $(TESTDIR)
