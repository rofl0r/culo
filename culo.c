#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE	/* Enable SIGWINCH on macOS */
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE		/* strcasestr */
#endif

/* Standard library headers */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System headers */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include "nregex.h"
#include "syntax.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include <time.h>

/* Search mode flags (bitmask) */
typedef enum {
	SM_NONE = 0,
	SM_CASE_SENSITIVE = 1,
	SM_BACKWARDS = 2,
	SM_REPLACE = 4,
	SM_REGEX = 8,
} search_mode_t;

#define CTRL_(k) ((k) & (0x1f))
#define META_(k) (0x800 | (unsigned char)(k))
#define KEY_FLAG_NAV 0x1000
#define KEY_FLAG_SHIFT 0x2000
#define KEY_FLAG_CTRL 0x4000
#define KEY_NAV_(id) (KEY_FLAG_NAV | (id))
#define KEY_SHIFT_NAV_(id) (KEY_FLAG_SHIFT | KEY_NAV_(id))
#define KEY_CTRL_NAV_(id) (KEY_FLAG_CTRL | KEY_NAV_(id))
/* Synthetic keys: plain TAB/SPACE never carry these flags, so they can
 * never collide with real input bytes. */
#define SHIFT_TAB (KEY_SHIFT_NAV_(9))
#define SHIFT_SPACE (KEY_SHIFT_NAV_(32))
#define TAB_STOP 4
#define TAB_HEAD_STYLE "\x1b[90m"
#define UNDO_STACK_CAP 64
#define SYNTAX_AUTO_DISABLE_THRESHOLD (16u * 1024u * 1024u)

static inline int clamp(int value, int min, int max)
{
	/* Some callers pass runtime-derived maxima that can go negative. */
	if (max < min)
		max = min;
	if (value < min)
		return min;
	if (value > max)
		return max;
	return value;
}

/* UTF-8 handling functions */

/* Get the byte length of a UTF-8 character from its first byte */
static inline int utf8_byte_length(uint8_t c)
{
	if (!(c & 0x80))
		return 1;	/* ASCII */
	if ((c & 0xE0) == 0xC0 && c >= 0xC2)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0 && c <= 0xF4)
		return 4;
	return 1;		/* Invalid UTF-8, treat as single byte */
}

/* Check if byte is a UTF-8 continuation byte (10xxxxxx) */
static inline bool is_utf8_continuation(uint8_t c)
{
	return (c & 0xC0) == 0x80;
}

static int utf8_validate(const char *s, size_t max_len);

/* Get display width of a UTF-8 character (handles wide characters)
 * Returns 2 for CJK characters, 1 for most others, 0 for combining marks
 */
static inline int utf8_char_width_and_len(const char *s, size_t max_len,
					  int *char_len)
{
	unsigned char c = (unsigned char)*s;
	if (!(c & 0x80)) {
		*char_len = 1;
		/* C0 controls and DEL are non-printable and consume no screen cells. */
		if (c < 0x20 || c == 0x7F)
			return 0;
		return 1;
	}

	int len = utf8_validate(s, max_len);
	if (len == 0) {
		*char_len = 1;
		return 1;
	}
	*char_len = len;

	int codepoint;
	if (len == 2) {
		codepoint = ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
	} else if (len == 3) {
		codepoint = ((c & 0x0F) << 12) |
		    (((unsigned char)s[1] & 0x3F) << 6) |
		    ((unsigned char)s[2] & 0x3F);
	} else {
		codepoint = ((c & 0x07) << 18) |
		    (((unsigned char)s[1] & 0x3F) << 12) |
		    (((unsigned char)s[2] & 0x3F) << 6) |
		    ((unsigned char)s[3] & 0x3F);
	}

	if ((codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||	/* CJK Unified Ideographs */
	    (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||	/* CJK Extension A */
	    (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||	/* CJK Compatibility */
	    (codepoint >= 0x2E80 && codepoint <= 0x2EFF) ||	/* CJK Radicals */
	    (codepoint >= 0x3000 && codepoint <= 0x303F) ||	/* CJK Punctuation */
	    (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) {	/* Fullwidth forms */
		return 2;
	}

	/* Combining marks have zero width */
	if ((codepoint >= 0x0300 && codepoint <= 0x036F) ||	/* Combining Diacritical Marks */
	    (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||	/* Combining Diacritical Extended */
	    (codepoint >= 0x1DC0 && codepoint <= 0x1DFF)) {	/* Combining Diacritical Supplement */
		return 0;
	}

	return 1;
}

/* Move to the next UTF-8 character boundary */
static inline const char *utf8_next_char(const char *s)
{
	return *s ? s + utf8_byte_length((uint8_t) * s) : s;
}

/* Move to the previous UTF-8 character boundary */
static inline const char *utf8_prev_char(const char *start, const char *s)
{
	if (s <= start)
		return start;
	--s;
	while (s > start && is_utf8_continuation((uint8_t) * s))
		--s;
	return s;
}

/* Forward declaration needed early for debugging */
static void ui_set_message(const char *msg, ...);
static int ui_dialog_ask(const char *msg, char *const options[]);

/* Validate UTF-8 byte sequence and return its length
 * Returns 0 if invalid, otherwise returns number of bytes (1-4)
 */
static int utf8_validate(const char *s, size_t max_len)
{
	if (!s || max_len == 0)
		return 0;

	unsigned char c = (unsigned char)*s;

	/* ASCII character (0xxxxxxx) */
	if (c <= 0x7F)
		return 1;

	/* Invalid UTF-8 start byte */
	if (c < 0xC0 || c > 0xF7)
		return 0;

	/* 2-byte sequence (110xxxxx 10xxxxxx) */
	if (c <= 0xDF) {
		if (max_len < 2)
			return 0;
		if ((s[1] & 0xC0) != 0x80)
			return 0;
		/* Check for overlong encoding */
		if (c < 0xC2)
			return 0;
		return 2;
	}

	/* 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx) */
	if (c <= 0xEF) {
		if (max_len < 3)
			return 0;
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
			return 0;
		/* Check for overlong encoding and surrogates */
		if (c == 0xE0 && (unsigned char)s[1] < 0xA0)
			return 0;
		if (c == 0xED && (unsigned char)s[1] > 0x9F)
			return 0;
		return 3;
	}

	/* 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
	if (c <= 0xF4) {
		if (max_len < 4)
			return 0;
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
		    (s[3] & 0xC0) != 0x80)
			return 0;
		/* Check for overlong encoding and valid range */
		if (c == 0xF0 && (unsigned char)s[1] < 0x90)
			return 0;
		if (c == 0xF4 && (unsigned char)s[1] > 0x8F)
			return 0;
		return 4;
	}

	return 0;
}

/* Upstream tlist implementation:
 * https://github.com/rofl0r/libulz/blob/master/include/tlist.h */

#define TLIST_API static
#define TLIST_IMPL
#include "tlist.h"

typedef struct {
	int size;
	int render_size;
	char *chars;
	char *render;
	unsigned char *highlight;
	bool render_direct_map;
	bool hl_valid;
	bool span_open;
	int span_rule;
} editor_row_t;

/* X-macro for editor modes */
#define EDITOR_MODES                                  \
    _(NORMAL, "EDIT", "Default editing mode")         \
    _(SEARCH, "SEARCH", "Search mode (Ctrl-W)")       \
    _(SELECT, "SELECT", "Text selection mode")        \
    _(CONFIRM, "CONFIRM", "Confirmation dialog mode") \
    _(HELP, "HELP", "Help screen mode")               \
    _(BROWSER, "BROWSER", "File browser mode")

/* X-macro for key bindings - centralizes all shortcuts */
#define KEY_BINDINGS                      \
    _(QUIT, 'x', "Exit editor")           \
    _(SAVE, 'o', "Save file")             \
    _(FIND, 'w', "Search text")           \
    _(CUT, 'k', "Cut line/marked text")   \
    _(PASTE, 'u', "Paste/uncut")          \
    _(HELP, 'g', "Show help")

/* clang-format off */
typedef enum {
#define _(mode, name, desc) MODE_##mode,
	EDITOR_MODES
#undef _
	    MODE_COUNT		/* Total number of modes */
} editor_mode_t;
/* clang-format on */

typedef enum {
	LINE_ENDING_UNKNOWN = 0,
	LINE_ENDING_LF,
	LINE_ENDING_CR,
	LINE_ENDING_CRLF,
} line_ending_t;

/* Text selection state */
typedef struct {
	int start_x, start_y;	/* Selection start position */
	int end_x, end_y;	/* Selection end position */
	bool active;		/* Is selection active? */
} selection_state_t;

typedef struct {
	char name[NAME_MAX + 1];
	bool is_dir;
} browser_entry_t;

/* Mode-specific state data.  A union because only one mode is live at
 * a time, and the browser's current_dir is by far the largest member,
 * so a struct would waste it on the other four. */
typedef union {
	struct {
		int saved_x, saved_y, saved_col, saved_row;
	} search;
	struct {
		tlist *entries;	/* Browser entries */
		int selected;	/* Currently selected entry */
		int offset;	/* Scroll offset */
		char current_dir[PATH_MAX];	/* Current directory path */
		bool show_hidden;	/* Show hidden files (toggle with H) */
	} browser;
	struct {
		int offset;	/* Scroll offset (lines from top) */
	} help;
	struct {
		bool shift_only;	/* Selection was initiated by Shift+cursor keys */
	} select;
} mode_data_t;

/* Members of the union that outlive the mode that filled them.
 *
 * A union member read as a member that is not live is a garbage value,
 * and the editor had two such bugs before the owners below existed: the
 * search highlight is restored on the way out of search mode, but the
 * highlight only stays valid while no other mode has overwritten the
 * union; and the browser list has to be freed on a ^X from browser
 * mode, which is not the mode that filled it if a modal took over in
 * between.  These two fields are outside the union for that reason, so
 * the owner is named and there is nothing to read as anything else.
 */
typedef struct {
	/* Row index with the search highlight (-1 = none), and the copy of
	 * that row's highlight to put back when search mode ends.  Owned by
	 * editor_cleanup() and by mode_set() on the way out of search. */
	int search_highlight_line;
	char *search_saved_highlight;
	int search_saved_highlight_size;
	/* The browser entry list, owned by browser_free_entries() and
	 * editor_cleanup(). */
	tlist *browser_entries;
} mode_owners_t;

typedef struct {
	regex_t compiled;
	bool valid;
	unsigned char hl_code;
} compiled_rule_t;

typedef struct {
	regex_t start_compiled;
	regex_t end_compiled;
	bool valid_start;
	bool valid_end;
	unsigned char hl_code;
} compiled_span_rule_t;

/* Editor config structure */
struct {
	int cursor_x, cursor_y, render_x;
	int row_offset, col_offset;
	int screen_rows, screen_cols;
	tlist *rows;
	bool modified;
	char *file_name;
	char status_msg[512];	/* active prompt/dialog text shown in statusbar (not timed; use overlay_msg for transient messages) */
	char *copied_char_buffer;
	size_t file_size_bytes;
	int longest_line;	/* longest row in the buffer, in bytes */
	const struct syntax_desc *syntax;
	compiled_rule_t *syntax_compiled;
	size_t syntax_compiled_count;
	compiled_span_rule_t *syntax_span_compiled;
	size_t syntax_span_compiled_count;
	int hl_invalid_from;	/* first row with a stale highlight, -1 if none */
	struct {
		color_id_t fg;
		color_id_t bg;
	} syntax_palette[256];
	unsigned char syntax_palette_count;
	struct termios orig_termios;
	bool orig_termios_valid;	/* orig_termios holds the cooked state */
	/* Editor mode state machine */
	editor_mode_t mode;
	editor_mode_t prev_mode;	/* For returning from temporary modes */
	mode_data_t mode_state;	/* Mode-specific state data */
	mode_owners_t mode_owners;	/* members that outlive their mode */
	selection_state_t selection;	/* Text selection state */
	bool show_line_numbers;	/* Toggle line numbers display */
	bool show_whitespace;	/* Toggle tab/whitespace markers */
	bool last_was_cut;	/* True if previous key was ^K (for appending cuts) */
	bool read_only;		/* whether we can save to the original filename */
	line_ending_t line_ending;
	struct {
		char *query;	/* Persists across ^W invocations; NULL until first search */
		size_t query_len;
		size_t query_cap;
		bool prefill_from_start;	/* true: next printable key replaces prefilled query unless End was pressed */
		int prompt_query_start_col;	/* 1-based statusbar query start column (0 means inactive) */
		int mode;	/* search_mode_t bitmask */
		char *replace_query;	/* replacement text */
		size_t replace_len;
		size_t replace_cap;
		int replace_phase;	/* 0=search_input, 1=replace_input, 2=confirming each */
		int replace_count;	/* replacements made so far */
		int orig_row, orig_char;	/* cursor pos when replace phase 2 began; used to stop after one cycle */
		bool has_wrapped;	/* true once any search_do_from call in this session returned wrapped=true */
	} search;
	char overlay_msg[200];	/* transient overlay; cleared on next keypress */
	char browser_base_dir[PATH_MAX];
} ec;

static void init_ec(void)
{
	memset(&ec, 0, sizeof(ec));
	ec.mode = MODE_NORMAL;
	ec.prev_mode = MODE_NORMAL;
	snprintf(ec.browser_base_dir, sizeof(ec.browser_base_dir), ".");
	/* orig_row=-1 means "no active replace cycle"; orig_char is only
	 * meaningful when orig_row >= 0, so 0 is a fine default. */
	ec.search.orig_row = -1;
	/* -1 means "no row carries a stale highlight". */
	ec.hl_invalid_from = -1;
}

typedef enum {
	EDIT_INSERT = 0,
	EDIT_DELETE = 1,
	EDIT_REPLACE = 2,
	EDIT_BLOCK_INDENT = 3,
} edit_type_t;

typedef struct {
	edit_type_t type;
	int row, col;
	char *text;
	size_t len;
	size_t aux_len;
	int dir;		/* block indent: +1 indented, -1 unindented */
	int before_y, before_x;
	int after_y, after_x;
} undo_item_t;

typedef struct {
	undo_item_t history[UNDO_STACK_CAP];
	int start;		/* oldest entry index in ring */
	int count;		/* number of valid entries */
	int cursor;		/* number of applied entries from start; [0..count] */
	bool replaying;
	bool batching;
} undo_state_t;

static undo_state_t g_undo = { 0 };

/* Row access goes through these two names.  A test that defines
 * HL_COUNT_WALKS supplies its own wrappers and counts the treap walks,
 * which is how a per-keystroke walk of the row list stays visible. */
#ifndef HL_COUNT_WALKS
#define h_tlist_getsize tlist_getsize
#define h_tlist_get tlist_get
#endif

/* Number of rows */
#define NR ((int)h_tlist_getsize(ec.rows))

/* Pointer to the editor_row_t at index i */
static inline editor_row_t *ROW(int i)
{
	if (!ec.rows || i < 0 || i >= NR)
		return NULL;
	return (editor_row_t *) h_tlist_get(ec.rows, (size_t) (i));
}

static void hl_invalidate_from(int row_idx);
static void hl_mark_all(void);
static int rows_longest(void);

/* Manual typing growth alignment (power of two). */
#define ROW_MANUAL_ALLOC_ALIGN 64

static inline size_t row_manual_alloc_size(size_t content_len)
{
	size_t need = content_len + 1;	/* include NUL */
	size_t a = ROW_MANUAL_ALLOC_ALIGN;
	return (need + (a - 1)) & ~(a - 1);
}

/* Free a row's heap-owned sub-fields (NOT the row struct itself, which is
 * owned by the tlist node). */
static void row_free_contents(editor_row_t * row)
{
	free(row->chars);
	row->chars = NULL;
	free(row->render);
	row->render = NULL;
	free(row->highlight);
	row->highlight = NULL;
}

/* Delete row at index `at`, freeing its contents first, then removing the
 * tlist node. */
static void row_erase(int at)
{
	editor_row_t *r = ROW(at);
	if (r)
		row_free_contents(r);
	/* Every row above moves down one index, so any row below keeps
	 * its bytes but changes index, and a span rule that started or
	 * ended in the erased row changes state for all of them. */
	hl_mark_all();
	tlist_delete(ec.rows, (size_t) at);
	/* A removed row may have been the longest, and the rows below it
	 * have all moved up, so the width is measured again.  This is the
	 * one place a whole-buffer walk is worth it: the erase is rare and
	 * the counter would otherwise keep the width of a line that is
	 * gone. */
	ec.longest_line = rows_longest();
}

typedef struct {
	char *buf;
	int len;
	int cap;		/* bytes allocated for buf */
} editor_buf_t;

/* clang-format off */
enum editor_key {
	BACKSPACE = 0x7f,
	ARROW_LEFT = KEY_NAV_(1), ARROW_RIGHT = KEY_NAV_(2),
	ARROW_UP = KEY_NAV_(3), ARROW_DOWN = KEY_NAV_(4),
	PAGE_UP = KEY_NAV_(5), PAGE_DOWN = KEY_NAV_(6),
	HOME_KEY = KEY_NAV_(7), END_KEY = KEY_NAV_(8), DEL_KEY = KEY_NAV_(9),
	SHIFT_ARROW_LEFT = KEY_SHIFT_NAV_(1), SHIFT_ARROW_RIGHT =
	    KEY_SHIFT_NAV_(2),
	SHIFT_ARROW_UP = KEY_SHIFT_NAV_(3), SHIFT_ARROW_DOWN = KEY_SHIFT_NAV_(4),
	SHIFT_PAGE_UP = KEY_SHIFT_NAV_(5), SHIFT_PAGE_DOWN = KEY_SHIFT_NAV_(6),
	SHIFT_HOME_KEY = KEY_SHIFT_NAV_(7), SHIFT_END_KEY =
	    KEY_SHIFT_NAV_(8),
	CTRL_ARROW_LEFT = KEY_CTRL_NAV_(1), CTRL_ARROW_RIGHT =
	    KEY_CTRL_NAV_(2),
};

typedef enum {
	NORMAL = 0,
	MATCH = 1,
	HIGHLIGHT_DYNAMIC_BASE = 2
} highlight_type_t;
/* clang-format on */

const struct syntax_desc syntax_rules[] = {
#include "nanorc.h"
	/* terminating sentinel */
	{.file_regex = NULL, .rule_count = 0, .rules = NULL}
};

static char *ui_prompt(const char *prefix, const char *hint, const char *init, void (*callback) (char *, int));
static void editor_refresh(void);
/* Set by the SIGWINCH handler, acted on by editor_refresh. */
static volatile sig_atomic_t g_resize_pending;
/* Set by the SIGCONT handler, acted on by editor_refresh.  sig_atomic_t
 * is the only type a signal handler may touch. */
static volatile sig_atomic_t g_cont_pending;
static int get_line_number_width(void);
static void editor_newline(void);
static void burst_append(const char *s, size_t n);
static void burst_break(void);
static void burst_close(void);
static void editor_insert_char(int c, bool manual_typing);
static void editor_move_cursor(int key);
static void editor_move_word(int dir);
static void undo_record_insert(int row, int col, const char *text, size_t len,
			       int before_y, int before_x, int after_y,
			       int after_x);
static void undo_record_delete(int row, int col, const char *text, size_t len,
			       int before_y, int before_x, int after_y,
			       int after_x);
static void undo_record_replace(int row, int col, const char *old_text,
				size_t old_len, const char *new_text,
				size_t new_len, int before_y, int before_x,
				int after_y, int after_x);
static void undo_perform(bool redo);
static bool undo_insert_bytes(int row_idx, int col, const char *s,
			      size_t len);
static bool undo_delete_bytes(int row_idx, int col, size_t len);
static void selection_indent(int dir, const char *indent);
static bool undo_apply_block_indent(const undo_item_t * item, bool redo);

/* Mode management implementation */
static void mode_set(editor_mode_t new_mode)
{
	/* Save current mode as previous (unless entering temporary mode) */
	if (ec.mode != MODE_CONFIRM && ec.mode != MODE_HELP)
		ec.prev_mode = ec.mode;

	/* Clean up old mode state */
	switch (ec.mode) {
	case MODE_SEARCH:
		/* ec.search.query is persistent — do NOT free it here */
		/* Restore saved highlight if leaving search mode */
		if (ec.mode_owners.search_saved_highlight &&
		    ec.mode_owners.search_highlight_line >= 0 &&
		    ec.mode_owners.search_highlight_line < NR &&
		    ROW(ec.mode_owners.search_highlight_line)->highlight &&
		    ec.mode_owners.search_saved_highlight_size ==
		    ROW(ec.mode_owners.search_highlight_line)->render_size) {
			memcpy(ROW(ec.mode_owners.search_highlight_line)->
			       highlight, ec.mode_owners.search_saved_highlight,
			       ROW(ec.mode_owners.search_highlight_line)->
			       render_size);
		}
		free(ec.mode_owners.search_saved_highlight);
		ec.mode_owners.search_saved_highlight = NULL;
		break;
	default:
		break;
	}

	/* Initialize new mode */
	ec.mode = new_mode;
	memset(&ec.mode_state, 0, sizeof(ec.mode_state));

	/* Mode-specific initialization */
	switch (new_mode) {
	case MODE_SELECT:
		/* Ensure cursor is in valid position before starting selection */
		if (ec.cursor_y >= NR && NR > 0) {
			ec.cursor_y = NR - 1;
			ec.cursor_x = ROW(ec.cursor_y)->size;
		}
		ec.selection.start_x = ec.cursor_x;
		ec.selection.start_y = ec.cursor_y;
		ec.selection.end_x = ec.cursor_x;
		ec.selection.end_y = ec.cursor_y;
		ec.selection.active = true;
		ui_set_message
		    ("-- SELECT MODE -- Use arrows to extend, ^C to cancel");
		break;
	case MODE_SEARCH:{
			/* The query buffer is made by strbuf_grow() on the first
			 * character typed, so there is nothing to allocate here.
			 * A NULL query with a zero length is the state before the
			 * first key, and every reader already handles it. */
			/* Reset replace phase each time search mode is entered */
			ec.search.replace_phase = 0;
			ec.search.replace_count = 0;
			ec.search.prefill_from_start = (ec.search.query_len > 0);
			/* A new search starts with no saved highlight.  A copy
			 * from an earlier search can still be here, because
			 * the highlight is stored before the mode is entered
			 * on some paths, so it is released rather than
			 * dropped. */
			free(ec.mode_owners.search_saved_highlight);
			ec.mode_owners.search_saved_highlight = NULL;
			ec.mode_owners.search_highlight_line = -1;
			ec.mode_owners.search_saved_highlight_size = 0;
			break;
		}
	case MODE_HELP:
		ec.mode_state.help.offset = 0;
		ui_set_message
		    ("^X or ^G or ^C to exit, arrows/PgUp/PgDn to scroll");
		break;
	case MODE_NORMAL:
		ec.selection.active = false;
		ui_set_message("");
		break;
	default:
		break;
	}
}

static void mode_restore(void)
{
	mode_set(ec.prev_mode);
}

static const char *mode_get_name(editor_mode_t mode)
{
	if (mode >= 0 && mode < MODE_COUNT) {
		/* Generate mode names using X-macro */
		static const char *mode_names[] = {
#define _(mode, name, desc) [MODE_##mode] = name,
			EDITOR_MODES
#undef _
		};
		return mode_names[mode];
	}
	return "UNKNOWN";
}

/* Static help content shown by ^G */
static const char *const help_lines[] = {
	"File:",
	"  ^X      Exit (prompts to save if modified)",
	"  ^O      Write/save file",
	"  M-B     Open file browser",
	"",
	"Editing:",
	"  ^K      Cut current line (consecutive ^K appends to cut buffer)",
	"  ^U      Uncut/paste",
	"  M-U     Undo last edit",
	"  M-E     Redo last undo",
	"  ^Z/^Y   Undo/redo aliases",
	"  M-A     Set/toggle mark",
	"  Shift+Cursor transient mark (clears on first non-shift-cursor key)",
	"  M-6     Copy marked region",
	"  Tab     Indent selected lines by one tab (M-A or Shift+Cursor mark)",
	"  S-Tab   Remove one tab from selected lines",
	"  Space   Indent selected lines by one space (repeat for more)",
	"  M-Space Remove one leading space from selected lines",
	"",
	"Search:",
	"  ^W      Find text (Where is)",
	"  ^R      Toggle replace mode (press Enter after search term to prompt for replacement)",
	"  M-C     Toggle case sensitivity (default: case-insensitive)",
	"  M-B     Toggle backwards search",
	"  M-R     Toggle regex mode",
	"",
	"Navigation:",
	"  Arrows  Move cursor",
	"  ^Left   Jump to previous word",
	"  ^Right  Jump to next word",
	"  PgUp    Page up",
	"  PgDn    Page down",
	"  Home    Beginning of line",
	"  End     End of line",
	"  M-\\     Go to first line of file",
	"  M-/     Go to last line of file",
	"  M-G     Go to line number",
	"  M-]     Go to matching bracket",
	"",
	"View:",
	"  M-#     Toggle line numbers",
	"  M-P     Toggle whitespace display",
	"  M-Y     Toggle syntax highlighting",
	"  ^G      Show this help screen",
	"",
};

#define HELP_NUM_LINES (int)(sizeof(help_lines) / sizeof(help_lines[0]))

/* Write the whole buffer, even when the tty takes it in pieces.  A
 * frame is one repaint of the whole screen, and a short write on a busy
 * or slow tty would leave the rest of it on the next frame, so the tail
 * is written until nothing is left.  A write that fails outright is
 * reported by the caller, so a closed terminal does not spin here. */
static void term_write_all(const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(STDOUT_FILENO, buf + off, len - off);
		if (n > 0) {
			off += (size_t) n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;	/* a signal landed mid-write */
		return;		/* the terminal is gone; nothing to do */
	}
}

static void term_clear(void)
{
	term_write_all("\x1b[2J", 4);
	term_write_all("\x1b[1;1H", 6);
}

static void perr(const char *s) {
	while(*s && *s != '%') write(STDOUT_FILENO, s++, 1);
	s = strerror(errno);
	while(*s) write(STDOUT_FILENO, s++, 1);
	write(STDOUT_FILENO, "\n", 1);
}

static void panic(const char *s)
{
	term_clear();
	perr(s);
	exit(1);
}

static void term_open_buffer(void)
{
	if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
		panic("Error changing terminal buffer: ");
}

static void term_disable_raw(void)
{
	/* TCSANOW, not TCSAFLUSH: flush discards queued input, and a
	 * resume must not drop keys typed while suspended. */
	if (tcsetattr(STDIN_FILENO, TCSANOW, &ec.orig_termios) == -1)
		panic("Failed to disable raw mode: ");
}

static void term_enable_raw(void)
{
	struct termios raw;
	/* The state to restore is taken once, from the terminal as the
	 * shell left it.  A second call, which the resume after a Ctrl-Z
	 * makes, would otherwise read back the raw settings this function
	 * installed and store those, so the exit handler would put the
	 * terminal back in raw mode.  The atexit registration is per
	 * session for the same reason: twice would run the restore twice.
	 */
	if (tcgetattr(STDIN_FILENO, &raw) == -1)
		panic("Failed to get current terminal state: ");
	if (!ec.orig_termios_valid) {
		ec.orig_termios = raw;
		ec.orig_termios_valid = true;
		atexit(term_disable_raw);
	}
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	term_open_buffer();
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
		panic("Failed to set raw mode: ");
}

/* Report whether more input bytes are already waiting on stdin.
 * A pasted burst arrives as many bytes in the tty input buffer, so
 * this lets the main loop collapse the whole burst into one repaint.
 * The timeout is zero on purpose: a nonzero timeout would add that
 * delay to every single keypress, and a terminal that delivers a
 * paste in chunks still only costs one repaint per chunk. */
static bool input_pending(void)
{
	struct pollfd pfd;
	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;
	pfd.revents = 0;
	int r;
	do {
		r = poll(&pfd, 1, 0);
	} while (r == -1 && errno == EINTR);
	return r > 0 && (pfd.revents & POLLIN) != 0;
}

static int term_read_key(void)
{
	static bool meta_pending = false;
	int nread;
	unsigned char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if ((nread == -1) && (errno != EAGAIN))
			panic("Error reading input: ");
	}
	/* If a previous lone ESC was recorded, combine it with this character.
	 * A following ESC starts a new sequence instead: fall through so
	 * CSI decoding still happens. */
	if (meta_pending) {
		meta_pending = false;
		if (c != '\x1b' && (unsigned char)c >= 0x20)
			return META_((unsigned char)c);
	}
	if (c == '\x1b') {
		unsigned char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) {
			/* Lone ESC with no following byte — remember it for next call */
			meta_pending = true;
			return '\x1b';
		}
		/* If next byte is not '[' or 'O', treat as Meta+key */
		if (seq[0] != '[' && seq[0] != 'O')
			return META_((unsigned char)seq[0]);
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';
		if (seq[0] == '[') {
			if (isdigit(seq[1])) {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1':
					case '7':
						return HOME_KEY;
					case '4':
					case '8':
						return END_KEY;
					case '3':
						return DEL_KEY;
					case '5':
						return PAGE_UP;
					case '6':
						return PAGE_DOWN;
					}
				} else if (seq[2] == ';') {
					char mod, fin;
					if (read(STDIN_FILENO, &mod, 1) != 1 ||
					    read(STDIN_FILENO, &fin, 1) != 1)
						/* Incomplete CSI modifier sequence: treat as literal ESC
						 * so we don't invent a partial key event from truncated bytes. */
						return '\x1b';
					if (mod == '5' && (fin == 'C' || fin == 'D'))
						/* Ctrl+Right / Ctrl+Left */
						return fin == 'C' ?
						    CTRL_ARROW_RIGHT :
						    CTRL_ARROW_LEFT;
					if (mod == '2') {
						if (fin == '~') {
							switch (seq[1]) {
							case '1':
							case '7':
								return SHIFT_HOME_KEY;
							case '4':
							case '8':
								return SHIFT_END_KEY;
							case '5':
								return SHIFT_PAGE_UP;
							case '6':
								return SHIFT_PAGE_DOWN;
							}
						} else {
							switch (fin) {
							case 'A':
								return SHIFT_ARROW_UP;
							case 'B':
								return SHIFT_ARROW_DOWN;
							case 'C':
								return SHIFT_ARROW_RIGHT;
							case 'D':
								return SHIFT_ARROW_LEFT;
							case 'H':
								return SHIFT_HOME_KEY;
							case 'F':
								return SHIFT_END_KEY;
							}
						}
					}
				}
			} else {
				switch (seq[1]) {
				case 'A':
					return ARROW_UP;
				case 'B':
					return ARROW_DOWN;
				case 'C':
					return ARROW_RIGHT;
				case 'D':
					return ARROW_LEFT;
				case 'H':
					return HOME_KEY;
				case 'F':
					return END_KEY;
				case 'Z':	/* Shift+Tab (backtab) */
					return SHIFT_TAB;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'H':
				return HOME_KEY;
			case 'F':
				return END_KEY;
			}
		}
		return '\x1b';
	}
	return c;
}

static int term_get_size(int *screen_rows, int *screen_cols)
{
	struct winsize ws;
	if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0))
		return -1;
	*screen_cols = ws.ws_col;
	*screen_rows = ws.ws_row;
	return 0;
}

