/* test_ui.c - headless tests for the immediate-mode toolkit.
 * Widgets are driven with synthesized events against an offscreen pixel
 * canvas and an lt_cells grid; also renders build/ui_gallery.ppm/.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "lt_test.h"
#include "../src/gui/ui.h"
#include "../src/gui/ui_priv.h"
#include "../src/plat/plat.h"
#include "ui_gallery.h"

/* ------------------------------------------------------------------ */
/* harness                                                             */
typedef struct harness {
    lt_ui *ui;
    lt_canvas cv;
    lt_cells cells;
    lt_gfx g;
    int cell;
    double t;
    void (*scene)(lt_ui *ui);
} harness;

static harness H;
static lt_rect R[16];

static void h_open(int w, int h, int cell)
{
    memset(&H, 0, sizeof(H));
    H.cell = cell;
    H.ui = lt_ui_create();
    if (cell) {
        lt_cells_init(&H.cells, w, h);
        lt_gfx_init_cells(&H.g, &H.cells);
    } else {
        lt_canvas_init(&H.cv, w, h);
        lt_gfx_init_canvas(&H.g, &H.cv);
    }
    H.t = 1000.0;
}

static void h_close(void)
{
    lt_ui_destroy(H.ui);
    if (H.cell) { lt_cells_free(&H.cells); } else { lt_canvas_free(&H.cv); }
    memset(&H, 0, sizeof(H));
}

static void frame(void)
{
    lt_ui_begin(H.ui, &H.g, H.t);
    if (H.scene != NULL) { H.scene(H.ui); }
    lt_ui_end(H.ui);
    H.t += 20.0;
}

static void frames(int n)
{
    int i;
    for (i = 0; i < n; i++) { frame(); }
}

static void ev_mouse(int type, int x, int y, int button, int mods)
{
    lt_event e;
    memset(&e, 0, sizeof(e));
    e.type = (lt_ev_type)type; e.x = x; e.y = y; e.button = button; e.mods = mods;
    lt_ui_input(H.ui, &e);
}

static void mouse_move(int x, int y) { ev_mouse(LT_EV_MOUSE_MOVE, x, y, 0, 0); }
static void mouse_down(int x, int y, int b, int mods) { ev_mouse(LT_EV_MOUSE_DOWN, x, y, b, mods); }
static void mouse_up(int x, int y, int b, int mods) { ev_mouse(LT_EV_MOUSE_UP, x, y, b, mods); }

static void click_ex(int x, int y, int button, int mods)
{
    mouse_move(x, y); frame();
    mouse_down(x, y, button, mods); frame();
    mouse_up(x, y, button, mods); frame();
    frame();
}

static void click(int x, int y) { click_ex(x, y, LT_MB_LEFT, 0); }
static int cx_of(lt_rect r) { return r.x + r.w / 2; }
static int cy_of(lt_rect r) { return r.y + r.h / 2; }
static void click_rect(lt_rect r) { click(cx_of(r), cy_of(r)); }

static void key_ex(int key, int mods)
{
    lt_event e;
    memset(&e, 0, sizeof(e));
    e.type = LT_EV_KEY_DOWN; e.key = key; e.mods = mods;
    lt_ui_input(H.ui, &e);
    frame();
    frame();
}

static void key(int k) { key_ex(k, 0); }

static void type_str(const char *s)
{
    for (; *s; s++) {
        lt_event e;
        memset(&e, 0, sizeof(e));
        e.type = LT_EV_KEY_DOWN; e.key = tolower((unsigned char)*s);
        lt_ui_input(H.ui, &e);
        memset(&e, 0, sizeof(e));
        e.type = LT_EV_TEXT; e.text[0] = *s;
        lt_ui_input(H.ui, &e);
    }
    frame();
    frame();
}

static void wheel(int x, int y, int d)
{
    lt_event e;
    memset(&e, 0, sizeof(e));
    e.type = LT_EV_WHEEL; e.x = x; e.y = y; e.wheel = d;
    lt_ui_input(H.ui, &e);
    frame();
    frame();
}

static lt_u32 px(int x, int y) { return H.cv.px[y * H.cv.pitch + x] & 0xFFFFFFUL; }

static int cells_has_text(const char *s)
{
    int r, c, n = (int)strlen(s);
    for (r = 0; r < H.cells.rows; r++) {
        for (c = 0; c + n <= H.cells.cols; c++) {
            int i, ok = 1;
            for (i = 0; i < n && ok; i++) {
                if (H.cells.cells[r * H.cells.cols + c + i].ch != (lt_u32)(unsigned char)s[i]) { ok = 0; }
            }
            if (ok) { return 1; }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* gallery rendering                                                   */
static void write_ppm(lt_canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            lt_u32 v = c->px[y * c->pitch + x];
            fputc((int)((v >> 16) & 255), f); fputc((int)((v >> 8) & 255), f);
            fputc((int)(v & 255), f);
        }
    }
    fclose(f);
}

static void put_utf8(FILE *f, lt_u32 cp)
{
    if (cp < 0x80) { fputc((int)cp, f); }
    else if (cp < 0x800) { fputc((int)(0xC0 | (cp >> 6)), f); fputc((int)(0x80 | (cp & 0x3F)), f); }
    else {
        fputc((int)(0xE0 | (cp >> 12)), f); fputc((int)(0x80 | ((cp >> 6) & 0x3F)), f);
        fputc((int)(0x80 | (cp & 0x3F)), f);
    }
}

static void write_cells(lt_cells *g, const char *path)
{
    FILE *f = fopen(path, "wb");
    int r, c, last;
    if (f == NULL) { return; }
    for (r = 0; r < g->rows; r++) {
        last = g->cols - 1;
        while (last >= 0 && g->cells[r * g->cols + last].ch == ' ') { last--; }
        for (c = 0; c <= last; c++) { put_utf8(f, g->cells[r * g->cols + c].ch); }
        fputc('\n', f);
    }
    fclose(f);
}

static gallery GAL;
static void scene_gallery(lt_ui *ui) { gallery_frame(ui, &GAL); }

static void settle(int n) { frames(n); }

static void render_gallery(const char *path, int variant)
{
    h_open(1280, 800, 0);
    gallery_init(&GAL);
    H.scene = scene_gallery;
    lt_theme_preset(lt_ui_theme(H.ui), variant == 3 ? "light" : "dark");
    settle(4);
    if (variant == 1) {
        /* open File > Recent submenu */
        click(30, 13);
        settle(3);
        mouse_move(60, 13 + 26 * 4); settle(3);
        mouse_move(60, 13 + 26 * 4 + 1); settle(3);
    } else if (variant == 2) {
        GAL.modal_open = 1;
        settle(5);
    } else if (variant == 4) {
        lt_ui_filedlg_open(&GAL.fd, LT_UI_FILE_OPEN, "tests", "", "Tune files|*.msq;*.xml|All files|*");
        settle(5);
    } else if (variant == 0) {
        /* hover tooltip + toast + selection in the grid */
        lt_ui_toast(H.ui, 1, "Saved tune to project");
        settle(2);
    }
    if (variant == 0) {
        GAL.win_open = 1;
        settle(3);
    }
    write_ppm(&H.cv, path);
    gallery_free(&GAL);
    h_close();
}

LT_TEST(test_gallery_render)
{
    FILE *f;
    render_gallery("build/ui_gallery.ppm", 0);
    f = fopen("build/ui_gallery.ppm", "rb");
    LT_CHECK(f != NULL);
    if (f != NULL) { fclose(f); }
    render_gallery("build/ui_gallery_menu.ppm", 1);
    render_gallery("build/ui_gallery_modal.ppm", 2);
    render_gallery("build/ui_gallery_light.ppm", 3);
    render_gallery("build/ui_gallery_files.ppm", 4);
    /* tty render: 160x50 cells = 1280x800 */
    h_open(160, 50, 1);
    gallery_init(&GAL);
    H.scene = scene_gallery;
    settle(5);
    write_cells(&H.cells, "build/ui_gallery.txt");
    LT_CHECK(cells_has_text("File"));
    LT_CHECK(cells_has_text("Buttons"));
    gallery_free(&GAL);
    h_close();
}


/* ------------------------------------------------------------------ */
/* shared scene state                                                  */
static int s_n[8];
static int s_res;
static int s_flags;
static char s_buf[64];
static double s_val;
static int s_sel, s_open, s_open2, s_active;
static lt_rect s_wr, s_wr2;
static int s_sy;
static int s_gf;
static int s_menu_edit_open, s_menu_sub_open;

static void reset_state(void)
{
    memset(s_n, 0, sizeof(s_n));
    memset(R, 0, sizeof(R));
    s_res = 0; s_flags = 0; s_buf[0] = '\0'; s_val = 0.0; s_sel = 0; s_open = 0; s_open2 = 0;
    s_active = 0; s_sy = 0; s_gf = 0; s_menu_edit_open = 0; s_menu_sub_open = 0;
}


/* ------------------------------------------------------------------ */
LT_TEST(test_ids_and_theme)
{
    lt_ui *ui = lt_ui_create();
    lt_u32 a = lt_ui_id(ui, "Save"), b = lt_ui_id(ui, "Save##2"), c = lt_ui_id(ui, "Save##3");
    lt_u32 d = lt_ui_id(ui, "Open###x"), e = lt_ui_id(ui, "Close###x"), f;
    lt_theme t;
    LT_CHECK(a != b && b != c && a != 0);
    LT_CHECK(d == e);
    lt_ui_push_id(ui, "panel");
    f = lt_ui_id(ui, "Save");
    lt_ui_pop_id(ui);
    LT_CHECK(f != a && lt_ui_id(ui, "Save") == a);
    lt_ui_push_id_int(ui, 1);
    f = lt_ui_id(ui, "x");
    lt_ui_pop_id(ui);
    lt_ui_push_id_int(ui, 2);
    LT_CHECK(f != lt_ui_id(ui, "x"));
    lt_ui_pop_id(ui);
    lt_ui_destroy(ui);

    lt_theme_dark(&t);
    LT_CHECK(t.bg_app == 0xFF1A1A1AUL && t.surface == 0xFF242424UL && t.selected == 0xFF0D47A1UL);
    LT_CHECK(t.primary == 0xFF1976D2UL && t.text == 0xFFE8E8E8UL && t.table_grid == 0xFF383838UL);
    LT_CHECK(t.cursor_live == 0xFF00FF00UL && t.error == 0xFFF44336UL && t.dark);
    lt_theme_light(&t);
    LT_CHECK(t.bg_app == 0xFFF5F5F5UL && !t.dark);
    LT_CHECK(lt_theme_preset(&t, "nord") == 0 && t.bg_app == 0xFF2E3440UL);
    LT_CHECK(lt_theme_preset(&t, "highcontrast") == 0 && lt_theme_preset(&t, "nope") == -1);
    LT_CHECK(lt_heat_color(0, 0.0) == 0xFF0000FFUL && lt_heat_color(0, 1.0) == 0xFFFF0000UL);
    LT_CHECK(lt_heat_color(1, 0.0) == 0xFF000000UL && lt_heat_color(1, 1.0) == 0xFFFFFFFFUL);
    LT_CHECK_NEAR(lt_heat_color(1, 0.5) & 0xFF, 128, 1);
    LT_CHECK(lt_heat_color(0, -5.0) == 0xFF0000FFUL && lt_heat_color(0, 7.0) == 0xFFFF0000UL);
    LT_CHECK(lt_color_contrast(0xFF000000UL) == 0xFFFFFFFFUL && lt_color_contrast(0xFFFFFFFFUL) != 0xFFFFFFFFUL);
}

/* ---- buttons / checkbox / radio / toggle ------------------------- */
static int s_chk, s_radio, s_tog;
static void scene_buttons(lt_ui *ui)
{
    int w[2];
    w[0] = 100; w[1] = 100;
    lt_ui_layout_row(ui, 2, w, 0);
    if (lt_ui_button(ui, "One")) { s_n[0]++; }
    R[0] = lt_ui_last_rect(ui);
    if (lt_ui_button_ex(ui, "Two##b2", LT_UI_BTN_DISABLED)) { s_n[1]++; }
    R[1] = lt_ui_last_rect(ui);
    lt_ui_layout_row(ui, 3, NULL, 0);
    if (lt_ui_checkbox(ui, "Check", &s_chk)) { s_n[2]++; }
    R[2] = lt_ui_last_rect(ui);
    if (lt_ui_radio(ui, "RA", &s_radio, 1)) { s_n[3]++; }
    R[3] = lt_ui_last_rect(ui);
    if (lt_ui_toggle(ui, "Tog", &s_tog)) { s_n[4]++; }
    R[4] = lt_ui_last_rect(ui);
}

LT_TEST(test_button_click)
{
    reset_state();
    s_chk = 0; s_radio = 0; s_tog = 0;
    h_open(400, 300, 0);
    H.scene = scene_buttons;
    frames(3);
    LT_CHECK(lt_ui_row_height(H.ui) == 24 && !lt_ui_cell_mode(H.ui));
    LT_CHECK(R[0].w == 100 && R[0].h == 24 && R[1].x > R[0].x + 100);
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 1);
    click(R[0].x + R[0].w + 2, R[0].y + 5);     /* gap between buttons */
    LT_CHECK_INT(s_n[0], 1);
    click(300, 250);                             /* empty space */
    LT_CHECK_INT(s_n[0], 1);
    click_rect(R[1]);                            /* disabled */
    LT_CHECK_INT(s_n[1], 0);
    /* press and release delivered in the same frame still click */
    mouse_move(cx_of(R[0]), cy_of(R[0])); frame();
    mouse_down(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, 0);
    mouse_up(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, 0);
    frame();
    LT_CHECK_INT(s_n[0], 2);
    /* press on the button but release elsewhere: no click */
    mouse_move(cx_of(R[0]), cy_of(R[0])); frame();
    mouse_down(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, 0); frame();
    mouse_move(300, 250); frame();
    mouse_up(300, 250, LT_MB_LEFT, 0); frame(); frame();
    LT_CHECK_INT(s_n[0], 2);
    /* keyboard activation of the focused button */
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 3);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[0], 4);
    key(' ');
    LT_CHECK_INT(s_n[0], 5);
    h_close();
}

