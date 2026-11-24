/* test_prefs.c - preferences, session restore, project connection settings, assisted INI search,
 * TunerStudio import preview, tune files, error report, settings search
 * (G-05, G-06, G-17, G-18, G-19, G-92, G-93, G-94, G-96, G-102, G-104).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/app/dialogs.h"
#include "../src/app/prefs.h"
#include "../src/app/online_logic.h"
#include "../src/app/files_logic.h"
#include "../src/plat/plat.h"

static void unique_name(char *out, size_t cap, const char *prefix)
{
    char ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(out, cap, "%s%s_%lu", prefix, ts, (unsigned long)lt_plat_now_ms());
}

/* config and projects of the test app live under build/ */
static void sandbox(const char *tag)
{
    char cfg[128], proj[128];
    unique_name(cfg, sizeof(cfg), "build/test_prefs_cfg_");
    unique_name(proj, sizeof(proj), "build/test_prefs_proj_");
    lt_snprintf(cfg + strlen(cfg), sizeof(cfg) - strlen(cfg), "%s", tag);
    lt_snprintf(proj + strlen(proj), sizeof(proj) - strlen(proj), "%s", tag);
    (void)lt_plat_mkdirs(cfg);
    (void)lt_plat_mkdirs(proj);
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", cfg) == 0);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_PROJECTS", proj) == 0);
}

/* ---- prefs ---- */
LT_TEST(prefs_roundtrip)
{
    lt_prefs a, b;
    lt_props pr;
    char *txt;
    lt_u32 v = 0;
    lt_prefs_defaults(&a);
    LT_CHECK(a.y_bottom == 1 && a.restore_session == 1 && a.trail_fade_sec == 8 && a.sb_n == 0);
    a.packet_mode = LT_PKT_OCH;
    a.adaptive_timing = 1;
    a.auto_burn_close = 1;
    a.change_alert = 1;
    a.change_abs = 2.5;
    a.y_bottom = 0;
    lt_strlcpy(a.cursor_color, "#ff8000", sizeof(a.cursor_color));
    lt_strlcpy(a.heat_scheme[1], "custom", sizeof(a.heat_scheme[1]));
    a.heat_stops[1][0] = 0x112233UL; a.heat_stops[1][1] = 0xAABBCCUL; a.heat_n[1] = 2;
    LT_CHECK(lt_prefs_sb_toggle(&a, "RPM") == 1 && lt_prefs_sb_toggle(&a, "MAP") == 1);
    LT_CHECK(lt_prefs_session_add(&a, "table", "veTable1") == 0);
    LT_CHECK(lt_prefs_session_add(&a, "dialog", "injChars") == 0);
    a.last_active = 1;
    lt_strlcpy(a.session_project, "/x/y", sizeof(a.session_project));
    lt_props_init(&pr);
    lt_prefs_to_props(&a, &pr);
    txt = lt_props_to_string(&pr);
    lt_props_free(&pr);
    lt_props_init(&pr);
    LT_CHECK(lt_props_parse(&pr, txt) == 0);
    lt_free(txt);
    lt_prefs_defaults(&b);
    lt_prefs_from_props(&b, &pr);
    lt_props_free(&pr);
    LT_CHECK(b.packet_mode == LT_PKT_OCH && b.adaptive_timing == 1 && b.auto_burn_close == 1);
    LT_CHECK(b.change_alert == 1 && b.y_bottom == 0);
    LT_CHECK_NEAR(b.change_abs, 2.5, 1e-9);
    LT_CHECK_STR(b.cursor_color, "#ff8000");
    LT_CHECK_STR(b.heat_scheme[1], "custom");
    LT_CHECK(b.heat_n[1] == 2 && b.heat_stops[1][0] == 0x112233UL && b.heat_stops[1][1] == 0xAABBCCUL);
    LT_CHECK(b.sb_n == 2 && strcmp(b.sb_chan[0], "RPM") == 0 && strcmp(b.sb_chan[1], "MAP") == 0);
    LT_CHECK_STR(b.open_tabs, a.open_tabs);
    LT_CHECK(b.last_active == 1);
    LT_CHECK_STR(b.session_project, "/x/y");
    LT_CHECK(lt_prefs_parse_color(b.cursor_color, &v) && v == 0xFF8000UL);
}

