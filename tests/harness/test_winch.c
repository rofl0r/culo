/* Regression test: a resize must not move the cursor.
 *
 * The SIGWINCH handler clamped cursor_y to screen_rows - 1 and
 * cursor_x to screen_cols - 1.  Those are window bounds, not buffer
 * bounds, so shrinking the terminal moved the cursor to a different
 * line of the file.  Reproduced over a pty: page down to line 78 in a
 * 40-row window, then resize to 4 rows, and the status bar showed
 * line 3.  Every resize threw away the cursor position.
 *
 * Bound the cursor by the buffer instead.  The window only decides how
 * much of the buffer is visible, and editor_scroll already follows the
 * cursor into the new size.  Clamp cursor_x to the row it is on, since
 * a narrower window can leave a column past the end of the line.
 *
 * The handler re-reads the size through term_size_reader, so replacing
 * that one function lets a resize be driven without a tty.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_winch.c"

static int fake_rows = 40, fake_cols = 200;

static int fake_get_size(int *rows, int *cols)
{
	*rows = fake_rows;
	*cols = fake_cols;
	return 0;
}

static void open_with(const char *body)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	if (body)
		fputs(body, f);
	fclose(f);
	editor_init();
	term_size_reader = fake_get_size;
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

/* One resize, through the real handler. */
static void resize(int rows, int cols)
{
	fake_rows = rows;
	fake_cols = cols;
	sig_winch_handler(0);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */
	open_with("a\nb\nc\nd\ne\nf\ng\nh\n");
	resize(40, 200);

	/* --- Cursor past the window, inside the file ------------------ */
	ec.cursor_y = 6;
	ec.cursor_x = 1;
	resize(3, 200);
	h_expect_eq(ec.cursor_y, 6);	/* was clamped to 2 */
	h_expect_eq(ec.cursor_x, 1);

	/* --- A narrower window bounds the column to the row it is on -- */
	resize(40, 1);
	h_expect_eq(ec.cursor_y, 6);
	h_expect_eq(ec.cursor_x, 1);	/* "h" is one byte, so 1 stands */

	/* The cursor on the last row of a file shorter than the window
	 * stays where it is. */
	resize(40, 200);
	ec.cursor_y = NR - 1;
	ec.cursor_x = 0;
	resize(2, 200);
	h_expect_eq(ec.cursor_y, NR - 1);

	/* --- An empty buffer keeps the cursor at row 0 ----------------- */
	open_with(NULL);
	resize(2, 200);
	h_expect_eq(NR, 0);
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.cursor_x, 0);

	h_summary("resize keeps the cursor tests");
	return h_fails ? 1 : 0;
}
