/* Backspace on the virtual line must join, not do nothing.
 *
 * The cursor may sit on the virtual line past the last row: that is
 * where nano puts it, and editor_insert_char() turns it into a real
 * row when the next character arrives.  editor_delete_char() returned
 * early for that row, so Backspace there deleted nothing at all, and
 * the way to join the last two lines was to put the cursor at the
 * start of the last line by hand.
 *
 * Joining is the same code the start-of-row path already uses: drop
 * the newline between the row above and the virtual line, which is the
 * same as the end of the last row.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_backspace.c"

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("abc\ndef\n", f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	h_expect_eq(NR, 2);

	/* The virtual line is the one past the last row, at column 0. */
	ec.cursor_y = NR;
	ec.cursor_x = 0;
	h_expect_eq(ec.cursor_y, 2);

	/* Backspace there removes the newline before the last line. */
	h_run("\177");
	h_expect_eq(NR, 1);
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.cursor_x, 3);
	h_expect_row(0, "abcdef");

	/* It is one undo step, like the same join from the last row. */
	h_expect_eq(g_undo.count, 1);
	h_run("\033u");		/* M-u: undo */
	h_expect_eq(NR, 2);
	h_expect_row(0, "abc");
	h_expect_row(1, "def");
	h_run("\033e");		/* M-e: redo */
	h_expect_eq(NR, 1);
	h_expect_row(0, "abcdef");

	/* At the start of the buffer nothing happens, as before. */
	ec.cursor_y = 0;
	ec.cursor_x = 0;
	h_run("\177");
	h_expect_eq(NR, 1);
	h_expect_row(0, "abcdef");

	h_summary("backspace on the virtual line tests");
	return 0;
}
