/*
 * The string buffers grow by doubling, and a failed growth changes
 * nothing.
 *
 * The editor had three copies of the same growth code: the search query,
 * the replacement text, and the buffer ui_prompt() edits.  The two search
 * buffers grew by one byte per character, so typing a query reallocated
 * once per key and copied everything typed so far each time, and the
 * three copies disagreed about what a failed allocation should do.  One
 * helper now grows all three by doubling.
 *
 * Sections 1 to 5 drive the helper on a buffer of its own, so the realloc
 * count is exact: nothing else allocates.  They check the lengths where
 * an off-by-one either reallocates on every character or writes one byte
 * past the end, and the failure path, which must leave the text, the
 * length and the cap as they were.
 *
 * Section 6 checks the cap through the editor instead of counting
 * reallocs, because a repaint runs per keystroke and its frame buffer
 * takes over the address the query just released.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int sb_calls;		/* reallocs of the watched buffer */
static int sb_grows;		/* the ones that succeeded */
static int sb_fail_at;		/* fail this call, 0 for none */
static int sb_on;		/* count only while set: the C library
				 * reallocs too, and only the buffer under
				 * test is of interest */

/* Defined before culo.c is included so the helper sees it. */
void *realloc(void *p, size_t n)
{
	static void *(*real_realloc)(void *, size_t);
	if (!real_realloc)
		real_realloc = dlsym(RTLD_NEXT, "realloc");
	if (sb_on) {
		sb_calls++;
		if (sb_fail_at == sb_calls) {
			errno = ENOMEM;
			return NULL;
		}
		sb_grows++;
	}
	return real_realloc(p, n);
}

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

/* Sections 1 to 5 drive the helper directly.  On a tree without it the
 * pointer is NULL and the first assertion fails, so the test cannot
 * pass by accident against a tree that grows per byte. */
static bool (*const grow)(char **, size_t *, size_t *, int) = strbuf_grow;
static bool (*const reserve)(char **, size_t *, size_t *, size_t) =
    strbuf_reserve;

#define FIXTURE_PATH "tests/test_tmp/h_strbuf.c"

/* The buffer holds `len` bytes, every one of them `fill`, and a NUL. */
/* The first `want_len` bytes are all `fill`.  The buffer may be longer,
 * so no NUL is required at want_len: a test that checks the bytes before
 * a later append has not been terminated yet. */
static void expect_fill(const char *what, const char *buf, size_t want_len,
			char fill)
{
	if (!grow) {
		printf("FAIL: %s: the growth helper is not in the editor\n",
		       what);
		h_fails++;
		return;
	}
	if (!buf) {
		printf("FAIL: %s: no buffer\n", what);
		h_fails++;
		return;
	}
	for (size_t i = 0; i < want_len; i++) {
		if (buf[i] != fill) {
			printf("FAIL: %s: byte %zu is %d, want %d\n", what,
			       i, (int)(unsigned char)buf[i], (int)fill);
			h_fails++;
			return;
		}
	}
	printf("ok   %s: %zu bytes of %d\n", what, want_len, (int)fill);
}

/* The whole buffer holds `fill`, is NUL terminated at `len`, and the cap
 * can hold `len` bytes and the NUL. */
static void expect_all(const char *what, const char *buf, size_t len,
		       size_t cap, char fill)
{
	if (!buf) {
		printf("FAIL: %s: no buffer\n", what);
		h_fails++;
		return;
	}
	if (buf[len] != '\0') {
		printf("FAIL: %s: not NUL terminated at %zu\n", what, len);
		h_fails++;
		return;
	}
	if (cap < len + 1) {
		printf("FAIL: %s: cap %zu cannot hold %zu bytes and a NUL\n",
		       what, cap, len);
		h_fails++;
		return;
	}
	expect_fill(what, buf, len, fill);
}

