/* tables_lambda.c - read-only lambda companion of a Target AFR table (G-27): the AFR cells shown
 * as lambda for a selectable ethanol blend (or, when the table is displayed in lambda, as AFR),
 * with live lambda / AFR / calculated-fuel readouts. A viewing aid only: no editing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tables_priv.h"
#include "app_ext.h"
#include "view_kit.h"

static const char *fuel_get(void *ud, int i)
{
    static char buf[40];
    LT_UNUSED(ud);
    lt_snprintf(buf, sizeof(buf), "%s (%.2f:1)", lt_lambda_fuel_label(i), lt_lambda_fuel_stoich(i));
    return buf;
}

static lt_bool live_any(lt_app *app, const char *const *names, int n, double *out)
{
    int i;
    for (i = 0; i < n; i++) {
        if (lt_app_channel(app, names[i], out)) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static void live_line(lt_app *app, char *out, size_t cap)
{
    double lam = 0.0, afr = 0.0;
    static const char *const ln[] = { "lambdaValue", "lambda", "lambda1", "Lambda" };
    static const char *const an[] = { "AFRValue", "afr", "afr1", "AFR" };
    lt_bool hl = live_any(app, ln, 4, &lam);
    lt_bool ha = live_any(app, an, 4, &afr);
    char l[24], a[24], f[24];
    int e = hl && ha ? lt_lambda_blend_percent(afr, lam) : -1;
    if (hl) { lt_snprintf(l, sizeof(l), "%.2f", lam); } else { lt_strlcpy(l, "--", sizeof(l)); }
    if (ha) { lt_snprintf(a, sizeof(a), "%.1f", afr); } else { lt_strlcpy(a, "--", sizeof(a)); }
    if (e >= 0) { lt_snprintf(f, sizeof(f), "E%d", e); } else { lt_strlcpy(f, "--", sizeof(f)); }
    lt_snprintf(out, cap, "lambda %s   AFR %s   fuel %s", l, a, f);
}

/* the preview's grid follows the table grid (see lt_tabs_lsync); called right after its begin */
static void lambda_sync(lt_ui *ui, lt_tabs_lsync *s, int cw, int ch)
{
    int lx = 0, ly = 0;
    if (s == NULL || !s->ok || s->mcw <= 0 || s->mch <= 0) { return; }
    lt_ui_scroll_get(ui, &lx, &ly);
    if (s->have && (lx != s->lx || ly != s->ly)) {
        /* the preview was scrolled itself: the table follows */
        s->follow = 1;
        s->fx = (int)((long)lx * s->mcw / cw);
        s->fy = (int)((long)ly * s->mch / ch);
        lt_ui_request_redraw(ui);
    } else {
        lt_ui_grid_scroll_to(ui, (int)((long)s->msx * cw / s->mcw), (int)((long)s->msy * ch / s->mch));
    }
    lt_ui_scroll_get(ui, &s->lx, &s->ly);
    s->have = 1;
    if (s->sel[0] >= 0) { lt_ui_grid_select(ui, s->sel[0], s->sel[1], s->sel[2], s->sel[3]); }
}

int lt_tabs_lambda_strip(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lt_rect r = lt_ui_layout_next(ui);
    int w1 = -1, hit = 0;
    lt_ui_region_begin(ui, r, th->bg_app);
    lt_ui_layout_row(ui, 1, &w1, 28);
    if (lt_vk_icon_btn(ui, LT_ICON_CHEVRONS_LEFT, "Show the lambda preview", 0, 24, 24)) { hit = 1; }
    lt_ui_region_end(ui);
    return hit;
}