LT_TEST(prefs_file)
{
    lt_prefs a, b;
    char dir[128];
    unique_name(dir, sizeof(dir), "build/test_prefs_file_");
    (void)lt_plat_mkdirs(dir);
    lt_prefs_defaults(&a);
    LT_CHECK(lt_prefs_load(&b, dir) < 0);                   /* no file: defaults */
    LT_CHECK(b.y_bottom == 1);
    LT_CHECK(a.lambda_pane == 1);                           /* lambda preview open by default */
    a.help_icons = 0;
    a.trail_fade_sec = 30;
    a.lambda_pane = 0;
    LT_CHECK(lt_prefs_save(&a, dir) == 0);
    LT_CHECK(lt_prefs_load(&b, dir) == 0);
    LT_CHECK(b.help_icons == 0 && b.trail_fade_sec == 30);
    LT_CHECK(b.lambda_pane == 0);
}

LT_TEST(prefs_colors_heat)
{
    lt_prefs p;
    lt_u32 v = 0, c = 0;
    int scheme = -1;
    char hex[8];
    LT_CHECK(!lt_prefs_parse_color("ff8000", &v));
    LT_CHECK(!lt_prefs_parse_color("#ff80", &v));
    LT_CHECK(!lt_prefs_parse_color("#gg8000", &v));
    LT_CHECK(lt_prefs_parse_color("#0A0b0C", &v) && v == 0x0A0B0CUL);
    lt_prefs_format_color(0x0A0B0CUL, hex);
    LT_CHECK_STR(hex, "#0a0b0c");
    lt_prefs_defaults(&p);
    LT_CHECK(!lt_prefs_heat(&p, 0, 0.5, &c, &scheme) && scheme == 0);
    lt_strlcpy(p.heat_scheme[0], "viridis", sizeof(p.heat_scheme[0]));
    LT_CHECK(!lt_prefs_heat(&p, 0, 0.5, &c, &scheme) && scheme == 4);
    lt_strlcpy(p.heat_scheme[2], "custom", sizeof(p.heat_scheme[2]));
    p.heat_n[2] = 1;                                         /* too few stops: not custom */
    LT_CHECK(!lt_prefs_heat(&p, 2, 0.5, &c, &scheme));
    p.heat_n[2] = 3;
    p.heat_stops[2][0] = 0x000000UL; p.heat_stops[2][1] = 0xFF0000UL; p.heat_stops[2][2] = 0x00FF00UL;
    LT_CHECK(lt_prefs_heat(&p, 2, 0.0, &c, &scheme) && c == 0x000000UL);
    LT_CHECK(lt_prefs_heat(&p, 2, 0.5, &c, &scheme) && c == 0xFF0000UL);
    LT_CHECK(lt_prefs_heat(&p, 2, 1.0, &c, &scheme) && c == 0x00FF00UL);
    LT_CHECK(lt_prefs_heat(&p, 2, 0.25, &c, &scheme) && c == 0x7F0000UL);
    LT_CHECK(lt_prefs_heat(&p, 2, 7.0, &c, &scheme) && c == 0x00FF00UL);   /* clamped */
}

LT_TEST(prefs_change_alert)
{
    lt_prefs p;
    lt_prefs_defaults(&p);
    LT_CHECK(!lt_prefs_change_exceeds(&p, 50.0, 90.0));      /* alert off */
    p.change_alert = 1;
    LT_CHECK(lt_prefs_change_exceeds(&p, 50.0, 56.0));       /* 6 > 5 absolute */
    LT_CHECK(!lt_prefs_change_exceeds(&p, 100.0, 104.0));    /* 4 and 4 % */
    p.change_abs = 100.0;
    LT_CHECK(lt_prefs_change_exceeds(&p, 20.0, 23.0));       /* 15 % > 10 % */
    LT_CHECK(!lt_prefs_change_exceeds(&p, 0.0, 3.0));        /* zero old: percent unused */
    p.change_pct = 0.0;
    LT_CHECK(!lt_prefs_change_exceeds(&p, 20.0, 23.0));
}

