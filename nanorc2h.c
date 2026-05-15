#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LZ_COMPRESSOR
#include "lzcomp.h"
#include "syntax.h"

#define LINE_BUF_SIZE 8192
/* "(%s)|(%s)" + NUL */
#define MERGE_REGEX_OVERHEAD 6

typedef struct {
	color_id_t fg;
	color_id_t bg;
	char *regex;
} merged_rule_t;

typedef struct {
	color_id_t fg;
	color_id_t bg;
	char *start_regex;
	char *end_regex;
} span_rule_t;

typedef struct {
	char *file_regex;
	char *file_magic;
	const char *source_name;
	merged_rule_t *rules;
	size_t rule_count;
	size_t rule_cap;
	span_rule_t *span_rules;
	size_t span_rule_count;
	size_t span_rule_cap;
} parse_result_t;

typedef struct {
	unsigned char *data;
	size_t len;
	size_t cap;
} byte_buf_t;

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *out = malloc(n);
	if (!out)
		return NULL;
	memcpy(out, s, n);
	return out;
}

static char *skip_ws(char *s)
{
	while (*s && isspace((unsigned char) *s))
		++s;
	return s;
}

static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'
			 || isspace((unsigned char) s[n - 1]))) {
		s[n - 1] = '\0';
		--n;
	}
}

static int starts_with_keyword(const char *s, const char *kw)
{
	size_t n = strlen(kw);
	return strncmp(s, kw, n) == 0
	    && (s[n] == '\0' || isspace((unsigned char) s[n]));
}

