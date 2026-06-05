CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I.. -I../../protocol
LDFLAGS += -L../../protocol
LDLIBS  += -lopcproto

TESTS := test_fielddump

all: $(TESTS)

test_fielddump: test_fielddump.o ../fielddump.o ../../protocol/libopcproto.a
	$(CC) $(CFLAGS) -o $@ test_fielddump.o ../fielddump.o $(LDFLAGS) $(LDLIBS)

../fielddump.o: ../fielddump.c ../fielddump.h
	$(CC) $(CFLAGS) -c ../fielddump.c -o ../fielddump.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: check clean
check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
	@echo "all vhlctl unit tests passed"

clean:
	rm -f *.o ../fielddump.o $(TESTS)
