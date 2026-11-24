/* test_navigate.c - finding things and getting around: the single File > Connect entry, Alt
 * access keys of the ECU's own menus, the resizable sidebar, one-column INI panels in narrow
 * windows and the settings search (field captions, keyboard).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/dialogs_embed.h"
#include "../src/app/feat.h"
#include "../src/app/hotkeys.h"
#include "../src/app/search_index.h"
#include "../src/plat/plat.h"

static void sandbox(const char *tag)
{
    char cfg[160], ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(cfg, sizeof(cfg), "build/test_navigate_%s_%s_%lu", tag, ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(cfg);
    /* the override, not XDG_CONFIG_HOME: Windows ignores XDG and would share %APPDATA% */
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_CONFIG_DIR", cfg) == 0);
}

static int has_dialog(const lt_app *app, const char *kind)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0 && app->dialogs[i]->open) { return 1; }
    }
    return 0;
}

static void drop_dialogs(lt_app *app)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_tmp.ppm", 400, 300, 2) == 0);
}

/* ---- one connect entry ---- */
LT_TEST(connect_entries)
{
    char ini[512], dir[300];
    lt_app *app;
    lt_project p;
    int i, n;
    sandbox("conn");
    app = lt_app_new();
    /* the wizard is no separate File entry any more: File > Connect opens it when needed */
    n = lt_ext_n_menu_items(app, LT_EXT_MENU_FILE);
    for (i = 0; i < n; i++) {
        LT_CHECK(strstr(lt_ext_menu_item_label(app, LT_EXT_MENU_FILE, i), "Wizard") == NULL);
    }
    LT_CHECK(n > 3);
    /* the old shortcut id still connects; the settings dialog has its own (unbound) id */
    LT_CHECK(lt_hk_core_find("conn.connect") != NULL);
    LT_CHECK(lt_hk_core_find("conn.settings") != NULL && lt_hk_core_find("conn.settings")->key == LT_HK_NONE);
    /* definition open but no project (demo, disconnected): the wizard */
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    lt_app_connect(app);                                /* already online: nothing happens */
    LT_CHECK(app->n_dialogs == 0);
    lt_app_disconnect(app);
    lt_app_connect(app);
    LT_CHECK(has_dialog(app, "connect_wizard"));
    drop_dialogs(app);
    /* a project with a saved port connects there; when that fails the settings dialog opens */
    lt_snprintf(dir, sizeof(dir), "build/test_navigate_proj_%lu", (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(dir);
    LT_CHECK(lt_project_create("navconn", ini, app->ini->signature, dir, &p) == 0);
    lt_strlcpy(dir, p.path, sizeof(dir));
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(app->has_project && app->conn == LT_APP_OFFLINE);
    lt_app_connect(app);                                /* no saved port: the wizard */
    LT_CHECK(has_dialog(app, "connect_wizard") && !has_dialog(app, "connect"));
    drop_dialogs(app);
    lt_free(app->project.cfg.conn.port);
    app->project.cfg.conn.port = lt_strdup("/nonexistent/ttyNAV0");
    app->project.cfg.conn.baud_rate = 115200;
    lt_app_connect(app);
    LT_CHECK(app->conn == LT_APP_OFFLINE);
    LT_CHECK(has_dialog(app, "connect") && !has_dialog(app, "connect_wizard"));
    drop_dialogs(app);
    LT_CHECK(lt_app_connect_dialog(app) == 0 && has_dialog(app, "connect"));
    lt_app_free(app);
}

/* ---- Alt access keys of the ECU menus ---- */
static void check_unique_keys(lt_app *app)
{
    char seen[128], lab[160];
    size_t i;
    memset(seen, 0, sizeof(seen));
    seen['f'] = seen['e'] = seen['v'] = seen['t'] = seen['h'] = 1;     /* the shell's own */
    for (i = 0; i < app->ini->n_menus; i++) {
        char k = lt_app_menu_access_key(app, i, lab, sizeof(lab));
        if (k == 0) { continue; }
        LT_CHECK(!seen[(unsigned char)k]);
        seen[(unsigned char)k] = 1;
        LT_CHECK(strchr(lab, '&') != NULL);
    }
}

LT_TEST(ini_menu_access_keys)
{
    char ini[512], lab[160];
    lt_app *app;
    lt_event ev;
    sandbox("keys");
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_menus.ppm", 1600, 500, 2) == 0);
    /* rusEFI: the INI's own letters first ("&Setup" S, "&Idle" I), then the first free one: "Fuel"
     * (F is File) u, "Ignition" g, "&Expert/Extra" (E is Edit) x, "CAN-bus" b */
    LT_CHECK(lt_app_menu_access_key(app, 0, lab, sizeof(lab)) == 's' && strcmp(lab, "&Setup") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 1, lab, sizeof(lab)) == 'u' && strcmp(lab, "F&uel") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 2, lab, sizeof(lab)) == 'g' && strcmp(lab, "I&gnition") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 4, lab, sizeof(lab)) == 'i' && strcmp(lab, "&Idle") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 7, lab, sizeof(lab)) == 'b' && strcmp(lab, "CAN-&bus") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 9, lab, sizeof(lab)) == 'x' && strcmp(lab, "E&xpert/Extra") == 0);
    check_unique_keys(app);
    /* Alt+U opens Fuel */
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_KEY_DOWN; ev.key = 'u'; ev.mods = LT_MOD_ALT;
    lt_ui_input(app->ui, &ev);
    lt_ui_begin(app->ui, &app->gfx, 5000.0);
    lt_app_frame(app);
    lt_ui_end(app->ui);
    LT_CHECK(lt_ui_popup_open(app->ui));
    /* too narrow for every menu: they move into Tuning, whose T belongs to Tools */
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_menus.ppm", 800, 500, 2) == 0);
    LT_CHECK(lt_app_menu_access_key(app, (size_t)-1, lab, sizeof(lab)) == 'u' && strcmp(lab, "T&uning") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 1, lab, sizeof(lab)) == 0);
    lt_app_free(app);
    /* Speeduino: "&Tuning", "&Spark", "&Startup/Idle", an INI menu called Tools... */
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, "tests/fixtures/ini/speeduino_202501.ini") == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_menus.ppm", 2400, 500, 2) == 0);
    LT_CHECK(lt_app_menu_access_key(app, 1, lab, sizeof(lab)) == 'u' && strcmp(lab, "T&uning") == 0);
    LT_CHECK(lt_app_menu_access_key(app, 2, lab, sizeof(lab)) == 's' && strcmp(lab, "&Spark") == 0);
    check_unique_keys(app);
    lt_app_free(app);
}

