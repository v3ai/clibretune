/* test_app.c - application shell lifecycle without a window: demo connect,
 * realtime poll, edit -> ECU write -> burn, save/load tune, project round trip.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"

static const char *demo_ini(char *buf, size_t cap)
{
    return lt_app_find_demo_ini(buf, cap);
}

static const lt_ini_const *first_scalar(const lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (!c->is_pc_var && c->shape.cols <= 1 && c->shape.rows <= 1 && c->type != LT_DT_BITS &&
            c->size_bytes > 0 && c->max > c->min) {
            return c;
        }
    }
    return NULL;
}

LT_TEST(demo_edit_burn_save)
{
    char buf[512], tmp[300];
    lt_app *app = lt_app_new();
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v = 0.0, nv;
    int i;
    LT_CHECK(demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(app->conn == LT_APP_DEMO);
    LT_CHECK(app->n_tabs == 1);                       /* dashboard */
    for (i = 0; i < 3; i++) { lt_plat_sleep_ms(60); lt_app_service(app); }
    LT_CHECK(app->frames_ok >= 2);
    LT_CHECK(!lt_app_can_burn(app));
    c = first_scalar(app->ini);
    LT_CHECK(c != NULL);
    if (c != NULL) {
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        nv = c->min + (c->max - c->min) / 2.0;
        if (nv == v) { nv = c->min; }
        LT_CHECK(lt_ini_set(app->ini, &ps, c->name, nv) == 0);
        lt_app_edited(app);
        LT_CHECK(app->modified);
        LT_CHECK(lt_app_can_burn(app));
        LT_CHECK(lt_app_burn(app) == 0);
        LT_CHECK(!lt_app_can_burn(app));
        lt_snprintf(tmp, sizeof(tmp), "build/test_app_tune.msq");
        LT_CHECK(lt_app_save_tune_as(app, tmp) == 0);
        LT_CHECK(!app->modified);
        lt_app_disconnect(app);
        LT_CHECK(app->conn == LT_APP_OFFLINE);
        /* the edit survived the disconnect in the tune */
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        LT_CHECK(v != 0.0 || nv == 0.0);
        LT_CHECK(lt_app_load_tune(app, tmp) == 0);
    }
    lt_app_free(app);
}

LT_TEST(tabs_and_dialogs)
{
    char buf[512];
    lt_app *app = lt_app_new();
    lt_app_tab *t;
    LT_CHECK(lt_app_open_ini(app, demo_ini(buf, sizeof(buf))) == 0);
    LT_CHECK(lt_app_open_target(app, "veTableDialog", "VE") == 0 || app->n_tabs >= 1);
    t = lt_app_tab_open(app, "dialog", "x", "X", 1);
    LT_CHECK(t != NULL);
    LT_CHECK(lt_app_tab_open(app, "dialog", "x", "X", 1) == t);   /* dedup */
    LT_CHECK(lt_app_tab_open(app, "nosuchkind", "x", "X", 1) == NULL);
    lt_app_tab_close(app, t);
    LT_CHECK(lt_app_tab_find(app, "dialog", "x") == NULL);
    LT_CHECK(lt_app_dialog_push(app, "settings", "", "S") != NULL);
    lt_app_free(app);
}