LT_TEST(test_check_radio_toggle)
{
    reset_state();
    s_chk = 0; s_radio = 0; s_tog = 0;
    h_open(400, 300, 0);
    H.scene = scene_buttons;
    frames(3);
    click_rect(R[2]);
    LT_CHECK(s_chk == 1 && s_n[2] == 1);
    click_rect(R[2]);
    LT_CHECK(s_chk == 0 && s_n[2] == 2);
    click_rect(R[3]);
    LT_CHECK(s_radio == 1 && s_n[3] == 1);
    click_rect(R[3]);
    LT_CHECK(s_radio == 1 && s_n[3] == 1);       /* already selected: no change */
    click_rect(R[4]);
    LT_CHECK(s_tog == 1 && s_n[4] == 1);
    click_rect(R[4]);
    LT_CHECK(s_tog == 0);
    h_close();
}

/* ---- text input -------------------------------------------------- */
static char g_clip[256];
static void mock_set(lt_backend *b, const char *s) { LT_UNUSED(b); lt_strlcpy(g_clip, s, sizeof(g_clip)); }
static char *mock_get(lt_backend *b) { LT_UNUSED(b); return g_clip[0] ? lt_strdup(g_clip) : NULL; }

static void scene_text(lt_ui *ui)
{
    int w = 200;
    lt_ui_layout_row(ui, 1, &w, 0);
    s_res |= lt_ui_text(ui, "t", s_buf, sizeof(s_buf), s_flags);
    R[0] = lt_ui_last_rect(ui);
    lt_ui_button(ui, "Other");
    R[1] = lt_ui_last_rect(ui);
}

LT_TEST(test_text_editing)
{
    lt_backend mb;
    char *cb;
    reset_state();
    memset(&mb, 0, sizeof(mb));
    mb.set_clipboard = mock_set; mb.get_clipboard = mock_get;
    g_clip[0] = '\0';
    h_open(400, 300, 0);
    lt_ui_set_backend(H.ui, &mb);
    H.scene = scene_text;
    frames(2);
    click_rect(R[0]);
    LT_CHECK(lt_ui_text_focused(H.ui));
    type_str("hello");
    LT_CHECK_STR(s_buf, "hello");
    LT_CHECK(s_res & LT_UI_CHANGED);
    key(LT_KEY_BACKSPACE);
    LT_CHECK_STR(s_buf, "hell");
    key(LT_KEY_HOME);
    type_str("X");
    LT_CHECK_STR(s_buf, "Xhell");
    key(LT_KEY_RIGHT);
    key(LT_KEY_DELETE);
    LT_CHECK_STR(s_buf, "Xhll");
    /* selection with shift+End, replace by typing */
    key(LT_KEY_HOME);
    key_ex(LT_KEY_END, LT_MOD_SHIFT);
    type_str("Z");
    LT_CHECK_STR(s_buf, "Z");
    /* ctrl+a / ctrl+c / paste */
    type_str("abc");
    key_ex('a', LT_MOD_CTRL);
    key_ex('c', LT_MOD_CTRL);
    LT_CHECK_STR(g_clip, "Zabc");
    key(LT_KEY_END);
    key_ex('v', LT_MOD_CTRL);
    LT_CHECK_STR(s_buf, "ZabcZabc");
    /* shift+left selection and cut */
    key_ex(LT_KEY_LEFT, LT_MOD_SHIFT);
    key_ex(LT_KEY_LEFT, LT_MOD_SHIFT);
    key_ex('x', LT_MOD_CTRL);
    LT_CHECK_STR(s_buf, "ZabcZa");
    LT_CHECK_STR(g_clip, "bc");
    /* word movement */
    key_ex(LT_KEY_LEFT, LT_MOD_CTRL);
    type_str("_");
    LT_CHECK_STR(s_buf, "_ZabcZa");
    /* mouse caret placement: "abcdef", click after 3 chars */
    key_ex('a', LT_MOD_CTRL);
    type_str("abcdef");
    click(R[0].x + lt_ui_theme(H.ui)->pad_x + lt_ui_text_width(H.ui, "abc") + 1, cy_of(R[0]));
    type_str("X");
    LT_CHECK_STR(s_buf, "abcXdef");
    /* Esc restores the text from focus time and drops focus */
    s_res = 0;
    key(LT_KEY_ESCAPE);
    LT_CHECK(s_res & LT_UI_CANCEL);
    LT_CHECK_STR(s_buf, "");
    LT_CHECK(!lt_ui_text_focused(H.ui));
    /* Enter submits */
    click_rect(R[0]);
    type_str("ok");
    s_res = 0;
    key(LT_KEY_ENTER);
    LT_CHECK(s_res & LT_UI_SUBMIT);
    LT_CHECK_STR(s_buf, "ok");
    /* click elsewhere leaves editing but keeps text */
    click_rect(R[1]);
    LT_CHECK(!lt_ui_text_focused(H.ui));
    LT_CHECK_STR(s_buf, "ok");
    /* numeric-only filter */
    s_buf[0] = '\0';
    s_flags = LT_UI_TEXT_NUMERIC;
    click_rect(R[0]);
    type_str("a1b2.5x");
    LT_CHECK_STR(s_buf, "12.5");
    /* capacity is respected */
    s_flags = 0; s_buf[0] = '\0';
    key_ex('a', LT_MOD_CTRL);
    {
        int i;
        for (i = 0; i < 80; i++) { type_str("m"); }
    }
    LT_CHECK(strlen(s_buf) == sizeof(s_buf) - 1);
    /* read-only text can be selected/copied but not edited */
    h_close();
    /* internal clipboard fallback */
    h_open(100, 100, 0);
    lt_ui_clipboard_set(H.ui, "fallback");
    cb = lt_ui_clipboard_get(H.ui);
    LT_CHECK(cb != NULL && strcmp(cb, "fallback") == 0);
    lt_free(cb);
    h_close();
}

/* ---- number field ------------------------------------------------ */
static void scene_number(lt_ui *ui)
{
    int w = 120;
    lt_ui_layout_row(ui, 1, &w, 0);
    s_res |= lt_ui_number(ui, "n", &s_val, 0.0, 10.0, 0.5, 1);
    R[0] = lt_ui_last_rect(ui);
    lt_ui_button(ui, "Other");
    R[1] = lt_ui_last_rect(ui);
}

LT_TEST(test_number_field_keeps_out_of_range_value)
{
    /* a value stored outside the INI range (an ECU that was never set up) is shown, not
     * rewritten: drawing the field must not report a change, or merely opening a dialog
     * would edit the tune */
    reset_state();
    s_val = -3.0;
    s_res = 0;
    h_open(400, 300, 0);
    H.scene = scene_number;
    frames(3);
    LT_CHECK_NEAR(s_val, -3.0, 1e-9);
    LT_CHECK(!(s_res & LT_UI_CHANGED));
    /* editing it still clamps */
    click_rect(R[0]);
    key(LT_KEY_UP);
    LT_CHECK(s_val >= 0.0 && s_val <= 10.0);
    LT_CHECK(s_res & LT_UI_CHANGED);
    h_close();
}

LT_TEST(test_number_field)
{
    reset_state();
    s_val = 5.0;
    h_open(400, 300, 0);
    H.scene = scene_number;
    frames(2);
    click_rect(R[0]);
    LT_CHECK(lt_ui_text_focused(H.ui));
    key(LT_KEY_UP);
    LT_CHECK_NEAR(s_val, 5.5, 1e-9);
    key(LT_KEY_DOWN);
    key(LT_KEY_DOWN);
    LT_CHECK_NEAR(s_val, 4.5, 1e-9);
    key_ex(LT_KEY_UP, LT_MOD_SHIFT);
    LT_CHECK_NEAR(s_val, 9.5, 1e-9);
    key(LT_KEY_UP);
    key(LT_KEY_UP);
    LT_CHECK_NEAR(s_val, 10.0, 1e-9);            /* clamped to max */
    LT_CHECK(s_res & LT_UI_CHANGED);
    /* typed value commits on Enter, clamped */
    type_str("3.25");
    s_res = 0;
    key(LT_KEY_ENTER);
    LT_CHECK_NEAR(s_val, 3.25, 1e-9);
    LT_CHECK(s_res & LT_UI_SUBMIT);
    LT_CHECK(!lt_ui_text_focused(H.ui));
    click_rect(R[0]);
    type_str("99");
    key(LT_KEY_ENTER);
    LT_CHECK_NEAR(s_val, 10.0, 1e-9);
    /* Esc cancels the edit */
    click_rect(R[0]);
    type_str("2");
    key(LT_KEY_ESCAPE);
    LT_CHECK_NEAR(s_val, 10.0, 1e-9);
    /* click away commits what was typed */
    click_rect(R[0]);
    type_str("7");
    click_rect(R[1]);
    LT_CHECK_NEAR(s_val, 7.0, 1e-9);
    /* drag scrubs the value */
    {
        int x = cx_of(R[0]), y = cy_of(R[0]);
        mouse_move(x, y); frame();
        mouse_down(x, y, LT_MB_LEFT, 0); frame();
        mouse_move(x - 12, y); frame();
        mouse_move(x - 18, y); frame();
        mouse_up(x - 18, y, LT_MB_LEFT, 0); frame(); frame();
        LT_CHECK_NEAR(s_val, 4.0, 1e-9);          /* 7 - 0.5 * 18 / 3 */
        LT_CHECK(!lt_ui_text_focused(H.ui));      /* a drag does not start text editing */
    }
    h_close();
}

/* ---- slider ------------------------------------------------------ */
static void scene_slider(lt_ui *ui)
{
    int w = 300;
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_slider(ui, "s", &s_val, 0.0, 100.0, 0)) { s_n[0]++; }
    R[0] = lt_ui_last_rect(ui);
}

LT_TEST(test_slider)
{
    int x0, y;
    reset_state();
    s_val = 10.0;
    h_open(400, 200, 0);
    H.scene = scene_slider;
    frames(2);
    x0 = R[0].x + 8;
    y = cy_of(R[0]);
    mouse_move(x0 + 40, y); frame();
    mouse_down(x0 + 40, y, LT_MB_LEFT, 0); frame();
    LT_CHECK(s_val > 10.0 && s_val < 50.0);
    mouse_move(R[0].x + 400, y); frame();
    LT_CHECK_NEAR(s_val, 100.0, 1e-9);
    mouse_move(x0 - 50, y); frame();
    LT_CHECK_NEAR(s_val, 0.0, 1e-9);
    mouse_up(x0 - 50, y, LT_MB_LEFT, 0); frame();
    key(LT_KEY_RIGHT);
    LT_CHECK_NEAR(s_val, 1.0, 1e-9);
    key_ex(LT_KEY_RIGHT, LT_MOD_SHIFT);
    LT_CHECK_NEAR(s_val, 6.0, 1e-9);
    key(LT_KEY_END);
    LT_CHECK_NEAR(s_val, 100.0, 1e-9);
    h_close();
}

/* ---- combo ------------------------------------------------------- */
static const char *items3[] = { "Alpha", "Beta", "Gamma" };
static void scene_combo(lt_ui *ui)
{
    int w = 160;
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_combo(ui, "c", &s_sel, items3, 3)) { s_n[0]++; }
    R[0] = lt_ui_last_rect(ui);
    if (lt_ui_button(ui, "Under")) { s_n[1]++; }
    R[1] = lt_ui_last_rect(ui);
}

static int combo_item_y(int i) { return R[0].y + R[0].h + 2 + 3 + i * 24 + 12; }

LT_TEST(test_combo)
{
    reset_state();
    h_open(400, 300, 0);
    H.scene = scene_combo;
    frames(3);
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    click(R[0].x + 30, combo_item_y(2));
    LT_CHECK_INT(s_sel, 2);
    LT_CHECK_INT(s_n[0], 1);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    /* Esc closes without change */
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    key(LT_KEY_ESCAPE);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    LT_CHECK_INT(s_sel, 2);
    /* keyboard keeps working on the focused closed combo */
    key(LT_KEY_UP);
    LT_CHECK_INT(s_sel, 1);
    key(LT_KEY_DOWN);
    key(LT_KEY_DOWN);
    LT_CHECK_INT(s_sel, 2);
    /* type-ahead */
    type_str("a");
    LT_CHECK_INT(s_sel, 0);
    H.t += 1500.0;
    type_str("b");
    LT_CHECK_INT(s_sel, 1);
    /* popup keyboard navigation: Enter on the highlighted item */
    key(' ');
    LT_CHECK(lt_ui_popup_open(H.ui));
    key(LT_KEY_DOWN);
    key(LT_KEY_ENTER);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    LT_CHECK_INT(s_sel, 2);
    /* a click outside closes the popup and does not fall through */
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    click_rect(R[1]);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    LT_CHECK_INT(s_n[1], 0);
    click_rect(R[1]);
    LT_CHECK_INT(s_n[1], 1);
    h_close();
}