/* The buffer holds `want`, and its length agrees. */
static void expect_text(const char *what, const char *buf, size_t len,
			size_t cap, const char *want)
{
	(void) cap;		/* not every caller knows the cap */
	if (!buf) {
		printf("FAIL: %s: no buffer\n", what);
		h_fails++;
		return;
	}
	if (strcmp(buf, want) != 0) {
		printf("FAIL: %s: got |%s| want |%s|\n", what, buf, want);
		h_fails++;
		return;
	}
	/* A length of 0 means the caller does not track one, which is the
	 * case for a string it only compared. */
	if (len && len != strlen(want)) {
		printf("FAIL: %s: len %zu does not match |%s|\n", what, len,
		       want);
		h_fails++;
		return;
	}
	printf("ok   %s: |%s|\n", what, want);
}

int main(void)
{
	/* --- 1. One byte makes one allocation of 128, and the next 126
	 *     bytes fit in it without another. */
	char *q = NULL;
	size_t qlen = 0, qcap = 0;
	sb_calls = sb_grows = 0;
	sb_on = 1;
	h_expect_eq(grow(&q, &qlen, &qcap, 'a'), true);
	for (int i = 0; i < 126; i++)
		grow(&q, &qlen, &qcap, 'a');
	sb_on = 0;
	expect_all("127 bytes in the first 128", q, qlen, qcap, 'a');
	h_expect_eq((int) qcap, 128);
	h_expect_eq(sb_grows, 1);	/* nothing reallocated inside the cap */

	/* --- 2. The byte that needs a bigger cap doubles it, and the text
	 *     before it is still there. */
	sb_calls = sb_grows = 0;
	sb_on = 1;
	h_expect_eq(grow(&q, &qlen, &qcap, 'b'), true);
	sb_on = 0;
	h_expect_eq(sb_grows, 1);
	h_expect_eq((int) qcap, 256);
	h_expect_eq(qlen, 128);
	h_expect_eq((int) q[0], 'a');
	h_expect_eq((int) q[126], 'a');
	h_expect_eq((int) q[127], 'b');

	/* --- 3. A long run crosses each cap once, not once per byte. */
	sb_calls = sb_grows = 0;
	sb_on = 1;
	for (int i = 0; i < 299; i++)
		grow(&q, &qlen, &qcap, 'c');
	sb_on = 0;
	/* 127 a, one b, 299 c: the first 127 are still 'a'. */
	h_expect_eq(qlen, 427);
	for (size_t i = 0; i < 127; i++) {
		if (q[i] != 'a') {
			printf("FAIL: byte %zu is %d, want 97\n", i,
			       (int)q[i]);
			h_fails++;
			break;
		}
	}
	if (q[127] != 'b' || q[128] != 'c' || q[427] != '\0') {
		printf("FAIL: 127=%d 128=%d 427=%d, want 98 99 0\n",
		       (int)q[127], (int)q[128], (int)q[427]);
		h_fails++;
	}
	h_expect_eq((int) qcap, 512);
	/* The run starts in a 256 cap and needs 429 slots, so it crosses
	 * 256 once.  The old per-byte growth did 300 reallocs here. */
	h_expect_eq(sb_grows, 1);

	/* --- 4. A failed growth appends nothing, changes no cap, and
	 *     leaves the text readable, so the caller can carry on. */
	free(q);
	q = NULL;
	qlen = 0;
	qcap = 0;
	for (int i = 0; i < 255; i++)
		grow(&q, &qlen, &qcap, 'a');
	expect_all("255 bytes in the first 256", q, qlen, qcap, 'a');
	h_expect_eq((int) qcap, 256);

	sb_calls = sb_grows = 0;
	sb_fail_at = 1;
	sb_on = 1;
	h_expect_eq(grow(&q, &qlen, &qcap, 'z'), false);
	sb_on = 0;
	sb_fail_at = 0;
	h_expect_eq(qlen, 255);		/* not appended */
	h_expect_eq((int) qcap, 256);	/* not resized */
	h_expect_eq(sb_grows, 0);	/* the failed call is not a grow */
	expect_all("after a failed grow", q, qlen, qcap, 'a');

	/* The buffer is still usable, and the byte the failed append would
	 * have taken is the byte the next append takes. */
	h_expect_eq(grow(&q, &qlen, &qcap, 'z'), true);
	/* The fill is the first 255 bytes; the new one follows it. */
	expect_fill("after a retry: the 255 a are intact", q, 255, 'a');
	h_expect_eq(qlen, 256);
	h_expect_eq((int) q[255], 'z');
	/* And the byte after that crosses the cap and grows it. */
	h_expect_eq(grow(&q, &qlen, &qcap, 'y'), true);
	h_expect_eq(qlen, 257);
	h_expect_eq((int) qcap, 512);
	h_expect_eq((int) q[256], 'y');
	free(q);

	/* --- 5. A failure with no buffer at all reports false and leaves
	 *     nothing to free, so the caller sees no buffer. */
	char *none = NULL;
	size_t nlen = 0, ncap = 0;
	sb_calls = sb_grows = 0;
	sb_fail_at = 1;
	sb_on = 1;
	h_expect_eq(grow(&none, &nlen, &ncap, 'a'), false);
	sb_on = 0;
	sb_fail_at = 0;
	h_expect_ptr(none, NULL);
	h_expect_eq((int) nlen, 0);
	h_expect_eq((int) ncap, 0);
	/* The first allocation after that succeeds and is the usual 128. */
	h_expect_eq(grow(&none, &nlen, &ncap, 'a'), true);
	expect_text("first allocation retried", none, nlen, ncap, "a");
	h_expect_eq((int) ncap, 128);
	free(none);

	/* --- 5b. reserve() makes room without writing, and the length the
	 *     caller then sets is its own.  ui_prompt() prefills its
	 *     buffer from the file name this way, and a prompt with an
	 *     init string must return that string, not an empty one. */
	{
		char *r = NULL;
		size_t rlen = 0, rcap = 0;
		h_expect_eq(reserve(&r, &rlen, &rcap, 0), true);
		h_expect_eq((int) rcap, 128);
		h_expect_eq(rlen, 0);
		/* A want that already fits changes nothing. */
		sb_calls = sb_grows = 0;
		sb_on = 1;
		h_expect_eq(reserve(&r, &rlen, &rcap, 100), true);
		sb_on = 0;
		h_expect_eq((int) rcap, 128);
		h_expect_eq(sb_grows, 0);
		/* A want that does not fit doubles until it does. */
		h_expect_eq(reserve(&r, &rlen, &rcap, 200), true);
		h_expect_eq((int) rcap, 256);
		h_expect_eq(reserve(&r, &rlen, &rcap, 300), true);
		h_expect_eq((int) rcap, 512);
		h_expect_eq(reserve(&r, &rlen, &rcap, 5000), true);
		h_expect_eq((int) rcap, 8192);
		/* reserve writes nothing, so the length is unchanged. */
		h_expect_eq(rlen, 0);
		/* And a failed reserve leaves the buffer alone: sb_fail_at
		 * counts reallocs of any pointer, and the prompt is not
		 * running, so the next realloc is the one to fail. */
		free(r);
		r = NULL;
		rlen = 0;
		rcap = 0;
		h_expect_eq(reserve(&r, &rlen, &rcap, 5000), true);
		sb_calls = sb_grows = 0;
		sb_fail_at = 1;
		sb_on = 1;
		h_expect_eq(reserve(&r, &rlen, &rcap, 100000), false);
		sb_on = 0;
		sb_fail_at = 0;
		h_expect_eq((int) rcap, 8192);
		h_expect_eq(rlen, 0);
		free(r);
	}

	/* --- 6. The search query takes the cap from the helper, so a long
	 *     query needs a doubled cap rather than one byte more than it
	 *     has.  The cap is the evidence, not a realloc count: a
	 *     repaint runs per keystroke and its frame takes over the
	 *     address the query just released. */
	mkdir("tests/test_tmp", 0777);
	FILE *f = fopen(FIXTURE_PATH, "w");
	if (!f)
		exit(2);
	fputs("needle needle needle\n", f);
	fclose(f);
	h_open(FIXTURE_PATH);

	/* Entering search mode with nothing typed allocates nothing. */
	h_run("\x17");
	h_expect_ptr(ec.search.query, NULL);
	h_expect_eq((int) ec.search.query_len, 0);
	h_expect_eq((int) ec.search.query_cap, 0);

	{
		char keys[128];
		memset(keys, 'e', 127);
		keys[127] = '\0';
		/* 127 characters fit in the first 128 byte cap. */
		h_run(keys);
		h_expect_eq((int) ec.search.query_len, 127);
		h_expect_eq((int) ec.search.query_cap, 128);
		/* 127 more cross it once: 254 bytes need 255 slots, so the
		 * cap is 256.  Per-byte growth would have made it 256 for
		 * the right reason and 202 for a 200 byte query, which is
		 * what the next run shows. */
		h_run(keys);
		h_expect_eq((int) ec.search.query_len, 254);
		h_expect_eq((int) ec.search.query_cap, 256);
		/* 254 more: 508 bytes need 509 slots, which the 512 cap
		 * holds, so the cap stays.  Cross it: 638 bytes need 639. */
		h_run(keys);
		h_run(keys);
		h_expect_eq((int) ec.search.query_len, 508);
		h_expect_eq((int) ec.search.query_cap, 512);
		h_expect_eq((int) ec.search.query[0], 'e');
		h_expect_eq((int) ec.search.query[508], 0);
		h_run(keys);
		h_expect_eq((int) ec.search.query_len, 635);
		h_expect_eq((int) ec.search.query_cap, 1024);
		/* Backspace shortens the text and the cap stays. */
		h_run("\x7f");
		h_expect_eq((int) ec.search.query_len, 634);
		h_expect_eq((int) ec.search.query_cap, 1024);
		h_expect_eq((int) ec.search.query[634], 0);
	}
	/* ^W cancels and the query persists for the next search. */
	h_run("\x17");
	h_expect_eq(ec.mode, MODE_NORMAL);
	h_expect_eq((int) ec.search.query_len, 634);
	h_expect_eq((int) ec.search.query_cap, 1024);

	/* An empty search finds nothing and leaves no buffer behind. */
	free(ec.search.query);
	ec.search.query = NULL;
	ec.search.query_len = 0;
	ec.search.query_cap = 0;
	h_run("\x17");
	h_run("\r");
	h_expect_eq(ec.mode, MODE_NORMAL);
	h_expect_ptr(ec.search.query, NULL);

	/* --- 8. A prompt that starts with a string returns that string.
	 *     This is the path ^O takes: the buffer is made from the file
	 *     name before any key arrives, so an Enter straight away
	 *     accepts that name and the save goes through.  Copying the
	 *     name through strbuf_grow() byte by byte left the buffer
	 *     empty, and the prompt never accepted anything. */
	{
		/* ui_prompt() reads keys on its own, so the script has to
		 * span the call.  An Enter straight away accepts whatever
		 * the prompt was given, which is what ^O relies on. */
		const char *saved = h_keys;
		size_t saved_pos = h_pos;
		int saved_exhausted = h_exhausted;
		char *name;
		h_keys = "\r";
		h_pos = 0;
		h_exhausted = 0;
		name = ui_prompt("Save as: ", "^C: cancel", "pin8.txt", NULL);
		h_keys = saved;
		h_pos = saved_pos;
		h_exhausted = saved_exhausted;
		expect_text("the prompt returns its init string", name, 0, 0,
			    "pin8.txt");
		free(name);

		/* A name typed over the init is appended, not dropped. */
		h_keys = "b\r";
		h_pos = 0;
		h_exhausted = 0;
		name = ui_prompt("Save as: ", "^C: cancel", "pin8.txt", NULL);
		h_keys = saved;
		h_pos = saved_pos;
		h_exhausted = saved_exhausted;
		expect_text("the prompt appends to its init string", name, 0, 0,
			    "pin8.txtb");
		free(name);

		/* ^C returns nothing, and frees the buffer. */
		h_keys = "\003";
		h_pos = 0;
		h_exhausted = 0;
		name = ui_prompt("Save as: ", "^C: cancel", "pin8.txt", NULL);
		h_keys = saved;
		h_pos = saved_pos;
		h_exhausted = saved_exhausted;
		h_expect_ptr(name, NULL);
	}
	h_summary("string buffer tests");
	return 0;
}
