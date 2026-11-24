/* dialogs_embed.c - components embedded in INI dialogs: compact curve editor (live plot with
 * draggable points + X/Y value grid), compact heat-mapped table editor, indicator panels,
 * readout panels and gauges.  Edits go through the same path as the editor tabs: an
 * lt_tblview loaded from lt_app_pages(), committed back with lt_tblview_commit and
 * lt_app_edited (dirty, sent to the ECU RAM, burn), with the view's undo ring.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "dialogs_embed.h"
#include "tables_priv.h"
#include "view_kit.h"
#include "dash_gauges.h"
#include "iniext.h"

typedef struct emb {
    int kind;
    const void *key;
    lt_tblview v;
    int loaded;
    char msg[120];
    int frame;
    int xdig, ydig, zdig;           /* digits: X axis, Y axis (tables), values */
    double lo, hi, xlo, xhi;        /* value / X limits (display units); hi <= lo = none */
    double px0, px1, py0, py1;      /* curve plot ranges */
    int sel, drag, drag_moved, prev_down, hot, sync, drag_y0;
    double live_x, live_y;
    int live_ok;
    unsigned char *gsel; size_t gsel_cap;   /* curve: per point bit0 X / bit1 Y; table: per cell */
    unsigned char *esel;            /* table: gsel when the cell edit began (gsel_cap bytes) */
    int nsel;
    lt_rect pr;
    lt_color line, grid, pt, selc, live, axis, bg;
    int cell, pct;
    lt_dash_gauge g;
    int g_init;
    double gval;
} emb;

void lt_emb_free(lt_embeds *e)
{
    int i;
    for (i = 0; i < e->n; i++) {
        emb *m = (emb *)e->v[i];
        lt_tblview_free(&m->v);
        if (m->g_init) { lt_dash_gauge_free(&m->g); }
        lt_free(m->gsel);
        lt_free(m->esel);
        lt_free(m);
    }
    lt_free(e->v);
    e->v = NULL;
    e->n = 0;
}

static emb *emb_get(lt_embeds *e, int kind, const void *key)
{
    int i;
    emb *m;
    for (i = 0; i < e->n; i++) {
        m = (emb *)e->v[i];
        if (m->kind == kind && m->key == key) { return m; }
    }
    m = (emb *)lt_calloc(1, sizeof(emb));
    m->kind = kind;
    m->key = key;
    m->drag = -1;
    m->hot = -1;
    lt_tblview_init(&m->v);
    e->v = (void **)lt_realloc(e->v, (size_t)(e->n + 1) * sizeof(void *));
    e->v[e->n++] = m;
    return m;
}

int lt_emb_kind(lt_app *app, const char *name)
{
    if (app->ini == NULL || name == NULL) { return LT_EMB_NONE; }
    if (lt_ini_find_dialog(app->ini, name) != NULL) { return LT_EMB_DIALOG; }
    if (lt_ini_find_curve(app->ini, name) != NULL) { return LT_EMB_CURVE; }
    if (lt_ini_find_table(app->ini, name) != NULL) { return LT_EMB_TABLE; }
    if (lt_ini_find_ind_panel(app->ini, name) != NULL) { return LT_EMB_IND; }
    if (lt_ini_find_readout_panel(app->ini, name) != NULL) { return LT_EMB_READOUT; }
    if (lt_ini_find_gauge(app->ini, name) != NULL) { return LT_EMB_GAUGE; }
    if (lt_ini_std_panel(app->ini, name) != NULL) { return LT_EMB_DIALOG; }
    return LT_EMB_NONE;
}

/* ------------------------------------------------------------------ */
/* shared: load, commit, follow external changes                       */
static int emb_load_into(lt_app *app, emb *m, lt_tblview *v)
{
    lt_pagesrc src = lt_app_pages(app);
    if (m->kind == LT_EMB_CURVE) { return lt_tblview_load_curve(v, app->ini, (const lt_ini_curve *)m->key, &src); }
    return lt_tblview_load_table(v, app->ini, (const lt_ini_table *)m->key, &src);
}

static void emb_limits(lt_app *app, emb *m)
{
    const lt_ini_const *xc = NULL, *yc = NULL, *zc = NULL;
    if (m->kind == LT_EMB_CURVE) {
        const lt_ini_curve *c = (const lt_ini_curve *)m->key;
        xc = c->x_const != NULL ? lt_ini_find_const(app->ini, c->x_const) : NULL;
        zc = c->y_const != NULL ? lt_ini_find_const(app->ini, c->y_const) : NULL;
    } else {
        const lt_ini_table *t = (const lt_ini_table *)m->key;
        xc = t->x_const != NULL ? lt_ini_find_const(app->ini, t->x_const) : NULL;
        yc = t->y_const != NULL ? lt_ini_find_const(app->ini, t->y_const) : NULL;
        zc = t->z_const != NULL ? lt_ini_find_const(app->ini, t->z_const) : NULL;
    }
    m->xdig = lt_uconv_digits(&m->v.xu, xc != NULL ? xc->digits : 0);
    m->ydig = lt_uconv_digits(&m->v.yu, yc != NULL ? yc->digits : 0);
    m->zdig = lt_uconv_digits(&m->v.zu, zc != NULL ? zc->digits : 0);
    m->xlo = xc != NULL ? xc->min : 0.0; m->xhi = xc != NULL ? xc->max : 0.0;
    m->lo = zc != NULL ? zc->min : 0.0; m->hi = zc != NULL ? zc->max : 0.0;
    lt_uconv_range(&m->v.xu, &m->xlo, &m->xhi);
    lt_uconv_range(&m->v.zu, &m->lo, &m->hi);
}

