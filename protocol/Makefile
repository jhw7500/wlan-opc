CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -fPIC
CFLAGS  += -I.

# Out-of-tree: objects + library land under build/<arch>/protocol/, passed in
# by the top-level Makefile via BUILDROOT. Defaults to native for standalone
# `make -C protocol`.
BUILDROOT ?= $(abspath ../build/native)
OBJDIR    := $(BUILDROOT)/protocol

OBJS := $(addprefix $(OBJDIR)/,codec.o frame.o commands.o indications.o)
LIB  := $(OBJDIR)/libopcproto.a

.PHONY: all clean
all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependency tracking (#40): -MMD emits a .d per object so a header
# edit recompiles every .o that includes it (two stale-ABI incidents before).
-include $(OBJS:.o=.d)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)
