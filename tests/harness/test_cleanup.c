/* Cleanup must not free a union member it does not own.
 *
 * mode_data_t is a union, so browser.entries is the same word as
 * search.saved_x.  editor_cleanup() freed prompt.buffer whatever the
 * mode, and that word belonged to whichever mode was live, so a cleanup
 * taken from search mode freed the saved cursor of the search and a
 * cleanup taken from browser mode freed the entry list a second time.
 * Both are heap corruption on a normal key sequence.
 *
 * The prompt mode is gone, and with it the one member of the union that
 * was a pointer, so the only frees left in the mode switch are the ones
 * in mode_owners, whose owner is named.  The last section checks that
 * a browser left open at cleanup does not free the list twice.
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

	/* --- 1. The saved highlight is released exactly once --------- */
	/* It outlives the mode that made it, so mode_owners holds it and
	 * not the union.  Cleanup releases it. */
	search_highlight_match(0, 0, 3);
	h_expect_eq(ec.mode_owners.search_highlight_line, 0);
	h_expect_eq(ec.mode_owners.search_saved_highlight != NULL, 1);
	h_expect_eq(ec.mode_owners.search_saved_highlight_size > 0, 1);
	editor_cleanup();
	h_expect_ptr(ec.mode_owners.search_saved_highlight, NULL);
	h_expect_eq(ec.mode_owners.search_highlight_line, -1);

	/* And the mode releases it on its own way out. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
	search_highlight_match(0, 0, 3);
	h_expect_eq(ec.mode_owners.search_saved_highlight != NULL, 1);
	/* Leaving search mode is what restores the row and frees the copy.
	 * The editor has to be in search mode for that path to run, so the
	 * test enters it the way ^W does. */
	/* A new search resets the mark, so the copy made before the mode was
	 * entered has to be released explicitly: it belongs to no mode now.
	 * That is the whole point of the reset, and the copy outliving the
	 * search that made it is the leak this test guards. */
	mode_set(MODE_SEARCH);
	mode_set(MODE_NORMAL);
	h_expect_ptr(ec.mode_owners.search_saved_highlight, NULL);
	h_expect_eq(ec.mode_owners.search_highlight_line, -1);
	/* The search query is a persistent buffer the editor keeps across
	 * searches; this test ends here, so it is dropped by hand. */
	free(ec.search.query);
	ec.search.query = NULL;

	/* --- 2. Cleanup from browser mode frees the list once -------- */
	/* editor_cleanup() freed ec.rows, so start a new editor. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	mode_set(MODE_BROWSER);
	ec.mode_owners.browser_entries = tlist_new(sizeof(browser_entry_t));
	if (!ec.mode_owners.browser_entries)
		exit(2);
	/* The list outlives the mode too, so cleanup releases it.  The
	 * search query that mode_set(MODE_SEARCH) allocated is released by
	 * editor_cleanup() too, which is why the browser case starts from a
	 * fresh editor. */
	editor_cleanup();
	h_expect_ptr(ec.mode_owners.browser_entries, NULL);

	/* And the same list, freed by the browser rather than by cleanup. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	mode_set(MODE_BROWSER);
	ec.mode_owners.browser_entries = tlist_new(sizeof(browser_entry_t));
	if (!ec.mode_owners.browser_entries)
		exit(2);
	browser_free_entries();
	h_expect_ptr(ec.mode_owners.browser_entries, NULL);

	/* --- 3. Cleanup taken in browser mode frees the list once ---- */
	/* A cleanup in a mode other than normal is not reachable from a
	 * key sequence, but the frees in the mode switch must not depend
	 * on that: the union holds a treap pointer while the browser is
	 * live, and the free belongs to mode_owners, not to the union. */
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	mode_set(MODE_BROWSER);
	ec.mode_owners.browser_entries = tlist_new(sizeof(browser_entry_t));
	if (!ec.mode_owners.browser_entries)
		exit(2);
	ec.mode_state.browser.selected = 0;
	editor_cleanup();
	h_expect_ptr(ec.mode_owners.browser_entries, NULL);
	/* h_summary frees the state a test still owns; the last
	 * editor_cleanup() left nothing behind. */
	ec.rows = tlist_new(sizeof(editor_row_t));

	h_summary("editor cleanup tests");
	return 0;
}