/* Where the window size comes from.  A test replaces this with its own
 * reader so a resize can be driven without a tty. */
static int (*term_size_reader)(int *, int *) = term_get_size;

static void term_update_size(void)
{
	if (term_size_reader(&ec.screen_rows, &ec.screen_cols) == -1) {
		/* Fallback to reasonable defaults for testing */
		ec.screen_rows = 24, ec.screen_cols = 80;
	}
	ec.screen_rows -= 1;
}

static void term_close_buffer(void)
{
	if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
	    write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
		panic("Error restoring buffer state: ");
	term_clear();
}

static int color_id_to_ansi_fg_code(color_id_t id)
{
	if (id == COLOR_NONE)
		return 39;
	if (id >= COLOR_BLACK && id <= COLOR_WHITE)
		return 30 + (int) id;
	if (id >= COLOR_BRIGHTBLACK && id <= COLOR_BRIGHTWHITE)
		return 90 + ((int) id - (int) COLOR_BRIGHTBLACK);
	return 39;
}

static int color_id_to_ansi_bg_code(color_id_t id)
{
	if (id == COLOR_NONE)
		return 49;
	if (id >= COLOR_BLACK && id <= COLOR_WHITE)
		return 40 + (int) id;
	if (id >= COLOR_BRIGHTBLACK && id <= COLOR_BRIGHTWHITE)
		return 100 + ((int) id - (int) COLOR_BRIGHTBLACK);
	return 49;
}

static int syntax_style_escape(unsigned char highlight, char *buf, size_t buflen)
{
	if (highlight == MATCH)
		return snprintf(buf, buflen, "\x1b[43m");
	if (highlight >= HIGHLIGHT_DYNAMIC_BASE &&
	    highlight < ec.syntax_palette_count) {
		color_id_t fg = ec.syntax_palette[highlight].fg;
		color_id_t bg = ec.syntax_palette[highlight].bg;
		return snprintf(buf, buflen, "\x1b[%d;%dm",
				color_id_to_ansi_fg_code(fg),
				color_id_to_ansi_bg_code(bg));
	}
	return snprintf(buf, buflen, "\x1b[39;49m");
}

static unsigned char syntax_palette_code(color_id_t fg, color_id_t bg)
{
	for (unsigned char i = HIGHLIGHT_DYNAMIC_BASE;
	     i < ec.syntax_palette_count; i++) {
		if (ec.syntax_palette[i].fg == fg && ec.syntax_palette[i].bg == bg)
			return i;
	}
	if (ec.syntax_palette_count == 255)
		return NORMAL;
	ec.syntax_palette[ec.syntax_palette_count].fg = fg;
	ec.syntax_palette[ec.syntax_palette_count].bg = bg;
	return ec.syntax_palette_count++;
}

static void syntax_reset_compiled_rules(void)
{
	for (size_t i = 0; i < ec.syntax_compiled_count; i++) {
		if (ec.syntax_compiled[i].valid)
			regfree(&ec.syntax_compiled[i].compiled);
	}
	free(ec.syntax_compiled);
	ec.syntax_compiled = NULL;
	ec.syntax_compiled_count = 0;
	for (size_t i = 0; i < ec.syntax_span_compiled_count; i++) {
		if (ec.syntax_span_compiled[i].valid_start)
			regfree(&ec.syntax_span_compiled[i].start_compiled);
		if (ec.syntax_span_compiled[i].valid_end)
			regfree(&ec.syntax_span_compiled[i].end_compiled);
	}
	free(ec.syntax_span_compiled);
	ec.syntax_span_compiled = NULL;
	ec.syntax_span_compiled_count = 0;
	ec.syntax_palette_count = HIGHLIGHT_DYNAMIC_BASE;
}

static void syntax_invalidate_all_rows(void)
{
	ec.hl_invalid_from = 0;
	for (int file_row = 0; file_row < NR; file_row++)
		ROW(file_row)->hl_valid = false;
}

/* row_update changed the bytes of row row_idx, so its own highlight is
 * stale, and a change to any row can flip a multi-line span rule, so
 * every row from row_idx to the end is stale too.  Marking that with a
 * single number keeps the mark lazy: no walk of the row list on every
 * keystroke, and the re-highlight runs when a row is next drawn.
 *
 * One change, one mark: every row from row_idx down is stale, so keep
 * the lowest index seen and never weaken it.  The row list itself
 * carries no per-row stale bit, so hl_invalid_from is the record; a
 * second mark must not forget the first. */
static void hl_invalidate_from(int row_idx)
{
	if (row_idx < 0)
		return;
	if (ec.hl_invalid_from < 0 || row_idx < ec.hl_invalid_from) {
		ec.hl_invalid_from = row_idx;
		/* Clear the bits the lazy mark replaces, so nothing can be
		 * read as fresh while the mark stands for it. */
		for (int i = row_idx; i < NR; i++)
			ROW(i)->hl_valid = false;
	}
}

/* Mark every row stale, for a change that renumbers the rows below. */
static void hl_mark_all(void)
{
	ec.hl_invalid_from = 0;
	for (int i = 0; i < NR; i++)
		ROW(i)->hl_valid = false;
	ec.longest_line = rows_longest();
}

/* Longest row in the buffer, in bytes.  The status bar sizes the column
 * counter from it, and it has to shrink as well as grow: a line that
 * was cut or backspaced away must not keep the width it once needed. */
static int rows_longest(void)
{
	int longest = 0;
	for (int i = 0; i < NR; i++) {
		editor_row_t *row = ROW(i);
		if (row && row->size > longest)
			longest = row->size;
	}
	return longest;
}

static void syntax_disable(bool announce)
{
	syntax_reset_compiled_rules();
	ec.syntax = NULL;
	syntax_invalidate_all_rows();
	if (announce)
		ui_set_message("Syntax highlighting disabled");
}

static void syntax_apply_rules(editor_row_t *row)
{
	if (!row->render)		/* allocation failed: nothing to match */
		return;
	for (size_t r = 0; r < ec.syntax_compiled_count; r++) {
		size_t offset = 0;
		regmatch_t match;
		while (offset <= (size_t) row->render_size) {
			int rc = regexec(&ec.syntax_compiled[r].compiled,
					 row->render + offset, 1, &match,
					 (offset == 0) ? 0 : REG_NOTBOL);
			size_t from, to;
			if (rc != 0 || match.rm_so < 0 || match.rm_eo < 0)
				break;
			from = offset + (size_t) match.rm_so;
			to = offset + (size_t) match.rm_eo;
			if (from > (size_t) row->render_size)
				break;
			if (to > (size_t) row->render_size)
				to = (size_t) row->render_size;
			if (to > from) {
				for (size_t i = from; i < to; i++)
					if (row->highlight[i] == NORMAL)
						row->highlight[i] =
						    ec.syntax_compiled[r].hl_code;
			}
			if (to <= offset) {
				if (offset == (size_t) row->render_size)
					break;
				offset++;
			} else {
				offset = to;
			}
		}
	}
}

static void syntax_fill_range(editor_row_t *row, size_t from, size_t to,
			      unsigned char hl_code, bool only_normal)
{
	if (from > (size_t) row->render_size)
		return;
	if (to > (size_t) row->render_size)
		to = (size_t) row->render_size;
	if (to <= from)
		return;
	if (!only_normal) {
		memset(row->highlight + from, hl_code, to - from);
		return;
	}
	for (size_t i = from; i < to; i++)
		if (row->highlight[i] == NORMAL)
			row->highlight[i] = hl_code;
}

static bool syntax_find_match(regex_t *rx, const char *s, size_t offset,
			      size_t * out_from, size_t * out_to)
{
	regmatch_t m;
	int rc = regexec(rx, s + offset, 1, &m, (offset == 0) ? 0 : REG_NOTBOL);
	if (rc != 0 || m.rm_so < 0 || m.rm_eo < 0)
		return false;
	*out_from = offset + (size_t) m.rm_so;
	*out_to = offset + (size_t) m.rm_eo;
	return *out_to > *out_from;
}

static void syntax_apply_span_rules(editor_row_t *row, int row_idx)
{
	if (!row->render)		/* allocation failed: nothing to match */
		return;
	size_t pos = 0;
	bool in_span = false;
	int active_span = -1;
	size_t len = (size_t) row->render_size;

	row->span_open = false;
	row->span_rule = -1;
	if (ec.syntax_span_compiled_count == 0)
		return;

	if (row_idx > 0 && ROW(row_idx - 1)->span_open) {
		in_span = true;
		active_span = ROW(row_idx - 1)->span_rule;
	}

	while (pos < len) {
		if (in_span) {
			size_t end_from, end_to;
			if (!syntax_find_match
			    (&ec.syntax_span_compiled[active_span].end_compiled,
			     row->render, pos, &end_from, &end_to)) {
				syntax_fill_range(row, pos, len,
						  ec.syntax_span_compiled
						  [active_span].hl_code, false);
				row->span_open = true;
				row->span_rule = active_span;
				return;
			}
			syntax_fill_range(row, pos, end_to,
					  ec.syntax_span_compiled
					  [active_span].hl_code, false);
			pos = end_to;
			in_span = false;
			active_span = -1;
			continue;
		}

		bool found_start = false;
		size_t best_from = 0, best_to = 0;
		int best_rule = 0;
		for (size_t r = 0; r < ec.syntax_span_compiled_count; r++) {
			size_t from, to;
			if (!syntax_find_match
			    (&ec.syntax_span_compiled[r].start_compiled,
			     row->render, pos, &from, &to))
				continue;
			if (!found_start || from < best_from ||
			    (from == best_from && (int)r < best_rule)) {
				found_start = true;
				best_from = from;
				best_to = to;
				best_rule = (int)r;
			}
		}
		if (!found_start)
			break;
		{
			size_t end_from, end_to;
			if (!syntax_find_match
			    (&ec.syntax_span_compiled[best_rule].end_compiled,
			     row->render, best_to, &end_from, &end_to)) {
				syntax_fill_range(row, best_from, len,
						  ec.syntax_span_compiled
						  [best_rule].hl_code, false);
				row->span_open = true;
				row->span_rule = best_rule;
				return;
			}
			syntax_fill_range(row, best_from, end_to,
					  ec.syntax_span_compiled
					  [best_rule].hl_code, false);
			pos = end_to;
		}
	}
}

static void syntax_highlight(editor_row_t *row, int row_idx);

static void syntax_ensure_row_highlighted(int row_idx)
{
	int start = row_idx;
	if (row_idx < 0 || row_idx >= NR)
		return;
	if (ec.hl_invalid_from >= 0) {
		/* The lazy mark in hl_invalid_from says every row from that
		 * index on is stale.  Drop the hl_valid bits for the rows
		 * this call covers, and drop the mark itself once nothing
		 * can still be stale below the mark.  The mark is left in
		 * place when it sits below row_idx, because rows under it
		 * stay stale until a draw reaches them. */
		if (row_idx >= ec.hl_invalid_from) {
			for (int i = ec.hl_invalid_from; i < NR; i++)
				ROW(i)->hl_valid = false;
			ec.hl_invalid_from = -1;
		} else {
			ROW(row_idx)->hl_valid = false;
		}
	}
	if (ROW(row_idx)->hl_valid)
		return;
	if (ec.syntax_span_compiled_count > 0)
		while (start > 0 && !ROW(start - 1)->hl_valid)
			start--;
	for (int i = start; i <= row_idx; i++)
		if (!ROW(i)->hl_valid)
			syntax_highlight(ROW(i), i);
}

static void syntax_highlight(editor_row_t *row, int row_idx)
{
	/* render_size 0 means an empty row, and realloc(p, 0) may free the
	 * buffer and return NULL, while the draw reads the highlight for
	 * every column it paints, so keep a one-byte buffer instead.  A
	 * failed realloc returns NULL and leaves the old pointer alone, so
	 * the result is only stored once it is known to be good. */
	size_t want = row->render_size > 0 ? (size_t) row->render_size : 1;
	unsigned char *hl = realloc(row->highlight, want);
	if (!hl) {
		/* No highlight to write.  Leave the row stale so the next
		 * draw tries again, and draw it without colours. Drop the
		 * old buffer and zero render_size so the draw cannot read
		 * past the old allocation, which may be smaller than the
		 * new render_size. */
		free(row->highlight);
		row->highlight = NULL;
		row->render_size = 0;
		row->hl_valid = false;
		return;
	}
	row->highlight = hl;
	memset(row->highlight, NORMAL, row->render_size);
	row->span_open = false;
	row->span_rule = -1;
	if (!ec.syntax)
	{
		row->hl_valid = true;
		return;
	}
	syntax_apply_rules(row);
	syntax_apply_span_rules(row, row_idx);
	row->hl_valid = true;
}

static bool syntax_match_regex(const char *pattern, const char *text)
{
	regex_t rx = NULL;
	bool ok = false;

	if (!pattern || !*pattern || !text)
		return false;
	if (regcomp(&rx, pattern, REG_EXTENDED | REG_NOSUB) != 0)
		return false;
	ok = (regexec(&rx, text, 0, NULL, 0) == 0);
	regfree(&rx);
	return ok;
}

static void syntax_select(void)
{
	syntax_reset_compiled_rules();
	ec.syntax = NULL;
	syntax_invalidate_all_rows();
	if (!ec.file_name)
		return;
	for (size_t j = 0; syntax_rules[j].file_regex; j++) {
		if (syntax_match_regex(syntax_rules[j].file_regex, ec.file_name)) {
			ec.syntax = &syntax_rules[j];
			break;
		}
	}
	if (!ec.syntax && NR > 0) {
		const char *first_line = ROW(0)->chars;
		for (size_t j = 0; syntax_rules[j].file_regex; j++) {
			if (syntax_match_regex(syntax_rules[j].file_magic,
					       first_line)) {
				ec.syntax = &syntax_rules[j];
				break;
			}
		}
	}
	if (!ec.syntax)
		return;
	{
		size_t single_count = 0;
		size_t span_count = 0;
		size_t single_idx = 0;
		size_t span_idx = 0;

		for (size_t i = 0; i < ec.syntax->rule_count; i++) {
			const struct syntax_rule *rule = &ec.syntax->rules[i];
			if (!rule->regex || (rule->fg == COLOR_NONE &&
					     rule->bg == COLOR_NONE))
				continue;
			if (rule->end_regex)
				span_count++;
			else
				single_count++;
		}

		if (single_count > 0) {
			ec.syntax_compiled =
			    calloc(single_count, sizeof(*ec.syntax_compiled));
			if (!ec.syntax_compiled)
				return;
		}
		if (span_count > 0) {
			ec.syntax_span_compiled =
			    calloc(span_count, sizeof(*ec.syntax_span_compiled));
			if (!ec.syntax_span_compiled) {
				free(ec.syntax_compiled);
				ec.syntax_compiled = NULL;
				return;
			}
		}

		for (size_t i = 0; i < ec.syntax->rule_count; i++) {
			const struct syntax_rule *rule = &ec.syntax->rules[i];
			unsigned char hl_code;

			if (!rule->regex || (rule->fg == COLOR_NONE &&
					     rule->bg == COLOR_NONE))
				continue;
			hl_code = syntax_palette_code(rule->fg, rule->bg);
			if (hl_code == NORMAL)
				continue;

			if (rule->end_regex) {
				regex_t start_rx = NULL;
				regex_t end_rx = NULL;
				if (regcomp(&start_rx, rule->regex,
					    REG_EXTENDED) != 0)
					continue;
				if (regcomp(&end_rx, rule->end_regex,
					    REG_EXTENDED) != 0) {
					regfree(&start_rx);
					continue;
				}
				ec.syntax_span_compiled[span_idx].start_compiled =
				    start_rx;
				ec.syntax_span_compiled[span_idx].end_compiled =
				    end_rx;
				ec.syntax_span_compiled[span_idx].valid_start =
				    true;
				ec.syntax_span_compiled[span_idx].valid_end = true;
				ec.syntax_span_compiled[span_idx].hl_code =
				    hl_code;
				span_idx++;
			} else {
				regex_t rule_rx = NULL;
				if (regcomp(&rule_rx, rule->regex,
					    REG_EXTENDED) != 0)
					continue;
				ec.syntax_compiled[single_idx].compiled = rule_rx;
				ec.syntax_compiled[single_idx].valid = true;
				ec.syntax_compiled[single_idx].hl_code = hl_code;
				single_idx++;
			}
		}
		ec.syntax_compiled_count = single_idx;
		ec.syntax_span_compiled_count = span_idx;
	}
}