LT_TEST(scale_and_geometry_settings)
{
    lt_app *app = lt_app_new();
    lt_app_settings *s;
    lt_app_settings d;
    char dir[300];
    int w, h, x, y, fl;
    /* scale selection: env > setting > hint, rounded to 25 and clamped */
    LT_CHECK(lt_app_pick_scale(NULL, 0, 1.0) == 100);
    LT_CHECK(lt_app_pick_scale(NULL, 0, 1.5) == 150);
    LT_CHECK(lt_app_pick_scale(NULL, 0, 1.3) == 125);
    LT_CHECK(lt_app_pick_scale(NULL, 0, 2.0) == 200);
    LT_CHECK(lt_app_pick_scale(NULL, 0, 9.0) == 300);
    LT_CHECK(lt_app_pick_scale(NULL, 0, 0.5) == 100);
    LT_CHECK(lt_app_pick_scale(NULL, 175, 1.0) == 175);
    LT_CHECK(lt_app_pick_scale("150", 175, 1.0) == 150);
    LT_CHECK(lt_app_pick_scale("auto", 175, 1.0) == 175);
    LT_CHECK(lt_app_pick_scale("2", 0, 1.0) == 200);       /* old style 1..3 */
    LT_CHECK(lt_app_pick_scale("junk", 0, 1.25) == 125);
    /* settings round trip */
    lt_snprintf(dir, sizeof(dir), "build/test_app_settings");
    lt_plat_mkdirs(dir);
    lt_free(app->config_dir);
    app->config_dir = lt_strdup(dir);
    s = &app->settings;
    lt_app_settings_load(app);
    LT_CHECK(s->scale_pct == 0 || s->scale_pct >= 100);
    s->scale_pct = 150; s->win_w = 1400; s->win_h = 900; s->win_x = 40; s->win_y = 60;
    s->win_pos = 1; s->win_max = 1;
    lt_app_settings_save(app);
    memset(&app->settings, 0, sizeof(app->settings));
    lt_app_settings_load(app);
    LT_CHECK(s->scale_pct == 150 && s->win_w == 1400 && s->win_h == 900);
    LT_CHECK(s->win_pos == 1 && s->win_x == 40 && s->win_y == 60 && s->win_max == 1);
    s->scale_pct = 0; s->win_pos = 0; s->win_max = 0; s->win_w = 0; s->win_h = 0;
    lt_app_settings_save(app);
    lt_app_settings_load(app);
    LT_CHECK(s->scale_pct == 0 && s->win_w == 0 && s->win_pos == 0 && s->win_max == 0);
    {
        /* files from older builds: their "scale" key held a fixed default (1, later
         * rewritten as 100), not a choice, so it must not pin the scale: auto */
        static const char legacy[] = "scale=100\nwin_w=99999\nwin_h=100\nwin_x=5\nwin_y=6\n";
        static const char legacy1[] = "scale=1\n";
        static const char chosen[] = "scale=100\nui_scale=175\n";
        char *p = lt_path_join(dir, "settings.conf");
        char *txt = lt_file_read(p, NULL);
        LT_CHECK(txt != NULL && strstr(txt, "ui_scale=auto") != NULL);
        LT_CHECK(txt != NULL && strstr(txt, "\nscale=") == NULL && strncmp(txt, "scale=", 6) != 0);
        lt_free(txt);
        LT_CHECK(lt_file_write(p, legacy1, sizeof(legacy1) - 1) == 0);
        s->scale_pct = 150;
        lt_app_settings_load(app);
        LT_CHECK(s->scale_pct == 0);
        LT_CHECK(lt_file_write(p, chosen, sizeof(chosen) - 1) == 0);
        lt_app_settings_load(app);
        LT_CHECK(s->scale_pct == 175);                   /* the new key is honoured */
        LT_CHECK(lt_file_write(p, legacy, sizeof(legacy) - 1) == 0);
        lt_free(p);
    }
    lt_app_settings_load(app);
    LT_CHECK(s->scale_pct == 0);                         /* stale 100 no longer pins it */
    LT_CHECK(lt_app_pick_scale(NULL, s->scale_pct, 2.0) == 200);
    LT_CHECK(s->win_w == 0 && s->win_h == 0);        /* insane size dropped */
    LT_CHECK(s->win_pos == 1 && s->win_x == 5);
    /* window plan: default 1280x800 logical */
    memset(&d, 0, sizeof(d));
    lt_app_pick_window(&d, 100, 1920, 1080, &w, &h, &x, &y, &fl);
    LT_CHECK(w == 1280 && h == 800 && fl == 0);
    lt_app_pick_window(&d, 150, 2560, 1440, &w, &h, &x, &y, &fl);
    LT_CHECK(w == 1920 && h == 1200);
    lt_app_pick_window(&d, 200, 2560, 1440, &w, &h, &x, &y, &fl);   /* 2560x1600 > screen: clamped */
    LT_CHECK(w == 2560 && h == 1440);
    d.win_w = 1500; d.win_h = 950; d.win_x = 100; d.win_y = 50; d.win_pos = 1; d.win_max = 1;
    lt_app_pick_window(&d, 100, 1920, 1080, &w, &h, &x, &y, &fl);
    LT_CHECK(w == 1500 && h == 950 && x == 100 && y == 50);
    LT_CHECK((fl & LT_GEOM_POS) && (fl & LT_GEOM_MAX));
    lt_app_pick_window(&d, 150, 1920, 1080, &w, &h, &x, &y, &fl);   /* 2250x1425 does not fit: default */
    LT_CHECK(w == 1920 && h == 1080);                                /* default 1920x1200 clamped */
    d.win_x = 5000;                                                  /* off screen: position dropped */
    lt_app_pick_window(&d, 100, 1920, 1080, &w, &h, &x, &y, &fl);
    LT_CHECK(!(fl & LT_GEOM_POS));
    d.win_w = 800; d.win_h = 500;                                    /* below the 1024x700 minimum */
    lt_app_pick_window(&d, 100, 1920, 1080, &w, &h, &x, &y, &fl);
    LT_CHECK(w == 1280 && h == 800);
    lt_app_free(app);
}