static void emb_ensure(lt_app *app, emb *m)
{
    size_t need;
    if (!m->loaded) {
        lt_tblview_free(&m->v);
        lt_tblview_init(&m->v);
        if (emb_load_into(app, m, &m->v) < 0) {
            lt_snprintf(m->msg, sizeof(m->msg), "Not available: %s", lt_err_get());
            return;
        }
        m->loaded = 1;
        emb_limits(app, m);
    } else if (lt_tblview_units_sync(&m->v)) {
        emb_limits(app, m);
    }
    need = m->v.rows * m->v.cols + 1;
    if (need > m->gsel_cap) {
        lt_free(m->gsel);
        lt_free(m->esel);
        m->gsel = (unsigned char *)lt_calloc(need, 1);
        m->esel = (unsigned char *)lt_calloc(need, 1);
        m->gsel_cap = need;
    }
}

/* the page changed behind our back (editor tab, autotune, tune load): reload */
static void emb_follow(lt_app *app, emb *m)
{
    lt_tblview t;
    size_t k, nz;
    int same;
    if (!m->loaded || m->drag >= 0 || lt_tblview_is_dirty(&m->v) || (m->frame++ % 20) != 0) { return; }
    lt_tblview_init(&t);
    if (emb_load_into(app, m, &t) < 0) { lt_tblview_free(&t); return; }
    lt_tblview_units_sync(&t);
    same = t.rows == m->v.rows && t.cols == m->v.cols;
    nz = t.rows * t.cols;
    for (k = 0; same && k < nz; k++) { if (fabs(t.z[k] - m->v.z[k]) > 1e-9) { same = 0; } }
    for (k = 0; same && k < t.cols; k++) { if (fabs(t.x[k] - m->v.x[k]) > 1e-9) { same = 0; } }
    for (k = 0; same && t.y != NULL && m->v.y != NULL && k < t.rows; k++) {
        if (fabs(t.y[k] - m->v.y[k]) > 1e-9) { same = 0; }
    }
    if (same) { lt_tblview_free(&t); return; }
    lt_tblview_free(&m->v);
    m->v = t;
    emb_limits(app, m);
}

static void emb_commit(lt_app *app, emb *m)
{
    lt_pagesrc dst = lt_app_pages(app);
    int n;
    (void)lt_tabs_check_change(app, &m->v);
    n = lt_tblview_commit(&m->v, app->ini, &dst);
    if (n > 0) { lt_app_edited(app); }
    else if (n < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); }
}

static double clampz(const emb *m, double v)
{
    if (m->hi > m->lo) { v = v < m->lo ? m->lo : (v > m->hi ? m->hi : v); }
    return lt_tabs_round(v, m->zdig);
}

static double step_of(int digits)
{
    double s = 1.0;
    int i;
    for (i = 0; i < digits && i < 6; i++) { s /= 10.0; }
    return s;
}

/* narrowest grid column that shows every value (and axis value) */
static int emb_cell_w(const emb *m, int cell)
{
    size_t k, n = m->v.rows * m->v.cols;
    char b[48];
    int w = 3;
    for (k = 0; k < n; k++) {
        lt_tabs_fmt(b, sizeof(b), m->v.z[k], m->zdig);
        if ((int)strlen(b) > w) { w = (int)strlen(b); }
    }
    for (k = 0; k < m->v.cols; k++) {
        lt_tabs_fmt(b, sizeof(b), m->v.x[k], m->xdig);
        if ((int)strlen(b) > w) { w = (int)strlen(b); }
    }
    return w * 8 + (cell ? 8 : 12);
}

/* toolbar under an embedded editor; returns the id of the clicked control (0 none) */
#define TB_MINUS 1
#define TB_PLUS  2
#define TB_UNDO  3
#define TB_HELP  4
#define TB_OPEN  5
static int emb_toolbar(lt_app *app, const char *info, const char *help, int enabled, double *val,
                       int has_val, double lo, double hi, int digits, int *val_changed)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w[7], n = 0, hit = 0, cell = lt_ui_cell_mode(ui);
    w[n++] = -1;
    if (has_val) { w[n++] = cell ? 12 * 8 : 96; }
    w[n++] = cell ? 3 * 8 : 28;
    w[n++] = cell ? 3 * 8 : 28;
    w[n++] = cell ? 6 * 8 : 56;
    if (help != NULL) { w[n++] = cell ? 3 * 8 : 28; }
    w[n++] = cell ? 16 * 8 : 116;
    lt_ui_layout_row(ui, n, w, 0);
    lt_ui_label_ex(ui, info, t->muted, LT_UI_ALIGN_LEFT);
    *val_changed = 0;
    lt_ui_disabled_begin(ui, !enabled);
    if (has_val) {
        double before = *val;
        if (!(hi > lo)) { lo = -1e12; hi = 1e12; }
        /* a value already outside the limits must not be "corrected" just by being shown */
        if (before < lo) { lo = before; }
        if (before > hi) { hi = before; }
        if ((lt_ui_number(ui, "embval", val, lo, hi, step_of(digits), digits) & LT_UI_CHANGED) && fabs(*val - before) > 0.0) {
            *val_changed = 1;
        }
    }
    if (lt_ui_button(ui, "-##embm")) { hit = TB_MINUS; }
    lt_ui_tooltip(ui, "Decrease the selected values by one step");
    if (lt_ui_button(ui, "+##embp")) { hit = TB_PLUS; }
    lt_ui_tooltip(ui, "Increase the selected values by one step");
    if (lt_ui_button(ui, "Undo##embu")) { hit = TB_UNDO; }
    lt_ui_disabled_end(ui);
    if (help != NULL) {
        if (lt_ui_button(ui, "?##embh")) { hit = TB_HELP; }
        lt_ui_tooltip(ui, "Help");
    }
    if (lt_ui_button(ui, "Open in editor##embo")) { hit = TB_OPEN; }
    return hit;
}

