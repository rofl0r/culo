/* Selection mode through the real key decoder.
 *
 * These replace the expect shell tests in test-selection.sh.
 * They assert on selection state and on the clipboard, which
 * the shell tests could not do: most of them only checked
 * that the editor process exited cleanly.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033a M-A start selection   \0336 M-6 copy selection
 *   \003 ^C cancel   \177 backspace deletes selection
 *   \033[C right   \033[B down   \033[F End   \033[H Home
 *   \033[1;2C Shift+Right
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_selection.c"

static void open_with(const char *body)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs(body, f);
	fclose(f);
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

static void expect_clipboard(const char *want)
{
	if (!ec.copied_char_buffer ||
	    strcmp(ec.copied_char_buffer, want) != 0) {
		printf("FAIL clipboard: got [%s], want [%s]\n",
		       ec.copied_char_buffer ? ec.copied_char_buffer
						     : "<null>",
		       want);
		h_fails++;
	} else {
		printf("ok   clipboard [%s]\n", want);
	}
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* Enter selection, move, copy, leave. */
	open_with("Select this text\nAnd this line too\n");
	h_run("\033a");		/* M-A */
	h_expect_eq(ec.mode, MODE_SELECT);
	h_expect_eq(ec.selection.active, 1);
	h_expect_eq(ec.selection.start_x, 0);
	h_run("\033[C");
	h_run("\033[C");
	h_run("\033[C");
	h_run("\033[B");
	h_expect_eq(ec.selection.end_x, 3);
	h_expect_eq(ec.selection.end_y, 1);
	h_run("\0336");		/* M-6: copy marked region */
	h_expect_eq(ec.mode, MODE_NORMAL);
	/* "Sel" plus the rest of line 1 up to the end marker. */
	h_expect_eq(ec.copied_char_buffer != NULL, 1);

	/* Home and End extend the selection. */
	open_with("Start Middle End\n");
	h_run("\033a");
	h_run("\033[F");		/* End: select to end of line */
	h_expect_eq(ec.selection.end_x, 16);
	h_run("\0336");
	expect_clipboard("Start Middle End");

	/* Cancel with ^C leaves the buffer and the clipboard
	 * untouched and returns to normal mode. */
	open_with("Cancel this selection\n");
	h_run("\033a");
	h_run("\033[C");
	h_run("\033[C");
	h_run("\003");
	h_expect_eq(ec.mode, MODE_NORMAL);
	h_expect_eq(ec.selection.active, 0);
	h_expect_row(0, "Cancel this selection");

	/* Delete the selection: Shift+Right twice selects "ab",
	 * Backspace deletes it, leaving "cdef". */
	open_with("abcdef\n");
	h_run("\033[1;2C");
	h_run("\033[1;2C");
	h_expect_eq(ec.mode, MODE_SELECT);
	h_expect_eq(ec.selection.end_x, 2);
	h_run("\177");
	h_expect_row(0, "cdef");
	h_expect_eq(ec.mode, MODE_NORMAL);

	/* Cut the selection: same selection, ^K cuts it and
	 * leaves it in the clipboard. */
	open_with("abcdef\n");
	h_run("\033[1;2C");
	h_run("\033[1;2C");
	h_run("\013");		/* ^K in select mode: cut marked text */
	h_expect_row(0, "cdef");
	expect_clipboard("ab");

	h_summary("selection tests");
	return 0;
}