/* ---- list box ---------------------------------------------------- */
static const char *list_get(void *u, int i)
{
    static char b[8][24];
    static int k;
    LT_UNUSED(u);
    k = (k + 1) & 7;
    lt_snprintf(b[k], sizeof(b[k]), "Item %d", i);
    return b[k];
}

static void scene_list(lt_ui *ui)
{
    int w = 200;
    lt_ui_layout_row(ui, 1, &w, 24 * 5);
    s_res |= lt_ui_listbox_cb(ui, "l", &s_sel, 40, list_get, NULL);
    R[0] = lt_ui_last_rect(ui);
}

LT_TEST(test_listbox)
{
    reset_state();
    h_open(400, 300, 0);
    H.scene = scene_list;
    s_sel = -1;
    frames(3);
    click(R[0].x + 20, R[0].y + 2 * 24 + 12);
    LT_CHECK_INT(s_sel, 2);
    LT_CHECK(s_res & LT_UI_CHANGED);
    s_res = 0;
    key(LT_KEY_DOWN);
    LT_CHECK_INT(s_sel, 3);
    key(LT_KEY_END);
    LT_CHECK_INT(s_sel, 39);
    key(LT_KEY_HOME);
    LT_CHECK_INT(s_sel, 0);
    key(LT_KEY_PAGEDOWN);
    LT_CHECK(s_sel >= 3);
    /* Enter activates */
    s_res = 0;
    key(LT_KEY_ENTER);
    LT_CHECK(s_res & LT_UI_ACTIVATED);
    /* double click activates */
    s_res = 0;
    click(R[0].x + 20, R[0].y + 12);
    click(R[0].x + 20, R[0].y + 12);
    LT_CHECK(s_res & LT_UI_ACTIVATED);
    /* wheel scrolls two rows per notch; first visible row is item 2 */
    key(LT_KEY_HOME);
    wheel(R[0].x + 20, R[0].y + 40, -1);
    click(R[0].x + 20, R[0].y + 12);
    LT_CHECK_INT(s_sel, 2);
    h_close();
}

/* ---- tree -------------------------------------------------------- */
static void scene_tree(lt_ui *ui)
{
    int w = -1, r;
    lt_ui_layout_row(ui, 1, &w, 0);
    lt_ui_layout_gap(ui, 8, 0);
    memset(&R[1], 0, sizeof(R[1]));
    r = lt_ui_tree_node(ui, "Folder", LT_UI_ICON_FOLDER, 0);
    R[0] = lt_ui_last_rect(ui);
    if (r & LT_UI_TREE_CLICKED) { s_n[0]++; }
    if (r & LT_UI_TREE_OPEN) {
        if (lt_ui_tree_node(ui, "Child", 0, LT_UI_TREE_LEAF) & LT_UI_TREE_CLICKED) { s_n[1]++; }
        R[1] = lt_ui_last_rect(ui);
        lt_ui_tree_end(ui);
    }
    lt_ui_tree_node(ui, "Other", 0, LT_UI_TREE_LEAF);
    R[2] = lt_ui_last_rect(ui);
}

LT_TEST(test_tree_view)
{
    reset_state();
    h_open(300, 300, 0);
    H.scene = scene_tree;
    frames(3);
    LT_CHECK(R[1].w == 0);
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 1);
    LT_CHECK(R[1].w > 0);                        /* expanded: child present */
    LT_CHECK(R[1].x == R[0].x && R[1].y > R[0].y);
    LT_CHECK(R[2].y > R[1].y);
    click_rect(R[1]);
    LT_CHECK_INT(s_n[1], 1);
    click_rect(R[0]);
    LT_CHECK(R[1].w == 0);                       /* collapsed again */
    /* keyboard: Right opens, Left closes, Down moves to the child */
    key(LT_KEY_RIGHT);
    LT_CHECK(R[1].w > 0);
    key(LT_KEY_DOWN);
    LT_CHECK(lt_ui_focus(H.ui) == lt_ui_id(H.ui, "Folder") ? 0 : 1);
    key(LT_KEY_UP);
    key(LT_KEY_LEFT);
    LT_CHECK(R[1].w == 0);
    h_close();
}

/* ---- tab bar ----------------------------------------------------- */
static void scene_tabs(lt_ui *ui)
{
    int r;
    lt_ui_tabbar_begin(ui, "tb", &s_active);
    lt_ui_tab(ui, "First", 0);
    R[0] = lt_ui_last_rect(ui);
    r = lt_ui_tab(ui, "Second", LT_UI_TAB_CLOSABLE);
    R[1] = lt_ui_last_rect(ui);
    if (r & LT_UI_TAB_CLOSED) { s_n[1]++; }
    r = lt_ui_tab(ui, "Third", LT_UI_TAB_CLOSABLE | LT_UI_TAB_DIRTY);
    R[2] = lt_ui_last_rect(ui);
    if (r & LT_UI_TAB_CLOSED) { s_n[2]++; }
    s_n[0] = lt_ui_tabbar_end(ui);
}

/* ------------------------------------------------------------------ */
/* proportional text, styles, Lucide icons, stock themes, shell widgets  */
static void scene_shell(lt_ui *ui)
{
    int w = -1, r;
    static const char *const labs[3] = { "General", "Appearance", "Keys" };
    lt_ui_layout_row(ui, 1, &w, 0);
    lt_ui_layout_gap(ui, 0, 0);
    s_n[3] += lt_ui_tabs_flat(ui, "flat", labs, 3, &s_active) ? 1 : 0;
    R[0] = lt_ui_last_rect(ui);
    lt_ui_layout_row(ui, 1, &w, 20);
    r = lt_ui_tree_node_ex(ui, "Root", LT_UI_LUCIDE(LT_ICON_WRENCH), LT_RGB(255, 152, 0), 0);
    R[1] = lt_ui_last_rect(ui);
    memset(&R[2], 0, sizeof(R[2]));
    if (r & LT_UI_TREE_OPEN) {
        lt_ui_tree_node_ex(ui, "Leaf", LT_UI_LUCIDE(LT_ICON_GAUGE), LT_RGB(0, 200, 0), LT_UI_TREE_LEAF);
        R[2] = lt_ui_last_rect(ui);
        lt_ui_tree_end(ui);
    }
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_SETTINGS), "Settings", 0)) { s_n[4]++; }
    R[3] = lt_ui_last_rect(ui);
}

LT_TEST(test_shell_widgets)
{
    int w_i, w_w, w_style;
    lt_theme t;
    int i;
    static const char *const names[9] = { "dark", "light", "midnight", "carbon", "synthwave",
                                          "solarized", "nord", "dracula", "highcontrast" };
    reset_state();
    h_open(500, 300, 0);
    H.scene = scene_shell;
    frames(3);
    /* proportional Plex: narrow letters are narrower, widths follow the style */
    w_i = lt_ui_text_width(H.ui, "iiiiii");
    w_w = lt_ui_text_width(H.ui, "WWWWWW");
    LT_CHECK(w_i > 0 && w_i < w_w);
    LT_CHECK(lt_ui_text_width_f(H.ui, "Hello", (int)LT_FONT_SANS, LT_UI_FS_BASE) == lt_ui_text_width(H.ui, "Hello"));
    w_style = lt_ui_text_width_f(H.ui, "Hello", (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_XL);
    LT_CHECK(w_style > lt_ui_text_width(H.ui, "Hello"));
    lt_ui_text_style(H.ui, (int)LT_FONT_SANS, LT_UI_FS_XL);
    LT_CHECK(lt_ui_text_width(H.ui, "Hello") > w_w / 2);
    lt_ui_text_style_reset(H.ui);
    LT_CHECK(lt_ui_text_width(H.ui, "") == 0);
    /* flat tabs: clicking a label selects it */
    s_active = 0;
    click(R[0].x + 5, cy_of(R[0]));               /* R[0] is the last label ("Keys") */
    LT_CHECK_INT(s_active, 2);
    LT_CHECK_INT(s_n[3], 1);
    /* tree node with a Lucide icon opens on click; expand-all reaches closed nodes */
    LT_CHECK(R[2].w == 0);
    click_rect(R[1]);
    LT_CHECK(R[2].w > 0);
    click_rect(R[1]);
    LT_CHECK(R[2].w == 0);
    lt_ui_tree_set_all(H.ui, 1);
    frames(10);
    LT_CHECK(R[2].w > 0);
    lt_ui_tree_set_all(H.ui, 0);
    frames(10);
    LT_CHECK(R[2].w == 0);
    /* Lucide icon button is clickable and draws ink */
    click_rect(R[3]);
    LT_CHECK_INT(s_n[4], 1);
    {
        int ink = 0, x, y;
        for (y = R[3].y; y < R[3].y + R[3].h; y++) {
            for (x = R[3].x; x < R[3].x + R[3].w; x++) {
                if (px(x, y) != px(R[3].x - 2, R[3].y)) { ink++; }
            }
        }
        LT_CHECK(ink > 20);
    }
    h_close();
    /* stock themes: every name resolves; values are the LibreTune tokens */
    for (i = 0; i < 9; i++) {
        LT_CHECK(lt_theme_preset(&t, names[i]) == 0);
        LT_CHECK(strcmp(t.name, names[i]) == 0);
        LT_CHECK(t.row_h == 24 && t.tree_row_h == 20);
    }
    LT_CHECK(lt_theme_preset(&t, "nope") == -1);
    lt_theme_preset(&t, "light");
    LT_CHECK((t.bg_app & 0xFFFFFFUL) == 0xF5F5F5UL && !t.dark);
    lt_theme_preset(&t, "midnight");
    LT_CHECK((t.primary & 0xFFFFFFUL) == 0x238636UL && t.dark);
    LT_CHECK((t.bg_toolbar & 0xFFFFFFUL) == 0x0D1117UL);
    /* cell mode keeps the 8x16 grid for widths */
    h_open(400, 160, 1);
    H.scene = scene_shell;
    frames(2);
    LT_CHECK(lt_ui_text_width(H.ui, "Hello") == 5 * 8);
    h_close();
}

LT_TEST(test_tab_bar)
{
    reset_state();
    h_open(500, 200, 0);
    H.scene = scene_tabs;
    frames(3);
    LT_CHECK_INT(s_n[0], 0);
    click(R[1].x + 15, cy_of(R[1]));
    LT_CHECK_INT(s_active, 1);
    LT_CHECK_INT(s_n[0], 1);
    click(R[2].x + 15, cy_of(R[2]));
    LT_CHECK_INT(s_active, 2);
    /* close button of tab 2 */
    click(R[1].x + R[1].w - 13, cy_of(R[1]));
    LT_CHECK_INT(s_n[1], 1);
    LT_CHECK_INT(s_active, 2);                    /* closing does not select */
    /* middle click closes too */
    click_ex(R[2].x + 20, cy_of(R[2]), LT_MB_MIDDLE, 0);
    LT_CHECK_INT(s_n[2], 1);
    h_close();
}

/* ---- menus ------------------------------------------------------- */
static void scene_menu(lt_ui *ui)
{
    int open;
    s_menu_edit_open = 0; s_menu_sub_open = 0;
    if (lt_ui_menubar_begin(ui)) {
        open = lt_ui_menu_begin(ui, "&File");
        R[0] = lt_ui_last_rect(ui);
        if (open) {
            if (lt_ui_menu_item(ui, "New", "Ctrl+N", 0)) { s_n[0]++; }
            R[1] = lt_ui_last_rect(ui);
            if (lt_ui_menu_item(ui, "Open", NULL, LT_UI_MENU_DISABLED)) { s_n[1]++; }
            R[2] = lt_ui_last_rect(ui);
            lt_ui_menu_separator(ui);
            if (lt_ui_submenu_begin(ui, "Recent", 0)) {
                s_menu_sub_open = 1;
                if (lt_ui_menu_item(ui, "One", NULL, LT_UI_MENU_CHECKED)) { s_n[2]++; }
                R[5] = lt_ui_last_rect(ui);
                lt_ui_submenu_end(ui);
            }
            R[3] = lt_ui_last_rect(ui);
            if (lt_ui_menu_item(ui, "Quit", NULL, 0)) { s_n[3]++; }
            R[4] = lt_ui_last_rect(ui);
            lt_ui_menu_end(ui);
        }
        if (lt_ui_menu_begin(ui, "&Edit")) {
            s_menu_edit_open = 1;
            lt_ui_menu_item(ui, "Copy", NULL, 0);
            lt_ui_menu_end(ui);
        }
        R[6] = lt_ui_last_rect(ui);
        lt_ui_menubar_end(ui);
    }
    lt_ui_layout_set_rect(ui, lt_rect_make(250, 200, 80, 24));
    if (lt_ui_button(ui, "Under")) { s_n[4]++; }
    R[7] = lt_ui_last_rect(ui);
}

LT_TEST(test_menus)
{
    reset_state();
    h_open(500, 400, 0);
    H.scene = scene_menu;
    frames(3);
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    LT_CHECK(R[1].w > 0 && R[1].y > R[0].y);
    /* disabled items never fire and keep the menu open */
    click_rect(R[2]);
    LT_CHECK_INT(s_n[1], 0);
    LT_CHECK(lt_ui_popup_open(H.ui));
    /* separator makes Quit lower than a plain row */
    LT_CHECK(R[4].y > R[3].y);
    click_rect(R[1]);
    LT_CHECK_INT(s_n[0], 1);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    /* Esc closes */
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    key(LT_KEY_ESCAPE);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    /* hover switches to the neighbouring menu while one is open */
    click_rect(R[0]);
    mouse_move(cx_of(R[6]), cy_of(R[6])); frames(3);
    LT_CHECK(s_menu_edit_open);
    LT_CHECK(!s_menu_sub_open);
    key(LT_KEY_ESCAPE);
    /* submenu opens on hover and its item can be clicked */
    click_rect(R[0]);
    mouse_move(cx_of(R[3]), cy_of(R[3])); frames(3);
    mouse_move(cx_of(R[3]) + 1, cy_of(R[3])); frames(3);
    LT_CHECK(s_menu_sub_open);
    click_rect(R[5]);
    LT_CHECK_INT(s_n[2], 1);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    /* clicking a widget under the open menu only closes the menu */
    click_rect(R[0]);
    click_rect(R[7]);
    LT_CHECK_INT(s_n[4], 0);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    click_rect(R[7]);
    LT_CHECK_INT(s_n[4], 1);
    /* keyboard: Down highlights first enabled item, Enter activates */
    click_rect(R[0]);
    key(LT_KEY_DOWN);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[0], 2);
    /* Left/Right moves between menus */
    click_rect(R[0]);
    key(LT_KEY_RIGHT);
    LT_CHECK(s_menu_edit_open);
    key(LT_KEY_ESCAPE);
    /* Alt+E opens Edit */
    key_ex('e', LT_MOD_ALT);
    LT_CHECK(s_menu_edit_open);
    h_close();
}

