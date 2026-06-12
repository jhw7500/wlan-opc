CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I.. -I../../protocol
CFLAGS  += -pthread   # store_async worker thread (compile)

# Out-of-tree: test objects + binaries land under build/<arch>/opcd/tests/.
# Reuses the opcd objects already compiled into build/<arch>/opcd/ (so the
# tests link against the host build without dragging in platform_nxp.c) and
# the protocol library from build/<arch>/protocol/. Defaults to native.
BUILDROOT ?= $(abspath ../../build/native)
OPCDOBJ   := $(BUILDROOT)/opcd
PROTODIR  := $(BUILDROOT)/protocol
TESTDIR   := $(BUILDROOT)/opcd/tests

LDFLAGS += -L$(PROTODIR)
# Link flag too — cross-toolchains may treat the CFLAGS -pthread as
# compile-only. Applied to every test link line (all include $(LDFLAGS)).
LDFLAGS += -pthread
LDLIBS  += -lopcproto

OPCD_OBJS_INVENTORY := $(OPCDOBJ)/inventory.o $(OPCDOBJ)/json_util.o
OPCD_OBJS_NTP       := $(OPCDOBJ)/ntp_parse.o
OPCD_OBJS_JSON      := $(OPCDOBJ)/json_util.o
OPCD_OBJS_SNAPSHOT  := $(OPCDOBJ)/snapshot.o
# test_handler drives opcd_dispatch end-to-end, so it links the handler and
# everything it depends on, with the host-only stub platform backend.
OPCD_OBJS_HANDLER   := $(OPCDOBJ)/handler.o $(OPCDOBJ)/store.o $(OPCDOBJ)/store_async.o \
                       $(OPCDOBJ)/indication.o $(OPCDOBJ)/fault_probe.o \
                       $(OPCDOBJ)/inventory.o $(OPCDOBJ)/json_util.o $(OPCDOBJ)/snapshot.o \
                       $(OPCDOBJ)/platform_stub.o
OPCD_OBJS_STORE_ASYNC := $(OPCDOBJ)/store.o $(OPCDOBJ)/store_async.o
OPCD_OBJS_FAULT     := $(OPCDOBJ)/fault_probe.o

TESTS := test_inventory test_ntp_parse test_json_util test_snapshot test_handler \
         test_store_async test_fault_probe
BINS  := $(addprefix $(TESTDIR)/,$(TESTS))

.PHONY: all check clean
all: $(BINS)

$(TESTDIR)/test_inventory: $(TESTDIR)/test_inventory.o $(OPCD_OBJS_INVENTORY) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_inventory.o $(OPCD_OBJS_INVENTORY) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_ntp_parse: $(TESTDIR)/test_ntp_parse.o $(OPCD_OBJS_NTP)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_ntp_parse.o $(OPCD_OBJS_NTP)

$(TESTDIR)/test_json_util: $(TESTDIR)/test_json_util.o $(OPCD_OBJS_JSON)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_json_util.o $(OPCD_OBJS_JSON)

$(TESTDIR)/test_snapshot: $(TESTDIR)/test_snapshot.o $(OPCD_OBJS_SNAPSHOT) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_snapshot.o $(OPCD_OBJS_SNAPSHOT) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_handler: $(TESTDIR)/test_handler.o $(OPCD_OBJS_HANDLER) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_handler.o $(OPCD_OBJS_HANDLER) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_store_async: $(TESTDIR)/test_store_async.o $(OPCD_OBJS_STORE_ASYNC)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_store_async.o $(OPCD_OBJS_STORE_ASYNC) $(LDFLAGS)

$(TESTDIR)/test_fault_probe: $(TESTDIR)/test_fault_probe.o $(OPCD_OBJS_FAULT)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_fault_probe.o $(OPCD_OBJS_FAULT)

$(TESTDIR)/%.o: %.c | $(TESTDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependency tracking (#40) — each .d sits next to its test .o. The
# reused $(OPCDOBJ)/*.o are covered by opcd/Makefile's own .d includes (the
# top-level `check` always runs `make native` first).
-include $(BINS:%=%.d)

$(TESTDIR):
	mkdir -p $(TESTDIR)

check: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; $$t || exit 1; done
	@echo "all opcd unit tests passed"

clean:
	rm -rf $(TESTDIR)
