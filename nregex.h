/*
 * NEATLIBC C STANDARD LIBRARY
 *
 * Copyright (C) 2010-2020 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This regex implementation supports POSIX ERE syntax only.
 * regcomp() requires REG_EXTENDED and returns REG_BADPAT otherwise.
 *
 * Major missing POSIX features:
 * - POSIX BRE syntax and BRE-specific escaping rules
 * - Backreferences (\1, \2, ...)
 * - Collating elements/equivalence classes and locale-aware collation
 * - regerror() messages (currently a stub)
 */
#define REG_EXTENDED		0x01
#define REG_NOSUB		0x02
#define REG_ICASE		0x04
#define REG_NEWLINE		0x08
#define REG_NOTBOL		0x10
#define REG_NOTEOL		0x20
#define REG_NOMATCH		1
#define REG_BADPAT		2

typedef struct {
	long rm_so;
	long rm_eo;
} regmatch_t;

typedef struct regex *regex_t;

int regcomp(regex_t *restrict preg, const char *restrict regex, int cflags);
int regexec(const regex_t *restrict preg, const char *restrict str, int nmatch, regmatch_t pmatch[restrict], int eflags);
int regerror(int errcode, const regex_t *restrict preg, char *restrict errbuf, int errbuf_size);
void regfree(regex_t *preg);