/* a menu longer than the window stays under its title and scrolls (keys and wheel) */
static void scene_long_menu(lt_ui *ui)
{
    int i, open;
    if (lt_ui_menubar_begin(ui)) {
        open = lt_ui_menu_begin(ui, "&File");
        R[0] = lt_ui_last_rect(ui);
        if (open) {
            for (i = 0; i < 30; i++) {
                char b[16];
                lt_snprintf(b, sizeof(b), "Item %d", i);
                if (lt_ui_menu_item(ui, b, NULL, 0)) { s_n[0] = 100 + i; }
                if (i == 0) { R[1] = lt_ui_last_rect(ui); }
            }
            if (lt_ui_menu_item(ui, "Exit", NULL, 0)) { s_n[1]++; }
            R[2] = lt_ui_last_rect(ui);
            lt_ui_menu_end(ui);
        }
        lt_ui_menubar_end(ui);
    }
}

LT_TEST(test_menu_taller_than_window_scrolls)
{
    reset_state();
    h_open(400, 300, 0);
    H.scene = scene_long_menu;
    frames(2);
    key_ex('f', LT_MOD_ALT);
    frames(2);
    LT_CHECK(R[1].y >= R[0].y + R[0].h);                  /* does not cover the menu bar */
    LT_CHECK(R[2].y > 300);                               /* Exit is below the window */
    key(LT_KEY_UP);                                       /* wraps to the last item */
    frames(2);
    LT_CHECK(R[2].y >= R[0].y + R[0].h && R[2].y + R[2].h <= 300);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[1], 1);
    /* the wheel scrolls; a row scrolled into view can be clicked */
    key_ex('f', LT_MOD_ALT);
    frames(2);
    LT_CHECK(R[2].y > 300);
    wheel(R[1].x + 10, 150, -30);
    LT_CHECK(R[2].y + R[2].h <= 300);
    LT_CHECK(R[1].y + R[1].h <= R[0].y + R[0].h);
    click_rect(R[2]);
    LT_CHECK_INT(s_n[1], 2);
    h_close();
    /* terminal: 80x12 cells */
    reset_state();
    h_open(80, 12, 1);
    H.scene = scene_long_menu;
    frames(2);
    key_ex('f', LT_MOD_ALT);
    key(LT_KEY_UP);
    frames(2);
    LT_CHECK(R[2].y + R[2].h <= 12 * 16);
    LT_CHECK(cells_has_text("Exit"));
    h_close();
}

static void scene_context(lt_ui *ui)
{
    R[0] = lt_rect_make(100, 100, 200, 100);
    lt_ui_draw_fill(ui, R[0], 0xFF303030UL);
    if (lt_ui_context_begin(ui, "ctx", R[0])) {
        if (lt_ui_menu_item(ui, "Copy", NULL, 0)) { s_n[0]++; }
        R[1] = lt_ui_last_rect(ui);
        if (lt_ui_menu_item(ui, "Paste", NULL, 0)) { s_n[1]++; }
        R[2] = lt_ui_last_rect(ui);
        lt_ui_context_end(ui);
    }
}

LT_TEST(test_context_menu)
{
    reset_state();
    h_open(500, 400, 0);
    H.scene = scene_context;
    frames(3);
    click_ex(10, 10, LT_MB_RIGHT, 0);            /* outside the area */
    LT_CHECK(!lt_ui_popup_open(H.ui));
    click_ex(150, 120, LT_MB_RIGHT, 0);
    LT_CHECK(lt_ui_popup_open(H.ui));
    LT_CHECK(R[1].x >= 150 && R[1].y >= 120);
    click_rect(R[2]);
    LT_CHECK_INT(s_n[1], 1);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    h_close();
}

/* ---- modal dialogs ----------------------------------------------- */
static void scene_modal(lt_ui *ui)
{
    if (lt_ui_button(ui, "Under")) { s_n[0]++; }
    R[0] = lt_ui_last_rect(ui);
    if (lt_ui_modal_begin(ui, "Dlg", 300, 150, &s_open)) {
        if (lt_ui_button_ex(ui, "OK", LT_UI_BTN_DEFAULT)) { s_n[1]++; s_open = 0; }
        R[1] = lt_ui_last_rect(ui);
        if (lt_ui_button(ui, "Cancel")) { s_n[2]++; s_open = 0; }
        R[2] = lt_ui_last_rect(ui);
        lt_ui_modal_end(ui);
    }
}

LT_TEST(test_modal)
{
    reset_state();
    h_open(500, 400, 0);
    H.scene = scene_modal;
    frames(3);
    s_open = 1;
    frames(3);
    click_rect(R[0]);                            /* blocked by the modal */
    LT_CHECK_INT(s_n[0], 0);
    LT_CHECK(s_open);
    click(5, 5);                                 /* dimmed backdrop does not close */
    LT_CHECK(s_open);
    key(LT_KEY_ESCAPE);
    LT_CHECK(!s_open);
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 1);
    /* Enter triggers the default button */
    s_open = 1; frames(3);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[1], 1);
    LT_CHECK(!s_open);
    /* clicking Cancel */
    s_open = 1; frames(3);
    click_rect(R[2]);
    LT_CHECK_INT(s_n[2], 1);
    LT_CHECK(!s_open);
    /* close button in the title bar */
    s_open = 1; frames(3);
    click(250 + 150 - 17, 200 - 75 + 15);
    LT_CHECK(!s_open);
    /* the window is centered */
    s_open = 1; frames(3);
    LT_CHECK(R[1].x > 100 && R[1].x < 250 && R[1].y > 125 && R[1].y < 275);
    h_close();
}

/* a dialog taller than the window: the body scrolls, so the buttons under the list stay
 * reachable; with room to spare the fill row takes the slack and nothing scrolls */
static void scene_modal_tall(lt_ui *ui)
{
    int w = -1, i;
    if (lt_ui_modal_begin(ui, "Tall", 300, 600, &s_open)) {
        for (i = 0; i < 8; i++) {
            char b[16];
            lt_snprintf(b, sizeof(b), "Field %d", i);
            lt_ui_layout_row(ui, 1, &w, 0);
            lt_ui_label(ui, b);
        }
        lt_ui_layout_row(ui, 1, &w, -(24 + 4 + 1));
        lt_ui_label(ui, "list");
        R[3] = lt_ui_last_rect(ui);
        lt_ui_layout_row(ui, 1, &w, 0);
        if (lt_ui_button(ui, "Create")) { s_n[0]++; }
        R[0] = lt_ui_last_rect(ui);
        s_n[7] = (int)lt_ui_last_id(ui);
        R[1] = lt_ui_clip_get(ui);
        lt_ui_modal_end(ui);
    }
}

LT_TEST(test_modal_taller_than_window_scrolls)
{
    reset_state();
    h_open(500, 240, 0);
    H.scene = scene_modal_tall;
    s_open = 1;
    frames(4);
    LT_CHECK(R[0].y + R[0].h > 240);                       /* below the window at first */
    LT_CHECK_INT(R[3].h, 24);                              /* the fill row shrank to one row */
    wheel(250, 120, -20);
    LT_CHECK(R[0].y >= R[1].y && R[0].y + R[0].h <= R[1].y + R[1].h);
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 1);
    /* keyboard only: Tab to the button scrolls it into view */
    wheel(250, 120, 20);
    LT_CHECK(R[0].y + R[0].h > 240);
    key(LT_KEY_TAB);
    LT_CHECK(lt_ui_focus(H.ui) == (lt_u32)s_n[7]);
    frames(2);
    LT_CHECK(R[0].y >= R[1].y && R[0].y + R[0].h <= R[1].y + R[1].h);
    h_close();
    /* a window with room: no scrolling, the fill row grows */
    reset_state();
    h_open(500, 800, 0);
    H.scene = scene_modal_tall;
    s_open = 1;
    frames(4);
    LT_CHECK(R[3].h > 100);
    LT_CHECK(R[0].y + R[0].h <= 400 + 300);
    LT_CHECK_INT(H.ui->nscr, 0);
    h_close();
    /* the terminal too (80x8 cells) */
    reset_state();
    h_open(80, 8, 1);
    H.scene = scene_modal_tall;
    s_open = 1;
    frames(4);
    wheel(40 * 8, 4 * 16, 20);
    LT_CHECK(R[0].y + R[0].h > R[1].y + R[1].h);
    key(LT_KEY_TAB);
    frames(2);
    LT_CHECK(R[0].y >= R[1].y && R[0].y + R[0].h <= R[1].y + R[1].h);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[0], 1);
    h_close();
}

static void scene_msgbox(lt_ui *ui)
{
    int r = lt_ui_msgbox(ui, "m", "Confirm", "Save the changes before closing?", "Save|Discard|Cancel", &s_open);
    if (r != 0) { s_res = r; }
}

LT_TEST(test_msgbox)
{
    reset_state();
    h_open(600, 400, 0);
    H.scene = scene_msgbox;
    frames(2);
    s_open = 1; frames(4);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_res, 1);
    LT_CHECK(!s_open);
    s_open = 1; s_res = 0; frames(4);
    key(LT_KEY_TAB);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_res, 2);
    s_open = 1; s_res = 0; frames(4);
    key(LT_KEY_ESCAPE);
    LT_CHECK_INT(s_res, -1);
    LT_CHECK(!s_open);
    h_close();
}

/* ---- floating windows -------------------------------------------- */
static void scene_windows(lt_ui *ui)
{
    if (lt_ui_window_begin(ui, "WinA", &s_wr, &s_open, LT_UI_WIN_RESIZE)) {
        lt_ui_label(ui, "content A");
        lt_ui_window_end(ui);
    }
    if (lt_ui_window_begin(ui, "WinB", &s_wr2, &s_open2, 0)) {
        lt_ui_label(ui, "content B");
        lt_ui_window_end(ui);
    }
}

LT_TEST(test_windows)
{
    lt_u32 ida, idb;
    reset_state();
    h_open(600, 500, 0);
    H.scene = scene_windows;
    s_wr = lt_rect_make(100, 100, 200, 150);
    s_wr2 = lt_rect_make(500, 100, 80, 80);
    s_open = 1; s_open2 = 0;
    frames(3);
    /* drag by the title bar */
    mouse_move(150, 112); frame();
    mouse_down(150, 112, LT_MB_LEFT, 0); frame();
    mouse_move(250, 212); frame();
    mouse_up(250, 212, LT_MB_LEFT, 0); frame(); frame();
    LT_CHECK(s_wr.x == 200 && s_wr.y == 200);
    /* resize from the corner */
    {
        int gx = s_wr.x + s_wr.w - 8, gy = s_wr.y + s_wr.h - 8;
        mouse_move(gx, gy); frame();
        mouse_down(gx, gy, LT_MB_LEFT, 0); frame();
        mouse_move(gx + 40, gy + 30); frame();
        mouse_up(gx + 40, gy + 30, LT_MB_LEFT, 0); frame(); frame();
        LT_CHECK(s_wr.w == 240 && s_wr.h == 180);
    }
    /* close button */
    click(s_wr.x + s_wr.w - 17, s_wr.y + 15);
    LT_CHECK_INT(s_open, 0);
    /* z order: clicking the lower window raises it */
    s_open = 1; s_open2 = 1;
    s_wr = lt_rect_make(100, 300, 200, 150);
    s_wr2 = lt_rect_make(150, 330, 200, 150);
    frames(3);
    ida = lt_ui_id(H.ui, "WinA") ^ 0x57494EUL;
    idb = lt_ui_id(H.ui, "WinB") ^ 0x57494EUL;
    LT_UNUSED(idb);
    click(110, 340);                             /* only WinA is under the pointer here */
    LT_CHECK(H.ui->win_order[H.ui->nwin - 1] == ida);
    h_close();
}

