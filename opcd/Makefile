CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I../protocol
LDFLAGS += -L../protocol
LDLIBS  += -lopcproto

# Platform backend selection — exactly one platform_<PLATFORM>.c is linked.
# Override on the command line: `make PLATFORM=nxp` once platform_nxp.c lands.
PLATFORM ?= stub

OBJS := opcd.o store.o handler.o indication.o json_util.o inventory.o ntp_parse.o snapshot.o platform_$(PLATFORM).o

opcd: $(OBJS) ../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	# Use wildcard so that switching PLATFORM does not leave stale
	# platform_<other>.o behind ($(OBJS) only references the *current* PLATFORM).
	rm -f *.o opcd
