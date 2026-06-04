CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I.. -I../../protocol
LDFLAGS += -L../../protocol
LDLIBS  += -lopcproto

# Object files from the opcd source tree, recompiled for the host so the
# tests can link against them without dragging in platform_nxp.c (which is
# target-specific).
OPCD_OBJS_INVENTORY := ../inventory.o ../json_util.o
OPCD_OBJS_NTP       := ../ntp_parse.o
OPCD_OBJS_JSON      := ../json_util.o
OPCD_OBJS_SNAPSHOT  := ../snapshot.o
# test_handler drives opcd_dispatch end-to-end, so it links the handler and
# everything it depends on, with the host-only stub platform backend.
OPCD_OBJS_HANDLER   := ../handler.o ../store.o ../indication.o ../inventory.o \
                       ../json_util.o ../snapshot.o ../platform_stub.o

TESTS := test_inventory test_ntp_parse test_json_util test_snapshot test_handler

all: $(TESTS)

test_inventory: test_inventory.o $(OPCD_OBJS_INVENTORY) ../../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ test_inventory.o $(OPCD_OBJS_INVENTORY) $(LDFLAGS) $(LDLIBS)

test_ntp_parse: test_ntp_parse.o $(OPCD_OBJS_NTP)
	$(CC) $(CFLAGS) -o $@ test_ntp_parse.o $(OPCD_OBJS_NTP)

test_json_util: test_json_util.o $(OPCD_OBJS_JSON)
	$(CC) $(CFLAGS) -o $@ test_json_util.o $(OPCD_OBJS_JSON)

test_snapshot: test_snapshot.o $(OPCD_OBJS_SNAPSHOT) ../../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ test_snapshot.o $(OPCD_OBJS_SNAPSHOT) $(LDFLAGS) $(LDLIBS)

test_handler: test_handler.o $(OPCD_OBJS_HANDLER) ../../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ test_handler.o $(OPCD_OBJS_HANDLER) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: check clean
check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
	@echo "all opcd unit tests passed"

clean:
	rm -f *.o $(TESTS)
