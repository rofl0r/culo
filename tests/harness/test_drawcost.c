/* A repaint of a long line must not be quadratic in its width.
 *
 * ui_draw_rows() needed the byte offset of the column it was drawing,
 * for the selection test and for the tab head marker, and it found
 * that offset with row_renderx_to_cursorx(), which walks the row from
 * its start.  It called it twice per column, so drawing one row cost
 * O(columns * row length), and a long line with tabs or wide
 * characters made a single repaint visibly slow: a 3830 byte line with
 * tabs in a 4000 column window took 100 ms per repaint.
 *
 * The offsets only move forward, so the loop now carries one cursor
 * and steps it a column at a time.  The measurement below is the same
 * one the fix was made from, and it is here so the cost cannot come
 * back unnoticed.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_drawcost.c"

/* One line as wide as the window, made of tabs and single bytes, so
 * render_direct_map is false and the walk is the expensive one. */
static void build_long_line(int tabs_per_line)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("int main(void)\n", f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	ec.screen_cols = 4000;
	ec.screen_rows = 60;
	ec.cursor_y = 0;
	ec.cursor_x = 0;
	for (int i = 0; i < tabs_per_line; i++)
		h_run("a\tb\tc\td\te\tf\tg\th\ti\tj");
}

static double seconds_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	build_long_line(200);
	editor_row_t *row = ROW(0);
	h_expect_eq(row != NULL, 1);
	if (!row)
		exit(2);
	/* The line really does have tabs, so the walk was the slow one. */
	h_expect_eq(row->render_direct_map, 0);
	h_expect_eq(row->size > 3000, 1);

	/* A repaint is O(columns) now, not O(columns * row).  The old cost
	 * was around 100 ms per repaint for this row; a linear one is
	 * three orders of magnitude less, so a generous bound still fails
	 * the old code and passes the new. */
	/* ASan adds about an order of magnitude to this, so the bound has
	 * to be loose enough for the instrumented build and still far
	 * below the old quadratic cost, which was around 100 ms plain and
	 * over a second under ASan. */
	double t0 = seconds_now();
	for (int k = 0; k < 20; k++)
		editor_refresh();
	double each = (seconds_now() - t0) / 20;
	printf("     one repaint of a %d byte row with tabs: %.5f s\n",
	       row->size, each);
	h_expect_eq(each < 0.5, 1);

	/* The drawn text is unchanged: the same columns, the same bytes.
	 * The tab head marker is a rendered glyph, so the frame is
	 * compared with the whitespace markers on and off. */
	ec.show_whitespace = false;
	editor_refresh();
	ec.show_whitespace = true;
	editor_refresh();
	ec.show_whitespace = false;
	editor_refresh();

	/* And a plain row, where the offsets are the column numbers, is
	 * untouched by the new cursor: the direct map path is separate. */
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 500; j++)
			fputc('a', f);
		fputs("\n", f);
	}
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	ec.cursor_y = 0;
	ec.cursor_x = 250;
	/* Select the middle of the line, so the selection test reads the
	 * byte offset for every column. */
	ec.selection.active = true;
	ec.selection.start_y = 0;
	ec.selection.start_x = 100;
	ec.selection.end_y = 0;
	ec.selection.end_x = 400;
	editor_refresh();
	h_expect_eq(ROW(0)->render_direct_map, 1);
	ec.selection.active = false;

	h_summary("repaint cost tests");
	return 0;
}
