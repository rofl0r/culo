/* Regression test: M-] must not read a row that is not there.
 *
 * editor_goto_matching_bracket read ROW(row)->chars with no guard.  On
 * a file with no rows NR is 0, ROW(0) returns NULL, and the first byte
 * comparison dereferenced it, so M-] on an empty file segfaulted.  The
 * same read indexed chars[col] with the cursor taken straight from the
 * editor, which may sit one past the last byte of a row.
 *
 * Fetch the row once, refuse a position that is not on a byte of it,
 * and report that there is no matching bracket.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033] M-] go to matching bracket
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_bracket.c"

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

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* --- Empty buffer: no row exists at all ---------------------- */
	open_with(NULL);
	h_expect_eq(NR, 0);
	editor_goto_matching_bracket();
	if (strcmp(ec.overlay_msg, "No matching bracket") != 0) {
		printf("FAIL: M-] on an empty file reported \"%s\" instead of\n"
		       "      no matching bracket\n", ec.overlay_msg);
		h_fails++;
	}

	/* --- Cursor past the last byte of a row ---------------------- */
	open_with("ab\n");
	ec.cursor_y = 0;
	ec.cursor_x = 2;		/* one past "ab" */
	editor_goto_matching_bracket();
	if (strcmp(ec.overlay_msg, "No matching bracket") != 0) {
		printf("FAIL: M-] with the cursor past the line end reported\n"
		       "      \"%s\"\n", ec.overlay_msg);
		h_fails++;
	}

	/* --- The real cases still work -------------------------------- */
	open_with("int f(void) {\n\treturn 1;\n}\n");

	/* Forward pair on one line: the '(' at column 5 matches the ')' at
	 * column 10. */
	ec.cursor_y = 0;
	ec.cursor_x = 5;
	editor_goto_matching_bracket();
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.cursor_x, 10);

	/* Backward pair on the same line, from the closing paren. */
	ec.cursor_y = 0;
	ec.cursor_x = 10;
	editor_goto_matching_bracket();
	h_expect_eq(ec.cursor_y, 0);
	h_expect_eq(ec.cursor_x, 5);

	/* A brace pair spanning three rows, from the '{' at column 12. */
	ec.cursor_y = 0;
	ec.cursor_x = 12;
	editor_goto_matching_bracket();
	h_expect_eq(ec.cursor_y, 2);
	h_expect_eq(ec.cursor_x, 0);
	if (ec.overlay_msg[0] != '\0') {
		printf("FAIL: a real match still reported \"%s\"\n",
		       ec.overlay_msg);
		h_fails++;
	}

	h_summary("matching bracket guard tests");
	return 0;
}
