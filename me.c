/* Mazu Editor:
 * A minimalist editor with syntax highlight, copy/paste, and search.
 */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* Enable SIGWINCH on macOS */
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strcasestr */
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <regex.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include <time.h>

/* Search mode flags (bitmask) */
typedef enum {
    SM_NONE           = 0,
    SM_CASE_SENSITIVE = 1,
    SM_BACKWARDS      = 2,
    SM_REPLACE        = 4,
    SM_REGEX          = 8,
} search_mode_t;

#define CTRL_(k) ((k) & (0x1f))
#define META_(k) (0x800 | (unsigned char)(k))
#define TAB_STOP 4
#define UNDO_STACK_CAP 64

/* UTF-8 handling functions */

/* Get the byte length of a UTF-8 character from its first byte */
static inline int utf8_byte_length(uint8_t c)
{
    if (!(c & 0x80))
        return 1; /* ASCII */
    if ((c & 0xE0) == 0xC0 && c >= 0xC2)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0 && c <= 0xF4)
        return 4;
    return 1; /* Invalid UTF-8, treat as single byte */
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
static inline int utf8_char_width_and_len(const char *s, size_t max_len, int *char_len)
{
    unsigned char c = (unsigned char) *s;
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
        codepoint = ((c & 0x1F) << 6) | ((unsigned char) s[1] & 0x3F);
    } else if (len == 3) {
        codepoint = ((c & 0x0F) << 12) |
                    (((unsigned char) s[1] & 0x3F) << 6) |
                    ((unsigned char) s[2] & 0x3F);
    } else {
        codepoint = ((c & 0x07) << 18) |
                    (((unsigned char) s[1] & 0x3F) << 12) |
                    (((unsigned char) s[2] & 0x3F) << 6) |
                    ((unsigned char) s[3] & 0x3F);
    }

    if ((codepoint >= 0x4E00 &&
         codepoint <= 0x9FFF) || /* CJK Unified Ideographs */
        (codepoint >= 0x3400 && codepoint <= 0x4DBF) || /* CJK Extension A */
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) || /* CJK Compatibility */
        (codepoint >= 0x2E80 && codepoint <= 0x2EFF) || /* CJK Radicals */
        (codepoint >= 0x3000 && codepoint <= 0x303F) || /* CJK Punctuation */
        (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) { /* Fullwidth forms */
        return 2;
    }

    /* Combining marks have zero width */
    if ((codepoint >= 0x0300 &&
         codepoint <= 0x036F) || /* Combining Diacritical Marks */
        (codepoint >= 0x1AB0 &&
         codepoint <= 0x1AFF) || /* Combining Diacritical Extended */
        (codepoint >= 0x1DC0 &&
         codepoint <= 0x1DFF)) { /* Combining Diacritical Supplement */
        return 0;
    }

    return 1;
}

/* Move to the next UTF-8 character boundary */
static inline const char *utf8_next_char(const char *s)
{
    return *s ? s + utf8_byte_length((uint8_t) *s) : s;
}

/* Move to the previous UTF-8 character boundary */
static inline const char *utf8_prev_char(const char *start, const char *s)
{
    if (s <= start)
        return start;
    --s;
    while (s > start && is_utf8_continuation((uint8_t) *s))
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

    unsigned char c = (unsigned char) *s;

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
        if (c == 0xE0 && (unsigned char) s[1] < 0xA0)
            return 0;
        if (c == 0xED && (unsigned char) s[1] > 0x9F)
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
        if (c == 0xF0 && (unsigned char) s[1] < 0x90)
            return 0;
        if (c == 0xF4 && (unsigned char) s[1] > 0x8F)
            return 0;
        return 4;
    }

    return 0;
}

/* Upstream tlist implementation:
 * https://github.com/rofl0r/libulz/blob/master/include/tlist.h */

typedef struct tlist tlist;

#define TLIST_INTERNAL static


#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif

TLIST_INTERNAL int tlist_mrand(unsigned *seed)
{
       return ((*seed = (*seed+1) * 1103515245 + 12345 - 1)+1) & 0x7fffffff;
}

typedef struct item* pitem;
struct item {
	unsigned prior, cnt;
	pitem l, r;
};

TLIST_INTERNAL unsigned tlist_cnt (pitem it) {
	return it ? it->cnt : 0;
}

TLIST_INTERNAL void tlist_upd_cnt (pitem it) {
	if (it)
		it->cnt = tlist_cnt(it->l) + tlist_cnt(it->r) + 1;
}

TLIST_INTERNAL void tlist_merge (pitem *t, pitem l, pitem r) {
	if (!l || !r)
		*t = l ? l : r;
	else if (l->prior > r->prior)
		tlist_merge (&l->r, l->r, r),  *t = l;
	else
		tlist_merge (&r->l, l, r->l),  *t = r;
	tlist_upd_cnt (*t);
}

TLIST_INTERNAL void tlist_split (pitem t, pitem *l, pitem *r, unsigned key, unsigned add) {
	if (!t) {
		*l = *r = 0;
		return;
	}
	unsigned cur_key = add + tlist_cnt(t->l);
	if (key <= cur_key)
		tlist_split (t->l, l, &t->l, key, add),  *r = t;
	else
		tlist_split (t->r, &t->r, r, key, add + 1 + tlist_cnt(t->l)),  *l = t;
	tlist_upd_cnt (t);
}

TLIST_INTERNAL pitem tlist_getitem(pitem t, unsigned idx, unsigned add) {
	if (!t) return t;
	unsigned ls = tlist_cnt (t->l), cur_key = add + ls;
	if (cur_key == idx) return t;
	if (cur_key < idx)
		return tlist_getitem (t->r, idx, add + 1 + ls);
	else
		return tlist_getitem (t->l, idx, add);
}

TLIST_INTERNAL void tlist_insert_item(pitem *t, pitem n, unsigned idx) {
	pitem t1, t2;
	tlist_split (*t, &t1, &t2, idx, 0);
	tlist_merge (t, t1, n);
	tlist_merge (t, *t, t2);
}

TLIST_INTERNAL void tlist_remove(pitem *t, unsigned idx, unsigned add) {
	pitem n;
	if (!(*t)) return;
	unsigned cur_key = add + tlist_cnt ((*t)->l), new_add = cur_key + 1;
	unsigned lk = UINT_MAX, rk = UINT_MAX;
	if ((*t)->l) lk = tlist_cnt ((*t)->l->l) + add;
	if ((*t)->r) rk = tlist_cnt ((*t)->r->l) + new_add;
	if (cur_key == idx) {
		tlist_merge (t, (*t)->l, (*t)->r);
	} else if (lk == idx) {
		tlist_merge (&n, (*t)->l->l, (*t)->l->r);
		(*t)->l = n;
		tlist_upd_cnt (*t);
	} else if (rk == idx) {
		tlist_merge (&n, (*t)->r->l, (*t)->r->r);
		(*t)->r = n;
		tlist_upd_cnt (*t);
	} else if (cur_key < idx) {
		tlist_remove (&(*t)->r, idx, new_add);
		tlist_upd_cnt (*t);
	} else {
		tlist_remove (&(*t)->l, idx, add);
		tlist_upd_cnt (*t);
	}
}

TLIST_INTERNAL pitem tlist_new_item(void* value, unsigned valsz, unsigned *seed) {
	pitem n = malloc(sizeof(struct item) + valsz);
	if(!n) return n;
	memcpy(n+1, value, valsz);
	n->prior = tlist_mrand(seed);
	n->cnt = 1;
	n->l = n->r = 0;
	return n;
}

struct tlist {
	unsigned seed;
	unsigned itemsize;
	pitem root;
};

static struct tlist *tlist_new(unsigned itemsize) {
	struct tlist* new = malloc(sizeof (struct tlist));
	if(!new) return 0;
	new->seed = 385-1;
	new->itemsize = itemsize;
	new->root = 0;
	return new;
}

TLIST_INTERNAL void* tlist_data(pitem it) {
	return it+1;
}

static size_t tlist_getsize(struct tlist* t) {
	return tlist_cnt(t->root);
}

static void* tlist_get(struct tlist* t, size_t idx) {
	if(idx >= tlist_cnt (t->root)) return 0;
	return tlist_data(tlist_getitem(t->root, idx, 0));
}

static int tlist_insert(struct tlist* t, size_t idx, void *value) {
	if(idx > tlist_cnt (t->root)) return 0;
	pitem new = tlist_new_item(value, t->itemsize, &t->seed);
	if(!new) return 0;
	tlist_insert_item(&t->root, new, idx);
	return 1;
}

TLIST_INTERNAL int tlist_delete_impl(struct tlist *t, size_t idx) {
	if(idx >= tlist_cnt (t->root)) return 0;
	pitem it = tlist_getitem(t->root, idx, 0);
	tlist_remove(&t->root, idx, 0);
	free(it);
	return 1;
}

static int tlist_delete(struct tlist *t, size_t idx) {
	return tlist_delete_impl(t, idx);
}

static void tlist_free_items(struct tlist *t) {
	while(tlist_cnt(t->root)) tlist_delete_impl(t, 0);
}

static void* tlist_free(struct tlist *t) {
	tlist_free_items(t);
	free(t);
	return 0;
}

#undef TLIST_INTERNAL

typedef struct {
    int size;
    int render_size;
    char *chars;
    char *render;
    unsigned char *highlight;
    bool render_direct_map;
    bool hl_open_comment;
    bool hl_valid;
} editor_row_t;

/* One entry in a keyword table.  len is a compile-time sizeof(literal)-1 so
 * the highlight loop never calls strlen(). */
typedef struct { const char *str; int len; unsigned char type; } keyword_t;

/* Syntax highlighting structure */
typedef struct {
    char *file_type;
    char **file_match;
    const keyword_t *keywords;
    char *sl_comment_start;                  /* single line */
    char *ml_comment_start, *ml_comment_end; /* multiple lines */
    int flags;
} editor_syntax_t;

/* X-macro for editor modes */
#define EDITOR_MODES                                  \
    _(NORMAL, "NORMAL", "Default editing mode")       \
    _(SEARCH, "SEARCH", "Search mode (Ctrl-W)")       \
    _(PROMPT, "PROMPT", "Generic prompt mode")        \
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
    MODE_COUNT /* Total number of modes */
} editor_mode_t;
/* clang-format on */

/* Text selection state */
typedef struct {
    int start_x, start_y; /* Selection start position */
    int end_x, end_y;     /* Selection end position */
    bool active;          /* Is selection active? */
} selection_state_t;

/* Mode-specific state data */
typedef union {
    struct {
        int saved_x, saved_y, saved_col, saved_row;
        int highlight_line;    /* Row index with search highlight (-1 = none) */
        char *saved_highlight; /* Saved highlight data for highlight_line */
    } search;
    struct {
        char *buffer;
    } prompt;
    struct {
        char **entries;    /* Array of file/dir names */
        int num_entries;   /* Number of entries */
        int selected;      /* Currently selected entry */
        int offset;        /* Scroll offset */
        char *current_dir; /* Current directory path */
        bool show_hidden;  /* Show hidden files (toggle with H) */
    } browser;
    struct {
        int offset;        /* Scroll offset (lines from top) */
    } help;
} mode_data_t;

/* Editor config structure */
struct {
    int cursor_x, cursor_y, render_x;
    int row_offset, col_offset;
    int screen_rows, screen_cols;
    tlist *rows;
    bool modified;
    char *file_name;
    char status_msg[90];
    time_t status_msg_time;
    char *copied_char_buffer;
    editor_syntax_t *syntax;
    struct termios orig_termios;
    /* Editor mode state machine */
    editor_mode_t mode;
    editor_mode_t prev_mode;     /* For returning from temporary modes */
    mode_data_t mode_state;      /* Mode-specific state data */
    selection_state_t selection; /* Text selection state */
    bool show_line_numbers;      /* Toggle line numbers display */
    bool last_was_cut;           /* True if previous key was ^K (for appending cuts) */
    struct {
        char *query;             /* Persists across ^W invocations; NULL until first search */
        size_t query_len;
        size_t query_cap;
        int mode;                /* search_mode_t bitmask */
        char *replace_query;     /* replacement text */
        size_t replace_len;
        size_t replace_cap;
        int replace_phase;       /* 0=search_input, 1=replace_input, 2=confirming each */
        int replace_count;       /* replacements made so far */
        int orig_row, orig_char; /* cursor pos when replace phase 2 began; used to stop after one cycle */
        bool has_wrapped;        /* true once any search_do_from call in this session returned wrapped=true */
    } search;
    char notfound_msg[200];      /* transient "not found" overlay; cleared on next keypress */
} ec = {
    .cursor_x = 0,
    .cursor_y = 0,
    .render_x = 0,
    .row_offset = 0,
    .col_offset = 0,
    .rows = NULL,
    .modified = false,
    .file_name = NULL,
    .status_msg = "",
    .status_msg_time = 0,
    .copied_char_buffer = NULL,
    .syntax = NULL,
    .mode = MODE_NORMAL,
    .prev_mode = MODE_NORMAL,
    .mode_state = {{0}},
    .selection =
        {
            .start_x = 0,
            .start_y = 0,
            .end_x = 0,
            .end_y = 0,
            .active = false,
        },
    .show_line_numbers = false,
    .last_was_cut = false,
    .search = { .query = NULL, .query_len = 0, .query_cap = 0, .mode = SM_NONE,
                .replace_query = NULL, .replace_len = 0, .replace_cap = 0,
                /* orig_row=-1 means "no active replace cycle"; orig_char is only
                 * meaningful when orig_row >= 0, so 0 is a fine default. */
                .replace_phase = 0, .replace_count = 0, .orig_row = -1, .orig_char = 0,
                .has_wrapped = false },
    .notfound_msg = "",
};

