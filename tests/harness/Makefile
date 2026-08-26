# In-process editor test harnesses.
#
# Each test_*.c includes culo.c directly and drives the real key
# decoder, so assertions can inspect editor internals (selection state,
# undo stack) that a pty-based suite cannot reach.  Run via `make check-harness`.

HARNESS_CC ?= musl-gcc          # musl + valgrind exposes heap bugs glibc hides
HARNESS_CFLAGS = -g -Wall -Wno-unused -std=gnu99 -D_GNU_SOURCE -I../..
LDLIBS = ../../nregex.c -ldl

TESTS := $(wildcard test_*.c)
BINARIES := $(TESTS:.c=)

.PHONY: all clean run

all: $(BINARIES)

%: %.c harness.h ../../culo.c ../../nregex.c
	$(HARNESS_CC) $(HARNESS_CFLAGS) -include dlfcn.h -o $@ $< $(LDLIBS)

run: all
	@for t in $(BINARIES); do ./$$t > /dev/null 2>&1 || { echo "FAIL: $$t"; exit 1; }; echo "ok   $$t"; done

clean:
	$(RM) $(BINARIES)
