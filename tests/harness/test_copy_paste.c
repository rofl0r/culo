/* Copy, cut and paste through the real key decoder.
 *
 * These replace the expect shell tests in test-copy_paste.sh.
 * Three of those only asserted that a handler existed.  These
 * assert on the clipboard and on the buffer after each paste.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \0336 M-6 copy current line or marked region
 *   \013 ^K cut line   \025 ^U paste (uncut)
 *   \033a M-A mark   \033[F End   \033[B down
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_copy_paste.c"

static void open_with(const char *body)
{
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
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* Copy the current line with M-6 and paste it on the
	 * next line: the clipboard holds the line, then ^U
	 * inserts it at the cursor. */
	open_with("alpha\nbeta\n");
	h_run("\0336");
	h_expect_eq(ec.copied_char_buffer != NULL &&
		    strcmp(ec.copied_char_buffer, "alpha") == 0, 1);
	h_run("\033[B");
	h_run("\025");
	h_expect_row(1, "alphabeta");

	/* Cut a line with ^K and paste it back: the cut line
	 * leaves the buffer, the paste puts it at the cursor. */
	open_with("Line to cut\nKeep this line\nAnother line\n");
	h_run("\013");
	h_expect_eq(NR, 2);
	h_expect_row(0, "Keep this line");
	h_run("\033[B");
	h_run("\025");
	h_expect_eq(NR, 3);
	h_expect_row(1, "Line to cut");
	h_expect_row(2, "Another line");

	/* Paste twice: the clipboard must survive the first
	 * paste unchanged. */
	open_with("src\n");
	h_run("\0336");
	h_run("\033[F");
	h_run("\025");
	h_run("\025");
	h_expect_row(0, "srcsrcsrc");

	/* Cut a marked region, not a whole line: M-A, End, ^K
	 * cuts the mark, ^U pastes it where the cursor now is. */
	open_with("abcdef\n");
	h_run("\033a");
	h_run("\033[F");
	h_run("\013");
	h_expect_row(0, "");
	h_run("\025");
	h_expect_row(0, "abcdef");

	h_summary("copy paste tests");
	return 0;
}
