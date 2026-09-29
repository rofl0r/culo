/* Cleanup must not free a union member it does not own.
 *
 * mode_data_t is a union, so prompt.buffer is the same word as
 * search.saved_x.  editor_cleanup() freed it whatever the mode, so a
 * cleanup taken from search mode freed the saved cursor of the search,
 * and a cleanup taken from browser mode freed the entry list a second
 * time.  Both are heap corruption on a normal key sequence.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_cleanup.c"

static void write_fixture(void)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("int main(void)\nint g(void)\n", f);
	fclose(f);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	write_fixture();
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();

	/* --- 1. Cleanup from search mode keeps the search state ------ */
	search_highlight_match(0, 0, 3);
	h_expect_eq(ec.mode_state.search.highlight_line, 0);
	h_expect_eq(ec.mode_state.search.saved_highlight != NULL, 1);
	int saved_x = ec.mode_state.search.saved_x;
	int saved_y = ec.mode_state.search.saved_y;
	/* The union word that prompt.buffer would read is the saved
	 * cursor, so cleanup must leave it alone. */
	h_expect_ptr(ec.mode_state.prompt.buffer, (char *) (long) saved_x);
	editor_cleanup();
	h_expect_ptr(ec.mode_state.search.saved_highlight, NULL);
	h_expect_eq(ec.mode_state.search.saved_x, saved_x);
	h_expect_eq(ec.mode_state.search.saved_y, saved_y);

	/* --- 2. Cleanup from browser mode frees the list once -------- */
	/* editor_cleanup() freed ec.rows, so start a new editor. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	mode_set(MODE_BROWSER);
	ec.mode_state.browser.entries = tlist_new(sizeof(browser_entry_t));
	if (!ec.mode_state.browser.entries)
		exit(2);
	/* The list pointer is the first word of the union, so it is also
	 * what prompt.buffer reads.  Cleanup must not free it twice. */
	h_expect_ptr((void *) ec.mode_state.prompt.buffer,
		     (void *) ec.mode_state.browser.entries);
	editor_cleanup();
	h_expect_ptr(ec.mode_state.browser.entries, NULL);

	/* --- 3. The prompt buffer is not cleanup's to free ------------ */
	/* MODE_PROMPT is never entered: mode_set() has no caller for it
	 * and ui_prompt() manages its own buffer, so the word that
	 * prompt.buffer shares with the rest of the union belongs to
	 * whichever mode is live.  Cleanup must leave it alone, and
	 * mode_set() is what frees a prompt buffer, on the way out of
	 * the mode that owns it. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	mode_set(MODE_PROMPT);
	ec.mode_state.prompt.buffer = xstrndup0("typed", 5);
	if (!ec.mode_state.prompt.buffer)
		exit(2);
	/* Leaving the mode is what releases the prompt buffer, so the
	 * editor is in a normal mode before cleanup runs.  Cleanup with
	 * the union still holding a prompt pointer is not reachable from
	 * a key sequence: ^X leaves MODE_PROMPT through mode_set() on its
	 * way out, and no other path reaches editor_cleanup. */
	mode_set(MODE_NORMAL);
	h_expect_ptr(ec.mode_state.prompt.buffer, NULL);
	editor_cleanup();
	/* h_summary frees the state a test still owns; the last
	 * editor_cleanup() left nothing behind. */
	ec.rows = tlist_new(sizeof(editor_row_t));

	h_summary("editor cleanup tests");
	return 0;
}
