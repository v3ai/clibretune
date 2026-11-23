/* test_tools.c - tools batch: table comparison (G-80), dyno overlay (G-81), action recorder (G-82),
 * 3D view model (G-26), lambda preview (G-27), large-change alert (G-28), performance calculator (G-35).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/app/feat_log.h"
#include "../src/app/tables_priv.h"
#include "../src/app/tables_3d.h"
#include "../src/app/dlg_perf.h"
#include "../src/app/actions_logic.h"
#include "../src/tableops/tablecmp.h"

static void frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static void press(lt_app *app, int key, int mods)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_KEY_DOWN; ev.key = key; ev.mods = mods;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_KEY_UP;
    lt_ui_input(app->ui, &ev);
    frame(app);
}

static lt_app *demo_app(void)
{
    char buf[512];
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    return app;
}

static double cell0(lt_app *app, const char *tname, size_t row, size_t col)
{
    lt_pagesrc ps = lt_app_pages(app);
    lt_tblview v;
    double r;
    const lt_ini_table *t = lt_ini_find_table(app->ini, tname);
    lt_tblview_init(&v);
    if (t == NULL || lt_tblview_load_table(&v, app->ini, t, &ps) < 0) { return -1e9; }
    r = lt_tblview_get(&v, row, col);
    lt_tblview_free(&v);
    return r;
}

/* ---- G-80 ---- */
LT_TEST(tablecmp_compute)
{
    double a[6], b[6];
    lt_tblcmp c;
    size_t i;
    for (i = 0; i < 6; i++) { a[i] = 10.0 + (double)i; b[i] = a[i]; }
    LT_CHECK(lt_tblcmp_compute(a, b, 2, 3, &c) == 0);
    LT_CHECK(c.n == 0 && c.avg_diff == 0.0 && c.max_diff == 0.0);
    LT_CHECK_NEAR(lt_tblcmp_heat(&c, LT_TBC_VIEW_DIFF, 0, 0), 0.5, 1e-12);
    lt_tblcmp_free(&c);
    b[1] += 2.0;        /* 11 -> 13: +18.2 % */
    b[4] -= 0.8;        /* 14 -> 13.2: -5.7 % */
    b[5] += 0.00005;    /* below the threshold */
    LT_CHECK(lt_tblcmp_compute(a, b, 2, 3, &c) == 0);
    LT_CHECK(c.n == 2);
    LT_CHECK(c.cells[0].row == 0 && c.cells[0].col == 1);
    LT_CHECK(c.cells[1].row == 1 && c.cells[1].col == 1);
    LT_CHECK_NEAR(c.cells[0].diff, 2.0, 1e-12);
    LT_CHECK_NEAR(c.cells[0].pct, 2.0 / 11.0 * 100.0, 1e-9);
    LT_CHECK_NEAR(c.max_diff, 2.0, 1e-12);
    LT_CHECK_NEAR(c.avg_diff, 1.4, 1e-9);
    LT_CHECK(c.n_high == 1 && c.n_med == 1 && c.n_low == 0);
    LT_CHECK_NEAR(lt_tblcmp_value(&c, LT_TBC_VIEW_DIFF, 0, 1), 2.0, 1e-12);
    LT_CHECK_NEAR(lt_tblcmp_value(&c, LT_TBC_VIEW_B, 0, 1), 13.0, 1e-12);
    LT_CHECK_NEAR(lt_tblcmp_value(&c, LT_TBC_VIEW_PCT, 0, 0), 0.0, 1e-12);
    LT_CHECK(lt_tblcmp_heat(&c, LT_TBC_VIEW_DIFF, 0, 1) > 0.99);
    LT_CHECK(lt_tblcmp_heat(&c, LT_TBC_VIEW_DIFF, 1, 1) < 0.5);
    LT_CHECK_NEAR(lt_tblcmp_heat(&c, LT_TBC_VIEW_B, 0, 0), 0.0, 1e-12);
    lt_tblcmp_free(&c);
    LT_CHECK(lt_tblcmp_compute(a, b, 0, 3, &c) < 0);
}

LT_TEST(tablecmp_percent_rules)
{
    LT_CHECK_NEAR(lt_tbc_percent(0.0, 5.0), 100.0, 1e-12);
    LT_CHECK_NEAR(lt_tbc_percent(0.0, 0.00001), 0.0, 1e-12);
    LT_CHECK_NEAR(lt_tbc_percent(-10.0, -11.0), 10.0, 1e-9);
    LT_CHECK(lt_tbc_severity(10.0) == LT_TBC_HIGH);
    LT_CHECK(lt_tbc_severity(-5.0) == LT_TBC_MED);
    LT_CHECK(lt_tbc_severity(4.99) == LT_TBC_LOW);
    LT_CHECK(lt_tbc_severity(0.0) == LT_TBC_NONE);
}

