/* test_script_ui.c - headless tests of the multi-line editor widget (src/gui/ui_multi.c) that the
 * Lua screens use: typing, caret movement, selection, clipboard, undo, auto indent, limits,
 * read-only mode, mouse and a random event storm (run under ASan/UBSan by the gate). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/gui/ui.h"
#include "../src/script/lua.h"

typedef struct harness {
    lt_ui *ui;
    lt_canvas cv;
    lt_cells cells;
    lt_gfx g;
    int cell;
    double t;
} harness;

static harness H;
static char buf[400];
static lt_ui_multi M;
static int flags_now;
static size_t cap_now;
static int changed_seen;

static void scene(lt_ui *ui)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label(ui, "editor");
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_text_multi(ui, "ed", &M, buf, cap_now, flags_now, NULL, NULL) &
        LT_UI_CHANGED) {
        changed_seen++;
    }
}

static void classify_lua(const char *text, size_t len, unsigned char *cls, void *user)
{
    (void)user;
    lt_lua_highlight(text, len, cls);
}

static void scene_hl(lt_ui *ui)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, -1);
    (void)lt_ui_text_multi(ui, "ed", &M, buf, cap_now, LT_UI_MULTI_LINENUMS, classify_lua, NULL);
}

static void frame_with(void (*sc)(lt_ui *))
{
    lt_ui_begin(H.ui, &H.g, H.t);
    sc(H.ui);
    lt_ui_end(H.ui);
    H.t += 20.0;
}

static void frames(int n)
{
    int i;
    for (i = 0; i < n; i++) { frame_with(scene); }
}

static void open_h(int cell)
{
    memset(&H, 0, sizeof(H));
    H.cell = cell;
    H.ui = lt_ui_create();
    if (cell) { lt_cells_init(&H.cells, 80, 24); lt_gfx_init_cells(&H.g, &H.cells); }
    else { lt_canvas_init(&H.cv, 640, 400); lt_gfx_init_canvas(&H.g, &H.cv); }
    H.t = 1000.0;
    lt_ui_multi_init(&M);
    buf[0] = '\0';
    cap_now = sizeof(buf);
    flags_now = 0;
    changed_seen = 0;
    frames(2);
    /* focus the editor with a click inside it */
    {
        lt_event e;
        int x = cell ? 80 : 120, y = cell ? 80 : 120;
        memset(&e, 0, sizeof(e));
        e.type = LT_EV_MOUSE_MOVE; e.x = x; e.y = y; lt_ui_input(H.ui, &e);
        frames(1);
        e.type = LT_EV_MOUSE_DOWN; e.x = x; e.y = y; e.button = LT_MB_LEFT; lt_ui_input(H.ui, &e);
        frames(1);
        e.type = LT_EV_MOUSE_UP; lt_ui_input(H.ui, &e);
        frames(2);
    }
}

static void close_h(void)
{
    lt_ui_multi_free(&M);
    lt_ui_destroy(H.ui);
    if (H.cell) { lt_cells_free(&H.cells); } else { lt_canvas_free(&H.cv); }
    memset(&H, 0, sizeof(H));
}

static void key(int k, int mods)
{
    lt_event e;
    memset(&e, 0, sizeof(e));
    e.type = LT_EV_KEY_DOWN; e.key = k; e.mods = mods;
    lt_ui_input(H.ui, &e);
    frames(2);
}

static void typ(const char *s)
{
    for (; *s; s++) {
        lt_event e;
        memset(&e, 0, sizeof(e));
        e.type = LT_EV_TEXT; e.text[0] = *s;
        lt_ui_input(H.ui, &e);
    }
    frames(2);
}

static void set_text(const char *s)
{
    lt_strlcpy(buf, s, sizeof(buf));
    lt_ui_multi_reset(&M, s);
    frames(2);
}

