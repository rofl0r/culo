/*
 * The virtual line is a place to type at, not a row to page from.
 *
 * Keys:  \x1b[B  Down   \x1b[6~  Page Down   \x1b[5~  Page Up
 *        \x1bOF  End
 *
 * On a two row file the down arrow puts the cursor on the virtual line,
 * row NR, which is where a new row is created when a character is typed.
 * Page Down from there jumped to the last visible line, 22, and then
 * walked down from there, so the cursor stopped on row NR again, still
 * one past the end.  Page Up from the same place moved the cursor to
 * row NR - 23, which is above the first row, and the window scrolled so
 * that row 0 was off screen.
 *
 * A page key clamps the cursor to a row that exists, and keeps it in the
 * window that the file shows.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_page.c"

static void write_rows(int rows)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	for (int i = 0; i < rows; i++)
		fprintf(f, i == 0 ? "first\n" : "row%d\n", i);
	fclose(f);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	write_rows(2);
	term_update_size();
	int page = ec.screen_rows;

	h_open(FIXTURE_PATH);
	h_expect_eq(NR, 2);
	int last = NR - 1;

	/* The down arrow on the last row reaches the virtual line. */
	ec.cursor_y = last;
	ec.cursor_x = 0;
	editor_refresh();
	h_run("\x1b[B");
	h_expect_eq(ec.cursor_y, NR);

	/* Page Down from there has nowhere to go, and must not stay on a
	 * row past the end. */
	h_run("\x1b[6~");
	h_expect_eq(ec.cursor_y, last);
	h_run("\x1bOF");
	h_expect_eq(ec.cursor_x, ROW(last)->size);
	h_expect_row(last, "row1");

	/* Page Up from the virtual line goes to the top row, not to a row
	 * above it, and the window stays on the file. */
	ec.cursor_y = NR;
	ec.cursor_x = 0;
	editor_refresh();
	h_run("\x1b[5~");
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.row_offset, 0);

	/* A page up from the top row also stays on the top row. */
	h_run("\x1b[5~");
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.row_offset, 0);

	/* Typing on the virtual line still creates the row. */
	ec.cursor_y = last;
	ec.cursor_x = 0;
	editor_refresh();
	h_run("\x1b[B");
	h_run("x");
	h_expect_eq(NR, 3);
	h_expect_row(2, "x");

	/* And the cursor is on the new row, not past it. */
	h_expect_eq(ec.cursor_y, 2);
	h_expect_eq(ec.cursor_x, 1);

	/* A file longer than the window pages a window at a time and keeps
	 * the cursor inside the window. */
	int rows = page * 3;
	write_rows(rows);
	h_open(FIXTURE_PATH);
	h_expect_eq(NR, rows);
	last = rows - 1;

	ec.cursor_y = 0;
	ec.cursor_x = 0;
	ec.row_offset = 0;
	editor_refresh();
	h_run("\x1b[6~");
	h_expect_eq(ec.cursor_y, page);
	h_expect_eq(ec.cursor_y >= ec.row_offset, 1);
	h_expect_eq(ec.cursor_y < ec.row_offset + page, 1);

	int before = ec.cursor_y;
	h_run("\x1b[6~");
	h_expect_eq(ec.cursor_y, before + page);
	h_expect_eq(ec.cursor_y >= ec.row_offset, 1);

	/* A page up walks the same distance back. */
	h_run("\x1b[5~");
	h_expect_eq(ec.cursor_y, before);

	/* Paging down at the end of the file stops on the last row. */
	ec.cursor_y = last;
	ec.cursor_x = 0;
	editor_refresh();
	h_run("\x1b[6~");
	h_expect_eq(ec.cursor_y, last);
	h_run("\x1bOF");
	h_expect_eq(ec.cursor_x, ROW(last)->size);

	/* Select mode pages with the same keys and the same bound. */
	ec.cursor_y = 0;
	ec.cursor_x = 0;
	ec.row_offset = 0;
	editor_refresh();
	h_run("\033[1;2B");	/* Shift+Down enters select mode */
	h_expect_eq(ec.mode, MODE_SELECT);
	h_run("\x1b[6;2~");	/* Shift+Page Down */
	h_expect_eq(ec.cursor_y, 1 + page);
	h_expect_eq(ec.cursor_y >= ec.row_offset, 1);
	h_run("\x1b[5;2~");	/* Shift+Page Up */
	h_expect_eq(ec.cursor_y, 1);
	h_expect_eq(ec.selection.end_y, 1);

	h_summary("page key tests");
	return 0;
}
