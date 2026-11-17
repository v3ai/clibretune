/* tables_curve.c - curve editor tab: line plot with draggable points plus an editable list.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "tables_priv.h"
#include "view_kit.h"

typedef struct crv_tab {
    lt_tblview v;
    int loaded;
    char msg[160];
    const lt_ini_curve *c;
    int xdig, ydig;
    double ylo, yhi, xlo, xhi;     /* constant limits */
    double px0, px1, py0, py1;     /* plot ranges */
    int drag, drag_moved, prev_down;
    int frame;
    double live_x, live_y;
    int live_ok;
    double *ox, *oz; size_t on;    /* X / Y values at open (Esc reverts the selection to them) */
    unsigned char *selr; size_t selr_cap;   /* per row: bit0 X cell selected, bit1 Y cell selected */
    int cur_r, cur_c;
    int range_mode;                /* 0 auto, 1 fit data, 2 full constant limits */
    int move_x;                    /* drag also moves points horizontally */
    int select_all;
    lt_rect list_area;
    int scale_open, scale_mode; double scale_val;
} crv_tab;

static void crv_load(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const lt_ini_const *xc, *yc;
    lt_tblview_free(&st->v);
    lt_tblview_init(&st->v);
    st->loaded = 0;
    st->c = lt_ini_find_curve(app->ini, tab->target);
    if (st->c == NULL) { lt_snprintf(st->msg, sizeof(st->msg), "Unknown curve '%s'", tab->target); return; }
    if (lt_tblview_load_curve(&st->v, app->ini, st->c, &src) < 0) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load curve: %s", lt_err_get());
        return;
    }
    xc = st->c->x_const != NULL ? lt_ini_find_const(app->ini, st->c->x_const) : NULL;
    yc = st->c->y_const != NULL ? lt_ini_find_const(app->ini, st->c->y_const) : NULL;
    st->xdig = xc != NULL ? xc->digits : 0;
    st->ydig = yc != NULL ? yc->digits : 0;
    st->xlo = xc != NULL ? xc->min : 0.0; st->xhi = xc != NULL ? xc->max : 0.0;
    st->ylo = yc != NULL ? yc->min : 0.0; st->yhi = yc != NULL ? yc->max : 0.0;
    lt_free(st->ox); lt_free(st->oz);
    st->on = st->v.cols;
    st->ox = (double *)lt_calloc(st->on + 1, sizeof(double));
    st->oz = (double *)lt_calloc(st->on + 1, sizeof(double));
    memcpy(st->ox, st->v.x, st->on * sizeof(double));
    memcpy(st->oz, st->v.z, st->on * sizeof(double));
    st->loaded = 1;
}

static void crv_ranges(crv_tab *st)
{
    size_t i;
    double xl = 1e300, xh = -1e300, yl = 1e300, yh = -1e300, pad;
    for (i = 0; i < st->v.cols; i++) {
        if (st->v.x[i] < xl) { xl = st->v.x[i]; }
        if (st->v.x[i] > xh) { xh = st->v.x[i]; }
        if (st->v.z[i] < yl) { yl = st->v.z[i]; }
        if (st->v.z[i] > yh) { yh = st->v.z[i]; }
    }
    if (st->range_mode == 0 && st->c->has_xaxis && st->c->xaxis[1] > st->c->xaxis[0]) { st->px0 = st->c->xaxis[0]; st->px1 = st->c->xaxis[1]; }
    else { pad = (xh - xl) * 0.05; if (pad <= 0) { pad = 1; } st->px0 = xl - pad; st->px1 = xh + pad; }
    if (st->range_mode == 2 && st->yhi > st->ylo) { st->py0 = st->ylo; st->py1 = st->yhi; }
    else if (st->range_mode == 0 && st->c->has_yaxis && st->c->yaxis[1] > st->c->yaxis[0]) { st->py0 = st->c->yaxis[0]; st->py1 = st->c->yaxis[1]; }
    else { pad = (yh - yl) * 0.05; if (pad <= 0) { pad = 1; } st->py0 = yl - pad; st->py1 = yh + pad; }
}

static void crv_commit(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    int n = lt_tblview_commit(&st->v, app->ini, &dst);
    if (n > 0) { lt_app_edited(app); tab->dirty = 1; }
    else if (n < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); }
}

static void crv_check_external(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    const lt_ini_const *yc;
    lt_pagesrc src;
    double *tmp;
    size_t i, n = st->v.cols;
    int diff = 0;
    if (st->drag || lt_tblview_is_dirty(&st->v) || st->c->y_const == NULL) { return; }
    yc = lt_ini_find_const(app->ini, st->c->y_const);
    if (yc == NULL) { return; }
    tmp = (double *)lt_calloc(n + 1, sizeof(double));
    src = lt_app_pages(app);
    if (lt_ini_const_get_array(app->ini, yc, &src, tmp, n) != (int)n) { diff = 1; }
    for (i = 0; i < n && !diff; i++) { if (fabs(tmp[i] - st->v.z[i]) > 1e-9) { diff = 1; } }
    lt_free(tmp);
    if (diff) { crv_load(app, tab, st); }
}