/* ------------------------------------------------------------------ */
/* curve                                                               */
#define PL_L(m) lt_ui_px_pct(46, (m)->pct)
#define PL_R(m) lt_ui_px_pct(14, (m)->pct)
#define PL_T(m) lt_ui_px_pct(10, (m)->pct)
#define PL_B(m) lt_ui_px_pct(22, (m)->pct)

static double sx_of(const emb *m, double x)
{
    return m->pr.x + PL_L(m) + (x - m->px0) / (m->px1 - m->px0) * (m->pr.w - PL_L(m) - PL_R(m));
}

static double sy_of(const emb *m, double y)
{
    return m->pr.y + m->pr.h - PL_B(m) - (y - m->py0) / (m->py1 - m->py0) * (m->pr.h - PL_T(m) - PL_B(m));
}

static void crv_ranges(emb *m, const lt_ini_curve *c)
{
    size_t i;
    double xl = 1e300, xh = -1e300, yl = 1e300, yh = -1e300, pad;
    for (i = 0; i < m->v.cols; i++) {
        if (m->v.x[i] < xl) { xl = m->v.x[i]; }
        if (m->v.x[i] > xh) { xh = m->v.x[i]; }
        if (m->v.z[i] < yl) { yl = m->v.z[i]; }
        if (m->v.z[i] > yh) { yh = m->v.z[i]; }
    }
    if (c->has_xaxis && c->xaxis[1] > c->xaxis[0]) {
        m->px0 = c->xaxis[0]; m->px1 = c->xaxis[1];
        lt_uconv_range(&m->v.xu, &m->px0, &m->px1);
        if (xl < m->px0) { m->px0 = xl; }
        if (xh > m->px1) { m->px1 = xh; }
    } else { pad = (xh - xl) * 0.04; if (pad <= 0) { pad = 1; } m->px0 = xl - pad; m->px1 = xh + pad; }
    if (c->has_yaxis && c->yaxis[1] > c->yaxis[0]) {
        m->py0 = c->yaxis[0]; m->py1 = c->yaxis[1];
        lt_uconv_range(&m->v.zu, &m->py0, &m->py1);
        if (yl < m->py0) { m->py0 = yl; }
        if (yh > m->py1) { m->py1 = yh; }
    } else { pad = (yh - yl) * 0.08; if (pad <= 0) { pad = 1; } m->py0 = yl - pad; m->py1 = yh + pad; }
}

static void plot_cb(lt_gfx *g, lt_rect r, void *user)
{
    emb *m = (emb *)user;
    lt_font *fm = lt_font_get(LT_FONT_MONO);
    double f10 = lt_gfx_fpx(g, 10.0);
    size_t i;
    int k, x0, x1, y0, y1;
    char b[48];
    LT_UNUSED(r);
    if (m->v.cols == 0 || !(m->px1 > m->px0) || !(m->py1 > m->py0)) { return; }
    x0 = (int)sx_of(m, m->px0); x1 = (int)sx_of(m, m->px1);
    y0 = (int)sy_of(m, m->py0); y1 = (int)sy_of(m, m->py1);
    for (k = 0; k <= 4; k++) {
        double fy = m->py0 + (m->py1 - m->py0) * k / 4.0, fx = m->px0 + (m->px1 - m->px0) * k / 4.0;
        double yy = sy_of(m, fy), xx = sx_of(m, fx);
        lt_gfx_hline(g, x0, x1, (int)yy, m->grid);
        lt_gfx_vline(g, (int)xx, y1, y0, m->grid);
        if (!m->cell) {
            lt_tabs_fmt(b, sizeof(b), fy, m->zdig);
            lt_gfx_text_bl(g, fm, f10, (double)x0 - (double)lt_gfx_px(g, 6) - (fm != NULL ? lt_gfx_text_measure(fm, f10, b) : 0.0),
                           (int)yy + lt_gfx_px(g, 4), b, m->axis);
            lt_tabs_fmt(b, sizeof(b), fx, m->xdig);
            lt_gfx_text_bl(g, fm, f10, xx - (fm != NULL ? lt_gfx_text_measure(fm, f10, b) / 2.0 : 0.0),
                           y0 + lt_gfx_px(g, 15), b, m->axis);
        }
    }
    if (!m->cell) {
        lt_gfx_rect(g, lt_rect_make(x0, y1, x1 - x0 + 1, y0 - y1 + 1), m->grid);
        if (m->v.cols >= 2 && m->v.cols <= 128) {
            double poly[2 * 132];
            int n = 0;
            for (i = 0; i < m->v.cols; i++) { poly[n * 2] = sx_of(m, m->v.x[i]); poly[n * 2 + 1] = sy_of(m, m->v.z[i]); n++; }
            poly[n * 2] = sx_of(m, m->v.x[m->v.cols - 1]); poly[n * 2 + 1] = (double)y0; n++;
            poly[n * 2] = sx_of(m, m->v.x[0]); poly[n * 2 + 1] = (double)y0; n++;
            lt_gfx_fill_poly(g, poly, n, (m->line & 0x00FFFFFFUL) | 0x22000000UL);
        }
    }
    for (i = 0; i + 1 < m->v.cols; i++) {
        lt_gfx_line(g, sx_of(m, m->v.x[i]), sy_of(m, m->v.z[i]), sx_of(m, m->v.x[i + 1]), sy_of(m, m->v.z[i + 1]),
                    2.0, m->line);
    }
    for (i = 0; i < m->v.cols; i++) {
        double px = sx_of(m, m->v.x[i]), py = sy_of(m, m->v.z[i]);
        int on = (int)i == m->sel || (int)i == m->hot;
        lt_gfx_fill_circle(g, px, py, on ? 5.0 : 3.5, on ? m->selc : m->pt);
        if (!m->cell) { lt_gfx_circle(g, px, py, on ? 5.0 : 3.5, 1.0, LT_RGB(255, 255, 255)); }
    }
    if (m->live_ok) {
        double lx = sx_of(m, m->live_x), ly = sy_of(m, m->live_y);
        if (lx >= x0 && lx <= x1) {
            lt_gfx_line(g, lx, (double)y1, lx, (double)y0, 1.0, (m->live & 0x00FFFFFFUL) | 0x90000000UL);
            lt_gfx_fill_circle(g, lx, ly, 4.0, m->live);
        }
    }
}

