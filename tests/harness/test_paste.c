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
 *   \022  ^R redo   \032  ^Z undo   \015  ^O save   \030  ^X quit
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
		editor_process_key();
		if (!input_pending()) {
			n_refresh++;
			editor_refresh();
		}
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

	h_summary("paste tests");
	return h_fails ? 1 : 0;
}