static double clampy(const crv_tab *st, double v)
{
    if (st->yhi > st->ylo) { v = v < st->ylo ? st->ylo : (v > st->yhi ? st->yhi : v); }
    return lt_tabs_round(v, st->ydig);
}

typedef struct plot_ctx { crv_tab *st; lt_rect r; lt_color line, grid, pt, sel, live, axis, bg; int hot, cell, pct; } plot_ctx;

/* plot margins at 100%; scaled by the UI scale kept in plot_ctx.pct */
#define PL_L(p) lt_ui_px_pct(54, (p)->pct)
#define PL_R(p) lt_ui_px_pct(20, (p)->pct)
#define PL_T(p) lt_ui_px_pct(18, (p)->pct)
#define PL_B(p) lt_ui_px_pct(38, (p)->pct)

static double sx_of(const plot_ctx *p, double x)
{
    return p->r.x + PL_L(p) + (x - p->st->px0) / (p->st->px1 - p->st->px0) * (p->r.w - PL_L(p) - PL_R(p));
}
static double sy_of(const plot_ctx *p, double y)
{
    return p->r.y + p->r.h - PL_B(p) - (y - p->st->py0) / (p->st->py1 - p->st->py0) * (p->r.h - PL_T(p) - PL_B(p));
}

static void chip(lt_gfx *g, double cx, double ytop, const char *s, lt_color fg)
{
    lt_font *f = lt_font_get(LT_FONT_MONO_MEDIUM);
    double fp = lt_gfx_fpx(g, 11.0);
    double tw = f != NULL ? lt_gfx_text_measure(f, fp, s) : (double)strlen(s) * 8.0;
    lt_rect c = lt_rect_make((int)(cx - tw / 2.0) - lt_gfx_px(g, 6), (int)ytop, (int)tw + lt_gfx_px(g, 12), lt_gfx_px(g, 18));
    lt_gfx_fill_round_rect(g, c, lt_gfx_px(g, 4), LT_RGBA(0, 0, 0, 215));
    lt_gfx_rect(g, c, LT_RGBA(255, 255, 255, 40));
    lt_gfx_text_bl(g, f, fp, (double)c.x + (double)lt_gfx_px(g, 6), c.y + lt_gfx_px(g, 13), s, fg);
}

