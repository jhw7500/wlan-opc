CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I..
LDFLAGS += -L..
LDLIBS  += -lopcproto

TESTS := test_codec

all: $(TESTS)

test_codec: test_codec.o ../libopcproto.a
	$(CC) $(CFLAGS) -o $@ test_codec.o $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: check clean
check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
	@echo "all tests passed"

clean:
	rm -f *.o $(TESTS)
