/* test_tables.c - table editor / AutoTune screens driven headlessly through key events.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/tableops/tableops.h"

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

LT_TEST(table_keys_edit_undo)
{
    char buf[512];
    lt_app *app = lt_app_new();
    double v0, v1;
    size_t rows = 0;
    const lt_ini_table *t;
    lt_pagesrc ps;
    lt_tblview v;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables.ppm", 900, 700, 3);
    t = lt_ini_find_table(app->ini, "veTableTbl");
    ps = lt_app_pages(app);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, t, &ps) == 0);
    rows = v.rows;
    lt_tblview_free(&v);
    /* cursor starts at display (0,0) = data (rows-1, 0) with Y axis at bottom */
    v0 = cell0(app, "veTableTbl", rows - 1, 0);
    press(app, '+', 0);
    v1 = cell0(app, "veTableTbl", rows - 1, 0);
    LT_CHECK_NEAR(v1, v0 + 1.0, 0.051);
    LT_CHECK(app->modified);
    press(app, 'z', LT_MOD_CTRL);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0, 0.051);
    press(app, 'y', LT_MOD_CTRL);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0 + 1.0, 0.051);
    press(app, '-', 0);
    press(app, '-', 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0 - 1.0, 0.051);
    LT_CHECK(lt_app_can_burn(app));
    lt_app_free(app);
}

LT_TEST(curve_and_autotune_tabs)
{
    char buf[512];
    lt_app *app = lt_app_new();
    int i;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_curve_editor_open(app, "knockThresholdCurve") == 0);
    LT_CHECK(lt_app_autotune_open(app) == 0);
    lt_app_screenshot(app, "build/lt_test_tables2.ppm", 1000, 700, 3);
    {   /* the demo tune has an all-zero AFR target table: give it 14.7 so pre-flight passes */
        const lt_ini_table *tt = lt_ini_find_table(app->ini, app->ini->ve_analyze.target_table);
        const lt_ini_const *zc = tt != NULL ? lt_ini_find_const(app->ini, tt->z_const) : NULL;
        lt_pagesrc ps = lt_app_pages(app);
        double arr[1024];
        size_t n = zc != NULL ? zc->shape.rows * zc->shape.cols : 0, k;
        LT_CHECK(n > 0 && n <= 1024);
        for (k = 0; k < n; k++) { arr[k] = 14.7; }
        if (n > 0 && n <= 1024) { lt_ini_const_set_array(app->ini, zc, &ps, arr, n); lt_app_edited(app); }
    }
    press(app, ' ', 0);
    for (i = 0; i < 40; i++) { lt_plat_sleep_ms(30); lt_app_service(app); frame(app); }
    lt_app_screenshot(app, "build/lt_test_at.ppm", 1280, 800, 3);
    LT_CHECK(lt_app_tab_find(app, "autotune", "autotune") != NULL);
    LT_CHECK(lt_app_tab_find(app, "curve", "knockThresholdCurve") != NULL);
    lt_app_free(app);
}

static void click(lt_app *app, int x, int y, int button)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_MOUSE_MOVE; ev.x = x; ev.y = y; lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_DOWN; ev.button = button; lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_MOUSE_UP; lt_ui_input(app->ui, &ev);
    frame(app);
}

LT_TEST(table_selection_keys)
{
    char buf[512];
    lt_app *app = lt_app_new();
    size_t rows = 0, cols = 0;
    double a, b, c, d, e, f;
    const lt_ini_table *t;
    lt_pagesrc ps;
    lt_tblview v;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables3.ppm", 1100, 800, 3);
    t = lt_ini_find_table(app->ini, "veTableTbl");
    ps = lt_app_pages(app);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, t, &ps) == 0);
    rows = v.rows; cols = v.cols;
    lt_tblview_free(&v);
    click(app, 500, 300, LT_MB_LEFT);
    press(app, LT_KEY_HOME, LT_MOD_CTRL);              /* cursor to display (0,0) = data (rows-1,0) */
    a = cell0(app, "veTableTbl", rows - 1, 0);
    b = cell0(app, "veTableTbl", rows - 1, 1);
    c = cell0(app, "veTableTbl", rows - 2, 1);
    d = cell0(app, "veTableTbl", rows - 1, 2);
    /* Shift+arrows extend the selection to a 2x2 block; '+' adds 1 to all four */
    press(app, LT_KEY_RIGHT, LT_MOD_SHIFT);
    press(app, LT_KEY_DOWN, LT_MOD_SHIFT);
    press(app, '+', 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), a + 1.0, 0.051);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 1), b + 1.0, 0.051);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 2, 1), c + 1.0, 0.051);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 2), d, 1e-9);
    /* Escape reverts the selection to the values at open */
    press(app, LT_KEY_ESCAPE, 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), a, 1e-9);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 2, 1), c, 1e-9);
    /* Ctrl+A selects everything */
    e = cell0(app, "veTableTbl", 0, cols - 1);
    f = cell0(app, "veTableTbl", rows / 2, cols / 2);
    press(app, 'a', LT_MOD_CTRL);
    press(app, '-', 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", 0, cols - 1), e - 1.0, 0.051);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows / 2, cols / 2), f - 1.0, 0.051);
    press(app, LT_KEY_ESCAPE, 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", 0, cols - 1), e, 1e-9);
    /* Delete clears the cursor cell to zero */
    press(app, LT_KEY_HOME, LT_MOD_CTRL);
    press(app, LT_KEY_DELETE, 0);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), 0.0, 1e-9);
    press(app, 'z', LT_MOD_CTRL);
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), a, 1e-9);
    /* right click opens the context menu without changing values */
    click(app, 500, 300, LT_MB_RIGHT);
    LT_CHECK(lt_ui_popup_open(app->ui));
    press(app, LT_KEY_ESCAPE, 0);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(table_selection_keys);
    LT_RUN(table_keys_edit_undo);
    LT_RUN(curve_and_autotune_tabs);
    LT_TEST_MAIN_END
}