/* ---- scroll area ------------------------------------------------- */
static void scene_scroll(lt_ui *ui)
{
    int w = -1, i;
    lt_ui_layout_row(ui, 1, &w, 100);
    if (lt_ui_scroll_begin(ui, "sc")) {
        R[0] = lt_ui_scroll_view(ui);
        for (i = 0; i < 12; i++) {
            char b[24];
            lt_snprintf(b, sizeof(b), "Row %d", i);
            lt_ui_layout_row(ui, 1, &w, 0);
            if (lt_ui_button(ui, b) && i == 5) { s_n[5]++; }
            if (i == 5) { R[1] = lt_ui_last_rect(ui); }
        }
        lt_ui_scroll_get(ui, NULL, &s_sy);
        lt_ui_scroll_end(ui);
    }
}

LT_TEST(test_scroll_area)
{
    int y0;
    reset_state();
    h_open(400, 300, 0);
    H.scene = scene_scroll;
    frames(4);
    LT_CHECK_INT(s_sy, 0);
    y0 = R[1].y;
    wheel(R[0].x + 30, R[0].y + 20, -1);
    LT_CHECK_INT(s_sy, 48);
    LT_CHECK_INT(R[1].y, y0 - 48);
    wheel(R[0].x + 30, R[0].y + 20, 1);
    LT_CHECK_INT(s_sy, 0);
    wheel(R[0].x + 30, R[0].y + 20, -50);
    /* 12 rows of 24 + 11 gaps of 4 = 332 content, view 100 */
    LT_CHECK_INT(s_sy, 332 - 100);
    /* wheel outside the area does nothing */
    wheel(350, 250, 1);
    LT_CHECK_INT(s_sy, 232);
    /* scrollbar thumb drag */
    {
        int x = R[0].x + R[0].w - 4;
        mouse_move(x, R[0].y + 95); frame();
        mouse_down(x, R[0].y + 95, LT_MB_LEFT, 0); frame();
        mouse_move(x, R[0].y + 5); frame();
        mouse_up(x, R[0].y + 5, LT_MB_LEFT, 0); frame(); frame();
        LT_CHECK(s_sy < 50);
    }
    /* a clipped-out widget cannot be clicked */
    wheel(R[0].x + 30, R[0].y + 20, 20);
    click(R[1].x + 5, R[1].y + 5);
    LT_CHECK_INT(s_n[5], 0);
    /* but a visible one can after scrolling to it */
    wheel(R[0].x + 30, R[0].y + 20, -3);
    click_rect(R[1]);
    LT_CHECK_INT(s_n[5], 1);
    h_close();
}

/* Tab through a scroll area: the focused widget is scrolled into view, both ways */
static void scene_scroll_focus(lt_ui *ui)
{
    int w = -1, i;
    lt_ui_layout_row(ui, 1, &w, 100);
    if (lt_ui_scroll_begin(ui, "sc")) {
        R[0] = lt_ui_scroll_view(ui);
        for (i = 0; i < 12; i++) {
            char b[24];
            lt_snprintf(b, sizeof(b), "Row %d", i);
            lt_ui_layout_row(ui, 1, &w, 0);
            (void)lt_ui_button(ui, b);
            if (lt_ui_last_focused(ui)) { R[1] = lt_ui_last_rect(ui); }
        }
        lt_ui_scroll_end(ui);
    }
}

LT_TEST(test_tab_scrolls_focus_into_view)
{
    int i;
    reset_state();
    h_open(400, 300, 0);
    H.scene = scene_scroll_focus;
    frames(3);
    for (i = 0; i < 12; i++) {
        key(LT_KEY_TAB);
        frame();                                  /* the scroll lands one frame later */
        LT_CHECK(R[1].y >= R[0].y && R[1].y + R[1].h <= R[0].y + R[0].h);
    }
    for (i = 0; i < 11; i++) {
        key_ex(LT_KEY_TAB, LT_MOD_SHIFT);
        frame();
        LT_CHECK(R[1].y >= R[0].y && R[1].y + R[1].h <= R[0].y + R[0].h);
    }
    /* the wheel still scrolls away from the focused widget */
    wheel(R[0].x + 30, R[0].y + 20, -50);
    LT_CHECK(R[1].y + R[1].h <= R[0].y);
    h_close();
}

/* ---- splitter ---------------------------------------------------- */
static int s_split;
static void scene_split(lt_ui *ui)
{
    lt_rect a, b;
    lt_ui_splitter(ui, "sp", lt_rect_make(0, 0, 400, 200), 1, &s_split, 60, 80, &a, &b);
    R[0] = a; R[1] = b;
}

LT_TEST(test_splitter)
{
    reset_state();
    s_split = 100;
    h_open(400, 200, 0);
    H.scene = scene_split;
    frames(3);
    LT_CHECK(R[0].w == 100 && R[1].x == 100 + lt_ui_theme(H.ui)->splitter_w);
    mouse_move(102, 50); frame();
    mouse_down(102, 50, LT_MB_LEFT, 0); frame();
    mouse_move(202, 50); frame();
    LT_CHECK_INT(s_split, 200);
    mouse_move(5, 50); frame();
    LT_CHECK_INT(s_split, 60);                   /* min_a */
    mouse_move(399, 50); frame();
    LT_CHECK_INT(s_split, 400 - 80 - lt_ui_theme(H.ui)->splitter_w);   /* min_b */
    mouse_up(399, 50, LT_MB_LEFT, 0); frame();
    LT_CHECK(R[0].w == s_split && R[0].w + lt_ui_theme(H.ui)->splitter_w + R[1].w == 400);
    h_close();
}

/* ---- layout ------------------------------------------------------ */
LT_TEST(test_layout)
{
    int w[3];
    lt_rect r0, r1, r2, r3, rr;
    int sp;
    reset_state();
    h_open(800, 600, 0);
    lt_ui_begin(H.ui, &H.g, 0.0);
    sp = lt_ui_theme(H.ui)->spacing_x;
    w[0] = 100; w[1] = -1; w[2] = -3;
    lt_ui_layout_row(H.ui, 3, w, 30);
    r0 = lt_ui_layout_next(H.ui);
    r1 = lt_ui_layout_next(H.ui);
    r2 = lt_ui_layout_next(H.ui);
    LT_CHECK(r0.x == 0 && r0.y == 0 && r0.w == 100 && r0.h == 30);
    LT_CHECK(r1.x == 100 + sp && r1.w == (800 - 100 - 2 * sp) / 4);
    LT_CHECK(r2.x + r2.w == 800 && r2.w == 3 * r1.w);
    /* next row starts below with the vertical spacing */
    lt_ui_layout_row(H.ui, 1, NULL, 0);
    r3 = lt_ui_layout_next(H.ui);
    LT_CHECK(r3.y == 30 + lt_ui_theme(H.ui)->spacing_y && r3.w == 800 && r3.h == 24);
    /* explicit rectangle override */
    lt_ui_layout_set_rect(H.ui, lt_rect_make(10, 20, 33, 44));
    rr = lt_ui_layout_next(H.ui);
    LT_CHECK(rr.x == 10 && rr.y == 20 && rr.w == 33 && rr.h == 44);
    /* fill-height row */
    lt_ui_layout_row(H.ui, 1, NULL, -1);
    r3 = lt_ui_layout_next(H.ui);
    LT_CHECK(r3.y + r3.h == 600);
    /* indent and vertical space */
    lt_ui_layout_row(H.ui, 1, NULL, 0);
    lt_ui_layout_indent(H.ui, 16);
    r3 = lt_ui_layout_next(H.ui);
    LT_CHECK(r3.x == 16 && r3.w == 800 - 16);
    lt_ui_layout_indent(H.ui, -16);
    lt_ui_end(H.ui);
    /* auto-sized label: width follows the text */
    H.scene = NULL;
    lt_ui_begin(H.ui, &H.g, 1.0);
    w[0] = 0; w[1] = -1;
    lt_ui_layout_row(H.ui, 2, w, 0);
    lt_ui_label(H.ui, "Hello");
    r0 = lt_ui_last_rect(H.ui);
    LT_CHECK(r0.w == lt_ui_text_width(H.ui, "Hello"));
    lt_ui_label(H.ui, "rest");
    r1 = lt_ui_last_rect(H.ui);
    LT_CHECK(r1.x == r0.w + sp && r1.x + r1.w == 800);
    /* nested column */
    lt_ui_layout_row(H.ui, 2, (w[0] = 200, w[1] = -1, w), 100);
    lt_ui_layout_column_begin(H.ui);
    lt_ui_layout_row(H.ui, 1, NULL, 20);
    r0 = lt_ui_layout_next(H.ui);
    r1 = lt_ui_layout_next(H.ui);
    lt_ui_layout_column_end(H.ui);
    LT_CHECK(r0.w == 200 && r1.y == r0.y + 20 + sp - sp + lt_ui_theme(H.ui)->spacing_y);
    lt_ui_end(H.ui);
    h_close();
}

/* ---- grid -------------------------------------------------------- */
#define GR 8
#define GC 8
static int g_rows = GR, g_cols = GC;
static char g_set_text[16];
static void scene_grid(lt_ui *ui)
{
    int w = 400, fl;
    lt_ui_layout_row(ui, 1, &w, 24 + 8 * 20 + 2);
    if (lt_ui_grid_begin(ui, "g", g_rows, g_cols, 40, 20, 30, 24)) {
        int r0, r1, c0, c1, r, c;
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        s_n[6] = r1 - r0 + 1;
        s_n[7] = c1 - c0 + 1;
        R[0] = lt_ui_grid_cell_rect(ui, 0, 0);
        R[1] = lt_ui_grid_cell_rect(ui, 2, 3);
        R[2] = lt_ui_grid_cell_rect(ui, 4, 5);
        R[3] = lt_ui_grid_cell_rect(ui, 1, 1);
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) { lt_ui_grid_cell(ui, r, c, "1", 0xFF252525UL, 0xFFFFFFFFUL); }
        }
        fl = lt_ui_grid_end(ui);
        s_gf |= fl;
        if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
            lt_ui_grid_edit_set_text(ui, g_set_text[0] ? g_set_text : "12");
        }
        if (fl & LT_UI_GRID_EDIT_COMMIT) { lt_strlcpy(s_buf, lt_ui_grid_edit_text(ui), sizeof(s_buf)); }
    }
}

static int gsel(int *r0, int *c0, int *r1, int *c1) { return lt_ui_grid_sel_bounds(H.ui, r0, c0, r1, c1); }

LT_TEST(test_grid_selection)
{
    int r0, c0, r1, c1, cr, cc, n;
    reset_state();
    g_rows = GR; g_cols = GC; g_set_text[0] = '\0';
    h_open(500, 300, 0);
    H.scene = scene_grid;
    frames(3);
    LT_CHECK_INT(s_n[6], 8);
    LT_CHECK_INT(s_n[7], 8);
    LT_CHECK(R[1].x == R[0].x + 3 * 40 && R[1].y == R[0].y + 2 * 20);
    click_rect(R[1]);
    LT_CHECK(lt_ui_grid_selected(H.ui, 2, 3));
    n = gsel(&r0, &c0, &r1, &c1);
    LT_CHECK(n == 1 && r0 == 2 && c0 == 3);
    LT_CHECK(s_gf & LT_UI_GRID_SELECTION);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 2 && cc == 3);
    /* shift-click selects the rectangle between anchor and cell */
    click_ex(cx_of(R[2]), cy_of(R[2]), LT_MB_LEFT, LT_MOD_SHIFT);
    n = gsel(&r0, &c0, &r1, &c1);
    LT_CHECK(n == 9 && r0 == 2 && r1 == 4 && c0 == 3 && c1 == 5);
    /* ctrl-click toggles a single extra cell */
    click_ex(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, LT_MOD_CTRL);
    LT_CHECK(gsel(NULL, NULL, NULL, NULL) == 10 && lt_ui_grid_selected(H.ui, 0, 0));
    click_ex(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, LT_MOD_CTRL);
    LT_CHECK(gsel(NULL, NULL, NULL, NULL) == 9 && !lt_ui_grid_selected(H.ui, 0, 0));
    /* plain click resets */
    click_rect(R[3]);
    LT_CHECK(gsel(&r0, &c0, &r1, &c1) == 1 && r0 == 1 && c0 == 1);
    /* drag a rectangle */
    mouse_move(cx_of(R[0]), cy_of(R[0])); frame();
    mouse_down(cx_of(R[0]), cy_of(R[0]), LT_MB_LEFT, 0); frame();
    mouse_move(cx_of(R[3]) + 40, cy_of(R[3]) + 1); frame();
    mouse_up(cx_of(R[3]) + 40, cy_of(R[3]) + 1, LT_MB_LEFT, 0); frame(); frame();
    n = gsel(&r0, &c0, &r1, &c1);
    LT_CHECK(n == 6 && r0 == 0 && r1 == 1 && c0 == 0 && c1 == 2);
    /* keyboard: arrows move, shift extends */
    click_rect(R[3]);
    key(LT_KEY_RIGHT);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 1 && cc == 2 && gsel(NULL, NULL, NULL, NULL) == 1);
    key(LT_KEY_DOWN);
    key_ex(LT_KEY_RIGHT, LT_MOD_SHIFT);
    LT_CHECK(gsel(&r0, &c0, &r1, &c1) == 2 && r0 == 2 && c0 == 2 && c1 == 3);
    key_ex(LT_KEY_DOWN, LT_MOD_SHIFT);
    LT_CHECK(gsel(&r0, &c0, &r1, &c1) == 4 && r0 == 2 && r1 == 3);
    key_ex(LT_KEY_LEFT, LT_MOD_SHIFT);
    LT_CHECK(gsel(&r0, &c0, &r1, &c1) == 2 && c0 == 2 && c1 == 2);
    key(LT_KEY_HOME);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cc == 0);
    key_ex(LT_KEY_HOME, LT_MOD_CTRL);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 0 && cc == 0);
    key(LT_KEY_UP);                               /* clamps */
    key(LT_KEY_LEFT);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 0 && cc == 0);
    key(LT_KEY_PAGEDOWN);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 7 && cc == 0);                 /* one visible page, clamped */
    key_ex(LT_KEY_END, LT_MOD_CTRL);
    lt_ui_grid_cursor(H.ui, &cr, &cc);
    LT_CHECK(cr == 7 && cc == 7);
    key_ex('a', LT_MOD_CTRL);
    LT_CHECK(gsel(NULL, NULL, NULL, NULL) == 64);
    /* header clicks select whole rows/columns */
    click(R[0].x - 15, R[1].y + 5);               /* row 2 header */
    n = gsel(&r0, &c0, &r1, &c1);
    LT_CHECK(n == 8 && r0 == 2 && r1 == 2 && c0 == 0 && c1 == 7);
    click(R[1].x + 5, R[0].y - 10);               /* column 3 header */
    n = gsel(&r0, &c0, &r1, &c1);
    LT_CHECK(n == 8 && c0 == 3 && c1 == 3 && r0 == 0 && r1 == 7);
    /* programmatic selection */
    lt_ui_grid_select(H.ui, 1, 1, 2, 2);
    LT_CHECK(gsel(&r0, &c0, &r1, &c1) == 4 && r0 == 1 && c1 == 2);
    h_close();
}

