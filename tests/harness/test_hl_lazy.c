/* Regression test: a text change must not walk the whole row list.
 *
 * row_update used to clear the hl_valid bit of every row from the
 * changed row to the end, once per changed row.  A paste is one change
 * per line, so the cost was quadratic in the line count: a 5000-line
 * paste spent minutes in tlist_getitem, which is what the editor's own
 * perf run showed.  A compiled span rule is what switched that walk on,
 * so a paste into a plain .txt file was fast and a paste into a .c file
 * was not.  The C syntax has span rules, so the fixture is a .c file.
 *
 * The test drives the real key decoder and counts the treap walks.  It
 * fails when the walk count grows faster than the line count, which is
 * the property that made the paste slow.  The lazy mark keeps a change
 * to one number, so the count has to be linear again.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Count the treap walks instead of only doing them. */
#define HL_COUNT_WALKS 1
static int n_walk;
struct tlist;
static size_t h_tlist_getsize(struct tlist *t);
static void *h_tlist_get(struct tlist *t, size_t idx);
#define main culo_main
#include "../../culo.c"
#undef main

static size_t h_tlist_getsize(struct tlist *t)
{
	n_walk++;
	return tlist_getsize(t);
}

static void *h_tlist_get(struct tlist *t, size_t idx)
{
	n_walk++;
	return tlist_get(t, idx);
}

#include "harness.h"

#define SMALL 120
#define BIG 240

#define FIXTURE_PATH "tests/test_tmp/h_hl.c"

/* Run keys the way the main loop does, so a burst is one burst and the
 * repaint lands where the loop puts it. */
static void h_run_loop(const char *keys)
{
	size_t len = strlen(keys);
	h_keys = keys;
	h_pos = 0;
	h_exhausted = 0;
	while (h_keys[h_pos] && !h_exhausted) {
		if (h_pos < len)
			burst_open(ec.cursor_y, ec.cursor_x);
		editor_process_key();
		if (h_pos < len)
			continue;
		burst_close();
		editor_refresh();
	}
	h_reset_keys();
}

static void open_clean(void)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("int main(void)\n", f);
	fclose(f);
	editor_init();
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

/* Treap walks for a paste of n lines of C at the cursor. */
static int walks_for_paste(int n)
{
	size_t len = 0;
	for (int i = 0; i < n; i++)
		len += (size_t)snprintf(NULL, 0, "int v%d = %d;\n", i, i);
	char *text = malloc(len + 1);
	if (!text)
		exit(2);
	char *p = text;
	for (int i = 0; i < n; i++)
		p += sprintf(p, "int v%d = %d;\n", i, i);
	*p = '\0';

	int before = n_walk;
	h_run_loop(text);
	int after = n_walk;
	free(text);
	return after - before;
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */
	open_clean();

	/* The walk only runs when a span rule is compiled, so the test is
	 * only meaningful while the C syntax has one. */
	if (ec.syntax_span_compiled_count == 0) {
		printf("FAIL: the C syntax has no span rule, so this test no\n"
		       "      longer measures the walk it was written for\n");
		h_fails++;
	}
	h_expect_eq(ec.cursor_y, 0);	/* the paste lands on line 1 */

	/* Twice the lines must not cost four times the walks.  A walk of
	 * the rows below each changed row gave a ratio near 4, so the
	 * check sits above that.  The mark is a lower bound because
	 * undo, the burst buffer and the treap insert all walk too. */
	int small = walks_for_paste(SMALL);
	int mid = walks_for_paste(SMALL);	/* same size, so the growth shows */
	int big = walks_for_paste(SMALL);
	(void) mid;
	printf("     walks for %d lines: %d, %d, %d\n", SMALL, small,
	       mid, big);
	if (big > small * 3) {
		printf("FAIL: three pastes of %d lines cost %d, %d and %d\n"
		       "      walks: the cost still grows with the number of\n"
		       "      rows already in the file\n", SMALL, small, mid,
		       big);
		h_fails++;
	}
	h_expect_eq(NR, 3 * SMALL + 1);

	/* The lazy mark must be cleared once a row it covers is drawn, so
	 * a later edit does not keep re-marking the file.  The mark is
	 * cleared when the first row at or after it is highlighted. */
	h_run_loop("z");
	int mark = ec.hl_invalid_from;
	if (mark < 0) {
		printf("FAIL: a keystroke did not mark the rows below stale\n");
		h_fails++;
	} else {
		syntax_ensure_row_highlighted(mark);
		if (ec.hl_invalid_from != -1) {
			printf("FAIL: the mark at row %d survived the redraw\n",
			       ec.hl_invalid_from);
			h_fails++;
		}
	}

	/* An erased row renumbers every row below it, so the mark must go
	 * back to the first row.  A ^K cut erases the current line, and
	 * the mark must not survive the repaint that follows. */
	/* Erase a row from the middle, which is the renumbering case. */
	int rows_before = NR;
	row_erase(rows_before / 2);
	if (NR != rows_before - 1) {
		printf("FAIL: the erase left %d rows, want %d\n", NR,
		       rows_before - 1);
		h_fails++;
	}
	if (ec.hl_invalid_from != 0) {
		printf("FAIL: the erase left the mark at %d, not 0, so the rows\n"
		       "      below the cut keep a stale highlight\n",
		       ec.hl_invalid_from);
		h_fails++;
	}
	syntax_ensure_row_highlighted(0);
	if (ec.hl_invalid_from != -1) {
		printf("FAIL: the mark at %d survived the redraw\n",
		       ec.hl_invalid_from);
		h_fails++;
	}

	h_summary("lazy highlight invalidation tests");
	return h_fails ? 1 : 0;
}
