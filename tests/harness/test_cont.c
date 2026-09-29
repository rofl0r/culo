/* A Ctrl-Z must not leave raw mode, and the handler must not do it.
 *
 * sig_cont_handler() called term_disable_raw(), term_open_buffer() and
 * term_enable_raw() and then repainted, all from signal context.  A
 * repaint allocates and writes, tcsetattr is not async-signal-safe,
 * and term_enable_raw() registered the exit handler a second time and
 * read the state to restore back out of the terminal, which by then
 * held the raw settings this function had just installed.
 *
 * The handler now only raises a flag, and the repaint that follows
 * every key does the work.  term_enable_raw() takes the state to
 * restore once, so a second call cannot overwrite it.
 *
 * The editor runs on a pty here, so the terminal state can be read
 * back, and the handler is driven the way a signal drives it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_cont.c"

static int pty_master, pty_slave;
static struct termios pty_state;

static void pty_open(void)
{
	pty_master = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty_master < 0)
		exit(2);
	if (grantpt(pty_master) != 0 || unlockpt(pty_master) != 0)
		exit(2);
	pty_slave = open(ptsname(pty_master), O_RDWR | O_NOCTTY);
	if (pty_slave < 0)
		exit(2);
}

static bool pty_raw(void)
{
	if (tcgetattr(pty_slave, &pty_state) != 0)
		exit(2);
	return (pty_state.c_lflag & (ECHO | ICANON)) == 0;
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("int main(void)\n", f);
	fclose(f);

	pty_open();
	int before_raw = pty_raw();
	h_expect_eq(before_raw, 0);	/* the shell left it cooked */

	/* Run the editor with the pty slave as stdin, the way main() does. */
	if (dup2(pty_slave, STDIN_FILENO) < 0)
		exit(2);
	signal(SIGCONT, SIG_IGN);	/* the real handler needs the tty too */

	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);
	term_enable_raw();
	h_expect_eq(pty_raw(), 1);

	/* A Ctrl-Z and fg.  The handler only raises the flag, so the
	 * terminal is untouched until the main loop acts on it. */
	sig_cont_handler(0);
	h_expect_eq(g_cont_pending, 1);
	h_expect_eq(pty_raw(), 1);

	/* The repaint consumes the flag and takes raw mode back. */
	editor_refresh();
	h_expect_eq(g_cont_pending, 0);
	h_expect_eq(pty_raw(), 1);

	/* The state to restore is still the cooked one, so exiting puts
	 * the terminal back the way the shell left it.  term_enable_raw
	 * was called twice above, and the second call must not have read
	 * the raw settings back over it. */
	h_expect_eq(ec.orig_termios_valid, 1);
	if (tcsetattr(pty_slave, TCSAFLUSH, &ec.orig_termios) != 0)
		exit(2);
	h_expect_eq(pty_raw(), 0);
	/* And it is the state the shell had, not just any cooked state:
	 * the shell's line discipline has ISIG and ECHO set. */
	h_expect_eq((int)((ec.orig_termios.c_lflag & ISIG) != 0), 1);
	h_expect_eq((int)((ec.orig_termios.c_lflag & ECHO) != 0), 1);

	/* h_summary() exits, and the atexit handler that term_enable_raw
	 * registered will then run term_disable_raw() on stdin, which is
	 * the real terminal again and not a pty, so it panics.  Point it
	 * back at the pty for the exit, and check the failures here. */
	if (dup2(pty_slave, STDIN_FILENO) < 0)
		exit(2);
	h_summary("suspend and resume tests");
	return 0;
}