LT_TEST(tablecmp_dialog_runs)
{
    lt_app *app = demo_app();
    LT_CHECK(lt_ext_n_menu_items(app, LT_EXT_MENU_TOOLS) > 0);
    LT_CHECK(lt_app_dialog_push(app, "tablecmp", "", "Table Comparison") != NULL);
    lt_app_screenshot(app, "build/lt_test_tablecmp.ppm", 1100, 800, 3);
    lt_app_free(app);
}

/* ---- G-26 ---- */
LT_TEST(t3d_camera_limits)
{
    lt_t3d t;
    int i;
    lt_t3d_init(&t);
    LT_CHECK(t.style == LT_T3D_SOLID && t.color_mode == LT_T3D_BY_VALUE && t.grid == 1);
    for (i = 0; i < 100; i++) { lt_t3d_zoom(&t, 0.5); }
    LT_CHECK_NEAR(t.zoom, 0.25, 1e-12);
    for (i = 0; i < 100; i++) { lt_t3d_zoom(&t, 2.0); }
    LT_CHECK_NEAR(t.zoom, 4.0, 1e-12);
    lt_t3d_zoom(&t, -1.0);
    LT_CHECK_NEAR(t.zoom, 4.0, 1e-12);
    lt_t3d_wheel(&t, 3);
    LT_CHECK(t.zoom < 4.0 && t.zoom > 2.8);
    lt_t3d_orbit(&t, 0.0, 10.0);
    LT_CHECK_NEAR(t.pitch, 1.45, 1e-12);
    lt_t3d_orbit(&t, 0.0, -10.0);
    LT_CHECK_NEAR(t.pitch, 0.08, 1e-12);
    lt_t3d_pan(&t, 100000.0, -100000.0, 400);
    LT_CHECK(fabs(t.panx) <= 12.0 && fabs(t.pany) <= 12.0 && t.panx < 0.0 && t.pany < 0.0);
    t.style = LT_T3D_WIRE;
    lt_t3d_reset_view(&t);
    LT_CHECK_NEAR(t.zoom, 1.0, 1e-12);
    LT_CHECK(t.panx == 0.0 && t.pany == 0.0 && t.style == LT_T3D_WIRE);
}

LT_TEST(t3d_pick_and_pan)
{
    lt_t3d t;
    lt_cam3 cam, cam2;
    double z[12], wx, wy, wz, sx, sy;
    int r = -1, c = -1, i, hit;
    for (i = 0; i < 12; i++) { z[i] = (double)(i % 4) * 2.0 + (double)(i / 4); }
    lt_t3d_init(&t);
    lt_cam3_setup(&t, 0, 0, 600, 400, &cam);
    for (i = 0; i < 12; i++) {
        int rr = i / 4, cc = i % 4;
        lt_t3d_world(3, 4, 0, 0.0, 8.0, rr, cc, z[i], &wx, &wy, &wz);
        (void)lt_cam3_proj(&cam, wx, wy, wz, &sx, &sy);
        hit = lt_t3d_pick(&cam, 3, 4, 0, z, 0.0, 8.0, (int)(sx + 0.5), (int)(sy + 0.5), 12.0, &r, &c);
        LT_CHECK(hit == 1 && r == rr && c == cc);
    }
    LT_CHECK(lt_t3d_pick(&cam, 3, 4, 0, z, 0.0, 8.0, -500, -500, 20.0, &r, &c) == 0);
    /* flipped Y axis maps the first row to the other side but still picks itself */
    lt_t3d_world(3, 4, 1, 0.0, 8.0, 0, 2, z[2], &wx, &wy, &wz);
    (void)lt_cam3_proj(&cam, wx, wy, wz, &sx, &sy);
    LT_CHECK(lt_t3d_pick(&cam, 3, 4, 1, z, 0.0, 8.0, (int)(sx + 0.5), (int)(sy + 0.5), 12.0, &r, &c) == 1);
    LT_CHECK(r == 0 && c == 2);
    /* panning moves the scene on screen */
    lt_t3d_pan(&t, 40.0, 0.0, 400);
    lt_cam3_setup(&t, 0, 0, 600, 400, &cam2);
    {
        double sx2, sy2;
        lt_t3d_world(3, 4, 0, 0.0, 8.0, 1, 1, z[5], &wx, &wy, &wz);
        (void)lt_cam3_proj(&cam, wx, wy, wz, &sx, &sy);
        (void)lt_cam3_proj(&cam2, wx, wy, wz, &sx2, &sy2);
        LT_CHECK(sx2 > sx + 10.0);
        LT_CHECK(fabs(sy2 - sy) < 8.0);
    }
    /* zooming in enlarges the footprint */
    lt_t3d_reset_view(&t);
    lt_cam3_setup(&t, 0, 0, 600, 400, &cam);
    lt_t3d_world(3, 4, 0, 0.0, 8.0, 0, 0, z[0], &wx, &wy, &wz);
    (void)lt_cam3_proj(&cam, wx, wy, wz, &sx, &sy);
    lt_t3d_zoom(&t, 0.5);
    lt_cam3_setup(&t, 0, 0, 600, 400, &cam2);
    {
        double sx2, sy2;
        (void)lt_cam3_proj(&cam2, wx, wy, wz, &sx2, &sy2);
        LT_CHECK(fabs(sx2 - 300.0) > fabs(sx - 300.0));
    }
}

