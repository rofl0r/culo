/* Key-code probe: show what the terminal really sends.
 *
 *   ./keydump raw     hex-dump every raw input byte
 *   ./keydump decode  feed bytes through the editor's decoder
 *                     (links the real culo.c)
 *
 * Redirect stdout to a file, e.g.
 *   ./keydump raw > /tmp/keys.txt
 * then press the keys in question.  Ctrl-C (byte 03) quits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios kd_orig;

static void kd_raw(int on)
{
	struct termios r;
	if (tcgetattr(STDIN_FILENO, &kd_orig))
		exit(2);
	r = kd_orig;
	cfmakeraw(&r);
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &r))
		exit(2);
	(void)on;
}

static void kd_done(void)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &kd_orig);
}

static int kd_getbyte(void)
{
	unsigned char c;
	while (read(STDIN_FILENO, &c, 1) != 1)
		;
	return c;
}

static void dump_raw(void)
{
	int b;
	kd_raw(1);
	printf("raw mode: each byte as hex (printable shown), Ctrl-C quits\n");
	fflush(stdout);
	while ((b = kd_getbyte()) != 0x03) {
		if (b >= 0x20 && b < 0x7f)
			printf("%02x '%c'\n", b, b);
		else
			printf("%02x\n", b);
		fflush(stdout);
	}
	kd_done();
}

/* ---- decode mode: reuse the shipped decoder verbatim ---- */
#define main culo_main
#include "culo.c"
#undef main

static void print_key(int k)
{
	if (k == CTRL_('c')) {
		printf("^C\n");
		return;
	}
	if (k == 9) {
		printf("TAB (\\t)\n");
		return;
	}
	if (k == 32) {
		printf("SPACE (0x20)\n");
		return;
	}
	if (k & KEY_FLAG_SHIFT && k & KEY_FLAG_NAV) {
		int id = k & ~(KEY_FLAG_SHIFT | KEY_FLAG_NAV);
		if (id == 9)
			printf("SHIFT_TAB (%04x)\n", k);
		else if (id == 32)
			printf("SHIFT_SPACE (%04x)\n", k);
		else
			printf("SHIFT_NAV id=%d (%04x)\n", id, k);
		return;
	}
	if (k & KEY_FLAG_NAV) {
		printf("NAV id=%d (%04x)\n", k & ~KEY_FLAG_NAV, k);
		return;
	}
	if (k & 0x800) {
		printf("META '%c' (%04x)\n", (char)(k & 0xff), k);
		return;
	}
	if (k < 0x20 || k == 0x7f)
		printf("^%c (%02x)\n", k ? '@' + k : '?', k);
	else
		printf("'%c' (%02x)\n", (char)k, k);
}

static void dump_decode(void)
{
	int k;
	kd_raw(1);
	printf("decode mode: one line per decoded key, Ctrl-C quits\n");
	fflush(stdout);
	do {
		k = term_read_key();
		print_key(k);
		fflush(stdout);
	} while (k != CTRL_('c'));
	kd_done();
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "raw"))
		dump_raw();
	else if (argc > 1 && !strcmp(argv[1], "decode"))
		dump_decode();
	else {
		fprintf(stderr, "usage: %s raw|decode\n", argv[0]);
		return 1;
	}
	return 0;
}