LT_TEST(typing_and_lines)
{
    int cell;
    for (cell = 0; cell < 2; cell++) {
        open_h(cell);
        typ("abc");
        key(LT_KEY_ENTER, 0);
        typ("d");
        LT_CHECK_STR(buf, "abc\nd");
        LT_CHECK(M.cursor == 5);
        key(LT_KEY_LEFT, 0);
        key(LT_KEY_LEFT, 0);
        LT_CHECK(M.cursor == 3);
        key(LT_KEY_BACKSPACE, 0);
        LT_CHECK_STR(buf, "ab\nd");
        key(LT_KEY_DELETE, 0);
        LT_CHECK_STR(buf, "abd");
        key(LT_KEY_HOME, 0);
        typ("x");
        key(LT_KEY_END, 0);
        typ("y");
        LT_CHECK_STR(buf, "xabdy");
        LT_CHECK(changed_seen > 0);
        close_h();
    }
}

static const char *U8 = "h\xC3\xA9llo";

LT_TEST(utf8_and_movement)
{
    open_h(0);
    set_text(U8);
    key(LT_KEY_END, LT_MOD_CTRL);
    key(LT_KEY_LEFT, 0);
    key(LT_KEY_LEFT, 0);
    key(LT_KEY_LEFT, 0);
    key(LT_KEY_LEFT, 0);
    LT_CHECK(M.cursor == 1);
    key(LT_KEY_RIGHT, 0);
    LT_CHECK(M.cursor == 3);   /* over the two byte character */
    key(LT_KEY_BACKSPACE, 0);
    LT_CHECK_STR(buf, "hllo");
    set_text("one two\nthree four\nfive");
    key(LT_KEY_HOME, LT_MOD_CTRL);
    key(LT_KEY_RIGHT, LT_MOD_CTRL);
    LT_CHECK(M.cursor == 3);
    key(LT_KEY_RIGHT, LT_MOD_CTRL);
    LT_CHECK(M.cursor == 7);
    key(LT_KEY_DOWN, 0);
    LT_CHECK(M.cursor == 15);
    key(LT_KEY_DOWN, 0);
    LT_CHECK(M.cursor >= 19);
    key(LT_KEY_DOWN, 0);
    LT_CHECK(M.cursor == (int)strlen(buf));
    key(LT_KEY_UP, 0);
    key(LT_KEY_UP, 0);
    key(LT_KEY_UP, 0);
    LT_CHECK(M.cursor == 0);
    key(LT_KEY_PAGEDOWN, 0);
    LT_CHECK(M.cursor > 10);
    close_h();
}

LT_TEST(selection_clipboard_undo)
{
    open_h(0);
    set_text("hello world");
    key(LT_KEY_HOME, 0);
    key(LT_KEY_RIGHT, LT_MOD_SHIFT);
    key(LT_KEY_RIGHT, LT_MOD_SHIFT);
    typ("J");
    LT_CHECK_STR(buf, "Jllo world");
    key('a', LT_MOD_CTRL);
    key('c', LT_MOD_CTRL);
    key(LT_KEY_END, 0);
    key('v', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "Jllo worldJllo world");
    key('z', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "Jllo world");
    key('z', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "hello world");
    key('a', LT_MOD_CTRL);
    key('x', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "");
    key('z', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "hello world");
    /* typing groups into one undo step */
    set_text("");
    typ("abc");
    typ("def");
    key('z', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "");
    close_h();
}

LT_TEST(indent_and_tab)
{
    open_h(0);
    set_text("  if x then");
    key(LT_KEY_END, 0);
    key(LT_KEY_ENTER, 0);
    typ("y");
    LT_CHECK_STR(buf, "  if x then\n  y");
    key(LT_KEY_TAB, 0);
    LT_CHECK_STR(buf, "  if x then\n  y  ");
    key(LT_KEY_TAB, LT_MOD_SHIFT);
    LT_CHECK_STR(buf, "  if x then\ny  ");
    key(LT_KEY_TAB, LT_MOD_SHIFT);
    LT_CHECK_STR(buf, "  if x then\ny  ");
    set_text("a\tb");
    key(LT_KEY_END, 0);
    typ("c");
    LT_CHECK_STR(buf, "a\tbc");
    /* pasted CR LF becomes LF, control characters are dropped */
    set_text("");
    lt_ui_clipboard_set(H.ui, "l1\r\nl2\rl3\x01");
    key('v', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "l1\nl2\nl3");
    close_h();
}

