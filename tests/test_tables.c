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

static void type_text(lt_app *app, const char *s)
{
    lt_event ev;
    for (; *s != '\0'; s++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = LT_EV_TEXT; ev.text[0] = *s;
        lt_ui_input(app->ui, &ev);
        frame(app);
    }
}

static size_t table_snap(lt_app *app, const char *tname, double *out, size_t cap)
{
    lt_pagesrc ps = lt_app_pages(app);
    lt_tblview v;
    size_t n = 0;
    const lt_ini_table *t = lt_ini_find_table(app->ini, tname);
    lt_tblview_init(&v);
    if (t != NULL && lt_tblview_load_table(&v, app->ini, t, &ps) == 0 && v.rows * v.cols <= cap) {
        n = v.rows * v.cols;
        memcpy(out, v.z, n * sizeof(double));
    }
    lt_tblview_free(&v);
    return n;
}

/* a typed value is committed by clicking another cell: it belongs to the cell that was being
 * edited, not to the cell that was clicked */
LT_TEST(table_edit_commit_on_click_away)
{
    static double before[4096], after[4096];
    char buf[512];
    lt_app *app = lt_app_new();
    size_t n, k, changed = 0, at = 0;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables4.ppm", 1100, 800, 3);
    click(app, 500, 300, LT_MB_LEFT);
    press(app, LT_KEY_HOME, LT_MOD_CTRL);
    n = table_snap(app, "veTableTbl", before, 4096);
    LT_CHECK(n > 0);
    type_text(app, "12");
    click(app, 700, 450, LT_MB_LEFT);
    LT_CHECK(table_snap(app, "veTableTbl", after, 4096) == n);
    for (k = 0; k < n; k++) {
        if (after[k] != before[k]) { changed++; at = k; }
    }
    LT_CHECK(changed == 1);
    LT_CHECK_NEAR(after[at], 12.0, 1e-9);
    {   /* the cursor cell at display (0,0) is data (rows-1, 0) */
        lt_pagesrc ps = lt_app_pages(app);
        lt_tblview v;
        lt_tblview_init(&v);
        LT_CHECK(lt_tblview_load_table(&v, app->ini, lt_ini_find_table(app->ini, "veTableTbl"), &ps) == 0);
        LT_CHECK(at == (v.rows - 1) * v.cols);
        lt_tblview_free(&v);
    }
    lt_app_free(app);
}

static double curve_val(lt_app *app, const char *cname, int xaxis, size_t i)
{
    lt_pagesrc ps = lt_app_pages(app);
    lt_tblview v;
    double r = -1e9;
    const lt_ini_curve *c = lt_ini_find_curve(app->ini, cname);
    lt_tblview_init(&v);
    if (c != NULL && lt_tblview_load_curve(&v, app->ini, c, &ps) == 0 && i < v.cols) {
        r = xaxis ? v.x[i] : v.z[i];
    }
    lt_tblview_free(&v);
    return r;
}

/* Tab after typing in the curve list commits to the edited cell (not the next one) and an X value
 * that would break the axis order is kept between its neighbours */
LT_TEST(curve_edit_commit_on_tab)
{
    char buf[512];
    lt_app *app = lt_app_new();
    double x1, x2;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_curve_editor_open(app, "cltFuelCorrCurve") == 0);
    lt_app_screenshot(app, "build/lt_test_tables5.ppm", 1600, 1000, 3);
    x1 = curve_val(app, "cltFuelCorrCurve", 1, 1);
    x2 = curve_val(app, "cltFuelCorrCurve", 1, 2);
    click(app, 1224, 192, LT_MB_LEFT);               /* first row, Y column */
    type_text(app, "2");
    press(app, LT_KEY_TAB, 0);
    LT_CHECK_NEAR(curve_val(app, "cltFuelCorrCurve", 0, 0), 2.0, 1e-6);
    LT_CHECK_NEAR(curve_val(app, "cltFuelCorrCurve", 1, 1), x1, 1e-9);
    /* the cursor is now on the second row's X: 500 is past the third point */
    type_text(app, "500");
    press(app, LT_KEY_TAB, 0);
    LT_CHECK_NEAR(curve_val(app, "cltFuelCorrCurve", 1, 1), x2, 1e-6);
    LT_CHECK(curve_val(app, "cltFuelCorrCurve", 0, 1) < 2.0);
    lt_app_free(app);
}

/* Paste only writes the pasted cells: other cells keep values finer than the display precision
 * (Speeduino's closed loop boost duty table is 0.5 % per step shown with 0 decimals) */