/* closing a tab or dialog mid-frame (Ctrl+W, Esc) freed the state that the frame's custom draw
 * callbacks (3D view, overlay, dyno chart) read when the frame was replayed */
LT_TEST(close_mid_frame_keeps_draw_state)
{
    lt_app *app;
    LT_CHECK(lt_plat_setenv("LT_TABLE_VIEW", "3d") == 0);
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "tests/fixtures/dyno/run_before.csv") == 0);
    app = demo_app();
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_close_mid.ppm", 1100, 800, 2);
    LT_CHECK(app->n_tabs >= 2);
    while (app->n_tabs > 1) {
        int n = app->n_tabs;
        press(app, 'w', LT_MOD_CTRL);
        if (app->n_tabs == n) { break; }        /* the dashboard tab may be pinned */
    }
    LT_CHECK(lt_app_dialog_push(app, "dyno", "", "Dyno Overlay") != NULL);
    frame(app);
    frame(app);
    press(app, LT_KEY_ESCAPE, 0);
    frame(app);
    LT_CHECK(app->n_dialogs == 0);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_TABLE_VIEW", "") == 0);
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "") == 0);
}

/* a dialog that opens another project (New Project's Create, sync, import...) closes the old one,
 * which frees every dialog; the dialog loop then read the freed dialog */
static int g_reopen_draws;
static void reopen_draw(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(d);
    g_reopen_draws++;
    lt_app_close_project(app);                  /* d is freed here: touch nothing */
}

LT_TEST(dialog_that_closes_the_project)
{
    lt_app *app = demo_app();
    lt_app_dialog_kind dk;
    memset(&dk, 0, sizeof(dk));
    dk.kind = "test_reopen"; dk.draw = reopen_draw; dk.close = NULL;
    lt_app_register_dialog_kind(app, &dk);
    LT_CHECK(lt_app_dialog_push(app, "about", "", "About") != NULL);
    LT_CHECK(lt_app_dialog_push(app, "test_reopen", "", "Reopen") != NULL);
    LT_CHECK(lt_app_dialog_push(app, "shortcuts", "", "Keys") != NULL);
    g_reopen_draws = 0;
    lt_app_screenshot(app, "build/lt_test_reopen.ppm", 800, 600, 2);
    LT_CHECK(g_reopen_draws == 1);
    LT_CHECK(app->n_dialogs == 0);
    lt_app_free(app);
}

/* "3D view" from the grid's context menu: the 3D callback ran before the table's colours (and its
 * app pointer) were set that frame and crashed on a NULL app */
LT_TEST(t3d_from_context_menu)
{
    lt_app *app;
    LT_CHECK(lt_plat_setenv("LT_SHOT_SCRIPT", "m:400/400 rclick:50/25 m:470/785 m:470/785 "
                            "d:470/785 u:470/785 m:470/785") == 0);
    app = demo_app();
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_3d_menu.ppm", 1100, 800, 2);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_SHOT_SCRIPT", "") == 0);
}

LT_TEST(t3d_tab_renders_and_takes_input)
{
    lt_app *app;
    lt_event ev;
    int i;
    LT_CHECK(lt_plat_setenv("LT_TABLE_VIEW", "3d,wire,pick") == 0);
    app = demo_app();
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_3d.ppm", 1280, 800, 3);
    /* wheel, drag and right drag over the 3D area; keys while hovering */
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_MOUSE_MOVE; ev.x = 1000; ev.y = 450;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_WHEEL; ev.wheel = 1;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_DOWN; ev.button = LT_MB_LEFT;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_MOVE; ev.x = 1060; ev.y = 480;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_UP;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_DOWN; ev.button = LT_MB_RIGHT;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_MOVE; ev.x = 1020; ev.y = 440;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_UP;
    lt_ui_input(app->ui, &ev);
    frame(app);
    press(app, LT_KEY_LEFT, 0);
    press(app, LT_KEY_PAGEUP, 0);
    press(app, LT_KEY_HOME, 0);
    for (i = 0; i < 3; i++) { frame(app); }
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_TABLE_VIEW", "") == 0);
}

