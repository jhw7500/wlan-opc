CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I../protocol
LDFLAGS += -L../protocol
LDLIBS  += -lopcproto

# Platform backend selection — exactly one platform_<PLATFORM>.c is linked.
# Override on the command line: `make PLATFORM=nxp` once platform_nxp.c lands.
PLATFORM ?= stub

OBJS := opcd.o store.o handler.o indication.o platform_$(PLATFORM).o

opcd: $(OBJS) ../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) opcd