static void draw_plot(lt_gfx *g, lt_rect r, void *user)
{
    plot_ctx *p = (plot_ctx *)user;
    crv_tab *st = p->st;
    lt_font *fm = lt_font_get(LT_FONT_MONO), *fs = lt_font_get(LT_FONT_SANS_MEDIUM);
    size_t i;
    int k, x0, x1, y0, y1;
    char b[48];
    double f10 = lt_gfx_fpx(g, 10.0), f11 = lt_gfx_fpx(g, 11.0);
    x0 = (int)sx_of(p, st->px0); x1 = (int)sx_of(p, st->px1);
    y0 = (int)sy_of(p, st->py0); y1 = (int)sy_of(p, st->py1);
    /* plot area */
    lt_gfx_fill_rect_vgrad(g, lt_rect_make(x0, y1, x1 - x0, y0 - y1), p->bg,
                           lt_color_mix(p->bg, LT_RGB(0, 0, 0), 60));
    for (k = 0; k <= 4; k++) {
        double fy = st->py0 + (st->py1 - st->py0) * k / 4.0, fx = st->px0 + (st->px1 - st->px0) * k / 4.0;
        double yy = sy_of(p, fy), xx = sx_of(p, fx);
        lt_gfx_hline(g, x0, x1, (int)yy, p->grid);
        lt_gfx_vline(g, (int)xx, y1, y0, p->grid);
        lt_snprintf(b, sizeof(b), "%g", fy);
        lt_gfx_text_bl(g, fm, f10, (double)x0 - (double)lt_gfx_px(g, 8) - (fm != NULL ? lt_gfx_text_measure(fm, f10, b) : 0.0),
                       (int)yy + lt_gfx_px(g, 4), b, p->axis);
        lt_snprintf(b, sizeof(b), "%g", fx);
        lt_gfx_text_bl(g, fm, f10, xx - (fm != NULL ? lt_gfx_text_measure(fm, f10, b) / 2.0 : 0.0), y0 + lt_gfx_px(g, 16), b, p->axis);
    }
    lt_gfx_rect(g, lt_rect_make(x0, y1, x1 - x0 + 1, y0 - y1 + 1), LT_RGBA(255, 255, 255, 45));
    /* axis captions */
    if (st->c->col_x != NULL) {
        lt_gfx_text_bl(g, fs, f11, (double)(x0 + x1) / 2.0 - lt_gfx_text_measure(fs, f11, st->c->col_x) / 2.0,
                       r.y + r.h - lt_gfx_px(g, 6), st->c->col_x, p->axis);
    }
    if (st->c->col_y != NULL) {
        lt_gfx_text_rot(g, fs, f11, r.x + lt_gfx_px(g, 12), (double)(y0 + y1) / 2.0, st->c->col_y, -1.57079632679, 1, 2, p->axis);
    }
    if (st->v.cols >= 2 && st->v.cols <= 128 && !p->cell) {
        double poly[2 * 132];
        int n = 0;
        for (i = 0; i < st->v.cols; i++) {
            poly[n * 2] = sx_of(p, st->v.x[i]); poly[n * 2 + 1] = sy_of(p, st->v.z[i]); n++;
        }
        poly[n * 2] = sx_of(p, st->v.x[st->v.cols - 1]); poly[n * 2 + 1] = (double)y0; n++;
        poly[n * 2] = sx_of(p, st->v.x[0]); poly[n * 2 + 1] = (double)y0; n++;
        lt_gfx_fill_poly(g, poly, n, (p->line & 0x00FFFFFFUL) | 0x28000000UL);
    }
    for (i = 0; i + 1 < st->v.cols; i++) {
        double ax = sx_of(p, st->v.x[i]), ay = sy_of(p, st->v.z[i]);
        double bx = sx_of(p, st->v.x[i + 1]), by = sy_of(p, st->v.z[i + 1]);
        lt_gfx_line(g, ax, ay, bx, by, 6.0, (p->line & 0x00FFFFFFUL) | 0x30000000UL);
        lt_gfx_line(g, ax, ay, bx, by, 2.2, p->line);
    }
    for (i = 0; i < st->v.cols; i++) {
        double px = sx_of(p, st->v.x[i]), py = sy_of(p, st->v.z[i]);
        int hot = (int)i == p->hot;
        if (hot) { lt_gfx_fill_circle(g, px, py, 11.0, (p->sel & 0x00FFFFFFUL) | 0x30000000UL); }
        lt_gfx_fill_circle(g, px, py, hot ? 6.5 : 5.0, LT_RGBA(0, 0, 0, 120));
        lt_gfx_fill_circle(g, px, py, hot ? 5.5 : 4.0, hot ? p->sel : p->pt);
        lt_gfx_circle(g, px, py, hot ? 5.5 : 4.0, 1.2, LT_RGB(255, 255, 255));
    }
    if (p->hot >= 0 && (size_t)p->hot < st->v.cols) {
        char vb[48];
        lt_tabs_fmt(b, sizeof(b), st->v.x[p->hot], st->xdig);
        lt_tabs_fmt(vb, sizeof(vb), st->v.z[p->hot], st->ydig);
        lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), ", %s", vb);
        chip(g, sx_of(p, st->v.x[p->hot]), sy_of(p, st->v.z[p->hot]) - 30.0, b, LT_RGB(255, 255, 255));
    }
    if (st->live_ok) {
        double lx = sx_of(p, st->live_x), ly = sy_of(p, st->live_y);
        lt_gfx_line(g, lx, (double)y1, lx, (double)y0, 1.2, (p->live & 0x00FFFFFFUL) | 0x90000000UL);
        lt_gfx_fill_circle(g, lx, ly, 10.0, (p->live & 0x00FFFFFFUL) | 0x30000000UL);
        lt_gfx_fill_circle(g, lx, ly, 5.0, p->live);
        lt_gfx_circle(g, lx, ly, 5.5, 1.2, LT_RGB(255, 255, 255));
        lt_tabs_fmt(b, sizeof(b), st->live_y, st->ydig);
        chip(g, lx, ly + 12.0, b, p->live);
    }
}

static void crv_live(lt_app *app, crv_tab *st)
{
    double v;
    size_t i;
    st->live_ok = 0;
    if (st->c->x_channel == NULL || st->v.cols < 2 || app->rt == NULL) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return; }
    if (!lt_app_channel(app, st->c->x_channel, &v)) { return; }
    for (i = 0; i + 2 < st->v.cols && v > st->v.x[i + 1]; i++) { }
    {
        double x0 = st->v.x[i], x1 = st->v.x[i + 1], f = x1 > x0 ? (v - x0) / (x1 - x0) : 0.0;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        st->live_x = v;
        st->live_y = st->v.z[i] + (st->v.z[i + 1] - st->v.z[i]) * f;
        st->live_ok = 1;
    }
}

/* ---- editing: the same operation set as the table editor, on the X or Y column ---- */
enum { C_EQUAL = 1, C_ADD, C_SCALE, C_SET, C_INTERP, C_SMOOTH, C_CLEAR, C_RESTORE };

/* column the operations act on: X when only X cells are selected, else Y */
static int crv_target_x(const crv_tab *st)
{
    size_t i;
    int any = 0;
    for (i = 0; i < st->v.cols && i < st->selr_cap; i++) {
        if (st->selr[i] & 2) { return 0; }
        if (st->selr[i] & 1) { any = 1; }
    }
    return any;
}