typedef enum {
    EDIT_INSERT = 0,
    EDIT_DELETE = 1,
    EDIT_REPLACE = 2,
} edit_type_t;

typedef struct {
    edit_type_t type;
    int row, col;
    char *text;
    size_t len;
    size_t aux_len;
    int before_y, before_x;
    int after_y, after_x;
} undo_item_t;

typedef struct {
    undo_item_t history[UNDO_STACK_CAP];
    int start;   /* oldest entry index in ring */
    int count;   /* number of valid entries */
    int cursor;  /* number of applied entries from start; [0..count] */
    bool replaying;
    bool batching;
} undo_state_t;

static undo_state_t g_undo = {0};

/* Number of rows */
#define NR ((int)tlist_getsize(ec.rows))

/* Pointer to the editor_row_t at index i */
static inline editor_row_t *ROW(int i)
{
    if (!ec.rows || i < 0 || i >= NR)
        return NULL;
    return (editor_row_t *)tlist_get(ec.rows, (size_t) (i));
}

/* Manual typing growth alignment (power of two). */
#define ROW_MANUAL_ALLOC_ALIGN 64

static inline size_t row_manual_alloc_size(size_t content_len)
{
    size_t need = content_len + 1; /* include NUL */
    size_t a = ROW_MANUAL_ALLOC_ALIGN;
    return (need + (a - 1)) & ~(a - 1);
}

/* Free a row's heap-owned sub-fields (NOT the row struct itself, which is
 * owned by the tlist node). */
static void row_free_contents(editor_row_t *row)
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
    tlist_delete(ec.rows, (size_t) at);
}

typedef struct {
    char *buf;
    int len;
} editor_buf_t;

/* clang-format off */
enum editor_key {
    BACKSPACE = 0x7f,
    ARROW_LEFT = 0x3e8, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    PAGE_UP, PAGE_DOWN,
    HOME_KEY, END_KEY, DEL_KEY,
};

/* X-macro for syntax highlighting types */
#define HIGHLIGHT_TYPES                         \
    _(NORMAL, 97, "Default text")               \
    _(MATCH, 43, "Search match")                \
    _(SL_COMMENT, 36, "Single-line comment")    \
    _(ML_COMMENT, 36, "Multi-line comment")     \
    _(KEYWORD_1, 93, "Primary keyword")         \
    _(KEYWORD_2, 92, "Secondary keyword")       \
    _(KEYWORD_3, 36, "Preprocessor")            \
    _(STRING, 91, "String literal")             \
    _(NUMBER, 31, "Numeric literal")

/* Generate highlight enum using X-macro */
typedef enum {
#define _(type, color, desc) type,
    HIGHLIGHT_TYPES
#undef _
    HIGHLIGHT_COUNT
} highlight_type_t;
/* clang-format on */

#define HIGHLIGHT_NUMBERS (1 << 0)
#define HIGHLIGHT_STRINGS (1 << 1)

char *C_extensions[] = {".c", ".cc", ".cxx", ".cpp", ".h", NULL};

/* Macros to build keyword table entries with compile-time lengths. */
#define KW1(s) { s, sizeof(s)-1, KEYWORD_1 }
#define KW2(s) { s, sizeof(s)-1, KEYWORD_2 }
#define KW3(s) { s, sizeof(s)-1, KEYWORD_3 }

keyword_t C_keywords[] = {
    KW1("switch"),   KW1("if"),       KW1("while"),    KW1("for"),      KW1("break"),
    KW1("continue"), KW1("return"),   KW1("else"),     KW1("struct"),   KW1("union"),
    KW1("typedef"),  KW1("static"),   KW1("enum"),     KW1("class"),    KW1("case"),
    KW1("volatile"), KW1("register"), KW1("sizeof"),   KW1("goto"),     KW1("const"),
    KW1("auto"),
    KW3("#if"),      KW3("#endif"),   KW3("#error"),   KW3("#ifdef"),   KW3("#ifndef"),
    KW3("#elif"),    KW3("#define"),  KW3("#undef"),   KW3("#include"),
    KW2("int"),      KW2("long"),     KW2("double"),   KW2("float"),    KW2("char"),
    KW2("unsigned"), KW2("signed"),   KW2("void"),     KW2("bool"),
    { NULL, 0, 0 },
};

#undef KW1
#undef KW2
#undef KW3

editor_syntax_t DB[] = {
    {
        "c",
        C_extensions,
        C_keywords,
        "//",
        "/*",
        "*/",
        HIGHLIGHT_NUMBERS | HIGHLIGHT_STRINGS,
    },
};

#define DB_ENTRIES (sizeof(DB) / sizeof(DB[0]))

static char *ui_prompt(const char *msg, void (*callback)(char *, int));
static void editor_refresh(void);
static int get_line_number_width(void);
static void editor_newline(void);
static void editor_insert_char(int c, bool manual_typing);
static void undo_reset_history(void);
static void undo_record_insert(int row, int col, const char *text, size_t len,
                               int before_y, int before_x, int after_y, int after_x);
static void undo_record_delete(int row, int col, const char *text, size_t len,
                               int before_y, int before_x, int after_y, int after_x);
static void undo_record_replace(int row, int col, const char *old_text, size_t old_len,
                                const char *new_text, size_t new_len,
                                int before_y, int before_x, int after_y, int after_x);
static void undo_perform_undo(void);
static void undo_perform_redo(void);
static bool undo_is_replaying(void);
static bool undo_is_batching(void);
static void undo_set_batching(bool batching);

/* Mode management implementation */
static void mode_set(editor_mode_t new_mode)
{
    /* Save current mode as previous (unless entering temporary mode) */
    if (ec.mode != MODE_PROMPT && ec.mode != MODE_CONFIRM &&
        ec.mode != MODE_HELP)
        ec.prev_mode = ec.mode;

    /* Clean up old mode state */
    switch (ec.mode) {
    case MODE_SEARCH:
        /* ec.search.query is persistent — do NOT free it here */
        /* Restore saved highlight if leaving search mode */
        if (ec.mode_state.search.saved_highlight &&
            ec.mode_state.search.highlight_line >= 0 &&
            ec.mode_state.search.highlight_line < NR &&
            ROW(ec.mode_state.search.highlight_line)->highlight) {
            memcpy(ROW(ec.mode_state.search.highlight_line)->highlight,
                   ec.mode_state.search.saved_highlight,
                   ROW(ec.mode_state.search.highlight_line)->render_size);
        }
        free(ec.mode_state.search.saved_highlight);
        ec.mode_state.search.saved_highlight = NULL;
        break;
    case MODE_PROMPT:
        free(ec.mode_state.prompt.buffer);
        ec.mode_state.prompt.buffer = NULL;
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
        ui_set_message("-- SELECT MODE -- Use arrows to extend, ^C to cancel");
        break;
    case MODE_SEARCH: {
        /* Allocate the query buffer the first time; subsequent ^W reuses it */
        if (!ec.search.query) {
            size_t cap = 128;
            char *q = calloc(cap, 1);
            if (q) {
                ec.search.query = q;
                ec.search.query_cap = cap;
                ec.search.query_len = 0;
            }
        }
        /* Reset replace phase each time search mode is entered */
        ec.search.replace_phase = 0;
        ec.search.replace_count = 0;
        ec.mode_state.search.highlight_line = -1;
        ec.mode_state.search.saved_highlight = NULL;
        break;
    }
    case MODE_HELP:
        ec.mode_state.help.offset = 0;
        ui_set_message("^X or ^G or ^C to exit, arrows/PgUp/PgDn to scroll");
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
static const char * const help_lines[] = {
    "Help  (^X or ^G or ^C to exit, arrows/PgUp/PgDn to scroll)",
    "",
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
    "  M-6     Copy marked region",
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
    "  PgUp    Page up",
    "  PgDn    Page down",
    "  Home    Beginning of line",
    "  End     End of line",
    "  M-\\     Go to first line of file",
    "  M-/     Go to last line of file",
    "",
    "View:",
    "  ^N      Toggle line numbers",
    "  ^G      Show this help screen",
    "",
};
#define HELP_NUM_LINES (int)(sizeof(help_lines) / sizeof(help_lines[0]))

static void term_clear(void)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

static void panic(const char *s)
{
    term_clear();
    perror(s);
    puts("\r\n");
    exit(1);
}

static void term_open_buffer(void)
{
    if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
        panic("Error changing terminal buffer");
}

static void term_disable_raw(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec.orig_termios) == -1)
        panic("Failed to disable raw mode");
}

static void term_enable_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &ec.orig_termios) == -1)
        panic("Failed to get current terminal state");
    atexit(term_disable_raw);
    struct termios raw = ec.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    term_open_buffer();
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        panic("Failed to set raw mode");
}

static int term_read_key(void)
{
    static bool meta_pending = false;
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if ((nread == -1) && (errno != EAGAIN))
            panic("Error reading input");
    }
    /* If a previous lone ESC was recorded, combine it with this character */
    if (meta_pending) {
        meta_pending = false;
        /* Only META printable characters; leave control chars for their normal handlers */
        if (c != '\x1b' && (unsigned char)c >= 0x20)
            return META_((unsigned char)c);
        /* Two ESCs in a row, or a control char: fall through to handle normally */
    }
    if (c == '\x1b') {
        char seq[3];
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

static void term_update_size(void)
{
    if (term_get_size(&ec.screen_rows, &ec.screen_cols) == -1) {
        /* Fallback to reasonable defaults for testing */
        ec.screen_rows = 24, ec.screen_cols = 80;
    }
    ec.screen_rows -= 2;
}

static void term_close_buffer(void)
{
    if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
        write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
        panic("Error restoring buffer state");
    term_clear();
}

static bool syntax_is_separator(int c)
{
    return isspace(c) || !c || strchr(",.()+-/*=~%<>[]:;", c);
}

static bool syntax_is_number_part(int c)
{
    return c == '.' || c == 'x' || c == 'a' || c == 'b' || c == 'c' ||
           c == 'd' || c == 'e' || c == 'f' || c == 'A' || c == 'X' ||
           c == 'B' || c == 'C' || c == 'D' || c == 'E' || c == 'F' ||
           c == 'h' || c == 'H';
}

static void syntax_highlight(editor_row_t *row, int row_idx)
{
    row->highlight = realloc(row->highlight, row->render_size);
    memset(row->highlight, NORMAL, row->render_size);
    if (!ec.syntax)
        return;
    const keyword_t *keywords = ec.syntax->keywords;
    char *scs = ec.syntax->sl_comment_start;
    char *mcs = ec.syntax->ml_comment_start;
    char *mce = ec.syntax->ml_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    bool prev_sep = true;
    int in_string = 0;
    bool in_comment = (row_idx > 0 && ROW(row_idx - 1)->hl_open_comment);
    int i = 0;
    while (i < row->render_size) {
        char c = row->render[i];
        unsigned char prev_highlight = (i > 0) ? row->highlight[i - 1] : NORMAL;
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->highlight[i], SL_COMMENT, row->render_size - i);
                break;
            }
        }
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->highlight[i] = ML_COMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->highlight[i], ML_COMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = true;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->highlight[i], ML_COMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        if (ec.syntax->flags & HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->highlight[i] = STRING;
                if ((c == '\\') && (i + 1 < row->render_size)) {
                    row->highlight[i + 1] = STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string)
                    in_string = 0;
                i++;
                prev_sep = true;
                continue;
            } else {
                if ((c == '"') || (c == '\'')) {
                    in_string = c;
                    row->highlight[i] = STRING;
                    i++;
                    continue;
                }
            }
        }
        if (ec.syntax->flags & HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || (prev_highlight == NUMBER))) ||
                (syntax_is_number_part(c) && (prev_highlight == NUMBER))) {
                row->highlight[i] = NUMBER;
                i++;
                prev_sep = false;
                continue;
            }
        }
        if (prev_sep) {
            const keyword_t *kw;
            for (kw = keywords; kw->str; kw++) {
                if (!strncmp(&row->render[i], kw->str, kw->len) &&
                    syntax_is_separator(row->render[i + kw->len])) {
                    memset(&row->highlight[i], kw->type, kw->len);
                    i += kw->len;
                    break;
                }
            }
            if (kw->str) {
                prev_sep = false;
                continue;
            }
        }
        prev_sep = syntax_is_separator(c);
        i++;
    }
    bool changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row_idx + 1 < NR)
        syntax_highlight(ROW(row_idx + 1), row_idx + 1);
}

/* Reference: https://misc.flogisoft.com/bash/tip_colors_and_formatting */
static int syntax_token_color(int highlight)
{
    /* Generate color mapping using X-macro */
    static const int highlight_colors[] = {
#define _(type, color, desc) [type] = color,
        HIGHLIGHT_TYPES
#undef _
    };

    if (highlight >= 0 && highlight < HIGHLIGHT_COUNT)
        return highlight_colors[highlight];
    return 97; /* Default white */
}

