/* A row whose allocations failed must not be drawn from a NULL buffer.
 *
 * row_update() freed the old render buffer and then wrote into the
 * result of malloc() without checking it, so an allocation failure
 * left a row with chars and no render buffer, and ui_draw_rows()
 * applied col_offset to that NULL pointer and read the highlight
 * beside it.  syntax_highlight() had the same shape: realloc() to
 * render_size, then memset on the result, and render_size 0 makes
 * realloc(p, 0) free the buffer and return NULL on glibc.
 *
 * Both paths are forced here by failing the allocation, so the test
 * needs no real memory pressure.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_alloc.c"

/* Fail the next n allocations of the given size class, or of any size
 * when size is 0.  A count below zero disables the switch again. */
static long fail_left = -1;
static size_t fail_size;

void *malloc(size_t size)
{
	if (fail_left > 0 && (fail_size == 0 || fail_size == size)) {
		fail_left--;
		errno = ENOMEM;
		return NULL;
	}
	static void *(*real_malloc)(size_t);
	if (!real_malloc)
		real_malloc = dlsym(RTLD_NEXT, "malloc");
	return real_malloc(size);
}

void *realloc(void *p, size_t size)
{
	if (fail_left > 0 && (fail_size == 0 || fail_size == size)) {
		fail_left--;
		errno = ENOMEM;
		return NULL;
	}
	static void *(*real_realloc)(void *, size_t);
	if (!real_realloc)
		real_realloc = dlsym(RTLD_NEXT, "realloc");
	return real_realloc(p, size);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("alpha\nbeta\n", f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	h_expect_eq(NR, 2);
	editor_row_t *row = ROW(0);
	h_expect_eq(row != NULL, 1);
	h_expect_eq(row->render != NULL, 1);
	h_expect_eq(row->highlight != NULL, 1);

	/* --- row_update cannot build the render buffer ---------------- */
	/* The old buffer is freed first, so a failure here leaves the row
	 * without a render buffer.  The refresh below must not read it. */
	fail_size = 0;
	fail_left = 1;
	row_update(row, 0);
	fail_left = -1;
	h_expect_ptr(row->render, NULL);
	h_expect_eq(row->render_size, 0);
	/* The text itself is untouched, so the editor is still usable. */
	h_expect_row(0, "alpha");
	editor_refresh();
	editor_refresh();

	/* The row is marked stale, so a later edit rebuilds the render
	 * buffer.  A draw on its own cannot: it has nothing to rebuild
	 * from, and re-highlighting a row that has no render buffer is
	 * pointless.  The edit is what recovers the row. */
	h_run("X");
	h_expect_eq(ROW(0)->render != NULL, 1);
	h_expect_eq(ROW(0)->render_size, 6);
	h_expect_row(0, "Xalpha");

	/* --- syntax_highlight cannot build the highlight buffer -------- */
	fail_size = 0;
	fail_left = 1;
	syntax_highlight(row, 0);
	fail_left = -1;
	/* The row stays stale, so the next draw tries again, and the draw
	 * does not read the highlight it could not allocate. */
	h_expect_eq(row->hl_valid, 0);
	editor_refresh();
	/* The retry succeeded, so the row is valid again. */
	h_expect_eq(row->hl_valid, 1);
	h_expect_eq(row->highlight != NULL, 1);
	editor_refresh();

	/* --- An empty row keeps a usable highlight buffer -------------- */
	/* render_size 0 makes realloc(p, 0) free the buffer and return
	 * NULL on glibc, and the draw reads the highlight for every
	 * column, so a one-byte buffer is kept instead.  A backspace on
	 * the virtual line leaves the last row empty, which is the
	 * cheapest way to reach that state. */
	/* Emptying the last line leaves a row with no bytes, so
	 * render_size is 0 and realloc(p, 0) is the case to cover. */
	ec.cursor_y = 1;
	ec.cursor_x = ROW(1)->size;
	h_run("\177\177\177\177");	/* backspace the whole line */
	h_expect_eq(ROW(1)->size, 0);
	h_expect_eq(ROW(1)->render_size, 0);
	syntax_highlight(ROW(1), 1);
	h_expect_eq(ROW(1)->highlight != NULL, 1);
	h_expect_eq(ROW(1)->hl_valid, 1);
	editor_refresh();

	h_summary("failed row allocation tests");
	return 0;
}