/* write new values (a scratch copy of z, or of x when xaxis) through the shared path: only what
 * changes, limited and rounded, one undo step; nothing changed = no undo, nothing modified */
static void emb_apply(lt_app *app, emb *m, int xaxis, const double *nv)
{
    lt_tabs_lim lim;
    if (xaxis) { lt_tabs_lim_init(&lim, m->xlo, m->xhi, m->xdig); }
    else { lt_tabs_lim_init(&lim, m->lo, m->hi, m->zdig); }
    if (lt_tabs_apply(&m->v, xaxis ? LT_TABS_X : LT_TABS_Z, nv, &lim) > 0) { emb_commit(app, m); }
}

/* scratch copy of the values (z, or x when xaxis); free with lt_free */
static double *emb_copy(const emb *m, int xaxis)
{
    size_t n = xaxis ? m->v.cols : m->v.rows * m->v.cols;
    double *t = (double *)lt_calloc(n + 1, sizeof(double));
    memcpy(t, xaxis ? m->v.x : m->v.z, n * sizeof(double));
    return t;
}

static void crv_adjust(lt_app *app, emb *m, double d)
{
    size_t i;
    int any = 0;
    double *nz = emb_copy(m, 0);
    for (i = 0; i < m->v.cols; i++) {
        if (m->gsel[i] & 2) { nz[i] += d; any = 1; }
    }
    if (!any && m->sel >= 0 && (size_t)m->sel < m->v.cols) { nz[m->sel] += d; }
    emb_apply(app, m, 0, nz);
    lt_free(nz);
}