static void syntax_select(void)
{
    ec.syntax = NULL;
    if (!ec.file_name)
        return;
    for (size_t j = 0; j < DB_ENTRIES; j++) {
        editor_syntax_t *es = &DB[j];
        for (size_t i = 0; es->file_match[i]; i++) {
            char *p = strstr(ec.file_name, es->file_match[i]);
            if (!p)
                continue;
            int pat_len = strlen(es->file_match[i]);
            if ((es->file_match[i][0] != '.') || (p[pat_len] == '\0')) {
                ec.syntax = es;
                for (int file_row = 0; file_row < NR; file_row++)
                    syntax_highlight(ROW(file_row), file_row);
                return;
            }
        }
    }
}

static int row_cursorx_to_renderx(editor_row_t *row, int cursor_x)
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
            int char_width = utf8_char_width_and_len(&row->chars[byte_pos],
                                                     (size_t) (row->size - byte_pos),
                                                     &char_len);
            render_x += char_width;
            byte_pos += char_len;
        }
    }
    return render_x;
}

static int row_renderx_to_cursorx(editor_row_t *row, int render_x)
{
    if (row->render_direct_map) {
        if (render_x < 0)
            return 0;
        return render_x > row->size ? row->size : render_x;
    }

    int cur_render_x = 0;
    int byte_pos = 0;

    while (byte_pos < row->size) {
        int next_render_x = cur_render_x;
        int char_len = 1;

        if (row->chars[byte_pos] == '\t') {
            next_render_x += (TAB_STOP - 1) - (cur_render_x % TAB_STOP);
            next_render_x++;
        } else {
            next_render_x += utf8_char_width_and_len(&row->chars[byte_pos],
                                                     (size_t) (row->size - byte_pos),
                                                     &char_len);
        }

        if (next_render_x > render_x)
            return byte_pos;

        cur_render_x = next_render_x;

        if (row->chars[byte_pos] == '\t') {
            byte_pos++;
        } else {
            byte_pos += char_len;
        }
    }
    return byte_pos;
}

static void row_update(editor_row_t *row, int row_idx)
{
    int tabs = 0;
    int wide_chars = 0;
    bool direct_map = true;
    int byte_pos = 0;

    /* Count tabs and wide characters for buffer allocation */
    while (byte_pos < row->size) {
        unsigned char c = (unsigned char) row->chars[byte_pos];
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
            int char_width = utf8_char_width_and_len(&row->chars[byte_pos],
                                                     (size_t) (row->size - byte_pos),
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
    /* Allocate extra space for tabs and wide characters */
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + wide_chars + 1);

    int idx = 0;
    byte_pos = 0;

    while (byte_pos < row->size) {
        if (row->chars[byte_pos] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0)
                row->render[idx++] = ' ';
            byte_pos++;
        } else {
            int char_len = utf8_byte_length((uint8_t) row->chars[byte_pos]);
            /* Copy the UTF-8 sequence as-is */
            for (int i = 0; i < char_len && byte_pos + i < row->size; i++)
                row->render[idx++] = row->chars[byte_pos + i];
            byte_pos += char_len;
        }
    }
    row->render[idx] = '\0';
    row->render_size = idx;
    syntax_highlight(row, row_idx);
}

static void row_insert(int at, const char *s, size_t line_len)
{
    if (at < 0 || at > NR)
        return;
    editor_row_t row = {0};
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

static void undo_item_free(undo_item_t *item)
{
    free(item->text);
    item->text = NULL;
    item->len = 0;
    item->aux_len = 0;
}

static int undo_history_index(int logical_idx)
{
    int idx = g_undo.start + logical_idx;
    if (idx >= UNDO_STACK_CAP)
        idx -= UNDO_STACK_CAP;
    return idx;
}

static void undo_history_clear(void)
{
    for (int i = 0; i < g_undo.count; i++) {
        int idx = undo_history_index(i);
        undo_item_free(&g_undo.history[idx]);
        memset(&g_undo.history[idx], 0, sizeof(g_undo.history[idx]));
    }
    g_undo.start = 0;
    g_undo.count = 0;
    g_undo.cursor = 0;
}

static void undo_history_discard_redo(void)
{
    while (g_undo.count > g_undo.cursor) {
        int idx = undo_history_index(g_undo.count - 1);
        undo_item_free(&g_undo.history[idx]);
        memset(&g_undo.history[idx], 0, sizeof(g_undo.history[idx]));
        g_undo.count--;
    }
}

static void undo_history_append(undo_item_t item)
{
    undo_history_discard_redo();
    if (g_undo.count == UNDO_STACK_CAP) {
        int idx = g_undo.start;
        undo_item_free(&g_undo.history[idx]);
        memset(&g_undo.history[idx], 0, sizeof(g_undo.history[idx]));
        g_undo.start++;
        if (g_undo.start >= UNDO_STACK_CAP)
            g_undo.start = 0;
        g_undo.count--;
        if (g_undo.cursor > 0)
            g_undo.cursor--;
    }
    int write_idx = undo_history_index(g_undo.count);
    g_undo.history[write_idx] = item;
    g_undo.count++;
    g_undo.cursor = g_undo.count;
}

static bool undo_insert_bytes(int row_idx, int col, const char *s, size_t len)
{
    editor_row_t *row = ROW(row_idx);
    if (!row || len == 0)
        return row != NULL;
    if (col < 0)
        col = 0;
    if (col > row->size)
        col = row->size;
    char *nc = realloc(row->chars, (size_t)row->size + len + 1);
    if (!nc)
        return false;
    row->chars = nc;
    memmove(&row->chars[col + len], &row->chars[col], (size_t)(row->size - col) + 1);
    memcpy(&row->chars[col], s, len);
    row->size += (int)len;
    row_update(row, row_idx);
    return true;
}

static bool undo_apply_insert_text(int row, int col, const char *text, size_t len)
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
            row_insert(y + 1, &r->chars[x], (size_t)(r->size - x));
            r = ROW(y);
            r->size = x;
            r->chars[x] = '\0';
            row_update(r, y);
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
    if (col < 0 || col > row->size || (size_t)(row->size - col) < len)
        return false;
    memmove(&row->chars[col], &row->chars[col + len], (size_t)(row->size - col) - len + 1);
    row->size -= (int)len;
    row_update(row, row_idx);
    return true;
}

static bool undo_apply_delete_text(int row, int col, const char *text, size_t len)
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
            char *nc = realloc(r->chars, (size_t)r->size + (size_t)next->size + 1);
            if (!nc)
                return false;
            r->chars = nc;
            memcpy(&r->chars[r->size], next->chars, (size_t)next->size + 1);
            r->size += next->size;
            row_update(r, y);
            row_erase(y + 1);
            i++;
        }
    }
    ec.modified = true;
    return true;
}

static void undo_reset_history(void)
{
    undo_history_clear();
}

static bool undo_is_replaying(void)
{
    return g_undo.replaying;
}

static bool undo_is_batching(void)
{
    return g_undo.batching;
}

static void undo_set_batching(bool batching)
{
    g_undo.batching = batching;
}

static void undo_record_common(edit_type_t type, int row, int col, const char *text, size_t len,
                               int before_y, int before_x, int after_y, int after_x)
{
    if (g_undo.replaying || g_undo.batching || !text || len == 0)
        return;
    undo_item_t item = {
        .type = type,
        .row = row,
        .col = col,
        .text = xstrndup0(text, len),
        .len = len,
        .before_y = before_y,
        .before_x = before_x,
        .after_y = after_y,
        .after_x = after_x,
    };
    if (!item.text)
        return;
    undo_history_append(item);
}

static void undo_record_insert(int row, int col, const char *text, size_t len,
                               int before_y, int before_x, int after_y, int after_x)
{
    undo_record_common(EDIT_INSERT, row, col, text, len, before_y, before_x, after_y, after_x);
}

static void undo_record_delete(int row, int col, const char *text, size_t len,
                               int before_y, int before_x, int after_y, int after_x)
{
    undo_record_common(EDIT_DELETE, row, col, text, len, before_y, before_x, after_y, after_x);
}

static bool undo_apply_replace_span(int row, int col, size_t from_len,
                                    const char *to_text, size_t to_len)
{
    editor_row_t *r = ROW(row);
    if (!r || col < 0 || col > r->size || (size_t)(r->size - col) < from_len)
        return false;
    size_t new_size = (size_t)r->size - from_len + to_len;
    if (new_size > INT_MAX)
        return false;
    char *nc = realloc(r->chars, new_size + 1);
    if (!nc)
        return false;
    r->chars = nc;
    memmove(&r->chars[col + to_len], &r->chars[col + from_len],
            (size_t)(r->size - col) - from_len + 1);
    if (to_len > 0)
        memcpy(&r->chars[col], to_text, to_len);
    r->size = (int)new_size;
    r->chars[r->size] = '\0';
    row_update(r, row);
    ec.modified = true;
    return true;
}

static void undo_record_replace(int row, int col, const char *old_text, size_t old_len,
                                const char *new_text, size_t new_len,
                                int before_y, int before_x, int after_y, int after_x)
{
    if (g_undo.replaying || g_undo.batching || !old_text || !new_text)
        return;
    if (old_len > SIZE_MAX - new_len - 1)
        return;
    size_t total_len = old_len + new_len;
    undo_item_t item = {
        .type = EDIT_REPLACE,
        .row = row,
        .col = col,
        .text = malloc(total_len + 1),
        .len = old_len,
        .aux_len = new_len,
        .before_y = before_y,
        .before_x = before_x,
        .after_y = after_y,
        .after_x = after_x,
    };
    if (!item.text)
        return;
    memcpy(item.text, old_text, old_len);
    memcpy(item.text + old_len, new_text, new_len);
    item.text[total_len] = '\0';
    undo_history_append(item);
}

static void undo_apply_item(const undo_item_t *item, bool redo)
{
    bool ok;
    if (redo) {
        if (item->type == EDIT_INSERT) {
            ok = undo_apply_insert_text(item->row, item->col, item->text, item->len);
        } else if (item->type == EDIT_DELETE) {
            ok = undo_apply_delete_text(item->row, item->col, item->text, item->len);
        } else {
            ok = undo_apply_replace_span(item->row, item->col, item->len,
                                         item->text + item->len, item->aux_len);
        }
        if (ok) {
            ec.cursor_y = item->after_y;
            ec.cursor_x = item->after_x;
        }
    } else {
        if (item->type == EDIT_INSERT) {
            ok = undo_apply_delete_text(item->row, item->col, item->text, item->len);
        } else if (item->type == EDIT_DELETE) {
            ok = undo_apply_insert_text(item->row, item->col, item->text, item->len);
        } else {
            ok = undo_apply_replace_span(item->row, item->col, item->aux_len,
                                         item->text, item->len);
        }
        if (ok) {
            ec.cursor_y = item->before_y;
            ec.cursor_x = item->before_x;
        }
    }
    if (ec.cursor_y < 0) ec.cursor_y = 0;
    if (ec.cursor_y > NR) ec.cursor_y = NR;
    if (ec.cursor_y < NR && ec.cursor_x > ROW(ec.cursor_y)->size)
        ec.cursor_x = ROW(ec.cursor_y)->size;
    if (ec.cursor_x < 0) ec.cursor_x = 0;
}

static void undo_perform_undo(void)
{
    if (g_undo.cursor <= 0) {
        ui_set_message("Nothing to undo");
        return;
    }
    /* cursor points to the next unapplied entry; step back to the
     * entry we are about to undo and run its inverse. */
    g_undo.cursor--;
    undo_item_t *item = &g_undo.history[undo_history_index(g_undo.cursor)];
    g_undo.replaying = true;
    undo_apply_item(item, false);
    g_undo.replaying = false;
}

static void undo_perform_redo(void)
{
    if (g_undo.cursor >= g_undo.count) {
        ui_set_message("Nothing to redo");
        return;
    }
    undo_item_t *item = &g_undo.history[undo_history_index(g_undo.cursor)];
    g_undo.replaying = true;
    undo_apply_item(item, true);
    g_undo.replaying = false;
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

    int before_y = ec.cursor_y, before_x = ec.cursor_x;
    bool is_last_line = (ec.cursor_y == NR - 1);
    bool remove_newline = (NR > 1 && !is_last_line);
    size_t del_len = (size_t)ROW(ec.cursor_y)->size + (remove_newline ? 1u : 0u);
    bool have_newline = false;
    char *deleted = xstrndup0(ROW(ec.cursor_y)->chars, (size_t)ROW(ec.cursor_y)->size);
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
    size_t new_len = line_size + 1; /* text + \n */

    if (append && ec.copied_char_buffer) {
        /* Append this line to the existing clipboard */
        size_t old_len = strlen(ec.copied_char_buffer);
        char *new_buf = realloc(ec.copied_char_buffer, old_len + new_len + 1);
        if (!new_buf) {
            ui_set_message("Memory allocation failed");
            return;
        }
        ec.copied_char_buffer = new_buf;
        memcpy(ec.copied_char_buffer + old_len, ROW(ec.cursor_y)->chars, line_size);
        ec.copied_char_buffer[old_len + line_size] = '\n';
        ec.copied_char_buffer[old_len + line_size + 1] = '\0';
    } else {
        /* Replace clipboard with this line */
        char *new_buf = realloc(ec.copied_char_buffer, new_len + 1);
        if (!new_buf) {
            ui_set_message("Memory allocation failed");
            return;
        }
        ec.copied_char_buffer = new_buf;
        memcpy(ec.copied_char_buffer, ROW(ec.cursor_y)->chars, line_size);
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
                           before_y, before_x, ec.cursor_y, ec.cursor_x);
        free(deleted);
    }
}

