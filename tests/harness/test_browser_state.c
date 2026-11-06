/* Browser cursor state must survive a temporary mode.
 *
 * mode_set() memsets mode_state on every mode change. Browser
 * selected, offset, current_dir and show_hidden lived in the
 * union, so entering help from browser and restoring browser
 * reset them to zero. The fix saves them in mode_owners on
 * exit from browser and restores them on entry.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_browser.c"

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f) exit(2);
	fputs("int main(void)\n", f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH)) exit(2);
	editor_refresh();

	mode_set(MODE_BROWSER);
	browser_load_directory("tests/test_tmp");
	h_expect_eq(ec.mode, MODE_BROWSER);
	ec.mode_state.browser.selected = 3;
	ec.mode_state.browser.offset = 2;
	ec.mode_state.browser.show_hidden = true;
	/* Save to owners via mode exit, then enter help, then restore. */
	mode_set(MODE_HELP);
	h_expect_eq(ec.mode, MODE_HELP);
	mode_restore();
	h_expect_eq(ec.mode, MODE_BROWSER);
	h_expect_eq(ec.mode_state.browser.selected, 3);
	h_expect_eq(ec.mode_state.browser.offset, 2);
	h_expect_eq(ec.mode_state.browser.show_hidden, 1);
	h_expect_eq(strcmp(ec.mode_state.browser.current_dir, "tests/test_tmp"), 0);

	/* Cleanup must not leak the browser list. editor_cleanup()
	 * frees ec.rows, so h_summary needs a fresh rows list. */
	editor_cleanup();
	h_expect_ptr(ec.mode_owners.browser_entries, NULL);
	ec.rows = tlist_new(sizeof(editor_row_t));

	h_summary("browser state across temporary modes tests");
	return 0;
}
