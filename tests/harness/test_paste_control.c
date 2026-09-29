/* A paste is text, not a stream of commands.
 *
 * The main loop opens a burst whenever input is already queued, so
 * every byte of a paste arrives with the burst open.  The key handler
 * never looked at that: it decoded the byte and dispatched it like a
 * keypress.  A control byte in the clipboard therefore ran a command.
 *
 * "AB", ^K, "C" pasted into a three-line file cut the first line and
 * left the clipboard holding "ABone\n".  "AB", ^X, "n" on a modified
 * buffer asked to quit and pulled the rest of the paste, including the
 * trailing n, into the confirmation dialog, so the editor hung in a
 * modal loop with no key left to press.
 *
 * Inside a burst only the bytes that belong in text are accepted: LF,
 * CR, tab and everything printable.  A control byte ends the burst and
 * is dropped, which is what the normal-mode default already did with
 * a control byte that arrived on its own.
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

#define FIXTURE_PATH "tests/test_tmp/h_pastectl.c"

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

static void show(const char *tag)
{
	printf("     %s: NR=%d records=%d", tag, NR, g_undo.count);
	for (int i = 0; i < NR; i++)
		printf(" [%s]", ROW(i)->chars);
	printf("\n");
}

static void open_with(void)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("one\ntwo\nthree\n", f);
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

/* Row text check that tolerates a missing row: a control byte that
 * still acts as a key can cut or erase rows, and the test should report
 * that rather than dereference NULL. */
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

/* Every one of these must leave the buffer exactly as the printable
 * text of the paste describes, and the clipboard alone. */
static void paste_with_control(int byte)
{
	char keys[8];
	keys[0] = 'A';
	keys[1] = 'B';
	keys[2] = (char)byte;
	keys[3] = 'C';
	keys[4] = '\0';
	open_with();
	h_run_burst(keys);
	show("after paste with a control byte");
	h_expect_eq(NR, 3);
	h_expect_row_safe(0, "ABCone");
	h_expect_row_safe(1, "two");
	h_expect_row_safe(2, "three");
	h_expect_ptr(ec.copied_char_buffer, NULL);
}



int main(void)
{
	mkdir("tests/test_tmp", 0777);

	/* --- A cut inside a paste must not cut ------------------------ */
	paste_with_control(CTRL_('k'));
	/* A paste, a replace prompt, a search prompt and a quit request. */
	paste_with_control(CTRL_('r'));
	paste_with_control(CTRL_('w'));
	paste_with_control(CTRL_('x'));
	paste_with_control(CTRL_('a'));
	paste_with_control(CTRL_('d'));
	paste_with_control(CTRL_('z'));	/* undo */
	paste_with_control(CTRL_('o'));	/* save */

	/* --- A ^X in a paste must not ask to quit --------------------- */
	/* The buffer is modified, so a ^X would open the save dialog and
	 * swallow the rest of the paste into it. */
	open_with();
	h_run_burst("zz");
	h_expect_eq(ec.modified, 1);
	h_run_burst("AB\030C");
	show("after paste with ^X");
	h_expect_eq(ec.mode, MODE_NORMAL);
	h_expect_eq(NR, 3);
	h_expect_row(0, "zzABCone");
	h_expect_eq(ec.status_msg[0] == '\0', 1);

	/* --- The bytes a paste may carry still work ------------------- */
	/* LF, tab and CR all survive, so the paste keeps its shape. */
	open_with();
	h_run_burst("a\nb\tc\r\nd");
	show("after multi-line paste");
	h_expect_eq(NR, 6);
	h_expect_row(0, "a");
	h_expect_row(1, "b\tc");
	h_expect_row(2, "");
	h_expect_row(3, "done");
	h_expect_row(4, "two");
	h_expect_row(5, "three");
	/* One burst, one undo step, however many newlines it carried. */
	h_expect_eq(g_undo.count, 1);
	h_run("\033u");		/* M-u: undo */
	h_expect_eq(NR, 3);
	h_expect_row(0, "one");
	h_expect_row(1, "two");
	h_expect_row(2, "three");

	/* --- A control byte typed on its own still reaches the command - */
	/* The burst is closed between keypresses, so ^K keeps cutting. */
	open_with();
	h_run("\013");		/* ^K */
	h_expect_eq(NR, 2);
	h_expect_row(0, "two");
	/* And the cut line is in the clipboard, as ^K always leaves it. */
	h_expect_eq(ec.copied_char_buffer != NULL, 1);
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;

	h_summary("a paste is text, not commands tests");
	return 0;
}
