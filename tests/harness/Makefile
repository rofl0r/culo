# In-process editor test harnesses.
#
# Each test_*.c includes culo.c directly and drives the real key
# decoder, so assertions can inspect editor internals (selection state,
# undo stack) that a pty-based suite cannot reach.  Run via `make check-harness`.

HARNESS_CC ?= $(CC)          # musl + valgrind exposes heap bugs glibc hides
HARNESS_CFLAGS = -g -Wall -Wno-unused -std=gnu99 -D_GNU_SOURCE -I../..
LDLIBS = ../../nregex.c -ldl

# SANITIZE=address builds a second, instrumented copy of every harness.
# ASAN_OPTIONS caps the allocator so a leak check cannot eat the machine.
SANITIZE ?=
ifeq ($(SANITIZE),address)
HARNESS_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZER_CC = $(HARNESS_CC)
SANITIZER_CFLAGS = $(HARNESS_CFLAGS)
SANITIZER_LDLIBS = $(LDLIBS)
SANITIZER_BINARIES = $(addprefix asan_,$(BINARIES))
else
SANITIZER_BINARIES =
endif

TESTS := $(wildcard test_*.c)
BINARIES := $(TESTS:.c=)
# Fixtures use repo-root-relative paths, so the binaries run from the root.
REPO_ROOT = ../..
FIXTURE_DIR = $(REPO_ROOT)/tests/test_tmp

.PHONY: all clean run asan

all: $(BINARIES)

%: %.c harness.h ../../culo.c ../../nregex.c
	$(HARNESS_CC) $(HARNESS_CFLAGS) -include dlfcn.h -o $@ $< $(LDLIBS)

# asan_<name> is the same source built with the sanitizers, so the ASan
# run needs no second copy of any test.
asan_%: %.c harness.h ../../culo.c ../../nregex.c
	$(SANITIZER_CC) $(SANITIZER_CFLAGS) -include dlfcn.h -o $@ $< $(SANITIZER_LDLIBS)

asan: $(SANITIZER_BINARIES) $(FIXTURE_DIR)
	@cd $(REPO_ROOT) && for t in $(addprefix tests/harness/,$(SANITIZER_BINARIES)); do \
		ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 \
		UBSAN_OPTIONS=halt_on_error=0:print_stacktrace=1 \
		$$t > /dev/null 2> $$t.log || { echo "FAIL: $$t"; cat $$t.log; exit 1; }; \
		grep -q 'ERROR\|runtime error' $$t.log && { echo "SANITIZER OUTPUT: $$t"; cat $$t.log; exit 1; }; \
		echo "ok   $$t"; done

run: all $(FIXTURE_DIR)
	@cd $(REPO_ROOT) && for t in $(addprefix tests/harness/,$(BINARIES)); do ./$$t > /dev/null 2>&1 || { echo "FAIL: $$t"; exit 1; }; echo "ok   $$t"; done

$(FIXTURE_DIR):
	mkdir -p $@

clean:
	$(RM) $(BINARIES)
	$(RM) asan_$(BINARIES)
	$(RM) *.log
