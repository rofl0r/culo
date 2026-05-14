#ifndef SYNTAX_H
#define SYNTAX_H

#include <stddef.h>

typedef enum color_id {
	COLOR_NONE = -1,
	COLOR_BLACK = 0,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_YELLOW,
	COLOR_BLUE,
	COLOR_MAGENTA,
	COLOR_CYAN,
	COLOR_WHITE,
	COLOR_BRIGHTBLACK,
	COLOR_BRIGHTRED,
	COLOR_BRIGHTGREEN,
	COLOR_BRIGHTYELLOW,
	COLOR_BRIGHTBLUE,
	COLOR_BRIGHTMAGENTA,
	COLOR_BRIGHTCYAN,
	COLOR_BRIGHTWHITE,
	COLOR_ID_COUNT
} color_id_t;

struct syntax_color_rule {
	color_id_t fg;
	color_id_t bg;
	const char *regex;
};

struct syntax_span_rule {
	color_id_t fg;
	color_id_t bg;
	const char *start_regex;
	const char *end_regex;
};

#define SYNTAX_MAX_RULES 128
#define SYNTAX_MAX_SPAN_RULES 64

struct syntax_desc {
	const char *file_regex;
	size_t rule_count;
	struct syntax_color_rule rules[SYNTAX_MAX_RULES];
	size_t span_rule_count;
	struct syntax_span_rule span_rules[SYNTAX_MAX_SPAN_RULES];
};

#endif