LT_TEST(table_paste_leaves_other_cells)
{
    static double before[4096], after[4096];
    double arr[64];
    lt_app *app = lt_app_new();
    const lt_ini_const *zc;
    lt_pagesrc ps;
    size_t n, k, changed = 0;
    LT_CHECK(lt_app_start_demo(app, "tests/fixtures/ini/speeduino_202501.ini") == 0);
    zc = lt_ini_find_const(app->ini, "boostTableDutyLookup");
    LT_CHECK(zc != NULL);
    if (zc == NULL) { lt_app_free(app); return; }
    for (k = 0; k < 64; k++) { arr[k] = 30.5 + (double)k; }
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_const_set_array(app->ini, zc, &ps, arr, 64) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "boostDCLupTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables6.ppm", 1100, 800, 3);
    click(app, 500, 300, LT_MB_LEFT);
    press(app, LT_KEY_HOME, LT_MOD_CTRL);
    n = table_snap(app, "boostDCLupTbl", before, 4096);
    LT_CHECK(n == 64);
    press(app, 'c', LT_MOD_CTRL);
    press(app, LT_KEY_RIGHT, 0);
    press(app, 'v', LT_MOD_CTRL);
    LT_CHECK(table_snap(app, "boostDCLupTbl", after, 4096) == n);
    for (k = 0; k < n; k++) {
        if (after[k] != before[k]) { changed++; }
    }
    LT_CHECK(changed == 1);
    lt_app_free(app);
}

/* Revert brings back the values at open even after more edits than the undo history holds */
LT_TEST(table_revert_after_many_edits)
{
    char buf[512];
    lt_app *app = lt_app_new();
    size_t rows = 0;
    double v0;
    int i;
    lt_pagesrc ps;
    lt_tblview v;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables7.ppm", 1100, 800, 3);
    ps = lt_app_pages(app);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, lt_ini_find_table(app->ini, "veTableTbl"), &ps) == 0);
    rows = v.rows;
    lt_tblview_free(&v);
    click(app, 500, 300, LT_MB_LEFT);
    press(app, LT_KEY_HOME, LT_MOD_CTRL);
    v0 = cell0(app, "veTableTbl", rows - 1, 0);
    for (i = 0; i < LT_TBLVIEW_UNDO_MAX + 8; i++) { press(app, '+', 0); }
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0 + (double)(LT_TBLVIEW_UNDO_MAX + 8), 0.051);
    click(app, 871, 137, LT_MB_LEFT);                /* toolbar: Revert */
    LT_CHECK_NEAR(cell0(app, "veTableTbl", rows - 1, 0), v0, 1e-9);
    lt_app_free(app);
}

/* Closing a table tab after it drew in the same frame (Ctrl+W, closing the project from a
 * question) must not replay its live-cell overlay from freed state (caught by ASan builds) */
LT_TEST(table_tab_closed_mid_frame)
{
    char buf[512];
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_app_table_editor_open(app, "veTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables8.ppm", 1100, 800, 4);
    LT_CHECK(lt_app_tab_find(app, "table", "veTableTbl") != NULL);
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_app_tab_close(app, lt_app_tab_find(app, "table", "veTableTbl"));
    lt_ui_end(app->ui);
    LT_CHECK(lt_ui_needs_redraw(app->ui));
    frame(app);
    LT_CHECK(lt_app_tab_find(app, "table", "veTableTbl") == NULL);
    lt_app_free(app);
}

static double afr_raw(lt_app *app, size_t i)
{
    static double v[4096];
    lt_pagesrc ps = lt_app_pages(app);
    const lt_ini_const *c = lt_ini_find_const(app->ini, "lambdaTable");
    v[i] = -999.0;
    if (c != NULL) { (void)lt_ini_const_get_array(app->ini, c, &ps, v, 4096); }
    return v[i];
}

/* The +/- step follows the unit shown: on an AFR table shown in lambda it is one AFR in lambda
 * (about 0.07), not 1.0 lambda, which pinned every cell at the upper limit */
LT_TEST(table_step_in_lambda)
{
    char buf[512];
    lt_app *app = lt_app_new();
    size_t k;
    int i;
    double before;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    app->settings.afr_unit = 1;
    LT_CHECK(lt_app_table_editor_open(app, "afrTableTbl") == 0);
    lt_app_screenshot(app, "build/lt_test_tables9.ppm", 1100, 800, 3);
    click(app, 500, 300, LT_MB_LEFT);
    press(app, LT_KEY_HOME, LT_MOD_CTRL);
    k = 31 * 32;                                       /* display (0,0) = data (rows-1, 0) */
    before = afr_raw(app, k);
    press(app, '+', 0);
    LT_CHECK(afr_raw(app, k) > before + 0.5 && afr_raw(app, k) < before + 1.5);
    /* opened in AFR, switched to lambda: the step is converted with the cells */
    press(app, 'z', LT_MOD_CTRL);
    LT_CHECK_NEAR(afr_raw(app, k), before, 1e-9);
    app->settings.afr_unit = 0;
    for (i = 0; i < 3; i++) { frame(app); }
    press(app, '+', 0);
    LT_CHECK_NEAR(afr_raw(app, k), before + 1.0, 0.051);
    app->settings.afr_unit = 1;
    for (i = 0; i < 3; i++) { frame(app); }
    press(app, '-', 0);
    LT_CHECK(afr_raw(app, k) > before - 0.15 && afr_raw(app, k) < before + 0.15);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(table_selection_keys);
    LT_RUN(table_edit_commit_on_click_away);
    LT_RUN(curve_edit_commit_on_tab);
    LT_RUN(table_keys_edit_undo);
    LT_RUN(curve_and_autotune_tabs);
    LT_RUN(table_paste_leaves_other_cells);
    LT_RUN(table_revert_after_many_edits);
    LT_RUN(table_tab_closed_mid_frame);
    LT_RUN(table_step_in_lambda);
    LT_TEST_MAIN_END
}