int lt_emb_curve(lt_app *app, lt_embeds *e, const lt_ini_curve *c, int enabled)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    emb *m = emb_get(e, LT_EMB_CURVE, c);
    int w1 = -1, cell = lt_ui_cell_mode(ui), i, hit, vch = 0, down, inside;
    char info[160], b[48];
    double val = 0.0;
    lt_point mp;
    emb_ensure(app, m);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (!m->loaded) { lt_ui_label_ex(ui, m->msg, th->muted, LT_UI_ALIGN_LEFT); return 0; }
    emb_follow(app, m);
    if (m->v.cols == 0) { return 0; }
    if (m->sel < 0 || (size_t)m->sel >= m->v.cols) { m->sel = 0; }
    crv_ranges(m, c);
    {   /* live operating point */
        double lv;
        size_t li;
        m->live_ok = 0;
        if (c->x_channel != NULL && lt_tabs_live_index(app, c->x_channel, m->v.x, m->v.cols, &lv, &li)) {
            size_t j = 0;
            m->live_x = lv;
            while (j + 1 < m->v.cols && m->v.x[j + 1] < lv) { j++; }
            if (j + 1 < m->v.cols && m->v.x[j + 1] > m->v.x[j] && lv >= m->v.x[j]) {
                double f = (lv - m->v.x[j]) / (m->v.x[j + 1] - m->v.x[j]);
                if (f > 1.0) { f = 1.0; }
                m->live_y = m->v.z[j] + f * (m->v.z[j + 1] - m->v.z[j]);
            } else { m->live_y = m->v.z[li]; }
            m->live_ok = 1;
            lt_ui_request_redraw(ui);
        }
    }
    /* plot */
    lt_ui_layout_row(ui, 1, &w1, cell ? 9 * 16 : 190);
    m->pr = lt_ui_layout_next(ui);
    m->cell = cell; m->pct = lt_ui_scale_pct(ui);
    m->line = th->primary_hover; m->pt = th->accent; m->selc = th->warning; m->live = th->cursor_live;
    m->axis = th->text2; m->bg = th->table_bg;
    m->grid = cell ? th->border_subtle : (th->dark ? LT_RGBA(255, 255, 255, 24) : LT_RGBA(0, 0, 0, 28));
    mp = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0;
    inside = mp.x >= m->pr.x && mp.x < m->pr.x + m->pr.w && mp.y >= m->pr.y && mp.y < m->pr.y + m->pr.h;
    if (m->drag < 0) {
        double best = cell ? 1e12 : 160.0;
        m->hot = -1;
        for (i = 0; i < (int)m->v.cols && inside && enabled && !lt_ui_popup_open(ui); i++) {
            double dx = sx_of(m, m->v.x[i]) - mp.x, dy = sy_of(m, m->v.z[i]) - mp.y, d = dx * dx + dy * dy;
            if (d < best) { best = d; m->hot = i; }
        }
        if (down && !m->prev_down && m->hot >= 0) {
            m->drag = m->hot; m->drag_moved = 0; m->sel = m->hot; m->sync = 1;
            m->drag_y0 = mp.y;
        }
    }
    if (m->drag >= 0) {
        if (!down) {
            if (m->drag_moved) { emb_commit(app, m); }
            m->drag = -1;
        } else if (!m->drag_moved && abs(mp.y - m->drag_y0) <= (cell ? 0 : lt_ui_px(ui, 2))) {
            m->hot = m->drag;                   /* a click selects the point: no change, no undo */
        } else {
            double y0 = sy_of(m, m->py0), y1 = sy_of(m, m->py1);
            double ny = m->py0 + (y0 - mp.y) / (y0 - y1) * (m->py1 - m->py0);
            if (!m->drag_moved) { lt_tblview_push_undo(&m->v); }
            lt_tblview_set(&m->v, 0, (size_t)m->drag, clampz(m, ny));
            m->drag_moved = 1;
            m->hot = m->drag;
        }
    }
    m->prev_down = down;
    lt_ui_draw_box(ui, m->pr, th->table_bg, th->border_subtle, 0);
    lt_ui_draw_custom(ui, m->pr, plot_cb, m);
    if (m->hot >= 0 && m->drag < 0) {
        char tip[96], xb[32], yb[32];
        lt_tabs_fmt(xb, sizeof(xb), m->v.x[m->hot], m->xdig);
        lt_tabs_fmt(yb, sizeof(yb), m->v.z[m->hot], m->zdig);
        lt_snprintf(tip, sizeof(tip), "%s: %s, %s: %s", c->col_x != NULL ? c->col_x : "X", xb,
                    c->col_y != NULL ? c->col_y : "Y", yb);
        lt_ui_tooltip(ui, tip);
    }
    /* value grid: row 0 = X bins, row 1 = values */
    {
        int ch = cell ? 16 : 22, chh = cell ? 16 : 22, rhw = cell ? 10 * 8 : 84, cw, fl;
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui)) - rhw - 4;
        int cwmin = emb_cell_w(m, cell);
        cw = cwmin + (cell ? 0 : 8);
        if (!cell && m->v.cols > 0 && avail / (int)m->v.cols >= cwmin) { cw = LT_MIN(avail / (int)m->v.cols, 72); }
        lt_ui_layout_row(ui, 1, &w1, chh + 2 * ch + (cell ? 16 : 12));
        lt_ui_disabled_begin(ui, !enabled);
        if (lt_ui_grid_begin(ui, "cvals", 2, (int)m->v.cols, cw, ch, rhw, chh)) {
            int r0, r1, c0, c1, r, k, cr, cc;
            lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
            lt_ui_grid_corner(ui, "");
            for (k = c0; k <= c1; k++) {
                lt_snprintf(b, sizeof(b), "%d", k + 1);
                lt_ui_grid_header(ui, 1, k, b);
            }
            for (r = r0; r <= r1; r++) {
                lt_ui_grid_header(ui, 0, r, r == 0 ? (c->col_x != NULL ? c->col_x : "X") : (c->col_y != NULL ? c->col_y : "Y"));
                for (k = c0; k <= c1; k++) {
                    double v = r == 0 ? m->v.x[k] : m->v.z[k];
                    lt_color bg = r == 1 ? lt_tabs_heat(app, th, m->py0, m->py1, v) : th->table_axis;
                    if (k == m->hot || (k == m->drag)) { bg = th->table_selected; }
                    lt_tabs_fmt(b, sizeof(b), v, r == 0 ? m->xdig : m->zdig);
                    lt_ui_grid_cell(ui, r, k, b, bg, r == 1 ? lt_tabs_heat_text(bg) : th->text);
                }
            }
            if (m->sync) {
                lt_ui_grid_select(ui, 1, m->sel, 1, m->sel);
                lt_ui_grid_set_cursor(ui, 1, m->sel);
                m->sync = 0;
            }
            memset(m->gsel, 0, m->gsel_cap);
            m->nsel = 0;
            for (k = 0; k < (int)m->v.cols; k++) {
                if (lt_ui_grid_selected(ui, 0, k)) { m->gsel[k] |= 1; }
                if (lt_ui_grid_selected(ui, 1, k)) { m->gsel[k] |= 2; m->nsel++; }
            }
            lt_ui_grid_cursor(ui, &cr, &cc);
            fl = lt_ui_grid_end(ui);
            if ((fl & LT_UI_GRID_SELECTION) && cc >= 0 && cc < (int)m->v.cols) { m->sel = cc; }
            if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
                int er, ec;
                lt_ui_grid_editing(ui, &er, &ec);
                if (ec >= 0 && ec < (int)m->v.cols) {
                    lt_tabs_fmt(b, sizeof(b), er == 0 ? m->v.x[ec] : m->v.z[ec], er == 0 ? m->xdig : m->zdig);
                    lt_ui_grid_edit_set_text(ui, b);
                }
            }
            if ((fl & LT_UI_GRID_EDIT_COMMIT) && enabled) {
                double v;
                int er = -1, ec = -1;
                /* the edited cell, not the cursor: Tab or a click-away has already moved that */
                (void)lt_ui_grid_editing(ui, &er, &ec);
                if (lt_parse_double(lt_ui_grid_edit_text(ui), &v) && er >= 0 && ec >= 0 && ec < (int)m->v.cols) {
                    double *nv = emb_copy(m, er == 0);
                    if (er == 0) {
                        if (m->xhi > m->xlo) { v = v < m->xlo ? m->xlo : (v > m->xhi ? m->xhi : v); }
                        v = lt_tbl_clamp_between(m->v.x, m->v.cols, (size_t)ec, v);
                    }
                    nv[ec] = v;
                    emb_apply(app, m, er == 0, nv);
                    lt_free(nv);
                } else { lt_app_toast(app, 2, "Not a number"); }
            }
        }
        lt_ui_disabled_end(ui);
    }
    /* toolbar */
    val = m->v.z[m->sel];
    {
        char xb[32];
        lt_tabs_fmt(xb, sizeof(xb), m->v.x[m->sel], m->xdig);
        lt_snprintf(info, sizeof(info), "Point %d of %d (%s %s)", m->sel + 1, (int)m->v.cols,
                    c->col_x != NULL ? c->col_x : "X", xb);
    }
    hit = emb_toolbar(app, info, c->help, enabled, &val, 1, m->lo, m->hi, m->zdig, &vch);
    if (vch && enabled) {
        double *nz = emb_copy(m, 0);
        nz[m->sel] = val;
        emb_apply(app, m, 0, nz);
        lt_free(nz);
    }
    switch (hit) {
    case TB_MINUS: crv_adjust(app, m, -step_of(m->zdig)); break;
    case TB_PLUS: crv_adjust(app, m, step_of(m->zdig)); break;
    case TB_UNDO:
        if (lt_tblview_undo(&m->v)) { emb_commit(app, m); } else { lt_app_toast(app, 0, "Nothing to undo"); }
        break;
    case TB_HELP: (void)lt_app_help_open(app, c->help); break;
    case TB_OPEN: (void)lt_app_curve_editor_open(app, c->name); break;
    default: break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* table                                                               */
static void tbl_adjust(lt_app *app, emb *m, double d, int set, double val)
{
    size_t k, n = m->v.rows * m->v.cols;
    double *nz = emb_copy(m, 0);
    for (k = 0; k < n; k++) {
        if (m->gsel[k]) { nz[k] = set ? val : nz[k] + d; }
    }
    emb_apply(app, m, 0, nz);
    lt_free(nz);
}

int lt_emb_table(lt_app *app, lt_embeds *e, const lt_ini_table *t, int enabled)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    emb *m = emb_get(e, LT_EMB_TABLE, t);
    int w1 = -1, cell = lt_ui_cell_mode(ui), hit, vch = 0, rows, cols;
    char info[160], b[48];
    double zlo = 1e300, zhi = -1e300, dummy = 0.0;
    size_t k, lr = 0, lc = 0;
    int live = 0;
    emb_ensure(app, m);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (!m->loaded) { lt_ui_label_ex(ui, m->msg, th->muted, LT_UI_ALIGN_LEFT); return 0; }
    emb_follow(app, m);
    rows = (int)m->v.rows; cols = (int)m->v.cols;
    if (rows == 0 || cols == 0) { return 0; }
    for (k = 0; k < m->v.rows * m->v.cols; k++) {
        if (m->v.z[k] < zlo) { zlo = m->v.z[k]; }
        if (m->v.z[k] > zhi) { zhi = m->v.z[k]; }
    }
    if (m->v.y != NULL && t->x_channel != NULL && t->y_channel != NULL &&
        lt_tabs_live_index(app, t->x_channel, m->v.x, m->v.cols, NULL, &lc) &&
        lt_tabs_live_index(app, t->y_channel, m->v.y, m->v.rows, NULL, &lr)) {
        live = 1;
        lt_ui_request_redraw(ui);
    }
    {
        int ch = cell ? 16 : 22, chh = cell ? 16 : 22, rhw = cell ? 7 * 8 : 60, cw, fl, h;
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui)) - rhw - 14;
        int cwmin = emb_cell_w(m, cell);
        cw = cwmin + (cell ? 0 : 8);
        if (!cell && avail / cols >= cwmin) { cw = LT_MIN(avail / cols, 64); }
        h = chh + rows * ch + (cell ? 16 : 12);
        if (h > chh + 14 * ch + 12) { h = chh + 14 * ch + 12; }
        lt_ui_layout_row(ui, 1, &w1, h);
        lt_ui_disabled_begin(ui, !enabled);
        if (lt_ui_grid_begin(ui, "tvals", rows, cols, cw, ch, rhw, chh)) {
            int r0, r1, c0, c1, r, c;
            lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
            lt_ui_grid_corner(ui, "");
            for (c = c0; c <= c1; c++) {
                lt_tabs_fmt(b, sizeof(b), m->v.x[c], m->xdig);
                lt_ui_grid_header(ui, 1, c, b);
            }
            for (r = r0; r <= r1; r++) {
                size_t dr = (size_t)(rows - 1 - r);     /* highest load row on top, like the editor */
                lt_tabs_fmt(b, sizeof(b), m->v.y != NULL ? m->v.y[dr] : (double)dr, m->ydig);
                lt_ui_grid_header(ui, 0, r, b);
                for (c = c0; c <= c1; c++) {
                    double v = m->v.z[dr * (size_t)cols + (size_t)c];
                    lt_color bg = lt_tabs_heat(app, th, zlo, zhi, v);
                    lt_tabs_fmt(b, sizeof(b), v, m->zdig);
                    lt_ui_grid_cell(ui, r, c, b, bg, lt_tabs_heat_text(bg));
                }
            }
            if (live) { lt_ui_grid_mark(ui, rows - 1 - (int)lr, (int)lc, th->cursor_live, 2); }
            memset(m->gsel, 0, m->gsel_cap);
            m->nsel = 0;
            for (r = 0; r < rows; r++) {
                for (c = 0; c < cols; c++) {
                    if (lt_ui_grid_selected(ui, r, c)) {
                        m->gsel[(size_t)(rows - 1 - r) * (size_t)cols + (size_t)c] = 1;
                        m->nsel++;
                    }
                }
            }
            lt_ui_grid_cursor(ui, &r, &c);
            m->sel = r >= 0 && c >= 0 ? (rows - 1 - r) * cols + c : -1;
            fl = lt_ui_grid_end(ui);
            if ((fl & LT_UI_GRID_EDIT_COMMIT) && enabled) {
                /* the cells selected when the edit began: Tab or a click-away moved the selection */
                double v;
                if (lt_parse_double(lt_ui_grid_edit_text(ui), &v)) {
                    unsigned char *keep = m->gsel;
                    m->gsel = m->esel;
                    tbl_adjust(app, m, 0.0, 1, v);
                    m->gsel = keep;
                } else { lt_app_toast(app, 2, "Not a number"); }
            }
            if (fl & LT_UI_GRID_EDIT_BEGIN) {
                int er, ec;
                lt_ui_grid_editing(ui, &er, &ec);
                memcpy(m->esel, m->gsel, m->gsel_cap);
                if (er >= 0 && ec >= 0 && er < rows && ec < cols) {
                    size_t ki = (size_t)(rows - 1 - er) * (size_t)cols + (size_t)ec;
                    if (m->nsel == 0) { m->esel[ki] = 1; }
                    if (lt_ui_grid_edit_wants_text(ui)) {
                        lt_tabs_fmt(b, sizeof(b), m->v.z[ki], m->zdig);
                        lt_ui_grid_edit_set_text(ui, b);
                    }
                }
            }
        }
        lt_ui_disabled_end(ui);
    }
    lt_snprintf(info, sizeof(info), "%dx%d  %s", cols, rows, m->nsel > 1 ? "(selection)" : "");
    if (m->nsel <= 1 && m->sel >= 0 && (size_t)m->sel < m->v.rows * m->v.cols) {
        char vb[32];
        lt_tabs_fmt(vb, sizeof(vb), m->v.z[m->sel], m->zdig);
        lt_snprintf(info, sizeof(info), "%dx%d  cell %s", cols, rows, vb);
    }
    hit = emb_toolbar(app, info, t->help, enabled, &dummy, 0, 0.0, 0.0, m->zdig, &vch);
    if ((hit == TB_MINUS || hit == TB_PLUS) && m->nsel == 0 && m->sel >= 0) { m->gsel[m->sel] = 1; }
    switch (hit) {
    case TB_MINUS: tbl_adjust(app, m, -step_of(m->zdig), 0, 0.0); break;
    case TB_PLUS: tbl_adjust(app, m, step_of(m->zdig), 0, 0.0); break;
    case TB_UNDO:
        if (lt_tblview_undo(&m->v)) { emb_commit(app, m); } else { lt_app_toast(app, 0, "Nothing to undo"); }
        break;
    case TB_HELP: (void)lt_app_help_open(app, t->help); break;
    case TB_OPEN: (void)lt_app_table_editor_open(app, t->name); break;
    default: break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* indicator / readout panels                                          */
static lt_color ini_color(const char *s, lt_color dflt)
{
    lt_u32 argb;
    if (s != NULL && lt_ini_parse_color(s, &argb)) { return (lt_color)(argb | 0xFF000000UL); }
    return dflt;
}

int lt_emb_indicators(lt_app *app, const lt_ini_indicator_panel *p)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int ncol = p->columns > 0 ? (p->columns > 6 ? 6 : p->columns) : 2, w[6], i, cell = lt_ui_cell_mode(ui);
    size_t k;
    if (p->vis_cond != NULL && !lt_expr_cache_test(&app->expr, p->vis_cond, &app->varenv.env)) { return 0; }
    if (p->n_inds == 0) { return 0; }
    for (i = 0; i < ncol; i++) { w[i] = -1; }
    for (k = 0; k < p->n_inds; k++) {
        const lt_ini_indicator *in = &p->inds[k];
        int on = lt_expr_cache_f64(&app->expr, in->expression, &app->varenv.env, 0.0) != 0.0;
        char lb[128];
        const char *raw = on ? in->label_on : in->label_off, *txt = raw != NULL ? raw : "";
        lt_color bg = on ? ini_color(in->on_bg, t->success_bg) : ini_color(in->off_bg, t->surface);
        lt_color fg = on ? ini_color(in->on_fg, t->success) : ini_color(in->off_fg, t->muted);
        lt_rect r;
        if (k % (size_t)ncol == 0) { lt_ui_layout_row(ui, ncol, w, cell ? 0 : 26); }
        if (raw != NULL && raw[0] == '{') {
            char *d = lt_expr_cache_display(&app->expr, raw, &app->varenv.env);
            if (d != NULL) { lt_strlcpy(lb, d, sizeof(lb)); lt_free(d); txt = lb; }
        }
        r = lt_ui_layout_next(ui);
        lt_ui_draw_box(ui, r, bg, cell ? bg : lt_color_mix(bg, LT_RGB(0, 0, 0), 60), cell ? 0 : 4);
        lt_ui_draw_text_in(ui, r, txt, fg, LT_UI_ALIGN_CENTER);
    }
    for (; k % (size_t)ncol != 0; k++) { (void)lt_ui_layout_next(ui); }   /* finish the row */
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) { lt_ui_request_redraw(ui); }
    return 1;
}

