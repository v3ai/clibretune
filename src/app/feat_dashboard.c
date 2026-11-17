/* feat_dashboard.c - dashboard features outside the tab itself: display settings (refresh cap,
 * right-aligned values, indicator grid, designer snapping / lock, range sync), full screen
 * (F11, View menu) and the kiosk frame that shows only the dashboard.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include "feat.h"
#include "dash_tab.h"
#include "../plat/plat.h"

static char *cfg_path(lt_app *app)
{
    return app->config_dir != NULL ? lt_path_join(app->config_dir, "dashboard.conf") : NULL;
}

static void cfg_defaults(dash_cfg *c)
{
    c->refresh_hz = 60;
    c->right_align = 0;
    c->ind_cols = 0; c->ind_fill = 0; c->ind_wrap = 0;
    c->snap = 1; c->free_move = 0; c->lock = 0; c->auto_sync = 1;
}

static int clamp_hz(int hz)
{
    static const int ok[6] = { 10, 15, 20, 25, 30, 60 };
    int i, best = 4;
    for (i = 0; i < 6; i++) {
        int d = hz > ok[i] ? hz - ok[i] : ok[i] - hz;
        int bd = hz > ok[best] ? hz - ok[best] : ok[best] - hz;
        if (d < bd) { best = i; }
    }
    return ok[best];
}

dash_cfg *lt_dashx_cfg(lt_app *app)
{
    dash_cfg *c = (dash_cfg *)lt_ext_state(app, "dashboard", sizeof(dash_cfg), NULL);
    if (!c->loaded) {
        char *path = cfg_path(app);
        c->loaded = 1;
        cfg_defaults(c);
        if (path != NULL && lt_file_exists(path)) {
            lt_props p;
            lt_props_init(&p);
            if (lt_props_load(&p, path) == 0) {
                lt_props_get_i32(&p, "refresh_hz", &c->refresh_hz);
                lt_props_get_i32(&p, "right_align", &c->right_align);
                lt_props_get_i32(&p, "indicator_columns", &c->ind_cols);
                lt_props_get_i32(&p, "indicator_fill_empty", &c->ind_fill);
                lt_props_get_i32(&p, "indicator_text_wrap", &c->ind_wrap);
                lt_props_get_i32(&p, "snap_to_grid", &c->snap);
                lt_props_get_i32(&p, "free_move", &c->free_move);
                lt_props_get_i32(&p, "lock_positions", &c->lock);
                lt_props_get_i32(&p, "auto_sync_ranges", &c->auto_sync);
            }
            lt_props_free(&p);
        }
        lt_free(path);
        c->refresh_hz = clamp_hz(c->refresh_hz);
        if (c->ind_cols < 0 || c->ind_cols > 40) { c->ind_cols = 0; }
        lt_dashx_apply(app);
    }
    return c;
}

void lt_dashx_cfg_save(lt_app *app)
{
    dash_cfg *c = lt_dashx_cfg(app);
    char *path = cfg_path(app);
    lt_props p;
    char num[24];
    if (path == NULL) { return; }
    lt_props_init(&p);
#define PUT(key, val) lt_snprintf(num, sizeof num, "%d", (val)); lt_props_set(&p, key, num)
    PUT("refresh_hz", c->refresh_hz);
    PUT("right_align", c->right_align);
    PUT("indicator_columns", c->ind_cols);
    PUT("indicator_fill_empty", c->ind_fill);
    PUT("indicator_text_wrap", c->ind_wrap);
    PUT("snap_to_grid", c->snap);
    PUT("free_move", c->free_move);
    PUT("lock_positions", c->lock);
    PUT("auto_sync_ranges", c->auto_sync);
#undef PUT
    (void)lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(path);
}

void lt_dashx_apply(lt_app *app)
{
    dash_cfg *c = (dash_cfg *)lt_ext_state(app, "dashboard", sizeof(dash_cfg), NULL);
    lt_gauges_set_options(c->right_align, c->ind_wrap);
}

/* ---- full screen ---- */
int lt_dashx_set_kiosk(lt_app *app, int on)
{
    dash_cfg *c = lt_dashx_cfg(app);
    if (on) {
        if (app->ini == NULL) { return -1; }
        if (lt_app_dashboard_tab_open(app) != 0) { return -1; }
        c->kiosk_t0 = app->ui != NULL ? lt_ui_now(app->ui) : 0.0;
    }
    c->kiosk = on ? 1 : 0;
    (void)lt_backend_set_fullscreen(app->backend, on ? 1 : 0);
    if (app->ui != NULL) { lt_ui_request_redraw(app->ui); }
    return 0;
}

static void kiosk_hint_cb(lt_gfx *g, lt_rect r, void *user)
{
    LT_UNUSED(user);
    lt_gfx_fill_rect(g, r, LT_RGBA(0, 0, 0, 170));
}

