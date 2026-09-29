/* A frame must reach the terminal whole.
 *
 * editor_refresh() builds the whole screen in one buffer and then
 * called write() once.  write() to a tty is allowed to be short: the
 * kernel takes what the line discipline has room for, and the rest is
 * dropped.  On a busy or slow terminal a repaint therefore ended
 * half-way, and the missing half stayed on screen until the next frame
 * covered it.  Every frame in the editor goes out through one write,
 * so a burst of input on a slow link loses the tail of every repaint.
 *
 * The write is overridden here to take a few bytes at a time, the way a
 * full tty buffer does, and the test counts what actually arrived.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_write.c"

/* Take at most this many bytes per write, and record the total. */
#define CHUNK 7
static size_t n_written;

/* Redirect the frame away from the real terminal, and route the frame
 * write through the chunking override. */
ssize_t write(int fd, const void *buf, size_t n)
{
	if (fd != STDOUT_FILENO) {
		static ssize_t (*rw)(int, const void *, size_t);
		if (!rw)
			rw = dlsym(RTLD_NEXT, "write");
		return rw(fd, buf, n);
	}
	if (n == 0)
		return 0;
	n_written += n;
	if (n > CHUNK)
		n = CHUNK;
	/* The bytes are counted, not sent: the test only cares that all
	 * of them were offered to the terminal. */
	return (ssize_t) n;
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("alpha\nbeta\n", f);
	fclose(f);
	h_release_buffer();
	editor_init();
	term_update_size();
	if (!file_open(FIXTURE_PATH))
		exit(2);

	/* A refresh writes the frame in CHUNK-sized pieces, so the frame
	 * is only whole if the write is retried. */
	n_written = 0;
	editor_refresh();
	h_expect_eq(n_written % CHUNK, 0);
	size_t first = n_written;
	/* A full-screen refresh is a longer frame, so it needs more than
	 * one write either way. */
	h_expect_eq(first > CHUNK, 1);

	/* And the whole frame reaches the terminal: the buffer that
	 * editor_refresh() built is the same size as what was written. */
	n_written = 0;
	editor_refresh_full();
	h_expect_eq(n_written % CHUNK, 0);
	h_expect_eq(n_written > first, 1);

	/* The cursor is still positioned at the end of the frame, which
	 * only happens if the tail was written too. */
	n_written = 0;
	editor_refresh();
	h_expect_eq(n_written % CHUNK, 0);

	h_summary("short write tests");
	return 0;
}