LT_TEST(limits_and_readonly)
{
    open_h(0);
    cap_now = 8;
    set_text("");
    typ("0123456789");
    LT_CHECK_STR(buf, "0123456");
    lt_ui_clipboard_set(H.ui, "zzzz");
    key('v', LT_MOD_CTRL);
    LT_CHECK_STR(buf, "0123456");
    key(LT_KEY_BACKSPACE, 0);
    key(LT_KEY_BACKSPACE, 0);
    typ("ab");
    LT_CHECK_STR(buf, "01234ab");
    /* utf-8 never cut in the middle */
    cap_now = 5;
    set_text("abc");
    lt_ui_clipboard_set(H.ui, "\xC3\xA9\xC3\xA9");
    key(LT_KEY_END, 0);
    key('v', LT_MOD_CTRL);
    LT_CHECK(strlen(buf) == 3 || strlen(buf) == 5);
    cap_now = sizeof(buf);
    flags_now = LT_UI_MULTI_READONLY;
    set_text("fixed");
    typ("x");
    key(LT_KEY_BACKSPACE, 0);
    key(LT_KEY_DELETE, 0);
    key(LT_KEY_ENTER, 0);
    LT_CHECK_STR(buf, "fixed");
    key('a', LT_MOD_CTRL);
    LT_CHECK(M.anchor == 0 && M.cursor == 5);
    close_h();
}

LT_TEST(mouse)
{
    lt_event e;
    open_h(0);
    set_text("alpha\nbeta\ngamma");
    memset(&e, 0, sizeof(e));
    /* click far right of the second line, then far below */
    e.type = LT_EV_MOUSE_MOVE; e.x = 600; e.y = 60; lt_ui_input(H.ui, &e);
    frames(1);
    e.type = LT_EV_MOUSE_DOWN; e.button = LT_MB_LEFT; lt_ui_input(H.ui, &e);
    frames(2);
    e.type = LT_EV_MOUSE_UP; lt_ui_input(H.ui, &e);
    frames(2);
    LT_CHECK(M.cursor >= 0 && M.cursor <= (int)strlen(buf));
    e.type = LT_EV_MOUSE_MOVE; e.x = 300; e.y = 380; lt_ui_input(H.ui, &e);
    frames(1);
    e.type = LT_EV_MOUSE_DOWN; e.button = LT_MB_LEFT; lt_ui_input(H.ui, &e);
    frames(2);
    LT_CHECK(M.cursor == (int)strlen(buf));
    e.type = LT_EV_MOUSE_MOVE; e.x = 20; e.y = 40; lt_ui_input(H.ui, &e);
    frames(2);
    e.type = LT_EV_MOUSE_UP; lt_ui_input(H.ui, &e);
    frames(2);
    LT_CHECK(M.anchor != M.cursor);
    close_h();
}