/* ---- input helpers: one event, one frame (the clock moves 80 ms per frame) ---- */
static double g_t = 10000.0;

static void run1(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, g_t);
    lt_app_frame(app);
    lt_ui_end(app->ui);
    g_t += 80.0;
}

static void mouse(lt_app *app, int type, int x, int y)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (lt_ev_type)type; ev.x = x; ev.y = y; ev.button = LT_MB_LEFT;
    lt_ui_input(app->ui, &ev);
    run1(app);
}

static void key(lt_app *app, int k, int mods)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_KEY_DOWN; ev.key = k; ev.mods = mods;
    lt_ui_input(app->ui, &ev);
    run1(app);
}

static void text(lt_app *app, const char *s)
{
    lt_event ev;
    for (; *s != '\0'; s++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = LT_EV_KEY_DOWN; ev.key = (unsigned char)*s;
        lt_ui_input(app->ui, &ev);
        memset(&ev, 0, sizeof(ev));
        ev.type = LT_EV_TEXT; ev.text[0] = *s;
        lt_ui_input(app->ui, &ev);
        run1(app);
    }
}

static void drag(lt_app *app, int x0, int y, int x1)
{
    mouse(app, LT_EV_MOUSE_MOVE, x0, y);
    mouse(app, LT_EV_MOUSE_DOWN, x0, y);
    mouse(app, LT_EV_MOUSE_MOVE, (x0 + x1) / 2, y);
    mouse(app, LT_EV_MOUSE_MOVE, x1, y);
    mouse(app, LT_EV_MOUSE_UP, x1, y);
    mouse(app, LT_EV_MOUSE_MOVE, x1 + 300, y + 100);
}

/* x of the sidebar's border (the splitter bar) in device px */
static int edge_x(lt_app *app)
{
    return lt_ui_px(app->ui, app->settings.sidebar_w) + lt_ui_theme(app->ui)->splitter_w / 2;
}