/* ---- G-27 ---- */
LT_TEST(lambda_helpers)
{
    LT_CHECK_STR(lt_lambda_fuel_label(0), "E0");
    LT_CHECK_STR(lt_lambda_fuel_label(4), "E100");
    LT_CHECK_STR(lt_lambda_fuel_label(99), "E0");
    LT_CHECK_NEAR(lt_lambda_fuel_stoich(3), 9.85, 1e-12);
    LT_CHECK_NEAR(lt_lambda_preview_value(14.7, 0, 14.7), 1.0, 1e-12);
    LT_CHECK_NEAR(lt_lambda_preview_value(12.5, 0, 9.0), 12.5 / 9.0, 1e-12);
    LT_CHECK_NEAR(lt_lambda_preview_value(0.85, 1, 14.7), 12.495, 1e-9);
    LT_CHECK(lt_lambda_blend_percent(14.7, 1.0) == 0);
    LT_CHECK(lt_lambda_blend_percent(9.0, 1.0) == 100);
    LT_CHECK(lt_lambda_blend_percent(11.85, 1.0) == 50);
    LT_CHECK(lt_lambda_blend_percent(14.7, 0.0) == -1);
    LT_CHECK(lt_lambda_blend_percent(20.0, 1.0) == 0);
}

LT_TEST(lambda_preview_renders)
{
    lt_app *app = demo_app();
    int t0, p0, a0, m0, i;
    lt_units_get(&t0, &p0, &a0, &m0);
    LT_CHECK(lt_app_table_editor_open(app, "afrTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_lambda.ppm", 1280, 800, 4);
    lt_units_set(t0, p0, 1, m0);     /* lambda display: the companion then shows AFR */
    for (i = 0; i < 3; i++) { frame(app); }
    lt_units_set(t0, p0, a0, m0);
    lt_app_free(app);
}

/* ---- G-28 ---- */
LT_TEST(large_change_alert)
{
    lt_app *app = demo_app();
    lt_logx *x = lt_logx_get(app);
    lt_tblview v;
    double z[4], z0[4];
    x->alert_on = 1; x->alert_abs = 5.0; x->alert_pct = 10.0;
    LT_CHECK(lt_logx_check_change(app, 50.0, 51.0) == 0);      /* 1: below both */
    LT_CHECK(lt_logx_check_change(app, 50.0, 56.0) == 1);      /* absolute */
    LT_CHECK(lt_logx_check_change(app, 4.0, 4.5) == 1);        /* percent */
    LT_CHECK(lt_logx_check_change(app, 50.0, 50.0) == 0);
    memset(&v, 0, sizeof(v));
    z0[0] = 1.0; z0[1] = 2.0; z0[2] = 3.0; z0[3] = 4.0;
    memcpy(z, z0, sizeof(z));
    v.rows = 2; v.cols = 2; v.z = z; v.z0 = z0;
    LT_CHECK(lt_tabs_check_change(app, &v) == 0);              /* nothing changed */
    z[3] = 4.05;
    LT_CHECK(lt_tabs_check_change(app, &v) == 0);              /* 1.25 %, 0.05 */
    z[1] = 9.0;                                                /* the largest change wins */
    LT_CHECK(lt_tabs_check_change(app, &v) == 1);
    x->alert_on = 0;
    LT_CHECK(lt_tabs_check_change(app, &v) == 0);              /* switched off */
    lt_app_free(app);
}

/* ---- G-35 ---- */
LT_TEST(perf_math)
{
    lt_perf_specs s;
    lt_perf_result r;
    double g[4], et, tr, z60;
    lt_perf_specs_default(&s);
    LT_CHECK_NEAR(s.mass_kg, 1360.776, 1e-3);
    LT_CHECK_NEAR(lt_perf_drag_force(0.0, s.frontal_m2, s.cd), 0.0, 1e-12);
    LT_CHECK_NEAR(lt_perf_drag_force(30.0, 2.0, 0.3), 0.5 * 1.225 * 900.0 * 0.3 * 2.0, 1e-9);
    LT_CHECK_NEAR(lt_perf_rolling_force(1000.0), 0.015 * 1000.0 * 9.81, 1e-9);
    /* steady 100 km/h cruise: no acceleration term, the power is the resistance power over the drivetrain */
    {
        double v = 100.0 / 3.6, w = (lt_perf_drag_force(v, s.frontal_m2, s.cd) + lt_perf_rolling_force(s.mass_kg)) * v / 0.85;
        LT_CHECK_NEAR(lt_perf_power_w(&s, v, 0.0), w, 1e-6);
        lt_perf_compute(&s, v, 0.0, 3000.0, &r);
        LT_CHECK_NEAR(r.hp, w / 745.7, 1e-6);
        LT_CHECK_NEAR(r.tq_nm, w / (2.0 * 3.14159265358979323846 * 3000.0 / 60.0), 1e-6);
        LT_CHECK_NEAR(r.tq_lbft, r.tq_nm * 0.737562, 1e-9);
    }
    lt_perf_estimates(300.0, 3000.0, &et, &tr, &z60);
    LT_CHECK_NEAR(et, 5.825 * pow(10.0, 1.0 / 3.0), 1e-9);
    LT_CHECK_NEAR(tr, 234.0 * pow(0.1, 1.0 / 3.0), 1e-9);
    LT_CHECK_NEAR(z60, 3.7 * pow(10.0, 0.42), 1e-9);
    lt_perf_estimates(0.0, 3000.0, &et, &tr, &z60);
    LT_CHECK(et == 0.0 && tr == 0.0 && z60 == 0.0);
    /* coasting down gives no negative power */
    lt_perf_compute(&s, 20.0, -5.0, 3000.0, &r);
    LT_CHECK(r.hp == 0.0 && r.tq_nm == 0.0);
    /* gearing round trip */
    {
        double v = lt_perf_speed_at_rpm(&s, 1.0, 3000.0);
        LT_CHECK_NEAR(v, 3000.0 / 3.55 * 3.14159265358979323846 * 26.0 * 0.0254 / 60.0, 1e-9);
        LT_CHECK_NEAR(lt_perf_rpm_at_speed(&s, 1.0, v), 3000.0, 1e-6);
        LT_CHECK(lt_perf_speed_at_rpm(&s, 0.0, 3000.0) == 0.0);
    }
    LT_CHECK(lt_perf_parse_gears("2.97, 2.07  1.43;1", g, 4) == 4);
    LT_CHECK_NEAR(g[2], 1.43, 1e-12);
    LT_CHECK(lt_perf_parse_gears("2.97, x", g, 4) == -1);
    LT_CHECK(lt_perf_parse_gears("1 2 3 4 5", g, 4) == -1);
    LT_CHECK(lt_perf_parse_gears("-1", g, 4) == -1);
    LT_CHECK(lt_perf_parse_gears("", g, 4) == 0);
    LT_CHECK_NEAR(lt_perf_accel(10.0, 12.0, 0.5), 4.0, 1e-12);
    LT_CHECK(lt_perf_accel(10.0, 12.0, 0.0) == 0.0);
}

LT_TEST(perf_fuel_economy)
{
    /* 4 injectors, 440 cc/min, 3 ms at 3000 rpm: 1500 events/min per injector set */
    double cc = lt_perf_fuel_cc_min(3.0, 3000.0, 4, 440.0, 0);
    LT_CHECK_NEAR(cc, 1500.0 * 4.0 * 0.003 * 440.0 / 60.0, 1e-9);
    LT_CHECK_NEAR(lt_perf_fuel_cc_min(3.0, 3000.0, 4, 440.0, 1), 2.0 * cc, 1e-9);
    LT_CHECK(lt_perf_fuel_cc_min(0.0, 3000.0, 4, 440.0, 0) == 0.0);
    LT_CHECK(lt_perf_mpg(0.0, 100.0) == 0.0);
    LT_CHECK(lt_perf_mpg(20.0, 0.0) == 0.0);
    {
        double v = 60.0 * 0.44704;             /* 60 mph */
        double mpg = lt_perf_mpg(v, 3785.411784 / 2.0);   /* half a gallon a minute: 2 miles per gallon */
        LT_CHECK_NEAR(mpg, 2.0, 1e-6);
        LT_CHECK_NEAR(lt_perf_kpl(1000.0 / 60.0, 1000.0), 1.0, 1e-9);
        LT_CHECK_NEAR(lt_perf_l100(1000.0 / 60.0, 1000.0), 100.0, 1e-9);
    }
}

LT_TEST(perf_dialog_renders)
{
    lt_app *app = demo_app();
    LT_CHECK(lt_app_dialog_push(app, "perf", "", "Performance Calculator") != NULL);
    lt_app_screenshot(app, "build/lt_test_perf.ppm", 1100, 800, 3);
    lt_app_free(app);
}

/* ---- G-81 ---- */
LT_TEST(dyno_chart_range)
{
    lt_dyno_run runs[2];
    double r0 = 0, r1 = 0, vm = 0;
    LT_CHECK(lt_dyno_from_csv("tests/fixtures/dyno/run_before.csv", "A", &runs[0]) == 0);
    LT_CHECK(lt_dyno_from_csv("tests/fixtures/dyno/run_after.csv", "B", &runs[1]) == 0);
    LT_CHECK(runs[0].n == 21 && runs[1].n == 21);
    LT_CHECK(runs[0].has_peak_hp && runs[1].peak_hp > runs[0].peak_hp);
    LT_CHECK(lt_dyno_chart_range(runs, 2, &r0, &r1, &vm));
    LT_CHECK_NEAR(r0, 2000.0, 1e-9);
    LT_CHECK_NEAR(r1, 7000.0, 1e-9);
    LT_CHECK(vm >= runs[1].peak_hp && vm >= runs[1].peak_tq && fmod(vm, 50.0) == 0.0);
    LT_CHECK(!lt_dyno_chart_range(runs, 0, &r0, &r1, &vm));
    lt_dyno_run_free(&runs[0]);
    lt_dyno_run_free(&runs[1]);
    {
        char **h = NULL;
        size_t n = 0;
        LT_CHECK(lt_dyno_csv_headers("tests/fixtures/dyno/run_before.csv", &h, &n) == 0);
        LT_CHECK(n == 6);
        LT_CHECK_STR(h[1], "HP");
        lt_strv_free(h, n);
    }
}

LT_TEST(dyno_dialog_with_runs)
{
    lt_app *app;
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "tests/fixtures/dyno/run_before.csv|tests/fixtures/dyno/run_after.csv|tests/fixtures/dyno/missing.csv") == 0);
    LT_CHECK(lt_plat_setenv("LT_DYNO_TAB", "c") == 0);
    app = demo_app();
    LT_CHECK(lt_app_dialog_push(app, "dyno", "", "Dyno Overlay") != NULL);
    lt_app_screenshot(app, "build/lt_test_dyno.ppm", 1100, 800, 3);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_DYNO_TAB", "1") == 0);
    app = demo_app();
    LT_CHECK(lt_app_dialog_push(app, "dyno", "", "Dyno Overlay") != NULL);
    lt_app_screenshot(app, "build/lt_test_dyno2.ppm", 1100, 800, 3);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "") == 0);
    LT_CHECK(lt_plat_setenv("LT_DYNO_TAB", "") == 0);
}