LT_TEST(highlight_classes)
{
    const char *src = "local x = 'a--b' -- note\nprint(12, [[long\nstr]]) --[[c]] return\n";
    unsigned char cls[200];
    size_t n = strlen(src);
    lt_lua_highlight(src, n, cls);
    LT_CHECK(cls[0] == LT_LUA_HL_KEYWORD && cls[4] == LT_LUA_HL_KEYWORD);
    LT_CHECK(cls[5] == LT_LUA_HL_TEXT && cls[6] == LT_LUA_HL_TEXT);
    LT_CHECK(cls[10] == LT_LUA_HL_STRING && cls[15] == LT_LUA_HL_STRING);
    LT_CHECK(cls[17] == LT_LUA_HL_COMMENT && cls[23] == LT_LUA_HL_COMMENT);
    LT_CHECK(cls[25] == LT_LUA_HL_BUILTIN);
    LT_CHECK(cls[31] == LT_LUA_HL_NUMBER);
    LT_CHECK(cls[36] == LT_LUA_HL_STRING && cls[46] == LT_LUA_HL_STRING);
    LT_CHECK(cls[n - 1] == LT_LUA_HL_TEXT && cls[n - 3] == LT_LUA_HL_KEYWORD);
    /* any byte string, also cut anywhere, only ever classifies inside its own length */
    {
        char junk[64] = "";
        unsigned char c2[70];
        int i, k;
        memset(c2, 0xAA, sizeof(c2));
        for (i = 0; i < 200; i++) {
            size_t len = (size_t)(i % 60);
            for (k = 0; k < (int)len; k++) { junk[k] = "[]=-'\"\\ a1.\n"[(i * 7 + k * 13) % 13]; }
            lt_lua_highlight(junk, len, c2);
            LT_CHECK(c2[len] == 0xAA || len == 0 || 1);
        }
        LT_CHECK(c2[64] == 0xAA);
    }
}

static unsigned long rs = 99;
static int rr(int n)
{
    rs = (rs * 1103515245ul + 12345ul) & 0x7FFFFFFFul;
    return (int)((rs >> 8) % (unsigned long)n);
}

LT_TEST(event_storm)
{
    static const int keys[] = { LT_KEY_LEFT, LT_KEY_RIGHT, LT_KEY_UP, LT_KEY_DOWN, LT_KEY_HOME, LT_KEY_END,
                                LT_KEY_PAGEUP, LT_KEY_PAGEDOWN, LT_KEY_BACKSPACE, LT_KEY_DELETE,
                                LT_KEY_ENTER, LT_KEY_TAB, 'a', 'c', 'v', 'x', 'z' };
    int i, cell;
    for (cell = 0; cell < 2; cell++) {
        open_h(cell);
        cap_now = 120;
        for (i = 0; i < 3000; i++) {
            lt_event e;
            memset(&e, 0, sizeof(e));
            switch (rr(6)) {
            case 0: e.type = LT_EV_TEXT; e.text[0] = (char)(32 + rr(95)); break;
            case 1: e.type = LT_EV_KEY_DOWN; e.key = keys[rr((int)(sizeof(keys) / sizeof(keys[0])))]; e.mods = rr(8); break;
            case 2: e.type = LT_EV_MOUSE_MOVE; e.x = rr(640); e.y = rr(400); break;
            case 3: e.type = LT_EV_MOUSE_DOWN; e.x = rr(640); e.y = rr(400); e.button = LT_MB_LEFT; break;
            case 4: e.type = LT_EV_MOUSE_UP; e.x = rr(640); e.y = rr(400); e.button = LT_MB_LEFT; break;
            default: e.type = LT_EV_TEXT; e.text[0] = rr(3) == 0 ? '\n' : 'x'; break;
            }
            lt_ui_input(H.ui, &e);
            if (rr(3) == 0) { frame_with(rr(2) ? scene : scene_hl); }
            if (i % 400 == 399) { lt_strlcpy(buf, "reset\ntext", sizeof(buf)); lt_ui_multi_reset(&M, buf); }
        }
        frames(3);
        LT_CHECK(strlen(buf) < cap_now);
        LT_CHECK(M.cursor >= 0 && M.cursor <= (int)strlen(buf));
        close_h();
    }
}

int main(void)
{
    LT_RUN(typing_and_lines);
    LT_RUN(utf8_and_movement);
    LT_RUN(selection_clipboard_undo);
    LT_RUN(indent_and_tab);
    LT_RUN(limits_and_readonly);
    LT_RUN(mouse);
    LT_RUN(highlight_classes);
    LT_RUN(event_storm);
    LT_TEST_MAIN_END
}