static size_t crv_sel_cells(const crv_tab *st, int xcol, lt_tcell *out)
{
    size_t i, n = 0;
    for (i = 0; i < st->v.cols && i < st->selr_cap; i++) {
        if (st->selr[i] & (xcol ? 1 : 2)) { out[n].y = 0; out[n].x = i; n++; }
    }
    if (n == 0 && st->cur_r >= 0 && (size_t)st->cur_r < st->v.cols) {
        out[0].y = 0; out[0].x = (size_t)st->cur_r; n = 1;
    }
    return n;
}

static void crv_op(lt_app *app, lt_app_tab *tab, crv_tab *st, int op, double arg)
{
    size_t cols = st->v.cols, n, i, changed = 0;
    int xcol = crv_target_x(st);
    double *cur = xcol ? st->v.x : st->v.z, *tmp, lo = xcol ? st->xlo : st->ylo,
           hi = xcol ? st->xhi : st->yhi;
    int dig = xcol ? st->xdig : st->ydig;
    lt_tcell *sel = (lt_tcell *)lt_calloc(cols + 1, sizeof(lt_tcell));
    n = crv_sel_cells(st, xcol, sel);
    tmp = (double *)lt_calloc(cols + 1, sizeof(double));
    memcpy(tmp, cur, cols * sizeof(double));
    switch (op) {
    case C_EQUAL: {
        double sum = 0.0;
        for (i = 0; i < n; i++) { sum += cur[sel[i].x]; }
        lt_tbl_set_equal(tmp, 1, cols, sel, n, lt_tabs_round(sum / (double)n, dig));
        break; }
    case C_ADD: lt_tbl_offset(tmp, 1, cols, sel, n, arg); break;
    case C_SCALE: lt_tbl_scale(tmp, 1, cols, sel, n, arg); break;
    case C_SET: lt_tbl_set_equal(tmp, 1, cols, sel, n, arg); break;
    case C_INTERP: lt_tbl_interp_linear(tmp, 1, cols, sel, n, LT_TBL_AXIS_ROW); break;
    case C_SMOOTH: lt_tbl_smooth(tmp, 1, cols, sel, n, 1.0); break;
    case C_CLEAR: lt_tbl_clear(tmp, 1, cols, sel, n, lo, hi); break;
    case C_RESTORE:
        if (st->on == cols) { lt_tbl_restore(tmp, 1, cols, sel, n, xcol ? st->ox : st->oz); }
        break;
    default: break;
    }
    for (i = 0; i < n; i++) {
        double v = tmp[sel[i].x];
        if (hi > lo) { v = v < lo ? lo : (v > hi ? hi : v); }
        v = lt_tabs_round(v, dig);
        if (v != cur[sel[i].x]) { changed++; }
        tmp[sel[i].x] = v;
    }
    if (changed > 0) {
        lt_tblview_push_undo(&st->v);
        for (i = 0; i < n; i++) {
            if (xcol) { lt_tblview_set_x(&st->v, sel[i].x, tmp[sel[i].x]); }
            else { lt_tblview_set(&st->v, 0, sel[i].x, tmp[sel[i].x]); }
        }
        crv_commit(app, tab, st);
    }
    lt_free(tmp);
    lt_free(sel);
}

static void crv_copy(lt_app *app, crv_tab *st)
{
    lt_buf b;
    char t1[48], t2[48];
    size_t i, k = 0;
    int both = 0;
    lt_buf_init(&b);
    for (i = 0; i < st->v.cols && i < st->selr_cap; i++) {
        if (st->selr[i] & 1) { both |= 1; }
        if (st->selr[i] & 2) { both |= 2; }
    }
    for (i = 0; i < st->v.cols; i++) {
        int sx = i < st->selr_cap && (st->selr[i] & 1), sy = i < st->selr_cap && (st->selr[i] & 2);
        if (!(sx || sy) && !(both == 0 && (int)i == st->cur_r)) { continue; }
        lt_tabs_fmt(t1, sizeof(t1), st->v.x[i], st->xdig);
        lt_tabs_fmt(t2, sizeof(t2), st->v.z[i], st->ydig);
        if (both == 3 || (sx && sy)) { lt_buf_puts(&b, t1); lt_buf_puts(&b, "\t"); lt_buf_puts(&b, t2); }
        else if (sx) { lt_buf_puts(&b, t1); }
        else { lt_buf_puts(&b, t2); }
        lt_buf_puts(&b, "\n");
        k++;
    }
    lt_ui_clipboard_set(app->ui, b.data != NULL ? (const char *)b.data : "");
    lt_buf_free(&b);
    lt_app_toast(app, 0, "Copied %lu points", (unsigned long)k);
}

