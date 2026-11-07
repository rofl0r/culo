/* Regression test: search on a long line with multi-byte UTF-8.
 *
 * row_cursorx_to_renderx() advanced by display cell width while
 * row->render and row->highlight are indexed by render byte.
 * Every multi-byte character before a match drifted render_x
 * from the byte offset by (char_len - char_width), so the window
 * editor_scroll() opened for a match far into a long line did
 * not contain the match and the cursor sat in unrelated text.
 * The reported case was a second search for "HTTP/1.1 4" in a
 * log, which landed in the middle of a very long JSON string.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   none, the search is driven directly.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_search_long_utf8.c"

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	/* Many 3-byte UTF-8 characters, then the needle far in. */
	for (int i = 0; i < 200; i++)
		fputs("\xef\xbf\xbd", f);	/* U+FFFD: 3 bytes, width 1 */
	for (int i = 0; i < 200; i++)
		fputc('x', f);
	fputs("HTTP/1.1 4\n", f);
	fputs("tail\n", f);
	fclose(f);

	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	ec.screen_cols = 80;
	ec.screen_rows = 24;

	/* Search from the start: the match is at byte 800. */
	ec.cursor_y = 0;
	ec.cursor_x = 0;
	bool wrapped = false;
	if (!search_do_from("HTTP/1.1 4", 0, 1, false, &wrapped)) {
		printf("FAIL: search did not find the match\n");
		h_fails++;
	}
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.cursor_x, 800);
	/* The bytes at the cursor are the needle itself. */
	if (strncmp(ROW(0)->chars + ec.cursor_x, "HTTP/1.1 4", 9) != 0) {
		printf("FAIL: cursor is not on the match\n");
		h_fails++;
	}

	/* The render offset must equal the render byte index, or
	 * editor_scroll() opens a window that excludes the match. */
	h_expect_eq(row_cursorx_to_renderx(ROW(0), ec.cursor_x),
		    ec.cursor_x);
	editor_scroll();
	/* With an 80 column window and the cursor at the right
	 * edge, the match start must be inside the window. */
	int match_render = row_cursorx_to_renderx(ROW(0),
						  ec.cursor_x);
	if (!(match_render >= ec.col_offset &&
	      match_render < ec.col_offset + 80)) {
		printf("FAIL: match at render %d outside window [%d, %d)\n",
		       match_render, ec.col_offset,
		       ec.col_offset + 80);
		h_fails++;
	}
	/* The drawn slice starting at col_offset reaches the
	 * match within the window. */
	if (strncmp(ROW(0)->render + match_render, "HTTP/1.1 4", 9) != 0) {
		printf("FAIL: render slice at match does not hold needle\n");
		h_fails++;
	}

	/* Highlight must also land on the match, not drifted. */
	search_highlight_match(0, match_render, 9);
	for (int i = 0; i < 9; i++)
		h_expect_eq(ROW(0)->highlight[match_render + i], MATCH);

	h_summary("search on long UTF-8 line tests");
	return 0;
}
