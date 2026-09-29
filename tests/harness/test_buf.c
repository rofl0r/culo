/* A repaint must not realloc per byte.
 *
 * buf_append() reallocated the frame to the exact size needed on every
 * call, and ui_draw_rows() appends a byte at a time for each column of
 * each row, so one repaint of a full screen did thousands of reallocs,
 * each one copying what had been written so far.  The cost is quadratic
 * in the frame size.
 *
 * The buffer now grows by doubling, so a repaint does a logarithmic
 * number of reallocs.  The count is what the test watches: it overrides
 * realloc and checks that a repaint's frame costs a handful of
 * allocations rather than one per byte.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_buf.c"

static int n_realloc;
static int n_bytes_requested;
static int on;

void *realloc(void *p, size_t n)
{
	static void *(*real_realloc)(void *, size_t);
	if (!real_realloc)
		real_realloc = dlsym(RTLD_NEXT, "realloc");
	if (on) {
		n_realloc++;
		n_bytes_requested += (int) n;
	}
	return real_realloc(p, n);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	/* A screenful of rows, each long enough to fill the width, so the
	 * frame is as large as a real repaint makes it. */
	for (int i = 0; i < 60; i++) {
		for (int j = 0; j < 200; j++)
			fputs("int x = 1; /* filler */\n", f);
	}
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	ec.screen_rows = 40;
	ec.screen_cols = 80;
	editor_refresh();

	/* One repaint, with the allocation counter on. */
	on = 1;
	n_realloc = 0;
	n_bytes_requested = 0;
	editor_refresh();
	on = 0;

	/* The frame is a few thousand bytes: 40 rows of 80 columns with
	 * escape sequences, plus the status bar. */
	printf("     one repaint: %d reallocs, %d bytes requested\n",
	       n_realloc, n_bytes_requested);
	h_expect_eq(n_bytes_requested > 1000, 1);
	/* A repaint appends per byte, so the old code reallocated at least
	 * once per byte: thousands.  Doubling makes it a handful. */
	h_expect_eq(n_realloc < 40, 1);

	/* The frame is still correct: the rows are untouched and a repaint
	 * after an edit still draws. */
	h_expect_eq(ROW(0) != NULL, 1);
	h_run("z");
	h_expect_row(0, "zint x = 1; /* filler */");
	editor_refresh();

	/* And the buffer is not leaked: the counter is the only thing this
	 * test added, so the leak check in the ASan run is the assertion
	 * that buf_destroy releases the frame. */
	h_summary("frame buffer growth tests");
	return 0;
}
