/* test_navigate.c - finding things and getting around: the single File > Connect entry, Alt
 * access keys of the ECU's own menus, the resizable sidebar, one-column INI panels in narrow
 * windows and the settings search (field captions, keyboard).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/app/hotkeys.h"
#include "../src/plat/plat.h"

static void sandbox(const char *tag)
{
    char cfg[160], ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(cfg, sizeof(cfg), "build/test_navigate_%s_%s_%lu", tag, ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(cfg);
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", cfg) == 0);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_CONFIG_DIR", "") == 0);
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
    ev.type = type; ev.x = x; ev.y = y; ev.button = LT_MB_LEFT;
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

int main(void)
{
    LT_RUN(connect_entries);
    LT_RUN(ini_menu_access_keys);
    LT_RUN(sidebar_resize);
    LT_TEST_MAIN_END
}
