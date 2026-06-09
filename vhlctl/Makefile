CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I../protocol

# Out-of-tree: objects + binary land under build/<arch>/vhlctl/, and link the
# protocol library from build/<arch>/protocol/. BUILDROOT is passed in by the
# top-level Makefile; defaults to native for standalone `make -C vhlctl`.
BUILDROOT ?= $(abspath ../build/native)
OBJDIR    := $(BUILDROOT)/vhlctl
PROTODIR  := $(BUILDROOT)/protocol

LDFLAGS += -L$(PROTODIR)
LDLIBS  += -lopcproto

OBJS := $(addprefix $(OBJDIR)/,vhlctl.o fielddump.o)
BIN  := $(OBJDIR)/vhlctl

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