int lt_tabs_lambda_draw(lt_app *app, const lt_tblview *v, int ybottom, lt_tabs_lsync *sync)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int *fuel = (int *)lt_ext_state(app, "tables_lambda", sizeof(int), NULL);
    lt_rect r = lt_ui_layout_next(ui);
    int w1 = -1, w2[2], rows = (int)v->rows, cols = (int)v->cols, shown_l, cw, cell = lt_ui_cell_mode(ui);
    int collapse = 0;
    double stoich, zlo = 1e300, zhi = -1e300;
    size_t k;
    char b[96];
    if (*fuel < 0 || *fuel >= LT_LAMBDA_FUELS) { *fuel = 0; }
    stoich = lt_lambda_fuel_stoich(*fuel);
    shown_l = v->zu.kind == LT_UK_AFR && v->zu.dst == 1;
    for (k = 0; k < v->rows * v->cols; k++) {
        if (v->z[k] < zlo) { zlo = v->z[k]; }
        if (v->z[k] > zhi) { zhi = v->z[k]; }
    }
    lt_ui_region_begin(ui, r, th->bg_app);
    w2[0] = -1; w2[1] = cell ? 4 : 28;
    lt_ui_layout_row(ui, cell ? 1 : 2, w2, 0);
    lt_ui_label_ex(ui, shown_l ? "AFR preview (read-only)" : "Lambda preview (read-only)", th->text, LT_UI_ALIGN_LEFT);
    lt_ui_tooltip(ui, shown_l ? "The table in AFR for the chosen fuel; edit it on the left"
                              : "The table in lambda for the chosen fuel; edit it on the left");
    if (!cell && lt_vk_icon_btn(ui, LT_ICON_CHEVRONS_RIGHT, "Hide the preview", 0, 24, 24)) { collapse = 1; }
    w2[0] = -1; w2[1] = cell ? 22 : 150;
    lt_ui_layout_row(ui, 2, w2, 0);
    live_line(app, b, sizeof(b));
    lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
    (void)lt_ui_combo_cb(ui, "lambda_fuel", fuel, LT_LAMBDA_FUELS, fuel_get, NULL);
    lt_ui_layout_row(ui, 1, &w1, -1);
    cw = cell ? 56 : 46;
    lt_ui_disabled_begin(ui, 1);
    if (lt_ui_grid_begin(ui, "lambda_grid", rows, cols, cw, cell ? 16 : 22, 56, cell ? 16 : 24)) {
        int r0, r1, c0, c1, rr, cc;
        if (!cell) { lambda_sync(ui, sync, cw, 22); }
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, "");
        for (cc = c0; cc <= c1; cc++) {
            lt_tabs_fmt(b, sizeof(b), v->x[cc], 0);
            lt_ui_grid_header(ui, 1, cc, b);
        }
        for (rr = r0; rr <= r1; rr++) {
            int dr = ybottom ? rows - 1 - rr : rr;
            lt_tabs_fmt(b, sizeof(b), v->y != NULL ? v->y[dr] : 0.0, 0);
            lt_ui_grid_header(ui, 0, rr, b);
        }
        for (rr = r0; rr <= r1; rr++) {
            for (cc = c0; cc <= c1; cc++) {
                int dr = ybottom ? rows - 1 - rr : rr;
                double z = v->z[(size_t)dr * (size_t)cols + (size_t)cc];
                lt_color bg = lt_color_mix(th->table_cell, lt_tabs_heat(app, th, zlo, zhi, z), 90);
                lt_tabs_fmt(b, sizeof(b), lt_lambda_preview_value(z, shown_l, stoich), shown_l ? 1 : 2);
                lt_ui_grid_cell(ui, rr, cc, b, bg, lt_tabs_heat_text(bg));
            }
        }
        (void)lt_ui_grid_end(ui);
    }
    lt_ui_disabled_end(ui);
    lt_ui_region_end(ui);
    if (sync != NULL && sync->ok && !cell && !lt_ui_popup_open(ui) && lt_ui_wheel(ui) != 0) {
        /* the read-only grid takes no input: the wheel over the preview scrolls the table, and
         * the preview follows it (Shift: sideways) */
        lt_point m = lt_ui_mouse(ui);
        if (m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h) {
            int w = lt_ui_wheel_take(ui), step = lt_ui_row_height(ui) * 2;
            sync->follow = 1;
            sync->fx = sync->msx; sync->fy = sync->msy;
            if (lt_ui_mods(ui) & LT_MOD_SHIFT) { sync->fx -= w * step; } else { sync->fy -= w * step; }
            if (sync->fx < 0) { sync->fx = 0; }
            if (sync->fy < 0) { sync->fy = 0; }
            lt_ui_request_redraw(ui);
        }
    }
    return collapse;
}