LT_TEST(prefs_statusbar)
{
    lt_prefs p;
    char *names[4];
    int i;
    lt_prefs_defaults(&p);
    for (i = 0; i < LT_PREFS_SB_MAX; i++) {
        char n[16];
        lt_snprintf(n, sizeof(n), "ch%d", i);
        LT_CHECK(lt_prefs_sb_toggle(&p, n) == 1);
    }
    LT_CHECK(lt_prefs_sb_toggle(&p, "extra") == -1);        /* full */
    LT_CHECK(lt_prefs_sb_toggle(&p, "ch3") == 0);            /* removal keeps the order */
    LT_CHECK(p.sb_n == 7 && strcmp(p.sb_chan[3], "ch4") == 0 && !lt_prefs_sb_has(&p, "ch3"));
    LT_CHECK(lt_prefs_sb_toggle(&p, "extra") == 1 && p.sb_n == 8);
    lt_prefs_defaults(&p);
    names[0] = (char *)"foo"; names[1] = (char *)"MAP"; names[2] = (char *)"RPM"; names[3] = (char *)"TPS";
    lt_feat_statusbar_defaults(&p, names, 4);
    LT_CHECK(p.sb_n == 3 && strcmp(p.sb_chan[0], "RPM") == 0 && strcmp(p.sb_chan[1], "MAP") == 0 &&
             strcmp(p.sb_chan[2], "TPS") == 0);
    lt_prefs_defaults(&p);
    names[0] = (char *)"a"; names[1] = (char *)"b";
    lt_feat_statusbar_defaults(&p, names, 2);                /* none of the usual: the first ones */
    LT_CHECK(p.sb_n == 1 && strcmp(p.sb_chan[0], "a") == 0);
}

LT_TEST(prefs_session_list)
{
    lt_prefs p;
    char k[24], t[64];
    int i;
    lt_prefs_defaults(&p);
    LT_CHECK(!lt_prefs_session_get(&p, 0, k, sizeof(k), t, sizeof(t)));
    LT_CHECK(lt_prefs_session_add(&p, "table", "veTable1") == 0);
    LT_CHECK(lt_prefs_session_add(&p, "curve", "warmup") == 0);
    LT_CHECK(lt_prefs_session_add(&p, "table", "bad|name") == -1);
    LT_CHECK(lt_prefs_session_get(&p, 1, k, sizeof(k), t, sizeof(t)) && strcmp(k, "curve") == 0 &&
             strcmp(t, "warmup") == 0);
    LT_CHECK(!lt_prefs_session_get(&p, 2, k, sizeof(k), t, sizeof(t)));
    for (i = 0; i < 200; i++) { (void)lt_prefs_session_add(&p, "table", "a_rather_long_table_name"); }
    LT_CHECK(strlen(p.open_tabs) < sizeof(p.open_tabs));       /* bounded */
    LT_CHECK(lt_prefs_session_add(&p, "table", "a_rather_long_table_name") == -1);
}

/* ---- online INI search ---- */
LT_TEST(online_sources)
{
    LT_CHECK(lt_ionline_n() == 4);
    LT_CHECK(lt_ionline_pick("speeduino 202305") == 0);
    LT_CHECK(lt_ionline_pick("FOME master.2025") == 1);
    LT_CHECK(lt_ionline_pick("rusEFI 2025.06") == 2);
    LT_CHECK(lt_ionline_pick("epicEFI 1") == 3);
    LT_CHECK(lt_ionline_pick("MS3 1.5") == -1 && lt_ionline_pick("") == -1 && lt_ionline_pick(NULL) == -1);
    LT_CHECK(strncmp(lt_ionline_get(0)->url, "https://", 8) == 0);
    LT_CHECK(lt_ionline_get(9) == NULL);
}

