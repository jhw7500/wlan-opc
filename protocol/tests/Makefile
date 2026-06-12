CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I..

# Out-of-tree: test objects + binaries land under build/<arch>/protocol/tests/,
# linking the library from build/<arch>/protocol/. Defaults to native.
BUILDROOT ?= $(abspath ../../build/native)
PROTODIR  := $(BUILDROOT)/protocol
TESTDIR   := $(BUILDROOT)/protocol/tests

LDFLAGS += -L$(PROTODIR)
LDLIBS  += -lopcproto

TESTS := test_codec
BINS  := $(addprefix $(TESTDIR)/,$(TESTS))

.PHONY: all check clean
all: $(BINS)

$(TESTDIR)/test_codec: $(TESTDIR)/test_codec.o $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_codec.o $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/%.o: %.c | $(TESTDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependency tracking (#40) — each .d sits next to its test .o.
-include $(BINS:%=%.d)

$(TESTDIR):
	mkdir -p $(TESTDIR)

check: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; $$t || exit 1; done
	@echo "all tests passed"

clean:
	rm -rf $(TESTDIR)