static int row_cursorx_to_renderx(editor_row_t * row, int cursor_x)
{
	if (row->render_direct_map) {
		if (cursor_x < 0)
			return 0;
		return cursor_x > row->size ? row->size : cursor_x;
	}

	int render_x = 0;
	int byte_pos = 0;

	while (byte_pos < row->size && byte_pos < cursor_x) {
		if (row->chars[byte_pos] == '\t') {
			render_x += (TAB_STOP - 1) - (render_x % TAB_STOP);
			render_x++;
			byte_pos++;
		} else {
			int char_len = 1;
			int char_width =
			    utf8_char_width_and_len(&row->chars[byte_pos],
						    (size_t) (row->size -
							      byte_pos),
						    &char_len);
			render_x += char_width;
			byte_pos += char_len;
		}
	}
	return render_x;
}

static void row_update(editor_row_t * row, int row_idx)
{
	int tabs = 0;
	int wide_chars = 0;
	bool direct_map = true;
	int byte_pos = 0;

	/* Count tabs and wide characters for buffer allocation */
	while (byte_pos < row->size) {
		unsigned char c = (unsigned char)row->chars[byte_pos];
		if (c == '\t') {
			tabs++;
			direct_map = false;
			byte_pos++;
		} else if (!(c & 0x80)) {
			if (c < 0x20 || c == 0x7F)
				direct_map = false;
			byte_pos++;
		} else {
			int char_len = 1;
			int char_width =
			    utf8_char_width_and_len(&row->chars[byte_pos],
						    (size_t) (row->size -
							      byte_pos),
						    &char_len);
			direct_map = false;
			if (char_width > 1) {
				wide_chars += (char_width - 1);
			}
			byte_pos += char_len;
		}
	}
	row->render_direct_map = direct_map;

	free(row->render);
	/* Allocate extra space for tabs and wide characters.  The old
	 * pointer goes first, so a failed allocation leaves the row without
	 * a render buffer rather than with a dangling one, and the writes
	 * below are skipped. */
	row->render =
	    malloc(row->size + tabs * (TAB_STOP - 1) + wide_chars + 1);
	if (!row->render) {
		row->render_size = 0;
		row->render_direct_map = false;
		ui_set_message("Memory allocation failed");
		hl_invalidate_from(row_idx);
		return;
	}

	int idx = 0;
	byte_pos = 0;

	while (byte_pos < row->size) {
		if (row->chars[byte_pos] == '\t') {
			row->render[idx++] = ' ';
			while (idx % TAB_STOP != 0)
				row->render[idx++] = ' ';
			byte_pos++;
		} else {
			int char_len =
			    utf8_byte_length((uint8_t) row->chars[byte_pos]);
			/* Copy the UTF-8 sequence as-is */
			for (int i = 0;
			     i < char_len && byte_pos + i < row->size; i++)
				row->render[idx++] = row->chars[byte_pos + i];
			byte_pos += char_len;
		}
	}
	row->render[idx] = '\0';
	row->render_size = idx;
	/* The status bar sizes the column counter from the longest row,
	 * and a row that shrank may have been it.  Only the row that grew
	 * is compared, so a shrink does not walk the whole row list on
	 * every keystroke. */
	if (row->size > ec.longest_line)
		ec.longest_line = row->size;
	hl_invalidate_from(row_idx);
}

static void row_insert(int at, const char *s, size_t line_len)
{
	if (at < 0 || at > NR)
		return;
	editor_row_t row = { 0 };
	row.size = (int)line_len;
	row.chars = malloc(line_len + 1);
	if (!row.chars) {
		ui_set_message("Memory allocation failed");
		return;
	}
	memcpy(row.chars, s, line_len);
	row.chars[line_len] = '\0';
	if (!tlist_insert(ec.rows, (size_t) at, &row)) {
		free(row.chars);
		ui_set_message("Memory allocation failed");
		return;
	}
	row_update(ROW(at), at);
	ec.modified = true;
}

static char *xstrndup0(const char *s, size_t len)
{
	char *p = malloc(len + 1);
	if (!p)
		return NULL;
	if (len)
		memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

/* Append one byte to a NUL terminated buffer, growing it by doubling.
 * The editor had three copies of this: the search query, the
 * replacement text, and the buffer ui_prompt() edits.  The two search
 * buffers grew by one byte per character, so typing a query reallocated
 * once per key and copied everything typed so far each time.
 *
 * A failed allocation appends nothing and leaves the buffer, the length
 * and the cap as they were, so the caller keeps whatever it had and the
 * next keystroke tries again.  The cap always holds the NUL: it doubles
 * when len + 1 exceeds it, and never below the first allocation. */
static bool strbuf_grow(char **buf, size_t *len, size_t *cap, int byte)
{
	if (*len < SIZE_MAX - 1 && *len + 2 > *cap) {
		size_t need = *len + 2;
		size_t new_cap = *cap ? *cap : 128;
		while (new_cap < need) {
			if (new_cap > SIZE_MAX / 2)
				return false; /* doubling would overflow */
			new_cap *= 2;
		}
		char *nb = realloc(*buf, new_cap);
		if (!nb) {
			/* errno is set by realloc; the buffer is untouched. */
			return false;
		}
		*buf = nb;
		*cap = new_cap;
	}
	(*buf)[(*len)++] = (char)byte;
	(*buf)[*len] = '\0';
	return true;
}

/* Make room for `want` bytes and a NUL, doubling as strbuf_grow() does,
 * without writing anything.  A caller that already has the bytes copies
 * them in itself, rather than handing them over one at a time. */
static bool strbuf_reserve(char **buf, size_t *len, size_t *cap, size_t want)
{
	if (want < SIZE_MAX && want + 1 <= *cap)
		return true;
	if (want >= SIZE_MAX)
		return false;
	size_t need = want + 1;
	size_t new_cap = *cap ? *cap : 128;
	while (new_cap < need) {
		if (new_cap > SIZE_MAX / 2)
			return false;
		new_cap *= 2;
	}
	char *nb = realloc(*buf, new_cap);
	if (!nb)
		return false;
	*buf = nb;
	*cap = new_cap;
	return true;
}

static void undo_item_free(undo_item_t * item)
{
	free(item->text);
	item->text = NULL;
	item->len = 0;
	item->aux_len = 0;
}

static int undo_history_index(int logical_idx)
{
	int idx = g_undo.start + logical_idx;
	return idx >= UNDO_STACK_CAP ? idx - UNDO_STACK_CAP : idx;
}

static void undo_history_clear(void)
{
	for (int i = 0; i < g_undo.count; i++)
		undo_item_free(&g_undo.history[undo_history_index(i)]);
	g_undo.start = g_undo.count = g_undo.cursor = 0;
}

static void undo_history_discard_redo(void)
{
	while (g_undo.count > g_undo.cursor)
		undo_item_free(&g_undo.
			       history[undo_history_index(--g_undo.count)]);
}

static void undo_history_append(undo_item_t item)
{
	undo_history_discard_redo();
	if (g_undo.count == UNDO_STACK_CAP) {
		undo_item_free(&g_undo.history[g_undo.start]);
		if (++g_undo.start >= UNDO_STACK_CAP)
			g_undo.start = 0;
		g_undo.count--;
		if (g_undo.cursor > 0)
			g_undo.cursor--;
	}
	g_undo.history[undo_history_index(g_undo.count++)] = item;
	g_undo.cursor = g_undo.count;
}

static bool undo_insert_bytes(int row_idx, int col, const char *s, size_t len)
{
	editor_row_t *row = ROW(row_idx);
	if (!row || len == 0)
		return row != NULL;
	col = col < 0 ? 0 : (col > row->size ? row->size : col);
	char *nc = realloc(row->chars, (size_t) row->size + len + 1);
	if (!nc)
		return false;
	row->chars = nc;
	memmove(&row->chars[col + len], &row->chars[col],
		(size_t) (row->size - col) + 1);
	memcpy(&row->chars[col], s, len);
	row->size += (int)len;
	row_update(row, row_idx);
	/* row_update only widens the counter, because a shrink would cost
	 * a walk of every row on each keystroke.  Here the row is known,
	 * so the exact widest line is cheap. */
	if (ec.longest_line > row->size)
		ec.longest_line = rows_longest();
	return true;
}

static bool undo_apply_insert_text(int row, int col, const char *text,
				   size_t len)
{
	if (row < 0 || row > NR)
		return false;
	if (row == NR)
		row_insert(NR, "", 0);
	int y = row, x = col;
	size_t i = 0;
	while (i < len) {
		size_t seg = 0;
		while (i + seg < len && text[i + seg] != '\n')
			seg++;
		if (seg > 0 && !undo_insert_bytes(y, x, text + i, seg))
			return false;
		x += (int)seg;
		i += seg;
		if (i < len && text[i] == '\n') {
			editor_row_t *r = ROW(y);
			if (!r)
				return false;
			row_insert(y + 1, &r->chars[x], (size_t) (r->size - x));
			r = ROW(y);
			r->size = x;
			r->chars[x] = '\0';
			row_update(r, y);
			/* The row above was split, so the widest line may now
			 * be shorter than the counter says.  Measuring is one
			 * pass over the rows, and the replay is not on the
			 * per-keystroke path. */
			ec.longest_line = rows_longest();
			/* row_insert grew the new row, so the counter is at
			 * least its width. */
			if (ROW(y + 1) && ROW(y + 1)->size > ec.longest_line)
				ec.longest_line = ROW(y + 1)->size;
			y++;
			x = 0;
			i++;
		}
	}
	ec.modified = true;
	return true;
}

static bool undo_delete_bytes(int row_idx, int col, size_t len)
{
	editor_row_t *row = ROW(row_idx);
	if (!row || len == 0)
		return row != NULL;
	if (col < 0 || col > row->size || (size_t) (row->size - col) < len)
		return false;
	memmove(&row->chars[col], &row->chars[col + len],
		(size_t) (row->size - col) - len + 1);
	row->size -= (int)len;
	row_update(row, row_idx);
	/* The row shrank, so the counter has to be measured again if this
	 * row was the widest.  Same reasoning as undo_insert_bytes(). */
	if (ec.longest_line > row->size)
		ec.longest_line = rows_longest();
	return true;
}

static bool undo_apply_delete_text(int row, int col, const char *text,
				   size_t len)
{
	if (row < 0 || row >= NR)
		return false;
	int y = row, x = col;
	size_t i = 0;
	while (i < len) {
		size_t seg = 0;
		while (i + seg < len && text[i + seg] != '\n')
			seg++;
		if (seg > 0 && !undo_delete_bytes(y, x, seg))
			return false;
		i += seg;
		if (i < len && text[i] == '\n') {
			if (y + 1 >= NR)
				return false;
			editor_row_t *r = ROW(y);
			editor_row_t *next = ROW(y + 1);
			if (!r || !next)
				return false;
			char *nc =
			    realloc(r->chars,
				    (size_t) r->size + (size_t) next->size + 1);
			if (!nc)
				return false;
			r->chars = nc;
			memcpy(&r->chars[r->size], next->chars,
			       (size_t) next->size + 1);
			r->size += next->size;
			row_update(r, y);
			row_erase(y + 1);
			i++;
		}
	}
	ec.modified = true;
	return true;
}

/* Build an undo item; text is heap-copied, NULL on OOM. */
static undo_item_t undo_item_make(edit_type_t type, int row, int col,
				  const char *text, size_t len, int dir,
				  int before_y, int before_x, int after_y,
				  int after_x)
{
	undo_item_t item = {
		.type = type,
		.row = row,
		.col = col,
		.text = xstrndup0(text, len),
		.len = len,
		.dir = dir,
		.before_y = before_y,
		.before_x = before_x,
		.after_y = after_y,
		.after_x = after_x,
	};
	return item;
}

static void undo_record_common(edit_type_t type, int row, int col,
			       const char *text, size_t len, int before_y,
			       int before_x, int after_y, int after_x)
{
	if (g_undo.replaying || g_undo.batching || !text || len == 0)
		return;
	undo_item_t item = undo_item_make(type, row, col, text, len, 0,
					  before_y, before_x, after_y,
					  after_x);
	if (item.text)
		undo_history_append(item);
}

/* Record a block indent over [row, aux_len] using the indent string.
 * dir is +1 for indent, -1 for unindent.  Stored as one item so
 * undo/redo flips the whole operation. */
static void undo_record_block_indent(int first_row, int last_row,
				     const char *indent, size_t len, int dir,
				     int before_y, int before_x, int after_y,
				     int after_x)
{
	/* Deliberately NOT guarded by g_undo.batching: selection_indent
	 * batches its row edits so each per-line insert/delete is not
	 * recorded, then records the whole block as one item here. */
	if (g_undo.replaying || !indent || len == 0)
		return;
	undo_item_t item = undo_item_make(EDIT_BLOCK_INDENT, first_row, 0,
					  indent, len, dir, before_y,
					  before_x, after_y, after_x);
	if (!item.text)
		return;
	item.aux_len = last_row;
	undo_history_append(item);
}

static void undo_record_insert(int row, int col, const char *text, size_t len,
			       int before_y, int before_x, int after_y,
			       int after_x)
{
	undo_record_common(EDIT_INSERT, row, col, text, len, before_y, before_x,
			   after_y, after_x);
}

static void undo_record_delete(int row, int col, const char *text, size_t len,
			       int before_y, int before_x, int after_y,
			       int after_x)
{
	undo_record_common(EDIT_DELETE, row, col, text, len, before_y, before_x,
			   after_y, after_x);
}

/* Bytes an unindent removes from a line start: one full leading copy
 * of "indent", else one whitespace character (so shifts still work on
 * mixed-indent files).  Never eats any other text.  Shared by the live
 * unindent pass and by undo of a block indent, so both agree. */
static size_t indent_strip_len(const editor_row_t * row, const char *indent,
			       size_t ilen)
{
	if (!row || row->size == 0)
		return 0;
	if ((size_t) row->size >= ilen &&
	    memcmp(row->chars, indent, ilen) == 0)
		return ilen;
	return (row->chars[0] == ' ' || row->chars[0] == '\t') ? 1 : 0;
}

/* Apply a whole-block indent recorded as one undo step.
 * item->row = first line, item->aux_len = last line, item->text holds
 * the indent string, item->len is its length.  Redo re-indents each
 * line in the range; undo removes up to one indent per line bottom-up
 * so row indices stay valid. */
static bool undo_apply_block_indent(const undo_item_t * item, bool redo)
{
	if (!item->text || item->len == 0)
		return false;
	int first = item->row;
	int last = (int) item->aux_len;

	if (first < 0)
		first = 0;
	if (last > NR - 1)
		last = NR - 1;
	if (first > last)
		return false;

	for (int y = redo ? first : last;
	     redo ? y <= last : y >= first; y += redo ? 1 : -1) {
		editor_row_t *row = ROW(y);
		if (!row || row->size == 0)
			continue;
		size_t ilen = item->len;
		/* Invert exactly what was recorded: an indent press is
		 * undone by stripping, an unindent press by re-adding.
		 * Strip follows the live rule so no text but whitespace
		 * can ever be removed. */
		if ((redo ? item->dir < 0 : item->dir > 0)) {
			size_t strip_len =
			    indent_strip_len(row, item->text, ilen);
			if (strip_len > 0)
				undo_delete_bytes(y, 0, strip_len);
		} else {
			undo_insert_bytes(y, 0, item->text, ilen);
		}
	}
	return true;	/* best effort: keep cursor update */
}

static bool undo_apply_replace_span(int row, int col, size_t from_len,
				    const char *to_text, size_t to_len)
{
	editor_row_t *r = ROW(row);
	if (!r || col < 0 || col > r->size
	    || (size_t) (r->size - col) < from_len)
		return false;
	size_t new_size = (size_t) r->size - from_len + to_len;
	if (new_size > INT_MAX)
		return false;
	/* Same ordering constraint as do_replace_one(): the memmove reads
	 * up to chars[r->size], so shrink the block only after moving. */
	size_t stage_size = ((size_t) r->size + 1 > new_size + 1) ?
	    (size_t) r->size + 1 : new_size + 1;
	char *nc = realloc(r->chars, stage_size);
	if (!nc)
		return false;
	r->chars = nc;
	memmove(&r->chars[col + to_len], &r->chars[col + from_len],
		(size_t) (r->size - col) - from_len + 1);
	if (to_len > 0)
		memcpy(&r->chars[col], to_text, to_len);
	r->size = (int)new_size;
	r->chars[r->size] = '\0';
	if (stage_size != new_size + 1) {
		nc = realloc(r->chars, new_size + 1);
		if (nc)
			r->chars = nc;
	}
	row_update(r, row);
	ec.modified = true;
	return true;
}

static void undo_record_replace(int row, int col, const char *old_text,
				size_t old_len, const char *new_text,
				size_t new_len, int before_y, int before_x,
				int after_y, int after_x)
{
	if (g_undo.replaying || g_undo.batching || !old_text || !new_text ||
	    old_len > SIZE_MAX - new_len - 1)
		return;
	size_t total_len = old_len + new_len;
	char *text = malloc(total_len + 1);
	if (!text)
		return;
	memcpy(text, old_text, old_len);
	memcpy(text + old_len, new_text, new_len);
	text[total_len] = '\0';
	undo_item_t item = {
		.type = EDIT_REPLACE,
		.row = row,
		.col = col,
		.text = text,
		.len = old_len,
		.aux_len = new_len,
		.before_y = before_y,
		.before_x = before_x,
		.after_y = after_y,
		.after_x = after_x,
	};
	undo_history_append(item);
}

static void undo_apply_item(const undo_item_t * item, bool redo)
{
	bool ok;
	int cy, cx;
	if (item->type == EDIT_BLOCK_INDENT) {
		ok = undo_apply_block_indent(item, redo);
		cy = redo ? item->after_y : item->before_y;
		cx = redo ? item->after_x : item->before_x;
	} else if (redo) {
		ok = (item->type ==
		      EDIT_INSERT) ? undo_apply_insert_text(item->row,
							    item->col,
							    item->text,
							    item->len) : (item->
									  type
									  ==
									  EDIT_DELETE)
		    ? undo_apply_delete_text(item->row, item->col, item->text,
					     item->
					     len) :
		    undo_apply_replace_span(item->row, item->col, item->len,
					    item->text + item->len,
					    item->aux_len);
		cy = item->after_y;
		cx = item->after_x;
	} else {
		ok = (item->type ==
		      EDIT_INSERT) ? undo_apply_delete_text(item->row,
							    item->col,
							    item->text,
							    item->len) : (item->
									  type
									  ==
									  EDIT_DELETE)
		    ? undo_apply_insert_text(item->row, item->col, item->text,
					     item->
					     len) :
		    undo_apply_replace_span(item->row, item->col,
					    item->aux_len, item->text,
					    item->len);
		cy = item->before_y;
		cx = item->before_x;
	}
	if (ok) {
		ec.cursor_y = cy < 0 ? 0 : (cy > NR ? NR : cy);
		ec.cursor_x = cx < 0 ? 0 : cx;
		if (ec.cursor_y < NR && ec.cursor_x > ROW(ec.cursor_y)->size)
			ec.cursor_x = ROW(ec.cursor_y)->size;
	}
	/* A replayed indent moves line starts; a live selection anchored
	 * to the old geometry would then mis-shift the next keypress and
	 * desync later undos.  Drop it. */
	ec.selection.active = false;
}

static void undo_perform(bool redo)
{
	burst_break();
	if (redo ? g_undo.cursor >= g_undo.count : g_undo.cursor <= 0) {
		ui_set_message(redo ? "Nothing to redo" : "Nothing to undo");
		return;
	}
	if (!redo)
		g_undo.cursor--;
	g_undo.replaying = true;
	undo_apply_item(&g_undo.history[undo_history_index(g_undo.cursor)],
			redo);
	g_undo.replaying = false;
	if (redo)
		g_undo.cursor++;
}

static void editor_copy(int cut)
{
	if (ec.cursor_y >= NR)
		return;

	size_t len = strlen(ROW(ec.cursor_y)->chars) + 1;
	ec.copied_char_buffer = realloc(ec.copied_char_buffer, len);
	if (!ec.copied_char_buffer) {
		ui_set_message("Memory allocation failed");
		return;
	}
	snprintf(ec.copied_char_buffer, len, "%s", ROW(ec.cursor_y)->chars);
	ui_set_message(cut ? "Text cut" : "Text copied");
}

static void editor_cut(bool append)
{
	if (ec.cursor_y >= NR)
		return;
	burst_break();

	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	bool is_last_line = (ec.cursor_y == NR - 1);
	bool remove_newline = (NR > 1 && !is_last_line);
	size_t del_len =
	    (size_t) ROW(ec.cursor_y)->size + (remove_newline ? 1u : 0u);
	bool have_newline = false;
	char *deleted =
	    xstrndup0(ROW(ec.cursor_y)->chars, (size_t) ROW(ec.cursor_y)->size);
	if (deleted && remove_newline) {
		char *nd = realloc(deleted, del_len + 1);
		if (nd) {
			deleted = nd;
			deleted[del_len - 1] = '\n';
			deleted[del_len] = '\0';
			have_newline = true;
		}
	}

	/* Build the new clipboard text for this line */
	size_t line_size = ROW(ec.cursor_y)->size;
	size_t new_len = line_size + 1;	/* text + \n */

	if (append && ec.copied_char_buffer) {
		/* Append this line to the existing clipboard */
		size_t old_len = strlen(ec.copied_char_buffer);
		char *new_buf =
		    realloc(ec.copied_char_buffer, old_len + new_len + 1);
		if (!new_buf) {
			free(deleted);
			ui_set_message("Memory allocation failed");
			return;
		}
		ec.copied_char_buffer = new_buf;
		memcpy(ec.copied_char_buffer + old_len, ROW(ec.cursor_y)->chars,
		       line_size);
		ec.copied_char_buffer[old_len + line_size] = '\n';
		ec.copied_char_buffer[old_len + line_size + 1] = '\0';
	} else {
		/* Replace clipboard with this line */
		char *new_buf = realloc(ec.copied_char_buffer, new_len + 1);
		if (!new_buf) {
			free(deleted);
			ui_set_message("Memory allocation failed");
			return;
		}
		ec.copied_char_buffer = new_buf;
		memcpy(ec.copied_char_buffer, ROW(ec.cursor_y)->chars,
		       line_size);
		ec.copied_char_buffer[line_size] = '\n';
		ec.copied_char_buffer[line_size + 1] = '\0';
	}
	ui_set_message("Text cut");

	if (NR > 1 && !is_last_line) {
		row_erase(ec.cursor_y);
	} else {
		editor_row_t *row = ROW(ec.cursor_y);
		char *nc = realloc(row->chars, 1);
		if (!nc) {
			free(deleted);
			ui_set_message("Memory allocation failed");
			return;
		}
		row->chars = nc;
		row->size = 0;
		row->chars[0] = '\0';
		row_update(row, ec.cursor_y);
	}

	/* Adjust cursor */
	if (ec.cursor_y >= NR && NR > 0)
		ec.cursor_y = NR - 1;
	ec.cursor_x = 0;
	ec.modified = true;
	if (deleted) {
		size_t record_len = have_newline ? del_len : strlen(deleted);
		undo_record_delete(before_y, 0, deleted, record_len,
				   before_y, before_x, ec.cursor_y,
				   ec.cursor_x);
		free(deleted);
	}
}

static void editor_paste(void)
{
	if (!ec.copied_char_buffer)
		return;
	burst_break();

	/* Validate cursor position first */
	if (ec.cursor_y >= NR) {
		if (NR > 0) {
			ec.cursor_y = NR - 1;
			ec.cursor_x = ROW(ec.cursor_y)->size;
		} else {
			ec.cursor_y = 0;
			ec.cursor_x = 0;
		}
	}

	/* Ensure cursor_x doesn't exceed current row size */
	if (ec.cursor_y < NR) {
		int max_x = ROW(ec.cursor_y)->size;
		if (ec.cursor_x > max_x)
			ec.cursor_x = max_x;
	}

	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	size_t paste_len = strlen(ec.copied_char_buffer);
	g_undo.batching = true;
	for (size_t i = 0; i < paste_len; i++) {
		if (ec.copied_char_buffer[i] == '\n')
			editor_newline();
		else
			editor_insert_char((unsigned char)ec.
					   copied_char_buffer[i], false);
	}
	g_undo.batching = false;
	if (paste_len > 0) {
		undo_record_insert(before_y, before_x, ec.copied_char_buffer,
				   paste_len, before_y, before_x, ec.cursor_y,
				   ec.cursor_x);
	}
	ui_set_message("Pasted %zu bytes", paste_len);
}

/* Check if a position is within the selection */
static bool selection_contains(int x, int y)
{
	if (!ec.selection.active)
		return false;

	int start_y = ec.selection.start_y, start_x = ec.selection.start_x;
	int end_y = ec.selection.end_y, end_x = ec.selection.end_x;

	/* Normalize selection */
	if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
		int tmp_y = start_y;
		start_y = end_y;
		end_y = tmp_y;
		int tmp_x = start_x;
		start_x = end_x;
		end_x = tmp_x;
	}

	if (y < start_y || y > end_y)
		return false;

	if (y == start_y && y == end_y) {
		/* Single line selection */
		return x >= start_x && x < end_x;
	} else if (y == start_y) {
		/* First line of multi-line selection */
		return x >= start_x;
	} else if (y == end_y) {
		/* Last line of multi-line selection */
		return x < end_x;
	} else {
		/* Middle lines of multi-line selection */
		return true;
	}
}