static void editor_paste(void)
{
    if (!ec.copied_char_buffer)
        return;

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
    undo_set_batching(true);
    for (size_t i = 0; i < paste_len; i++) {
        if (ec.copied_char_buffer[i] == '\n')
            editor_newline();
        else
            editor_insert_char((unsigned char) ec.copied_char_buffer[i], false);
    }
    undo_set_batching(false);
    if (paste_len > 0) {
        undo_record_insert(before_y, before_x, ec.copied_char_buffer, paste_len,
                           before_y, before_x, ec.cursor_y, ec.cursor_x);
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
            if (row->chars) { /* Safety check */
                int actual_start = start_x > row->size ? row->size : start_x;
                int actual_end = end_x > row->size ? row->size : end_x;
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
                int actual_start = start_x > row->size ? row->size : start_x;
                int len = row->size - actual_start;
                if (len > 0)
                    total_size += len;
                if (y < end_y)
                    total_size++; /* Add newline if not last line */
            } else if (y == end_y) {
                /* Last line: from beginning to end_x */
                int actual_end = end_x > row->size ? row->size : end_x;
                total_size += actual_end;
            } else {
                /* Middle lines: entire line */
                total_size += row->size;
                total_size++; /* Add newline */
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
            if (row->chars) { /* Safety check */
                /* Clamp positions to actual row size */
                int actual_start = start_x > row->size ? row->size : start_x;
                int actual_end = end_x > row->size ? row->size : end_x;
                int len = actual_end - actual_start;
                if (len > 0) {
                    memcpy(p, &row->chars[actual_start], len);
                    p += len;
                }
            }
        }
    } else {
        /* Multi-line - properly include newlines */
        for (int y = start_y; y <= end_y && y < NR; y++) {
            editor_row_t *row = ROW(y);
            if (!row->chars)
                continue; /* Safety check */

            if (y == start_y) {
                /* First line: from start_x to end of line */
                int actual_start = start_x > row->size ? row->size : start_x;
                int len = row->size - actual_start;
                if (len > 0) {
                    memcpy(p, &row->chars[actual_start], len);
                    p += len;
                }
                /* Always add newline after first line if not the last line */
                if (y < end_y) {
                    *p++ = '\n';
                }
            } else if (y == end_y) {
                /* Last line: from beginning to end_x */
                int actual_end = end_x > row->size ? row->size : end_x;
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
                *p++ = '\n'; /* Add newline after middle lines */
            }
        }
    }
    *p = '\0';

    return buffer;
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
            memmove(&row->chars[start_x], &row->chars[end_x], row->size - end_x + 1);
            row->size -= end_x - start_x;
            row_update(row, start_y);
            ec.modified = true;
        }
    } else {
        editor_row_t *start_row = ROW(start_y);
        editor_row_t *end_row = ROW(end_y);
        if (start_row && end_row) {
            int suffix_len = end_row->size - end_x;
            char *nc = realloc(start_row->chars, start_x + suffix_len + 1);
            if (!nc) {
                ec.selection.active = false;
                mode_set(MODE_NORMAL);
                return;
            }
            start_row->chars = nc;
            memcpy(&start_row->chars[start_x], &end_row->chars[end_x], suffix_len);
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
                           before_y, before_x, ec.cursor_y, ec.cursor_x);
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
    int before_y = ec.cursor_y, before_x = ec.cursor_x;
    if (ec.cursor_x == 0) {
        row_insert(ec.cursor_y, "", 0);
    } else {
        editor_row_t *row = ROW(ec.cursor_y);
        row_insert(ec.cursor_y + 1, &row->chars[ec.cursor_x], row->size - ec.cursor_x);
        row = ROW(ec.cursor_y);
        row->size = ec.cursor_x;
        row->chars[row->size] = '\0';
        row_update(row, ec.cursor_y);
    }
    ec.cursor_y++;
    ec.cursor_x = 0;
    ec.modified = true;
    if (!undo_is_replaying() && !undo_is_batching()) {
        static const char nl[] = "\n";
        undo_record_insert(before_y, before_x, nl, 1, before_y, before_x,
                           ec.cursor_y, ec.cursor_x);
    }
}

/* Buffer for accumulating UTF-8 bytes */
static struct {
    char bytes[4];
    int len;
    int expected;
} utf8_buffer = {.len = 0, .expected = 0};

static void editor_insert_char(int c, bool manual_typing)
{
    unsigned char byte = (unsigned char) c;

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
    size_t new_content_len = (size_t)row->size + (size_t)utf8_buffer.len;
    size_t alloc_len = manual_typing ? row_manual_alloc_size(new_content_len)
                                     : (new_content_len + 1);
    char *nc = realloc(row->chars, alloc_len);
    if (!nc) {
        utf8_buffer.len = 0;
        utf8_buffer.expected = 0;
        return;
    }
    row->chars = nc;
    memmove(&row->chars[ec.cursor_x + utf8_buffer.len],
            &row->chars[ec.cursor_x],
            row->size - ec.cursor_x + 1);
    memcpy(&row->chars[ec.cursor_x], utf8_buffer.bytes, utf8_buffer.len);
    row->size += utf8_buffer.len;
    row_update(row, ec.cursor_y);
    ec.cursor_x += utf8_buffer.len;
    ec.modified = true;
    if (!undo_is_replaying() && !undo_is_batching()) {
        undo_record_insert(before_y, before_x, utf8_buffer.bytes, (size_t)utf8_buffer.len,
                           before_y, before_x, ec.cursor_y, ec.cursor_x);
    }

    /* Reset UTF-8 buffer */
    utf8_buffer.len = 0;
    utf8_buffer.expected = 0;
}

static void editor_delete_char(void)
{
    if (ec.cursor_y == NR)
        return;
    if (ec.cursor_x == 0 && ec.cursor_y == 0)
        return;

    editor_row_t *row = ROW(ec.cursor_y);
    int before_y = ec.cursor_y, before_x = ec.cursor_x;

    if (ec.cursor_x > 0) {
        /* Delete character before cursor */
        const char *prev = utf8_prev_char(row->chars, row->chars + ec.cursor_x);
        int prev_pos = prev - row->chars, char_len = ec.cursor_x - prev_pos;
        char *deleted = xstrndup0(&row->chars[prev_pos], (size_t)char_len);

        memmove(&row->chars[prev_pos], &row->chars[ec.cursor_x],
                row->size - ec.cursor_x + 1);
        row->size -= char_len;
        row_update(row, ec.cursor_y);
        ec.cursor_x = prev_pos;
        ec.modified = true;
        if (deleted) {
            undo_record_delete(before_y, prev_pos, deleted, (size_t)char_len,
                               before_y, before_x, ec.cursor_y, ec.cursor_x);
            free(deleted);
        }
    } else {
        /* Delete newline - join with previous line */
        if (ec.cursor_y > 0) {
            int prev_size = ROW(ec.cursor_y - 1)->size;
            ec.cursor_x = prev_size;
            editor_row_t *prev_row = ROW(ec.cursor_y - 1);
            char *new_chars = realloc(prev_row->chars,
                                      prev_row->size + row->size + 1);
            if (!new_chars)
                return;
            prev_row->chars = new_chars;
            memcpy(&prev_row->chars[prev_row->size], row->chars, row->size);
            prev_row->size += row->size;
            prev_row->chars[prev_row->size] = '\0';
            row_update(prev_row, ec.cursor_y - 1);
            row_erase(ec.cursor_y);
            ec.cursor_y--;
            ec.modified = true;
            if (!undo_is_replaying() && !undo_is_batching()) {
                static const char nl[] = "\n";
                undo_record_delete(before_y - 1, prev_size, nl, 1,
                                   before_y, before_x, ec.cursor_y, ec.cursor_x);
            }
        }
    }
}

static char *file_rows_to_string(int *buf_len)
{
    int total_len = 0;
    for (int j = 0; j < NR; j++)
        total_len += ROW(j)->size + 1;
    *buf_len = total_len;
    char *buf = malloc(total_len), *p = buf;
    for (int j = 0; j < NR; j++) {
        memcpy(p, ROW(j)->chars, ROW(j)->size);
        p += ROW(j)->size;
        *p++ = '\n';
    }
    return buf;
}