/* paste lines of 1 or 2 numbers starting at the cursor row / column */
static void crv_paste(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    char *txt = lt_ui_clipboard_get(app->ui);
    const char *p = txt;
    size_t row = st->cur_r >= 0 ? (size_t)st->cur_r : 0, put = 0;
    int c0 = st->cur_c > 0 ? 1 : 0, pushed = 0;
    if (txt == NULL || txt[0] == '\0') {
        lt_free(txt);
        lt_app_toast(app, 2, "Clipboard is empty");
        return;
    }
    while (*p != '\0' && row < st->v.cols) {
        char line[256];
        double vals[4];
        size_t k = 0;
        int cnt, j;
        while (*p != '\0' && *p != '\n') { if (k + 1 < sizeof(line)) { line[k++] = *p; } p++; }
        if (*p == '\n') { p++; }
        line[k] = '\0';
        cnt = lt_tabs_parse_list(line, vals, 4);
        if (cnt <= 0) { continue; }
        if (!pushed) { lt_tblview_push_undo(&st->v); pushed = 1; }
        for (j = 0; j < cnt && c0 + j < 2; j++) {
            if (c0 + j == 0) { lt_tblview_set_x(&st->v, row, lt_tabs_round(vals[j], st->xdig)); }
            else { lt_tblview_set(&st->v, 0, row, clampy(st, vals[j])); }
        }
        row++; put++;
    }
    lt_free(txt);
    if (pushed) { crv_commit(app, tab, st); }
    lt_app_toast(app, 0, "Pasted %lu points", (unsigned long)put);
}

static void crv_undo_redo(lt_app *app, lt_app_tab *tab, crv_tab *st, int redo)
{
    if (redo ? lt_tblview_redo(&st->v) : lt_tblview_undo(&st->v)) { crv_commit(app, tab, st); }
}

static void crv_keys(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_ui *ui = app->ui;
    double ss = 1.0;
    int i;
    if (lt_ui_text_focused(ui) || lt_ui_popup_open(ui) || st->drag >= 0) { return; }
    for (i = 0; i < st->ydig && i < 2; i++) { ss /= 10.0; }
    if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL)) { crv_undo_redo(app, tab, st, 0); }
    if (lt_ui_key_pressed(ui, 'y', LT_MOD_CTRL) || lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL | LT_MOD_SHIFT)) {
        crv_undo_redo(app, tab, st, 1);
    }
    if (lt_ui_key_pressed(ui, 'c', LT_MOD_CTRL)) { crv_copy(app, st); }
    if (lt_ui_key_pressed(ui, 'v', LT_MOD_CTRL)) { crv_paste(app, tab, st); }
    if (lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0)) { crv_op(app, tab, st, C_RESTORE, 0); }
    if (lt_ui_key_pressed(ui, LT_KEY_DELETE, 0) || lt_ui_key_pressed(ui, LT_KEY_BACKSPACE, 0)) {
        crv_op(app, tab, st, C_CLEAR, 0);
    }
    if (lt_ui_key_pressed(ui, '=', 0)) { crv_op(app, tab, st, C_EQUAL, 0); }
    if (lt_ui_key_pressed(ui, '=', LT_MOD_SHIFT) || lt_tabs_key(ui, '+') || lt_tabs_key(ui, '>')) {
        crv_op(app, tab, st, C_ADD, 1.0);
    }
    if (lt_tabs_key(ui, '-') || lt_tabs_key(ui, '_') || lt_tabs_key(ui, '<') || lt_ui_key_pressed(ui, ',', 0)) {
        crv_op(app, tab, st, C_ADD, -1.0);
    }
    if (lt_tabs_key(ui, 'q')) { crv_op(app, tab, st, C_ADD, ss); }
    if (lt_tabs_key(ui, 'w')) { crv_op(app, tab, st, C_ADD, -ss); }
    if (lt_tabs_key(ui, '*')) { st->scale_open = 1; st->scale_mode = 0; }
    if (lt_tabs_key(ui, '/') || lt_tabs_key(ui, 'h') || lt_tabs_key(ui, 'v')) { crv_op(app, tab, st, C_INTERP, 0); }
    if (lt_tabs_key(ui, 's')) { crv_op(app, tab, st, C_SMOOTH, 0); }
}

static const char *const crv_range_names[3] = { "Auto", "Fit data", "Full limits" };

enum { CB_UNDO, CB_REDO, CB_INTERP, CB_SMOOTH, CB_SCALE, CB_COPY, CB_PASTE, CB_RANGE, CB_MOVEX,
       CB_BURN, CB_N };