/* Get the selected text as a string */
static char *selection_get_text(void)
{
	if (!ec.selection.active)
		return NULL;

	int start_y = ec.selection.start_y;
	int start_x = ec.selection.start_x;
	int end_y = ec.selection.end_y;
	int end_x = ec.selection.end_x;

	/* Ensure selection is within valid rows */
	if (start_y >= NR || end_y >= NR)
		return NULL;

	/* Normalize selection (ensure start comes before end) */
	if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
		int tmp_y = start_y;
		start_y = end_y;
		end_y = tmp_y;
		int tmp_x = start_x;
		start_x = end_x;
		end_x = tmp_x;
	}

	/* Calculate total size needed */
	size_t total_size = 0;
	if (start_y == end_y) {
		/* Single line selection */
		if (start_y < NR) {
			editor_row_t *row = ROW(start_y);
			if (row->chars) {	/* Safety check */
				int actual_start =
				    start_x > row->size ? row->size : start_x;
				int actual_end =
				    end_x > row->size ? row->size : end_x;
				if (actual_end > actual_start)
					total_size = actual_end - actual_start;
			}
		}
	} else {
		/* Multi-line selection - properly count newlines */
		for (int y = start_y; y <= end_y && y < NR; y++) {
			editor_row_t *row = ROW(y);
			if (!row->chars) {
				/* Empty row, just count newline if not last line */
				if (y < end_y)
					total_size++;
				continue;
			}

			if (y == start_y) {
				/* First line: from start_x to end of line */
				int actual_start =
				    start_x > row->size ? row->size : start_x;
				int len = row->size - actual_start;
				if (len > 0)
					total_size += len;
				if (y < end_y)
					total_size++;	/* Add newline if not last line */
			} else if (y == end_y) {
				/* Last line: from beginning to end_x */
				int actual_end =
				    end_x > row->size ? row->size : end_x;
				total_size += actual_end;
			} else {
				/* Middle lines: entire line */
				total_size += row->size;
				total_size++;	/* Add newline */
			}
		}
	}

	if (total_size == 0)
		return NULL;

	char *buffer = malloc(total_size + 1);
	if (!buffer)
		return NULL;

	/* Copy selected text */
	char *p = buffer;
	if (start_y == end_y) {
		/* Single line */
		if (start_y < NR) {
			editor_row_t *row = ROW(start_y);
			if (row->chars) {	/* Safety check */
				/* Clamp positions to actual row size */
				int actual_start =
				    start_x > row->size ? row->size : start_x;
				int actual_end =
				    end_x > row->size ? row->size : end_x;
				int len = actual_end - actual_start;
				if (len > 0) {
					memcpy(p, &row->chars[actual_start],
					       len);
					p += len;
				}
			}
		}
	} else {
		/* Multi-line - properly include newlines */
		for (int y = start_y; y <= end_y && y < NR; y++) {
			editor_row_t *row = ROW(y);
			if (!row->chars)
				continue;	/* Safety check */

			if (y == start_y) {
				/* First line: from start_x to end of line */
				int actual_start =
				    start_x > row->size ? row->size : start_x;
				int len = row->size - actual_start;
				if (len > 0) {
					memcpy(p, &row->chars[actual_start],
					       len);
					p += len;
				}
				/* Always add newline after first line if not the last line */
				if (y < end_y) {
					*p++ = '\n';
				}
			} else if (y == end_y) {
				/* Last line: from beginning to end_x */
				int actual_end =
				    end_x > row->size ? row->size : end_x;
				if (actual_end > 0) {
					memcpy(p, row->chars, actual_end);
					p += actual_end;
				}
			} else {
				/* Middle lines: entire line with newline */
				if (row->size > 0) {
					memcpy(p, row->chars, row->size);
					p += row->size;
				}
				*p++ = '\n';	/* Add newline after middle lines */
			}
		}
	}
	*p = '\0';

	return buffer;
}

/* Indent or unindent every line covered by the active selection.
 * dir > 0 inserts "indent" at each line start, dir < 0 removes up to
 * one leading occurrence of it.  The whole operation is a single undo
 * step; the selection stays active and its offsets are adjusted so the
 * key can be repeated immediately. */
static void selection_indent(int dir, const char *indent)
{
	if (!ec.selection.active || !indent || !*indent)
		return;
	burst_break();
	size_t ilen = strlen(indent);

	int start_y = ec.selection.start_y;
	int end_y = ec.selection.end_y;

	/* Normalize the row range only; column offsets handled below. */
	if (start_y > end_y) {
		int ty = start_y;
		start_y = end_y;
		end_y = ty;
		int tx = ec.selection.start_x;
		ec.selection.start_x = ec.selection.end_x;
		ec.selection.end_x = tx;
	}
	if (end_y >= NR)
		end_y = NR - 1;
	if (start_y > end_y)
		return;

	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	int first_touched = -1, last_touched = -1;
	g_undo.batching = true;
	for (int y = start_y; y <= end_y; y++) {
		editor_row_t *row = ROW(y);
		if (!row)
			continue;
		bool changed = false;
		int shift = 0;
		if (dir > 0) {
			changed = undo_insert_bytes(y, 0, indent, ilen);
			shift = changed ? (int) ilen : 0;
		} else {
			size_t strip_len = indent_strip_len(row, indent, ilen);
			if (strip_len > 0) {
				changed = undo_delete_bytes(y, 0, strip_len);
				shift = -(int) strip_len;
			}
		}
		if (!changed) {
			if (dir > 0)
				break;	/* OOM: keep undo consistent */
			continue;
		}
		ec.modified = true;
		if (first_touched < 0)
			first_touched = y;
		last_touched = y;
		if (y == start_y)
			ec.selection.start_x += shift;
		if (y == end_y)
			ec.selection.end_x += shift;
		if (ec.cursor_y == y)
			ec.cursor_x += shift;
	}
	g_undo.batching = false;

	/* Record the exact range actually touched, not the selection:
	 * empty lines at the end of a block get no indent and must not
	 * be part of the undo item. */
	undo_record_block_indent(first_touched < 0 ? start_y : first_touched,
				 last_touched, indent, ilen, dir,
				 before_y, before_x, ec.cursor_y,
				 ec.cursor_x);
}

/* Copy selected text to clipboard */
static void selection_copy(void)
{
	if (!ec.selection.active) {
		ui_set_message("No selection to copy");
		return;
	}

	char *text = selection_get_text();
	if (text) {
		free(ec.copied_char_buffer);
		ec.copied_char_buffer = text;
		ui_set_message("Selection copied (%zu bytes)", strlen(text));
	}
}

/* Delete selected text */
static void selection_delete(void)
{
	if (!ec.selection.active)
		return;
	burst_break();

	int start_y = ec.selection.start_y;
	int start_x = ec.selection.start_x;
	int end_y = ec.selection.end_y;
	int end_x = ec.selection.end_x;

	/* Normalize selection */
	if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
		int tmp_y = start_y;
		start_y = end_y;
		end_y = tmp_y;
		int tmp_x = start_x;
		start_x = end_x;
		end_x = tmp_x;
	}
	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	char *deleted_text = selection_get_text();
	size_t deleted_len = deleted_text ? strlen(deleted_text) : 0;

	if (start_y == end_y) {
		editor_row_t *row = ROW(start_y);
		if (row && end_x > start_x) {
			memmove(&row->chars[start_x], &row->chars[end_x],
				row->size - end_x + 1);
			row->size -= end_x - start_x;
			row_update(row, start_y);
			ec.modified = true;
		}
	} else {
		editor_row_t *start_row = ROW(start_y);
		editor_row_t *end_row = ROW(end_y);
		if (start_row && end_row) {
			int suffix_len = end_row->size - end_x;
			char *nc =
			    realloc(start_row->chars, start_x + suffix_len + 1);
			if (!nc) {
				ec.selection.active = false;
				mode_set(MODE_NORMAL);
				return;
			}
			start_row->chars = nc;
			memcpy(&start_row->chars[start_x],
			       &end_row->chars[end_x], suffix_len);
			start_row->size = start_x + suffix_len;
			start_row->chars[start_row->size] = '\0';
			row_update(start_row, start_y);
			for (int y = end_y; y > start_y; y--)
				row_erase(y);
			ec.modified = true;
		}
		ec.cursor_y = start_y;
		ec.cursor_x = start_x;
	}

	ec.selection.active = false;
	mode_set(MODE_NORMAL);
	if (deleted_text && deleted_len > 0) {
		undo_record_delete(start_y, start_x, deleted_text, deleted_len,
				   before_y, before_x, ec.cursor_y,
				   ec.cursor_x);
	}
	free(deleted_text);
}

/* Cut selected text (copy then delete) */
static void selection_cut(void)
{
	if (!ec.selection.active) {
		ui_set_message("No selection to cut");
		return;
	}

	selection_copy();
	selection_delete();
	ui_set_message("Selection cut");
}

static void editor_newline(void)
{
	/* A newline does not break the burst: it is part of the pasted
	 * text, and undo_apply_insert_text() replays it by splitting on
	 * '\n', so the recorded item still describes the edit. */
	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	if (ec.cursor_x == 0) {
		row_insert(ec.cursor_y, "", 0);
		hl_invalidate_from(ec.cursor_y);
	} else {
		editor_row_t *row = ROW(ec.cursor_y);
		row_insert(ec.cursor_y + 1, &row->chars[ec.cursor_x],
			   row->size - ec.cursor_x);
		row = ROW(ec.cursor_y);
		row->size = ec.cursor_x;
		row->chars[row->size] = '\0';
		row_update(row, ec.cursor_y);
	}
	ec.cursor_y++;
	ec.cursor_x = 0;
	ec.modified = true;
	burst_append("\n", 1);
	if (ec.line_ending == LINE_ENDING_UNKNOWN)
		ec.line_ending = LINE_ENDING_LF;
	if (!g_undo.replaying && !g_undo.batching) {
		static const char nl[] = "\n";
		undo_record_insert(before_y, before_x, nl, 1, before_y,
				   before_x, ec.cursor_y, ec.cursor_x);
	}
}

/* Buffer for accumulating UTF-8 bytes */
static struct {
	char bytes[4];
	int len;
	int expected;
} utf8_buffer = {
.len = 0,.expected = 0};

/* Text of the burst currently being applied, in buffer order.  A paste
 * arrives as a run of keys, but it is one user action, so it must be
 * one undo step.  While open, the key handlers append what they insert
 * and skip their own undo records; the burst is closed when the input
 * queue drains and one EDIT_INSERT item covers the whole run.
 * undo_apply_insert_text already replays multi-line text, splitting on
 * '\n', so the replay needs no special case. */
static struct {
	char *buf;
	size_t len;
	size_t cap;
	bool open;
	bool pure;		/* every key so far only inserted text */
	int row, col;		/* where the burst started */
} g_burst = { 0 };

/* Append pasted text to the open burst. */
static void burst_append(const char *s, size_t n)
{
	if (!g_burst.open || n == 0)
		return;
	if (g_burst.len < SIZE_MAX - 1 && g_burst.len + n + 1 > g_burst.cap) {
		size_t need = g_burst.len + n + 1;
		size_t cap = g_burst.cap ? g_burst.cap * 2 : 1024;
		while (cap < need) {
			if (cap > SIZE_MAX / 2)
				return;
			cap *= 2;
		}
		char *nb = realloc(g_burst.buf, cap);
		if (!nb)
			return;	/* drop the text; the edit itself stands */
		g_burst.buf = nb;
		g_burst.cap = cap;
	}
	memcpy(g_burst.buf + g_burst.len, s, n);
	g_burst.len += n;
	g_burst.buf[g_burst.len] = '\0';
}

/* Open a burst at the cursor.  Suppresses the per-key undo records
 * while it is open. */
static void burst_open(int row, int col)
{
	if (g_burst.open)
		return;
	g_burst.len = 0;
	g_burst.pure = true;
	g_burst.row = row;
	g_burst.col = col;
	g_burst.open = true;
	g_undo.batching = true;
}

/* A burst is one undo step, but only while it is nothing but inserted
 * text.  A cut, a paste, a delete or a cursor move inside the burst
 * changes the buffer somewhere the recorded start row and column do
 * not describe, so the accumulated text no longer matches the
 * position and the single item would undo the wrong bytes.
 *
 * Such a key ends the burst.  The text typed so far is flushed as its
 * own step first, because g_undo.batching is still suppressing the
 * per-key records that would otherwise cover it, and the key that
 * breaks the burst then records its own edit with batching off. */
static void burst_break(void)
{
	if (!g_burst.open)
		return;
	/* Flush before close: burst_close() clears batching, and the
	 * record helper drops the item while batching is still on. */
	bool pure = g_burst.pure;
	size_t len = g_burst.len;
	g_burst.open = false;
	g_undo.batching = false;
	if (pure && len > 0)
		undo_record_insert(g_burst.row, g_burst.col, g_burst.buf,
				   len, g_burst.row, g_burst.col,
				   ec.cursor_y, ec.cursor_x);
}

/* Close the burst and record it as one undo step. */
static void burst_close(void)
{
	if (!g_burst.open)
		return;
	bool pure = g_burst.pure;
	g_burst.open = false;
	g_undo.batching = false;
	if (pure && g_burst.len > 0)
		undo_record_insert(g_burst.row, g_burst.col, g_burst.buf,
				   g_burst.len, g_burst.row, g_burst.col,
				   ec.cursor_y, ec.cursor_x);
}

static void editor_insert_char(int c, bool manual_typing)
{
	unsigned char byte = (unsigned char)c;

	/* Check if this is the start of a UTF-8 sequence */
	if (utf8_buffer.len == 0) {
		if (byte <= 0x7F) {
			/* ASCII - insert immediately */
			utf8_buffer.expected = 1;
		} else if ((byte & 0xE0) == 0xC0) {
			/* 2-byte UTF-8 */
			utf8_buffer.expected = 2;
		} else if ((byte & 0xF0) == 0xE0) {
			/* 3-byte UTF-8 */
			utf8_buffer.expected = 3;
		} else if ((byte & 0xF8) == 0xF0) {
			/* 4-byte UTF-8 */
			utf8_buffer.expected = 4;
		} else {
			/* Invalid UTF-8 start byte */
			return;
		}
	}

	/* Add byte to buffer */
	utf8_buffer.bytes[utf8_buffer.len++] = c;

	/* If we haven't collected all bytes yet, return */
	if (utf8_buffer.len < utf8_buffer.expected)
		return;

	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	if (ec.cursor_y == NR)
		row_insert(NR, "", 0);
	editor_row_t *row = ROW(ec.cursor_y);
	size_t new_content_len = (size_t) row->size + (size_t) utf8_buffer.len;
	size_t alloc_len =
	    manual_typing ? row_manual_alloc_size(new_content_len)
	    : (new_content_len + 1);
	char *nc = realloc(row->chars, alloc_len);
	if (!nc) {
		utf8_buffer.len = 0;
		utf8_buffer.expected = 0;
		return;
	}
	row->chars = nc;
	memmove(&row->chars[ec.cursor_x + utf8_buffer.len],
		&row->chars[ec.cursor_x], row->size - ec.cursor_x + 1);
	memcpy(&row->chars[ec.cursor_x], utf8_buffer.bytes, utf8_buffer.len);
	row->size += utf8_buffer.len;
	row_update(row, ec.cursor_y);
	ec.cursor_x += utf8_buffer.len;
	ec.modified = true;
	burst_append(utf8_buffer.bytes, (size_t) utf8_buffer.len);
	if (!g_undo.replaying && !g_undo.batching) {
		undo_record_insert(before_y, before_x, utf8_buffer.bytes,
				   (size_t) utf8_buffer.len, before_y, before_x,
				   ec.cursor_y, ec.cursor_x);
	}

	/* Reset UTF-8 buffer */
	utf8_buffer.len = 0;
	utf8_buffer.expected = 0;
}

static void editor_delete_char(void)
{
	burst_break();
	/* The cursor may sit on the virtual line past the last row, which
	 * is where nano puts it and where the next character lands.  There
	 * Backspace removes the newline before the last line, the same
	 * join the start-of-row path does at the end of a line. */
	if (ec.cursor_y >= NR) {
		if (NR == 0)
			return;	/* nothing to join */
		int before_y = ec.cursor_y, before_x = ec.cursor_x;
		/* Join the row above with the last row, which is what the
		 * newline between them held. */
		ec.cursor_y = NR - 1;
		ec.cursor_x = ROW(NR - 1)->size;
		int prev_size = ec.cursor_x;
		editor_row_t *prev_row = ROW(NR - 2);
		editor_row_t *last = ROW(NR - 1);
		/* The last row keeps its text: the newline is the only
		 * thing removed, and the virtual line is empty, so the
		 * result is the last row merged into the one above. */
		if (prev_row && last) {
			/* Record the index before the erase: row_erase()
			 * drops a row, so every index below it moves. */
			int joined = NR - 2;
			char *new_chars =
			    realloc(prev_row->chars,
				    (size_t) prev_row->size + (size_t) last->size + 1);
			if (!new_chars) {
				ec.cursor_y = before_y;
				ec.cursor_x = before_x;
				return;
			}
			prev_row->chars = new_chars;
			memcpy(&prev_row->chars[prev_row->size], last->chars,
			       (size_t) last->size);
			prev_row->size += last->size;
			prev_row->chars[prev_row->size] = '\0';
			row_update(prev_row, joined);
			row_erase(NR - 1);
			ec.cursor_y = joined;
			ec.cursor_x = prev_size;
			ec.modified = true;
			if (!g_undo.replaying && !g_undo.batching) {
				static const char nl[] = "\n";
				undo_record_delete(joined, prev_size, nl, 1,
						   before_y, before_x,
						   ec.cursor_y, ec.cursor_x);
			}
		}
		return;
	}
	if (ec.cursor_x == 0 && ec.cursor_y == 0)
		return;

	editor_row_t *row = ROW(ec.cursor_y);
	if (!row)
		return;		/* cursor below the last row: nothing to join */
	int before_y = ec.cursor_y, before_x = ec.cursor_x;

	if (ec.cursor_x > 0) {
		/* Delete character before cursor */
		const char *prev =
		    utf8_prev_char(row->chars, row->chars + ec.cursor_x);
		int prev_pos = prev - row->chars, char_len =
		    ec.cursor_x - prev_pos;
		char *deleted =
		    xstrndup0(&row->chars[prev_pos], (size_t) char_len);

		memmove(&row->chars[prev_pos], &row->chars[ec.cursor_x],
			row->size - ec.cursor_x + 1);
		row->size -= char_len;
		row_update(row, ec.cursor_y);
		ec.cursor_x = prev_pos;
		ec.modified = true;
		if (deleted) {
			undo_record_delete(before_y, prev_pos, deleted,
					   (size_t) char_len, before_y,
					   before_x, ec.cursor_y, ec.cursor_x);
			free(deleted);
		}
	} else {
		/* Delete newline - join with previous line */
		if (ec.cursor_y > 0) {
			int prev_size = ROW(ec.cursor_y - 1)->size;
			ec.cursor_x = prev_size;
			editor_row_t *prev_row = ROW(ec.cursor_y - 1);
			char *new_chars = realloc(prev_row->chars,
						  prev_row->size + row->size +
						  1);
			if (!new_chars)
				return;
			prev_row->chars = new_chars;
			memcpy(&prev_row->chars[prev_row->size], row->chars,
			       row->size);
			prev_row->size += row->size;
			prev_row->chars[prev_row->size] = '\0';
			row_update(prev_row, ec.cursor_y - 1);
			row_erase(ec.cursor_y);
			ec.cursor_y--;
			ec.modified = true;
			if (!g_undo.replaying && !g_undo.batching) {
				static const char nl[] = "\n";
				undo_record_delete(before_y - 1, prev_size, nl,
						   1, before_y, before_x,
						   ec.cursor_y, ec.cursor_x);
			}
		}
	}
}