/* ---- the sidebar: drag to resize, double-click resets, width and F9 state are kept ---- */
LT_TEST(sidebar_resize)
{
    char ini[512];
    lt_app *app;
    int w;
    sandbox("side");
    app = lt_app_new();
    LT_CHECK(app->settings.sidebar_w == 260 && app->settings.sidebar_on);
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_side.ppm", 1280, 800, 2) == 0);
    drag(app, edge_x(app), 400, edge_x(app) + 140);
    w = app->settings.sidebar_w;
    LT_CHECK(w >= 395 && w <= 405);
    /* saved when the drag ends */
    app->settings.sidebar_w = 0;
    lt_app_settings_load(app);
    LT_CHECK(app->settings.sidebar_w == w);
    /* at most half the window, at least 180 px */
    drag(app, edge_x(app), 400, 1200);
    LT_CHECK(app->settings.sidebar_w >= 630 && app->settings.sidebar_w <= 640);
    drag(app, edge_x(app), 400, 20);
    LT_CHECK(app->settings.sidebar_w == 180);
    /* double-click: the default width, saved */
    drag(app, edge_x(app), 400, 500);
    LT_CHECK(app->settings.sidebar_w > 400);
    mouse(app, LT_EV_MOUSE_MOVE, edge_x(app), 300);
    mouse(app, LT_EV_MOUSE_DOWN, edge_x(app), 300);
    mouse(app, LT_EV_MOUSE_UP, edge_x(app), 300);
    mouse(app, LT_EV_MOUSE_DOWN, edge_x(app), 300);
    mouse(app, LT_EV_MOUSE_MOVE, edge_x(app) + 40, 300);     /* no drag after the double-click */
    mouse(app, LT_EV_MOUSE_UP, edge_x(app) + 40, 300);
    LT_CHECK(app->settings.sidebar_w == 260);
    app->settings.sidebar_w = 0;
    lt_app_settings_load(app);
    LT_CHECK(app->settings.sidebar_w == 260);
    /* at 200% the limits are the same logical sizes */
    lt_plat_setenv("LT_SCALE", "200");
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_side.ppm", 1920, 1080, 2) == 0);
    lt_plat_setenv("LT_SCALE", "");
    drag(app, edge_x(app), 400, 10);
    LT_CHECK(app->settings.sidebar_w == 180);
    drag(app, edge_x(app), 400, 1900);
    LT_CHECK(app->settings.sidebar_w >= 470 && app->settings.sidebar_w <= 480);
    /* a smaller window draws it narrower but keeps the setting */
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_side.ppm", 800, 600, 2) == 0);
    LT_CHECK(app->settings.sidebar_w >= 470);
    /* F9 hides it, and that is kept too */
    key(app, LT_KEY_F9, 0);
    LT_CHECK(!app->settings.sidebar_on);
    app->settings.sidebar_on = 1;
    lt_app_settings_load(app);
    LT_CHECK(!app->settings.sidebar_on);
    key(app, LT_KEY_F9, 0);
    LT_CHECK(app->settings.sidebar_on);
    lt_app_free(app);
}

/* ---- narrow windows: side-by-side INI panels fall back to one column ---- */
/* a dialog that lays panels side by side, or holds one that does */
static int has_columns(const lt_ini *ini, const lt_ini_dialog *d, int depth)
{
    size_t k;
    if (d->layout_hint != NULL) { return 1; }
    for (k = 0; k < d->n_comps && depth < 5; k++) {
        const lt_ini_dialog *sub = d->comps[k].kind == LT_COMP_PANEL && d->comps[k].name != NULL
                                   ? lt_ini_find_dialog(ini, d->comps[k].name) : NULL;
        if (sub != NULL && sub != d && has_columns(ini, sub, depth + 1)) { return 1; }
    }
    return 0;
}

/* every such dialog of ini_path at w x h (scale pct, or "tty": the terminal UI, w x h cells): how
 * many overflow sideways; their names are printed */
