/* Check C syntax highlighting for preprocessor lines.
 *
 * User reports on whitish terminal:
 *   #ifdef ... as blue, #include <...> as yellow,
 *   #include "..." blue/yellow, #else as red.
 *
 * Expected from syntax/c.nanorc order and nano semantics:
 *   preprocessor directives are brightcyan, and the engine
 *   applies rules in order with first wins (only NORMAL cells
 *   get a color). Check each segment.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define main culo_main
#include "../../culo.c"
#undef main
#include "harness.h"

#define FIXTURE_PATH "tests/test_tmp/h_syntax.c"

static int hl_code_for(int row, int col) {
    editor_row_t *r = ROW(row);
    if (!r || !r->highlight) return -999;
    if (col < 0 || col >= r->render_size) return -999;
    return r->highlight[col];
}
static const char *hl_name(unsigned char code) {
    if (code == NORMAL) return "NORMAL";
    if (code >= HIGHLIGHT_DYNAMIC_BASE && code < ec.syntax_palette_count) {
        static char buf[64];
        snprintf(buf, sizeof(buf), "fg=%d bg=%d", ec.syntax_palette[code].fg, ec.syntax_palette[code].bg);
        return buf;
    }
    return "UNKNOWN";
}
static void dump_line(int row) {
    editor_row_t *r = ROW(row);
    printf("row %d render=|%s|\n", row, r ? r->render : "(null)");
    if (!r) return;
    for (int i=0;i<r->render_size;i++) {
        printf(" %d:%c=%s", i, r->render[i], hl_name(r->highlight[i]));
    }
    printf("\n");
}
static void check_seg(int row, int from, int to, const char *what, color_id_t want_fg) {
    editor_row_t *r = ROW(row);
    if (!r) { printf("FAIL %s: no row\n", what); h_fails++; return; }
    for (int i=from;i<to && i<r->render_size;i++) {
        unsigned char code = r->highlight[i];
        if (code == NORMAL) { printf("FAIL %s col %d NORMAL want fg %d\n", what, i, want_fg); h_fails++; return; }
        color_id_t fg = ec.syntax_palette[code].fg;
        if (fg != want_fg) {
            printf("FAIL %s col %d got fg %d want %d (%s)\n", what, i, fg, want_fg, hl_name(code));
            h_fails++; return;
        }
    }
    printf("ok   %s\n", what);
}

int main(void)
{
    mkdir("tests/test_tmp", 0777);
    FILE *f = fopen(FIXTURE_PATH, "w");
    if (!f) exit(2);
    fputs("#ifdef FOO\n", f);
    fputs("#include <stdio.h>\n", f);
    fputs("#include \"foo.h\"\n", f);
    fputs("#else\n", f);
    fputs("#endif\n", f);
    fputs("#define X 1\n", f);
    fputs("int main(void) { return 0; }\n", f);
    fclose(f);
    h_release_buffer();
    editor_init();
    term_update_size();
    if (!file_open(FIXTURE_PATH)) exit(2);
    editor_refresh();
    for (int i=0;i<NR;i++) {
        syntax_ensure_row_highlighted(i);
        dump_line(i);
    }
    /* #ifdef should be brightcyan (COLOR_BRIGHTCYAN) for the directive part.
     * At minimum the whole line should not be blue (COLOR_BLUE) or red. */
    check_seg(0, 0, 6, "#ifdef directive", COLOR_BRIGHTCYAN);
    check_seg(1, 0, 8, "#include directive", COLOR_BRIGHTCYAN);
    /* <stdio.h> part: brightyellow per nanorc */
    check_seg(1, 9, 18, "#include <stdio.h> path", COLOR_BRIGHTYELLOW);
    /* "foo.h" string part: brightyellow */
    check_seg(2, 9, 16, "#include \"foo.h\" path", COLOR_BRIGHTYELLOW);
    check_seg(3, 0, 5, "#else directive", COLOR_BRIGHTCYAN);
    check_seg(4, 0, 6, "#endif directive", COLOR_BRIGHTCYAN);
    check_seg(5, 0, 7, "#define directive", COLOR_BRIGHTCYAN);
    /* int should be green, return should be brightred */
    check_seg(6, 0, 3, "int keyword", COLOR_GREEN);
    /* find return */
    {
        editor_row_t *r = ROW(6);
        int pos = -1;
        for (int i=0;i+6<=r->render_size;i++) if (strncmp(r->render+i,"return",6)==0) {pos=i;break;}
        if (pos<0) {printf("FAIL return not found\n"); h_fails++;}
        else check_seg(6, pos, pos+6, "return keyword", COLOR_BRIGHTRED);
    }
    h_summary("C syntax preprocessor tests");
    return 0;
}