LT_TEST(test_grid_editing)
{
    int er, ec;
    reset_state();
    g_rows = GR; g_cols = GC; g_set_text[0] = '\0';
    h_open(500, 300, 0);
    H.scene = scene_grid;
    frames(3);
    click_rect(R[3]);                             /* cell (1,1) */
    s_gf = 0;
    type_str("7");
    LT_CHECK(s_gf & LT_UI_GRID_EDIT_BEGIN);
    LT_CHECK(lt_ui_grid_editing(H.ui, &er, &ec) && er == 1 && ec == 1);
    LT_CHECK(lt_ui_text_focused(H.ui));
    type_str(".5");
    s_gf = 0;
    key(LT_KEY_ENTER);
    LT_CHECK(s_gf & LT_UI_GRID_EDIT_COMMIT);
    LT_CHECK_STR(s_buf, "7.5");
    LT_CHECK(!lt_ui_grid_editing(H.ui, NULL, NULL));
    LT_CHECK(!lt_ui_text_focused(H.ui));
    /* Esc cancels */
    type_str("9");
    s_gf = 0;
    key(LT_KEY_ESCAPE);
    LT_CHECK(s_gf & LT_UI_GRID_EDIT_CANCEL);
    LT_CHECK(!lt_ui_grid_editing(H.ui, NULL, NULL));
    /* Enter / double click start an edit asking the app for the text */
    s_gf = 0;
    key(LT_KEY_ENTER);
    LT_CHECK(s_gf & LT_UI_GRID_EDIT_BEGIN);
    LT_CHECK_STR(lt_ui_grid_edit_text(H.ui), "12");
    s_buf[0] = '\0';
    key(LT_KEY_ENTER);
    LT_CHECK_STR(s_buf, "12");
    s_gf = 0;
    click_rect(R[1]);
    click_rect(R[1]);
    LT_CHECK(s_gf & LT_UI_GRID_ACTIVATED);
    LT_CHECK(lt_ui_grid_editing(H.ui, &er, &ec) && er == 2 && ec == 3);
    /* clicking elsewhere commits */
    s_buf[0] = '\0';
    type_str("4");
    click_rect(R[2]);
    LT_CHECK(!lt_ui_grid_editing(H.ui, NULL, NULL));
    LT_CHECK(s_buf[0] != '\0');
    /* Tab commits and moves right */
    click_rect(R[3]);
    type_str("5");
    s_buf[0] = '\0';
    key(LT_KEY_TAB);
    LT_CHECK_STR(s_buf, "5");
    {
        int cr, cc;
        lt_ui_grid_cursor(H.ui, &cr, &cc);
        LT_CHECK(cr == 1 && cc == 2);
    }
    /* non-numeric keys do not start an edit, so app shortcuts keep working */
    s_gf = 0;
    type_str("q");
    LT_CHECK(!(s_gf & LT_UI_GRID_EDIT_BEGIN));
    h_close();
}

LT_TEST(test_grid_big)
{
    int x;
    reset_state();
    g_rows = 200; g_cols = 40; g_set_text[0] = '\0';
    h_open(500, 300, 0);
    H.scene = scene_grid;
    frames(4);
    LT_CHECK(s_n[6] <= 9 && s_n[7] <= 11);        /* only visible cells are produced */
    LT_CHECK(H.ui->layers[0].cmds.len < 600);
    click_rect(R[3]);
    key_ex(LT_KEY_END, LT_MOD_CTRL);
    frames(3);
    {
        int cr, cc;
        lt_ui_grid_cursor(H.ui, &cr, &cc);
        LT_CHECK(cr == 199 && cc == 39);
    }
    /* the view followed the cursor: last cell is on screen */
    x = 0;
    frames(3);
    LT_CHECK(s_n[6] > 0 && x == 0);
    {
        lt_rect cr = lt_ui_grid_cell_rect(H.ui, 199, 39);
        LT_CHECK(cr.y >= 0 && cr.y < 300 && cr.x < 500);
    }
    g_rows = GR; g_cols = GC;
    h_close();
}

/* ---- focus / keyboard -------------------------------------------- */
static void scene_focus(lt_ui *ui)
{
    int w = 100;
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_button(ui, "A")) { s_n[0]++; }
    if (lt_ui_button(ui, "B")) { s_n[1]++; }
    if (lt_ui_button(ui, "C")) { s_n[2]++; }
}

LT_TEST(test_focus_cycle)
{
    lt_u32 a, b, c;
    reset_state();
    h_open(300, 200, 0);
    H.scene = scene_focus;
    frames(3);
    a = lt_ui_id(H.ui, "A"); b = lt_ui_id(H.ui, "B"); c = lt_ui_id(H.ui, "C");
    LT_CHECK(lt_ui_focus(H.ui) == 0);
    key(LT_KEY_TAB);
    LT_CHECK(lt_ui_focus(H.ui) == a);
    key(LT_KEY_TAB);
    LT_CHECK(lt_ui_focus(H.ui) == b);
    key(LT_KEY_TAB);
    LT_CHECK(lt_ui_focus(H.ui) == c);
    key(LT_KEY_TAB);
    LT_CHECK(lt_ui_focus(H.ui) == a);             /* wraps */
    key_ex(LT_KEY_TAB, LT_MOD_SHIFT);
    LT_CHECK(lt_ui_focus(H.ui) == c);             /* shift-tab wraps backwards */
    key_ex(LT_KEY_TAB, LT_MOD_SHIFT);
    LT_CHECK(lt_ui_focus(H.ui) == b);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[1], 1);
    LT_CHECK(s_n[0] == 0 && s_n[2] == 0);
    /* clicking empty space drops focus */
    click(250, 150);
    LT_CHECK(lt_ui_focus(H.ui) == 0);
    h_close();
}

static void scene_keys(lt_ui *ui)
{
    int w = 100;
    if (lt_ui_key_pressed(ui, 'q', 0)) { s_n[0]++; }
    if (lt_ui_key_pressed(ui, 's', LT_MOD_CTRL)) { s_n[1]++; }
    if (lt_ui_key_pressed(ui, LT_KEY_F5, 0)) { s_n[2]++; }
    lt_ui_layout_row(ui, 1, &w, 0);
    lt_ui_text(ui, "t", s_buf, sizeof(s_buf), 0);
    R[0] = lt_ui_last_rect(ui);
}

LT_TEST(test_key_shortcuts)
{
    reset_state();
    h_open(300, 200, 0);
    H.scene = scene_keys;
    frames(3);
    key(LT_KEY_ESCAPE);
    key_ex('q', 0);
    LT_CHECK_INT(s_n[0], 1);                      /* consumed once although two frames ran */
    key_ex('s', LT_MOD_CTRL);
    LT_CHECK_INT(s_n[1], 1);
    key_ex('s', LT_MOD_CTRL | LT_MOD_SHIFT);      /* modifiers must match exactly */
    LT_CHECK_INT(s_n[1], 1);
    key_ex('q', LT_MOD_CTRL);
    LT_CHECK_INT(s_n[0], 1);
    /* suppressed while a text field is being edited */
    click_rect(R[0]);
    LT_CHECK(lt_ui_text_focused(H.ui));
    key_ex('q', 0);
    LT_CHECK_INT(s_n[0], 1);
    key_ex('a', LT_MOD_CTRL);
    key_ex(LT_KEY_F5, 0);                         /* F-keys still work */
    LT_CHECK_INT(s_n[2], 1);
    key_ex('s', LT_MOD_CTRL);                     /* ctrl+s is not used by the field */
    LT_CHECK_INT(s_n[1], 2);
    key(LT_KEY_ESCAPE);
    key_ex('q', 0);
    LT_CHECK_INT(s_n[0], 2);
    h_close();
}

/* ---- tooltip / redraw -------------------------------------------- */
static void scene_tip(lt_ui *ui)
{
    lt_ui_button(ui, "Hover me");
    R[0] = lt_ui_last_rect(ui);
    lt_ui_tooltip(ui, "Tip text");
}

LT_TEST(test_tooltip_and_redraw)
{
    int mx, my, i;
    lt_u32 before, after;
    reset_state();
    h_open(300, 200, 0);
    H.scene = scene_tip;
    frames(4);
    mx = R[0].x + 10; my = R[0].y + 10;
    mouse_move(mx, my);
    frames(2);
    before = px(mx + 16, my + 28);
    for (i = 0; i < 10; i++) { frame(); }         /* 200 ms: still hidden */
    LT_CHECK(px(mx + 16, my + 28) == before);
    LT_CHECK(lt_ui_wait_hint_ms(H.ui) >= 0);      /* a wake-up is scheduled for the delay */
    for (i = 0; i < 30; i++) { frame(); }         /* > 500 ms */
    after = px(mx + 16, my + 28);
    LT_CHECK(after != before);
    LT_CHECK(after == (lt_ui_theme(H.ui)->elevated & 0xFFFFFFUL));
    mouse_move(250, 150);
    frames(2);
    LT_CHECK(px(mx + 16, my + 28) == before);
    /* idle detection */
    frames(4);
    LT_CHECK(!lt_ui_needs_redraw(H.ui));
    LT_CHECK(lt_ui_wait_hint_ms(H.ui) < 0);
    mouse_move(10, 10);
    LT_CHECK(lt_ui_needs_redraw(H.ui));
    frames(4);
    LT_CHECK(!lt_ui_needs_redraw(H.ui));
    h_close();
}

static void scene_anim(lt_ui *ui)
{
    lt_ui_spinner(ui);
    lt_ui_progress(ui, -1.0, NULL);
}

LT_TEST(test_animation_hint)
{
    reset_state();
    h_open(300, 100, 0);
    H.scene = scene_anim;
    frames(4);
    LT_CHECK(lt_ui_wait_hint_ms(H.ui) >= 0 && lt_ui_wait_hint_ms(H.ui) <= 50);
    h_close();
}

/* ---- toast, group, region, swatch, colour editor ------------------ */
static lt_color s_col;
static void scene_misc(lt_ui *ui)
{
    int w = 200, i;
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_group_begin(ui, "Group", 0)) {
        for (i = 0; i < 3; i++) { lt_ui_layout_row(ui, 1, &w, 0); lt_ui_label(ui, "line"); }
        lt_ui_group_end(ui);
    }
    R[0] = lt_ui_last_rect(ui);
    lt_ui_layout_row(ui, 1, &w, 0);
    if (lt_ui_color_edit(ui, "col", &s_col)) { s_n[1]++; }
    R[1] = lt_ui_last_rect(ui);
    lt_ui_layout_row(ui, 1, &w, 0);
    lt_ui_link(ui, "a link");
}

