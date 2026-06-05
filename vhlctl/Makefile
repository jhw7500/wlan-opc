CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I../protocol
LDFLAGS += -L../protocol
LDLIBS  += -lopcproto

OBJS := vhlctl.o fielddump.o

vhlctl: $(OBJS) ../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) vhlctl
