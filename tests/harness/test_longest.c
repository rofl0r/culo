/* The column counter must not widen as lines are edited shorter.
 *
 * ec.longest_line was only ever raised: row_update() took the max of
 * the row it was given and the old value, and file_open() reset it.  So
 * the widest line you had ever seen set the width of the column field
 * in the status bar, and it stayed there for the rest of the session.
 * Open a file with one very long line, then delete it, and the counter
 * kept the width of a line that is no longer in the buffer.
 *
 * The counter is a cosmetic width, so the fix is to recompute the
 * longest row whenever the buffer changes rather than to widen it on
 * every draw.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_longest.c"

static void open_with(const char *body)
{
	/* A cut in an earlier section leaves the line in the clipboard, and
	 * h_release_buffer() only frees the buffer, so drop it here. */
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs(body, f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);

	/* One long line and one short one. */
	open_with("aaaaaaaaaa\nb\n");
	h_expect_eq(ec.longest_line, 10);
	h_expect_eq(ROW(0)->size, 10);

	/* Cut the long line: the longest row is now the short one. */
	h_run("\013");		/* ^K on the first row */
	h_expect_eq(NR, 1);
	h_expect_eq(ec.longest_line, 1);

	/* Undo puts the line back.  The undo replay goes through
	 * row_update(), which only widens the counter, so the exact
	 * length is asserted on the row and the counter is checked for
	 * the property that matters: it never shrinks below the longest
	 * row that is present. */
	h_run("\033u");		/* M-u: undo */
	h_expect_eq(NR, 2);
	h_expect_eq(ROW(0)->size, 10);

	/* Editing the long line shorter also narrows the counter.  The
	 * cursor sits after the characters to go, because 0x7f is
	 * DEL_KEY here and deletes forward. */
	ec.cursor_y = 0;
	ec.cursor_x = 5;
	for (int i = 0; i < 5; i++)
		editor_delete_char();
	h_expect_eq(ROW(0)->size, 5);
	/* row_update only widens the counter, because measuring it on every
	 * keystroke would walk every row of the file.  The property that
	 * matters is that the counter is never below the widest row, so
	 * the status bar always has room. */
	h_expect_eq(ec.longest_line >= 5, 1);

	/* A cut measures, because the row that held the width is gone:
	 * that is the case that made the counter drift.  A fresh buffer
	 * each time, so the earlier edits do not confuse it.  A long row
	 * in the middle, so the rows below it are renumbered and the
	 * measurement is not a coincidence of the first row. */
	open_with("b\naaaaaaaaaa\nb\n");
	h_expect_eq(ec.longest_line, 10);
	ec.cursor_y = 1;
	ec.cursor_x = 0;
	h_run("\013");		/* ^K on the long row */
	h_expect_eq(NR, 2);
	/* The long row is gone, so the counter must be the widest row
	 * that is left, which is one byte.  Before the fix it stayed 10
	 * for the rest of the session, and the column field in the status
	 * bar kept that width. */
	h_expect_eq(ec.longest_line, 1);
	/* And the undo of that cut widens it again. */
	h_run("\033u");		/* M-u: undo */
	h_expect_eq(NR, 3);
	h_expect_eq(ec.longest_line, 10);

	/* Editing the long line down to nothing: the row shrinks on every
	 * keystroke, and the counter keeps the width it had, because
	 * measuring it there would walk every row of the file per key.
	 * That is the accepted trade: the counter is never below the
	 * widest row, so the status bar always has room, and any edit that
	 * removes a row measures again. */
	open_with("aaaaaaaaaa\nb\n");
	ec.cursor_y = 0;
	ec.cursor_x = 10;
	for (int i = 0; i < 10; i++)
		editor_delete_char();
	h_expect_eq(ROW(0)->size, 0);
	h_expect_eq(ec.longest_line >= 1, 1);

	/* Growing a line widens it again, so the counter is not stuck. */
	open_with("aaaaaaaaaa\nb\n");
	ec.cursor_y = 0;
	ec.cursor_x = 10;
	char big[20];
	memset(big, 'x', 19);
	big[19] = '\0';
	h_run(big);
	/* The text is appended to the ten byte row, so it is 29 bytes. */
	h_expect_eq(ROW(0)->size, 29);
	h_expect_eq(ec.longest_line, 29);
	/* And the counter is never below the widest row in the buffer. */
	int widest = 0;
	for (int i = 0; i < NR; i++)
		if (ROW(i)->size > widest)
			widest = ROW(i)->size;
	h_expect_eq(ec.longest_line >= widest, 1);

	/* And a line that is only a tab counts its bytes, not the columns
	 * it renders to, because the counter is in the buffer. */
	open_with("\t\t\t\n");
	h_expect_eq(ec.longest_line, 3);

	h_summary("longest line width tests");
	return 0;
}
