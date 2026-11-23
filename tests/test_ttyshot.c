/* test_ttyshot.c - snapshot tests of the terminal UI (cell mode): the app is
 * rendered headlessly with the LT_SHOT_* harness (src/app/app_ttyshot.c) at
 * 80x24 and 120x40, in truecolor / 256 / 16 colors and ASCII, and the text
 * and escape-stream dumps are checked. PPM images go to build/.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ttyshot.h"
#include "../src/app/gauges_cell.h"

#define MAXTXT 200000UL

static char *g_txt;
static char *g_ans;

static void clear_env(void)
{
    lt_plat_setenv("LT_SHOT_TTY", "");
    lt_plat_setenv("LT_SHOT_TEXT", "");
    lt_plat_setenv("LT_SHOT_ANSI", "");
    lt_plat_setenv("LT_SHOT_KEYS", "");
    lt_plat_setenv("LT_SHOT_COLORS", "");
    lt_plat_setenv("LT_SHOT_ASCII", "");
    lt_plat_setenv("LT_SHOT_BASIC", "");
    lt_plat_setenv("LT_SHOT_OPEN", "");
}

static void slurp(const char *path, char *dst, size_t cap)
{
    size_t n = 0;
    char *d;
    if (dst == NULL) { return; }
    d = lt_file_read(path, &n);
    dst[0] = '\0';
    if (d == NULL) { return; }
    if (n >= cap) { n = cap - 1; }
    memcpy(dst, d, n);
    dst[n] = '\0';
    lt_free(d);
}

/* render the demo app; results land in g_txt and g_ans */
static void render(int cols, int rows, const char *open, const char *keys, const char *colors,
                   int ascii, const char *ppm)
{
    char ini[512], size[32];
    lt_app *app;
    lt_plat_clock_virtual(1);       /* timing-independent demo values (slow CI machines) */
    app = lt_app_new();
    if (g_txt == NULL) {
        g_txt = (char *)lt_malloc(MAXTXT);
        g_ans = (char *)lt_malloc(MAXTXT);
    }
    clear_env();
    lt_snprintf(size, sizeof(size), "%dx%d", cols, rows);
    lt_plat_setenv("LT_SHOT_SIZE", size);
    lt_plat_setenv("LT_SHOT_TEXT", "build/ttyshot.txt");
    lt_plat_setenv("LT_SHOT_ANSI", "build/ttyshot.ans");
    if (ppm != NULL) { lt_plat_setenv("LT_SHOT_TTY", ppm); }
    if (keys != NULL) { lt_plat_setenv("LT_SHOT_KEYS", keys); }
    if (colors != NULL) { lt_plat_setenv("LT_SHOT_COLORS", colors); }
    if (ascii) { lt_plat_setenv("LT_SHOT_ASCII", "1"); }
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    LT_CHECK(lt_app_start_demo(app, ini) == 0);
    if (open != NULL) { lt_app_open_target(app, open, open); }
    LT_CHECK(lt_app_screenshot(app, "", cols, rows, 8) == 0);
    lt_app_free(app);
    lt_plat_clock_virtual(0);
    clear_env();
    slurp("build/ttyshot.txt", g_txt, MAXTXT);
    slurp("build/ttyshot.ans", g_ans, MAXTXT);
}

/* n-th line of g_txt (0-based) copied to out */
static void line_of(int n, char *out, size_t cap)
{
    const char *p = g_txt;
    size_t len;
    out[0] = '\0';
    while (n > 0 && *p != '\0') {
        const char *e = strchr(p, '\n');
        if (e == NULL) { return; }
        p = e + 1;
        n--;
    }
    len = strcspn(p, "\n");
    if (len >= cap) { len = cap - 1; }
    memcpy(out, p, len);
    out[len] = '\0';
}

static int count_lines(void)
{
    int n = 0;
    const char *p;
    for (p = g_txt; *p != '\0'; p++) { if (*p == '\n') { n++; } }
    return n;
}

static int utf8_cols(const char *s)
{
    int n = 0;
    while (*s != '\0') {
        if (((unsigned char)*s & 0xC0) != 0x80) { n++; }
        s++;
    }
    return n;
}