int lt_emb_readouts(lt_app *app, const lt_ini_readout_panel *p)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int ncol = p->columns > 0 ? (p->columns > 6 ? 6 : p->columns) : 1, w[6], i, cell = lt_ui_cell_mode(ui);
    size_t k;
    if (p->vis_cond != NULL && !lt_expr_cache_test(&app->expr, p->vis_cond, &app->varenv.env)) { return 0; }
    if (p->n_readouts == 0) { return 0; }
    for (i = 0; i < ncol; i++) { w[i] = -1; }
    for (k = 0; k < p->n_readouts; k++) {
        const lt_ini_readout *ro = &p->readouts[k];
        double v = 0.0;
        const char *ul = NULL;
        char vb[48], line[160];
        lt_rect r;
        int ok = ro->channel != NULL && lt_app_channel_disp(app, ro->channel, &v, &ul);
        if (k % (size_t)ncol == 0) { lt_ui_layout_row(ui, ncol, w, cell ? 0 : 40); }
        if (ok) { lt_tabs_fmt(vb, sizeof(vb), v, ro->precision); } else { lt_strlcpy(vb, "-", sizeof(vb)); }
        r = lt_ui_layout_next(ui);
        if (cell) {
            lt_snprintf(line, sizeof(line), "%s: %s %s", ro->title != NULL ? ro->title : ro->channel, vb,
                        ul != NULL ? ul : (ro->units != NULL ? ro->units : ""));
            lt_ui_draw_text_in(ui, r, line, t->accent, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_draw_box(ui, r, t->surface, t->border_subtle, 4);
            lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 6), r.y + lt_ui_px(ui, 2), r.w - lt_ui_px(ui, 12), lt_ui_px(ui, 14)),
                                 ro->title != NULL ? ro->title : ro->channel, t->muted, LT_UI_ALIGN_LEFT,
                                 (int)LT_FONT_SANS_MEDIUM, LT_UI_FS_XS);
            lt_snprintf(line, sizeof(line), "%s %s", vb, ul != NULL ? ul : (ro->units != NULL ? ro->units : ""));
            lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 6), r.y + lt_ui_px(ui, 16), r.w - lt_ui_px(ui, 12), lt_ui_px(ui, 22)),
                                 line, t->accent, LT_UI_ALIGN_LEFT, (int)LT_FONT_MONO_MEDIUM, LT_UI_FS_LG);
        }
    }
    for (; k % (size_t)ncol != 0; k++) { (void)lt_ui_layout_next(ui); }
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) { lt_ui_request_redraw(ui); }
    return 1;
}