static void crv_toolbar_items(lt_app *app, crv_tab *st, lt_vk_item *it, int cell)
{
    static char rtip[96];
    int n = 0;
    memset(it, 0, (size_t)(CB_N + 4) * sizeof(*it));
#define CB(cmd, ic, lbl, tp) do { it[n].kind = LT_VK_BTN; it[n].icon = (ic); \
    it[n].label = cell ? (lbl) : NULL; it[n].tip = (tp); n++; LT_UNUSED(cmd); } while (0)
    CB(CB_UNDO, LT_ICON_UNDO_2, "Undo", "Undo (Ctrl+Z)");
    CB(CB_REDO, LT_ICON_REDO_2, "Redo", "Redo (Ctrl+Y)");
    it[n].kind = LT_VK_SEP; n++;
    CB(CB_INTERP, LT_ICON_TRENDING_UP, "Interp", "Interpolate between the selection ends (/)");
    CB(CB_SMOOTH, LT_ICON_SPARKLES, "Smooth", "Smooth the selected points (s)");
    CB(CB_SCALE, LT_ICON_X, "Scale", "Multiply, add or set the selected values (*)");
    CB(CB_COPY, LT_ICON_COPY, "Copy", "Copy the selected points (Ctrl+C)");
    CB(CB_PASTE, LT_ICON_CLIPBOARD_PASTE, "Paste", "Paste at the cursor (Ctrl+V)");
    it[n].kind = LT_VK_SEP; n++;
    lt_snprintf(rtip, sizeof(rtip), "Y axis range: %s (click to change)", crv_range_names[st->range_mode]);
    CB(CB_RANGE, LT_ICON_ARROW_UP_DOWN, "Range", rtip);
    CB(CB_MOVEX, LT_ICON_MOVE_HORIZONTAL, "MoveX", "Dragging a point also moves it horizontally");
    it[n - 1].on = st->move_x;
    it[n].kind = LT_VK_SEP; n++;
    CB(CB_BURN, LT_ICON_FLAME, "Burn", "Write the changes to ECU flash");
    it[n - 1].accent = 1;
    it[n - 1].disabled = lt_app_can_burn(app) ? 0 : 1;
    it[n].kind = LT_VK_END;
#undef CB
}

/* map toolbar item index to command (separators are skipped by the toolkit's index) */
static int crv_toolbar_cmd(int hit)
{
    static const int cmds[16] = { CB_UNDO, CB_REDO, -1, CB_INTERP, CB_SMOOTH, CB_SCALE, CB_COPY,
                                  CB_PASTE, -1, CB_RANGE, CB_MOVEX, -1, CB_BURN, -1, -1, -1 };
    return hit >= 0 && hit < 16 ? cmds[hit] : -1;
}

static void crv_command(lt_app *app, lt_app_tab *tab, crv_tab *st, int cmd)
{
    switch (cmd) {
    case CB_UNDO: crv_undo_redo(app, tab, st, 0); break;
    case CB_REDO: crv_undo_redo(app, tab, st, 1); break;
    case CB_INTERP: crv_op(app, tab, st, C_INTERP, 0); break;
    case CB_SMOOTH: crv_op(app, tab, st, C_SMOOTH, 0); break;
    case CB_SCALE: st->scale_open = 1; st->scale_mode = 0; break;
    case CB_COPY: crv_copy(app, st); break;
    case CB_PASTE: crv_paste(app, tab, st); break;
    case CB_RANGE: st->range_mode = (st->range_mode + 1) % 3; break;
    case CB_MOVEX: st->move_x = !st->move_x; break;
    case CB_BURN: lt_app_burn(app); tab->dirty = 0; break;
    default: break;
    }
}