/* a run with absurd values: the chart grid stepped 50 at a time up to 1e23 (never finished) */
LT_TEST(dyno_hostile_values)
{
    static const char csv[] = "RPM,HP,Torque\n2000,60,150\n3000,99999999999999999999999,inf\n"
                              "4000,nan,1e309\n5000,120,160\n";
    lt_dyno_run run;
    lt_app *app;
    LT_CHECK(lt_file_write("build/lt_test_dyno_hostile.csv", csv, sizeof(csv) - 1) == 0);
    LT_CHECK(lt_dyno_from_csv("build/lt_test_dyno_hostile.csv", "H", &run) == 0);
    LT_CHECK(run.n == 4);
    LT_CHECK(run.data[1].has_hp && !run.data[1].has_tq);
    LT_CHECK(!run.data[2].has_hp && !run.data[2].has_tq);
    lt_dyno_run_free(&run);
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "build/lt_test_dyno_hostile.csv") == 0);
    app = demo_app();
    LT_CHECK(lt_app_dialog_push(app, "dyno", "", "Dyno Overlay") != NULL);
    lt_app_screenshot(app, "build/lt_test_dyno3.ppm", 800, 600, 3);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_DYNO_RUNS", "") == 0);
}

/* ---- G-82 ---- */
LT_TEST(actions_logic)
{
    lt_arec r;
    lt_action_set *set;
    double z[4], z0[4];
    lt_action a;
    char d[200], f[200];
    char **v = NULL;
    size_t n;
    lt_arec_init(&r);
    z0[0] = 1.0; z0[1] = 2.0; z0[2] = 3.0; z0[3] = 4.0;
    memcpy(z, z0, sizeof(z));
    z[1] = 2.5; z[2] = 3.5;
    LT_CHECK(lt_actx_record_table(&r, "veTbl", z, z0, 2, 2) == 0);   /* idle: nothing recorded */
    lt_free(lt_arec_start(&r, "Set", "desc", "me"));
    LT_CHECK(lt_actx_record_table(&r, "veTbl", z, z0, 2, 2) == 2);
    LT_CHECK(lt_actx_record_constant(&r, "reqFuel", 8.0, 9.0) == 1);
    LT_CHECK(lt_actx_record_constant(&r, "reqFuel", 9.0, 9.0) == 0);
    set = lt_arec_stop(&r);
    LT_CHECK(set != NULL && set->n == 3);
    LT_CHECK(set->actions[0].type == LT_ACT_TABLE_EDIT && set->actions[0].x_index == 1 && set->actions[0].y_index == 0);
    LT_CHECK(set->actions[1].x_index == 0 && set->actions[1].y_index == 1);
    LT_CHECK_NEAR(set->actions[1].new_value, 3.5, 1e-12);
    LT_CHECK_NEAR(set->actions[1].old_value, 3.0, 1e-12);
    lt_actx_describe(&set->actions[0], d, sizeof(d));
    LT_CHECK_STR(d, "Table edit veTbl [1, 0]: 2 -> 2.5");
    lt_actx_describe(&set->actions[2], d, sizeof(d));
    LT_CHECK_STR(d, "Constant reqFuel: 8 -> 9");
    lt_action_init(&a, LT_ACT_PAUSE);
    lt_action_pause(&a, 250UL);
    lt_actx_append(set, &a);
    lt_actx_describe(&a, d, sizeof(d));
    LT_CHECK_STR(d, "Pause 250 ms");
    lt_action_clear(&a);
    LT_CHECK(set->n == 4);
    LT_CHECK(lt_actx_remove(set, 0) == 0 && set->n == 3);
    LT_CHECK(lt_actx_remove(set, 9) < 0);
    LT_CHECK(set->actions[2].type == LT_ACT_PAUSE);
    /* tags / ECUs */
    lt_actx_set_tags(set, " fuel, ve ,, idle ");
    LT_CHECK(set->meta.ntags == 3 && strcmp(set->meta.tags[2], "idle") == 0);
    lt_actx_join_list(set->meta.tags, set->meta.ntags, d, sizeof(d));
    LT_CHECK_STR(d, "fuel, ve, idle");
    LT_CHECK(lt_actx_compatible(set, "anything"));       /* no restriction */
    lt_actx_set_ecus(set, "rusEFI, Speeduino");
    LT_CHECK(set->meta.necus == 2);
    LT_CHECK(lt_actx_compatible(set, "rusEFI 2025.06"));
    LT_CHECK(lt_actx_compatible(set, "SPEEDUINO"));
    LT_CHECK(!lt_actx_compatible(set, "MegaSquirt"));
    LT_CHECK(!lt_actx_compatible(set, ""));
    n = lt_actx_split_list("", &v);
    LT_CHECK(n == 0 && v == NULL);
    lt_action_set_free(set);
    lt_free(set);
    lt_arec_free(&r);
    /* file names */
    lt_actx_new_file("build", "My: set/1", f, sizeof(f));
    LT_CHECK_STR(f, "My_ set_1.ltaction");
    lt_actx_new_file("build", "  ", f, sizeof(f));
    LT_CHECK_STR(f, "actions.ltaction");
}