/* ------------------------------------------------------------------ */
/* gauges                                                              */
static void gauge_cb(lt_gfx *g, lt_rect r, void *user)
{
    emb *m = (emb *)user;
    lt_dash_render_gauge(g, r, &m->g, m->gval);
}

int lt_emb_gauge(lt_app *app, lt_embeds *e, const lt_ini_gauge *gi)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    emb *m = emb_get(e, LT_EMB_GAUGE, gi);
    int w1 = -1, cell = lt_ui_cell_mode(ui);
    lt_rect r;
    double v = 0.0;
    if (!m->g_init) {
        lt_dash_gauge_init(&m->g);
        lt_dash_gauge_from_ini(gi, &m->g, &app->varenv.env);
        m->g_init = 1;
    }
    if (gi->channel != NULL && lt_app_channel(app, gi->channel, &v)) { m->gval = v; }
    else { m->gval = m->g.min; }
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) { lt_ui_request_redraw(ui); }
    if (cell) {
        char line[160], vb[48];
        lt_tabs_fmt(vb, sizeof(vb), m->gval, gi->digits);
        lt_snprintf(line, sizeof(line), "%s: %s %s", gi->title != NULL ? gi->title : gi->name, vb,
                    gi->units != NULL ? gi->units : "");
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, line, t->accent, LT_UI_ALIGN_LEFT);
        return 1;
    }
    lt_ui_layout_row(ui, 1, &w1, 180);
    r = lt_ui_layout_next(ui);
    if (r.w > lt_ui_px(ui, 240)) { r.x += (r.w - lt_ui_px(ui, 240)) / 2; r.w = lt_ui_px(ui, 240); }
    lt_gauges_install();
    lt_gauges_set_env(&app->expr, &app->varenv.env);
    lt_ui_draw_custom(ui, r, gauge_cb, m);
    return 1;
}