static const struct {
	const char bytes[3];
	unsigned char len;
} line_ending_info[] = {
	{ "", 0 },
	{ "\n", 1 },
	{ "\r", 1 },
	{ "\r\n", 2 },
};

static bool line_has_trailing_eol_char(const char *line, ssize_t line_len)
{
	return line_len > 0 &&
	    (line[line_len - 1] == '\n' || line[line_len - 1] == '\r');
}

static line_ending_t line_ending_detect(const char *line, ssize_t line_len)
{
	if (line_len >= 2 && line[line_len - 2] == '\r' &&
	    line[line_len - 1] == '\n')
		return LINE_ENDING_CRLF;
	if (line_has_trailing_eol_char(line, line_len) &&
	    line[line_len - 1] == '\n')
		return LINE_ENDING_LF;
	if (line_has_trailing_eol_char(line, line_len) &&
	    line[line_len - 1] == '\r')
		return LINE_ENDING_CR;
	return LINE_ENDING_UNKNOWN;
}

static bool file_write_chunk(FILE *file, const void *data, size_t len,
			     size_t *total_written)
{
	if (len == 0)
		return true;
	if (fwrite(data, 1, len, file) != len)
		return false;
	*total_written += len;
	return true;
}

static void browser_set_base_dir_from_path(const char *path)
{
	if (!path || !*path) {
		snprintf(ec.browser_base_dir, sizeof(ec.browser_base_dir), ".");
		return;
	}
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	while (len > 1 && tmp[len - 1] == '/') {
		tmp[len - 1] = '\0';
		len--;
	}
	char *slash = strrchr(tmp, '/');
	if (!slash) {
		snprintf(ec.browser_base_dir, sizeof(ec.browser_base_dir), ".");
	} else if (slash == tmp) {
		snprintf(ec.browser_base_dir, sizeof(ec.browser_base_dir), "/");
	} else {
		*slash = '\0';
		snprintf(ec.browser_base_dir, sizeof(ec.browser_base_dir), "%s",
			 tmp);
	}
}

static FILE* try_open(const char *file_name, bool* m) {
	static const char mode[][3] = {"r+", "r"};
	*m = 0;
	FILE *f;
	for(;;) {
		if((f = fopen(file_name, mode[*m==true]))) break;
		if(errno == ENOENT) break;
		else if(errno != EACCES) break;
		if(*m) break;
		*m = true;
	}
	return f;
}

/* Drop every row and reset the view, without touching the name, the
 * undo history or the compiled syntax.  The caller has already decided
 * this is a fresh buffer. */
static void file_clear_buffer(void)
{
	undo_history_clear();
	for (int i = 0; i < NR; i++)
		row_free_contents(ROW(i));
	tlist_free_items(ec.rows);

	ec.cursor_x = 0;
	ec.cursor_y = 0;
	ec.row_offset = 0;
	ec.col_offset = 0;
	ec.render_x = 0;
	ec.file_size_bytes = 0;
	ec.longest_line = 0;
	ec.line_ending = LINE_ENDING_UNKNOWN;
	ec.modified = false;
	hl_mark_all();
}

static bool file_open(const char *file_name)
{
	/* Read first, clear second.  Opening a directory or a file the
	 * user may not read fails, and the old order had already thrown
	 * the buffer away by then: the browser reported the failure and
	 * left an empty editor on screen, so every unsaved change was
	 * gone with no way back. */
	FILE *file = try_open(file_name, &ec.read_only);
	if (!file && errno != ENOENT)
		return false;
	/* No syntax on the old name: the rows are about to go. */
	syntax_reset_compiled_rules();
	ec.syntax = NULL;
	file_clear_buffer();

	free(ec.file_name);
	ec.file_name = strdup(file_name);
	browser_set_base_dir_from_path(file_name);
	syntax_select();
	if (!file) {
		ec.modified = false;
		return true;
	}

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	while ((line_len = getline(&line, &line_cap, file)) != -1) {
		ec.file_size_bytes += (size_t) line_len;
		/* Lock style from first terminated line; later lines are normalized. */
		if (ec.line_ending == LINE_ENDING_UNKNOWN)
			ec.line_ending = line_ending_detect(line, line_len);
		/* Strip any trailing CR/LF combination from getline() output. */
		while (line_has_trailing_eol_char(line, line_len)) --line_len;
		row_insert(NR, line, line_len);
		if (ec.syntax
		    && ec.file_size_bytes > SYNTAX_AUTO_DISABLE_THRESHOLD) {
			syntax_disable(false);
			ui_set_message
			    ("Syntax highlighting auto-disabled above %u MiB",
			     (unsigned) (SYNTAX_AUTO_DISABLE_THRESHOLD >> 20));
		}
	}
	free(line);
	fclose(file);
	syntax_select();
	if (ec.syntax && ec.file_size_bytes > SYNTAX_AUTO_DISABLE_THRESHOLD) {
		syntax_disable(false);
		ui_set_message("Syntax highlighting auto-disabled above %u MiB",
			       (unsigned) (SYNTAX_AUTO_DISABLE_THRESHOLD >> 20));
	}
	ec.modified = false;
	return true;
}

static bool file_save(void)
{
	char *name = ui_prompt("Save as: ", "^C: cancel",
			       ec.file_name, NULL);
	if (!name) {
		ui_set_message("Save aborted");
		return false;
	}
	bool name_changed = !ec.file_name || strcmp(ec.file_name, name) != 0;
	free(ec.file_name);
	ec.file_name = name;
	browser_set_base_dir_from_path(ec.file_name);
	if (name_changed)
		syntax_select();
	const char *line_ending = line_ending_info[ec.line_ending].bytes;
	size_t line_ending_len = line_ending_info[ec.line_ending].len;
	size_t total_written = 0;
	FILE *file = fopen(ec.file_name, "wb");
	bool ok = file != NULL;
	if (!ok) goto out;
	for (int j = 0; j < NR; j++) {
		editor_row_t *row = ROW(j);
		if (!file_write_chunk
		    (file, row->chars, (size_t)row->size, &total_written) ||
		    !file_write_chunk(file, line_ending, line_ending_len,
				      &total_written)) {
			ok = false;
			break;
		}
	}
	int write_errno = errno;
	int close_rc = fclose(file);
	if (ok && close_rc == 0) {
		ec.modified = false;
		if (total_written >= 1024)
			ui_set_message("%d KiB written to disk",
				       (int)(total_written >> 10));
		else
			ui_set_message("%d B written to disk",
				       (int)total_written);
		return ok;
	}
	int saved_errno = write_errno;
	if (!saved_errno && close_rc != 0)
		saved_errno = errno;
	if (!saved_errno)
		saved_errno = EIO;
	errno = saved_errno;
out:
	if (!ok)
		ui_set_message("Error: %s", strerror(errno));
	return ok;
}

/* Highlight the match at (row_idx, match_offset, match_len), saving the
 * previous highlight so it can be restored when leaving search mode. */
static void search_highlight_match(int row_idx, int match_offset, int match_len)
{
	editor_row_t *r = ROW(row_idx);
	if (!r) return;
	if (!r->hl_valid)
		syntax_ensure_row_highlighted(row_idx);
	if (!r->highlight || r->render_size <= 0)
		return;

	/* Restore previous highlight if we've moved to a different row */
	if (ec.mode_owners.search_saved_highlight &&
	    ec.mode_owners.search_highlight_line != row_idx) {
		int prev = ec.mode_owners.search_highlight_line;
		editor_row_t *prev_row = (prev >= 0 && prev < NR) ?
		    ROW(prev) : NULL;
		if (prev_row && prev_row->highlight &&
		    ec.mode_owners.search_saved_highlight_size ==
		    prev_row->render_size) {
			memcpy(prev_row->highlight,
			       ec.mode_owners.search_saved_highlight,
			       prev_row->render_size);
		}
		free(ec.mode_owners.search_saved_highlight);
		ec.mode_owners.search_saved_highlight = NULL;
		ec.mode_owners.search_highlight_line = -1;
	}

	/* Save current row's highlight if not already saved */
	if (!ec.mode_owners.search_saved_highlight) {
		ec.mode_owners.search_saved_highlight = malloc(r->render_size);
		if (!ec.mode_owners.search_saved_highlight)
			return;	/* Can't save highlight; skip marking to avoid inconsistency */
		memcpy(ec.mode_owners.search_saved_highlight, r->highlight,
		       r->render_size);
		ec.mode_owners.search_highlight_line = row_idx;
		ec.mode_owners.search_saved_highlight_size = r->render_size;
	}

	/* Bounds check: only mark if match fits within the render buffer */
	if (match_offset + match_len <= r->render_size)
		memset(&r->highlight[match_offset], MATCH, match_len);
	/* else: match extends past render_size; skip highlight silently */
}

/* Info about the last successful match, in chars space.  Populated by
 * search_do_from() so that the replace code knows where to cut. */
typedef struct {
	int row;		/* row index */
	int char_off;		/* byte offset in row->chars */
	int char_len;		/* byte length of match */
} search_match_t;
static search_match_t g_last_match = {.row = -1,.char_off = 0,.char_len = 0 };

/* Find the LAST match of query in r->chars[0..max_off].
 * Because libc provides only "find first", we scan forward collecting every
 * hit and keep the rightmost one that is still at or before max_off.
 * Works for both plain-text and regex; pass a compiled re for regex mode.
 * Returns true and fills *out_off / *out_len on success. */
static bool row_find_last_match(editor_row_t * r, int max_off,
				bool case_sens,
				const char *query, size_t qlen, regex_t * re,
				int *out_off, int *out_len)
{
	if (max_off < 0)
		return false;
	bool found = false;
	const char *hay = r->chars;
	const char *p = hay;
	while (p <= hay + max_off) {
		int mo, ml;
		if (re) {
			regmatch_t mm;
			if (regexec(re, p, 1, &mm, 0) != 0)
				break;
			int abs_so = (int)(p - hay) + (int)mm.rm_so;
			if (abs_so > max_off)
				break;
			mo = abs_so;
			ml = (int)(mm.rm_eo - mm.rm_so);
			p = hay + abs_so + 1;
			if ((int)(p - hay) > r->size)
				break;
		} else {
			const char *hit =
			    case_sens ? strstr(p, query) : strcasestr(p, query);
			if (!hit)
				break;
			int abs_so = (int)(hit - hay);
			if (abs_so > max_off)
				break;
			mo = abs_so;
			ml = (int)qlen;
			p = hit + 1;
		}
		*out_off = mo;
		*out_len = ml;
		found = true;
	}
	return found;
}

/* Find the FIRST match of query in r->chars[min_off..].
 * Returns true and fills *out_off / *out_len on success. */
static bool row_find_first_match(editor_row_t * r, int min_off,
				 bool case_sens,
				 const char *query, size_t qlen, regex_t * re,
				 int *out_off, int *out_len)
{
	if (min_off > r->size)
		return false;
	if (min_off < 0)
		min_off = 0;
	if (re) {
		regmatch_t m;
		if (regexec(re, r->chars + min_off, 1, &m, 0) != 0)
			return false;
		*out_off = min_off + (int)m.rm_so;
		*out_len = (int)(m.rm_eo - m.rm_so);
	} else {
		const char *src = r->chars + min_off;
		const char *hit =
		    case_sens ? strstr(src, query) : strcasestr(src, query);
		if (!hit)
			return false;
		*out_off = min_off + (int)(hit - src);
		*out_len = (int)qlen;
	}
	return true;
}

/* Execute search starting from (start_row, start_char_off).
 * If no_wrap is true, stops at the end (or beginning for backwards) of the
 * file without cycling back — used during replace to avoid re-matching
 * already-replaced text.  If no_wrap is false, wraps through all n rows.
 * Searches r->chars so char offsets map directly to the gap buffer.
 * SM_CASE_SENSITIVE, SM_BACKWARDS, and SM_REGEX are all honoured.
 * Returns true if a match was found; updates ec.cursor_y/x and g_last_match.
 * If out_wrapped is non-NULL, *out_wrapped is set to true if the result row
 * is in the "wrapped" portion of the search (i.e. we looped past the file
 * boundary to reach it). */
static void set_overlay_msg(const char *fmt, ...);	/* forward declaration */
static bool search_do_from(const char *query, int start_row, int start_char_off,
			   bool no_wrap, bool * out_wrapped)
{
	if (out_wrapped)
		*out_wrapped = false;
	if (!query || !*query)
		return false;
	int n = NR;
	if (n == 0)
		return false;
	bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
	bool use_regex = (ec.search.mode & SM_REGEX) != 0;
	bool case_sens = (ec.search.mode & SM_CASE_SENSITIVE) != 0;

	regex_t re;
	if (use_regex) {
		int flags = REG_EXTENDED | (case_sens ? 0 : REG_ICASE);
		if (regcomp(&re, query, flags) != 0)
			return false;
	}

	size_t qlen = use_regex ? 0 : strlen(query);
	int dir = backwards ? -1 : 1;
	/* A wrapping scan visits every row, then gives the start row a
	 * second pass with a full-width window.  Without that pass a match
	 * that sits before start_char_off on the start row is never seen:
	 * the row was reached once with the window already consumed, so
	 * ^W reported "not found" on a line that plainly held the text. */
	int max_rows = no_wrap ? (backwards ? start_row + 1 : n - start_row)
	    : n + 1;
	bool found = false;

	for (int i = 0; i < max_rows && !found; i++) {
		/* i == n is the second visit to the start row. */
		bool revisit = i >= n;
		int ri = no_wrap ? start_row + i * dir
		    : ((start_row + (revisit ? 0 : i) * dir) % n + n) % n;
		editor_row_t *r = ROW(ri);
		if (!r || !r->chars)
			continue;

		/* Window for this row: the start row honours
		 * start_char_off on the first pass and scans its whole width
		 * on the revisit, later rows scan from the end the search
		 * arrives at. */
		int search_off = revisit ? (backwards ? r->size : 0)
		    : (i == 0) ? start_char_off
		    : (backwards ? r->size : 0);

		int match_off = -1, match_len = -1;
		bool hit = backwards
		    ? row_find_last_match(r, search_off, case_sens,
					  query, qlen, use_regex ? &re : NULL,
					  &match_off, &match_len)
		    : row_find_first_match(r, search_off, case_sens,
					   query, qlen, use_regex ? &re : NULL,
					   &match_off, &match_len);

		if (hit) {
			ec.cursor_y = ri;
			ec.cursor_x = match_off;
			ec.row_offset = n;	/* trigger scroll recalc */
			g_last_match = (search_match_t) {
			ri, match_off, match_len};
			int render_off = row_cursorx_to_renderx(r, match_off);
			int render_end =
			    row_cursorx_to_renderx(r, match_off + match_len);
			int render_len = render_end - render_off;
			if (render_len <= 0 && match_len > 0)
				render_len = 1;
			if (render_len > 0)
				search_highlight_match(ri, render_off,
						       render_len);
			/* Detect wrap: for forward search ri wrapped if ri < start_row (after i>0);
			 * for backward search ri wrapped if ri > start_row (after i>0). */
			if (out_wrapped && i > 0)
				*out_wrapped =
				    backwards ? (ri > start_row) : (ri <
								    start_row);
			found = true;
		}
	}

	if (use_regex)
		regfree(&re);
	return found;
}

/* Convenience wrapper: search from just past the current cursor position,
 * wrapping through the whole file.  Starts on the current line so that
 * matches later on the same line are found before wrapping to the next/previous
 * line.  Uses cursor_x+1 (forward) or cursor_x-1 (backward) to avoid
 * re-finding a match the cursor is already sitting on.
 * Shows "[ search wrapped ]" overlay when the match is in the wrapped portion. */
static bool search_do(const char *query)
{
	if (!query || !*query || NR == 0)
		return false;
	bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
	bool wrapped = false;
	bool found;
	if (backwards)
		found =
		    search_do_from(query, ec.cursor_y, ec.cursor_x - 1, false,
				   &wrapped);
	else
		found =
		    search_do_from(query, ec.cursor_y, ec.cursor_x + 1, false,
				   &wrapped);
	if (found && wrapped)
		set_overlay_msg("search wrapped");
	return found;
}

/* Perform one text replacement at g_last_match. */
static bool do_replace_one(const char *replacement, size_t repl_len)
{
	burst_break();
	if (g_last_match.row < 0 || g_last_match.row >= NR)
		return false;
	editor_row_t *row = ROW(g_last_match.row);
	if (!row)
		return false;
	int off = g_last_match.char_off;
	int oldlen = g_last_match.char_len;
	int before_y = ec.cursor_y, before_x = ec.cursor_x;
	char *old_text = NULL;
	old_text = xstrndup0(&row->chars[off], (size_t) oldlen);
	if (!old_text)
		goto out;
	size_t new_size = (size_t) row->size - (size_t) oldlen + repl_len;
	if (new_size > INT_MAX)
		goto out;
	/* Resize to the larger of old/new before shifting the tail: the
	 * memmove below reads the terminator at chars[row->size], which a
	 * shrink-to-new_size realloc would already have cut off.  Reading
	 * past a shrunken block copies adjacent heap bytes into the line. */
	size_t stage_size = ((size_t) row->size + 1 > new_size + 1) ?
	    (size_t) row->size + 1 : new_size + 1;
	char *nc = realloc(row->chars, stage_size);
	if (!nc)
		goto out;
	row->chars = nc;
	memmove(&row->chars[off + repl_len],
		&row->chars[off + oldlen], row->size - off - oldlen + 1);
	if (repl_len > 0)
		memcpy(&row->chars[off], replacement, repl_len);
	row->size = (int)new_size;
	row->chars[row->size] = '\0';
	if (stage_size != new_size + 1) {
		/* Shrink back to the exact size; keep old block on failure. */
		nc = realloc(row->chars, new_size + 1);
		if (nc)
			row->chars = nc;
	}
	row_update(row, g_last_match.row);
	ec.modified = true;
	undo_record_replace(g_last_match.row, off, old_text, (size_t) oldlen,
			    replacement, repl_len,
			    before_y, before_x, ec.cursor_y, ec.cursor_x);
	free(old_text);
	return true;
 out:
	free(old_text);
	return false;
}

/* Set the transient overlay message (cleared at the start of the next keypress). */
static void set_overlay_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ec.overlay_msg, sizeof(ec.overlay_msg), fmt, ap);
	va_end(ap);
}

/* Returns true if, after wrapping, the current cursor position has gone at or
 * past the original replace-start position (completing exactly one full cycle).
 * Uses ec.search.has_wrapped rather than a per-call flag so wrap is
 * remembered across multiple search_do_from calls in the same session. */
static bool replace_past_origin(bool backwards)
{
	if (!ec.search.has_wrapped || ec.search.orig_row < 0)
		return false;
	int or = ec.search.orig_row, oc = ec.search.orig_char;
	return backwards ? (ec.cursor_y < or
			    || (ec.cursor_y == or && ec.cursor_x <= oc))
	    : (ec.cursor_y > or || (ec.cursor_y == or && ec.cursor_x >= oc));
}

/* Replace the current g_last_match and advance to the next occurrence,
 * wrapping with a stop at the original replace-start position to avoid
 * cycling past the starting point.  Returns true if another match was found. */
static bool replace_and_advance(const char *rq, size_t rqlen)
{
	bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
	bool already_wrapped = ec.search.has_wrapped;
	int prev_off = g_last_match.char_off;
	if (!do_replace_one(rq, rqlen))
		return false;	/* invalid match: stop instead of looping */
	editor_row_t *rp = ROW(g_last_match.row);
	/* The row shrank/grew during the replace; clamp the skip offset so
	 * the next search never starts past the string terminator. */
	ec.search.replace_count++;
	/* Advance past the replaced range so we don't re-match it. */
	int skip_off;
	if (backwards) {
		skip_off = prev_off - 1;	/* look strictly before the replaced position */
	} else {
		/* New line: prefix + replacement + old tail, so the old tail
		 * starts at prev_off + rqlen. Using the old match length skips
		 * matches when the row shrank (rqlen < old match length). */
		int advance = (int)rqlen;
			/* rqlen==0 (empty replacement) with a zero-width match would
			 * leave skip_off==prev_off and cause an infinite loop. */
		if (advance <= 0)
			advance = 1;
		skip_off = prev_off + advance;
	}
	if (!backwards && rp && skip_off > rp->size)
		skip_off = rp->size;
	bool wrapped = false;
	bool found =
	    search_do_from(ec.search.query, ec.cursor_y, skip_off, false,
			   &wrapped);
	if (wrapped)
		ec.search.has_wrapped = true;
	if (!found)
		return false;
	/* A second wrap means we've completed one full cycle and would start
	 * revisiting matches (notably with case-insensitive replacements like
	 * "bool" -> "BOOL"), so stop here. */
	if (already_wrapped && wrapped)
		return false;
	return !replace_past_origin(backwards);
}

/* Skip the current match without replacing and advance to the next occurrence. */
static bool replace_skip(void)
{
	bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
	bool already_wrapped = ec.search.has_wrapped;
	int skip_off = backwards ? g_last_match.char_off - 1
	    : g_last_match.char_off + 1;
	bool wrapped = false;
	bool found =
	    search_do_from(ec.search.query, ec.cursor_y, skip_off, false,
			   &wrapped);
	if (wrapped)
		ec.search.has_wrapped = true;
	if (!found)
		return false;
	/* Same full-cycle guard as replace_and_advance(). */
	if (already_wrapped && wrapped)
		return false;
	return !replace_past_origin(backwards);
}

static void replace_finish(bool always_show_msg);

static void replace_confirm_step(void)
{
	const char *rq = ec.search.replace_query ? ec.search.replace_query : "";
	size_t rqlen = ec.search.replace_len;
	char *const options[] = { "Yes", "No", "All", NULL };
	int r = ui_dialog_ask("Replace this instance?", options);
	if (r == 0) {
		if (!replace_and_advance(rq, rqlen))
			replace_finish(true);
	} else if (r == 1) {
		if (!replace_skip())
			replace_finish(false);
	} else if (r == 2) {
		while (replace_and_advance(rq, rqlen)) ;
		replace_finish(true);
	} else {
		replace_finish(false);
	}
}

/* Finish a replace session: reset state, return to NORMAL, optionally show count.
 * SM_REPLACE is cleared so the next ^W launches a plain search. */
