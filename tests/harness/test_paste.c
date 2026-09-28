/* Paste-burst acceptance tests, driven in-process.
 *
 * Covers the three things that make a paste behave:
 *   1. Lines: a pasted LF splits a row, it does not land as a byte
 *   2. Cost:  a burst repaints once, not once per key
 *   3. Undo:  a burst is one undo step, and undo restores the buffer
 *
 * Feeding goes through the harness read() override, so the real key
 * decoder and the real main loop run.  poll() is overridden too, so
 * the loop sees "more input queued" exactly as it would with a real
 * tty, and the repaint count is the one the loop actually decides on.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033e M-e redo   \032  ^Z undo
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_paste.txt"

static int n_refresh;
static int n_poll;

/* The main loop asks poll() whether the tty still holds bytes.  Answer
 * from the script position instead, and count the decision. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	static int (*real_poll)(struct pollfd *, nfds_t, int);
	if (!real_poll)
		real_poll = dlsym(RTLD_NEXT, "poll");
	if (fds && nfds == 1 && fds[0].fd == STDIN_FILENO) {
		n_poll++;
		int ready = h_keys != NULL && h_pos < strlen(h_keys);
		fds[0].revents = ready ? POLLIN : 0;
		return ready ? 1 : 0;
	}
	return real_poll(fds, nfds, timeout);
}

/* Drive the main loop, the way main() does, counting repaints. */
static void h_run_loop(const char *keys)
{
	h_keys = keys;
	h_pos = 0;
	h_exhausted = 0;
	n_refresh = 0;
	while (h_keys[h_pos] && !h_exhausted) {
		if (input_pending())
			burst_open(ec.cursor_y, ec.cursor_x);
		editor_process_key();
		if (input_pending())
			continue;
		burst_close();
		n_refresh++;
		editor_refresh();
	}
	h_reset_keys();
}

static void open_clean(void)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("first\n", f);
	fclose(f);
	editor_init();
	term_update_size();
	ec.rows = tlist_new(sizeof(editor_row_t));
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

/* Row text check that tolerates a missing row, so a broken paste
 * reports a failure instead of dereferencing NULL. */
static void h_expect_row_safe(int y, const char *want)
{
	editor_row_t *row = (y >= 0 && y < NR) ? ROW(y) : NULL;
	if (!row) {
		printf("FAIL row %d: missing (NR=%d)\n", y + 1, NR);
		h_fails++;
		return;
	}
	h_expect_row(y, want);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */
	open_clean();

	/* --- 1. A pasted LF splits a row ------------------------------ */
	/* The cursor sits at the start of "first", so each pasted line
	 * pushes the rest of the file down.  Before the fix every byte
	 * landed in one row instead. */
	h_run_loop("alpha\nbeta\ngamma\n");
	h_expect_eq(NR, 4);
	h_expect_row_safe(0, "alpha");
	h_expect_row_safe(1, "beta");
	h_expect_row_safe(2, "gamma");
	h_expect_row_safe(3, "first");

	/* --- 2. A burst repaints once --------------------------------- */
	/* 400 keys, one repaint.  A per-key repaint would give 400. */
	char big[401];
	memset(big, 'x', 400);
	big[400] = '\0';
	h_run_loop(big);
	h_expect_eq(n_refresh, 1);
	h_expect_eq(NR, 4);	/* the burst must not add or lose rows */

	editor_row_t *r3 = (NR == 4) ? ROW(3) : NULL;
	h_expect_eq(r3 ? r3->size : -1, 405);	/* "first" + 400 x */

	/* --- 3. The whole run is one undo step ------------------------ */
	/* Three pasted lines then 400 characters: two records, because
	 * each paste is its own burst.  A per-key record would overrun
	 * the 64-slot ring and leave only the tail of the paste
	 * undoable, which is what typing into a file must not do. */
	h_expect_eq(g_undo.count, 2);
	h_expect_eq(g_undo.cursor, 2);
	h_expect_eq(g_undo.history[undo_history_index(0)].len, 17);
	h_expect_eq(g_undo.history[undo_history_index(1)].len, 400);

	h_run_loop("\032");	/* ^Z: drop the 400 x */
	h_expect_eq(NR, 4);
	h_expect_eq(g_undo.cursor, 1);
	editor_row_t *r3b = (NR == 4) ? ROW(3) : NULL;
	h_expect_eq(r3b ? r3b->size : -1, 5);	/* back to "first" */
	h_expect_row_safe(3, "first");

	h_run_loop("\032");	/* ^Z: drop the three pasted lines */
	h_expect_eq(NR, 1);
	h_expect_eq(g_undo.cursor, 0);	/* nothing applied, both redoable */
	h_expect_row_safe(0, "first");

	/* --- 4. Redo replays both steps, in order --------------------- */
	h_run_loop("\033e");	/* M-e: redo the three lines */
	h_expect_eq(NR, 4);
	h_expect_row_safe(0, "alpha");
	h_expect_row_safe(3, "first");
	h_run_loop("\033e");	/* M-e: redo the 400 x */
	h_expect_eq(NR, 4);
	h_expect_eq(g_undo.cursor, 2);
	editor_row_t *r3c = (NR == 4) ? ROW(3) : NULL;
	h_expect_eq(r3c ? r3c->size : -1, 405);

	h_summary("paste tests");
	return h_fails ? 1 : 0;
}
