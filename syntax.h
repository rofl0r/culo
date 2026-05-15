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

struct syntax_rule {
	color_id_t fg;
	color_id_t bg;
	const char *regex;
	const char *end_regex;
};

struct syntax_desc {
	const char *file_regex;
	const char *file_magic;
	size_t rule_count;
	unsigned int compressed_size;
	unsigned int uncompressed_size;
	const unsigned char *compressed_data;
};

#endif
