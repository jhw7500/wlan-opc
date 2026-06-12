CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I../protocol
CFLAGS  += -pthread   # store_async worker thread (compile)

# Out-of-tree: objects + binary land under build/<arch>/opcd/, and link the
# protocol library from build/<arch>/protocol/. BUILDROOT is passed in by the
# top-level Makefile; defaults to native for standalone `make -C opcd`.
BUILDROOT ?= $(abspath ../build/native)
OBJDIR    := $(BUILDROOT)/opcd
PROTODIR  := $(BUILDROOT)/protocol

LDFLAGS += -L$(PROTODIR)
# -pthread at link too: some cross-toolchains (NXP Yocto aarch64) treat the
# CFLAGS -pthread as compile-only and need the link flag to pull in libpthread.
LDFLAGS += -pthread
LDLIBS  += -lopcproto

# Platform backend selection — exactly one platform_<PLATFORM>.c is linked.
# Override on the command line: `make PLATFORM=nxp`.
PLATFORM ?= stub

SRCS := opcd.c store.c store_async.c handler.c indication.c fault_probe.c \
        json_util.c inventory.c ntp_parse.c snapshot.c platform_$(PLATFORM).c
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
BIN  := $(OBJDIR)/opcd

.PHONY: all clean
all: $(BIN)

$(BIN): $(OBJS) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)