static void replace_finish(bool always_show_msg)
{
	int cnt = ec.search.replace_count;
	ec.search.replace_count = 0;
	ec.search.replace_phase = 0;
	ec.search.orig_row = -1;
	ec.search.has_wrapped = false;
	ec.search.mode &= ~SM_REPLACE;
	mode_set(MODE_NORMAL);
	if (always_show_msg || cnt > 0)
		set_overlay_msg("Replaced %d occurrence%s", cnt,
				cnt == 1 ? "" : "s");
}

static void search_find(void)
{
	/* Save cursor position before mode_set() clears mode_state */
	int sx = ec.cursor_x, sy = ec.cursor_y;
	int sc = ec.col_offset, sr = ec.row_offset;
	mode_set(MODE_SEARCH);
	ec.mode_state.search.saved_x = sx;
	ec.mode_state.search.saved_y = sy;
	ec.mode_state.search.saved_col = sc;
	ec.mode_state.search.saved_row = sr;
	editor_refresh();	/* Show the [SEARCH] statusbar immediately */
}

static void search_clear_prefilled_query(void)
{
	if (!ec.search.query) {
		ec.search.prefill_from_start = false;
		return;
	}
	ec.search.query_len = 0;
	ec.search.query[0] = '\0';
	ec.search.prefill_from_start = false;
}

static void buf_append(editor_buf_t * eb, const char *s, int len)
{
	if (len <= 0)
		return;
	/* Grow by doubling.  A repaint appends per byte, so reallocating
	 * to the exact size each time made every repaint do thousands of
	 * reallocs and copy what is already there. */
	if (eb->len > 0 && len > INT_MAX - eb->len)
		return; /* would overflow int len */
	if (eb->len + len > eb->cap) {
		int need = eb->len + len;
		int cap = eb->cap ? eb->cap : 256;
		while (cap < need) {
			if (cap > INT_MAX / 2)
				return; /* doubling would overflow */
			cap *= 2;
		}
		char *new = realloc(eb->buf, (size_t) cap);
		if (!new)
			return;		/* drop the append; the frame is short */
		eb->buf = new;
		eb->cap = cap;
	}
	memcpy(&eb->buf[eb->len], s, (size_t) len);
	eb->len += len;
}

static void buf_destroy(editor_buf_t * eb)
{
	free(eb->buf);
}

/* Append the overlay message to an editor_buf_t (called at end of refresh). */
static void buf_append_overlay(editor_buf_t * eb)
{
	const char *msg = ec.overlay_msg;
	char framed_msg[sizeof(ec.overlay_msg) + 8];
	if (!msg[0]) {
		if (ec.mode != MODE_SEARCH || ec.status_msg[0])
			return;
		if (ec.search.replace_phase == 2)
			msg =
			    "Replace this instance?  [ Yes ]  [ No ]  [ All ]  ^C:Cancel";
		else if (ec.search.replace_phase == 1)
			msg =
			    "Enter replacement text, then press Enter  ^C:Cancel";
		else
			msg =
			    "M-C:Case Sens  M-B:Backwards  M-R:Regexp  ^R:Replace";
	}
	if (msg[0]) {
		snprintf(framed_msg, sizeof(framed_msg), "[ %s ]", msg);
		msg = framed_msg;
	}
	int msglen = (int)strlen(msg);
	int col = clamp((ec.screen_cols - msglen) / 2, 0, ec.screen_cols);
	char posbuf[32];
	snprintf(posbuf, sizeof(posbuf), "\x1b[%d;%dH\x1b[7m", ec.screen_rows,
		 col + 1);
	buf_append(eb, posbuf, strlen(posbuf));
	buf_append(eb, msg, msglen);
	buf_append(eb, "\x1b[m", 3);
}

/* Calculate line number display width */
static int get_line_number_width(void)
{
	if (!ec.show_line_numbers || NR == 0)
		return 0;

	/* Calculate width needed for line numbers */
	int max_line = NR;
	int width = 1;
	while (max_line >= 10) {
		width++;
		max_line /= 10;
	}
	return width + 2;	/* Add space for padding and separator */
}

static bool key_is_select_nav(int c)
{
	return (c & KEY_FLAG_NAV) != 0;
}

static bool key_is_shift_select_nav(int c)
{
	return (c & (KEY_FLAG_NAV | KEY_FLAG_SHIFT)) ==
	    (KEY_FLAG_NAV | KEY_FLAG_SHIFT);
}

static int key_to_select_nav(int c)
{
	if (!key_is_select_nav(c))
		return -1;
	return c & ~KEY_FLAG_SHIFT;
}

/* Move the cursor one page up or down and leave it on a row that
 * exists.  A page is the visible window, so the cursor and the window
 * both move by the height of the window and the cursor keeps its place
 * in it.  The old code jumped to the last visible line and then walked
 * down one row per screen row, which moved the cursor two pages and, on
 * a file shorter than the window, walked off the end onto the virtual
 * line past the last row. */
static void page_move_cursor(int key)
{
	int page = ec.screen_rows > 0 ? ec.screen_rows : 1;
	int last = NR > 0 ? NR - 1 : 0;
	int dir = (key == PAGE_UP) ? -1 : 1;
	ec.cursor_y = clamp(ec.cursor_y + dir * page, 0, last);
	/* Scroll by a page as well.  editor_scroll only pulls the window to
	 * the cursor, which would show one new line. */
	ec.row_offset = clamp(ec.row_offset + dir * page, 0, last);
	/* Keep the column on a row that exists. */
	if (ec.cursor_y < NR)
		ec.cursor_x = clamp(ec.cursor_x, 0, ROW(ec.cursor_y)->size);
	else
		ec.cursor_x = 0;
}

static void selection_move_cursor(int key)
{
	int nav = key_to_select_nav(key);
	if (nav < 0)
		return;

	switch (nav) {
	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editor_move_cursor(nav);
		/* Ensure selection doesn't go past last row */
		if (ec.cursor_y >= NR && NR > 0) {
			ec.cursor_y = NR - 1;
			ec.cursor_x = ROW(ec.cursor_y)->size;
		}
		break;
	case HOME_KEY:
		ec.cursor_x = 0;
		break;
	case END_KEY:
		if (ec.cursor_y < NR)
			ec.cursor_x = ROW(ec.cursor_y)->size;
		break;
	case PAGE_UP:
	case PAGE_DOWN:
		page_move_cursor(nav);
		break;
	default:
		break;
	}

	ec.selection.end_x = ec.cursor_x;
	ec.selection.end_y = ec.cursor_y;
}

static void editor_scroll(void)
{
	ec.render_x = 0;
	if (ec.cursor_y < NR)
		ec.render_x =
		    row_cursorx_to_renderx(ROW(ec.cursor_y), ec.cursor_x);
	if (ec.cursor_y < ec.row_offset)
		ec.row_offset = ec.cursor_y;
	if (ec.cursor_y >= ec.row_offset + ec.screen_rows)
		ec.row_offset = ec.cursor_y - ec.screen_rows + 1;

	/* Adjust horizontal scrolling for line numbers */
	int line_num_width = get_line_number_width();
	int available_cols = ec.screen_cols - line_num_width;

	if (ec.render_x < ec.col_offset)
		ec.col_offset = ec.render_x;
	if (ec.render_x >= ec.col_offset + available_cols)
		ec.col_offset = ec.render_x - available_cols + 1;
}

static int statusbar_build_query(char *dst, size_t cap, const char *prefix,
				 const char *query, int *query_start_col)
{
	int off = snprintf(dst, cap, "%s", prefix ? prefix : "");
	if (off < 0)
		off = 0;
	if (off >= (int)cap)
		off = (int)cap - 1;
	*query_start_col = off + 1;
	{
		int qlen =
		    snprintf(dst + off, cap - (size_t)off, "%s", query ? query : "");
		if (qlen < 0)
			qlen = 0;
		if (qlen > (int)cap - 1 - off)
			qlen = (int)cap - 1 - off;
		off += qlen;
	}
	return off;
}

static void ui_draw_statusbar(editor_buf_t * eb)
{
	buf_append(eb, "\x1b[93;44m", 8);	/* Yellow on blue */
	char status[80], r_status[128];
	r_status[0] = '\0';

	int len = 0, r_len = 0;
	ec.search.prompt_query_start_col = 0;
	if (ec.status_msg[0]) {
		/* Active prompt or dialog line uses gray background for readability. */
		int mlen = strlen(ec.status_msg);
		buf_append(eb, "\x1b[97;100m", 9);	/* White on gray */
		buf_append(eb, ec.status_msg, mlen);
		if (mlen < ec.screen_cols)
			buf_append(eb, "\x1b[K", 3);
		buf_append(eb, "\x1b[m", 3);
		return;
	} else if (ec.mode == MODE_SEARCH) {
		char prefix[80];
		const char *qtxt = "";
		if (ec.search.replace_phase == 1) {
			/* Phase 1: entering replacement text */
			snprintf(prefix, sizeof(prefix), " Replace with: ");
			if (ec.search.replace_query) qtxt = ec.search.replace_query;
		} else {
			/* Phase 0 and 2: show [SEARCH] / [REPLACE] with active flags and query */
			char flags[60];
			int flen = 0;
			const char *mode_label =
			    (ec.search.
			     mode & SM_REPLACE) ? "REPLACE" : "SEARCH";
			if (ec.search.mode & SM_CASE_SENSITIVE)
				flen +=
				    snprintf(flags + flen, sizeof(flags) - flen,
					     " [Case Sensitive]");
			if (ec.search.mode & SM_BACKWARDS)
				flen +=
				    snprintf(flags + flen, sizeof(flags) - flen,
					     " [Backwards]");
			if (ec.search.mode & SM_REGEX)
				flen +=
				    snprintf(flags + flen, sizeof(flags) - flen,
					     " [Regex]");
			if (flen == 0)
				flags[0] = '\0';
			snprintf(prefix, sizeof(prefix), " [%s]%s ", mode_label,
				 flags);
			if (ec.search.query) qtxt = ec.search.query;
		}
		len = statusbar_build_query(status, sizeof(status), prefix, qtxt,
					    &ec.search.prompt_query_start_col);
		r_len = 0;
		ec.search.prompt_query_start_col =
		    clamp(ec.search.prompt_query_start_col, 1, ec.screen_cols);
		len = clamp(len, 0, ec.screen_cols);
		buf_append(eb, status, len);
	} else {
		/* Build " [MODE] " left prefix */
		const char *mode_name = mode_get_name(ec.mode);
		char left[32];
		int left_vis =
		    snprintf(left, sizeof(left), " [%s] ", mode_name);
		left_vis = clamp(left_vis, 0, ec.screen_cols);

		/* Build right side: col/row info with zero-padded fixed-width numbers */
		int col_size =
		    (ec.cursor_y <= NR - 1) ? ROW(ec.cursor_y)->size : 0;
		int row_cur = (ec.cursor_y + 1 > NR) ? NR : ec.cursor_y + 1;
		int row_total = NR;
		int row_width = 1;
		{ int tmp = row_total > 0 ? row_total : 1; while (tmp >= 10) { row_width++; tmp /= 10; } }
		int col_max = ec.longest_line;
		if (col_size > col_max) col_max = col_size;
		if (ec.cursor_x + 1 > col_max) col_max = ec.cursor_x + 1;
		int col_width = 1;
		{ int tmp = col_max > 0 ? col_max : 1; while (tmp >= 10) { col_width++; tmp /= 10; } }
		/* Build numeric parts (plain text, used for visible length) */
		char row_part[32], col_part[32], fmt[32];
		/* Use a runtime-generated format string to avoid -Wformat-truncation
		 * from dynamic-width '%0*d'; the actual widths are tiny (1-10 digits). */
		snprintf(fmt, sizeof(fmt), "%%0%dd/%%0%dd", row_width, row_width);
		snprintf(row_part, sizeof(row_part), fmt, row_cur, row_total);
		snprintf(fmt, sizeof(fmt), "%%0%dd/%%0%dd", col_width, col_width);
		snprintf(col_part, sizeof(col_part), fmt, ec.cursor_x + 1, col_size);
		/* Colored version for emission; "L"+row_part+" C"+col_part = 3+lens */
		snprintf(r_status, sizeof(r_status), "\x1b[97mL\x1b[93m%s \x1b[97mC\x1b[93m%s", row_part, col_part);
		r_len = 3 + (int)strlen(row_part) + (int)strlen(col_part);

		const char *fname =
		    ec.file_name ? ec.file_name : "< New >";
		int fname_len = strlen(fname);
		int mod_vis = ec.modified ? 1 : 0;	/* asterisk */

		/* Max visible chars for filename: leave room for modified
		 * marker and at least 1 space before the col/row display */
		int max_fname_vis =
		    ec.screen_cols - left_vis - mod_vis -
		    (r_len > 0 ? 1 + r_len : 0);
		max_fname_vis = clamp(max_fname_vis, 0, ec.screen_cols);

		buf_append(eb, left, left_vis);	/* Left mode tag stays blue */
		len = left_vis;
		buf_append(eb, "\x1b[97;100m", 9);	/* Middle segment: white on gray */

		/* Append filename, tail-truncated with '<' if too long */
		if (fname_len <= max_fname_vis) {
			buf_append(eb, fname, fname_len);
			len += fname_len;
		} else if (max_fname_vis > 0) {
			buf_append(eb, "<", 1);
			len++;
			int tail = max_fname_vis - 1;
			if (tail > 0) {
				buf_append(eb, fname + fname_len - tail, tail);
				len += tail;
			}
		}

		/* Append white asterisk if modified (middle gray segment). */
		if (ec.modified) {
			buf_append(eb, "*", 1);
			len++;
		}
	}

	while (len < ec.screen_cols) {
		if (r_len > 0 && ec.screen_cols - len == r_len) {
			buf_append(eb, "\x1b[93;44m", 8);	/* Right position block blue */
			buf_append(eb, r_status, strlen(r_status));
			break;
		}
		buf_append(eb, " ", 1);
		len++;
	}
	buf_append(eb, "\x1b[m", 3);
}

static void ui_set_message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	vsnprintf(ec.overlay_msg, sizeof(ec.overlay_msg), msg, args);
	va_end(args);
}

static void ui_draw_rows(editor_buf_t * eb)
{
	/* Calculate line number width if enabled */
	int line_num_width = get_line_number_width();

	for (int y = 0; y < ec.screen_rows; y++) {
		int file_row = y + ec.row_offset;

		/* Draw line number if enabled */
		if (ec.show_line_numbers) {
			if (file_row < NR) {
				/* Draw line number */
				char line_num[32];
				int num_len =
				    snprintf(line_num, sizeof(line_num), "%*d ",
					     line_num_width - 1, file_row + 1);
				buf_append(eb, "\x1b[90m", 5);	/* Dark gray color */
				buf_append(eb, line_num, num_len);
				buf_append(eb, "\x1b[0m", 4);	/* Reset color */
			} else {
				/* Draw empty space for consistency */
				for (int i = 0; i < line_num_width; i++)
					buf_append(eb, " ", 1);
			}
		}

		if (file_row >= NR) {
			if (ec.show_whitespace) {
				buf_append(eb, TAB_HEAD_STYLE, sizeof(TAB_HEAD_STYLE)-1);
				buf_append(eb, "~", 1);
			}
		} else {
			editor_row_t *row = ROW(file_row);
			if (!row->hl_valid)
				syntax_ensure_row_highlighted(file_row);
			int available_cols = ec.screen_cols - line_num_width;
			int len = row->render_size - ec.col_offset;
			if (len < 0)
				len = 0;
			if (len > available_cols)
				len = available_cols;
			/* A row whose render or highlight allocation failed
			 * has nothing to draw.  Applying col_offset to
			 * NULL is undefined, so skip the row. */
			if (!row->render || !row->highlight)
				len = 0;
			char *c = row->render ? row->render + ec.col_offset
			    : NULL;
			unsigned char *hl = row->highlight
			    ? row->highlight + ec.col_offset : NULL;
			unsigned char current_style = NORMAL;
			bool in_selection = false;
			int prev_tab_cursor_x = -1;
			bool prev_tab_cursor_x_valid = false;
			/* The tab head and the selection test both need the byte
			 * offset of the column being drawn, and that offset
			 * used to be found by walking the row from its start,
			 * twice per column.  On a row with tabs or wide
			 * characters that walk is O(row length), so one
			 * repaint of a long line was quadratic in the width of
			 * the window.  The walk only moves forward, so one
			 * cursor for the whole column range replaces both. */
			/* The byte offset of a column, advanced one column at a
			 * time.  Both the selection test and the tab head need
			 * it, and it used to be found by walking the row from
			 * its start, twice per column.  On a row with tabs or
			 * wide characters that walk is O(row length), so one
			 * repaint of a long line was quadratic in the width of
			 * the window. */
			int scan_byte_pos = 0;
			int scan_render_x = 0;

			for (int j = 0; j < len; j++) {
				int render_pos = ec.col_offset + j;
				/* One step per column: a tab advances the byte
				 * offset past the bytes it covers, a plain byte
				 * by one, and a wide character by its length. */
				if (row->render_direct_map) {
					scan_byte_pos = render_pos;
				} else {
					while (scan_render_x < render_pos
					       && scan_byte_pos < row->size) {
						if (row->chars[scan_byte_pos]
						    == '\t') {
							scan_render_x +=
							    (TAB_STOP - 1)
							    - (scan_render_x %
							       TAB_STOP) + 1;
							scan_byte_pos++;
						} else {
							int clen = 1;
							scan_render_x +=
							    utf8_char_width_and_len
							    (&row->chars
							     [scan_byte_pos],
							     (size_t) (row->size - scan_byte_pos),
							     &clen);
							scan_byte_pos +=
							    clen;
						}
					}
					scan_render_x = render_pos;
				}
				int cursor_x = scan_byte_pos;
				bool is_selected =
				    selection_contains(cursor_x, file_row);

				/* Handle selection highlighting transitions */
				if (is_selected && !in_selection) {
					/* Enter selection - use inverse video */
					buf_append(eb, "\x1b[7m", 4);
					in_selection = true;
				} else if (!is_selected && in_selection) {
					/* Exit selection */
					buf_append(eb, "\x1b[27m", 5);
					in_selection = false;
				}

				/* The tab head is the first column of a tab, which is
				 * the column whose byte offset differs from the
				 * offset of the column before it. */
				bool tab_head = cursor_x < row->size
				    && row->chars[cursor_x] == '\t'
				    && (render_pos == 0
					|| prev_tab_cursor_x_valid == false
					|| prev_tab_cursor_x != cursor_x);
				prev_tab_cursor_x = cursor_x;
				prev_tab_cursor_x_valid = true;
				if (ec.show_whitespace && tab_head) {
					if (!in_selection)
						buf_append(eb, TAB_HEAD_STYLE,
							   sizeof(TAB_HEAD_STYLE)
							   - 1);
					buf_append(eb, "\xC2\xBB", 2);
					if (!in_selection) {
						if (current_style != NORMAL) {
							char buf[16];
							int c_len =
							    syntax_style_escape
							    (current_style, buf,
							     sizeof(buf));
							buf_append(eb, buf, c_len);
						} else {
							buf_append(eb,
								   "\x1b[39;49m",
								   8);
						}
					}
				} else if (iscntrl(c[j])) {
					char sym =
					    (c[j] <= 26) ? '@' + c[j] : '?';
					buf_append(eb, "\x1b[7m", 4);
					buf_append(eb, &sym, 1);
					buf_append(eb, "\x1b[m", 3);
					if (current_style != NORMAL) {
						char buf[16];
						int c_len = syntax_style_escape(
							current_style, buf,
							sizeof(buf));
						buf_append(eb, buf, c_len);
					}
				} else if (hl[j] == NORMAL) {
					if (current_style != NORMAL) {
						buf_append(eb, "\x1b[39;49m", 8);
						current_style = NORMAL;
					}
					buf_append(eb, &c[j], 1);
				} else {
					if (hl[j] == MATCH) {
						/* Use inverse video for search matches */
						buf_append(eb, "\x1b[7m", 4);
						buf_append(eb, &c[j], 1);
						buf_append(eb, "\x1b[27m", 5);
						if (current_style != NORMAL) {
							char buf[16];
							int c_len = syntax_style_escape(
								current_style, buf,
								sizeof(buf));
							buf_append(eb, buf,
								   c_len);
						}
					} else {
						if (hl[j] != current_style) {
							current_style = hl[j];
							char buf[16];
							int c_len = syntax_style_escape(
								hl[j], buf,
								sizeof(buf));
							buf_append(eb, buf,
								   c_len);
						}
						buf_append(eb, &c[j], 1);
					}
				}
			}
			/* Ensure selection highlighting is turned off at end of line */
			if (in_selection)
				buf_append(eb, "\x1b[27m", 5);
			buf_append(eb, "\x1b[39;49m", 8);
		}
		buf_append(eb, "\x1b[K", 3);
		buf_append(eb, "\r\n", 2);
	}
}

static void editor_get_cursor_pos(int *row, int *col)
{
	*row = (ec.cursor_y - ec.row_offset) + 1;
	*col = (ec.render_x - ec.col_offset) + 1 + get_line_number_width();

	if (ec.mode == MODE_SEARCH && ec.search.replace_phase != 2 &&
	    ec.search.prompt_query_start_col > 0) {
		size_t qlen = ec.search.replace_phase == 1 ?
		    ec.search.replace_len : ec.search.query_len;
		int max_qadd = ec.screen_cols - ec.search.prompt_query_start_col;
		max_qadd = clamp(max_qadd, 0, ec.screen_cols);
		int visible_query_len =
		    qlen > (size_t)max_qadd ? max_qadd : (int)qlen;
		int end_col =
		    ec.search.prompt_query_start_col + visible_query_len;
		*col = ec.search.prefill_from_start ?
		    ec.search.prompt_query_start_col : end_col;
		*col = clamp(*col, 1, ec.screen_cols);
		*row = ec.screen_rows + 1;
	}
}

/* Act on SIGCONT/SIGWINCH flags. The handlers only set flags, so this
 * runs in the main loop for every mode, not only in editor_refresh().
 * Browser, help and search modes skip editor_refresh(), so a resume
 * while in those modes stayed in cooked mode until the next repaint. */
static void editor_handle_signals(void)
{
	if (g_cont_pending) {
		g_cont_pending = 0;
		term_disable_raw();
		term_open_buffer();
		term_enable_raw();
	}
	if (g_resize_pending) {
		g_resize_pending = 0;
		term_update_size();
		ec.cursor_y = clamp(ec.cursor_y, 0, NR > 0 ? NR - 1 : 0);
		if (ec.cursor_y < NR)
			ec.cursor_x = clamp(ec.cursor_x, 0,
					     ROW(ec.cursor_y)->size);
		else
			ec.cursor_x = 0;
	}
}

static void editor_refresh(void)
{
	editor_handle_signals();
	editor_scroll();
	editor_buf_t eb = { NULL, 0, 0 };
	buf_append(&eb, "\x1b[?25l", 6);
	buf_append(&eb, "\x1b[H", 3);
	ui_draw_rows(&eb);
	ui_draw_statusbar(&eb);
	buf_append_overlay(&eb);	/* transient notfound/replaced overlay */

	/* Position terminal cursor in text area, or in status prompt while active. */
	int row, col;
	editor_get_cursor_pos(&row, &col);
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
	buf_append(&eb, buf, strlen(buf));
	buf_append(&eb, "\x1b[?25h", 6);
	term_write_all(eb.buf, (size_t) eb.len);
	buf_destroy(&eb);
}

