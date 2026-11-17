/* test_ttyshot.c - snapshot tests of the terminal UI (cell mode): the app is
 * rendered headlessly with the LT_SHOT_* harness (src/app/app_ttyshot.c) at
 * 80x24 and 120x40, in truecolor / 256 / 16 colors and ASCII, and the text
 * and escape-stream dumps are checked. PPM images go to build/.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ttyshot.h"

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
    lt_app *app = lt_app_new();
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
    render(80, 30, NULL, "alt+f", NULL, 0, "build/ttyshot_menu80.ppm");   /* tall enough for the whole File menu */
    line_of(1, l, sizeof(l));
    LT_CHECK(strncmp(l, "\xE2\x95\xAD\xE2\x94\x80", 6) == 0);        /* top-left corner then a rule */
    line_of(2, l, sizeof(l));
    LT_CHECK(strncmp(l, "\xE2\x94\x82  New Project...", 19) == 0);
    LT_CHECK(strstr(l, "Ctrl+N") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x94\x9C\xE2\x94\x80") != NULL);   /* separator joins the frame */
    LT_CHECK(strstr(g_txt, "Exit") != NULL);
    LT_CHECK(strstr(g_txt, "\xE2\x95\xB0") != NULL);               /* bottom-left corner */
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
    LT_RUN(ve_table_and_selection);
    LT_RUN(ve_table_16_colors_ascii);
    LT_RUN(dialog_frame);
    LT_RUN(dashboard_bars);
    LT_RUN(deterministic);
    LT_RUN(keys_script_parser);
    LT_TEST_MAIN_END
}
