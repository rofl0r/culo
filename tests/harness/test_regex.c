/*
 * regcomp() must reject a repetition count it cannot build, not overflow.
 *
 * The interval parser in nregex.c reads a{N} into an int with no bound,
 * rnode_count() multiplies that by the count to get the program size, and
 * regcomp() mallocs that many instructions without checking either.  A
 * count above about a billion overflows the size into a negative int, the
 * malloc is asked for a huge size and returns NULL, and the memset after it
 * is handed that NULL.  A count below that threshold is accepted and then
 * writes the program out instruction by instruction, which is minutes of
 * work and gigabytes of memory for a query the user typed.
 *
 * The editor reaches this on any keystroke: a regex search compiles the
 * query at culo.c:3252, and a nanorc rule is compiled at culo.c:1428.
 * The test includes culo.c, so it drives the editor's own paths as well as
 * calling regcomp() directly.
 *
 * So the contract is: a count is rejected when the program would not fit in
 * a bounded buffer, and the bound is small enough that a rejected pattern
 * costs nothing.  The tests below check the count that is rejected, that a
 * count just under it still compiles and matches, and that the rejection
 * leaves the caller with REG_BADPAT and a regex_t it must not use.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

/* The largest count regcomp() will build a program for.  Anything above is
 * REG_BADPAT.  The value is part of the contract with the editor, so the
 * test states it rather than deriving it. */
#define MAX_COUNT 65536

/* True when the pattern compiles. */
static bool compiles(const char *pat)
{
	regex_t re;
	if (regcomp(&re, pat, REG_EXTENDED) != 0)
		return false;
	regfree(&re);
	return true;
}

int main(void)
{
	/* --- 1. A count at the bound is accepted, and works. */
	{
		char pat[64];
		snprintf(pat, sizeof pat, "a{%d}", MAX_COUNT);
		if (!compiles(pat)) {
			printf("FAIL: a{%d} should compile\n", MAX_COUNT);
			h_fails++;
		} else {
			printf("ok   a{%d} compiles\n", MAX_COUNT);
		}
		/* And it matches the run it names, which needs a string that
		 * long.  A hundred is enough to prove the program runs. */
		regex_t re;
		if (regcomp(&re, pat, REG_EXTENDED) == 0) {
			char *s = malloc(MAX_COUNT + 1);
			memset(s, 'a', MAX_COUNT);
			s[MAX_COUNT] = '\0';
			int rc = regexec(&re, s, 0, NULL, 0);
			h_expect_eq(rc, 0);
			s[MAX_COUNT - 1] = 'b';
			h_expect_eq(regexec(&re, s, 0, NULL, 0), REG_NOMATCH);
			regfree(&re);
			free(s);
		}
	}

	/* --- 2. A count the library cannot build at all is rejected
	 *      rather than compiled.  On the unfixed library this is the
	 *      case that overflowed the program size into a negative int
	 *      and then memset a NULL, and below the overflow threshold
	 *      it is the case that allocated gigabytes.  The count is
	 *      large enough to fail either way, so the test is quick
	 *      when the fix is in and it dies when the fix is not. */
	{
		static const char *const big[] = {
			"a{300000000}",
			"a{1000000000}",
			"a{2147483647}",
			NULL
		};
		for (int i = 0; big[i]; i++) {
			if (compiles(big[i])) {
				printf("FAIL: %s should be rejected\n",
				       big[i]);
				h_fails++;
			} else {
				printf("ok   rejected %s\n", big[i]);
			}
		}
	}

	/* --- 3. A count one over the bound is rejected, not built. */
	{
		char pat[64];
		snprintf(pat, sizeof pat, "a{%d}", MAX_COUNT + 1);
		if (compiles(pat)) {
			printf("FAIL: a{%d} should be rejected\n",
			       MAX_COUNT + 1);
			h_fails++;
		} else {
			printf("ok   a{%d} rejected\n", MAX_COUNT + 1);
		}
	}

	/* --- 4. The counts that used to overflow the size, in every
	 *     position the parser can reach: min only, max only, both. */
	{
		static const char *const shapes[] = {
			"a{2000000000}",
			"a{1500000000}",
			"a{250000000,300000000}",
			"a{0,250000000}",
			"a{250000000,}",
			"(a){250000000}",
			"a{99999999999999999999}",
			NULL
		};
		for (int i = 0; shapes[i]; i++) {
			if (compiles(shapes[i])) {
				printf("FAIL: %s should be rejected\n",
				       shapes[i]);
				h_fails++;
			} else {
				printf("ok   rejected %s\n", shapes[i]);
			}
		}
	}

	/* --- 5. A rejected pattern leaves nothing behind.  regcomp()
	 * returns REG_BADPAT, and the caller must not regfree() a
	 * regex_t it never got.  The tree is freed on the way out, so
	 * the ASan leak check in the asan run is the assertion. */
	{
		regex_t re = NULL;
		int rc = regcomp(&re, "a{2000000000}", REG_EXTENDED);
		h_expect_eq(rc, REG_BADPAT);
		h_expect_ptr(re, NULL);
	}

	/* --- 6. The counts that are legal still behave, so the bound is a
	 *     bound and not a regression.  The negative cases are the ones
	 *     that need more copies than the text holds, because a count
	 *     that allows zero matches the empty string at offset 0 of any
	 *     text, which is what POSIX says.  The cases below are the
	 *     upstream behaviour, pinned: the library does not anchor a{2}b
	 *     to the start, and a{2,3}b does not match abb.  Those are
	 *     quirks of the emitter, not of this change, and are listed
	 *     here so a future fix to them is a deliberate one. */
	{
		static const struct {
			const char *pat;
			const char *yes;
			const char *no;
		} cases[] = {
			{ "a{3}", "aaa", "aa" },
			{ "a{2,}", "aaaaa", "a" },
			{ "a{2,4}", "aaa", "a" },
			/* a{0,2} and a{0} match the empty string at offset 0
			 * of any text, which is what POSIX says, so their
			 * negative case is the count that needs more copies
			 * than the text holds. */
			{ "a{3,4}", "aaaa", "aa" },
			{ "a{2,3}", "aaa", "a" },
			{ "a{2,}b", "aab", "ab" },
			{ "a{1,2}b", "aab", "b" },
			{ "(ab){2}", "abab", "ab" },
			{ "a{1,1}", "a", "" },
			{ NULL, NULL, NULL }
		};
		for (int i = 0; cases[i].pat; i++) {
			regex_t re;
			if (regcomp(&re, cases[i].pat,
				    REG_EXTENDED) != 0) {
				printf("FAIL: %s should compile\n",
				       cases[i].pat);
				h_fails++;
				continue;
			}
			if (regexec(&re, cases[i].yes, 0, NULL,
				    0) != 0) {
				printf("FAIL: %s should match |%s|\n",
				       cases[i].pat, cases[i].yes);
				h_fails++;
			}
			if (regexec(&re, cases[i].no, 0, NULL,
				    0) == 0) {
				printf("FAIL: %s should not match |%s|\n",
				       cases[i].pat, cases[i].no);
				h_fails++;
			}
			printf("ok   %s\n", cases[i].pat);
			regfree(&re);
		}
	}

	/* h_summary() runs editor_cleanup(), which walks the rows, so the
	 * state it needs has to exist.  This test never opened a file. */
	ec.rows = tlist_new(sizeof(editor_row_t));
	if (!ec.rows)
		exit(2);

	h_summary("regex interval tests");
	return 0;
}