LT_TEST(test_misc_widgets)
{
    int i;
    reset_state();
    s_col = LT_RGB(10, 20, 30);
    h_open(500, 400, 0);
    H.scene = scene_misc;
    frames(5);
    /* the auto-height group grew to fit three rows */
    {
        lt_rect gr;
        ui_state *st = ui_state_get(H.ui, lt_ui_id(H.ui, "Group") ^ 0x6770UL);
        gr = R[0];
        LT_CHECK(st->i[0] >= 3 * 24 + 2 * 4);
        LT_CHECK(gr.w > 0);
    }
    /* colour editor: click opens a popup with three sliders */
    click_rect(R[1]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    frames(3);
    /* drag the first slider knob to the far right: red becomes 255 */
    {
        int y = R[1].y + R[1].h + 2 + 3 + 12, x = R[1].x + 18 + 14;
        mouse_move(x, y); frame();
        mouse_down(x, y, LT_MB_LEFT, 0); frame();
        mouse_move(x + 400, y); frame();
        mouse_up(x + 400, y, LT_MB_LEFT, 0); frame(); frame();
        LT_CHECK(LT_COLOR_R(s_col) == 255);
        LT_CHECK(s_n[1] >= 1);
    }
    key(LT_KEY_ESCAPE);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    /* toasts expire after four seconds */
    lt_ui_toast(H.ui, 1, "hello");
    frames(3);
    LT_CHECK(H.ui->ntoasts == 1);
    for (i = 0; i < 4; i++) { H.t += 1500.0; frame(); }
    LT_CHECK(H.ui->ntoasts == 0);
    h_close();
}

/* ---- cell mode ---------------------------------------------------- */
static int s_chk2;
static void scene_cells(lt_ui *ui)
{
    int w[2];
    w[0] = 80; w[1] = 80;
    lt_ui_layout_row(ui, 2, w, 0);
    if (lt_ui_button(ui, "Save##hidden")) { s_n[0]++; }
    R[0] = lt_ui_last_rect(ui);
    lt_ui_button(ui, "Quit");
    lt_ui_layout_row(ui, 1, NULL, 0);
    lt_ui_checkbox(ui, "Check me", &s_chk2);
    R[1] = lt_ui_last_rect(ui);
    s_res |= lt_ui_text(ui, "txt", s_buf, sizeof(s_buf), 0);
    R[2] = lt_ui_last_rect(ui);
}

LT_TEST(test_cell_mode)
{
    int c, r, ok = 1;
    reset_state();
    s_chk2 = 0;
    h_open(80, 24, 1);
    H.scene = scene_cells;
    frames(4);
    LT_CHECK(lt_ui_cell_mode(H.ui) && lt_ui_row_height(H.ui) == 16);
    LT_CHECK(lt_ui_theme(H.ui)->spacing_y == 0 && lt_ui_theme(H.ui)->padding % 8 == 0);
    LT_CHECK(cells_has_text("Save") && !cells_has_text("##") && !cells_has_text("hidden"));
    LT_CHECK(cells_has_text("Quit") && cells_has_text("[ ] Check me"));
    /* all widget rects are cell aligned */
    for (r = 0; r < 3; r++) { if (R[r].x % 8 != 0 || R[r].y % 16 != 0 || R[r].h % 16 != 0) { ok = 0; } }
    LT_CHECK(ok);
    /* button background was painted in cell backgrounds */
    c = R[0].x / 8 + 1; r = R[0].y / 16;
    LT_CHECK(H.cells.cells[r * H.cells.cols + c].bg != lt_ui_theme(H.ui)->bg_app);
    /* clicking works with cell coordinates (pixel units of the 8x16 grid) */
    click(R[0].x + 4, R[0].y + 8);
    LT_CHECK_INT(s_n[0], 1);
    click(R[1].x + 4, R[1].y + 8);
    LT_CHECK_INT(s_chk2, 1);
    LT_CHECK(cells_has_text("] Check me"));
    LT_CHECK(H.cells.cells[(R[1].y / 16) * H.cells.cols + R[1].x / 8 + 1].ch == 0x2713);   /* check mark */
    /* text input in cell mode */
    click(R[2].x + 20, R[2].y + 8);
    type_str("abc");
    LT_CHECK_STR(s_buf, "abc");
    LT_CHECK(cells_has_text("abc"));
    h_close();
}

/* ---- file dialog -------------------------------------------------- */
static lt_ui_filedlg s_fd;
static void scene_filedlg(lt_ui *ui)
{
    int r = lt_ui_file_dialog(ui, &s_fd, "Open file");
    if (r != 0) { s_res = r; }
}

static int fd_has(const char *name)
{
    int i;
    for (i = 0; i < s_fd.n_names; i++) {
        if (strcmp(s_fd.names[i] + 1, name) == 0) { return 1; }
    }
    return 0;
}

LT_TEST(test_file_dialog)
{
    const char *dir = "build/ui_fd_test";
    char path[256];
    reset_state();
    lt_plat_mkdirs("build/ui_fd_test/sub");
    lt_file_write("build/ui_fd_test/a.msq", "a", 1);
    lt_file_write("build/ui_fd_test/c.msq", "c", 1);
    lt_file_write("build/ui_fd_test/b.txt", "b", 1);
    lt_file_write("build/ui_fd_test/.hidden", "h", 1);
    h_open(1280, 800, 0);
    H.scene = scene_filedlg;
    lt_ui_filedlg_init(&s_fd);
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_OPEN, dir, "", "Tune files|*.msq|All files|*");
    frames(5);
    LT_CHECK(s_fd.open && s_fd.n_names == 4);     /* "..", sub, a.msq, c.msq */
    LT_CHECK(fd_has("..") && fd_has("sub") && fd_has("a.msq") && fd_has("c.msq"));
    LT_CHECK(!fd_has("b.txt") && !fd_has(".hidden"));
    LT_CHECK(s_fd.is_dir[1] && !s_fd.is_dir[2]);   /* directories first */
    /* typing a file name and pressing Enter returns the full path */
    type_str("a.msq");
    LT_CHECK_STR(s_fd.name, "a.msq");
    s_res = 0;
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_res, 1);
    lt_snprintf(path, sizeof(path), "%s/a.msq", dir);
    LT_CHECK_STR(s_fd.path, path);
    LT_CHECK(!s_fd.open);
    /* open again, Esc cancels */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_OPEN, dir, "", "Tune files|*.msq|All files|*");
    s_res = 0; frames(5);
    key(LT_KEY_ESCAPE);
    LT_CHECK_INT(s_res, -1);
    /* a missing file is refused in open mode */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_OPEN, dir, "nothere.msq", "All files|*");
    s_res = 0; frames(5);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_res, 0);
    LT_CHECK(s_fd.open && s_fd.err[0] != '\0');
    key(LT_KEY_ESCAPE);
    /* double click on a directory descends into it; Up goes back */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_OPEN, dir, "", "All files|*");
    s_res = 0; frames(5);
    LT_CHECK(s_fd.n_names == 5 && fd_has("b.txt"));
    click(450, 252 + 24 + 12);
    click(450, 252 + 24 + 12);
    LT_CHECK(lt_str_ends(s_fd.dir, "sub"));
    LT_CHECK(s_fd.n_names == 1 && fd_has(".."));
    click(320 + 16 + 30, 170 + 42 + 12 + 12);     /* Up button */
    LT_CHECK(lt_str_ends(s_fd.dir, "ui_fd_test"));
    key(LT_KEY_ESCAPE);
    /* hidden files toggle */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_OPEN, dir, "", "All files|*");
    frames(5);
    s_fd.show_hidden = 1; s_fd.dirty = 1;
    frames(3);
    LT_CHECK(fd_has(".hidden"));
    key(LT_KEY_ESCAPE);
    /* save mode asks before overwriting */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_SAVE, dir, "a.msq", "All files|*");
    s_res = 0; frames(5);
    key(LT_KEY_ENTER);
    LT_CHECK(s_fd.confirm && s_res == 0);
    frames(4);
    key(LT_KEY_ENTER);                           /* default button of the confirmation: Replace */
    LT_CHECK_INT(s_res, 1);
    LT_CHECK_STR(s_fd.path, path);
    /* saving under a new name needs no confirmation */
    lt_ui_filedlg_open(&s_fd, LT_UI_FILE_SAVE, dir, "new.msq", "All files|*");
    s_res = 0; frames(5);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_res, 1);
    lt_plat_remove("build/ui_fd_test/a.msq");
    lt_plat_remove("build/ui_fd_test/b.txt");
    lt_plat_remove("build/ui_fd_test/c.msq");
    lt_plat_remove("build/ui_fd_test/.hidden");
    lt_plat_remove("build/ui_fd_test/sub");
    lt_plat_remove("build/ui_fd_test");
    lt_ui_filedlg_free(&s_fd);
    h_close();
}

/* ---- overlays in cell mode ---------------------------------------- */
LT_TEST(test_cell_mode_overlays)
{
    reset_state();
    h_open(100, 30, 1);
    H.scene = scene_menu;
    frames(3);
    click_rect(R[0]);
    LT_CHECK(lt_ui_popup_open(H.ui));
    LT_CHECK(cells_has_text("New") && cells_has_text("Ctrl+N"));
    LT_CHECK(R[1].x % 8 == 0 && R[1].y % 16 == 0 && R[1].h == 16);
    click_rect(R[1]);
    LT_CHECK_INT(s_n[0], 1);
    LT_CHECK(!lt_ui_popup_open(H.ui));
    LT_CHECK(!cells_has_text("Ctrl+N"));
    h_close();

    reset_state();
    h_open(100, 30, 1);
    H.scene = scene_modal;
    frames(3);
    s_open = 1;
    frames(4);
    LT_CHECK(cells_has_text("Dlg") && cells_has_text("OK") && cells_has_text("Cancel"));
    click_rect(R[0]);
    LT_CHECK_INT(s_n[0], 0);
    key(LT_KEY_ENTER);
    LT_CHECK_INT(s_n[1], 1);
    s_open = 1;
    frames(4);
    key(LT_KEY_ESCAPE);
    LT_CHECK(!s_open);
    h_close();
}

/* ---- scale, image, wrapped label ----------------------------------- */
static const lt_u32 img_px[4] = { 0xFFFF0000UL, 0xFF00FF00UL, 0xFF0000FFUL, 0xFFFFFF00UL };
static void scene_image(lt_ui *ui)
{
    int w[2];
    w[0] = lt_ui_cell_mode(ui) ? 96 : 60; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, lt_ui_cell_mode(ui) ? 16 : 2);
    lt_ui_image(ui, img_px, 2, 2, 2);
    R[0] = lt_ui_last_rect(ui);
    lt_ui_layout_row(ui, 1, NULL, 0);
    lt_ui_label_wrap(ui, "one two three four five six seven eight nine ten", 0xFFFFFFFFUL);
    R[1] = lt_ui_last_rect(ui);
}

LT_TEST(test_scale_image_wrap)
{
    reset_state();
    h_open(200, 200, 0);
    H.scene = scene_image;
    frames(3);
    LT_CHECK(px(R[0].x, R[0].y) == 0xFF0000UL && px(R[0].x + 1, R[0].y) == 0x00FF00UL);
    LT_CHECK(px(R[0].x, R[0].y + 1) == 0x0000FFUL && px(R[0].x + 1, R[0].y + 1) == 0xFFFF00UL);
    lt_ui_set_scale(H.ui, 2);
    frames(3);
    LT_CHECK(lt_ui_row_height(H.ui) == 48 && lt_ui_theme(H.ui)->pad_x == 16);
    h_close();
    h_open(40, 10, 1);
    H.scene = scene_image;
    frames(3);
    LT_CHECK(cells_has_text("[image 2x2]"));
    h_close();
}

/* ---- fractional UI scale ------------------------------------------ */
LT_TEST(test_fractional_scale)
{
    lt_ui *ui = lt_ui_create();
    /* rounding: (px * pct + 50) / 100, half away from zero */
    LT_CHECK(lt_ui_px_pct(10, 100) == 10);
    LT_CHECK(lt_ui_px_pct(10, 125) == 13);      /* 12.5 -> 13 */
    LT_CHECK(lt_ui_px_pct(10, 150) == 15);
    LT_CHECK(lt_ui_px_pct(3, 150) == 5);        /* 4.5 -> 5 */
    LT_CHECK(lt_ui_px_pct(1, 125) == 1);        /* 1.25 -> 1 */
    LT_CHECK(lt_ui_px_pct(1, 175) == 2);        /* 1.75 -> 2 */
    LT_CHECK(lt_ui_px_pct(0, 300) == 0);
    LT_CHECK(lt_ui_px_pct(-3, 150) == -5);      /* symmetric */
    LT_CHECK(lt_ui_px_pct(24, 125) == 30);
    LT_CHECK(lt_ui_px_pct(2000000000, 300) == 2147483647);   /* saturates, no overflow */
    LT_CHECK(lt_ui_px_pct(-2000000000, 300) == -2147483647);
    LT_CHECK(lt_ui_px_pct(7, 0) == 7);          /* bad percent = 100 */
    /* context API */
    LT_CHECK(lt_ui_scale_pct(ui) == 100 && lt_ui_px(ui, 24) == 24);
    lt_ui_set_scale_pct(ui, 150);
    LT_CHECK(lt_ui_scale_pct(ui) == 150 && lt_ui_px(ui, 24) == 36 && lt_ui_px(ui, 3) == 5);
    LT_CHECK(lt_ui_unpx(ui, 36) == 24);
    lt_ui_set_scale_pct(ui, 125);
    LT_CHECK(lt_ui_row_height(ui) == 30 && lt_ui_theme(ui)->padding == 10);
    lt_ui_set_scale_pct(ui, 50);
    LT_CHECK(lt_ui_scale_pct(ui) == 100);        /* clamped to 100..300 */
    lt_ui_set_scale_pct(ui, 900);
    LT_CHECK(lt_ui_scale_pct(ui) == 300);
    lt_ui_set_scale(ui, 2);                      /* integer wrapper */
    LT_CHECK(lt_ui_scale_pct(ui) == 200);
    lt_ui_destroy(ui);
    /* cell mode ignores the scale entirely */
    reset_state();
    h_open(40, 10, 1);
    lt_ui_set_scale_pct(H.ui, 200);
    H.scene = scene_image;
    frames(3);
    LT_CHECK(lt_ui_px(H.ui, 24) == 24 && lt_ui_row_height(H.ui) == 16);
    h_close();
    /* layout parameters are scaled by the toolkit: a 100 px wide cell is 150 px at 150% */
    reset_state();
    h_open(400, 200, 0);
    lt_ui_set_scale_pct(H.ui, 150);
    H.scene = scene_image;
    frames(3);
    LT_CHECK(R[0].w == 90 && R[0].h == 3);
    h_close();
}