static int has_high(const char *s)
{
    for (; *s != '\0'; s++) { if ((unsigned char)*s >= 0x80) { return 1; } }
    return 0;
}

LT_TEST(main_view_80x24)
{
    char l[1024];
    int i;
    render(80, 24, NULL, NULL, NULL, 0, "build/ttyshot_main80.ppm");
    LT_CHECK(count_lines() == 24);
    for (i = 0; i < 24; i++) {
        line_of(i, l, sizeof(l));
        LT_CHECK(utf8_cols(l) <= 80);
    }
    line_of(0, l, sizeof(l));
    LT_CHECK(strncmp(l, " File  Edit  View  Tools", 24) == 0);
    LT_CHECK(strstr(g_txt, "Setup") != NULL);
    LT_CHECK(strstr(g_txt, "Dashboard") != NULL);
    line_of(23, l, sizeof(l));                 /* status bar */
    LT_CHECK(strstr(l, "Demo") != NULL);
    LT_CHECK(strstr(l, "Burn") != NULL);
    LT_CHECK(strstr(g_txt, "RPM") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x95\xAD") != NULL);   /* rounded gauge frames */
}

LT_TEST(main_view_120x40_256)
{
    char l[1024];
    render(120, 40, NULL, NULL, "256", 0, "build/ttyshot_main120.ppm");
    LT_CHECK(count_lines() == 40);
    line_of(0, l, sizeof(l));
    LT_CHECK(strncmp(l, " File  Edit  View  Tools", 24) == 0);
    LT_CHECK(strstr(g_ans, "38;5;") != NULL);
    LT_CHECK(strstr(g_ans, "38;2;") == NULL);
    LT_CHECK(strstr(g_txt, "Coolant temp") != NULL);
    LT_CHECK(strstr(g_txt, "Air/Fuel Ratio") != NULL);
}

LT_TEST(menu_open)
{
    char l[1024];
    render(80, 34, NULL, "alt+f", NULL, 0, "build/ttyshot_menu80.ppm");   /* tall enough for the whole File menu */
    line_of(1, l, sizeof(l));
    LT_CHECK(strncmp(l, "\xE2\x95\xAD\xE2\x94\x80", 6) == 0);        /* top-left corner then a rule */
    line_of(2, l, sizeof(l));
    LT_CHECK(strncmp(l, "\xE2\x94\x82  New Project...", 19) == 0);
    LT_CHECK(strstr(l, "Ctrl+N") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x94\x9C\xE2\x94\x80") != NULL);   /* separator joins the frame */
    LT_CHECK(strstr(g_txt, "Exit") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x95\xB0") != NULL);               /* bottom-left corner */
}

/* the Tools menu shows the logging shortcut the user bound, not a fixed Ctrl+L */
LT_TEST(menu_shows_rebound_log_shortcut)
{
    char *old = lt_plat_getenv("LIBRETUNE_CONFIG_DIR");
    const char *hk = "tools.log=Ctrl+Alt+G\n";
    (void)lt_plat_mkdirs("build/test_ttyshot_hk");
    LT_CHECK(lt_file_write("build/test_ttyshot_hk/hotkeys.conf", hk, strlen(hk)) == 0);
    lt_plat_setenv("LIBRETUNE_CONFIG_DIR", "build/test_ttyshot_hk");
    render(80, 30, NULL, "alt+t", NULL, 0, NULL);
    lt_plat_setenv("LIBRETUNE_CONFIG_DIR", old != NULL ? old : "");
    lt_free(old);
    LT_CHECK(strstr(g_txt, "Data Logging") != NULL);
    LT_CHECK(strstr(g_txt, "Ctrl+Alt+G") != NULL);
    LT_CHECK(strstr(g_txt, "Ctrl+L ") == NULL);
}

/* the ECU definition's own Help menu joins LibreTune's Help instead of a second "Help" title */
LT_TEST(one_help_menu)
{
    char l[1024];
    const char *a, *b;
    render(200, 40, NULL, "alt+h", NULL, 0, NULL);
    line_of(0, l, sizeof(l));
    a = strstr(l, "Help");
    LT_CHECK(a != NULL);
    b = a != NULL ? strstr(a + 4, "Help") : NULL;
    LT_CHECK(b == NULL);
    LT_CHECK(strstr(g_txt, "About LibreTune") != NULL);
    LT_CHECK(strstr(g_txt, "rusEFI Help") != NULL);
}

/* keyboard only: Ctrl+K, type, Enter opens the first match of the filtered sidebar */
LT_TEST(sidebar_search_enter_opens_first_match)
{
    char l[1024];
    render(100, 30, NULL, "ctrl+k,text:inject,enter", NULL, 0, NULL);
    line_of(1, l, sizeof(l));
    LT_CHECK(strstr(l, "Injection configuration") != NULL);
    render(100, 30, NULL, "ctrl+k,text:zzzq,enter", NULL, 0, NULL);
    LT_CHECK(strstr(g_txt, "Nothing in the sidebar matches") != NULL);
    render(100, 30, NULL, "ctrl+k,text:zzzq", NULL, 0, NULL);
    LT_CHECK(strstr(g_txt, "No menu entry matches") != NULL);     /* an empty tree says why */
}

LT_TEST(ve_table_and_selection)
{
    char l[1024];
    render(120, 40, "veTableTbl", "down,right,shift+right", NULL, 0, "build/ttyshot_ve120.ppm");
    LT_CHECK(strstr(g_txt, "VE Table") != NULL);
    LT_CHECK(strstr(g_txt, "Y \\ X") != NULL);
    LT_CHECK(strstr(g_txt, "56.31") != NULL);
    line_of(39, l, sizeof(l));
    LT_CHECK(strstr(l, "Burn") != NULL);
    /* the cursor moved: the status line under the grid names the cell */
    LT_CHECK(strstr(g_txt, "selected") != NULL);
    LT_CHECK(strstr(g_txt, "2 selected") != NULL);
}

LT_TEST(ve_table_16_colors_ascii)
{
    char l[1024];
    int i;
    render(80, 24, "veTableTbl", "alt+f", "16", 1, "build/ttyshot_ve16.ppm");
    LT_CHECK(!has_high(g_txt));
    LT_CHECK(g_ans[0] != '\0');
    LT_CHECK(strstr(g_ans, "38;5;") == NULL && strstr(g_ans, "38;2;") == NULL);
    for (i = 0; i < 24; i++) {
        line_of(i, l, sizeof(l));
        LT_CHECK(strlen(l) <= 80);
    }
    LT_CHECK(strstr(g_txt, "New Project...") != NULL);
    LT_CHECK(strstr(g_txt, "+-") != NULL || strstr(g_txt, "|") != NULL);
}

LT_TEST(dialog_frame)
{
    render(120, 40, NULL, "ctrl+comma", NULL, 0, "build/ttyshot_dlg120.ppm");
    LT_CHECK(strstr(g_txt, "Settings") != NULL);
    LT_CHECK(strstr(g_txt, "Close") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x95\xAD") != NULL && strstr(g_txt, "\xE2\x95\xAF") != NULL);
}

LT_TEST(dashboard_bars)
{
    render(120, 40, NULL, NULL, NULL, 0, "build/ttyshot_dash120.ppm");
    /* eighth-block bars and the gauge labels */
    LT_CHECK(strstr(g_txt, "\xE2\x96\x88") != NULL);
    LT_CHECK(strstr(g_txt, "Throttle position") != NULL);
}

static int has_braille(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s == 0xE2 && ((unsigned char)s[1] & 0xFC) == 0xA0) { return 1; }
    }
    return 0;
}

LT_TEST(dashboard_dense_panels)
{
    char l[1024];
    int i, w, sizes[3][2];
    sizes[0][0] = 80; sizes[0][1] = 24;
    sizes[1][0] = 120; sizes[1][1] = 40;
    sizes[2][0] = 160; sizes[2][1] = 50;
    for (i = 0; i < 3; i++) {
        int row;
        render(sizes[i][0], sizes[i][1], NULL, NULL, NULL, 0, NULL);
        LT_CHECK(count_lines() == sizes[i][1]);
        for (row = 0; row < sizes[i][1]; row++) {
            line_of(row, l, sizeof(l));
            LT_CHECK(utf8_cols(l) <= sizes[i][0]);
        }
        LT_CHECK(strstr(g_txt, "\xE2\x97\x8B") != NULL);       /* indicator pills */
        LT_CHECK(strstr(g_txt, " more") != NULL);               /* what did not fit is counted */
        LT_CHECK(strstr(g_txt, "Config\xE2\x80\xA6Settin") == NULL);   /* no glued truncations */
    }
    render(120, 40, NULL, NULL, NULL, 0, "build/ttyshot_dash120.ppm");
    LT_CHECK(strstr(g_txt, "hi ") != NULL && strstr(g_txt, "avg ") != NULL && strstr(g_txt, "lo ") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x96\x80") != NULL || strstr(g_txt, "\xE2\x96\x88") != NULL);   /* block digits */
    LT_CHECK(has_braille(g_txt));                                /* trend plots */
    /* the panels fill the area: no empty band above the indicators at 160x50 */
    render(160, 50, NULL, NULL, NULL, 0, "build/ttyshot_dash160.ppm");
    w = 0;
    for (i = 4; i < 40; i++) {
        line_of(i, l, sizeof(l));
        if (strlen(l) > 60) { w++; }
    }
    LT_CHECK(w >= 34);
}

LT_TEST(label_shortening)
{
    char o[256];
    lt_cell_abbrev("Config Error", 20, o, sizeof(o));
    LT_CHECK_STR(o, "Config Error");
    lt_cell_abbrev("Not decel fuel cut", 12, o, sizeof(o));
    LT_CHECK(utf8_cols(o) <= 12 && o[0] == 'N');
    lt_cell_abbrev("Settings saved to flash memory", 10, o, sizeof(o));
    LT_CHECK(utf8_cols(o) <= 10 && o[0] == 'S');
    lt_cell_abbrev("ABCDEFGHIJKLMNOP", 6, o, sizeof(o));
    LT_CHECK(utf8_cols(o) == 6 && strcmp(o + strlen(o) - 3, "\xE2\x80\xA6") == 0);
    lt_cell_ellipsize_mid("Target AFR Warmup enrichment(CLT)", 20, o, sizeof(o));
    LT_CHECK(utf8_cols(o) == 20);
    LT_CHECK(strncmp(o, "Target AFR", 10) == 0 && strcmp(o + strlen(o) - 9, "ment(CLT)") == 0);
    lt_cell_ellipsize_mid("short", 20, o, sizeof(o));
    LT_CHECK_STR(o, "short");
}

LT_TEST(braille_plot)
{
    lt_cells cl;
    lt_gfx g;
    double v[16];
    int i;
    lt_cells_init(&cl, 10, 4);
    lt_gfx_init_cells(&g, &cl);
    for (i = 0; i < 16; i++) { v[i] = (double)i; }
    lt_gfx_cell_plot(&g, lt_rect_make(0, 0, 80, 64), v, 16, 0.0, 15.0, LT_RGB(255, 0, 0), LT_RGB(0, 0, 0),
                     LT_PLOT_CLEAR | LT_PLOT_STRETCH);
    LT_CHECK(cl.cells[3 * 10 + 0].ch >= 0x2801 && cl.cells[3 * 10 + 0].ch <= 0x28FF);   /* low left */
    LT_CHECK(cl.cells[0 * 10 + 9].ch >= 0x2801 && cl.cells[0 * 10 + 9].ch <= 0x28FF);   /* high right */
    LT_CHECK(cl.cells[0 * 10 + 0].ch == ' ' && cl.cells[3 * 10 + 9].ch == ' ');
    lt_cells_free(&cl);
}

LT_TEST(modal_fits_content)
{
    char l[1024];
    int row = -1, top = -1, i;
    render(120, 40, NULL, "ctrl+comma", NULL, 0, "build/ttyshot_dlg120.ppm");
    for (i = 0; i < 40; i++) {
        line_of(i, l, sizeof(l));
        if (top < 0 && strstr(l, "Settings") != NULL) { top = i; }
        if (strstr(l, "Close") != NULL) { row = i; }
    }
    /* the settings form fits its content instead of filling the whole screen */
    LT_CHECK(top >= 0 && row > top && row - top < 26);
    LT_CHECK(strstr(g_txt, "\xE2\x95\xB0") != NULL);
}

LT_TEST(sidebar_resize_keys)
{
    render(120, 40, NULL, NULL, NULL, 0, NULL);
    LT_CHECK(strstr(g_txt, "Odd Fire Cylinder offsets") == NULL);
    LT_CHECK(strstr(g_txt, "Odd Fire Cy\xE2\x80\xA6") != NULL);          /* middle ellipsis keeps the end */
    render(120, 40, NULL, "ctrl+right,ctrl+right,ctrl+right,ctrl+right,ctrl+right,ctrl+right", NULL, 0, NULL);
    LT_CHECK(strstr(g_txt, "Odd Fire Cylinder offsets") != NULL);
}

LT_TEST(logger_screens)
{
    render(120, 40, "tooth-logger", "click:79/3", NULL, 0, "build/ttyshot_tooth120.ppm");
    LT_CHECK(strstr(g_txt, "Tooth Logger") != NULL);
    LT_CHECK(strstr(g_txt, "Detected RPM") != NULL);
    render(120, 40, "composite-logger", "click:33/3", NULL, 0, "build/ttyshot_comp120.ppm");
    LT_CHECK(strstr(g_txt, "Primary pulses") != NULL);
    render(120, 40, "datalog", NULL, NULL, 0, "build/ttyshot_log120.ppm");
    LT_CHECK(strstr(g_txt, "Data Logging") != NULL);
    render(120, 40, "console", NULL, NULL, 0, "build/ttyshot_console120.ppm");
    LT_CHECK(strstr(g_txt, "onsole") != NULL);
}

LT_TEST(deterministic)
{
    char first[8192];
    render(80, 24, "veTableTbl", NULL, "256", 0, NULL);
    lt_strlcpy(first, g_txt, sizeof(first));
    render(80, 24, "veTableTbl", NULL, "256", 0, NULL);
    LT_CHECK(strcmp(first, g_txt) == 0);
}

LT_TEST(keys_script_parser)
{
    lt_event ev[32];
    int n;
    n = lt_app_shot_parse("alt+f,down,enter", ev, 32);
    LT_CHECK(n == 3);
    LT_CHECK(ev[0].type == LT_EV_KEY_DOWN && ev[0].key == 'f' && (ev[0].mods & LT_MOD_ALT));
    LT_CHECK(ev[1].key == LT_KEY_DOWN && ev[2].key == LT_KEY_ENTER);
    n = lt_app_shot_parse("click:10/3 wheel:4/5/-1 text:ab", ev, 32);
    LT_CHECK(n == 3 + 1 + 4);
    LT_CHECK(ev[0].type == LT_EV_MOUSE_MOVE && ev[0].x == 84 && ev[0].y == 56);
    LT_CHECK(ev[1].type == LT_EV_MOUSE_DOWN && ev[1].button == LT_MB_LEFT);
    LT_CHECK(ev[2].type == LT_EV_MOUSE_UP);
    LT_CHECK(ev[3].type == LT_EV_WHEEL && ev[3].wheel == -1);
    LT_CHECK(ev[4].type == LT_EV_KEY_DOWN && ev[4].key == 'a' && ev[5].type == LT_EV_TEXT);
    n = lt_app_shot_parse("ctrl+shift+tab,f10,nonsense", ev, 32);
    LT_CHECK(n == 2 && ev[0].key == LT_KEY_TAB && ev[0].mods == (LT_MOD_CTRL | LT_MOD_SHIFT));
    LT_CHECK(ev[1].key == LT_KEY_F10);
}

int main(void)
{
    LT_RUN(main_view_80x24);
    LT_RUN(main_view_120x40_256);
    LT_RUN(menu_open);
    LT_RUN(menu_shows_rebound_log_shortcut);
    LT_RUN(one_help_menu);
    LT_RUN(sidebar_search_enter_opens_first_match);
    LT_RUN(ve_table_and_selection);
    LT_RUN(ve_table_16_colors_ascii);
    LT_RUN(dialog_frame);
    LT_RUN(dashboard_bars);
    LT_RUN(dashboard_dense_panels);
    LT_RUN(label_shortening);
    LT_RUN(braille_plot);
    LT_RUN(modal_fits_content);
    LT_RUN(sidebar_resize_keys);
    LT_RUN(logger_screens);
    LT_RUN(deterministic);
    LT_RUN(keys_script_parser);
    LT_TEST_MAIN_END
}