static void put(const char *dir, const char *name, const char *text)
{
    char *p = lt_path_join(dir, name);
    LT_CHECK(lt_file_write(p, text, strlen(text)) == 0);
    lt_free(p);
}

LT_TEST(online_watch)
{
    lt_ionline_watch w;
    char dir[128], **paths = NULL, *d;
    size_t n, i;
    static const char ini_a[] = "[MegaTune]\n   signature = \"speeduino 202305\"\n";
    static const char ini_b[] = "[MegaTune]\n   signature = \"rusEFI 2025\"\n";
    unique_name(dir, sizeof(dir), "build/test_online_");
    (void)lt_plat_mkdirs(dir);
    put(dir, "old.ini", ini_a);
    lt_ionline_watch_init(&w);
    LT_CHECK(lt_ionline_watch_start(&w, "build/no_such_download_dir") < 0);
    LT_CHECK(lt_ionline_watch_start(&w, dir) == 0);
    n = lt_ionline_watch_poll(&w, &paths);
    LT_CHECK(n == 0 && paths == NULL);                        /* pre-existing files are ignored */
    put(dir, "new.INI", ini_b);
    put(dir, "notes.txt", "hello");
    put(dir, "partial.ini", "");                              /* download in progress */
    put(dir, "other.ini", "[Constants]\nfoo = 1\n");           /* no signature: not an ECU INI */
    n = lt_ionline_watch_poll(&w, &paths);
    LT_CHECK(n == 1);
    if (n == 1) { LT_CHECK(strcmp(lt_path_base(paths[0]), "new.INI") == 0); }
    for (i = 0; i < n; i++) { lt_free(paths[i]); }
    lt_free(paths);
    paths = NULL;
    LT_CHECK(lt_ionline_watch_poll(&w, &paths) == 0);         /* reported once */
    put(dir, "partial.ini", ini_a);                           /* the download finished */
    n = lt_ionline_watch_poll(&w, &paths);
    LT_CHECK(n == 1);
    for (i = 0; i < n; i++) { lt_free(paths[i]); }
    lt_free(paths);
    paths = NULL;
    n = lt_ionline_list(dir, &paths);                         /* folder scan: every ECU INI */
    LT_CHECK(n == 3);
    for (i = 0; i < n; i++) { lt_free(paths[i]); }
    lt_free(paths);
    lt_ionline_watch_free(&w);
    LT_CHECK(lt_plat_setenv("LT_DOWNLOADS_DIR", dir) == 0);
    d = lt_ionline_downloads_dir();
    LT_CHECK(d != NULL && strcmp(d, dir) == 0);
    lt_free(d);
    LT_CHECK(lt_plat_setenv("LT_DOWNLOADS_DIR", "") == 0);
}

/* ---- files ---- */
LT_TEST(files_ts_preview)
{
    lt_ts_preview pv;
    char root[512];
    LT_CHECK(lt_ts_preview_read("tests/fixtures/project/ts_proj", &pv) == 0);
    LT_CHECK_STR(pv.name, "MS3-Example_Project");
    LT_CHECK_STR(pv.ini_file, "mainController.ini");
    LT_CHECK(pv.ini_found && pv.has_tune && pv.has_pcv && pv.n_restore == 1 && pv.n_dash == 0);
    LT_CHECK_STR(pv.port, "COM8");
    LT_CHECK(pv.baud == 115200);
    LT_CHECK(pv.signature[0] != '\0');
    LT_CHECK(lt_ts_preview_read("tests/fixtures/project", &pv) < 0);          /* not a TS project */
    LT_CHECK(strstr(lt_err_get(), "Not a valid TS project") != NULL);
    lt_feat_import_ts_root("tests/fixtures/project/ts_proj", root, sizeof(root));
    LT_CHECK_STR(root, "tests/fixtures/project/ts_proj");
    lt_feat_import_ts_root("tests/fixtures/project/ts_proj/CurrentTune.msq", root, sizeof(root));
    LT_CHECK_STR(root, "tests/fixtures/project/ts_proj");
    lt_feat_import_ts_root("tests/fixtures/project/ts_proj/projectCfg/project.properties", root,
                           sizeof(root));
    LT_CHECK_STR(root, "tests/fixtures/project/ts_proj");
    lt_feat_import_ts_root("tests/fixtures/project/ts_proj/projectCfg", root, sizeof(root));
    LT_CHECK_STR(root, "tests/fixtures/project/ts_proj");
}

