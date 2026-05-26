CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -fPIC
CFLAGS  += -I.

OBJS := codec.o frame.o commands.o indications.o

libopcproto.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) libopcproto.a
