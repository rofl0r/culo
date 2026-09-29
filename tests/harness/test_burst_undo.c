/* A burst is one undo step only while it is pure insertion.
 *
 * burst_open() records where the run started, the handlers append the
 * text they insert, and burst_close() writes one EDIT_INSERT item
 * covering the whole run.  That is right for a paste, and wrong for a
 * run that also cuts: the cut is suppressed by g_undo.batching, so it
 * is in no undo item at all, and the recorded row and column no longer
 * point at the text that was collected.
 *
 * Held down as one run: "xy", ^K, "z" on a two-line file.  The cut
 * erased the first row, so the burst text "xyz" was recorded against
 * row 0, column 0 of a buffer that no longer had that row.  Undo
 * deleted four bytes from the middle of "zghijkl" and left "ijkl".
 *
 * A cut, a paste, a delete or a cursor move now ends the burst, so the
 * key that broke it records its own edit and the rest of the run opens
 * a fresh one.  A newline is not a break: it is part of the pasted
 * text, and undo_apply_insert_text() replays it by splitting on '\n'.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_burst.c"

/* The main loop asks poll() whether the tty still holds bytes, so the
 * whole script is one burst, the way a paste arrives. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	static int (*real_poll)(struct pollfd *, nfds_t, int);
	if (!real_poll)
		real_poll = dlsym(RTLD_NEXT, "poll");
	if (fds && nfds == 1 && fds[0].fd == STDIN_FILENO) {
		int ready = h_keys != NULL && h_pos < strlen(h_keys);
		fds[0].revents = ready ? POLLIN : 0;
		return ready ? 1 : 0;
	}
	return real_poll(fds, nfds, timeout);
}

static void h_run_burst(const char *keys)
{
	h_keys = keys;
	h_pos = 0;
	h_exhausted = 0;
	while (h_keys[h_pos] && !h_exhausted) {
		if (input_pending())
			burst_open(ec.cursor_y, ec.cursor_x);
		editor_process_key();
		if (input_pending())
			continue;
		burst_close();
		editor_refresh();
	}
	h_reset_keys();
}

static void open_with(const char *body)
{
	/* A cut in an earlier section leaves text in the clipboard, and
	 * h_release_buffer() only frees the buffer, so drop it here or the
	 * next editor_init() loses the pointer and ASan reports a leak. */
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;
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

/* Row text check that tolerates a missing row: a broken undo can erase
 * the row a later check reads, and the test should report that rather
 * than dereference NULL. */
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

static void show(const char *tag)
{
	printf("     %s: NR=%d", tag, NR);
	for (int i = 0; i < NR; i++)
		printf(" [%s]", ROW(i)->chars);
	printf("\n");
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);

	/* --- A pure run stays one undo step --------------------------- */
	open_with("abcdef\nghijkl\n");
	h_run_burst("abc");
	h_expect_eq(g_undo.count, 1);
	h_expect_eq(g_undo.history[undo_history_index(0)].len, 3);
	h_run("\033u");		/* M-u: undo */
	h_expect_eq(NR, 2);
	h_expect_row_safe(0, "abcdef");
	/* A pasted LF is part of the same step, and the replay splits on
	 * '\n', so a multi-line paste is one item too. */
	open_with("abcdef\nghijkl\n");
	h_run_burst("a\nb\n");
	h_expect_eq(g_undo.count, 1);
	h_expect_eq(g_undo.history[undo_history_index(0)].len, 4);
	h_expect_eq(NR, 4);
	h_run("\033u");
	h_expect_eq(NR, 2);
	h_expect_row_safe(0, "abcdef");

	/* --- A cut inside a run must not swallow the run -------------- */
	open_with("abcdef\nghijkl\n");
	h_run_burst("xy\013z");	/* type, ^K, type: one run of keys */
	/* ^K leaves the cut line in the clipboard, and the clipboard is
	 * freed by the cleanup at the end of the test. */
	show("after run");
	/* The cut did what ^K always does, and "z" landed after it. */
	h_expect_eq(NR, 1);
	h_expect_row_safe(0, "zghijkl");
	/* The cut is undoable in its own right, and so is the text
	 * before and after it, so nothing in the run is lost. */
	int records = g_undo.count;
	h_expect_eq(records >= 2, 1);

	/* The run is three steps: "xy", the cut, then "z".  Undoing all
	 * three puts the file back exactly as it was, so no part of the run
	 * is lost and no step undoes the wrong bytes.  Before the fix one
	 * item covered the whole run, so the first undo deleted four bytes
	 * from the middle of the surviving row. */
	for (int i = 0; i < records; i++)
		h_run("\033u");
	h_expect_eq(NR, 2);
	h_expect_row_safe(0, "abcdef");
	h_expect_row_safe(1, "ghijkl");

	/* And redo it all: back to the state after the run. */
	for (int i = 0; i < records; i++)
		h_run("\033e");
	h_expect_eq(NR, 1);
	h_expect_row_safe(0, "zghijkl");

	/* --- A cursor move inside a run must not swallow the run ------ */
	/* A pasted ESC is a Meta key, not a cursor move, so this run is
	 * pure insertion and stays one step: "abc" then Meta-x. */
	open_with("abcdef\n");
	h_run_burst("abc\033x");
	show("after run");
	h_expect_row_safe(0, "abcabcdef");
	h_expect_eq(g_undo.count, 1);
	h_run("\033u");
	h_expect_row_safe(0, "abcdef");
	h_run("\033e");
	h_expect_row_safe(0, "abcabcdef");

	/* A cursor move in the middle.  h_run() feeds one key at a time
	 * with no input pending, so each key is its own burst: "abc", the
	 * arrow, then "x" at column 2.  The arrow is a separate undo step,
	 * and the two text runs are not merged into one item that would
	 * put the whole run at the start column. */
	open_with("abcdef\n");
	h_run("abc");
	h_run("\033[D");
	h_run("x");
	show("after move run");
	h_expect_row_safe(0, "abxcabcdef");
	/* One item per burst: "abc" never merges with "x", because the
	 * arrow between them is not an insertion. */
	int abc = g_undo.count;
	h_expect_eq(abc >= 2, 1);
	undo_item_t *last = &g_undo.history[undo_history_index(abc - 1)];
	h_expect_eq(last->type, EDIT_INSERT);
	h_expect_eq(last->col, 2);	/* "x" landed at column 2, not 0 */
	for (int i = 0; i < abc; i++)
		h_run("\033u");
	h_expect_eq(NR, 1);
	h_expect_row_safe(0, "abcdef");

	/* --- A delete inside a run must not swallow the run ----------- */
	/* 0x7f is DEL_KEY: it moves right and deletes.  Two of them in one
	 * run must each keep their own undo step, and the "z" that follows
	 * must not be folded into the "xy" that came before them. */
	open_with("abcdef\n");
	h_run_burst("xy\177\177z");
	show("after run");
	h_expect_row_safe(0, "zabcdef");
	records = g_undo.count;
	h_expect_eq(records >= 3, 1);
	for (int i = 0; i < records; i++)
		h_run("\033u");
	h_expect_eq(NR, 1);
	h_expect_row_safe(0, "abcdef");
	for (int i = 0; i < records; i++)
		h_run("\033e");
	h_expect_row_safe(0, "zabcdef");

	h_summary("burst undo granularity tests");
	return 0;
}
