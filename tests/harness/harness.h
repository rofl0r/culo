/* Shared harness for in-process editor tests.
 *
 * Usage: create a .c file that does
 *
 *     #define main culo_main
 *     #include "../../culo.c"
 *     #undef main
 *     #include "harness.h"
 *
 * then define a test_main() and compile the whole thing as one binary.
 * The harness overrides read() so scripted keystrokes are fed to the
 * real decoder, and exposes helpers to run keys and assert on state.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <dlfcn.h>

#undef NR
#define NR ((int)tlist_getsize(ec.rows))

static const char *h_keys;          /* scripted keystrokes, or NULL */
static size_t h_pos;                /* read cursor into h_keys */
static int h_exhausted;             /* set when script ran dry */

/* Feed scripted bytes to stdin reads; EOF once the script runs out. */
ssize_t read(int fd, void *buf, size_t count)
{
	static ssize_t (*real_read)(int, void *, size_t);
	if (!real_read)
		real_read = dlsym(RTLD_NEXT, "read");
	if (fd == 0 && h_keys) {
		if (!h_keys[h_pos]) {
			h_exhausted = 1;
			return 0;
		}
		((char *) buf)[0] = h_keys[h_pos++];
		return 1;
	}
	for (;;)
		pause();        /* no input scripted: hang loudly */
	return real_read(fd, buf, count);
}

/* Reset interpreter state between scripts. */
static void h_reset_keys(void)
{
	h_keys = NULL;
	h_pos = 0;
	h_exhausted = 0;
}

/* Run one keystroke script to completion. */
static void h_run(const char *keys)
{
	h_keys = keys;
	h_pos = 0;
	h_exhausted = 0;
	while (h_keys[h_pos] && !h_exhausted)
		editor_process_key();
	h_reset_keys();
}

/* Open path with a clean editor state. */
static void h_open(const char *path)
{
	editor_init();
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	if (!file_open(path))
		exit(2);
	editor_refresh();
}

/* Assertion helpers. */
static int h_fails;

static void h_expect_eq(int got, int want)
{
	if (got != want) {
		printf("FAIL: got %d want %d\n", got, want);
		h_fails++;
	} else {
		printf("ok   %d\n", got);
	}
}

static void h_expect_ptr(const void *got, const void *want)
{
	if (got != want) {
		printf("FAIL: got %p want %p\n", got, want);
		h_fails++;
	} else {
		printf("ok   %p\n", got);
	}
}

static void h_expect_row(int y, const char *want)
{
	const char *got = ROW(y)->chars;
	if (strcmp(got, want) != 0) {
		printf("FAIL row %d: got |%s| want |%s|\n", y + 1, got, want);
		h_fails++;
	} else {
		printf("ok   row %d = |%s|\n", y + 1, got);
	}
}

static void h_summary(const char *name)
{
	printf("%s %s (%d failures)\n", h_fails ? "FAILED" : "PASSED",
	       name, h_fails);
	exit(h_fails ? 1 : 0);
}

#endif /* HARNESS_H */