static int overflowing_dialogs(const char *ini_path, int w, int h, const char *scale)
{
    lt_app *app = lt_app_new();
    size_t i;
    int bad = 0, n = 0, ov = 0, tty = strcmp(scale, "tty") == 0;
    LT_CHECK(lt_app_start_demo(app, ini_path) == 0);
    lt_plat_setenv(tty ? "LT_SHOT_TEXT" : "LT_SCALE", tty ? "build/test_navigate_narrow.txt" : scale);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_narrow.ppm", w, h, 1) == 0);
    lt_plat_setenv(tty ? "LT_SHOT_TEXT" : "LT_SCALE", "");
    for (i = 0; app->ini != NULL && i < app->ini->n_dialogs; i++) {
        const lt_ini_dialog *d = app->ini->dialogs[i];
        lt_app_tab *t;
        if (d->name == NULL || d->n_comps == 0 || !has_columns(app->ini, d, 0)) { continue; }
        if (lt_app_dialog_open(app, d->name) != 0) { continue; }
        run1(app); run1(app); run1(app); run1(app); run1(app); run1(app); run1(app); run1(app);
        if (lt_dlg_ini_stacked(app, d->name, &ov) >= 0 && ov > 0) {
            printf("    %s overflows at %dx%d@%s: %d px\n", d->name, w, h, scale, ov);
            bad++;
        }
        n++;
        t = lt_app_tab_find(app, "dialog", d->name);
        if (t != NULL) { lt_app_tab_close(app, t); }
    }
    LT_CHECK(n > 0);
    lt_app_free(app);
    return bad;
}