LT_TEST(actions_record_save_play)
{
    lt_app *app;
    size_t rows = 0, count = 0, applied = 0, i;
    double v0, v1;
    const lt_ini_table *t;
    lt_pagesrc ps;
    lt_tblview v;
    char **names = NULL, *path;
    int nn, rc, i2;
    (void)lt_plat_remove("build/lt_test_actions/rec.ltaction");
    LT_CHECK(lt_plat_setenv("LT_ACTIONS_DIR", "build/lt_test_actions") == 0);
    LT_CHECK(lt_plat_mkdirs("build/lt_test_actions") == 0);
    nn = lt_plat_list_dir("build/lt_test_actions", &names);
    for (i = 0; nn > 0 && i < (size_t)nn; i++) {
        char *p = lt_path_join("build/lt_test_actions", names[i]);
        (void)lt_plat_remove(p);
        lt_free(p);
    }
    if (nn >= 0) { lt_plat_free_list(names, nn); }
    app = demo_app();
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_act0.ppm", 900, 700, 3);
    t = lt_ini_find_table(app->ini, "veTableTbl");
    ps = lt_app_pages(app);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, t, &ps) == 0);
    rows = v.rows;
    lt_tblview_free(&v);
    v0 = cell0(app, "veTableTbl", rows - 1, 0);
    LT_CHECK(lt_feat_actions_recording(app) == 0);
    press(app, '+', 0);                                     /* not recorded */
    LT_CHECK(lt_feat_actions_stop(app, &count) < 0);        /* nothing is recording */
    LT_CHECK(lt_feat_actions_start(app, "", "") < 0);
    LT_CHECK(lt_feat_actions_start(app, "rec", "test set") == 0);
    LT_CHECK(lt_feat_actions_recording(app) == 1);
    press(app, '+', 0);
    v1 = cell0(app, "veTableTbl", rows - 1, 0);
    LT_CHECK_NEAR(v1, v0 + 2.0, 0.101);
    LT_CHECK(lt_feat_actions_stop(app, &count) == 0);
    LT_CHECK(count == 1);
    LT_CHECK(lt_feat_actions_recording(app) == 0);
    /* the set landed in the folder */
    path = lt_path_join("build/lt_test_actions", "rec.ltaction");
    LT_CHECK(lt_file_exists(path));
    {
        lt_action_set set;
        lt_action_set_init(&set);
        LT_CHECK(lt_action_set_load(path, &set) == 0);
        LT_CHECK(set.n == 1 && set.actions[0].type == LT_ACT_TABLE_EDIT);
        LT_CHECK_STR(set.actions[0].table_name, "veTableTbl");
        LT_CHECK(set.actions[0].x_index == 0 && set.actions[0].y_index == rows - 1);
        LT_CHECK_STR(set.name, "rec");
        lt_action_set_free(&set);
    }
    /* undo, then play the recorded set back: the edit returns */
    press(app, 'z', LT_MOD_CTRL);
    press(app, 'z', LT_MOD_CTRL);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0, 0.101);
    rc = lt_feat_actions_play_file(app, path, &applied);
    LT_CHECK(rc == 0 && applied == 1);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v1, 0.101);
    LT_CHECK(app->modified);
    /* a set that does not validate is refused */
    {
        lt_action_set bad;
        lt_action a;
        char *bp = lt_path_join("build/lt_test_actions", "bad.ltaction");
        lt_action_set_init(&bad);
        bad.name = lt_strdup("bad");
        lt_action_init(&a, LT_ACT_TABLE_EDIT);
        lt_action_table_edit(&a, "noSuchTable", 0, 0, 1.0, 0, 0.0);
        lt_actx_append(&bad, &a);
        lt_action_clear(&a);
        LT_CHECK(lt_action_set_save(&bad, bp) == 0);
        LT_CHECK(lt_feat_actions_play_file(app, bp, &applied) < 0 && applied == 0);
        lt_action_set_free(&bad);
        lt_free(bp);
    }
    lt_free(path);
    /* the dialog lists the saved sets */
    LT_CHECK(lt_app_dialog_push(app, "actions", "", "Action Recorder") != NULL);
    for (i2 = 0; i2 < 3; i2++) { frame(app); }
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_ACTIONS_DIR", "") == 0);
}