LT_TEST(files_tunes)
{
    char **names = NULL, dir[128], out[128];
    size_t n = 0, i;
    lt_tuneinfo ti;
    LT_CHECK(lt_tunes_list("tests/fixtures/tune", &names, &n) == 0 && n == 1);
    if (n == 1) { LT_CHECK_STR(names[0], "sample.msq"); }
    for (i = 0; i < n; i++) { lt_free(names[i]); }
    lt_free(names);
    LT_CHECK(lt_tunes_list("build/no_such_tune_dir", &names, &n) < 0);
    LT_CHECK(lt_tuneinfo_read("tests/fixtures/tune/sample.msq", &ti) == 0);
    LT_CHECK_STR(ti.file, "sample.msq");
    LT_CHECK(ti.size > 100 && ti.constants > 0);
    LT_CHECK(lt_tuneinfo_read("tests/fixtures/tune/mini_speeduino.ini", &ti) < 0);
    unique_name(dir, sizeof(dir), "build/test_tunenames_");
    (void)lt_plat_mkdirs(dir);
    lt_tunes_new_name(dir, "My Tune", out, sizeof(out));
    LT_CHECK_STR(out, "My Tune.msq");
    put(dir, "My Tune.msq", "x");
    lt_tunes_new_name(dir, "My Tune", out, sizeof(out));
    LT_CHECK_STR(out, "My Tune 2.msq");
    lt_tunes_new_name(dir, "a/b:c*", out, sizeof(out));
    LT_CHECK_STR(out, "a_b_c_.msq");
    lt_tunes_new_name(dir, "..", out, sizeof(out));
    LT_CHECK_STR(out, "NewTune.msq");
}

/* ---- app level ---- */
static int open_demo_project(lt_app *app, const char *prefix)
{
    char ini[512], name[64];
    if (lt_app_find_demo_ini(ini, sizeof(ini)) == NULL) { return -1; }
    unique_name(name, sizeof(name), prefix);
    return lt_app_new_project(app, name, ini);
}

LT_TEST(app_registry)
{
    lt_app *app;
    sandbox("reg");
    app = lt_app_new();
    LT_CHECK(app->settings.prefs.y_bottom == 1 && app->settings.prefs.restore_session == 1);
    LT_CHECK(lt_ext_n_settings_pages(app) >= 4);
    LT_CHECK(lt_ext_n_menu_items(app, LT_EXT_MENU_FILE) >= 3);
    LT_CHECK(lt_ext_n_menu_items(app, LT_EXT_MENU_TOOLS) >= 1);
    lt_app_free(app);
}

LT_TEST(app_project_connection)
{
    lt_app *app;
    char dir[512];
    sandbox("conn");
    app = lt_app_new();
    LT_CHECK(open_demo_project(app, "conn") == 0);
    lt_strlcpy(dir, app->project.path, sizeof(dir));
    LT_CHECK(app->project.cfg.conn.port == NULL || app->project.cfg.conn.port[0] == '\0');
    lt_ext_tick(app);                                            /* snapshot */
    lt_strlcpy(app->settings.port, "COM_TEST", sizeof(app->settings.port));
    app->settings.baud = 57600;
    app->settings.auto_connect = 1;
    lt_ext_tick(app);                                            /* written to the project */
    LT_CHECK(app->project.cfg.conn.port != NULL && strcmp(app->project.cfg.conn.port, "COM_TEST") == 0);
    LT_CHECK(app->project.cfg.conn.baud_rate == 57600 && app->project.cfg.settings.auto_connect);
    /* another global value, then the project opens again: its own values win */
    lt_strlcpy(app->settings.port, "OTHER", sizeof(app->settings.port));
    app->settings.baud = 9600;
    app->settings.auto_connect = 0;
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK_STR(app->settings.port, "COM_TEST");
    LT_CHECK(app->settings.baud == 57600 && app->settings.auto_connect == 1);
    lt_app_free(app);
}