/* ---- random input must never crash ----------------------------------- */
static unsigned long fz_state = 12345UL;
/* ---- review regressions ------------------------------------------- */
static char s_long[1024];
static void scene_long(lt_ui *ui)
{
    int w = 300;
    lt_ui_layout_row(ui, 1, &w, 0);
    s_res |= lt_ui_text(ui, "long", s_long, sizeof(s_long), 0);
    R[0] = lt_ui_last_rect(ui);
    lt_ui_button(ui, "Other");
    R[1] = lt_ui_last_rect(ui);
}

static void scene_grid_zero(lt_ui *ui)
{
    int w = 200;
    lt_ui_layout_row(ui, 1, &w, 100);
    LT_CHECK(!lt_ui_grid_begin(ui, "z", 3, 3, 0, 20, 10, 10));
    LT_CHECK(!lt_ui_grid_begin(ui, "z", 3, 3, 20, 0, 10, 10));
}

LT_TEST(test_review_fixes)
{
    int i;
    reset_state();
    /* long (> 256 and > 512 byte) text: focus, Esc and Ctrl+Z must not overflow or truncate */
    for (i = 0; i < 600; i++) { s_long[i] = (char)('a' + i % 26); }
    s_long[600] = '\0';
    h_open(400, 200, 0);
    H.scene = scene_long;
    frames(2);
    click_rect(R[0]);
    LT_CHECK(lt_ui_text_focused(H.ui));
    key_ex('z', LT_MOD_CTRL);
    LT_CHECK(strlen(s_long) == 600);
    key(LT_KEY_ESCAPE);
    LT_CHECK(strlen(s_long) == 600);
    /* utf-8: backspace and delete remove whole characters */
    strcpy(s_long, "a\xC3\xA9\xE2\x82\xAC");   /* a, e-acute, euro */
    click_rect(R[0]);
    key(LT_KEY_END);
    key(LT_KEY_BACKSPACE);
    LT_CHECK_STR(s_long, "a\xC3\xA9");
    key(LT_KEY_LEFT);
    key(LT_KEY_DELETE);
    LT_CHECK_STR(s_long, "a");
    h_close();
    /* degenerate grid cell sizes are rejected, not divided by */
    h_open(400, 200, 0);
    H.scene = scene_grid_zero;
    frames(2);
    h_close();
}

static int fz_rand(int n)
{
    fz_state = (fz_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return (int)((fz_state >> 8) % (unsigned long)n);
}

static void fuzz_run(int w, int h, int cell, int iters)
{
    int i, ok = 1;
    static const int keys[] = { LT_KEY_UP, LT_KEY_DOWN, LT_KEY_LEFT, LT_KEY_RIGHT, LT_KEY_ENTER,
                                LT_KEY_ESCAPE, LT_KEY_TAB, LT_KEY_BACKSPACE, LT_KEY_DELETE,
                                LT_KEY_HOME, LT_KEY_END, LT_KEY_PAGEUP, LT_KEY_PAGEDOWN, ' ', 'a', 'c',
                                'v', 'x', '5', LT_KEY_F2 };
    h_open(w, h, cell);
    gallery_init(&GAL);
    H.scene = scene_gallery;
    frames(3);
    for (i = 0; i < iters; i++) {
        lt_event e;
        memset(&e, 0, sizeof(e));
        e.x = fz_rand(w * (cell ? 8 : 1)); e.y = fz_rand(h * (cell ? 16 : 1));
        switch (fz_rand(7)) {
        case 0: e.type = LT_EV_MOUSE_MOVE; break;
        case 1: e.type = LT_EV_MOUSE_DOWN; e.button = 1 + fz_rand(3); break;
        case 2: e.type = LT_EV_MOUSE_UP; e.button = 1 + fz_rand(3); break;
        case 3: e.type = LT_EV_WHEEL; e.wheel = fz_rand(2) ? 1 : -1; break;
        case 4: e.type = LT_EV_KEY_DOWN; e.key = keys[fz_rand((int)LT_COUNTOF(keys))]; e.mods = fz_rand(8); break;
        case 5: e.type = LT_EV_TEXT; e.text[0] = (char)('0' + fz_rand(60)); break;
        default: e.type = LT_EV_RESIZE; break;
        }
        lt_ui_input(H.ui, &e);
        if (fz_rand(3) == 0) { frame(); }
        if (fz_rand(40) == 0) { H.t += 700.0; }
        if (GAL.fd.open && fz_rand(30) == 0) { GAL.fd.open = 0; }
    }
    frames(3);
    LT_CHECK(ok);
    gallery_free(&GAL);
    h_close();
}

LT_TEST(test_fuzz_gallery)
{
    int seed;
    for (seed = 0; seed < 6; seed++) {
        fz_state = 777UL + (unsigned long)seed * 7919UL;
        fuzz_run(800, 600, 0, 2500);
        fuzz_run(100, 40, 1, 2500);
    }
    fuzz_run(1280, 800, 0, 4000);
    fuzz_run(160, 50, 1, 4000);
    fuzz_run(300, 200, 0, 1500);      /* tiny surface */
    fuzz_run(40, 10, 1, 1500);
}

/* ---- backend selection ------------------------------------------- */
LT_TEST(test_backend_select)
{
    lt_backend *b;
    char *env = lt_plat_getenv("LT_BACKEND");
    lt_backend_destroy(NULL);
    if (env == NULL) {
        b = lt_backend_select("bogus");
        LT_CHECK(b == NULL);
        b = lt_backend_select("tty");              /* NULL unless stdout is a terminal */
        if (b != NULL) { LT_CHECK(strcmp(b->name, "tty") == 0 && b->cell_mode); lt_backend_destroy(b); }
    }
    lt_free(env);
    b = lt_backend_web_create(0);
    if (b != NULL) {
        LT_CHECK(strcmp(b->name, "web") == 0);
        lt_backend_destroy(b);                     /* never opened: must free cleanly */
    }
}

/* highlighted menu rows and selected tree rows stay readable in every theme (4.0 allows for
 * anti-aliasing; light theme had white primary text on a pale blue selection, 1.4:1) */
static double t_lin(int v) { double c = v / 255.0; return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double t_lum(lt_u32 c)
{
    return 0.2126 * t_lin((int)((c >> 16) & 255)) + 0.7152 * t_lin((int)((c >> 8) & 255)) + 0.0722 * t_lin((int)(c & 255));
}
static double best_contrast(lt_rect r, lt_u32 bg)
{
    int x, y;
    double best = 1.0, lb = t_lum(bg) + 0.05;
    for (y = r.y; y < r.y + r.h; y++) {
        for (x = r.x; x < r.x + r.w; x++) {
            double l = t_lum(px(x, y)) + 0.05, c = l > lb ? l / lb : lb / l;
            if (c > best) { best = c; }
        }
    }
    return best;
}

static void scene_sel_rows(lt_ui *ui)
{
    static const char *const items[] = { "Alpha", "Beta" };
    int w = -1;
    scene_menu(ui);
    lt_ui_layout_set_rect(ui, lt_rect_make(250, 250, 200, 24));
    lt_ui_tree_node(ui, "Selected node", 0, LT_UI_TREE_LEAF | LT_UI_TREE_SELECTED);
    R[8] = lt_ui_last_rect(ui);
    lt_ui_layout_set_rect(ui, lt_rect_make(250, 300, 200, 80));
    lt_ui_layout_row(ui, 1, &w, 0);
    lt_ui_listbox(ui, "lb", &s_sel, items, 2);
    R[9] = lt_ui_last_rect(ui);
}

LT_TEST(test_selection_text_readable_in_all_themes)
{
    static const char *names[] = { "dark", "light", "midnight", "carbon", "synthwave", "solarized",
                                   "nord", "dracula", "highcontrast" };
    size_t i;
    for (i = 0; i < LT_COUNTOF(names); i++) {
        lt_theme t;
        lt_rect hr;
        LT_CHECK(lt_theme_preset(&t, names[i]) == 0);
        reset_state();
        h_open(500, 400, 0);
        H.scene = scene_sel_rows;
        lt_ui_set_theme(H.ui, &t);
        frames(2);
        click_rect(R[9]);                                  /* focus the list: row 0 selected */
        key_ex('f', LT_MOD_ALT);                           /* File open, New highlighted */
        frames(2);
        hr = lt_rect_inset(R[1], 4, 3);
        if (best_contrast(hr, px(hr.x, hr.y + hr.h / 2)) < 4.0) { printf("menu row in %s\n", names[i]); LT_CHECK(0); }
        hr = lt_rect_inset(R[8], 4, 3);
        if (best_contrast(hr, t.selected & 0xFFFFFFUL) < 4.0) { printf("tree row in %s %.2f\n", names[i], best_contrast(hr, t.selected & 0xFFFFFFUL)); LT_CHECK(0); }
        h_close();
    }
}

LT_TEST(test_widgets_in_all_themes)
{
    static const char *names[] = { "dark", "light", "midnight", "carbon", "synthwave", "solarized",
                                   "nord", "dracula", "highcontrast" };
    size_t i;
    for (i = 0; i < LT_COUNTOF(names); i++) {
        lt_theme t;
        LT_CHECK(lt_theme_preset(&t, names[i]) == 0);
        LT_CHECK(t.text != t.bg_app && t.primary != t.bg_app);
        h_open(1280, 800, 0);
        gallery_init(&GAL);
        H.scene = scene_gallery;
        lt_ui_set_theme(H.ui, &t);
        frames(3);
        LT_CHECK(px(5, 5) == (t.menubar & 0xFFFFFFUL));
        gallery_free(&GAL);
        h_close();
    }
}

static int g_custom_calls;
static void count_cb(lt_gfx *g, lt_rect r, void *user)
{
    LT_UNUSED(g); LT_UNUSED(r); LT_UNUSED(user);
    g_custom_calls++;
}

/* a discarded frame does not replay its draw callbacks (their state may be freed by then), leaves
 * the surface alone and asks for the next frame; outside a frame the call does nothing */
LT_TEST(discard_frame)
{
    h_open(200, 100, 0);
    lt_ui_discard_frame(H.ui);
    g_custom_calls = 0;
    lt_ui_begin(H.ui, &H.g, H.t);
    lt_ui_draw_custom(H.ui, lt_rect_make(0, 0, 50, 50), count_cb, NULL);
    lt_ui_end(H.ui);
    LT_CHECK_INT(g_custom_calls, 1);
    H.cv.px[0] = 0x12345678UL;
    lt_ui_begin(H.ui, &H.g, H.t + 20.0);
    lt_ui_draw_custom(H.ui, lt_rect_make(0, 0, 50, 50), count_cb, NULL);
    lt_ui_discard_frame(H.ui);
    lt_ui_end(H.ui);
    LT_CHECK_INT(g_custom_calls, 1);
    LT_CHECK(H.cv.px[0] == 0x12345678UL);
    LT_CHECK(lt_ui_needs_redraw(H.ui));
    lt_ui_begin(H.ui, &H.g, H.t + 40.0);
    lt_ui_draw_custom(H.ui, lt_rect_make(0, 0, 50, 50), count_cb, NULL);
    lt_ui_end(H.ui);
    LT_CHECK_INT(g_custom_calls, 2);
    h_close();
}

int main(void)
{
    lt_plat_mkdirs("build");
    LT_RUN(test_ids_and_theme);
    LT_RUN(test_button_click);
    LT_RUN(test_check_radio_toggle);
    LT_RUN(test_text_editing);
    LT_RUN(test_number_field);
    LT_RUN(test_number_field_keeps_out_of_range_value);
    LT_RUN(test_slider);
    LT_RUN(test_combo);
    LT_RUN(test_listbox);
    LT_RUN(test_tree_view);
    LT_RUN(test_tab_bar);
    LT_RUN(test_shell_widgets);
    LT_RUN(test_menus);
    LT_RUN(test_menu_taller_than_window_scrolls);
    LT_RUN(test_context_menu);
    LT_RUN(test_modal);
    LT_RUN(test_modal_taller_than_window_scrolls);
    LT_RUN(test_msgbox);
    LT_RUN(test_windows);
    LT_RUN(test_scroll_area);
    LT_RUN(test_tab_scrolls_focus_into_view);
    LT_RUN(test_splitter);
    LT_RUN(test_layout);
    LT_RUN(test_grid_selection);
    LT_RUN(test_grid_editing);
    LT_RUN(test_grid_big);
    LT_RUN(test_focus_cycle);
    LT_RUN(test_key_shortcuts);
    LT_RUN(test_tooltip_and_redraw);
    LT_RUN(test_animation_hint);
    LT_RUN(test_misc_widgets);
    LT_RUN(test_cell_mode);
    LT_RUN(test_file_dialog);
    LT_RUN(test_cell_mode_overlays);
    LT_RUN(test_scale_image_wrap);
    LT_RUN(test_fractional_scale);
    LT_RUN(test_review_fixes);
    LT_RUN(test_fuzz_gallery);
    LT_RUN(test_backend_select);
    LT_RUN(test_widgets_in_all_themes);
    LT_RUN(test_selection_text_readable_in_all_themes);
    LT_RUN(test_gallery_render);
    LT_RUN(discard_frame);
    gallery_free(&GAL);
    LT_TEST_MAIN_END
}