LT_TEST(actions_record_constants)
{
    lt_app *app;
    lt_action_set set;
    char *path;
    LT_CHECK(lt_plat_setenv("LT_ACTIONS_DIR", "build/lt_test_actions") == 0);
    LT_CHECK(lt_plat_mkdirs("build/lt_test_actions") == 0);
    app = demo_app();
    LT_CHECK(lt_feat_actions_start(app, "consts", "") == 0);
    lt_feat_actions_note_constant(app, "rpmHardLimit", 6500.0, 7000.0);
    lt_feat_actions_note_constant(app, "rpmHardLimit", 7000.0, 7000.0);
    {
        size_t cnt = 0;
        LT_CHECK(lt_feat_actions_stop(app, &cnt) == 0 && cnt == 1);
    }
    path = lt_path_join("build/lt_test_actions", "consts.ltaction");
    lt_action_set_init(&set);
    LT_CHECK(lt_action_set_load(path, &set) == 0);
    LT_CHECK(set.n == 1 && set.actions[0].type == LT_ACT_CONSTANT_CHANGE);
    LT_CHECK_NEAR(set.actions[0].new_value, 7000.0, 1e-9);
    lt_action_set_free(&set);
    lt_app_screenshot(app, "build/lt_test_act2.ppm", 900, 700, 2);
    lt_free(path);
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_ACTIONS_DIR", "") == 0);
}

int main(void)
{
    LT_RUN(tablecmp_compute);
    LT_RUN(tablecmp_percent_rules);
    LT_RUN(tablecmp_dialog_runs);
    LT_RUN(t3d_camera_limits);
    LT_RUN(t3d_pick_and_pan);
    LT_RUN(dialog_that_closes_the_project);
    LT_RUN(t3d_from_context_menu);
    LT_RUN(t3d_tab_renders_and_takes_input);
    LT_RUN(close_mid_frame_keeps_draw_state);
    LT_RUN(lambda_helpers);
    LT_RUN(lambda_preview_renders);
    LT_RUN(large_change_alert);
    LT_RUN(perf_math);
    LT_RUN(perf_fuel_economy);
    LT_RUN(perf_dialog_renders);
    LT_RUN(dyno_chart_range);
    LT_RUN(dyno_dialog_with_runs);
    LT_RUN(dyno_hostile_values);
    LT_RUN(actions_logic);
    LT_RUN(actions_record_save_play);
    LT_RUN(actions_record_constants);
    LT_TEST_MAIN_END
}