static void crv_context(lt_app *app, lt_app_tab *tab, crv_tab *st, lt_rect area)
{
    lt_ui *ui = app->ui;
    if (!lt_ui_context_begin(ui, "crv_ctx", area)) { return; }
    if (lt_ui_menu_item(ui, "Reset to original", "Esc", 0)) { crv_op(app, tab, st, C_RESTORE, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Set equal (average)", "=", 0)) { crv_op(app, tab, st, C_EQUAL, 0); }
    if (lt_ui_menu_item(ui, "Set value...", NULL, 0)) { st->scale_open = 1; st->scale_mode = 2; }
    if (lt_ui_menu_item(ui, "Step up", "> or +", 0)) { crv_op(app, tab, st, C_ADD, 1.0); }
    if (lt_ui_menu_item(ui, "Step down", "< or -", 0)) { crv_op(app, tab, st, C_ADD, -1.0); }
    if (lt_ui_menu_item(ui, "Add amount...", NULL, 0)) { st->scale_open = 1; st->scale_mode = 1; }
    if (lt_ui_menu_item(ui, "Multiply by...", "*", 0)) { st->scale_open = 1; st->scale_mode = 0; }
    if (lt_ui_menu_item(ui, "Clear", "Del", 0)) { crv_op(app, tab, st, C_CLEAR, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Interpolate", "/", 0)) { crv_op(app, tab, st, C_INTERP, 0); }
    if (lt_ui_menu_item(ui, "Smooth", "S", 0)) { crv_op(app, tab, st, C_SMOOTH, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Copy", "Ctrl+C", 0)) { crv_copy(app, st); }
    if (lt_ui_menu_item(ui, "Paste", "Ctrl+V", 0)) { crv_paste(app, tab, st); }
    if (lt_ui_menu_item(ui, "Select all", "Ctrl+A", 0)) { st->select_all = 1; }
    lt_ui_context_end(ui);
}

static void crv_dialogs(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_ui *ui = app->ui;
    int w1 = -1, w2[2], wb[3];
    static const char *const modes[3] = { "Multiply by", "Add", "Set to" };
    if (!st->scale_open || !lt_ui_modal_begin(ui, "Adjust selection", 360, 150, &st->scale_open)) { return; }
    w2[0] = 110; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_combo(ui, "cmode", &st->scale_mode, modes, 3);
    lt_ui_number(ui, "cval", &st->scale_val, -100000.0, 100000.0, 0.05, 3);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label(ui, "Applies to the selected points, clamped to the allowed range.");
    wb[0] = -1; wb[1] = 90; wb[2] = 90;
    lt_ui_layout_row(ui, 3, wb, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_DEFAULT)) {
        crv_op(app, tab, st, st->scale_mode == 0 ? C_SCALE : (st->scale_mode == 1 ? C_ADD : C_SET), st->scale_val);
        st->scale_open = 0;
    }
    if (lt_ui_button(ui, "Cancel")) { st->scale_open = 0; }
    lt_ui_modal_end(ui);
}

static void crv_draw(lt_app *app, lt_app_tab *tab, lt_rect rr)
{
    crv_tab *st = (crv_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2];
    static plot_ctx pc;
    lt_rect pr;
    lt_point m;
    int down, inside, i;
    LT_UNUSED(rr);
    if (st == NULL) {
        st = (crv_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        st->drag = -1;
        st->scale_val = 1.05;
        crv_load(app, tab, st);
    }
    st->frame++;
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, tab->title);
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    if (st->frame % 15 == 0) { crv_check_external(app, tab, st); }
    if (!st->loaded) { return; }
    crv_live(app, st);
    crv_ranges(st);
    crv_keys(app, tab, st);
    /* header + toolbar */
    {
        lt_vk_item it[CB_N + 4];
        int hit;
        crv_toolbar_items(app, st, it, lt_ui_cell_mode(ui));
        if (lt_ui_cell_mode(ui)) {
            hit = lt_vk_toolbar(ui, it, 0);
        } else {
            lt_rect hr;
            lt_ui_layout_row(ui, 1, &w1, 40);
            hr = lt_ui_layout_next(ui);
            lt_ui_draw_fill(ui, hr, th->bg_app);
            lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, th->border_subtle);
            lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 14), hr.y, hr.w / 3, hr.h), (int)LT_FONT_SANS_SEMIBOLD, 14.0, tab->title, th->text, 0);
            /* items are right aligned inside the right two thirds */
            {
                lt_vk_item *sp = (lt_vk_item *)lt_calloc(CB_N + 6, sizeof(lt_vk_item));
                int k = 0, j = 0;
                sp[k].kind = LT_VK_SPACER; k++;
                while (it[j].kind != LT_VK_END) { sp[k++] = it[j++]; }
                sp[k].kind = LT_VK_END;
                hit = lt_vk_toolbar_in(ui, sp, lt_rect_make(hr.x + hr.w / 3, hr.y + lt_ui_px(ui, 5), hr.w * 2 / 3 - lt_ui_px(ui, 12), lt_ui_px(ui, 30)));
                lt_free(sp);
                if (hit >= 0) { hit -= 1; }   /* drop the spacer */
            }
        }
        if (hit >= 0) { crv_command(app, tab, st, crv_toolbar_cmd(hit)); }
    }
    w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, lt_ui_cell_mode(ui) ? 0 : 22);
    lt_ui_label_ex(ui, "  Drag a point vertically (MoveX also moves it sideways); edit values in the list", th->muted, LT_UI_ALIGN_LEFT);
    w2[0] = -3; w2[1] = -2;
    lt_ui_layout_row(ui, 2, w2, -1);
    pr = lt_ui_layout_next(ui);
    /* interaction */
    m = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0;
    inside = m.x >= pr.x && m.x < pr.x + pr.w && m.y >= pr.y && m.y < pr.y + pr.h;
    pc.st = st; pc.r = pr;
    pc.line = th->primary_hover; pc.grid = LT_RGBA(255, 255, 255, th->dark ? 22 : 0);
    if (!th->dark) { pc.grid = LT_RGBA(0, 0, 0, 28); }
    pc.pt = th->accent; pc.sel = th->warning; pc.live = th->cursor_live; pc.axis = th->text2;
    pc.bg = th->table_bg; pc.cell = lt_ui_cell_mode(ui); pc.pct = lt_ui_scale_pct(ui);
    pc.hot = st->drag;
    if (st->drag < 0) {
        double best = 100.0;
        pc.hot = -1;
        for (i = 0; i < (int)st->v.cols && inside; i++) {
            double dx = sx_of(&pc, st->v.x[i]) - m.x, dy = sy_of(&pc, st->v.z[i]) - m.y, d = dx * dx + dy * dy;
            if (d < best) { best = d; pc.hot = i; }
        }
        if (down && !st->prev_down && pc.hot >= 0 && !lt_ui_popup_open(ui)) {
            st->drag = pc.hot; st->drag_moved = 0;
            lt_tblview_push_undo(&st->v);
        }
    }
    if (st->drag >= 0) {
        if (!down) {
            if (st->drag_moved) { crv_commit(app, tab, st); }
            st->drag = -1;
        } else {
            double ny = st->py0 + (sy_of(&pc, st->py0) - m.y) / (sy_of(&pc, st->py0) - sy_of(&pc, st->py1)) * (st->py1 - st->py0);
            lt_tblview_set(&st->v, 0, (size_t)st->drag, clampy(st, ny));
            if (st->move_x) {
                double fx = (m.x - sx_of(&pc, st->px0)) / (sx_of(&pc, st->px1) - sx_of(&pc, st->px0));
                double nx = st->px0 + fx * (st->px1 - st->px0);
                nx = lt_tbl_clamp_between(st->v.x, st->v.cols, (size_t)st->drag, nx);
                if (st->xhi > st->xlo) { nx = nx < st->xlo ? st->xlo : (nx > st->xhi ? st->xhi : nx); }
                lt_tblview_set_x(&st->v, (size_t)st->drag, lt_tabs_round(nx, st->xdig));
            }
            st->drag_moved = 1;
            pc.hot = st->drag;
        }
    }
    st->prev_down = down;
    lt_ui_draw_box(ui, pr, th->table_bg, th->border_subtle, 0);
    lt_ui_draw_custom(ui, pr, draw_plot, &pc);
    /* value list */
    {
        int rhw = 40, cw = 78, ch = lt_ui_cell_mode(ui) ? 16 : 22;
        size_t need = st->v.cols + 1;
        if (need > st->selr_cap) {
            lt_free(st->selr);
            st->selr = (unsigned char *)lt_calloc(need, 1);
            st->selr_cap = need;
        }
        if (lt_ui_grid_begin(ui, "list", (int)st->v.cols, 2, cw, ch, rhw, lt_ui_cell_mode(ui) ? 16 : 24)) {
            int r0, r1, c0, c1, r, c, fl, cr, cc;
            char b[32];
            st->list_area = lt_ui_clip_get(ui);
            lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
            lt_ui_grid_corner(ui, "#");
            lt_ui_grid_header(ui, 1, 0, st->c->col_x != NULL ? st->c->col_x : "X");
            lt_ui_grid_header(ui, 1, 1, st->c->col_y != NULL ? st->c->col_y : "Y");
            for (r = r0; r <= r1; r++) {
                lt_snprintf(b, sizeof(b), "%d", r + 1);
                lt_ui_grid_header(ui, 0, r, b);
                for (c = 0; c < 2; c++) {
                    double v = c == 0 ? st->v.x[r] : st->v.z[r];
                    lt_color bg = (r == st->drag || r == pc.hot) ? th->table_selected : th->table_cell;
                    lt_tabs_fmt(b, sizeof(b), v, c == 0 ? st->xdig : st->ydig);
                    lt_ui_grid_cell(ui, r, c, b, bg, th->text);
                }
            }
            if (st->select_all) {
                lt_ui_grid_select(ui, 0, 0, (int)st->v.cols - 1, 1);
                st->select_all = 0;
            }
            memset(st->selr, 0, st->selr_cap);
            for (r = 0; r < (int)st->v.cols; r++) {
                if (lt_ui_grid_selected(ui, r, 0)) { st->selr[r] |= 1; }
                if (lt_ui_grid_selected(ui, r, 1)) { st->selr[r] |= 2; }
            }
            lt_ui_grid_cursor(ui, &cr, &cc);
            st->cur_r = cr; st->cur_c = cc;
            fl = lt_ui_grid_end(ui);
            if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
                int er, ec;
                lt_ui_grid_editing(ui, &er, &ec);
                lt_tabs_fmt(b, sizeof(b), ec == 0 ? st->v.x[er] : st->v.z[er], ec == 0 ? st->xdig : st->ydig);
                lt_ui_grid_edit_set_text(ui, b);
            }
            if (fl & LT_UI_GRID_EDIT_COMMIT) {
                double v;
                int er, ec;
                lt_ui_grid_cursor(ui, &er, &ec);
                if (lt_parse_double(lt_ui_grid_edit_text(ui), &v) && er >= 0 && ec >= 0) {
                    lt_tblview_push_undo(&st->v);
                    if (ec == 0) {
                        if (st->xhi > st->xlo) { v = v < st->xlo ? st->xlo : (v > st->xhi ? st->xhi : v); }
                        lt_tblview_set_x(&st->v, (size_t)er, lt_tabs_round(v, st->xdig));
                    } else { lt_tblview_set(&st->v, 0, (size_t)er, clampy(st, v)); }
                    crv_commit(app, tab, st);
                } else { lt_app_toast(app, 2, "Not a number"); }
            }
            crv_context(app, tab, st, st->list_area);
        }
    }
    crv_dialogs(app, tab, st);
}

static void crv_close(lt_app *app, lt_app_tab *tab)
{
    crv_tab *st = (crv_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    lt_tblview_free(&st->v);
    lt_free(st->ox); lt_free(st->oz); lt_free(st->selr);
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_tabs_kind_curve = { "curve", crv_draw, crv_close };
