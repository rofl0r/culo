/* Regression test: a wrapping search must rescan the start row.
 *
 * search_do_from gave every row exactly one visit.  A forward search
 * that started past a match on the start row came back to that row
 * with the window already consumed, so a match before the cursor on
 * the start row was never found and ^W reported "not found" on a line
 * that plainly held the text.  The start row now gets a second pass
 * with a full-width window.  The same gap existed backwards.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033b M-b toggle backwards search
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_search.c"

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
	open_with("xx\naa\nxx\n");

	/* Forward from past the only match on the start row. */
	ec.cursor_y = 1;
	ec.cursor_x = 2;
	bool wrapped = false;
	if (!search_do_from("a", 1, 3, false, &wrapped)) {
		printf("FAIL: a match before the cursor on the start row was\n"
		       "      never found: the search reported no match\n");
		h_fails++;
	}
	h_expect_eq(ec.cursor_y, 1);
	h_expect_eq(ec.cursor_x, 0);

	/* Backwards from before the only match on the start row. */
	ec.search.mode = SM_BACKWARDS;
	ec.cursor_y = 1;
	ec.cursor_x = 0;
	wrapped = false;
	if (!search_do_from("a", 1, -1, false, &wrapped)) {
		printf("FAIL: a backwards search missed a match after the\n"
		       "      cursor on the start row\n");
		h_fails++;
	}
	h_expect_eq(ec.cursor_y, 1);
	ec.search.mode = 0;

	/* A match on a later row is still found without a wrap, and the
	 * no_wrap scan must still stop at the file boundary. */
	ec.cursor_y = 0;
	wrapped = false;
	bool found = search_do_from("a", 0, 0, true, &wrapped);
	h_expect_eq(found, 1);
	h_expect_eq(ec.cursor_y, 1);
	h_expect_eq(wrapped, 0);

	/* A revisit match on the start row is not a wrap. Replace
	 * logic uses wrapped to stop after a full cycle, so a same
	 * row revisit must not set it. */
	ec.cursor_y = 1;
	ec.cursor_x = 2;
	wrapped = false;
	found = search_do_from("a", 1, 3, false, &wrapped);
	h_expect_eq(found, 1);
	h_expect_eq(wrapped, 0);
	h_expect_eq(ec.cursor_y, 1);
	h_expect_eq(ec.cursor_x, 0);

	h_summary("search start row revisit tests");
	return 0;
}