static int str_eq_nocase(const char *a, const char *b)
{
	while (*a && *b) {
		int ca = tolower((unsigned char) *a);
		int cb = tolower((unsigned char) *b);
		if (ca != cb)
			return 0;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

static color_id_t color_id_from_name(const char *name)
{
	if (!name || !*name || str_eq_nocase(name, "normal") ||
		str_eq_nocase(name, "default"))
		return COLOR_NONE;
	if (str_eq_nocase(name, "black"))
		return COLOR_BLACK;
	if (str_eq_nocase(name, "red"))
		return COLOR_RED;
	if (str_eq_nocase(name, "green"))
		return COLOR_GREEN;
	if (str_eq_nocase(name, "yellow"))
		return COLOR_YELLOW;
	if (str_eq_nocase(name, "blue"))
		return COLOR_BLUE;
	if (str_eq_nocase(name, "magenta"))
		return COLOR_MAGENTA;
	if (str_eq_nocase(name, "cyan"))
		return COLOR_CYAN;
	if (str_eq_nocase(name, "white"))
		return COLOR_WHITE;
	if (str_eq_nocase(name, "brightblack"))
		return COLOR_BRIGHTBLACK;
	if (str_eq_nocase(name, "brightred"))
		return COLOR_BRIGHTRED;
	if (str_eq_nocase(name, "brightgreen"))
		return COLOR_BRIGHTGREEN;
	if (str_eq_nocase(name, "lime"))
		return COLOR_BRIGHTGREEN;
	if (str_eq_nocase(name, "brightyellow"))
		return COLOR_BRIGHTYELLOW;
	if (str_eq_nocase(name, "brightblue"))
		return COLOR_BRIGHTBLUE;
	if (str_eq_nocase(name, "brightmagenta"))
		return COLOR_BRIGHTMAGENTA;
	if (str_eq_nocase(name, "brightcyan"))
		return COLOR_BRIGHTCYAN;
	if (str_eq_nocase(name, "brightwhite"))
		return COLOR_BRIGHTWHITE;
	return COLOR_NONE;
}

static char *parse_quoted(const char *s, const char **next_out)
{
	const char *start = strchr(s, '"');
	const char *p;
	const char *end = NULL;
	char *out;
	size_t n;

	if (!start) {
		if (next_out)
			*next_out = s;
		return NULL;
	}
	for (p = start + 1; *p; ++p) {
		int bs = 0;
		const char *q = p;
		if (*p != '"')
			continue;
		while (q > start + 1 && q[-1] == '\\') {
			++bs;
			--q;
		}
		if ((bs % 2) == 0 &&
		    (p[1] == '\0' || isspace((unsigned char)p[1]))) {
			end = p;
			break;
		}
		if ((bs % 2) == 0)
			end = p;
	}
	if (!end) {
		if (next_out)
			*next_out = start;
		return NULL;
	}
	n = (size_t) (end - (start + 1));
	out = malloc(n + 1);
	if (!out)
		return NULL;
	memcpy(out, start + 1, n);
	out[n] = '\0';
	if (next_out)
		*next_out = end + 1;
	return out;
}

static char *html_unescape(const char *s)
{
	char *out = malloc(strlen(s) + 1);
	size_t o;
	if (!out)
		return NULL;
	o = 0;
	while (*s) {
		if (!strncmp(s, "&amp;", 5)) {
			out[o++] = '&';
			s += 5;
		} else if (!strncmp(s, "&lt;", 4)) {
			out[o++] = '<';
			s += 4;
		} else if (!strncmp(s, "&gt;", 4)) {
			out[o++] = '>';
			s += 4;
		} else {
			out[o++] = *s++;
		}
	}
	out[o] = '\0';
	return out;
}

static char *normalize_regex(const char *regex)
{
	return html_unescape(regex);
}

static int ensure_rule_capacity(parse_result_t *pr)
{
	size_t new_cap;
	merged_rule_t *new_rules;

	if (pr->rule_count < pr->rule_cap)
		return 1;
	new_cap = pr->rule_cap ? pr->rule_cap * 2 : 8;
	new_rules = realloc(pr->rules, new_cap * sizeof(pr->rules[0]));
	if (!new_rules)
		return 0;
	pr->rules = new_rules;
	pr->rule_cap = new_cap;
	return 1;
}

static int ensure_span_rule_capacity(parse_result_t *pr)
{
	size_t new_cap;
	span_rule_t *new_rules;

	if (pr->span_rule_count < pr->span_rule_cap)
		return 1;
	new_cap = pr->span_rule_cap ? pr->span_rule_cap * 2 : 8;
	new_rules =
	    realloc(pr->span_rules, new_cap * sizeof(pr->span_rules[0]));
	if (!new_rules)
		return 0;
	pr->span_rules = new_rules;
	pr->span_rule_cap = new_cap;
	return 1;
}

static int merge_regex(char **dst, const char *regex)
{
	size_t dst_len, regex_len, total;
	char *merged;

	if (!*dst) {
		*dst = xstrdup(regex);
		return *dst != NULL;
	}
	dst_len = strlen(*dst);
	regex_len = strlen(regex);
	total = dst_len + regex_len + MERGE_REGEX_OVERHEAD;
	merged = malloc(total);
	if (!merged)
		return 0;
	snprintf(merged, total, "(%s)|(%s)", *dst, regex);
	free(*dst);
	*dst = merged;
	return 1;
}

static int byte_buf_reserve(byte_buf_t *b, size_t need)
{
	size_t new_cap;
	unsigned char *new_data;

	if (need <= b->cap)
		return 1;
	new_cap = b->cap ? b->cap * 2 : 256;
	while (new_cap < need)
		new_cap *= 2;
	new_data = realloc(b->data, new_cap);
	if (!new_data)
		return 0;
	b->data = new_data;
	b->cap = new_cap;
	return 1;
}

static int byte_buf_append(byte_buf_t *b, const void *src, size_t n)
{
	if (!byte_buf_reserve(b, b->len + n))
		return 0;
	memcpy(b->data + b->len, src, n);
	b->len += n;
	return 1;
}

static int byte_buf_append_u32(byte_buf_t *b, uint32_t v)
{
	unsigned char le[4];
	le[0] = (unsigned char)(v & 0xff);
	le[1] = (unsigned char)((v >> 8) & 0xff);
	le[2] = (unsigned char)((v >> 16) & 0xff);
	le[3] = (unsigned char)((v >> 24) & 0xff);
	return byte_buf_append(b, le, sizeof(le));
}

static int add_or_merge_rule(parse_result_t *pr, color_id_t fg, color_id_t bg,
			     const char *regex)
{
	size_t i;
	for (i = 0; i < pr->rule_count; ++i) {
		if (pr->rules[i].fg == fg && pr->rules[i].bg == bg)
			return merge_regex(&pr->rules[i].regex, regex);
	}
	if (!ensure_rule_capacity(pr))
		return 0;
	pr->rules[pr->rule_count].fg = fg;
	pr->rules[pr->rule_count].bg = bg;
	pr->rules[pr->rule_count].regex = xstrdup(regex);
	if (!pr->rules[pr->rule_count].regex)
		return 0;
	++pr->rule_count;
	return 1;
}

static int add_span_rule(parse_result_t *pr, color_id_t fg, color_id_t bg,
			 const char *start_regex, const char *end_regex)
{
	if (!ensure_span_rule_capacity(pr))
		return 0;
	pr->span_rules[pr->span_rule_count].fg = fg;
	pr->span_rules[pr->span_rule_count].bg = bg;
	pr->span_rules[pr->span_rule_count].start_regex = xstrdup(start_regex);
	pr->span_rules[pr->span_rule_count].end_regex = xstrdup(end_regex);
	if (!pr->span_rules[pr->span_rule_count].start_regex ||
	    !pr->span_rules[pr->span_rule_count].end_regex)
		return 0;
	++pr->span_rule_count;
	return 1;
}

static void parse_syntax_line(parse_result_t *pr, char *line)
{
	char *p = skip_ws(line);
	char *quoted;

	if (pr->file_regex) {
		fprintf(stderr,
			"nanorc2h: duplicate syntax declaration in %s (ignored)\n",
			pr->source_name ? pr->source_name : "(unknown)");
		return;
	}
	p += strlen("syntax");
	p = skip_ws(p);
	while (*p && !isspace((unsigned char) *p))
		++p;
	quoted = parse_quoted(p, NULL);
	if (quoted)
		pr->file_regex = quoted;
}

static void parse_header_line(parse_result_t *pr, char *line)
{
	char *p = skip_ws(line);
	char *quoted;
	char *normalized;

	if (pr->file_magic) {
		fprintf(stderr,
			"nanorc2h: duplicate header declaration in %s (ignored)\n",
			pr->source_name ? pr->source_name : "(unknown)");
		return;
	}
	p += strlen("header");
	p = skip_ws(p);
	quoted = parse_quoted(p, NULL);
	if (!quoted)
		return;
	normalized = normalize_regex(quoted);
	free(quoted);
	if (!normalized)
		return;
	pr->file_magic = normalized;
}

static void parse_color_spec(const char *spec, color_id_t *fg, color_id_t *bg)
{
	const char *comma = strchr(spec, ',');
	char lhs[64];
	char rhs[64];

	*fg = COLOR_NONE;
	*bg = COLOR_NONE;
	if (!comma) {
		size_t n = strlen(spec);
		if (n >= sizeof(lhs))
			n = sizeof(lhs) - 1;
		memcpy(lhs, spec, n);
		lhs[n] = '\0';
		*fg = color_id_from_name(lhs);
		return;
	}
	if (comma != spec) {
		size_t n = (size_t) (comma - spec);
		if (n >= sizeof(lhs))
			n = sizeof(lhs) - 1;
		memcpy(lhs, spec, n);
		lhs[n] = '\0';
		*fg = color_id_from_name(lhs);
	}
	if (comma[1]) {
		size_t n = strlen(comma + 1);
		if (n >= sizeof(rhs))
			n = sizeof(rhs) - 1;
		memcpy(rhs, comma + 1, n);
		rhs[n] = '\0';
		*bg = color_id_from_name(rhs);
	}
}

static void parse_color_line(parse_result_t *pr, char *line)
{
	char *p = skip_ws(line);
	char *spec_start;
	char *spec_end;
	char spec[128];
	char *regex;
	char *normalized;
	char *start_regex = NULL;
	char *end_regex = NULL;
	char *normalized_start = NULL;
	char *normalized_end = NULL;
	color_id_t fg, bg;

	p += strlen("color");
	p = skip_ws(p);
	if (!*p)
		return;
	spec_start = p;
	while (*p && !isspace((unsigned char) *p))
		++p;
	spec_end = p;
	if (spec_end <= spec_start)
		return;
	{
		size_t n = (size_t) (spec_end - spec_start);
		if (n >= sizeof(spec))
			n = sizeof(spec) - 1;
		memcpy(spec, spec_start, n);
		spec[n] = '\0';
	}
	parse_color_spec(spec, &fg, &bg);
	if (fg == COLOR_NONE && bg == COLOR_NONE)
		return;
	if (strstr(p, "start=") && strstr(p, "end=")) {
		const char *start_pos = strstr(p, "start=");
		const char *end_pos = strstr(p, "end=");
		if (start_pos && end_pos && end_pos > start_pos) {
			size_t seg_len = end_pos - start_pos;
			char *segment = malloc(seg_len + 1);
			if (segment) {
				memcpy(segment, start_pos, seg_len);
				segment[seg_len] = '\0';
				start_regex = parse_quoted(segment, NULL);
				free(segment);
			}
		}
		if (end_pos)
			end_regex = parse_quoted(end_pos, NULL);
		if (!start_regex || !end_regex)
			goto out;
		normalized_start = normalize_regex(start_regex);
		normalized_end = normalize_regex(end_regex);
		if (!normalized_start || !normalized_end
		    || !add_span_rule(pr, fg, bg, normalized_start,
				      normalized_end))
			fprintf(stderr,
				"nanorc2h: failed to add span rule start=\"%s\" end=\"%s\"\n",
				start_regex ? start_regex : "",
				end_regex ? end_regex : "");
		goto out;
	}
	regex = parse_quoted(p, NULL);
	if (!regex)
		goto out;
	normalized = normalize_regex(regex);
	if (!normalized || !add_or_merge_rule(pr, fg, bg, normalized))
		fprintf(stderr, "nanorc2h: failed to add rule with regex \"%s\"\n",
			regex);
	free(normalized);
	free(regex);
 out:
	free(normalized_start);
	free(normalized_end);
	free(start_regex);
	free(end_regex);
}

static void emit_c_string(const char *s)
{
	const unsigned char *p = (const unsigned char *) s;
	putchar('"');
	while (*p) {
		switch (*p) {
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\r':
			fputs("\\r", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			if (*p < 0x20)
				printf("\\x%02x", *p);
			else
				putchar(*p);
			break;
		}
		++p;
	}
	putchar('"');
}

static int build_uncompressed_blob(const parse_result_t *pr, byte_buf_t *blob)
{
	size_t i;
	size_t total = pr->rule_count + pr->span_rule_count;

	if (!byte_buf_append_u32(blob, (uint32_t) total))
		return 0;
	for (i = 0; i < pr->rule_count; ++i) {
		uint32_t regex_len = (uint32_t)strlen(pr->rules[i].regex);
		if (!byte_buf_append_u32(blob, (uint32_t) pr->rules[i].fg) ||
		    !byte_buf_append_u32(blob, (uint32_t) pr->rules[i].bg) ||
		    !byte_buf_append_u32(blob, regex_len) ||
		    !byte_buf_append_u32(blob, 0) ||
		    !byte_buf_append(blob, pr->rules[i].regex, regex_len))
			return 0;
	}
	for (i = 0; i < pr->span_rule_count; ++i) {
		uint32_t start_len = (uint32_t)strlen(pr->span_rules[i].start_regex);
		uint32_t end_len = (uint32_t)strlen(pr->span_rules[i].end_regex);
		if (!byte_buf_append_u32(blob, (uint32_t) pr->span_rules[i].fg) ||
		    !byte_buf_append_u32(blob, (uint32_t) pr->span_rules[i].bg) ||
		    !byte_buf_append_u32(blob, start_len) ||
		    !byte_buf_append_u32(blob, end_len) ||
		    !byte_buf_append(blob, pr->span_rules[i].start_regex, start_len) ||
		    !byte_buf_append(blob, pr->span_rules[i].end_regex, end_len))
			return 0;
	}
	return 1;
}

static void emit_hex_bytes(const unsigned char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (i % 12 == 0)
			printf("\n\t\t");
		printf("0x%02x", (unsigned)data[i]);
		if (i + 1 < len)
			printf(", ");
	}
	if (len == 0)
		printf("\n\t\t0x00");
	printf("\n\t");
}

static void emit_output(const parse_result_t *pr)
{
	byte_buf_t uncompressed = {0};
	unsigned char *compressed = NULL;
	unsigned int compressed_cap;
	int compressed_len;
	size_t total = pr->rule_count + pr->span_rule_count;

	if (!build_uncompressed_blob(pr, &uncompressed)) {
		fprintf(stderr, "nanorc2h: failed to build syntax blob\n");
		free(uncompressed.data);
		exit(1);
	}
	compressed_cap = (unsigned int) (uncompressed.len + (uncompressed.len / 25) + 64);
	if (compressed_cap < 64)
		compressed_cap = 64;
	compressed = malloc(compressed_cap);
	if (!compressed) {
		fprintf(stderr, "nanorc2h: out of memory\n");
		free(uncompressed.data);
		exit(1);
	}
	compressed_len =
	    lz_compress(uncompressed.data, compressed, (unsigned int) uncompressed.len);
	if (compressed_len <= 0) {
		fprintf(stderr, "nanorc2h: compression failed\n");
		free(compressed);
		free(uncompressed.data);
		exit(1);
	}
	printf("{\n");
	printf("\t.file_regex = ");
	emit_c_string(pr->file_regex ? pr->file_regex : "");
	printf(",\n");
	printf("\t.file_magic = ");
	emit_c_string(pr->file_magic ? pr->file_magic : "");
	printf(",\n");
	printf("\t.rule_count = %zu,\n", total);
	printf("\t.compressed_size = %u,\n", (unsigned) compressed_len);
	printf("\t.uncompressed_size = %u,\n", (unsigned) uncompressed.len);
	printf("\t.compressed_data = (const unsigned char[]) {");
	emit_hex_bytes(compressed, (size_t) compressed_len);
	printf("},\n");
	printf("},\n");
	free(compressed);
	free(uncompressed.data);
}

static void free_parse_result(parse_result_t *pr)
{
	size_t i;
	free(pr->file_regex);
	free(pr->file_magic);
	for (i = 0; i < pr->rule_count; ++i)
		free(pr->rules[i].regex);
	for (i = 0; i < pr->span_rule_count; ++i) {
		free(pr->span_rules[i].start_regex);
		free(pr->span_rules[i].end_regex);
	}
	free(pr->rules);
	free(pr->span_rules);
	memset(pr, 0, sizeof(*pr));
}

int main(int argc, char **argv)
{
	FILE *fp;
	char line[LINE_BUF_SIZE];
	parse_result_t pr = {0};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <nanorc-file>\n", argv[0]);
		return 1;
	}

	pr.source_name = argv[1];
	fp = fopen(argv[1], "r");
	if (!fp) {
		perror("fopen");
		return 1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *p;
		if (!strchr(line, '\n') && !feof(fp)) {
			int c;
			fprintf(stderr, "nanorc2h: skipping oversized line\n");
			while ((c = fgetc(fp)) != EOF && c != '\n')
				;
			continue;
		}
		rstrip(line);
		p = skip_ws(line);
		if (*p == '\0' || *p == '#')
			continue;
		if (starts_with_keyword(p, "syntax")) {
			parse_syntax_line(&pr, p);
			continue;
		}
		if (starts_with_keyword(p, "header")) {
			parse_header_line(&pr, p);
			continue;
		}
		if (starts_with_keyword(p, "color")) {
			parse_color_line(&pr, p);
			continue;
		}
		/* Intentionally ignore comment markers and everything else. */
	}

	fclose(fp);
	emit_output(&pr);
	free_parse_result(&pr);
	return 0;
}
