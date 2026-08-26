/* Ctrl+Left/Right word-jump tests.  Expected columns are computed
 * with strstr on the row the cursor sits on, so they cannot drift
 * from the fixture text. */
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_word.txt"

static void write_fixture(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("#include <stdint.h>\n", f);
	fputs("static int utf8_validate(const char *s, size_t max_len);\n",
	      f);
	fclose(f);
}

static int col_of(int row, const char *needle)
{
	const char *p = strstr(ROW(row)->chars, needle);
	return p ? (int) (p - ROW(row)->chars) : -1;
}

static void goto_pos(int row, int col)
{
	ec.cursor_y = row;	/* direct positioning: the editor's own
				 * arrow keys carry horizontal state that
				 * makes scripted navigation ambiguous. */
	ec.cursor_x = col;
}

int main(void)
{
	write_fixture();
	editor_init();
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	file_open(FIXTURE_PATH);
	editor_refresh();

	const char *cright = "\033[1;5C";
	const char *cleft = "\033[1;5D";
	int utf = col_of(1, "utf");
	int star = col_of(1, "*");
	int ch = col_of(1, "char");
	int szt = col_of(1, "size_t");

	/* row 0: from 'i' of stdint jump to 'h' of .h --------------- */
	goto_pos(0, col_of(0, "stdint") + 2);
	h_run(cright);
	h_expect_eq(ec.cursor_x, col_of(0, "h>"));

	/* row 1: user example - 'u' of utf -> c of const ------------- */
	goto_pos(1, utf);
	h_run(cright);
	h_expect_eq(ec.cursor_x, col_of(1, "const"));

	/* row 1: '*' skips to s -------------------------------------- */
	goto_pos(1, star);
	h_run(cright);
	h_expect_eq(ec.cursor_x, star + 1);

	/* row 1: c of char skips " *" to s --------------------------- */
	goto_pos(1, ch);
	h_run(cright);
	h_expect_eq(ec.cursor_x, star + 1);

	/* backward mirror: c of const -> u of utf -------------------- */
	goto_pos(1, col_of(1, "const"));
	h_run(cleft);
	h_expect_eq(ec.cursor_x, utf);

	/* backward: '*' -> c of char --------------------------------- */
	goto_pos(1, star);
	h_run(cleft);
	h_expect_eq(ec.cursor_x, ch);

	/* backward from inside a word goes to its own start ---------- */
	goto_pos(1, szt + 2);
	h_run(cleft);
	h_expect_eq(ec.cursor_x, szt);

	printf("word-jump tests: %d failures\n", h_fails);
	return h_fails ? 1 : 0;
}
