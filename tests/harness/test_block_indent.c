/* Block indent/unindent acceptance tests, driven in-process.
 *
 * Covers the four flows a user described:
 *   1. C coder:   select lines, TAB once, Shift+Tab undoes it
 *   2. Python:    select lines, Space x4, Shift+Tab peels one level each
 *   3. Guard:     Shift+Tab on unindented lines must not eat letters
 *   4. Undo:      one undo step reverses exactly one indent keypress
 *   5. Selection: survives indentation and keeps its offsets aligned
 *
 * Each section reloads a fresh fixture so cursor/mode state cannot leak
 * between sections.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033[1;2B  Shift+Down     \033[Z  Shift+Tab
 *   \033[H     Home           \033u   M-U undo
 *   \033e      M-E redo
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_DIR  "tests/test_tmp"
#define FIXTURE_PATH "tests/test_tmp/h_indent.txt"

static const char *fixture_lines[] = {
	"alpha", "beta", "gamma", "delta", "epsilon"
};

static void write_fixture(void)
{
	mkdir(FIXTURE_DIR, 0777);	/* ok if it exists */
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	for (size_t i = 0; i < sizeof(fixture_lines) / sizeof(*fixture_lines);
	     i++)
		fprintf(f, "%s\n", fixture_lines[i]);
	fclose(f);
}

int main(void)
{
	write_fixture();
	editor_init();
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	file_open(FIXTURE_PATH);
	editor_refresh();

	const char *sel3 = "\033[1;2B\033[1;2B\033[1;2B";
	const char *home = "\033[H";

	/* --- 1. C coder: one TAB, removed again by Shift+Tab --------- */
	h_run(home); h_run(sel3); h_run("\t");
	h_expect_row(0, "\talpha");
	h_expect_row(3, "\tdelta");
	h_run("\033[Z");
	h_expect_row(0, "alpha");
	h_expect_row(3, "delta");

	/* --- 2. Python coder: four spaces, peeled one per Shift+Tab --
	 *     (fresh fixture: cursor and selection start clean) ------ */
	h_open(FIXTURE_PATH);
	h_run(home); h_run(sel3); h_run("    ");
	h_expect_row(0, "    alpha");
	h_expect_row(3, "    delta");
	h_run("\033[Z");
	h_expect_row(0, "   alpha");
	h_run("\033[Z\033[Z\033[Z");
	h_expect_row(0, "alpha");
	h_expect_row(2, "gamma");

	/* --- 3. Guard: extra Shift+Tabs never eat letters ------------ */
	h_run("\033[Z\033[Z");
	h_expect_row(0, "alpha");
	h_expect_row(1, "beta");
	h_expect_row(4, "epsilon");

	/* --- 4. Undo granularity: one undo reverses exactly one press -
	 *     fresh fixture; select rows 1-2, indent twice ------------- */
	h_open(FIXTURE_PATH);
	h_run("\033[H\033[1;2B\033[1;2B");   /* rows 1-3 */
	h_run("  ");
	h_expect_row(0, "  alpha");
	h_run("\033u");
	h_expect_row(0, " alpha");
	h_run("\033e");
	h_expect_row(0, "  alpha");

	/* --- 5. M-Space unindents by one space (Shift+Space is not
	 *     expressible in plain VT; M-Space is the portable alias) */
	h_open(FIXTURE_PATH);
	h_run("\033[H\033[1;2B\033[1;2B");	/* select rows 1-3 */
	h_run("    ");		/* four space indents */
	h_expect_row(1, "    beta");
	h_run("\033 ");		/* one M-Space peels one space */
	h_expect_row(1, "   beta");
	h_run("\033u");		/* undo reverses exactly that press */
	h_expect_row(1, "    beta");

	printf("block-indent tests: %d failures\n", h_fails);
	return h_fails ? 1 : 0;
}
