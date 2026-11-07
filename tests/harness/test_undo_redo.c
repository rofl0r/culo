/* Undo and redo through the real key decoder.
 *
 * These replace the expect shell tests in test-undo_redo.sh.
 * Each undo step is its own h_run() call, because one burst
 * of inserted text is exactly one undo step.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \033[F End   \033u M-U undo   \033e M-E redo
 *   \033a M-A mark   \013 ^K cut   \025 ^U paste   \017\r save
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <poll.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_undo_redo.c"

/* One burst of typed text is one undo step.  The main loop asks
 * poll() whether the tty still holds bytes, so the whole script
 * is one burst, the way a paste arrives.  h_run() alone would
 * make each character its own undo step. */
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
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs(body, f);
	fclose(f);
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	editor_refresh();
}

static char *read_file(void)
{
	FILE *f = fopen(FIXTURE_PATH, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
	buf[got] = '\0';
	fclose(f);
	return buf;
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* Basic undo: type at End, undo, the text is gone. */
	open_with("Original text\n");
	h_run("\033[F");
	h_run_burst(" added");
	h_expect_row(0, "Original text added");
	h_run("\033u");
	h_expect_row(0, "Original text");

	/* Redo brings it back. */
	h_run("\033e");
	h_expect_row(0, "Original text added");
	h_run("\033u");
	h_expect_row(0, "Original text");

	/* Multiple undos: three typed runs, three undos, back
	 * to the original text, and the file saves that way. */
	open_with("Start\n");
	h_run("\033[F");
	h_run_burst(" one");
	h_run_burst(" two");
	h_run_burst(" three");
	h_expect_row(0, "Start one two three");
	h_run("\033u");
	h_run("\033u");
	h_run("\033u");
	h_expect_row(0, "Start");
	h_run("\017\r");
	char *saved = read_file();
	h_expect_eq(saved != NULL && strcmp(saved, "Start\n") == 0, 1);
	free(saved);

	/* Undo after cut and paste: cut line 1, paste it after
	 * line 2, then undo the paste and the cut. */
	open_with("Line 1\nLine 2\nLine 3\n");
	h_run("\033a");
	h_run("\033[F");
	h_run("\013");		/* cut the marked line */
	h_expect_eq(NR, 3);
	h_expect_row(0, "");
	h_run("\033[B");
	h_run("\025");		/* paste */
	h_run("\033u");		/* undo the paste */
	h_run("\033u");		/* undo the cut */
	h_expect_eq(NR, 3);
	h_expect_row(0, "Line 1");
	h_expect_row(1, "Line 2");
	h_expect_row(2, "Line 3");

	h_summary("undo redo tests");
	return 0;
}