LT_TEST(app_session_restore)
{
    lt_app *app;
    char dir[512], tname[96];
    const lt_ini_table *tb;
    sandbox("sess");
    app = lt_app_new();
    app->headless = 0;
    LT_CHECK(open_demo_project(app, "sess") == 0);
    LT_CHECK(app->ini != NULL && app->ini->n_tables > 0);
    if (app->ini == NULL || app->ini->n_tables == 0) { lt_app_free(app); return; }
    tb = app->ini->tables[0];
    lt_strlcpy(tname, tb->name, sizeof(tname));
    lt_strlcpy(dir, app->project.path, sizeof(dir));
    lt_ext_tick(app);                                            /* project seen */
    LT_CHECK(lt_app_open_target(app, tname, NULL) == 0);
    LT_CHECK(lt_app_tab_find(app, "table", tname) != NULL);
    LT_CHECK(lt_feat_prefs_session_record(app) == 1);
    LT_CHECK(strstr(app->settings.prefs.open_tabs, tname) != NULL);
    LT_CHECK(lt_feat_prefs_session_record(app) == 0);            /* unchanged */
    LT_CHECK(lt_app_open_project(app, dir) == 0);                /* reopen: only the dashboard tab */
    LT_CHECK(lt_app_tab_find(app, "table", tname) == NULL);
    lt_ext_tick(app);                                            /* restored */
    LT_CHECK(lt_app_tab_find(app, "table", tname) != NULL);
    {
        lt_app_tab *a = lt_app_tab_active(app);
        LT_CHECK(a != NULL && strcmp(a->target, tname) == 0);
    }
    /* switched off: nothing comes back */
    app->settings.prefs.restore_session = 0;
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    lt_ext_tick(app);
    LT_CHECK(lt_app_tab_find(app, "table", tname) == NULL);
    lt_app_free(app);
}

LT_TEST(app_prefs_persist)
{
    lt_app *app;
    lt_app *app2;
    sandbox("persist");
    app = lt_app_new();
    app->settings.prefs.auto_burn_close = 1;
    lt_strlcpy(app->settings.prefs.trail_color, "#102030", sizeof(app->settings.prefs.trail_color));
    LT_CHECK(lt_prefs_sb_toggle(&app->settings.prefs, "RPM") == 1);
    lt_app_settings_save(app);
    app2 = lt_app_new();
    LT_CHECK(app2->settings.prefs.auto_burn_close == 1);
    LT_CHECK_STR(app2->settings.prefs.trail_color, "#102030");
    LT_CHECK(app2->settings.prefs.sb_n == 1);
    {
        lt_color c = 0;
        LT_CHECK(lt_feat_prefs_color(app2, 1, &c) && c == LT_RGB(0x10, 0x20, 0x30));
        LT_CHECK(!lt_feat_prefs_color(app2, 0, &c));
    }
    lt_app_free(app2);
    lt_app_free(app);
}