/* Force full screen refresh by clearing first */
static void editor_refresh_full(void)
{
	editor_scroll();
	editor_buf_t eb = { NULL, 0, 0 };
	buf_append(&eb, "\x1b[?25l", 6);
	buf_append(&eb, "\x1b[2J", 4);	/* Clear entire screen */
	buf_append(&eb, "\x1b[H", 3);
	ui_draw_rows(&eb);
	ui_draw_statusbar(&eb);
	buf_append_overlay(&eb);	/* transient notfound/replaced overlay */

	/* Position terminal cursor in text area, or in status prompt while active. */
	int row, col;
	editor_get_cursor_pos(&row, &col);
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
	buf_append(&eb, buf, strlen(buf));
	buf_append(&eb, "\x1b[?25h", 6);
	term_write_all(eb.buf, (size_t) eb.len);
	buf_destroy(&eb);
}

/* The handler does nothing but set the flag above, because a handler
 * that repaints would malloc, realloc and write from signal context,
 * which is not async-signal-safe: a SIGWINCH that lands inside a malloc
 * on a paste can deadlock on the allocator lock. */
static void sig_winch_handler(int sig)
{
	(void)sig;		/* Unused parameter */
	g_resize_pending = 1;
}

/* Set by the SIGCONT handler, acted on by editor_refresh.  After a
 * Ctrl-Z the shell owns the terminal again, so the editor has to take
 * raw mode back and repaint.  None of that is async-signal-safe:
 * tcsetattr, the malloc in a repaint and the second atexit registration
 * inside term_enable_raw all belong in the main loop, so the handler
 * only sets this flag. */
static void sig_cont_handler(int sig)
{
	(void)sig;		/* Unused parameter */
	g_cont_pending = 1;
}

/* Ask a question with selectable options.
 * Returns selected option index, or -1 on cancel (^C/^X/ESC). */
static int ui_dialog_ask(const char *msg, char *const options[])
{
	int n = 0;
	while (options[n])
		n++;
	if (n <= 0)
		return -1;
	int choice = 0;

	while (1) {
		char status_msg[512];
		int off = snprintf(status_msg, sizeof(status_msg), "%s  ", msg);
		for (int i = 0; i < n; i++) {
			if (off < 0 || off >= (int)sizeof(status_msg))
				break;
			off +=
			    snprintf(status_msg + off,
				     sizeof(status_msg) - (size_t) off,
				     (i ==
				      choice) ? "\x1b[7m[ %s ]\x1b[27;97;100m" :
				     "[ %s ]", options[i]);
			if (i + 1 < n) {
				if (off < 0 || off >= (int)sizeof(status_msg))
					break;
				off +=
				    snprintf(status_msg + off,
					     sizeof(status_msg) - (size_t) off,
					     "  ");
			}
		}
		if (off > -1 && off < (int)sizeof(status_msg))
			snprintf(status_msg + off,
				 sizeof(status_msg) - (size_t) off,
				 "  ^C:Cancel");

		snprintf(ec.status_msg, sizeof(ec.status_msg), "%s", status_msg);
		editor_refresh();

		int c = term_read_key();
		switch (c) {
		case '\r':	/* Enter key */
			ec.status_msg[0] = '\0';
			return choice;
		case CTRL_('c'):	/* ^C - cancel */
		case CTRL_('x'):
		case '\x1b':
			ec.status_msg[0] = '\0';
			return -1;	/* Cancel */
		case ARROW_LEFT:
		case ARROW_UP:
			choice = (choice + n - 1) % n;
			break;
		case ARROW_RIGHT:
		case ARROW_DOWN:
			choice = (choice + 1) % n;
			break;
		case HOME_KEY:
			choice = 0;
			break;
		case END_KEY:
			choice = n - 1;
			break;
		default:
			c = tolower((unsigned char)c);
			for (int i = 0; i < n; i++) {
				int ch = tolower((unsigned char)options[i][0]);
				if (c == ch) {
					ec.status_msg[0] = '\0';
					return i;
				}
			}
			break;
		}
	}
}

/* Returns 1 = Yes, 0 = No, -1 = Cancel (ESC/^X) */
static int ui_confirm(const char *msg)
{
	char *const options[] = { "No", "Yes", NULL };
	int r = ui_dialog_ask(msg, options);
	if (r < 0)
		return -1;
	return r;
}

static char *ui_prompt(const char *prefix, const char *hint,
		       const char *init, void (*callback) (char *, int))
{
	size_t buf_size = 0;
	char *buf = NULL;
	size_t buf_len = 0;
	int trunc_visible_chars = -1;
	const int min_trunc_visible_chars = 2;

	/* Allocate an empty buffer at entry: the display snprintf reads
	 * buf even when init is NULL, and a NULL %s argument is undefined. */
	if (!strbuf_reserve(&buf, &buf_len, &buf_size, 0)) {
		free(buf);
		return NULL;
	}
	buf[0] = '\0';

	if (init) {
		/* Make room for the text that is already there, then copy it
		 * in one go.  Going through the growth helper byte by byte
		 * would reallocate once per character of the file name, and
		 * would read nothing: the helper is not a reader, so the
		 * only cost is the copying. */
		size_t init_len = strlen(init);
		if (!strbuf_reserve(&buf, &buf_len, &buf_size, init_len)) {
			free(buf);
			return NULL;
		}
		memcpy(buf, init, init_len + 1);
		buf_len = init_len;
	}

	while (1) {
		/* Build prompt display: prefix + [<]buf + spaces + hint */
		int pfx_len = (int)strlen(prefix);
		int hlen = hint ? (int)strlen(hint) : 0;
		int blen = (int)buf_len;
		int cols = ec.screen_cols;
		/* Visible chars available for the buffer content;
		 * reserve 1 for the gap space before the hint */
		int max_bvis = cols - pfx_len - hlen - 1;
		if (max_bvis < 0)
			max_bvis = 0;
		int vis_blen;	/* visible buffer chars shown */
		int slen;
		if (blen <= max_bvis) {
			trunc_visible_chars = -1;
			vis_blen = blen;
			slen =
			    snprintf(ec.status_msg, sizeof(ec.status_msg),
				     "%s%s", prefix, buf);
		} else {
			/* Tail-truncate: show '<' + tail of buffer */
			if (max_bvis <= 0)
				trunc_visible_chars = 0;
			/* Initialize or clamp truncation window to current width */
			else if (trunc_visible_chars < 0 || trunc_visible_chars > max_bvis)
				trunc_visible_chars = max_bvis;
			vis_blen = trunc_visible_chars;
			int tail = vis_blen > 0 ? vis_blen - 1 : 0;
			if (vis_blen > 0)
				slen =
				    snprintf(ec.status_msg, sizeof(ec.status_msg),
					     "%s<%s", prefix, buf + blen - tail);
			else
				slen =
				    snprintf(ec.status_msg, sizeof(ec.status_msg),
					     "%s", prefix);
		}
		/* Pad then right-align hint (always shown) */
		int spaces = cols - pfx_len - vis_blen - hlen;
		spaces = clamp(spaces, 0, cols);
		if (slen + spaces + hlen < (int)sizeof(ec.status_msg)) {
			memset(ec.status_msg + slen, ' ', spaces);
			slen += spaces;
			if (hint) {
				memcpy(ec.status_msg + slen, hint, hlen);
				slen += hlen;
			}
			ec.status_msg[slen] = '\0';
		}
		editor_refresh();
		/* Move terminal cursor to end of displayed input in status bar */
		char posbuf[32];
		int n =
		    snprintf(posbuf, sizeof(posbuf), "\x1b[%d;%dH",
			     ec.screen_rows + 1, pfx_len + vis_blen + 1);
		write(STDOUT_FILENO, posbuf, n);

		int c = term_read_key();
		if ((c == DEL_KEY) || (c == CTRL_('h')) || (c == BACKSPACE)) {
			if (buf_len != 0) {
				buf[--buf_len] = '\0';
				if (blen > max_bvis &&
				    max_bvis >= min_trunc_visible_chars) {
					if (trunc_visible_chars > min_trunc_visible_chars)
						trunc_visible_chars--;
					else
						trunc_visible_chars = max_bvis;
				}
			}
		} else if (c == CTRL_('c') || c == CTRL_('x') || c == '\x1b') {
			ec.status_msg[0] = '\0';
			if (callback)
				callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buf_len != 0) {
				ec.status_msg[0] = '\0';
				if (callback)
					callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && isprint(c)) {
			/* A prompt that cannot grow ends here.  The text typed so
			 * far is returned, not dropped: the caller asked for it. */
			if (!strbuf_grow(&buf, &buf_len, &buf_size, c)) {
				ec.status_msg[0] = '\0';
				if (callback)
					callback(buf, c);
				return buf;
			}
			if ((int)buf_len > max_bvis && max_bvis > 0)
				trunc_visible_chars = max_bvis;
		}
		if (callback)
			callback(buf, c);
	}
}

static void editor_move_cursor(int key)
{
	/* A move inside a burst means the burst is no longer a run of
	 * insertions at one spot, so it must not become one undo step. */
	burst_break();
	editor_row_t *row = (ec.cursor_y >= NR) ? NULL : ROW(ec.cursor_y);
	switch (key) {
	case ARROW_LEFT:
		if (ec.cursor_x != 0) {
			/* Move to previous UTF-8 character boundary */
			if (row) {
				const char *prev =
				    utf8_prev_char(row->chars,
						   row->chars + ec.cursor_x);
				ec.cursor_x = prev - row->chars;
			} else {
				ec.cursor_x--;
			}
		} else if (ec.cursor_y > 0) {
			/* The row above exists whenever the cursor is in
			 * range, but read it once so a cursor left out of
			 * range cannot dereference NULL. */
			ec.cursor_y--;
			editor_row_t *prev = ROW(ec.cursor_y);
			ec.cursor_x = prev ? prev->size : 0;
		}
		break;
	case ARROW_RIGHT:
		if (row && ec.cursor_x < row->size) {
			/* Move to next UTF-8 character boundary */
			const char *next =
			    utf8_next_char(row->chars + ec.cursor_x);
			ec.cursor_x = next - row->chars;
			if (ec.cursor_x > row->size)
				ec.cursor_x = row->size;
		} else if (row && ec.cursor_x == row->size) {
			ec.cursor_y++;
			ec.cursor_x = 0;
		}
		break;
	case ARROW_UP:
		if (ec.cursor_y != 0)
			ec.cursor_y--;
		break;
	case ARROW_DOWN:
		if (ec.cursor_y < NR)
			ec.cursor_y++;
		break;
	}
	row = (ec.cursor_y >= NR) ? NULL : ROW(ec.cursor_y);
	int row_len = row ? row->size : 0;
	if (ec.cursor_x > row_len)
		ec.cursor_x = row_len;
}

/* A word character: letter, digit, or underscore. */
static bool is_word_char(unsigned char c)
{
	return isalnum(c) || c == '_';
}

/* Jump one word forward (dir > 0) or backward (dir < 0).  A word is
 * letters, digits, underscore.  Forward always ends on the first
 * character of the next word; backward on the first character of the
 * previous one - symbols between words are crossed, never landed on.
 * Stops at line edges; does not cross rows, like nano's Ctrl+arrows. */
static void editor_move_word(int dir)
{
	burst_break();
	editor_row_t *row = (ec.cursor_y >= NR) ? NULL : ROW(ec.cursor_y);
	if (!row)
		return;
	if (dir > 0) {
		/* Leave the current word, then cross to the next. */
		while (ec.cursor_x < row->size &&
		       is_word_char((unsigned char) row->chars[ec.cursor_x]))
			ec.cursor_x++;
		while (ec.cursor_x < row->size &&
		       !is_word_char((unsigned char) row->chars[ec.cursor_x]))
			ec.cursor_x++;
	} else {
		/* Cross the gap, then walk the previous word down to
		 * its first character.  Starting inside a word skips
		 * the first loop and lands on that word's start. */
		while (ec.cursor_x > 0 && !is_word_char((unsigned char)
							row->chars[ec.cursor_x -
								       1]))
			ec.cursor_x--;
		while (ec.cursor_x > 0 &&
		       is_word_char((unsigned char) row->chars[ec.cursor_x - 1]))
			ec.cursor_x--;
	}
}

static void editor_goto_matching_bracket(void)
{
	burst_break();
	static const char brackets[] = "{}()[]";
	int row = ec.cursor_y;
	int col = ec.cursor_x;
	int idx = -1;
	int dir;
	int depth = 0;

	/* An empty buffer has no row to read, and the cursor can also sit
	 * one past the last byte of a row.  Read the row once and refuse a
	 * position that is not on a byte of it. */
	editor_row_t *cur = (row >= 0 && row < NR) ? ROW(row) : NULL;
	if (!cur || col < 0 || col >= cur->size) {
		set_overlay_msg("No matching bracket");
		return;
	}

	/* could do this in one line with strchr but we don't use it elsewhere
	   so pulling it in for this one case would make the binary bigger. */
	for (int i = 0; i < (int)sizeof(brackets) - 1; i++) {
		if (cur->chars[col] == brackets[i]) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		goto not_found;

	dir = (idx & 1) ? -1 : 1;

	for (int y = row; y < NR && y >= 0; y += dir) {
		editor_row_t *r = ROW(y);
		int x0 = (y == row) ? col + dir : (y > 0 ? 0 : r->size - 1);
		for (int x = x0; x < r->size && x >= 0; x += dir) {
			char c = r->chars[x];
			if (c == brackets[idx]) {
				depth++;
			} else if (c == brackets[idx+dir]) {
				if (depth == 0) {
					ec.cursor_y = y;
					ec.cursor_x = x;
					return;
				}
				depth--;
			}
		}
	}

 not_found:
	set_overlay_msg("No matching bracket");
}

/* File browser implementation */

/* Get file extension */
static const char *get_file_extension(const char *filename)
{
	const char *dot = strrchr(filename, '.');
	if (!dot || dot == filename)
		return "";
	return dot + 1;
}

/* Get file type indicator and color */
static const char *get_file_type_info(const char *filename, bool is_dir, int *color)
{
	static const char source_exts[][5] = {
		"c", "h", "cpp", "cxx", "hpp", "cc", "sh", "py", "rb",
		"js", "rs", "go", "java", "php", "pl", "lua", "vim", "asm",
		"s"
	};

	if (is_dir) {
		*color = 34;	/* Blue for directories */
		return "[DIR]  ";
	}

	const char *ext = get_file_extension(filename);

	for (size_t i = 0; i < sizeof(source_exts) / sizeof(source_exts[0]); i++) {
		if (!strcasecmp(ext, source_exts[i])) {
			*color = 32;	/* Green for source */
			return "[SRC]  ";
		}
	}

	/* All other files */
	*color = 37;		/* White for others */
	return "[FILE] ";
}

static void browser_free_entries(void)
{
	if (ec.mode_owners.browser_entries) {
		tlist_free(ec.mode_owners.browser_entries);
		ec.mode_owners.browser_entries = NULL;
	}
	/* current_dir is a union member, so it is only the browser's to
	 * clear while the browser mode is live. */
	if (ec.mode == MODE_BROWSER)
		ec.mode_state.browser.current_dir[0] = '\0';
}

static int browser_entry_cmp(const void *ap, const void *bp)
{
	const browser_entry_t *a = (const browser_entry_t *)ap;
	const browser_entry_t *b = (const browser_entry_t *)bp;
	if (a->is_dir && !b->is_dir)
		return -1;
	if (!a->is_dir && b->is_dir)
		return 1;
	return strcasecmp(a->name, b->name);
}

static bool path_join(char *dst, size_t dstsz, const char *base, const char *name)
{
	size_t blen = strlen(base), nlen = strlen(name);
	bool need_slash = (blen == 0 || base[blen - 1] != '/');
	size_t need = blen + (need_slash ? 1 : 0) + nlen + 1;
	if (need > dstsz)
		return false;
	memcpy(dst, base, blen);
	size_t off = blen;
	if (need_slash)
		dst[off++] = '/';
	memcpy(dst + off, name, nlen);
	dst[off + nlen] = '\0';
	return true;
}

static void browser_load_directory(const char *path)
{
	const char *dir_path = path ? path : ".";
	browser_free_entries();

	DIR *dir = opendir(dir_path);
	if (!dir) {
		ui_set_message("Cannot open directory: %s", strerror(errno));
		mode_set(MODE_NORMAL);
		return;
	}

	snprintf(ec.mode_state.browser.current_dir,
		 sizeof(ec.mode_state.browser.current_dir), "%s", dir_path);
	ec.mode_owners.browser_entries = tlist_new(sizeof(browser_entry_t));
	if (!ec.mode_owners.browser_entries) {
		closedir(dir);
		mode_set(MODE_NORMAL);
		ui_set_message("Out of memory");
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, "."))
			continue;

		if (!ec.mode_state.browser.show_hidden && de->d_name[0] == '.'
		    && strcmp(de->d_name, ".."))
			continue;

		char full_path[PATH_MAX];
		if (!path_join(full_path, sizeof(full_path),
			       ec.mode_state.browser.current_dir, de->d_name))
			continue;

		struct stat st;
		if (stat(full_path, &st) == 0) {
			if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
				continue;
			browser_entry_t e;
			snprintf(e.name, sizeof(e.name), "%s", de->d_name);
			e.is_dir = S_ISDIR(st.st_mode);
			if (!tlist_insert_sorted(ec.mode_owners.browser_entries, &e,
						 browser_entry_cmp))
				break;
		}
	}

	closedir(dir);
	ec.mode_state.browser.selected = 0;
	ec.mode_state.browser.offset = 0;
}

/* Shared rendering for help and browser screens */
static void list_screen_render(const char *title, int total_lines, int offset,
			       int selected, const char *const *lines,
			       tlist *entries, const char *status_left,
			       const char *status_right)
{
	editor_buf_t eb = { NULL, 0, 0 };
	int visible = ec.screen_rows - 1;

	buf_append(&eb, "\x1b[?25l\x1b[H", 9);

	/* Title bar with full-width inverse video */
	buf_append(&eb, "\x1b[7m", 4);
	int tlen = (int)strlen(title);
	if (tlen > ec.screen_cols)
		tlen = ec.screen_cols;
	buf_append(&eb, title, tlen);
	while (tlen < ec.screen_cols) {
		buf_append(&eb, " ", 1);
		tlen++;
	}
	buf_append(&eb, "\x1b[0m\r\n", 6);

	/* Content lines */
	for (int i = 0; i < visible; i++) {
		int idx = offset + i;
		buf_append(&eb, "\r\x1b[K", 4);
		if (idx < total_lines) {
			if (entries) {
				/* Browser mode: format entry with icon and color */
				browser_entry_t *entry =
				    (browser_entry_t *) tlist_get(entries, (size_t) idx);
				if (!entry)
					continue;
				int color;
				const char *type_str =
				    get_file_type_info(entry->name, entry->is_dir, &color);
				if (idx == selected)
					buf_append(&eb, "\x1b[7m", 4);
				char line[512];
				int llen =
				    snprintf(line, sizeof(line),
					     "\x1b[%dm  %s%s\x1b[0m",
					     color, type_str, entry->name);
				if (llen >= (int)sizeof(line))
					llen = sizeof(line) - 1;
				if (llen > ec.screen_cols)
					llen = ec.screen_cols;
				buf_append(&eb, line, llen);
			} else {
				/* Help mode: plain text */
				const char *line =
				    (lines && lines[idx]) ? lines[idx] : "";
				int hlen = (int)strlen(line);
				if (hlen > ec.screen_cols)
					hlen = ec.screen_cols;
				buf_append(&eb, line, hlen);
			}
		} else {
			buf_append(&eb, "~", 1);
		}
		if (i < visible - 1)
			buf_append(&eb, "\r\n", 2);
	}

	/* Status bar */
	buf_append(&eb, "\r\n\x1b[93;44m", 10);
	int len = (int)strlen(status_left);
	if (len > ec.screen_cols)
		len = ec.screen_cols;
	buf_append(&eb, status_left, len);
	if (status_right) {
		int r_len = (int)strlen(status_right);
		while (len < ec.screen_cols) {
			if (ec.screen_cols - len == r_len) {
				buf_append(&eb, status_right, r_len);
				break;
			}
			buf_append(&eb, " ", 1);
			len++;
		}
	} else {
		while (len < ec.screen_cols) {
			buf_append(&eb, " ", 1);
			len++;
		}
	}
	buf_append(&eb, "\x1b[m", 3);
	buf_append_overlay(&eb);
	term_write_all(eb.buf, (size_t) eb.len);
	buf_destroy(&eb);
}

static void browser_open_selected(void)
{
	int count = (int)tlist_getsize(ec.mode_owners.browser_entries);
	if (ec.mode_state.browser.selected >= count)
		return;
	browser_entry_t *entry =
	    (browser_entry_t *) tlist_get(ec.mode_owners.browser_entries,
					  (size_t) ec.mode_state.browser.selected);
	if (!entry)
		return;

	if (entry->is_dir) {
		/* Directory */
		char new_path[PATH_MAX];
		if (!path_join(new_path, sizeof(new_path),
			       ec.mode_state.browser.current_dir, entry->name)) {
			ui_set_message("Path too long");
			return;
		}
		browser_load_directory(new_path);
	} else {
		/* File - open it */
		char full_path[PATH_MAX];
		if (!path_join(full_path, sizeof(full_path),
			       ec.mode_state.browser.current_dir, entry->name)) {
			ui_set_message("Path too long");
			return;
		}
		if (ec.modified) {
			int r =
			    ui_confirm
			    ("Current file has been modified. Save it?");
			if (r == -1)
				return;
			if (r == 1)
				if(!file_save()) {
					editor_refresh_full();
					return;
				}
		}
		if(!file_open(full_path)) {
			ui_set_message("Failed to open file: %s", strerror(errno));
			editor_refresh_full();	/* Fully redraw the editor */
			return;
		}

		browser_free_entries();
		mode_set(MODE_NORMAL);
		ui_set_message("Opened: %s%s", full_path, ec.read_only? " [r/o]" : "");
		editor_refresh_full();	/* Fully redraw the editor */
	}
}

static void help_render(void)
{
	int visible = ec.screen_rows - 1;
	int first_visible = ec.mode_state.help.offset + 1;
	int last_visible = ec.mode_state.help.offset + visible;
	if (last_visible > HELP_NUM_LINES)
		last_visible = HELP_NUM_LINES;
	char status[80];
	snprintf(status, sizeof(status), " [HELP] lines %d-%d of %d",
		 first_visible, last_visible, HELP_NUM_LINES);
	list_screen_render("  Help", HELP_NUM_LINES, ec.mode_state.help.offset,
			   -1, help_lines, NULL, status, NULL);
}

