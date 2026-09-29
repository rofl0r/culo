/* Regression test: the cursor must stay inside the buffer.
 *
 * M-g clamped the requested line only when the file had rows, then
 * assigned cursor_y = lnum - 1 either way.  On an empty file that left
 * the cursor at row 4 with zero rows, and the next cursor move walked
 * off the end: editor_move_cursor read ROW(cursor_y - 1)->size with an
 * index past the end, so ROW returned NULL and the size load
 * dereferenced it.  Left arrow after M-g 5 on an empty file
 * segfaulted.
 *
 * Clamp the line to the last row, or to row 0 when the buffer is
 * empty, and make the move up and left tolerate a cursor that is out of
 * range so no future path can repeat the crash.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033g M-g goto line   \033[D left   \033[C right
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_cursor.c"

static void open_with(const char *body)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	if (body)
		fputs(body, f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

static void feed(const char *keys)
{
	h_keys = keys;
	h_pos = 0;
	h_exhausted = 0;
	while (h_keys[h_pos] && !h_exhausted)
		editor_process_key();
	h_reset_keys();
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* --- M-g on an empty buffer ----------------------------------- */
	open_with(NULL);
	h_expect_eq(NR, 0);
	feed("\033g5\r");	/* M-g, "5", Enter */
	if (ec.cursor_y < 0 || ec.cursor_y > NR) {
		printf("FAIL: M-g 5 on an empty file left the cursor at row\n"
		       "      %d with %d rows\n", ec.cursor_y, NR);
		h_fails++;
	}
	/* The move that used to dereference NULL. */
	feed("\033[D");		/* left */
	feed("\033[C");		/* right */
	feed("\033[A");		/* up */
	feed("\033[B");		/* down */
	h_expect_eq(NR, 0);
	if (ec.cursor_y < 0 || ec.cursor_y > NR) {
		printf("FAIL: cursor walks left the buffer: row %d of %d\n",
		       ec.cursor_y, NR);
		h_fails++;
	}

	/* --- M-g on a short file clamps to the last row --------------- */
	open_with("a\nb\nc\n");
	feed("\033g99\r");
	h_expect_eq(ec.cursor_y, 2);
	feed("\033[D");
	feed("\033g1\r");
	h_expect_eq(ec.cursor_y, 0);
	feed("\033[A");
	h_expect_eq(ec.cursor_y, 0);

	/* --- A cursor forced past the end stays safe ------------------ */
	/* Left arrow with the cursor below every row reads the row above
	 * the end, which is the other way this used to crash. */
	open_with("a\nb\n");
	ec.cursor_y = NR + 3;
	ec.cursor_x = 0;
	feed("\033[D");
	feed("\033[A");
	feed("\033[F");
	feed("\010");		/* backspace */
	printf("     survived with %d rows\n", NR);

	h_summary("cursor clamp tests");
	return 0;
}