int lt_dashx_kiosk_frame(lt_app *app)
{
    dash_cfg *c = lt_dashx_cfg(app);
    lt_ui *ui = app->ui;
    lt_app_tab *tab;
    dash_state *st;
    lt_rect cr;
    double age;
    if (!c->kiosk) { return 0; }
    tab = lt_app_tab_find(app, "dashboard", "dashboard");
    if (tab == NULL || app->ini == NULL) { (void)lt_dashx_set_kiosk(app, 0); return 0; }
    st = (dash_state *)tab->data;
    if (!lt_ui_text_focused(ui)) {
        int esc = lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0);
        if (lt_ui_key_pressed(ui, LT_KEY_F11, 0) ||
            (esc && !lt_ui_popup_open(ui) && (st == NULL || !st->zoom_open))) {
            (void)lt_dashx_set_kiosk(app, 0);
            return 0;
        }
    }
    cr = lt_ui_client_rect(ui);
    lt_ui_draw_fill(ui, cr, LT_RGB(0, 0, 0));
    lt_ui_region_begin(ui, cr, 0);
    lt_dtab_draw_body(app, tab, cr);
    lt_ui_region_end(ui);
    age = lt_ui_now(ui) - c->kiosk_t0;
    if (age >= 0.0 && age < 3500.0 && !lt_ui_cell_mode(ui)) {
        const char *msg = "Press F11 or Esc to leave full screen";
        int w = lt_ui_text_width(ui, msg) + lt_ui_px(ui, 24), h = lt_ui_row_height(ui) + lt_ui_px(ui, 8);
        lt_rect hr = lt_rect_make(cr.x + (cr.w - w) / 2, cr.y + lt_ui_px(ui, 16), w, h);
        lt_ui_draw_custom(ui, hr, kiosk_hint_cb, NULL);
        lt_ui_draw_text_in(ui, hr, msg, LT_RGB(255, 255, 255), LT_UI_ALIGN_CENTER);
        lt_ui_request_redraw(ui);
    }
    return 1;
}

static void act_fullscreen(lt_app *app)
{
    dash_cfg *c = lt_dashx_cfg(app);
    (void)lt_dashx_set_kiosk(app, c->kiosk ? 0 : 1);
}

/* ---- settings page ---- */
static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    dash_cfg *c = lt_dashx_cfg(app);
    static const char *const hz[6] = { "10 Hz (battery saver)", "15 Hz", "20 Hz", "25 Hz", "30 Hz", "60 Hz (default, smoothest)" };
    static const char *const fit[2] = { "Scale to fit", "Wrap onto two lines" };
    int w1 = -1, w2[2], ch = 0, sel = c->refresh_hz >= 60 ? 5 : (c->refresh_hz - 10) / 5, t;
    double d;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Gauges", lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    w2[0] = 220; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Gauge refresh rate");
    if (sel < 0) { sel = 0; }
    if (sel > 5) { sel = 5; }
    if (lt_ui_combo(ui, "dx_hz", &sel, hz, 6)) { c->refresh_hz = sel >= 5 ? 60 : 10 + 5 * sel; ch = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "Caps how often gauges are redrawn. Lower rates use less CPU.", lt_ui_theme(ui)->muted);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Right-align numeric values", &c->right_align)) { ch = 1; }
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Indicators", lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Columns (0 = automatic)");
    d = (double)c->ind_cols;
    if (lt_ui_number(ui, "dx_cols", &d, 0, 40, 1.0, 0) & LT_UI_CHANGED) { c->ind_cols = (int)d; ch = 1; }
    lt_ui_label(ui, "Text that does not fit");
    t = c->ind_wrap;
    if (lt_ui_combo(ui, "dx_fit", &t, fit, 2)) { c->ind_wrap = t; ch = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Fill the empty cells of the last indicator row", &c->ind_fill)) { ch = 1; }
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Designer", lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Snap gauges to the grid", &c->snap)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Free move (ignore snapping)", &c->free_move)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Lock gauge positions", &c->lock)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Adopt gauge ranges from the ECU definition", &c->auto_sync)) { ch = 1; }
    if (ch) {
        lt_dashx_cfg_save(app);
        lt_dashx_apply(app);
        lt_dtab_cfg_changed(app);
        lt_ui_request_redraw(ui);
    }
}

void lt_feat_dashboard_register(lt_app_ext *reg)
{
    lt_ext_add_menu_item(reg, LT_EXT_MENU_VIEW, "&Full Screen Dashboard", "F11", act_fullscreen, lt_ext_pred_ini);
    lt_ext_add_shortcut_named(reg, "view.fullscreen", "Dashboard full screen", LT_KEY_F11, 0,
                              act_fullscreen, lt_ext_pred_ini);
    lt_ext_add_settings_page(reg, "Dashboard", settings_page);
}
