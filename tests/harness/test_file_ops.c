/* File operations through the real key decoder.
 *
 * These replace the expect shell tests in test-file_operations.sh.
 * Each case drives editor_process_key() with scripted bytes, then
 * either checks row->chars directly or saves with ^O and reads the
 * file back, which is byte exact.  Input is fed one h_run() call
 * per key group on purpose: bytes that arrive in one burst are
 * pasted text, and a control or escape key in that burst is
 * dropped, which is how a keypress must arrive.
 *
 * Keys (octal escapes on purpose - see AGENTS.md):
 *   \017 ^O save   \r Enter   \177 backspace
 *   \033[C right   \033[D left   \033[F End
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_file_ops.c"

static void open_with(const char *body)
{
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	if (body)
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

static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
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
	if (out_len)
		*out_len = got;
	return buf;
}

static void expect_file(const char *want)
{
	size_t len = 0;
	char *got = read_file(FIXTURE_PATH, &len);
	size_t want_len = strlen(want);
	if (!got || len != want_len || memcmp(got, want, len) != 0) {
		printf("FAIL file: got %zu bytes [%s], want %zu [%s]\n",
		       len, got ? got : "<missing>", want_len, want);
		h_fails++;
	} else {
		printf("ok   file [%s]\n", want);
	}
	free(got);
}

/* ^O opens the save prompt prefilled with the file name, so an
 * Enter straight away accepts it.  The prompt reads its keys from
 * the same script, so both keys go in one h_run() call. */
static void save(void)
{
	h_run("\017\r");
	h_expect_eq(ec.modified, 0);
}

int main(void)
{
	mkdir("tests/test_tmp", 0777);	/* ok if it exists */

	/* Create and save a new file: the shell test wrote the file
	 * with echo and never drove the editor at all.  A new
	 * buffer has no line ending yet, so save writes no final
	 * newline; the shell test stripped it with $(cat). */
	open_with(NULL);
	h_expect_eq(NR, 0);
	h_run("Hello World");
	save();
	expect_file("Hello World");

	/* Open an existing file and save it unchanged. */
	open_with("Line 1\nLine 2\nLine 3\n");
	h_expect_eq(NR, 3);
	h_expect_row(0, "Line 1");
	save();
	expect_file("Line 1\nLine 2\nLine 3\n");

	/* Save modifications: type at End, save, read back. */
	open_with("Original text\n");
	h_run("\033[F");
	h_run(" added");
	save();
	expect_file("Original text added\n");

	/* Insert characters into an empty buffer. */
	open_with(NULL);
	h_run("hello");
	save();
	expect_file("hello");

	/* Delete a character: Right, Right, Backspace drops the
	 * second character, as in the shell test. */
	open_with("hi\n");
	h_run("\033[C");
	h_run("\033[C");
	h_run("\177");
	save();
	expect_file("h\n");

	/* Interleaved navigation and insertion.  This is the test
	 * that used to fail under expect: back to back sends looked
	 * like a paste burst and dropped every arrow key.  Split
	 * calls are the spaced sends, so each arrow is a keypress. */
	open_with(NULL);
	h_run("e");
	h_run("\033[D");
	h_run("t");
	h_run("\033[C");
	h_run("st");
	h_run("\033[D");
	h_run("\033[D");
	h_run("\033[D");
	h_run("\033[D");
	h_run("!!");
	h_run("\033[C");
	h_run("\033[C");
	h_run("\033[C");
	h_run("\033[C");
	h_run("!!");
	save();
	expect_file("!!test!!");

	/* A missing file is a new empty buffer.  file_open on a
	 * path that does not exist succeeds with no rows. */
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;
	h_release_buffer();
	editor_init();
	term_update_size();
	h_expect_eq(file_open("tests/test_tmp/no_such_file.c"), 1);
	h_expect_eq(NR, 0);
	editor_refresh();

	/* A large file: 1000 lines load, the last row is right. */
	{
		FILE *f = fopen(FIXTURE_PATH, "w");
		if (!f)
			exit(2);
		for (int i = 1; i <= 1000; i++)
			fprintf(f, "Line %d\n", i);
		fclose(f);
		h_release_buffer();
		editor_init();
		term_update_size();
		if (!file_open(FIXTURE_PATH))
			exit(2);
		editor_refresh();
		h_expect_eq(NR, 1000);
		h_expect_row(999, "Line 1000");
	}

	h_summary("file operation tests");
	return 0;
}