static void browser_render(void)
{
	/* Adjust offset to keep selected item visible */
	int count = (int)tlist_getsize(ec.mode_owners.browser_entries);
	int visible = ec.screen_rows - 1;
	if (ec.mode_state.browser.selected < ec.mode_state.browser.offset)
		ec.mode_state.browser.offset = ec.mode_state.browser.selected;
	if (ec.mode_state.browser.selected >=
	    ec.mode_state.browser.offset + visible)
		ec.mode_state.browser.offset =
		    ec.mode_state.browser.selected - visible + 1;

	char title[256], status_right[80];
	snprintf(title, sizeof(title), " [BROWSER] %.240s",
		 ec.mode_state.browser.current_dir);
	if (count > 0) {
		snprintf(status_right, sizeof(status_right), "%d/%d files",
			 ec.mode_state.browser.selected + 1, count);
	} else {
		snprintf(status_right, sizeof(status_right), "0/0 files");
	}
	list_screen_render(title, count,
			   ec.mode_state.browser.offset,
			   ec.mode_state.browser.selected, NULL,
			   ec.mode_owners.browser_entries, title, status_right);
}

/* Clean up all allocated memory before exit */
static void editor_cleanup(void)
{
	undo_history_clear();
	for (int i = 0; i < NR; i++)
		row_free_contents(ROW(i));
	tlist_free(ec.rows);
	ec.rows = NULL;

	/* Free the open burst buffer */
	free(g_burst.buf);
	g_burst.buf = NULL;
	g_burst.len = g_burst.cap = 0;

	/* Free file name */
	free(ec.file_name);
	ec.file_name = NULL;

	/* Free copied buffer */
	free(ec.copied_char_buffer);
	ec.copied_char_buffer = NULL;

	/* Free mode state */
	free(ec.search.query);
	ec.search.query = NULL;
	free(ec.search.replace_query);
	ec.search.replace_query = NULL;
	/* The saved search highlight and the browser list are owned by
	 * mode_owners, not by whichever mode happens to be live, so
	 * cleanup can release them whatever the mode.  No mode owns a
	 * pointer in the union, so nothing else needs a free here. */
	free(ec.mode_owners.search_saved_highlight);
	ec.mode_owners.search_saved_highlight = NULL;
	ec.mode_owners.search_highlight_line = -1;
	ec.mode_owners.search_saved_highlight_size = 0;
	syntax_reset_compiled_rules();
	browser_free_entries();
}

static void editor_process_key(void)
{
	/* Clear the transient overlay message from the previous keypress */
	ec.overlay_msg[0] = '\0';
	if (ec.mode == MODE_SEARCH && ec.search.replace_phase == 2) {
		replace_confirm_step();
		editor_refresh();
		return;
	}
	/* Save and clear the consecutive-cut flag before reading the new key.
	 * editor_cut() will check this to decide whether to append or replace. */
	bool consecutive_cut = ec.last_was_cut;
	ec.last_was_cut = false;
	int c = term_read_key();

	/* A control byte that follows inserted text in the same burst is
	 * pasted text, not a key the user pressed.  A terminal paste
	 * carries whatever was in the clipboard, and a control byte in it
	 * would otherwise run a command: ^K cut a line, ^W opened the
	 * search prompt, ^X asked to quit and pulled the rest of the
	 * paste into the dialog, where the editor waited for a key that
	 * was never going to arrive.
	 *
	 * The first byte of a burst is still a key.  The loop opens a
	 * burst whenever input is already queued, so the byte of a lone
	 * keypress can be queued too, and that key must act.  Only once
	 * the burst has inserted something does it read as a run of text
	 * arriving at once.  The bytes a paste may carry are LF, CR, tab
	 * and everything printable. */
	if (g_burst.open && g_burst.len > 0 && c < 0x20 &&
	    c != '\n' && c != '\r' && c != '\t') {
		/* Pasted text and a fast typist are the same on the wire: the
		 * burst only knows the tty had bytes queued.  A control byte
		 * in that stream is dropped, because a dialog waiting for
		 * its own key would otherwise swallow the rest of the paste
		 * and hang.  The first byte of a burst is still a key, so
		 * ^K, ^O, ^X and the rest keep working when they are typed
		 * on their own. */
		burst_break();
		return;
	}

	/* Handle mode-specific keys first */
	switch (ec.mode) {
	case MODE_BROWSER:
		/* File browser mode */
		switch (c) {
		case CTRL_('c'):	/* ^C - cancel */
		case CTRL_('x'):
			browser_free_entries();
			mode_set(MODE_NORMAL);
			editor_refresh_full();	/* Full redraw the editor */
			return;
		case '\r':	/* Enter - open file/directory */
			browser_open_selected();
			if (ec.mode == MODE_BROWSER)
				browser_render();
			return;
		case ARROW_UP:
			if (ec.mode_state.browser.selected > 0) {
				ec.mode_state.browser.selected--;
			}
			browser_render();
			return;
		case ARROW_DOWN:
		{
			int count = (int)tlist_getsize(ec.mode_owners.browser_entries);
			if (ec.mode_state.browser.selected <
			    count - 1) {
				ec.mode_state.browser.selected++;
			}
			browser_render();
			return;
		}
		case PAGE_UP:
			ec.mode_state.browser.selected -= ec.screen_rows - 3;
			if (ec.mode_state.browser.selected < 0)
				ec.mode_state.browser.selected = 0;
			browser_render();
			return;
		case PAGE_DOWN:
		{
			int count = (int)tlist_getsize(ec.mode_owners.browser_entries);
			ec.mode_state.browser.selected += ec.screen_rows - 3;
			if (ec.mode_state.browser.selected >= count)
				ec.mode_state.browser.selected = count - 1;
			if (ec.mode_state.browser.selected < 0)
				ec.mode_state.browser.selected = 0;
			browser_render();
			return;
		}
		case HOME_KEY:
			ec.mode_state.browser.selected = 0;
			browser_render();
			return;
		case END_KEY:
		{
			int count = (int)tlist_getsize(ec.mode_owners.browser_entries);
			ec.mode_state.browser.selected = count > 0 ? count - 1 : 0;
			browser_render();
			return;
		}
		case 'h':
		case 'H':
			/* Toggle hidden files */
			ec.mode_state.browser.show_hidden =
			    !ec.mode_state.browser.show_hidden;
			browser_load_directory(ec.mode_state.browser.
					       current_dir);
			browser_render();
			return;
		default:
			/* Ignore other keys */
			return;
		}
		break;

	case MODE_SELECT:
		/* Block indent keys work regardless of how the selection
		 * started; handle them before the navigation gates below,
		 * whose flag tests would otherwise swallow them. */
		if (c == '\t') {
			selection_indent(1, "\t");
			ui_set_message("Block indented");
			return;
		}
		if (c == SHIFT_TAB) {
			selection_indent(-1, "\t");
			ui_set_message("Block unindented");
			return;
		}
		if (c == ' ') {
			selection_indent(1, " ");
			ui_set_message("Block indented");
			return;
		}
		if (c == SHIFT_SPACE || c == META_(' ')) {
			/* Plain VT has no code for Shift+Space; M-Space
			 * is the alias every terminal can send. */
			selection_indent(-1, " ");
			ui_set_message("Block unindented");
			return;
		}
		if (ec.mode_state.select.shift_only) {
			if (key_is_shift_select_nav(c)) {
				selection_move_cursor(c);
				return;
			}
		} else if (key_is_select_nav(c)) {
			selection_move_cursor(c);
			return;
		}
		switch (c) {
		case '\x1b':		/* ignore lone ESC - probably part of a meta seq */
			break;
		case CTRL_('c'):	/* ^C - abort selection */
			ec.selection.active = false;
			mode_set(MODE_NORMAL);
			ui_set_message("Mark cancelled");
			return;
		case META_('6'):	/* Copy marked text and exit marking */
			selection_copy();
			mode_set(MODE_NORMAL);
			ui_set_message("Copied marked text");
			return;
		case CTRL_('k'):	/* Cut marked text and exit marking */
			selection_cut();
			mode_set(MODE_NORMAL);
			ui_set_message("Cut marked text");
			return;
		case CTRL_('u'):	/* Paste over selection */
			selection_delete();
			editor_paste();
			mode_set(MODE_NORMAL);
			return;
		case DEL_KEY:
		case BACKSPACE:
		/* CTRL-h is not only the nano kb for BACKSPACE, it also evaluates to
		   \b (8) which is the actual char sent when using backspace */
		case CTRL_('h'):
			selection_delete();
			return;
		case META_('e'):	/* Redo likewise. */
		case META_('E'):
			mode_set(MODE_NORMAL);
			ec.selection.active = false;
			undo_perform(true);
			return;
		case META_('u'):
		case META_('U'):
		case CTRL_('z'):
			mode_set(MODE_NORMAL);
			ec.selection.active = false;
			undo_perform(false);
			return;
		default:
			/* Exit selection mode for other keys */
		{
			bool shift_only = ec.mode_state.select.shift_only;
			mode_set(MODE_NORMAL);
			if (shift_only)
				return;	/* transient Shift-select consumes first non-navigation key */
			break;
		}
		}
		break;

	case MODE_HELP:{
			int visible = ec.screen_rows - 1;
			int max_offset = HELP_NUM_LINES - visible;
			if (max_offset < 0)
				max_offset = 0;
			switch (c) {
			case CTRL_('g'):
			case CTRL_('x'):
			case CTRL_('c'):
				mode_restore();
				editor_refresh_full();
				return;
			case ARROW_UP:
				if (ec.mode_state.help.offset > 0)
					ec.mode_state.help.offset--;
				break;
			case ARROW_DOWN:
				if (ec.mode_state.help.offset < max_offset)
					ec.mode_state.help.offset++;
				break;
			case PAGE_UP:
				ec.mode_state.help.offset =
				    clamp(ec.mode_state.help.offset - visible, 0,
					  max_offset);
				break;
			case PAGE_DOWN:
				ec.mode_state.help.offset =
				    clamp(ec.mode_state.help.offset + visible, 0,
					  max_offset);
				break;
			default:
				break;	/* ignore other keys */
			}
			help_render();
			return;
		}

	case MODE_SEARCH:{
			/* Phase 1: entering replacement text */
			if (ec.search.replace_phase == 1) {
				if (c == '\r') {
					/* Execute: find first match at or after the saved position (wrapping).
					 * Use search_do_from directly with saved_x so we include any match
					 * sitting exactly at the cursor rather than starting at saved_x+1. */
					int sx = ec.mode_state.search.saved_x;
					int sy = ec.mode_state.search.saved_y;
					bool backwards =
					    (ec.search.mode & SM_BACKWARDS) !=
					    0;
					bool found =
					    search_do_from(ec.search.query, sy,
							   backwards ? sx -
							   1 : sx, false, NULL);
					if (!found) {
						ec.search.replace_phase = 0;
						ec.search.replace_count = 0;
						const char *q =
						    ec.search.query ? ec.search.
						    query : "";
						mode_set(MODE_NORMAL);
						set_overlay_msg
						    ("\"%.*s\" not found",
						     (int)(sizeof
							   (ec.overlay_msg) -
							   20), q);
					} else {
						ec.search.replace_phase = 2;
						/* Store original position so replace_and_advance stops after one cycle. */
						ec.search.orig_row = sy;
						ec.search.orig_char = sx;
						ec.search.has_wrapped = false;
					}
					editor_refresh();
					return;
				} else if (c == CTRL_('c') || c == CTRL_('x')
					   || c == CTRL_('w')) {
					ec.search.replace_phase = 0;
					ec.search.replace_count = 0;
					ec.cursor_x =
					    ec.mode_state.search.saved_x;
					ec.cursor_y =
					    ec.mode_state.search.saved_y;
					ec.col_offset =
					    ec.mode_state.search.saved_col;
					ec.row_offset =
					    ec.mode_state.search.saved_row;
					mode_set(MODE_NORMAL);
					editor_refresh();
					return;
				} else if (c == BACKSPACE || c == DEL_KEY
					   || c == CTRL_('h')) {
					if (ec.search.replace_len > 0) {
						ec.search.replace_query[--ec.
									search.
									replace_len]
						    = '\0';
					}
				} else if (c == '\t' || (c < 0x100 && isprint(c))) {
					/* Append to the replacement text.  The buffer is made
					 * on the first character, so a replace that was given
					 * no input leaves no allocation.  A replacement that
					 * cannot grow keeps the text so far, as the old
					 * grow-then-append pair did. */
					strbuf_grow(&ec.search.replace_query,
						    &ec.search.replace_len,
						    &ec.search.replace_cap, c);
				}
				editor_refresh();
				return;
			}

			/* Phase 0: entering search term (default) */
			if (c == '\r') {
				if (ec.search.query && ec.search.query_len > 0) {
					if (ec.search.mode & SM_REPLACE) {
						/* Switch to replacement text entry */
						ec.search.replace_phase = 1;
						ec.search.replace_len = 0;
						if (ec.search.replace_query)
							ec.search.
							    replace_query[0] =
							    '\0';
					} else {
						/* Plain search */
						if (!search_do(ec.search.query)) {
							const char *q =
							    ec.search.query;
							mode_set(MODE_NORMAL);
							set_overlay_msg
							    ("\"%.*s\" not found",
							     (int)(sizeof
								   (ec.
								    overlay_msg)
								   - 20), q);
						} else {
							mode_set(MODE_NORMAL);
						}
					}
				} else {
					mode_set(MODE_NORMAL);
				}
			} else if (c == CTRL_('c') || c == CTRL_('x')
				   || c == CTRL_('w')) {
				/* Cancel: restore cursor */
				ec.cursor_x = ec.mode_state.search.saved_x;
				ec.cursor_y = ec.mode_state.search.saved_y;
				ec.col_offset = ec.mode_state.search.saved_col;
				ec.row_offset = ec.mode_state.search.saved_row;
				mode_set(MODE_NORMAL);
			} else if (c == END_KEY) {
				/* End switches from replace-on-type to append mode for the prefilled query. */
				ec.search.prefill_from_start = false;
			} else if (c == META_('c') || c == META_('C')) {
				ec.search.mode ^= SM_CASE_SENSITIVE;
			} else if (c == META_('b') || c == META_('B')) {
				ec.search.mode ^= SM_BACKWARDS;
			} else if (c == META_('r') || c == META_('R')) {
				ec.search.mode ^= SM_REGEX;
			} else if (c == CTRL_('r')) {
				ec.search.mode ^= SM_REPLACE;
			} else if (c == BACKSPACE || c == DEL_KEY
				   || c == CTRL_('h')) {
				if (ec.search.prefill_from_start) {
					/* Treat prefilled query as selected at BOL: first edit clears it. */
					search_clear_prefilled_query();
				} else if (ec.search.query_len > 0)
					ec.search.query[--ec.search.query_len] =
					    '\0';
			} else if (c == '\t' || (c < 0x100 && isprint(c))) {
				if (ec.search.prefill_from_start)
					search_clear_prefilled_query();
				/* Append character to query.  The buffer is made here
				 * rather than when the mode opens, so an empty search
				 * allocates nothing.  A query that cannot grow loses
				 * this character and keeps the rest, as the old
				 * grow-then-append pair did. */
				strbuf_grow(&ec.search.query, &ec.search.query_len,
					    &ec.search.query_cap, c);
			}
			editor_refresh();
			return;
		}

	case MODE_CONFIRM:
		/* The confirm dialog handles its own input */
		return;

	case MODE_NORMAL:
	default:
		break;
	}

	/* Normal mode key handling */
	switch (c) {
	case '\r':
	case '\n':		/* A paste delivers LF, not CR */
		editor_newline();
		break;
	case CTRL_('x'):	/* Exit editor (GNU nano: ^X) */
		if (ec.modified) {
			int r =
			    ui_confirm
			    ("Save modified buffer? (\"No\" will DISCARD changes)");
			if (r == -1)
				return;	/* Cancel: stay in editor */
			if (r == 1)
				if(!file_save()) return;	/* Yes: save then quit */
			/* No (r == 0): discard changes and quit */
		}
		term_clear();
		term_close_buffer();
		editor_cleanup();
		exit(0);
		break;
	case CTRL_('o'):	/* Save file (GNU nano: ^O Write Out) */
		file_save();
		break;
	case CTRL_('z'):
	case META_('u'):
	case META_('U'):
		undo_perform(false);
		break;
	case CTRL_('y'):
	case META_('e'):
	case META_('E'):
		undo_perform(true);
		break;
	case META_('a'):
	case META_('A'):	/* Start text marking (GNU nano: M-A Set Mark) */
		if (ec.mode != MODE_SELECT) {
			mode_set(MODE_SELECT);
			ec.mode_state.select.shift_only = false;
			ui_set_message
			    ("Mark set - Move cursor to select, M-6=Copy, ^K=Cut, "
			     "^C=Cancel");
		}
		break;
	case SHIFT_ARROW_UP:
	case SHIFT_ARROW_DOWN:
	case SHIFT_ARROW_LEFT:
	case SHIFT_ARROW_RIGHT:
	case SHIFT_HOME_KEY:
	case SHIFT_END_KEY:
	case SHIFT_PAGE_UP:
	case SHIFT_PAGE_DOWN:
		mode_set(MODE_SELECT);
		ec.mode_state.select.shift_only = true;
		selection_move_cursor(c);
		break;
	case META_('6'):	/* Copy current line (GNU nano: M-6 Copy) */
		if (ec.cursor_y < NR)
			editor_copy(0);
		break;
	case CTRL_('k'):	/* Cut current line (GNU nano: ^K Cut Line) */
		editor_cut(consecutive_cut);
		ec.last_was_cut = true;
		break;
	case CTRL_('u'):	/* Paste/uncut (GNU nano: ^U Uncut) */
		editor_paste();
		break;
	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editor_move_cursor(c);
		break;
	case CTRL_ARROW_LEFT:
		editor_move_word(-1);
		break;
	case CTRL_ARROW_RIGHT:
		editor_move_word(1);
		break;
	case PAGE_UP:
	case PAGE_DOWN:
		page_move_cursor(c);
		break;
	case HOME_KEY:
		burst_break();
		ec.cursor_x = 0;
		break;
	case END_KEY:
		burst_break();
		if (ec.cursor_y < NR)
			ec.cursor_x = ROW(ec.cursor_y)->size;
		break;
	case CTRL_('w'):	/* Find/search (GNU nano: ^W Where Is) */
		search_find();
		return;		/* editor_refresh() already called inside search_find */
	case META_('#'):	/* Toggle line numbers */
		ec.show_line_numbers = !ec.show_line_numbers;
		ui_set_message("Line numbers %s",
			       ec.show_line_numbers ? "enabled" : "disabled");
		break;
	case META_('p'):
	case META_('P'):	/* Toggle whitespace display */
		ec.show_whitespace = !ec.show_whitespace;
		ui_set_message("Whitespace display %s",
			       ec.show_whitespace ? "enabled" : "disabled");
		break;
	case META_('y'):
	case META_('Y'):	/* Toggle syntax highlighting display */
		if (ec.syntax) {
			syntax_disable(true);
		} else {
			syntax_select();
			if (ec.syntax && ec.file_size_bytes > SYNTAX_AUTO_DISABLE_THRESHOLD) {
				syntax_disable(false);
				ui_set_message
				    ("Syntax highlighting auto-disabled above %u MiB",
				     (unsigned) (SYNTAX_AUTO_DISABLE_THRESHOLD >> 20));
			} else {
				ui_set_message("Syntax highlighting %s",
					       ec.syntax ? "enabled" :
					       "not available");
			}
		}
		break;
	case META_('b'):
	case META_('B'):	/* Open file browser (M-B) */
		mode_set(MODE_BROWSER);
		browser_load_directory(ec.browser_base_dir);
		ui_set_message("File Browser: Enter to open, ^C to cancel");
		browser_render();
		return;		/* Don't continue to normal refresh */
	case META_('\\'):	/* M-\ Go to first line (GNU nano: M-\) */
		ec.cursor_y = 0;
		ec.cursor_x = 0;
		break;
	case META_('/'):	/* M-/ Go to last line (GNU nano: M-/) */
		if (NR > 0) {
			ec.cursor_y = NR - 1;
			ec.cursor_x = 0;
		}
		break;
	case META_('g'):	/* M-G Go to line number */
	case META_('G'):{
			char *s = ui_prompt("Enter line number: ", "^C: cancel", NULL, NULL);
			if (s) {
				char *endp;
				long lnum = strtol(s, &endp, 10);
				if (endp > s && *endp == '\0') {
					if (lnum < 1) lnum = 1;
					/* Clamp to the last row, or to row 0 on
					 * an empty buffer: a cursor past the end
					 * of the file makes the next cursor move
					 * dereference a row that is not there. */
					if (NR > 0) {
						if (lnum > NR) lnum = NR;
						ec.cursor_y = (int)lnum - 1;
					} else {
						ec.cursor_y = 0;
					}
					ec.cursor_x = 0;
				} else {
					set_overlay_msg
					    ("[ Invalid line or column number ]");
				}
				free(s);
			}
			break;
		}
	case META_(']'):	/* M-] Go to matching bracket */
		editor_goto_matching_bracket();
		break;
	case CTRL_('g'):	/* Show help (GNU nano: ^G) */
		mode_set(MODE_HELP);
		help_render();
		return;		/* Don't continue to normal refresh */
	case BACKSPACE:
	case CTRL_('h'):
	case DEL_KEY:
		if (c == DEL_KEY)
			editor_move_cursor(ARROW_RIGHT);
		editor_delete_char();
		break;
	case CTRL_('l'):
	case '\x1b':
		break;
	case '\t':
		editor_insert_char('\t', true);
		break;
	default:
		/* Only insert printable ASCII and high bytes (UTF-8);
		 * silently ignore unassigned control chars (< 0x20) and
		 * unassigned Meta/ESC sequences (>= 0x800). */
		if (c >= 0x20 && c < 0x100)
			editor_insert_char(c, true);
	}
}

static void editor_init(void)
{
	init_ec();
	term_update_size();
	signal(SIGWINCH, sig_winch_handler);
	signal(SIGCONT, sig_cont_handler);
	srand((unsigned int)time(NULL));
	ec.rows = tlist_new(sizeof(editor_row_t));
}

int main(int argc, char *argv[])
{
	editor_init();
	if (argc >= 2)
		if(!file_open(argv[1])) {
			perr("Failed to open file: %s");
			return 1;
		}
	term_enable_raw();
	ui_set_message("%s | ^G Help", ec.read_only?"no write permission":"CULO Editor");
	editor_refresh();

	/* Main event loop */
	while (1) {
		editor_handle_signals();
		/* Input already queued means the previous keypress was part
		 * of a burst, so this one is too: keep the burst open. */
		if (input_pending())
			burst_open(ec.cursor_y, ec.cursor_x);
		editor_process_key();
		/* A burst of input (paste) is still queued: skip the repaint
		 * and keep consuming keys. The final key of the burst finds
		 * the queue empty and repaints once. */
		if (input_pending())
			continue;
		burst_close();
		if (ec.mode != MODE_BROWSER && ec.mode != MODE_HELP &&
		    ec.mode != MODE_SEARCH)
			editor_refresh();
	}
	/* not reachable */
	return 0;
}
