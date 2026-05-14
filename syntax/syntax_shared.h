#ifndef SYNTAX_SHARED_H
#define SYNTAX_SHARED_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

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

struct syntax_desc {
	const char *file_type;
	const char *file_regex;
	const struct syntax_color_rule *rules;
	size_t rule_count;
};

static inline int color_id_to_ansi_fg_code(color_id_t id)
{
	if (id == COLOR_NONE)
		return -1;
	if (id >= COLOR_BLACK && id <= COLOR_WHITE)
		return 30 + (int) id;
	if (id >= COLOR_BRIGHTBLACK && id <= COLOR_BRIGHTWHITE)
		return 90 + ((int) id - (int) COLOR_BRIGHTBLACK);
	return -1;
}

static inline int color_id_to_ansi_bg_code(color_id_t id)
{
	if (id == COLOR_NONE)
		return -1;
	if (id >= COLOR_BLACK && id <= COLOR_WHITE)
		return 40 + (int) id;
	if (id >= COLOR_BRIGHTBLACK && id <= COLOR_BRIGHTWHITE)
		return 100 + ((int) id - (int) COLOR_BRIGHTBLACK);
	return -1;
}

static inline const char *color_id_to_vt100_fg(color_id_t id)
{
	switch (id) {
	case COLOR_NONE:
		return "";
	case COLOR_BLACK:
		return "\x1b[30m";
	case COLOR_RED:
		return "\x1b[31m";
	case COLOR_GREEN:
		return "\x1b[32m";
	case COLOR_YELLOW:
		return "\x1b[33m";
	case COLOR_BLUE:
		return "\x1b[34m";
	case COLOR_MAGENTA:
		return "\x1b[35m";
	case COLOR_CYAN:
		return "\x1b[36m";
	case COLOR_WHITE:
		return "\x1b[37m";
	case COLOR_BRIGHTBLACK:
		return "\x1b[90m";
	case COLOR_BRIGHTRED:
		return "\x1b[91m";
	case COLOR_BRIGHTGREEN:
		return "\x1b[92m";
	case COLOR_BRIGHTYELLOW:
		return "\x1b[93m";
	case COLOR_BRIGHTBLUE:
		return "\x1b[94m";
	case COLOR_BRIGHTMAGENTA:
		return "\x1b[95m";
	case COLOR_BRIGHTCYAN:
		return "\x1b[96m";
	case COLOR_BRIGHTWHITE:
		return "\x1b[97m";
	default:
		return "";
	}
}

static inline const char *color_id_to_vt100_bg(color_id_t id)
{
	switch (id) {
	case COLOR_NONE:
		return "";
	case COLOR_BLACK:
		return "\x1b[40m";
	case COLOR_RED:
		return "\x1b[41m";
	case COLOR_GREEN:
		return "\x1b[42m";
	case COLOR_YELLOW:
		return "\x1b[43m";
	case COLOR_BLUE:
		return "\x1b[44m";
	case COLOR_MAGENTA:
		return "\x1b[45m";
	case COLOR_CYAN:
		return "\x1b[46m";
	case COLOR_WHITE:
		return "\x1b[47m";
	case COLOR_BRIGHTBLACK:
		return "\x1b[100m";
	case COLOR_BRIGHTRED:
		return "\x1b[101m";
	case COLOR_BRIGHTGREEN:
		return "\x1b[102m";
	case COLOR_BRIGHTYELLOW:
		return "\x1b[103m";
	case COLOR_BRIGHTBLUE:
		return "\x1b[104m";
	case COLOR_BRIGHTMAGENTA:
		return "\x1b[105m";
	case COLOR_BRIGHTCYAN:
		return "\x1b[106m";
	case COLOR_BRIGHTWHITE:
		return "\x1b[107m";
	default:
		return "";
	}
}

static inline int str_eq_nocase(const char *a, const char *b)
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

static inline color_id_t color_id_from_name(const char *name)
{
	if (!name || !*name || str_eq_nocase(name, "normal")
	    || str_eq_nocase(name, "default"))
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

#endif