LT_TEST(app_import_ts)
{
    lt_app *app;
    char proj[128];
    sandbox("imp");
    app = lt_app_new();
    LT_CHECK(lt_feat_import_ts_with_ini(app, "tests/fixtures/project/ts_proj", NULL) == 0);
    LT_CHECK(app->has_project && app->project.cfg.name != NULL);
    LT_CHECK(app->project.cfg.conn.port != NULL && strcmp(app->project.cfg.conn.port, "COM8") == 0);
    {
        lt_restore_info *v = NULL;
        size_t n = 0;
        LT_CHECK(lt_restore_list(&app->project, &v, &n) == 0 && n == 1);
        lt_restore_info_free(v, n);
    }
    /* a missing INI replaced by another definition (new projects folder: the name is taken) */
    unique_name(proj, sizeof(proj), "build/test_prefs_proj2_");
    (void)lt_plat_mkdirs(proj);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_PROJECTS", proj) == 0);
    LT_CHECK(lt_feat_import_ts_with_ini(app, "tests/fixtures/project/ts_proj",
                                        "tests/fixtures/ini/demo.ini") == 0);
    LT_CHECK(app->has_project && app->ini != NULL);
    LT_CHECK(app->project.cfg.conn.port != NULL && strcmp(app->project.cfg.conn.port, "COM8") == 0);
    {
        lt_restore_info *v = NULL;
        size_t n = 0;
        LT_CHECK(lt_restore_list(&app->project, &v, &n) == 0 && n == 1);
        lt_restore_info_free(v, n);
    }
    LT_CHECK(lt_feat_import_ts_with_ini(app, "tests/fixtures/project", NULL) < 0);
    lt_app_free(app);
}

/* a TunerStudio project whose INI is missing: the wizard offers the definition bundled with
 * LibreTune for the tune's signature, and Import Project (Enter) uses it */
LT_TEST(app_import_ts_bundled_ini)
{
    lt_app *app;
    char root[160], cfg[200], defs[160], *dst;
    sandbox("impb");
    unique_name(defs, sizeof(defs), "build/test_prefs_defs_");
    (void)lt_plat_mkdirs(defs);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_DEFINITIONS", defs) == 0);     /* an empty INI library */
    unique_name(root, sizeof(root), "build/test_prefs_tsnoini_");
    lt_snprintf(cfg, sizeof(cfg), "%s/projectCfg", root);
    (void)lt_plat_mkdirs(cfg);
    dst = lt_path_join(root, "CurrentTune.msq");
    LT_CHECK(lt_plat_copy_file("tests/fixtures/project/ts_proj/CurrentTune.msq", dst) == 0);
    lt_free(dst);
    dst = lt_path_join(cfg, "project.properties");
    LT_CHECK(lt_plat_copy_file("tests/fixtures/project/ts_proj/projectCfg/project.properties", dst) == 0);
    lt_free(dst);
    app = lt_app_new();
    LT_CHECK(lt_app_dialog_push(app, "ts_import", root, "Import TS Project") != NULL);
    lt_plat_setenv("LT_SHOT_TEXT", "build/test_prefs_tsnoini.txt");
    lt_plat_setenv("LT_SHOT_KEYS", "enter");
    (void)lt_app_screenshot(app, "", 120, 40, 3);
    lt_plat_setenv("LT_SHOT_TEXT", "");
    lt_plat_setenv("LT_SHOT_KEYS", "");
    (void)lt_app_service(app);                     /* the import runs from the tick */
    LT_CHECK(app->has_project && app->ini != NULL && app->ini->signature != NULL);
    if (app->ini != NULL && app->ini->signature != NULL) {
        LT_CHECK_STR(app->ini->signature, "speeduino 202501");
    }
    LT_CHECK(lt_plat_setenv("LIBRETUNE_DEFINITIONS", "") == 0);
    lt_app_free(app);
}

LT_TEST(app_new_tune)
{
    lt_app *app;
    char f1[128], f2[128], *p;
    sandbox("newtune");
    app = lt_app_new();
    LT_CHECK(lt_feat_tune_new(app, "x", f1, sizeof(f1)) < 0);      /* no project */
    LT_CHECK(open_demo_project(app, "nt") == 0);
    LT_CHECK(lt_feat_tune_new(app, "My Tune", f1, sizeof(f1)) == 0);
    LT_CHECK_STR(f1, "My Tune.msq");
    p = lt_path_join(app->project.path, f1);
    LT_CHECK(lt_file_exists(p));
    lt_free(p);
    LT_CHECK(app->modified);
    LT_CHECK(lt_feat_tune_new(app, "My Tune", f2, sizeof(f2)) == 0);
    LT_CHECK_STR(f2, "My Tune 2.msq");
    lt_app_free(app);
}