/* ---- extension registry ---- */
static int hit_n;
static char hit_target[64];
static int my_target(lt_app *app, const char *t)
{
    LT_UNUSED(app);
    hit_n++;
    lt_strlcpy(hit_target, t, sizeof(hit_target));
    return 0;
}
static int my_tick_n;
static void my_tick(lt_app *app) { LT_UNUSED(app); my_tick_n++; }
static void my_action(lt_app *app) { LT_UNUSED(app); }
static void my_page(lt_app *app) { LT_UNUSED(app); }
static void my_free(lt_app *app, void *st) { LT_UNUSED(app); *(int *)st = 99; }

LT_TEST(ext_registry)
{
    char buf[512];
    lt_app *app = lt_app_new();
    int *st, *st2, n_pages, n_help, n_tools;
    LT_CHECK(app->ext != NULL);
    /* builtin features are registered */
    LT_CHECK(lt_ext_has_target(app, "std_realtime"));
    LT_CHECK(lt_ext_has_target(app, "och-status"));
    LT_CHECK(!lt_ext_has_target(app, "std_unit_test_target"));
    LT_CHECK(lt_ext_dispatch_target(app, "std_unit_test_target") == LT_EXT_NO_TARGET);
    n_pages = lt_ext_n_settings_pages(app);
    n_help = lt_ext_n_menu_items(app, LT_EXT_MENU_HELP);
    n_tools = lt_ext_n_menu_items(app, LT_EXT_MENU_TOOLS);
    LT_CHECK(n_pages >= 1);
    LT_CHECK(lt_ext_settings_title(app, 0) != NULL);
    LT_CHECK(n_help >= 2);
    LT_CHECK(strcmp(lt_ext_menu_item_label(app, LT_EXT_MENU_HELP, n_help - 1), "&About LibreTune") == 0);
    LT_CHECK(lt_ext_menu_item_label(app, LT_EXT_MENU_HELP, n_help) == NULL);
    /* a feature registers itself */
    lt_ext_add_target(app->ext, "std_unit_test_target", my_target);
    lt_ext_add_menu_item(app->ext, LT_EXT_MENU_TOOLS, "Unit &Test", NULL, my_action, NULL);
    lt_ext_add_settings_page(app->ext, "Unit Test", my_page);
    lt_ext_add_tick(app->ext, my_tick);
    LT_CHECK(lt_ext_has_target(app, "std_unit_test_target"));
    LT_CHECK(lt_ext_dispatch_target(app, "std_unit_test_target") == 0);
    LT_CHECK(hit_n == 1 && strcmp(hit_target, "std_unit_test_target") == 0);
    LT_CHECK(lt_ext_n_menu_items(app, LT_EXT_MENU_TOOLS) == n_tools + 1);
    LT_CHECK(lt_ext_n_settings_pages(app) == n_pages + 1);
    LT_CHECK(strcmp(lt_ext_settings_title(app, n_pages), "Unit Test") == 0);
    LT_CHECK(lt_ext_settings_title(app, n_pages + 1) == NULL);
    /* re-registering a name replaces the handler, no duplicate */
    lt_ext_add_target(app->ext, "std_unit_test_target", my_target);
    LT_CHECK(lt_ext_dispatch_target(app, "std_unit_test_target") == 0 && hit_n == 2);
    /* tick hooks run from the service loop even offline */
    lt_app_service(app);
    LT_CHECK(my_tick_n == 1);
    /* state blocks are zeroed, stable and cleaned up */
    st = (int *)lt_ext_state(app, "unit", sizeof(int), my_free);
    LT_CHECK(st != NULL && *st == 0);
    *st = 5;
    st2 = (int *)lt_ext_state(app, "unit", sizeof(int), my_free);
    LT_CHECK(st2 == st && *st2 == 5);
    /* open_target reaches registered targets (needs an ini) and still rejects unknown ones */
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_open_ini(app, buf) == 0);
    hit_n = 0;
    LT_CHECK(lt_app_open_target(app, "std_unit_test_target", "x") == 0 && hit_n == 1);
    LT_CHECK(lt_app_open_target(app, "no_such_target_zz", "x") < 0);
    /* shipped dialogs resolve through the dialog registry */
    LT_CHECK(lt_app_dialog_push(app, "about", "", "About") != NULL);
    LT_CHECK(lt_app_dialog_push(app, "shortcuts", "", "Shortcuts") != NULL);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(demo_edit_burn_save);
    LT_RUN(tabs_and_dialogs);
    LT_RUN(scale_and_geometry_settings);
    LT_RUN(ext_registry);
    LT_TEST_MAIN_END
}