LT_TEST(narrow_panels)
{
    char ini[512];
    lt_app *app;
    int ov = -1;
    sandbox("narrow");
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, ini) == 0);
    /* the known cases: gauges | fields | gauge, fields | table */
    LT_CHECK(lt_app_dialog_open(app, "fuelClosedLoopDialog") == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_fcl800.ppm", 800, 600, 6) == 0);
    LT_CHECK(lt_dlg_ini_stacked(app, "fuelClosedLoopDialog", &ov) >= 1 && ov == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_fcl1280.ppm", 1280, 800, 6) == 0);
    LT_CHECK(lt_dlg_ini_stacked(app, "fuelClosedLoopDialog", &ov) == 0 && ov == 0);
    LT_CHECK(lt_app_dialog_open(app, "gppwm1") == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_gppwm800.ppm", 800, 600, 6) == 0);
    LT_CHECK(lt_dlg_ini_stacked(app, "gppwm1", &ov) == 1 && ov == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_gppwm1280.ppm", 1280, 800, 6) == 0);
    LT_CHECK(lt_dlg_ini_stacked(app, "gppwm1", &ov) == 0 && ov == 0);
    lt_app_free(app);
    /* nothing overflows sideways, in any dialog of the rusEFI and Speeduino INIs (the MegaSquirt
     * fixtures are excerpts without dialogs) */
    LT_CHECK(overflowing_dialogs(ini, 800, 600, "100") == 0);
    LT_CHECK(overflowing_dialogs(ini, 1920, 1080, "200") == 0);
    LT_CHECK(overflowing_dialogs("tests/fixtures/ini/speeduino_202501.ini", 800, 600, "100") == 0);
    LT_CHECK(overflowing_dialogs("tests/fixtures/ini/speeduino_202501.ini", 1920, 1080, "200") == 0);
    LT_CHECK(overflowing_dialogs(ini, 100, 30, "tty") == 0);
    LT_CHECK(overflowing_dialogs("tests/fixtures/ini/speeduino_202501.ini", 100, 30, "tty") == 0);
}

/* ---- settings search: the fields inside the dialogs ---- */
static int all_true(void *ud, const char *c) { LT_UNUSED(ud); LT_UNUSED(c); return 1; }
static int all_false(void *ud, const char *c) { LT_UNUSED(ud); return c == NULL || c[0] == '\0'; }
static int app_cond(void *ud, const char *c)
{
    lt_app *app = (lt_app *)ud;
    return c == NULL || c[0] == '\0' || lt_expr_cache_test(&app->expr, c, &app->varenv.env);
}

LT_TEST(settings_index)
{
    char ini_path[512];
    lt_ini *ini;
    lt_sidx *x;
    const lt_sidx_hit *h = NULL;
    size_t n, total = 0, k, nfree = 0;
    int found = 0;
    /* whole word > start of a word > inside a word; camelCase and snake_case split words */
    LT_CHECK(lt_sidx_score("P factor", "factor") == 3);
    LT_CHECK(lt_sidx_score("P factor", "fact") == 2);
    LT_CHECK(lt_sidx_score("Interfactor", "fact") == 1);
    LT_CHECK(lt_sidx_score("idleRpmPid_pFactor", "factor") == 3);
    LT_CHECK(lt_sidx_score("idleRpmPid_pFactor", "rpm") == 3);
    LT_CHECK(lt_sidx_score("Boost control P gain", "boost gain") == 6);
    LT_CHECK(lt_sidx_score("Boost control P gain", "boost zzz") == 0);
    LT_CHECK(lt_sidx_score("ALS timeout", "TIMEOUT") == 3);
    ini = lt_ini_load_file(lt_app_find_demo_ini(ini_path, sizeof(ini_path)), NULL);
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    x = lt_sidx_build(ini);
    LT_CHECK(lt_sidx_count(x) > 1000);
    /* by caption, best first */
    n = lt_sidx_query(x, "pfactor", 50, &h, &total);
    LT_CHECK(n > 3 && total >= n);
    for (k = 0; k < n; k++) {
        if (k > 0) { LT_CHECK(h[k - 1].score >= h[k].score); }
        if (strcmp(h[k].caption, "pFactor") == 0 && strcmp(h[k].target, "etbDialog") == 0) {
            found = 1;
            LT_CHECK(strcmp(h[k].title, "Electronic throttle body") == 0);   /* the menu's name, no '&' */
        }
    }
    LT_CHECK(found);
    LT_CHECK(h[0].score == 4 && strcmp(h[0].caption, "pFactor") == 0);   /* the whole caption typed */
    /* and by constant name: "Proportional gain" is idleTimingPid_pFactor */
    found = 0;
    for (k = 0; k < n; k++) {
        if (strcmp(h[k].caption, "Proportional gain") == 0) {
            found = 1;
            LT_CHECK(h[k].comp->name != NULL && strstr(h[k].comp->name, "pFactor") != NULL);
        }
    }
    LT_CHECK(found);
    /* exact word before prefix before inside; on a tie a match in the caption before one in the
     * constant name only, then the shorter caption */
    n = lt_sidx_query(x, "idle", 50, &h, &total);
    LT_CHECK(n == 50 && total > 50);
    for (k = 1; k < n; k++) {
        LT_CHECK(h[k - 1].score > h[k].score ||
                 (h[k - 1].score == h[k].score && h[k - 1].in_caption > h[k].in_caption) ||
                 (h[k - 1].score == h[k].score && h[k - 1].in_caption == h[k].in_caption &&
                  strlen(h[k - 1].caption) <= strlen(h[k].caption)));
    }
    n = lt_sidx_query(x, "injection", 50, &h, &total);
    LT_CHECK(n > 5 && h[0].in_caption == 1 && strcmp(h[0].caption, "Mode") != 0);   /* not injectionMode's "Mode" */
    /* the same text again is the cached list */
    LT_CHECK(lt_sidx_query(x, "idle", 50, &h, NULL) == 50);
    /* every word must match */
    n = lt_sidx_query(x, "boost gain", 50, &h, &total);
    LT_CHECK(n > 0);
    for (k = 0; k < n; k++) { LT_CHECK(lt_sidx_score(h[k].caption, "gain") > 0 || lt_sidx_score(h[k].comp->name, "gain") > 0); }
    LT_CHECK(lt_sidx_query(x, "zzqqxx", 50, &h, &total) == 0 && total == 0);
    /* visibility goes through the field's, its panels' and its menu entry's conditions */
    n = lt_sidx_query(x, "pfactor", 50, &h, NULL);
    for (k = 0; k < n; k++) {
        LT_CHECK(lt_sidx_visible(x, h[k].entry, all_true, NULL));
        if (!lt_sidx_visible(x, h[k].entry, all_false, NULL)) { nfree++; }
    }
    LT_CHECK(nfree > 0);
    lt_sidx_free(x);
    lt_ini_free(ini);
    /* Speeduino */
    ini = lt_ini_load_file("tests/fixtures/ini/speeduino_202501.ini", NULL);
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    x = lt_sidx_build(ini);
    LT_CHECK(lt_sidx_count(x) > 300);
    n = lt_sidx_query(x, "pairing", 10, &h, NULL);
    LT_CHECK(n >= 1 && strcmp(h[0].caption, "Injector Pairing") == 0);
    lt_sidx_free(x);
    lt_ini_free(ini);
}

/* a key and the frame after it (a text field ends its editing when it is next drawn) */
static void nav(lt_app *app, int k)
{
    key(app, k, 0);
    run1(app);
}

static int sidebar_has(const char *txt, const char *s, int *line)
{
    const char *p = strstr(txt, s), *q;
    if (p == NULL) { return 0; }
    *line = 0;
    for (q = txt; q < p; q++) { if (*q == '\n') { (*line)++; } }
    return 1;
}

LT_TEST(settings_search)
{
    char ini[512];
    lt_app *app;
    const lt_sidx_hit *h = NULL;
    lt_u32 f1, f2, ret;
    size_t n;
    /* first query with a hit hidden by its own condition (SPI pins with SPI off, custom trigger) */
    static const char *const hidden_q[] = { "mosi", "tooth count" };
    size_t q;
    int i, hid = -1;
    sandbox("search");
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_search.ppm", 1280, 800, 2) == 0);
    /* Ctrl+K, a field's caption, Enter: its dialog opens as a tab, the field gets the focus and an
     * outline for 1.5 s */
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    LT_CHECK(lt_ui_text_focused(app->ui));
    text(app, "ifactor");
    LT_CHECK(app->sidx != NULL);
    key(app, LT_KEY_ENTER, 0);
    run1(app); run1(app);
    LT_CHECK(lt_app_tab_find(app, "dialog", "etbDialog") != NULL);
    LT_CHECK(lt_dlg_ini_revealed(app, "etbDialog"));
    LT_CHECK(lt_ui_text_focused(app->ui) && !app->search_in);    /* editing the iFactor field */
    ret = lt_ui_focus(app->ui);
    for (i = 0; i < 25; i++) { run1(app); }
    LT_CHECK(!lt_dlg_ini_revealed(app, "etbDialog") && app->reveal.comp == NULL);
    LT_CHECK(lt_ui_focus(app->ui) == ret);
    /* back in the search: Down into the results, Up and Down through them, Up from the first one
     * back to the box; Esc there empties it and gives the focus back to the iFactor field */
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    LT_CHECK(lt_ui_text_focused(app->ui) && app->search_in);
    key(app, 'a', LT_MOD_CTRL);
    text(app, "pfactor");                     /* no menu entry has it: the results are fields only */
    LT_CHECK(strcmp(app->search, "pfactor") == 0);
    nav(app, LT_KEY_DOWN);
    f1 = lt_ui_focus(app->ui);
    LT_CHECK(!lt_ui_text_focused(app->ui) && f1 != 0 && app->search_in);
    nav(app, LT_KEY_DOWN);
    f2 = lt_ui_focus(app->ui);
    LT_CHECK(f2 != f1 && f2 != 0);
    nav(app, LT_KEY_UP);
    LT_CHECK(lt_ui_focus(app->ui) == f1);
    nav(app, LT_KEY_UP);
    LT_CHECK(lt_ui_text_focused(app->ui));
    nav(app, LT_KEY_DOWN);
    LT_CHECK(lt_ui_focus(app->ui) == f1);
    key(app, LT_KEY_ESCAPE, 0);
    run1(app);
    LT_CHECK(app->search[0] == '\0' && lt_ui_focus(app->ui) == ret && !app->search_in);
    /* Esc in the box itself does the same */
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    text(app, "boost");
    nav(app, LT_KEY_ESCAPE);
    LT_CHECK(app->search[0] == '\0' && lt_ui_focus(app->ui) == ret);
    /* Enter on a result opens that one */
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    text(app, "pfactor");
    n = lt_sidx_query(app->sidx, "pfactor", 50, &h, NULL);
    LT_CHECK(n > 3);
    nav(app, LT_KEY_DOWN);
    nav(app, LT_KEY_DOWN);
    nav(app, LT_KEY_DOWN);
    key(app, LT_KEY_ENTER, 0);
    run1(app); run1(app);
    LT_CHECK(lt_app_tab_active(app) != NULL && strcmp(lt_app_tab_active(app)->target, h[2].target) == 0);
    LT_CHECK(lt_dlg_ini_revealed(app, h[2].target));
    /* the clear button is not in the Tab order: Tab from the box goes to "Expand all" */
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    i = app->tree_expanded;
    nav(app, LT_KEY_TAB);
    LT_CHECK(!lt_ui_text_focused(app->ui));
    key(app, LT_KEY_ENTER, 0);
    LT_CHECK(app->tree_expanded != i && strcmp(app->search, "pfactor") == 0);
    /* a hidden field is found too; opening it says why it cannot be seen */
    for (q = 0; q < sizeof(hidden_q) / sizeof(hidden_q[0]) && hid < 0; q++) {
        n = lt_sidx_query(app->sidx, hidden_q[q], 50, &h, NULL);
        for (i = 0; i < (int)n && hid < 0; i++) { if (!lt_sidx_visible(app->sidx, h[i].entry, app_cond, app)) { hid = i; } }
    }
    LT_CHECK(hid >= 0);
    q--;                                                      /* the query that found it */
    nav(app, LT_KEY_ESCAPE);
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    text(app, hidden_q[q]);
    for (i = 0; i <= hid; i++) { nav(app, LT_KEY_DOWN); }
    nav(app, LT_KEY_ENTER);
    LT_CHECK(strcmp(lt_app_tab_active(app)->target, h[hid].target) == 0);
    LT_CHECK(strstr(app->status, "is hidden by another setting") != NULL);
    for (i = 0; i < 40; i++) { run1(app); }
    LT_CHECK(app->reveal.comp == NULL);                       /* not drawn: given up */
    /* Ctrl+K with the sidebar hidden shows it */
    nav(app, LT_KEY_F9);
    LT_CHECK(!app->settings.sidebar_on);
    key(app, 'k', LT_MOD_CTRL);
    run1(app);
    run1(app);
    LT_CHECK(app->settings.sidebar_on && lt_ui_text_focused(app->ui));
    /* a new definition drops the index */
    lt_app_close_project(app);
    LT_CHECK(app->sidx == NULL && app->search[0] == '\0');
    lt_app_free(app);
}

/* the terminal UI: "Settings" below the menu entries, hidden ones say so */
LT_TEST(settings_search_tty)
{
    char ini[512], *txt = NULL;
    lt_app *app;
    size_t len = 0;
    int l_tree = -1, l_set = -1, l_field = -1, l_hidden = -1;
    sandbox("searchtty");
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    app->settings.sidebar_cols = 60;
    lt_strlcpy(app->search, "injection", sizeof(app->search));
    lt_plat_setenv("LT_SHOT_TEXT", "build/test_navigate_search.txt");
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_search_tty.ppm", 160, 60, 3) == 0);
    txt = lt_file_read("build/test_navigate_search.txt", &len);
    LT_CHECK(txt != NULL);
    if (txt != NULL) {
        LT_CHECK(sidebar_has(txt, "Injection configuration", &l_tree));
        LT_CHECK(sidebar_has(txt, "\n Settings ", &l_set));
        LT_CHECK(sidebar_has(txt, " - Injection", &l_field));
        LT_CHECK(l_tree >= 0 && l_set > l_tree && l_field > l_set);
        lt_free(txt);
        txt = NULL;
    }
    lt_strlcpy(app->search, "pfactor", sizeof(app->search));
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_search_tty.ppm", 160, 60, 3) == 0);
    txt = lt_file_read("build/test_navigate_search.txt", &len);
    LT_CHECK(txt != NULL);
    if (txt != NULL) {
        LT_CHECK(sidebar_has(txt, "pFactor - Electronic throttle body", &l_field));
        LT_CHECK(strstr(txt, "No menu entry or setting matches") == NULL);
        lt_free(txt);
        txt = NULL;
    }
    lt_strlcpy(app->search, "mosi", sizeof(app->search));          /* SPI pins, SPI is off */
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_search_tty.ppm", 160, 60, 3) == 0);
    txt = lt_file_read("build/test_navigate_search.txt", &len);
    LT_CHECK(txt != NULL);
    if (txt != NULL) {
        LT_CHECK(sidebar_has(txt, "(hidden by another setting)", &l_hidden));
        lt_free(txt);
        txt = NULL;
    }
    lt_strlcpy(app->search, "zzqqxx", sizeof(app->search));
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_search_tty.ppm", 160, 60, 3) == 0);
    txt = lt_file_read("build/test_navigate_search.txt", &len);
    LT_CHECK(txt != NULL);
    if (txt != NULL) { LT_CHECK(strstr(txt, "No menu entry or setting matches") != NULL); lt_free(txt); }
    lt_plat_setenv("LT_SHOT_TEXT", "");
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(settings_index);
    LT_RUN(settings_search);
    LT_RUN(settings_search_tty);
    LT_RUN(narrow_panels);
    LT_RUN(connect_entries);
    LT_RUN(ini_menu_access_keys);
    LT_RUN(sidebar_resize);
    LT_TEST_MAIN_END
}