LT_TEST(app_settings_search)
{
    LT_CHECK(lt_dlg_settings_match("Connection", ""));
    LT_CHECK(lt_dlg_settings_match("Connection", "conn"));
    LT_CHECK(lt_dlg_settings_match("Connection", "BAUD"));
    LT_CHECK(!lt_dlg_settings_match("General", "baud"));
    LT_CHECK(lt_dlg_settings_match("Table Display", "heat"));
    LT_CHECK(lt_dlg_settings_match("Behavior", "burn"));
    LT_CHECK(lt_dlg_settings_match("Status Bar", "channels"));
    LT_CHECK(!lt_dlg_settings_match("Status Bar", "zzzz"));
    LT_CHECK(lt_dlg_settings_match("Some Extension Page", "extension"));
}

LT_TEST(error_report)
{
    lt_proto_err e;
    char d[512], rep[1024];
    memset(&e, 0, sizeof(e));
    e.code = LT_PE_CRC_MISMATCH;
    lt_strlcpy(e.msg, "bad CRC in page read", sizeof(e.msg));
    e.a = 0x1234ABCDUL; e.b = 0x55UL;
    lt_feat_error_describe(&e, d, sizeof(d));
    LT_CHECK(strstr(d, "CRC mismatch") != NULL && strstr(d, "0x1234ABCD") != NULL &&
             strstr(d, "0x00000055") != NULL && strstr(d, "bad CRC in page read") != NULL);
    e.code = LT_PE_WRITE_VERIFY_FAILED; e.page = 2; e.offset = 17; e.expected = 5; e.actual = 9;
    lt_feat_error_describe(&e, d, sizeof(d));
    LT_CHECK(strstr(d, "Page 2 offset 17") != NULL);
    LT_CHECK(strcmp(lt_feat_error_code_name(LT_PE_TIMEOUT), "timeout") == 0);
    LT_CHECK(strcmp(lt_feat_error_code_name(9999), "none") == 0);
    lt_feat_error_report("Connection Failed", "no answer", d, rep, sizeof(rep));
    LT_CHECK(strstr(rep, "LibreTune Error Report") != NULL && strstr(rep, "Title: Connection Failed") != NULL &&
             strstr(rep, "Message: no answer") != NULL && strstr(rep, "Details:") != NULL);
    lt_feat_error_report("T", "M", "", rep, sizeof(rep));
    LT_CHECK(strstr(rep, "Details:") == NULL);
}

LT_TEST(error_dialog_push)
{
    lt_app *app;
    sandbox("err");
    app = lt_app_new();
    lt_feat_error_show(app, "Oops", "something failed", "line1\nline2");
    LT_CHECK(app->n_dialogs == 1 && strcmp(app->dialogs[0]->kind, "error_details") == 0);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(prefs_roundtrip);
    LT_RUN(prefs_file);
    LT_RUN(prefs_colors_heat);
    LT_RUN(prefs_change_alert);
    LT_RUN(prefs_statusbar);
    LT_RUN(prefs_session_list);
    LT_RUN(online_sources);
    LT_RUN(online_watch);
    LT_RUN(files_ts_preview);
    LT_RUN(files_tunes);
    LT_RUN(app_registry);
    LT_RUN(app_project_connection);
    LT_RUN(app_session_restore);
    LT_RUN(app_prefs_persist);
    LT_RUN(app_import_ts);
    LT_RUN(app_import_ts_bundled_ini);
    LT_RUN(app_new_tune);
    LT_RUN(app_settings_search);
    LT_RUN(error_report);
    LT_RUN(error_dialog_push);
    LT_TEST_MAIN_END
}