static void file_open(const char *file_name)
{
    undo_reset_history();
    for (int i = 0; i < NR; i++)
        row_free_contents(ROW(i));
    tlist_free_items(ec.rows);

    /* Reset cursor and scroll position */
    ec.cursor_x = 0;
    ec.cursor_y = 0;
    ec.row_offset = 0;
    ec.col_offset = 0;
    ec.render_x = 0;

    free(ec.file_name);
    ec.file_name = strdup(file_name);
    syntax_select();
    FILE *file = fopen(file_name, "r+");
    if (!file) {
        if (errno != ENOENT)
            panic("Failed to open the file");
        ec.modified = false;
        return;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, file)) != -1) {
        if (line_len > 0 &&
            (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        row_insert(NR, line, line_len);
    }
    free(line);
    fclose(file);
    ec.modified = false;
}

static void file_save(void)
{
    if (!ec.file_name) {
        ec.file_name = ui_prompt("Save as: %s (^C to cancel)", NULL);
        if (!ec.file_name) {
            ui_set_message("Save aborted");
            return;
        }
        syntax_select();
    }
    int len;
    char *buf = file_rows_to_string(&len);
    int fd = open(ec.file_name, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if ((ftruncate(fd, len) != -1) && (write(fd, buf, len) == len)) {
            close(fd);
            free(buf);
            ec.modified = false;
            if (len >= 1024)
                ui_set_message("%d KiB written to disk", len >> 10);
            else
                ui_set_message("%d B written to disk", len);
            return;
        }
        close(fd);
    }
    free(buf);
    ui_set_message("Error: %s", strerror(errno));
}

/* Highlight the match at (row_idx, match_offset, match_len), saving the
 * previous highlight so it can be restored when leaving search mode. */
static void search_highlight_match(int row_idx, int match_offset, int match_len)
{
    editor_row_t *r = ROW(row_idx);
    if (!r->highlight || r->render_size <= 0)
        return;

    /* Restore previous highlight if we've moved to a different row */
    if (ec.mode_state.search.saved_highlight &&
        ec.mode_state.search.highlight_line != row_idx) {
        int prev = ec.mode_state.search.highlight_line;
        if (prev >= 0 && prev < NR && ROW(prev)->highlight) {
            memcpy(ROW(prev)->highlight, ec.mode_state.search.saved_highlight,
                   ROW(prev)->render_size);
        }
        free(ec.mode_state.search.saved_highlight);
        ec.mode_state.search.saved_highlight = NULL;
        ec.mode_state.search.highlight_line = -1;
    }

    /* Save current row's highlight if not already saved */
    if (!ec.mode_state.search.saved_highlight) {
        ec.mode_state.search.saved_highlight = malloc(r->render_size);
        if (!ec.mode_state.search.saved_highlight)
            return; /* Can't save highlight; skip marking to avoid inconsistency */
        memcpy(ec.mode_state.search.saved_highlight, r->highlight,
               r->render_size);
        ec.mode_state.search.highlight_line = row_idx;
    }

    /* Bounds check: only mark if match fits within the render buffer */
    if (match_offset + match_len <= r->render_size)
        memset(&r->highlight[match_offset], MATCH, match_len);
    /* else: match extends past render_size; skip highlight silently */
}

/* Info about the last successful match, in chars space.  Populated by
 * search_do_from() so that the replace code knows where to cut. */
typedef struct {
    int row;       /* row index */
    int char_off;  /* byte offset in row->chars */
    int char_len;  /* byte length of match */
} search_match_t;
static search_match_t g_last_match = { .row = -1, .char_off = 0, .char_len = 0 };

/* Find the LAST match of query in r->chars[0..max_off].
 * Because libc provides only "find first", we scan forward collecting every
 * hit and keep the rightmost one that is still at or before max_off.
 * Works for both plain-text and regex; pass a compiled re for regex mode.
 * Returns true and fills *out_off / *out_len on success. */
static bool row_find_last_match(editor_row_t *r, int max_off,
                                bool case_sens,
                                const char *query, size_t qlen, regex_t *re,
                                int *out_off, int *out_len)
{
    if (max_off < 0) return false;
    bool found = false;
    const char *hay = r->chars;
    const char *p = hay;
    while (p <= hay + max_off) {
        int mo, ml;
        if (re) {
            regmatch_t mm;
            if (regexec(re, p, 1, &mm, 0) != 0) break;
            int abs_so = (int)(p - hay) + (int)mm.rm_so;
            if (abs_so > max_off) break;
            mo = abs_so;
            ml = (int)(mm.rm_eo - mm.rm_so);
            p = hay + abs_so + 1;
            if ((int)(p - hay) > r->size) break;
        } else {
            const char *hit = case_sens ? strstr(p, query) : strcasestr(p, query);
            if (!hit) break;
            int abs_so = (int)(hit - hay);
            if (abs_so > max_off) break;
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
static bool row_find_first_match(editor_row_t *r, int min_off,
                                 bool case_sens,
                                 const char *query, size_t qlen, regex_t *re,
                                 int *out_off, int *out_len)
{
    if (min_off > r->size) return false;
    if (re) {
        regmatch_t m;
        if (regexec(re, r->chars + min_off, 1, &m, 0) != 0) return false;
        *out_off = min_off + (int)m.rm_so;
        *out_len = (int)(m.rm_eo - m.rm_so);
    } else {
        const char *src = r->chars + min_off;
        const char *hit = case_sens ? strstr(src, query) : strcasestr(src, query);
        if (!hit) return false;
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
static void set_overlay_msg(const char *fmt, ...); /* forward declaration */
static bool search_do_from(const char *query, int start_row, int start_char_off,
                           bool no_wrap, bool *out_wrapped)
{
    if (out_wrapped) *out_wrapped = false;
    if (!query || !*query) return false;
    int n = NR;
    if (n == 0) return false;
    bool backwards = (ec.search.mode & SM_BACKWARDS)     != 0;
    bool use_regex  = (ec.search.mode & SM_REGEX)         != 0;
    bool case_sens  = (ec.search.mode & SM_CASE_SENSITIVE) != 0;

    regex_t re;
    if (use_regex) {
        int flags = REG_EXTENDED | (case_sens ? 0 : REG_ICASE);
        if (regcomp(&re, query, flags) != 0) return false;
    }

    size_t qlen = use_regex ? 0 : strlen(query);
    int dir = backwards ? -1 : 1;
    /* When no_wrap is true: scan only the rows reachable in the search direction
     * before hitting the file boundary. */
    int max_rows = no_wrap ? (backwards ? start_row + 1 : n - start_row) : n;
    bool found = false;

    for (int i = 0; i < max_rows && !found; i++) {
        int ri = no_wrap ? start_row + i * dir
                         : ((start_row + i * dir) % n + n) % n;
        editor_row_t *r = ROW(ri);
        if (!r->chars) continue;

        /* For the starting row honour start_char_off; for subsequent rows
         * scan from the appropriate end based on direction. */
        int search_off = (i == 0) ? start_char_off
                                   : (backwards ? r->size : 0);

        int match_off = -1, match_len = -1;
        bool hit = backwards
            ? row_find_last_match(r, search_off, case_sens,
                                  query, qlen, use_regex ? &re : NULL, &match_off, &match_len)
            : row_find_first_match(r, search_off, case_sens,
                                   query, qlen, use_regex ? &re : NULL, &match_off, &match_len);

        if (hit) {
            ec.cursor_y = ri;
            ec.cursor_x = match_off;
            ec.row_offset = n; /* trigger scroll recalc */
            g_last_match = (search_match_t){ ri, match_off, match_len };
            int render_off = row_cursorx_to_renderx(r, match_off);
            int render_end = row_cursorx_to_renderx(r, match_off + match_len);
            int render_len = render_end - render_off;
            if (render_len <= 0 && match_len > 0) render_len = 1;
            if (render_len > 0)
                search_highlight_match(ri, render_off, render_len);
            /* Detect wrap: for forward search ri wrapped if ri < start_row (after i>0);
             * for backward search ri wrapped if ri > start_row (after i>0). */
            if (out_wrapped && i > 0)
                *out_wrapped = backwards ? (ri > start_row) : (ri < start_row);
            found = true;
        }
    }

    if (use_regex) regfree(&re);
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
    if (!query || !*query || NR == 0) return false;
    bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
    bool wrapped = false;
    bool found;
    if (backwards)
        found = search_do_from(query, ec.cursor_y, ec.cursor_x - 1, false, &wrapped);
    else
        found = search_do_from(query, ec.cursor_y, ec.cursor_x + 1, false, &wrapped);
    if (found && wrapped)
        set_overlay_msg("[ search wrapped ]");
    return found;
}

/* Perform one text replacement at g_last_match. */
static bool do_replace_one(const char *replacement, size_t repl_len)
{
    if (g_last_match.row < 0 || g_last_match.row >= NR)
        return false;
    editor_row_t *row = ROW(g_last_match.row);
    if (!row)
        return false;
    int off = g_last_match.char_off;
    int oldlen = g_last_match.char_len;
    int before_y = ec.cursor_y, before_x = ec.cursor_x;
    char *old_text = NULL;
    old_text = xstrndup0(&row->chars[off], (size_t)oldlen);
    if (!old_text)
        goto out;
    size_t new_size = (size_t) row->size - (size_t) oldlen + repl_len;
    if (new_size > INT_MAX)
        goto out;
    char *nc = realloc(row->chars, new_size + 1);
    if (!nc)
        goto out;
    row->chars = nc;
    memmove(&row->chars[off + repl_len],
            &row->chars[off + oldlen],
            row->size - off - oldlen + 1);
    if (repl_len > 0)
        memcpy(&row->chars[off], replacement, repl_len);
    row->size = (int)new_size;
    row->chars[row->size] = '\0';
    row_update(row, g_last_match.row);
    ec.modified = true;
    undo_record_replace(g_last_match.row, off, old_text, (size_t)oldlen,
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
    vsnprintf(ec.notfound_msg, sizeof(ec.notfound_msg), fmt, ap);
    va_end(ap);
}

/* Returns true if, after wrapping, the current cursor position has gone at or
 * past the original replace-start position (completing exactly one full cycle).
 * Uses ec.search.has_wrapped rather than a per-call flag so wrap is
 * remembered across multiple search_do_from calls in the same session. */
static bool replace_past_origin(bool backwards)
{
    if (!ec.search.has_wrapped || ec.search.orig_row < 0) return false;
    int or = ec.search.orig_row, oc = ec.search.orig_char;
    return backwards ? (ec.cursor_y < or || (ec.cursor_y == or && ec.cursor_x <= oc))
                     : (ec.cursor_y > or || (ec.cursor_y == or && ec.cursor_x >= oc));
}

/* Replace the current g_last_match and advance to the next occurrence,
 * wrapping with a stop at the original replace-start position to avoid
 * cycling past the starting point.  Returns true if another match was found. */
static bool replace_and_advance(const char *rq, size_t rqlen)
{
    bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
    int prev_off   = g_last_match.char_off;
    int prev_len   = g_last_match.char_len;
    do_replace_one(rq, rqlen);
    ec.search.replace_count++;
    /* Advance past the replaced range so we don't re-match it. */
    int skip_off;
    if (backwards) {
        skip_off = prev_off - 1; /* look strictly before the replaced position */
    } else {
        int advance = (int)rqlen > prev_len ? (int)rqlen : prev_len;
        /* Guard against zero: rqlen==0 (empty replacement) and prev_len==0
         * (zero-width regex match like ^) would leave skip_off==prev_off
         * and cause an infinite loop. */
        if (advance <= 0) advance = 1;
        skip_off = prev_off + advance;
    }
    bool wrapped = false;
    bool found = search_do_from(ec.search.query, ec.cursor_y, skip_off, false, &wrapped);
    if (wrapped) ec.search.has_wrapped = true;
    if (!found) return false;
    return !replace_past_origin(backwards);
}

/* Skip the current match without replacing and advance to the next occurrence. */
static bool replace_skip(void)
{
    bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
    int skip_off = backwards ? g_last_match.char_off - 1
                             : g_last_match.char_off + 1;
    bool wrapped = false;
    bool found = search_do_from(ec.search.query, ec.cursor_y, skip_off, false, &wrapped);
    if (wrapped) ec.search.has_wrapped = true;
    if (!found) return false;
    return !replace_past_origin(backwards);
}

static void replace_finish(bool always_show_msg);

static void replace_confirm_step(void)
{
    const char *rq = ec.search.replace_query ? ec.search.replace_query : "";
    size_t rqlen = ec.search.replace_len;
    char *const options[] = {"Yes", "No", "All", NULL};
    int r = ui_dialog_ask("Replace this instance?", options);
    if (r == 0) {
        if (!replace_and_advance(rq, rqlen))
            replace_finish(true);
    } else if (r == 1) {
        if (!replace_skip())
            replace_finish(false);
    } else if (r == 2) {
        while (replace_and_advance(rq, rqlen))
            ;
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
        set_overlay_msg("[ Replaced %d occurrence%s ]", cnt, cnt == 1 ? "" : "s");
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
    editor_refresh(); /* Show the [SEARCH] statusbar immediately */
}

static void buf_append(editor_buf_t *eb, const char *s, int len)
{
    char *new = realloc(eb->buf, eb->len + len);
    if (!new)
        return;
    memcpy(&new[eb->len], s, len);
    eb->buf = new;
    eb->len += len;
}

static void buf_destroy(editor_buf_t *eb)
{
    free(eb->buf);
}

/* Append the overlay message to an editor_buf_t (called at end of refresh). */
static void buf_append_overlay(editor_buf_t *eb)
{
    if (!ec.notfound_msg[0])
        return;
    int msglen = (int)strlen(ec.notfound_msg);
    int col = (ec.screen_cols - msglen) / 2;
    if (col < 0) col = 0;
    char posbuf[32];
    snprintf(posbuf, sizeof(posbuf), "\x1b[%d;%dH\x1b[7m", ec.screen_rows, col + 1);
    buf_append(eb, posbuf, strlen(posbuf));
    buf_append(eb, ec.notfound_msg, msglen);
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
    return width + 2; /* Add space for padding and separator */
}

static void editor_scroll(void)
{
    ec.render_x = 0;
    if (ec.cursor_y < NR)
        ec.render_x = row_cursorx_to_renderx(ROW(ec.cursor_y), ec.cursor_x);
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

static void ui_draw_statusbar(editor_buf_t *eb)
{
    buf_append(eb, "\x1b[100m", 6); /* Dark gray */
    char status[80], r_status[80];

    int len, r_len;
    if (ec.mode == MODE_SEARCH) {
        if (ec.search.replace_phase == 1) {
            /* Phase 1: entering replacement text */
            const char *rq = ec.search.replace_query ? ec.search.replace_query : "";
            len = snprintf(status, sizeof(status), " Replace with: %s", rq);
        } else {
            /* Phase 0 and 2: show [SEARCH] / [REPLACE] with active flags and query */
            const char *q = ec.search.query ? ec.search.query : "";
            char flags[60];
            int flen = 0;
            const char *mode_label = (ec.search.mode & SM_REPLACE) ? "REPLACE" : "SEARCH";
            if (ec.search.mode & SM_CASE_SENSITIVE)
                flen += snprintf(flags + flen, sizeof(flags) - flen, " [Case Sensitive]");
            if (ec.search.mode & SM_BACKWARDS)
                flen += snprintf(flags + flen, sizeof(flags) - flen, " [Backwards]");
            if (ec.search.mode & SM_REGEX)
                flen += snprintf(flags + flen, sizeof(flags) - flen, " [Regex]");
            if (flen == 0) flags[0] = '\0';
            len = snprintf(status, sizeof(status), " [%s]%s %s", mode_label, flags, q);
        }
        r_len = 0;
        r_status[0] = '\0';
    } else {
        /* Include mode in status line */
        const char *mode_name = mode_get_name(ec.mode);
        len = snprintf(status, sizeof(status), " [%s] File: %.20s %s",
                       mode_name, ec.file_name ? ec.file_name : "< New >",
                       ec.modified ? "(modified)" : "");
        int col_size = (ec.cursor_y <= NR - 1) ? ROW(ec.cursor_y)->size : 0;
        r_len = snprintf(
            r_status, sizeof(r_status), "%d/%d lines  %d/%d cols",
            (ec.cursor_y + 1 > NR) ? NR : ec.cursor_y + 1,
            NR, ec.cursor_x + 1, col_size);
    }

    if (len > ec.screen_cols)
        len = ec.screen_cols;
    buf_append(eb, status, len);
    while (len < ec.screen_cols) {
        if (r_len > 0 && ec.screen_cols - len == r_len) {
            buf_append(eb, r_status, r_len);
            break;
        }
        buf_append(eb, " ", 1);
        len++;
    }
    buf_append(eb, "\x1b[m", 3);
    buf_append(eb, "\r\n", 2);
}

/*
 * Advance p past a CSI escape sequence (already past the opening ESC).
 * On entry p must point at the '[' of "ESC [".  Skips parameter bytes
 * (0x30–0x3F), intermediate bytes (0x20–0x2F) and the final byte (0x40–0x7E).
 */
static const char *skip_csi(const char *p)
{
    p++; /* skip '[' */
    /* Skip parameter and intermediate bytes (all bytes before the final) */
    while (*p && (unsigned char)*p < 0x40)
        p++;
    /* Skip the final byte (0x40–0x7E) */
    if (*p && (unsigned char)*p <= 0x7e)
        p++;
    return p;
}

/*
 * Scan string s counting visible (non-escape-sequence) characters.
 * Stops after max_visible visible characters; if max_visible < 0, scans to
 * the end of the string.  Returns the byte offset reached.  If vis_out is
 * non-NULL it receives the visible character count seen.
 */
static int str_visible_scan(const char *s, int max_visible, int *vis_out)
{
    const char *p = s;
    int vis = 0;
    while (*p) {
        if ((unsigned char)*p == '\x1b' && *(p + 1) == '[') {
            p = skip_csi(p + 1);
        } else {
            if (max_visible >= 0 && vis >= max_visible)
                break;
            vis++;
            p++;
        }
    }
    if (vis_out)
        *vis_out = vis;
    return (int)(p - s);
}

static void ui_draw_messagebar(editor_buf_t *eb)
{
    buf_append(eb, "\x1b[93m\x1b[44m\x1b[K", 13);
    int displayed_len = 0;

    if (ec.mode == MODE_SEARCH) {
        const char *hint;
        if (ec.search.replace_phase == 2) {
            /* Confirm-each phase draws via ui_dialog_ask() status text. */
            hint = ec.status_msg[0] ? ec.status_msg
                                    : "Replace this instance?  [ Yes ]  [ No ]  [ All ]  ^C:Cancel";
        } else if (ec.search.replace_phase == 1) {
            /* Entering replacement text */
            hint = "Enter replacement text, then press Enter  ^C:Cancel";
        } else {
            /* Phase 0: show search keybinding hints */
            hint = "M-C:Case Sens  M-B:Backwards  M-R:Regexp  ^R:Replace";
        }
        int vlen;
        int blen = str_visible_scan(hint, ec.screen_cols, &vlen);
        buf_append(eb, hint, blen);
        displayed_len = vlen;
    } else if (strstr(ec.status_msg, "File Browser:")) {
        int vlen;
        int blen = str_visible_scan(ec.status_msg, ec.screen_cols, &vlen);
        buf_append(eb, ec.status_msg, blen);
        displayed_len = vlen;
    } else {
        /* Regular messages: truncate to screen width and show for 5 seconds */
        int vlen;
        int blen = str_visible_scan(ec.status_msg, ec.screen_cols, &vlen);
        if (blen && time(NULL) - ec.status_msg_time < 5) {
            buf_append(eb, ec.status_msg, blen);
            displayed_len = vlen;
        }
    }

    /* Pad the rest of the line with spaces */
    while (displayed_len < ec.screen_cols) {
        buf_append(eb, " ", 1);
        displayed_len++;
    }

    buf_append(eb, "\x1b[0m", 4);
}

static void ui_set_message(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vsnprintf(ec.status_msg, sizeof(ec.status_msg), msg, args);
    va_end(args);
    ec.status_msg_time = time(NULL);
}

static void ui_draw_rows(editor_buf_t *eb)
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
                int num_len = snprintf(line_num, sizeof(line_num), "%*d ",
                                       line_num_width - 1, file_row + 1);
                buf_append(eb, "\x1b[90m", 5); /* Dark gray color */
                buf_append(eb, line_num, num_len);
                buf_append(eb, "\x1b[0m", 4); /* Reset color */
            } else {
                /* Draw empty space for consistency */
                for (int i = 0; i < line_num_width; i++)
                    buf_append(eb, " ", 1);
            }
        }

        if (file_row >= NR) {
            buf_append(eb, "~", 1);
        } else {
            int available_cols = ec.screen_cols - line_num_width;
            int len = ROW(file_row)->render_size - ec.col_offset;
            if (len < 0)
                len = 0;
            if (len > available_cols)
                len = available_cols;
            char *c = ROW(file_row)->render + ec.col_offset;
            unsigned char *hl = ROW(file_row)->highlight + ec.col_offset;
            int current_color = -1;
            bool in_selection = false;

            for (int j = 0; j < len; j++) {
                /* Check if this character is in selection */
                int cursor_x = row_renderx_to_cursorx(ROW(file_row),
                                                      ec.col_offset + j);
                bool is_selected = selection_contains(cursor_x, file_row);

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

                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    buf_append(eb, "\x1b[7m", 4);
                    buf_append(eb, &sym, 1);
                    buf_append(eb, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int c_len = snprintf(buf, sizeof(buf), "\x1b[%dm",
                                             current_color);
                        buf_append(eb, buf, c_len);
                    }
                } else if (hl[j] == NORMAL) {
                    if (current_color != -1) {
                        buf_append(eb, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    buf_append(eb, &c[j], 1);
                } else {
                    int color = syntax_token_color(hl[j]);
                    if (hl[j] == MATCH) {
                        /* Use inverse video for search matches */
                        buf_append(eb, "\x1b[7m", 4);
                        buf_append(eb, &c[j], 1);
                        buf_append(eb, "\x1b[27m", 5);
                        if (current_color != -1) {
                            char buf[16];
                            int c_len = snprintf(buf, sizeof(buf), "\x1b[%dm",
                                                 current_color);
                            buf_append(eb, buf, c_len);
                        }
                    } else {
                        if (color != current_color) {
                            current_color = color;
                            char buf[16];
                            int c_len =
                                snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                            buf_append(eb, buf, c_len);
                        }
                        buf_append(eb, &c[j], 1);
                    }
                }
            }
            /* Ensure selection highlighting is turned off at end of line */
            if (in_selection)
                buf_append(eb, "\x1b[27m", 5);
            buf_append(eb, "\x1b[39m", 5);
        }
        buf_append(eb, "\x1b[K", 3);
        buf_append(eb, "\r\n", 2);
    }
}

static void editor_refresh(void)
{
    editor_scroll();
    editor_buf_t eb = {NULL, 0};
    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[H", 3);
    ui_draw_rows(&eb);
    ui_draw_statusbar(&eb);
    ui_draw_messagebar(&eb);
    buf_append_overlay(&eb); /* transient notfound/replaced overlay */

    /* Adjust cursor position for line numbers */
    int line_num_width = get_line_number_width();
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1,
             (ec.render_x - ec.col_offset) + 1 + line_num_width);
    buf_append(&eb, buf, strlen(buf));
    buf_append(&eb, "\x1b[?25h", 6);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

/* Force full screen refresh by clearing first */
static void editor_refresh_full(void)
{
    editor_scroll();
    editor_buf_t eb = {NULL, 0};
    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[2J", 4); /* Clear entire screen */
    buf_append(&eb, "\x1b[H", 3);
    ui_draw_rows(&eb);
    ui_draw_statusbar(&eb);
    ui_draw_messagebar(&eb);
    buf_append_overlay(&eb); /* transient notfound/replaced overlay */

    /* Adjust cursor position for line numbers */
    int line_num_width = get_line_number_width();
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1,
             (ec.render_x - ec.col_offset) + 1 + line_num_width);
    buf_append(&eb, buf, strlen(buf));
    buf_append(&eb, "\x1b[?25h", 6);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

static void sig_winch_handler(int sig)
{
    (void) sig; /* Unused parameter */
    term_update_size();
    if (ec.cursor_y > ec.screen_rows)
        ec.cursor_y = ec.screen_rows - 1;
    if (ec.cursor_x > ec.screen_cols)
        ec.cursor_x = ec.screen_cols - 1;
    editor_refresh();
}

static void sig_cont_handler(int sig)
{
    (void) sig; /* Unused parameter */
    term_disable_raw();
    term_open_buffer();
    term_enable_raw();
    editor_refresh();
}

/* Ask a question with selectable options.
 * Returns selected option index, or -1 on cancel (^C/^X/ESC). */
static int ui_dialog_ask(const char *msg, char *const options[])
{
    int n = 0;
    while (options[n]) n++;
    if (n <= 0) return -1;
    int choice = 0;

    while (1) {
        char status_msg[512];
        int off = snprintf(status_msg, sizeof(status_msg), "%s  ", msg);
        for (int i = 0; i < n; i++) {
            if (off < 0 || off >= (int)sizeof(status_msg))
                break;
            off += snprintf(status_msg + off, sizeof(status_msg) - (size_t)off,
                            (i == choice) ? "\x1b[7m[ %s ]\x1b[m" : "[ %s ]", options[i]);
            if (i + 1 < n) {
                if (off < 0 || off >= (int)sizeof(status_msg))
                    break;
                off += snprintf(status_msg + off, sizeof(status_msg) - (size_t)off, "  ");
            }
        }
        if (off > -1 && off < (int)sizeof(status_msg))
            snprintf(status_msg + off, sizeof(status_msg) - (size_t)off, "  ^C:Cancel");

        ui_set_message("%s", status_msg);
        editor_refresh();

        int c = term_read_key();
        switch (c) {
        case '\r': /* Enter key */
            ui_set_message("");
            return choice;
        case CTRL_('c'): /* ^C - cancel */
        case CTRL_('x'):
        case '\x1b':
            ui_set_message("");
            return -1; /* Cancel */
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
                    ui_set_message("");
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
    char *const options[] = {"No", "Yes", NULL};
    int r = ui_dialog_ask(msg, options);
    if (r < 0)
        return -1;
    return r;
}

static char *ui_prompt(const char *msg, void (*callback)(char *, int))
{
    size_t buf_size = 128;
    char *buf = malloc(buf_size);
    if (!buf)
        return NULL;
    size_t buf_len = 0;
    buf[0] = '\0';

    while (1) {
        char formatted_msg[256];
        snprintf(formatted_msg, sizeof(formatted_msg), msg, buf);
        ui_set_message("%s", formatted_msg);
        editor_refresh();
        int c = term_read_key();
        if ((c == DEL_KEY) || (c == CTRL_('h')) || (c == BACKSPACE)) {
            if (buf_len != 0)
                buf[--buf_len] = '\0';
        } else if (c == CTRL_('c')) {
            ui_set_message("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                ui_set_message("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && isprint(c)) {
            if (buf_len == buf_size - 1) {
                buf_size *= 2;
                /* In case realloc fails */
                char *new_buf = realloc(buf, buf_size);
                if (NULL == new_buf) {
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

static void editor_move_cursor(int key)
{
    editor_row_t *row =
        (ec.cursor_y >= NR) ? NULL : ROW(ec.cursor_y);
    switch (key) {
    case ARROW_LEFT:
        if (ec.cursor_x != 0) {
            /* Move to previous UTF-8 character boundary */
            if (row) {
                const char *prev =
                    utf8_prev_char(row->chars, row->chars + ec.cursor_x);
                ec.cursor_x = prev - row->chars;
            } else {
                ec.cursor_x--;
            }
        } else if (ec.cursor_y > 0) {
            ec.cursor_y--;
            ec.cursor_x = ROW(ec.cursor_y)->size;
        }
        break;
    case ARROW_RIGHT:
        if (row && ec.cursor_x < row->size) {
            /* Move to next UTF-8 character boundary */
            const char *next = utf8_next_char(row->chars + ec.cursor_x);
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
static const char *get_file_type_info(const char *filename, int *color)
{
    if (filename[0] == '/') {
        *color = 34; /* Blue for directories */
        return "[DIR]  ";
    }

    const char *ext = get_file_extension(filename);

    /* Source and script files */
    if (!strcasecmp(ext, "c") || !strcasecmp(ext, "h") ||
        !strcasecmp(ext, "cpp") || !strcasecmp(ext, "cxx") ||
        !strcasecmp(ext, "hpp") || !strcasecmp(ext, "cc") ||
        !strcasecmp(ext, "sh") || !strcasecmp(ext, "py") ||
        !strcasecmp(ext, "rb") || !strcasecmp(ext, "js") ||
        !strcasecmp(ext, "rs") || !strcasecmp(ext, "go") ||
        !strcasecmp(ext, "java") || !strcasecmp(ext, "php") ||
        !strcasecmp(ext, "pl") || !strcasecmp(ext, "lua") ||
        !strcasecmp(ext, "vim") || !strcasecmp(ext, "asm") ||
        !strcasecmp(ext, "s")) {
        *color = 32; /* Green for source */
        return "[SRC]  ";
    }

    /* All other files */
    *color = 37; /* White for others */
    return "[FILE] ";
}

static void browser_free_entries(void)
{
    if (ec.mode_state.browser.entries) {
        for (int i = 0; i < ec.mode_state.browser.num_entries; i++)
            free(ec.mode_state.browser.entries[i]);
        free(ec.mode_state.browser.entries);
        ec.mode_state.browser.entries = NULL;
        ec.mode_state.browser.num_entries = 0;
    }
    free(ec.mode_state.browser.current_dir);
    ec.mode_state.browser.current_dir = NULL;
}

static int browser_compare_entries(const void *a, const void *b)
{
    const char *name_a = *(const char **) a, *name_b = *(const char **) b;

    /* Directories first (start with '/'), then files */
    bool is_dir_a = (name_a[0] == '/');
    bool is_dir_b = (name_b[0] == '/');

    if (is_dir_a && !is_dir_b)
        return -1;
    if (!is_dir_a && is_dir_b)
        return 1;

    /* Compare names, ignoring the '/' prefix for directories */
    const char *cmp_a = is_dir_a ? name_a + 1 : name_a;
    const char *cmp_b = is_dir_b ? name_b + 1 : name_b;

    return strcasecmp(cmp_a, cmp_b);
}

static void browser_load_directory(const char *path)
{
    browser_free_entries();

    DIR *dir = opendir(path ? path : ".");
    if (!dir) {
        ui_set_message("Cannot open directory: %s", strerror(errno));
        mode_set(MODE_NORMAL);
        return;
    }

    /* Store current directory */
    ec.mode_state.browser.current_dir = strdup(path ? path : ".");

    /* Count entries first */
    int capacity = 32;
    ec.mode_state.browser.entries = malloc(sizeof(char *) * capacity);
    ec.mode_state.browser.num_entries = 0;

    /* Add parent directory if not root */
    if (strcmp(ec.mode_state.browser.current_dir, "/")) {
        ec.mode_state.browser.entries[ec.mode_state.browser.num_entries++] =
            strdup("/..");
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Skip current and parent directory entries */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        /* Skip hidden files if show_hidden is false */
        if (!ec.mode_state.browser.show_hidden && de->d_name[0] == '.')
            continue;

        /* Check if we need to resize array */
        if (ec.mode_state.browser.num_entries >= capacity - 1) {
            capacity *= 2;
            ec.mode_state.browser.entries = realloc(
                ec.mode_state.browser.entries, sizeof(char *) * capacity);
        }

        /* Get file info to determine if it's a directory */
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 ec.mode_state.browser.current_dir, de->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                /* Directory - prefix with '/' */
                char *entry = malloc(strlen(de->d_name) + 2);
                sprintf(entry, "/%s", de->d_name);
                ec.mode_state.browser
                    .entries[ec.mode_state.browser.num_entries++] = entry;
            } else if (S_ISREG(st.st_mode)) {
                /* Regular file */
                ec.mode_state.browser
                    .entries[ec.mode_state.browser.num_entries++] =
                    strdup(de->d_name);
            }
        }
    }

    closedir(dir);

    /* Sort entries: directories first, then files, both alphabetically */
    if (ec.mode_state.browser.num_entries > 0) {
        qsort(ec.mode_state.browser.entries, ec.mode_state.browser.num_entries,
              sizeof(char *), browser_compare_entries);
    }

    ec.mode_state.browser.selected = 0;
    ec.mode_state.browser.offset = 0;
}

typedef enum {
    SCROLL_LINE_OK,
    SCROLL_LINE_END,
} scroll_line_result_t;

typedef enum {
    SCROLL_EV_ENTER,
    SCROLL_EV_CANCEL,
} scroll_event_t;

typedef enum {
    SCROLL_ACT_CONTINUE,
    SCROLL_ACT_REPAINT,
    SCROLL_ACT_EXIT,
} scroll_action_t;

typedef struct scrollable_screen scrollable_screen_t;

typedef scroll_line_result_t (*scroll_line_cb)(scrollable_screen_t *ss, int idx,
                                               char *buf, int len);
typedef scroll_action_t (*scroll_event_cb)(scrollable_screen_t *ss,
                                           scroll_event_t ev, int idx);
typedef scroll_action_t (*scroll_key_cb)(scrollable_screen_t *ss, int key);

struct scrollable_screen {
    const char     *title;
    const char     *status_right;
    int             total_lines;
    int             offset;
    int             selected;
    bool            selectable;
    scroll_line_cb  get_line;
    scroll_event_cb on_event;
    scroll_key_cb   on_key;
    void           *ctx;
};

typedef struct {
    char *selected_file;
} browser_ctx_t;

static void scrollable_screen_clamp(scrollable_screen_t *ss, int visible)
{
    if (ss->total_lines < 0)
        ss->total_lines = 0;
    if (visible < 0)
        visible = 0;

    int max_offset = ss->total_lines - visible;
    if (max_offset < 0)
        max_offset = 0;
    if (ss->offset < 0)
        ss->offset = 0;
    if (ss->offset > max_offset)
        ss->offset = max_offset;

    if (!ss->selectable) {
        ss->selected = -1;
        return;
    }

    if (ss->total_lines <= 0) {
        ss->selected = -1;
        ss->offset = 0;
        return;
    }

    if (ss->selected < 0)
        ss->selected = 0;
    if (ss->selected >= ss->total_lines)
        ss->selected = ss->total_lines - 1;

    if (ss->selected < ss->offset)
        ss->offset = ss->selected;
    if (visible > 0 && ss->selected >= ss->offset + visible)
        ss->offset = ss->selected - visible + 1;

    max_offset = ss->total_lines - visible;
    if (max_offset < 0)
        max_offset = 0;
    if (ss->offset > max_offset)
        ss->offset = max_offset;
}

static void scrollable_screen_render(scrollable_screen_t *ss)
{
    editor_buf_t eb = {NULL, 0};
    char line[4096];
    int visible = ec.screen_rows - 1;
    if (visible < 0)
        visible = 0;

    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[H", 3);

    buf_append(&eb, "\x1b[7m", 4);
    int tlen = ss->title ? (int)strlen(ss->title) : 0;
    if (tlen > ec.screen_cols)
        tlen = ec.screen_cols;
    if (ss->title && tlen > 0)
        buf_append(&eb, ss->title, tlen);
    while (tlen < ec.screen_cols) {
        buf_append(&eb, " ", 1);
        tlen++;
    }
    buf_append(&eb, "\x1b[0m\r\n", 6);

    for (int i = 0; i < visible; i++) {
        int idx = ss->offset + i;
        buf_append(&eb, "\r\x1b[K", 4);
        scroll_line_result_t line_result = SCROLL_LINE_END;
        if (ss->get_line)
            line_result = ss->get_line(ss, idx, line, (int)sizeof(line));
        if (line_result == SCROLL_LINE_OK) {
            int line_len = (int)strlen(line);
            if (line_len > ec.screen_cols)
                line_len = ec.screen_cols;
            if (ss->selectable && idx == ss->selected)
                buf_append(&eb, "\x1b[7m", 4);
            buf_append(&eb, line, line_len);
            buf_append(&eb, "\x1b[0m", 4);
        } else {
            buf_append(&eb, "~", 1);
        }
        if (i < visible - 1)
            buf_append(&eb, "\r\n", 2);
    }

    buf_append(&eb, "\r\n", 2);
    buf_append(&eb, "\x1b[100m", 6);
    char status[256], r_status[128];
    int len = snprintf(status, sizeof(status), "%s", ss->title ? ss->title : "");
    int r_len = 0;
    if (ss->status_right && ss->status_right[0] != '\0')
        r_len = snprintf(r_status, sizeof(r_status), "%s", ss->status_right);
    if (len > ec.screen_cols)
        len = ec.screen_cols;
    buf_append(&eb, status, len);
    while (len < ec.screen_cols) {
        if (r_len > 0 && ec.screen_cols - len == r_len) {
            buf_append(&eb, r_status, r_len);
            break;
        }
        buf_append(&eb, " ", 1);
        len++;
    }
    buf_append(&eb, "\x1b[m\r\n", 5);
    ui_draw_messagebar(&eb);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

static void scrollable_screen_run(scrollable_screen_t *ss)
{
    while (1) {
        int visible = ec.screen_rows - 1;
        if (visible < 0)
            visible = 0;
        scrollable_screen_clamp(ss, visible);
        scrollable_screen_render(ss);

        int c = term_read_key();
        scroll_action_t act = SCROLL_ACT_CONTINUE;
        int max_offset = ss->total_lines - visible;
        if (max_offset < 0)
            max_offset = 0;

        switch (c) {
        case ARROW_UP:
            ss->offset--;
            if (ss->selectable)
                ss->selected--;
            break;
        case ARROW_DOWN:
            ss->offset++;
            if (ss->selectable)
                ss->selected++;
            break;
        case PAGE_UP:
            ss->offset -= visible;
            if (ss->selectable)
                ss->selected -= visible;
            break;
        case PAGE_DOWN:
            ss->offset += visible;
            if (ss->selectable)
                ss->selected += visible;
            break;
        case HOME_KEY:
            ss->offset = 0;
            if (ss->selectable)
                ss->selected = 0;
            break;
        case END_KEY:
            ss->offset = max_offset;
            if (ss->selectable)
                ss->selected = ss->total_lines - 1;
            break;
        case '\r':
            if (ss->selectable && ss->on_event)
                act = ss->on_event(ss, SCROLL_EV_ENTER, ss->selected);
            break;
        case CTRL_('c'):
        case CTRL_('x'):
        case '\x1b':
            if (ss->on_event)
                act = ss->on_event(ss, SCROLL_EV_CANCEL, -1);
            else
                act = SCROLL_ACT_EXIT;
            break;
        default:
            if (ss->on_key)
                act = ss->on_key(ss, c);
            break;
        }

        scrollable_screen_clamp(ss, visible);

        if (act == SCROLL_ACT_EXIT)
            return;
    }
}

static scroll_line_result_t help_get_line(scrollable_screen_t *ss, int idx,
                                          char *buf, int len)
{
    (void)ss;
    if (idx < 0 || idx >= HELP_NUM_LINES)
        return SCROLL_LINE_END;
    snprintf(buf, (size_t)len, "%s", help_lines[idx]);
    return SCROLL_LINE_OK;
}

static scroll_action_t help_on_event(scrollable_screen_t *ss, scroll_event_t ev,
                                     int idx)
{
    (void)ss;
    (void)idx;
    if (ev == SCROLL_EV_CANCEL)
        return SCROLL_ACT_EXIT;
    return SCROLL_ACT_CONTINUE;
}

static scroll_action_t help_key_cb(scrollable_screen_t *ss, int key)
{
    (void)ss;
    if (key == CTRL_('g'))
        return SCROLL_ACT_EXIT;
    return SCROLL_ACT_CONTINUE;
}

static void browser_update_labels(scrollable_screen_t *ss)
{
    static char title[PATH_MAX + 32];
    static char right[64];
    const char *dir = ec.mode_state.browser.current_dir ?
                      ec.mode_state.browser.current_dir : ".";
    snprintf(title, sizeof(title), " [BROWSER] %s", dir);
    int current = ss->selected >= 0 ? ss->selected + 1 : 0;
    snprintf(right, sizeof(right), "%d/%d files", current, ss->total_lines);
    ss->title = title;
    ss->status_right = right;
}

static scroll_line_result_t browser_get_line(scrollable_screen_t *ss, int idx,
                                             char *buf, int len)
{
    if (idx == ss->offset)
        browser_update_labels(ss);
    if (idx < 0 || idx >= ec.mode_state.browser.num_entries)
        return SCROLL_LINE_END;

    const char *entry = ec.mode_state.browser.entries[idx];
    int color = 37;
    const char *type_str = get_file_type_info(entry, &color);
    snprintf(buf, (size_t)len, "\x1b[%dm  %s%s\x1b[0m", color, type_str,
             (entry[0] == '/') ? entry + 1 : entry);
    return SCROLL_LINE_OK;
}

static scroll_action_t browser_on_event(scrollable_screen_t *ss,
                                        scroll_event_t ev, int idx)
{
    browser_ctx_t *ctx = ss->ctx;

    if (ev == SCROLL_EV_CANCEL) {
        browser_free_entries();
        mode_set(MODE_NORMAL);
        return SCROLL_ACT_EXIT;
    }

    if (ev != SCROLL_EV_ENTER || idx < 0 || idx >= ec.mode_state.browser.num_entries)
        return SCROLL_ACT_CONTINUE;

    char *entry = ec.mode_state.browser.entries[idx];
    if (entry[0] == '/') {
        char new_path[PATH_MAX];
        if (!strcmp(entry, "/..")) {
            const char *cur = ec.mode_state.browser.current_dir;
            if (!cur || !strcmp(cur, "/")) {
                snprintf(new_path, sizeof(new_path), "/");
            } else {
                snprintf(new_path, sizeof(new_path), "%s", cur);
                char *last_slash = strrchr(new_path, '/');
                if (!last_slash || last_slash == new_path)
                    snprintf(new_path, sizeof(new_path), "/");
                else
                    *last_slash = '\0';
            }
        } else {
            snprintf(new_path, sizeof(new_path), "%s%s",
                     ec.mode_state.browser.current_dir, entry);
        }
        browser_load_directory(new_path);
        if (ec.mode != MODE_BROWSER)
            return SCROLL_ACT_EXIT;
        ss->total_lines = ec.mode_state.browser.num_entries;
        ss->offset = 0;
        ss->selected = ss->total_lines > 0 ? 0 : -1;
        browser_update_labels(ss);
        return SCROLL_ACT_REPAINT;
    }

    if (ec.modified) {
        int r = ui_confirm("Current file has been modified. Save before "
                           "opening new file?");
        if (r == -1)
            return SCROLL_ACT_CONTINUE;
        if (r == 1)
            file_save();
    }

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s",
             ec.mode_state.browser.current_dir, entry);
    free(ctx->selected_file);
    ctx->selected_file = NULL;
    ctx->selected_file = strdup(full_path);
    if (!ctx->selected_file) {
        ui_set_message("Out of memory");
        return SCROLL_ACT_CONTINUE;
    }
    return SCROLL_ACT_EXIT;
}

static scroll_action_t browser_key_cb(scrollable_screen_t *ss, int key)
{
    if (key != 'h' && key != 'H')
        return SCROLL_ACT_CONTINUE;

    ec.mode_state.browser.show_hidden = !ec.mode_state.browser.show_hidden;
    const char *cur = ec.mode_state.browser.current_dir ?
                      ec.mode_state.browser.current_dir : ".";
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", cur);
    browser_load_directory(path);
    if (ec.mode != MODE_BROWSER)
        return SCROLL_ACT_EXIT;
    ss->total_lines = ec.mode_state.browser.num_entries;
    ss->offset = 0;
    ss->selected = ss->total_lines > 0 ? 0 : -1;
    browser_update_labels(ss);
    return SCROLL_ACT_REPAINT;
}

static void help_screen_open(void)
{
    scrollable_screen_t ss = {
        .title = "  Help",
        .status_right = NULL,
        .total_lines = HELP_NUM_LINES,
        .offset = ec.mode_state.help.offset,
        .selected = -1,
        .selectable = false,
        .get_line = help_get_line,
        .on_event = help_on_event,
        .on_key = help_key_cb,
        .ctx = NULL,
    };
    scrollable_screen_run(&ss);
    mode_restore();
    editor_refresh_full();
}

static void browser_screen_open(void)
{
    browser_ctx_t ctx = {NULL};
    scrollable_screen_t ss = {
        .title = " [BROWSER]",
        .status_right = NULL,
        .total_lines = ec.mode_state.browser.num_entries,
        .offset = 0,
        .selected = ec.mode_state.browser.num_entries > 0 ? 0 : -1,
        .selectable = true,
        .get_line = browser_get_line,
        .on_event = browser_on_event,
        .on_key = browser_key_cb,
        .ctx = &ctx,
    };
    browser_update_labels(&ss);
    scrollable_screen_run(&ss);
    if (ctx.selected_file) {
        mode_set(MODE_NORMAL);
        browser_free_entries();
        file_open(ctx.selected_file);
        ui_set_message("Opened: %s", ctx.selected_file);
        free(ctx.selected_file);
        editor_refresh_full();
    }
}

/* Clean up all allocated memory before exit */
static void editor_cleanup(void)
{
    undo_reset_history();
    for (int i = 0; i < NR; i++)
        row_free_contents(ROW(i));
    tlist_free(ec.rows);
    ec.rows = NULL;

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
    free(ec.mode_state.search.saved_highlight);
    ec.mode_state.search.saved_highlight = NULL;
    free(ec.mode_state.prompt.buffer);
    ec.mode_state.prompt.buffer = NULL;
    browser_free_entries();
}

static void editor_process_key(void)
{
    static int indent_level = 0;
    /* Clear the transient overlay message from the previous keypress */
    ec.notfound_msg[0] = '\0';
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

    /* Handle mode-specific keys first */
    switch (ec.mode) {
    case MODE_SELECT:
        switch (c) {
        case CTRL_('c'): /* ^C - abort selection */
            ec.selection.active = false;
            mode_set(MODE_NORMAL);
            ui_set_message("Mark cancelled");
            return;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            /* Update selection while moving */
            editor_move_cursor(c);
            /* Ensure selection doesn't go past last row */
            if (ec.cursor_y >= NR && NR > 0) {
                ec.cursor_y = NR - 1;
                ec.cursor_x = ROW(ec.cursor_y)->size;
            }
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case HOME_KEY:
            ec.cursor_x = 0;
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case END_KEY:
            if (ec.cursor_y < NR)
                ec.cursor_x = ROW(ec.cursor_y)->size;
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP)
                ec.cursor_y = ec.row_offset;
            else
                ec.cursor_y = ec.row_offset + ec.screen_rows - 1;
            int times = ec.screen_rows;
            while (times--)
                editor_move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        }
        case META_('6'): /* Copy marked text and exit marking */
            selection_copy();
            mode_set(MODE_NORMAL);
            ui_set_message("Copied marked text");
            return;
        case CTRL_('k'): /* Cut marked text and exit marking */
            selection_cut();
            mode_set(MODE_NORMAL);
            ui_set_message("Cut marked text");
            return;
        case CTRL_('u'): /* Paste over selection */
            selection_delete();
            editor_paste();
            mode_set(MODE_NORMAL);
            return;
        case DEL_KEY:
        case BACKSPACE:
            selection_delete();
            return;
        default:
            /* Exit selection mode for other keys */
            mode_set(MODE_NORMAL);
            break;
        }
        break;

    case MODE_SEARCH: {
        /* Phase 1: entering replacement text */
        if (ec.search.replace_phase == 1) {
            if (c == '\r') {
                /* Execute: find first match at or after the saved position (wrapping).
                 * Use search_do_from directly with saved_x so we include any match
                 * sitting exactly at the cursor rather than starting at saved_x+1. */
                int sx = ec.mode_state.search.saved_x;
                int sy = ec.mode_state.search.saved_y;
                bool backwards = (ec.search.mode & SM_BACKWARDS) != 0;
                bool found = search_do_from(ec.search.query, sy,
                                            backwards ? sx - 1 : sx, false, NULL);
                if (!found) {
                    ec.search.replace_phase = 0;
                    ec.search.replace_count = 0;
                    const char *q = ec.search.query ? ec.search.query : "";
                    mode_set(MODE_NORMAL);
                    set_overlay_msg("[ \"%.*s\" not found ]",
                                    (int)(sizeof(ec.notfound_msg) - 20), q);
                } else {
                    ec.search.replace_phase = 2;
                    /* Store original position so replace_and_advance stops after one cycle. */
                    ec.search.orig_row = sy;
                    ec.search.orig_char = sx;
                    ec.search.has_wrapped = false;
                }
                editor_refresh();
                return;
            } else if (c == CTRL_('c') || c == CTRL_('x') || c == CTRL_('w')) {
                ec.search.replace_phase = 0;
                ec.search.replace_count = 0;
                ec.cursor_x = ec.mode_state.search.saved_x;
                ec.cursor_y = ec.mode_state.search.saved_y;
                ec.col_offset = ec.mode_state.search.saved_col;
                ec.row_offset = ec.mode_state.search.saved_row;
                mode_set(MODE_NORMAL);
                editor_refresh();
                return;
            } else if (c == BACKSPACE || c == DEL_KEY || c == CTRL_('h')) {
                if (ec.search.replace_len > 0) {
                    ec.search.replace_query[--ec.search.replace_len] = '\0';
                }
            } else if (c < 0x100 && isprint(c)) {
                /* Ensure replace buffer is allocated */
                if (!ec.search.replace_query) {
                    size_t cap = 128;
                    ec.search.replace_query = calloc(cap, 1);
                    ec.search.replace_cap = cap;
                    ec.search.replace_len = 0;
                }
                if (ec.search.replace_query &&
                    ec.search.replace_len + 2 > ec.search.replace_cap) {
                    size_t new_cap = ec.search.replace_cap * 2;
                    char *nq = realloc(ec.search.replace_query, new_cap);
                    if (nq) {
                        ec.search.replace_query = nq;
                        ec.search.replace_cap = new_cap;
                    }
                }
                if (ec.search.replace_query &&
                    ec.search.replace_len + 2 <= ec.search.replace_cap) {
                    ec.search.replace_query[ec.search.replace_len++] = (char)c;
                    ec.search.replace_query[ec.search.replace_len] = '\0';
                }
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
                        ec.search.replace_query[0] = '\0';
                } else {
                    /* Plain search */
                    if (!search_do(ec.search.query)) {
                        const char *q = ec.search.query;
                        mode_set(MODE_NORMAL);
                        set_overlay_msg("[ \"%.*s\" not found ]",
                                        (int)(sizeof(ec.notfound_msg) - 20), q);
                    } else {
                        mode_set(MODE_NORMAL);
                    }
                }
            } else {
                mode_set(MODE_NORMAL);
            }
        } else if (c == CTRL_('c') || c == CTRL_('x') || c == CTRL_('w')) {
            /* Cancel: restore cursor */
            ec.cursor_x = ec.mode_state.search.saved_x;
            ec.cursor_y = ec.mode_state.search.saved_y;
            ec.col_offset = ec.mode_state.search.saved_col;
            ec.row_offset = ec.mode_state.search.saved_row;
            mode_set(MODE_NORMAL);
        } else if (c == META_('c') || c == META_('C')) {
            ec.search.mode ^= SM_CASE_SENSITIVE;
        } else if (c == META_('b') || c == META_('B')) {
            ec.search.mode ^= SM_BACKWARDS;
        } else if (c == META_('r') || c == META_('R')) {
            ec.search.mode ^= SM_REGEX;
        } else if (c == CTRL_('r')) {
            ec.search.mode ^= SM_REPLACE;
        } else if (c == BACKSPACE || c == DEL_KEY || c == CTRL_('h')) {
            if (ec.search.query_len > 0)
                ec.search.query[--ec.search.query_len] = '\0';
        } else if (c < 0x100 && isprint(c)) {
            /* Append character to query */
            if (ec.search.query &&
                ec.search.query_len + 2 > ec.search.query_cap) {
                size_t new_cap = ec.search.query_cap * 2;
                char *nq = realloc(ec.search.query, new_cap);
                if (nq) {
                    ec.search.query = nq;
                    ec.search.query_cap = new_cap;
                }
            }
            if (ec.search.query &&
                ec.search.query_len + 2 <= ec.search.query_cap) {
                ec.search.query[ec.search.query_len++] = (char)c;
                ec.search.query[ec.search.query_len] = '\0';
            }
        }
        editor_refresh();
        return;
    }

    case MODE_PROMPT:
    case MODE_CONFIRM:
        /* These modes handle their own input */
        return;

    case MODE_NORMAL:
    default:
        break;
    }

    /* Normal mode key handling */
    switch (c) {
    case '\r':
        editor_newline();
        for (int i = 0; i < indent_level; i++)
            editor_insert_char('\t', false);
        break;
    case CTRL_('x'): /* Exit editor (GNU nano: ^X) */
        if (ec.modified) {
            int r = ui_confirm(
                "Save modified buffer? (Answering \"No\" will DISCARD changes)");
            if (r == -1)
                return; /* Cancel: stay in editor */
            if (r == 1)
                file_save(); /* Yes: save then quit */
            /* No (r == 0): discard changes and quit */
        }
        term_clear();
        term_close_buffer();
        editor_cleanup();
        exit(0);
        break;
    case CTRL_('o'): /* Save file (GNU nano: ^O Write Out) */
        file_save();
        break;
    case CTRL_('z'):
    case META_('u'):
    case META_('U'):
        undo_perform_undo();
        break;
    case CTRL_('y'):
    case META_('e'):
    case META_('E'):
        undo_perform_redo();
        break;
    case META_('a'):
    case META_('A'): /* Start text marking (GNU nano: M-A Set Mark) */
        if (ec.mode != MODE_SELECT) {
            mode_set(MODE_SELECT);
            ui_set_message(
                "Mark set - Move cursor to select, M-6=Copy, ^K=Cut, "
                "^C=Cancel");
        }
        break;
    case META_('6'): /* Copy current line (GNU nano: M-6 Copy) */
        if (ec.cursor_y < NR)
            editor_copy(0);
        break;
    case CTRL_('k'): /* Cut current line (GNU nano: ^K Cut Line) */
        editor_cut(consecutive_cut);
        ec.last_was_cut = true;
        break;
    case CTRL_('u'): /* Paste/uncut (GNU nano: ^U Uncut) */
        editor_paste();
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP)
            ec.cursor_y = ec.row_offset;
        else if (c == PAGE_DOWN)
            ec.cursor_y = ec.row_offset + ec.screen_rows - 1;
        int times = ec.screen_rows;
        while (times--)
            editor_move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
    } break;
    case HOME_KEY:
        ec.cursor_x = 0;
        break;
    case END_KEY:
        if (ec.cursor_y < NR)
            ec.cursor_x = ROW(ec.cursor_y)->size;
        break;
    case CTRL_('w'): /* Find/search (GNU nano: ^W Where Is) */
        search_find();
        return; /* editor_refresh() already called inside search_find */
    case CTRL_('n'): /* Toggle line numbers */
        ec.show_line_numbers = !ec.show_line_numbers;
        ui_set_message("Line numbers %s",
                       ec.show_line_numbers ? "enabled" : "disabled");
        break;
    case META_('b'):
    case META_('B'): /* Open file browser (M-B) */
        mode_set(MODE_BROWSER);
        browser_load_directory(".");
        if (ec.mode != MODE_BROWSER)
            return;
        ui_set_message("File Browser: Enter to open, ^C to cancel");
        browser_screen_open();
        return;      /* Don't continue to normal refresh */
    case META_('\\'): /* M-\ Go to first line (GNU nano: M-\) */
        ec.cursor_y = 0;
        ec.cursor_x = 0;
        break;
    case META_('/'): /* M-/ Go to last line (GNU nano: M-/) */
        if (NR > 0) {
            ec.cursor_y = NR - 1;
            ec.cursor_x = 0;
        }
        break;
    case CTRL_('g'): /* Show help (GNU nano: ^G) */
        mode_set(MODE_HELP);
        help_screen_open();
        return; /* Don't continue to normal refresh */
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
    case '{':
        editor_insert_char(c, true);
        indent_level++;
        break;
    case '}':
        if (ec.cursor_y == NR)
            goto none;
        if ((ec.cursor_x == 0) && (ec.cursor_y == 0))
            goto none;
        editor_row_t *row = ROW(ec.cursor_y);
        if ((ec.cursor_x > 0) && (row->chars[ec.cursor_x - 1] == '\t'))
            editor_delete_char();
    none:
        editor_insert_char(c, true);
        indent_level--;
        break;
    default:
        editor_insert_char(c, true);
    }
}

static void editor_init(void)
{
    term_update_size();
    signal(SIGWINCH, sig_winch_handler);
    signal(SIGCONT, sig_cont_handler);
    srand((unsigned int) time(NULL));
    ec.rows = tlist_new(sizeof(editor_row_t));
}

int main(int argc, char *argv[])
{
    editor_init();
    if (argc >= 2)
        file_open(argv[1]);
    term_enable_raw();
    ui_set_message("Mazu Editor | ^G Help");
    editor_refresh();

    /* Main event loop */
    while (1) {
        editor_process_key();
        if (ec.mode != MODE_BROWSER && ec.mode != MODE_HELP &&
            ec.mode != MODE_SEARCH)
            editor_refresh();
    }
    /* not reachable */
    return 0;
}
