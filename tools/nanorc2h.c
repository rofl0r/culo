#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../syntax/syntax_shared.h"

#define LINE_BUF_SIZE 8192
/* "(%s)|(%s)" + NUL */
#define MERGE_REGEX_OVERHEAD 6

typedef struct {
	color_id_t fg;
	color_id_t bg;
	char *regex;
} merged_rule_t;

typedef struct {
	char *file_type;
	char *file_regex;
	merged_rule_t *rules;
	size_t rule_count;
	size_t rule_cap;
} parse_result_t;

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
	/* Keep the last unescaped quote as delimiter because nanorc regexes may
	 * contain unescaped " inside character classes and alternations. */
	for (p = start + 1; *p; ++p) {
		int bs = 0;
		const char *q = p;
		if (*p != '"')
			continue;
		while (q > start + 1 && q[-1] == '\\') {
			++bs;
			--q;
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

static int ensure_rule_capacity(parse_result_t *pr)
{
	size_t new_cap;
	merged_rule_t *new_rules;

	if (pr->rule_count < pr->rule_cap)
		return 1;
	new_cap = pr->rule_cap ? pr->rule_cap * 2 : 8;
	new_rules =
	    realloc(pr->rules, new_cap * sizeof(pr->rules[0]));
	if (!new_rules)
		return 0;
	pr->rules = new_rules;
	pr->rule_cap = new_cap;
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

static void parse_syntax_line(parse_result_t *pr, char *line)
{
	char *p = skip_ws(line);
	char *name_start, *name_end;
	char *quoted;

	p += strlen("syntax");
	p = skip_ws(p);
	if (!*p)
		return;
	name_start = p;
	while (*p && !isspace((unsigned char) *p))
		++p;
	name_end = p;
	if (pr->file_type == NULL) {
		size_t len = (size_t) (name_end - name_start);
		pr->file_type = malloc(len + 1);
		if (!pr->file_type)
			return;
		memcpy(pr->file_type, name_start, len);
		pr->file_type[len] = '\0';
	}
	quoted = parse_quoted(p, NULL);
	if (quoted && !pr->file_regex) {
		pr->file_regex = quoted;
	} else {
		free(quoted);
	}
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
	color_id_t fg, bg;

	if (strstr(p, "start=") || strstr(p, "end="))
		return;
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
	regex = parse_quoted(p, NULL);
	if (!regex)
		return;
	if (!add_or_merge_rule(pr, fg, bg, regex))
		fprintf(stderr, "nanorc2h: failed to add rule with regex \"%s\"\n",
			regex);
	free(regex);
}

static const char *color_id_c_name(color_id_t id)
{
	switch (id) {
	case COLOR_NONE:
		return "COLOR_NONE";
	case COLOR_BLACK:
		return "COLOR_BLACK";
	case COLOR_RED:
		return "COLOR_RED";
	case COLOR_GREEN:
		return "COLOR_GREEN";
	case COLOR_YELLOW:
		return "COLOR_YELLOW";
	case COLOR_BLUE:
		return "COLOR_BLUE";
	case COLOR_MAGENTA:
		return "COLOR_MAGENTA";
	case COLOR_CYAN:
		return "COLOR_CYAN";
	case COLOR_WHITE:
		return "COLOR_WHITE";
	case COLOR_BRIGHTBLACK:
		return "COLOR_BRIGHTBLACK";
	case COLOR_BRIGHTRED:
		return "COLOR_BRIGHTRED";
	case COLOR_BRIGHTGREEN:
		return "COLOR_BRIGHTGREEN";
	case COLOR_BRIGHTYELLOW:
		return "COLOR_BRIGHTYELLOW";
	case COLOR_BRIGHTBLUE:
		return "COLOR_BRIGHTBLUE";
	case COLOR_BRIGHTMAGENTA:
		return "COLOR_BRIGHTMAGENTA";
	case COLOR_BRIGHTCYAN:
		return "COLOR_BRIGHTCYAN";
	case COLOR_BRIGHTWHITE:
		return "COLOR_BRIGHTWHITE";
	default:
		return "COLOR_NONE";
	}
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

static void emit_output(const parse_result_t *pr, const char *input_path)
{
	size_t i;
	printf("/* generated from %s */\n", input_path);
	printf("#include \"syntax/syntax_shared.h\"\n\n");
	printf("static const struct syntax_color_rule generated_rules[] = {\n");
	for (i = 0; i < pr->rule_count; ++i) {
		printf("\t{ %s, %s, ",
		       color_id_c_name(pr->rules[i].fg),
		       color_id_c_name(pr->rules[i].bg));
		emit_c_string(pr->rules[i].regex);
		printf(" },\n");
	}
	printf("};\n\n");
	printf("static const struct syntax_desc generated_syntax = {\n");
	printf("\t.file_type = ");
	emit_c_string(pr->file_type ? pr->file_type : "");
	printf(",\n");
	printf("\t.file_regex = ");
	emit_c_string(pr->file_regex ? pr->file_regex : "");
	printf(",\n");
	printf("\t.rules = generated_rules,\n");
	printf("\t.rule_count = sizeof(generated_rules) / sizeof(generated_rules[0]),\n");
	printf("};\n");
}

static void free_parse_result(parse_result_t *pr)
{
	size_t i;
	free(pr->file_type);
	free(pr->file_regex);
	for (i = 0; i < pr->rule_count; ++i)
		free(pr->rules[i].regex);
	free(pr->rules);
	memset(pr, 0, sizeof(*pr));
}

int main(int argc, char **argv)
{
	FILE *fp;
	char line[LINE_BUF_SIZE];
	parse_result_t pr = { 0 };

	if (argc != 2) {
		fprintf(stderr, "usage: %s <nanorc-file>\n", argv[0]);
		return 1;
	}

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
		if (starts_with_keyword(p, "color")) {
			parse_color_line(&pr, p);
			continue;
		}
		/* Intentionally ignore: header, magic, comment markers, and everything else. */
	}

	fclose(fp);
	emit_output(&pr, argv[1]);
	free_parse_result(&pr);
	return 0;
}
