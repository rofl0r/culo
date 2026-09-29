/* A failed open must not cost you the buffer.
 *
 * file_open() freed every row, cleared the undo history, replaced the
 * file name and picked a syntax before it tried to open anything.  A
 * directory or a file the user may not read then failed, and the
 * browser reported the failure and left an empty editor on screen:
 * every unsaved change was gone with no way back.  A missing file is
 * not a failure, so that path has to keep working.
 *
 * The open is now attempted before the buffer is touched.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_open.c"
#define DIR_PATH "tests/test_tmp"

static void open_ok(const char *body)
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
	open_ok("alpha\nbeta\n");
	h_expect_eq(NR, 2);

	/* An edit, so the buffer has something to lose: a modified file,
	 * a moved cursor and an undo entry. */
	h_run("X");
	h_expect_eq(ec.modified, 1);
	h_expect_row(0, "Xalpha");

	/* --- A directory keeps the buffer ----------------------------- */
	errno = 0;
	h_expect_eq(file_open(DIR_PATH), 0);
	h_expect_eq(NR, 2);
	h_expect_row(0, "Xalpha");
	h_expect_row(1, "beta");
	h_expect_eq(ec.cursor_y, 0);	/* not reset, the edit left it here */
	h_expect_eq(strcmp(ec.file_name, FIXTURE_PATH), 0);
	h_expect_eq(ec.modified, 1);	/* still modified */

	/* An edit made before the failed open is still undoable, so the
	 * undo history survived too. */
	h_run("\033u");
	h_expect_row(0, "alpha");

	/* --- A missing file is a new empty buffer ---------------------- */
	h_expect_eq(file_open("tests/test_tmp/does_not_exist.c"), 1);
	h_expect_eq(NR, 0);
	h_expect_eq(ec.modified, 0);
	h_expect_eq(strcmp(ec.file_name, "tests/test_tmp/does_not_exist.c"), 0);

	/* --- And a real open still works after both -------------------- */
	open_ok("one\ntwo\n");
	h_expect_eq(NR, 2);
	h_expect_row(0, "one");
	h_expect_row(1, "two");

	/* --- An unreadable file keeps the buffer as well --------------- */
	open_ok("keep\n");
	h_expect_eq(NR, 1);
	const char *locked = "tests/test_tmp/h_open_locked.c";
	FILE *f = fopen(locked, "w");
	if (!f)
		exit(2);
	fputs("secret\n", f);
	fclose(f);
	if (chmod(locked, 0) != 0)
		exit(2);
	if (geteuid() != 0) {	/* root ignores the permission bits */
		errno = 0;
		h_expect_eq(file_open(locked), 0);
		h_expect_eq(NR, 1);
		h_expect_row(0, "keep");
	} else {
		printf("skip  the unreadable file case: running as root\n");
	}
	chmod(locked, 0600);
	remove(locked);

	h_summary("failed open keeps the buffer tests");
	return 0;
}
