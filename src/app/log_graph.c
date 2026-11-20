/* log_graph.c - stacked graph panes of the data logging tabs (MegaLogViewer style): every pane
 * plots a few channels, each with its own scale (auto from the visible data or fixed) on a left or
 * right axis; one shared time axis, markers, a cursor with the values of every channel at the
 * cursor time, wheel zoom and drag pan.  Works in pixel and cell (terminal) mode.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "log_view.h"
#include "view_kit.h"

const lt_color lt_lg_palette[LT_LG_COLORS] = {
    0xFF4FC3F7UL, 0xFFFFB74DUL, 0xFF81C784UL, 0xFFE57373UL,
    0xFFBA68C8UL, 0xFFFFF176UL, 0xFF4DB6ACUL, 0xFFF06292UL
};

#define LG_MAXCOLS 4096

long lt_lg_find(const lt_log *l, double t)
{
    long lo = 0, hi = l->n - 1, mid;
    if (l->n <= 0) { return 0; }
    if (t <= l->t[0]) { return 0; }
    if (t >= l->t[l->n - 1]) { return l->n - 1; }
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (l->t[mid] <= t) { lo = mid; } else { hi = mid - 1; }
    }
    return lo;
}

/* text helper: proportional Plex text in pixel mode, the 8x16 grid in cell mode */
static void gtext(lt_ui *ui, int x, int y, int w, int h, const char *s, lt_color c, int align, int mono)
{
    if (lt_ui_cell_mode(ui)) {
        int tw = (int)strlen(s) * 8;
        int xx = align == 2 ? x + w - tw : (align == 1 ? x + (w - tw) / 2 : x);
        lt_ui_draw_text(ui, xx, y, s, c);
        return;
    }
    lt_ui_draw_text_in_f(ui, lt_rect_make(x, y, w, h), s, c, align,
                         mono ? (int)LT_FONT_MONO : (int)LT_FONT_SANS_MEDIUM, mono ? LT_UI_FS_XS : LT_UI_FS_SM);
}

static int gtext_w(lt_ui *ui, const char *s, int mono)
{
    if (lt_ui_cell_mode(ui)) { return (int)strlen(s) * 8; }
    return lt_ui_text_width_f(ui, s, mono ? (int)LT_FONT_MONO : (int)LT_FONT_SANS_MEDIUM, mono ? LT_UI_FS_XS : LT_UI_FS_SM);
}

static void fmt_val(char *b, size_t cap, double v)
{
    double a = fabs(v);
    lt_fmt_double(b, cap, v, a >= 1000.0 ? 0 : (a >= 100.0 ? 1 : (a >= 1.0 ? 2 : 3)));
}

/* tick label of an axis value (step decides the decimals) */
static void fmt_tick(char *b, size_t cap, double v, double step)
{
    int d = step >= 1.0 ? 0 : (step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3));
    if (step >= 1.0 && fabs(step - floor(step)) > 1e-9) { d = 1; }
    else if (step < 1.0 && fabs(step * pow(10.0, (double)d) - floor(step * pow(10.0, (double)d) + 0.5)) > 1e-6) { d++; }
    if (fabs(v) < step * 1e-6) { v = 0.0; }
    lt_snprintf(b, cap, "%.*f", d, v);
}

typedef struct lg_tr {
    int ci;                       /* log channel index or -1 */
    lt_uconv cv;
    double lo, hi;
    double cur;                   /* value at the cursor (display unit) */
    int has_cur;
    const char *unit;
} lg_tr;

/* scale the trace's visible values (display units) and the value at the cursor */
static void trace_prepare(const lt_log *L, const lt_graph_trace *gt, lg_tr *t, long i0, long i1,
                          long icur, int have_cur, double *tmp)
{
    long i, n = 0, step;
    step = (i1 - i0) / LG_MAXCOLS + 1;
    for (i = i0; i <= i1; i += step) {
        tmp[n++] = lt_uconv_to_disp(&t->cv, LT_LOG_AT(L, i, t->ci));
    }
    if (step > 1 && i1 >= i0) {   /* decimated: make sure the extremes are in */
        double a = 1e300, b = -1e300;
        for (i = i0; i <= i1; i++) {
            double v = LT_LOG_AT(L, i, t->ci);
            if (v < a) { a = v; }
            if (v > b) { b = v; }
        }
        if (a <= b) {
            tmp[n++] = lt_uconv_to_disp(&t->cv, a);
            tmp[n++] = lt_uconv_to_disp(&t->cv, b);
        }
    }
    lt_graph_trace_range(gt, tmp, n, &t->lo, &t->hi);
    t->has_cur = have_cur && L->n > 0;
    t->cur = t->has_cur ? lt_uconv_to_disp(&t->cv, LT_LOG_AT(L, icur, t->ci)) : 0.0;
}

static void trace_plot(lt_ui *ui, const lt_log *L, const lg_tr *t, lt_rect pl, long i0, long i1,
                       double t0, double t1, lt_color col, int cell)
{
    double span = t1 - t0, ys = (double)(pl.h - 1) / (t->hi - t->lo);
    double lw = cell ? 1.0 : 1.5 * (double)lt_ui_scale_pct(ui) / 100.0;
    long i;
    int cols = pl.w > LG_MAXCOLS ? LG_MAXCOLS : pl.w;
    if (cell) { cols = pl.w / 4; }      /* braille dots are 4 px apart */
    if (span <= 0.0 || i1 < i0 || cols < 2) { return; }
#define LG_Y(v) ((double)(pl.y + pl.h - 1) - ((v) - t->lo) * ys)
    if (i1 - i0 <= (long)cols) {
        double px = 0.0, py = 0.0;
        int have = 0;
        for (i = i0; i <= i1; i++) {
            double x = pl.x + (L->t[i] - t0) / span * (pl.w - 1);
            double y = LG_Y(lt_uconv_to_disp(&t->cv, LT_LOG_AT(L, i, t->ci)));
            if (y < pl.y - 4) { y = pl.y - 4; }
            if (y > pl.y + pl.h + 3) { y = pl.y + pl.h + 3; }
            if (have) {
                if (!cell) { lt_ui_draw_line(ui, px, py, x, y, lw * 3.0, (col & 0x00FFFFFFUL) | 0x26000000UL); }
                lt_ui_draw_line(ui, px, py, x, y, lw, col);
            }
            px = x; py = y; have = 1;
        }
    } else {
        /* min/max envelope per pixel column, joined at the column means */
        int c, prev = -1;
        double pmid = 0.0;
        long k = i0;
        for (c = 0; c < cols; c++) {
            double te = t0 + span * (double)(c + 1) / (double)cols, a = 1e300, b = -1e300, s = 0.0;
            long cnt = 0;
            while (k <= i1 && (L->t[k] < te || c == cols - 1)) {
                double v = lt_uconv_to_disp(&t->cv, LT_LOG_AT(L, k, t->ci));
                if (v < a) { a = v; }
                if (v > b) { b = v; }
                s += v; cnt++; k++;
            }
            if (cnt == 0) { continue; }
            {
                double x = pl.x + (double)c * (pl.w - 1) / (double)(cols - 1);
                double ya = LG_Y(a), yb = LG_Y(b), ym = LG_Y(s / (double)cnt);
                if (!cell && ya - yb >= 1.0) { lt_ui_draw_line(ui, x, ya, x, yb, lw, col); }
                if (prev >= 0) {
                    double xp = pl.x + (double)prev * (pl.w - 1) / (double)(cols - 1);
                    lt_ui_draw_line(ui, xp, pmid, x, ym, lw, col);
                }
                prev = c; pmid = ym;
            }
        }
    }
#undef LG_Y
}

/* value ticks of one axis, labels in the trace colour; side 0 left, 1 right */
static void axis_ticks(lt_ui *ui, lt_rect pl, const lg_tr *t, int side, int gutter, lt_color col, int cell)
{
    int want = pl.h / (cell ? 32 : lt_ui_px(ui, 34)), lh = cell ? 16 : lt_ui_px(ui, 14);
    double step, v;
    char b[32];
    if (want < 1) { want = 1; }
    step = lt_graph_nice_step(t->hi - t->lo, want);
    for (v = ceil(t->lo / step) * step; v <= t->hi + step * 1e-9; v += step) {
        int y = pl.y + pl.h - 1 - (int)((v - t->lo) / (t->hi - t->lo) * (double)(pl.h - 1));
        fmt_tick(b, sizeof(b), v, step);
        if (side == 0) {
            gtext(ui, pl.x - gutter, y - lh / 2, gutter - (cell ? 8 : lt_ui_px(ui, 4)), lh, b, col, 2, 1);
        } else {
            gtext(ui, pl.x + pl.w + (cell ? 8 : lt_ui_px(ui, 4)), y - lh / 2, gutter, lh, b, col, 0, 1);
        }
        if (step <= 0.0) { break; }
    }
}

int lt_lg_draw(lt_app *app, lt_rect r, const lt_log *L, const lt_graph_page *pg, lt_lg_view *v,
               double tmin, double tmax)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int np = pg != NULL && pg->n_panes > 0 ? pg->n_panes : 1;
    int axis_h = cell ? 16 : lt_ui_px(ui, 22), gap = cell ? 0 : lt_ui_px(ui, 4);
    int hdr = cell ? 16 : lt_ui_px(ui, 22), lm = cell ? 48 : lt_ui_px(ui, 50);
    int pane_h = (r.h - axis_h) / np, q, k, changed = 0;
    double t0 = v->t0, t1 = v->t1;
    long i0 = 0, i1 = -1, icur = 0;
    double *tmp;
    lt_rect plot_any;
    lt_point m = lt_ui_mouse(ui);
    int inside = m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h - axis_h;
    int down = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0, rdown;
    rdown = (lt_ui_mouse_down(ui, LT_MB_RIGHT) || lt_ui_mouse_down(ui, LT_MB_MIDDLE)) ? 1 : 0;
    if (t1 <= t0) { t1 = t0 + 1.0; }
    memset(&plot_any, 0, sizeof(plot_any));
    if (L != NULL && L->n > 0) {
        i0 = lt_lg_find(L, t0);
        i1 = lt_lg_find(L, t1);
        if (i1 + 1 < L->n && L->t[i1] < t1) { i1++; }
        icur = lt_lg_find(L, v->has_cursor ? v->cursor_t : t1);
    }
    tmp = (double *)lt_malloc((size_t)(LG_MAXCOLS + 8) * sizeof(double));
    for (q = 0; q < np; q++) {
        const lt_graph_pane *pn = pg != NULL && q < pg->n_panes ? &pg->pane[q] : NULL;
        lg_tr tr[LT_GRAPH_MAX_TRACES];
        lt_rect pr, pl;
        int has_right = 0, first_l = -1, first_r = -1, lx, nt = pn != NULL ? pn->n : 0, g;
        int sel = q == v->sel_pane && np > 1;
        pr = lt_rect_make(r.x, r.y + q * pane_h, r.w, pane_h - gap);
        for (k = 0; k < nt; k++) {
            if (pn->tr[k].right) { has_right = 1; }
        }
        pl = lt_rect_make(pr.x + lm, pr.y + hdr, pr.w - lm - (has_right ? lm : (cell ? 8 : lt_ui_px(ui, 12))),
                          pr.h - hdr - (cell ? 0 : lt_ui_px(ui, 6)));
        if (pl.w < 8 || pl.h < 4) { continue; }
        plot_any = pl;
        lt_ui_draw_box(ui, pr, th->table_bg, sel ? th->primary : th->border_subtle, cell ? 0 : lt_ui_px(ui, 6));
        if (!cell) {
            lt_ui_draw_vgrad(ui, pl, LT_RGBA(255, 255, 255, th->dark ? 5 : 0), LT_RGBA(0, 0, 0, th->dark ? 36 : 6));
        }
        for (g = 1; g < 4; g++) {
            lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, pl.y + pl.h * g / 4,
                             cell ? th->table_grid : (th->dark ? LT_RGBA(255, 255, 255, 20) : LT_RGBA(0, 0, 0, 22)));
        }
        {   /* time grid */
            double st = lt_graph_nice_step(t1 - t0, pl.w / (cell ? 96 : lt_ui_px(ui, 110)) + 1), tt;
            for (tt = ceil(t0 / st) * st; tt < t1; tt += st) {
                int x = pl.x + (int)((tt - t0) / (t1 - t0) * (double)(pl.w - 1));
                lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h,
                                 cell ? th->table_grid : (th->dark ? LT_RGBA(255, 255, 255, 14) : LT_RGBA(0, 0, 0, 16)));
            }
        }
        lx = pr.x + (cell ? 8 : lt_ui_px(ui, 10));
        for (k = 0; k < nt; k++) {
            const lt_graph_trace *gt = &pn->tr[k];
            lt_color col = lt_lg_palette[k % LT_LG_COLORS];
            char lab[128], vb[32];
            int nw;
            tr[k].ci = L != NULL ? lt_log_channel_index(L, gt->channel) : -1;
            tr[k].unit = "";
            tr[k].lo = 0.0; tr[k].hi = 1.0;
            tr[k].has_cur = 0;
            if (tr[k].ci >= 0) {
                const char *u = L->units != NULL && L->units[tr[k].ci] != NULL ? L->units[tr[k].ci] : "";
                lt_uconv_make(&tr[k].cv, u);
                tr[k].unit = lt_uconv_label(&tr[k].cv, u);
                if (i1 >= i0 && L->n > 0) {
                    trace_prepare(L, gt, &tr[k], i0, i1, icur, 1, tmp);
                    lt_ui_clip_push(ui, lt_rect_make(pl.x, pl.y - 3, pl.w, pl.h + 6));
                    trace_plot(ui, L, &tr[k], pl, i0, i1, t0, t1, col, cell);
                    lt_ui_clip_pop(ui);
                } else if (!gt->autoscale && gt->hi > gt->lo) {
                    tr[k].lo = gt->lo; tr[k].hi = gt->hi;
                }
                if (gt->right) { if (first_r < 0) { first_r = k; } }
                else if (first_l < 0) { first_l = k; }
            }
            /* legend chip: name value unit */
            if (tr[k].ci < 0) { lt_snprintf(vb, sizeof(vb), "--"); }
            else if (tr[k].has_cur) { fmt_val(vb, sizeof(vb), tr[k].cur); }
            else { lt_snprintf(vb, sizeof(vb), "-"); }
            lt_snprintf(lab, sizeof(lab), "%s%s", gt->channel, tr[k].ci < 0 ? " (missing)" : "");
            nw = gtext_w(ui, lab, 0);
            if (lx + nw > pr.x + pr.w - 8) { continue; }
            if (cell) {
                lt_ui_draw_text(ui, lx, pr.y, lab, col);
                lt_ui_draw_text(ui, lx + nw + 8, pr.y, vb, th->text);
                lx += nw + 16 + gtext_w(ui, vb, 1) + gtext_w(ui, tr[k].unit, 0) + 8;
                if (tr[k].unit[0]) { lt_ui_draw_text(ui, lx - gtext_w(ui, tr[k].unit, 0) - 8, pr.y, tr[k].unit, th->muted); }
            } else {
                int vw = gtext_w(ui, vb, 1), uw = gtext_w(ui, tr[k].unit, 0), cy = pr.y + hdr / 2;
                lt_ui_draw_circle_fill(ui, (double)lx + 4.0 * (double)lt_ui_scale_pct(ui) / 100.0, (double)cy,
                                       3.5 * (double)lt_ui_scale_pct(ui) / 100.0, col);
                gtext(ui, lx + lt_ui_px(ui, 12), pr.y + lt_ui_px(ui, 2), nw + lt_ui_px(ui, 4), hdr - lt_ui_px(ui, 2), lab, th->text2, 0, 0);
                gtext(ui, lx + lt_ui_px(ui, 16) + nw, pr.y + lt_ui_px(ui, 2), vw + lt_ui_px(ui, 4), hdr - lt_ui_px(ui, 2), vb, col, 0, 1);
                gtext(ui, lx + lt_ui_px(ui, 20) + nw + vw, pr.y + lt_ui_px(ui, 2), uw + lt_ui_px(ui, 4), hdr - lt_ui_px(ui, 2), tr[k].unit, th->muted, 0, 0);
                lx += lt_ui_px(ui, 36) + nw + vw + uw;
            }
        }
        if (nt == 0) {
            gtext(ui, pl.x + (cell ? 8 : lt_ui_px(ui, 8)), pl.y + pl.h / 2 - (cell ? 8 : lt_ui_px(ui, 8)), pl.w,
                  cell ? 16 : lt_ui_px(ui, 16), "Empty pane: tick channels in the list to plot them here",
                  th->muted, 0, 0);
        }
        if (first_l >= 0) { axis_ticks(ui, pl, &tr[first_l], 0, lm, lt_lg_palette[first_l % LT_LG_COLORS], cell); }
        if (first_r >= 0) { axis_ticks(ui, pl, &tr[first_r], 1, lm, lt_lg_palette[first_r % LT_LG_COLORS], cell); }
        /* markers */
        if (L != NULL) {
            long mk;
            for (mk = 0; mk < L->n_markers; mk++) {
                double f = (L->markers[mk].t - t0) / (t1 - t0);
                if (f >= 0.0 && f <= 1.0) {
                    int mx = pl.x + (int)(f * (double)(pl.w - 1));
                    lt_ui_draw_vline(ui, mx, pl.y, pl.y + pl.h, 0xFFFFB74DUL);
                    if (q == 0) { lt_ui_draw_text(ui, mx + 2, pl.y + 1, L->markers[mk].text, 0xFFFFB74DUL); }
                }
            }
        }
        /* cursor */
        if (v->has_cursor && v->cursor_t >= t0 && v->cursor_t <= t1) {
            int cx = pl.x + (int)((v->cursor_t - t0) / (t1 - t0) * (double)(pl.w - 1));
            lt_ui_draw_vline(ui, cx, pl.y, pl.y + pl.h, cell ? th->text : (th->dark ? LT_RGBA(255, 255, 255, 170) : LT_RGBA(0, 0, 0, 150)));
        }
        /* input: press selects the pane */
        if (down && !v->prev_down && inside && !lt_ui_popup_open(ui) &&
            m.y >= pr.y && m.y < pr.y + pane_h) {
            v->sel_pane = q;
        }
    }
    lt_free(tmp);
    /* time axis + cursor readout */
    if (plot_any.w > 0) {
        lt_rect ax = lt_rect_make(r.x, r.y + np * pane_h, r.w, axis_h);
        double st = lt_graph_nice_step(t1 - t0, plot_any.w / (cell ? 96 : lt_ui_px(ui, 110)) + 1), tt;
        char b[64];
        int tw = cell ? 64 : lt_ui_px(ui, 60);
        for (tt = ceil(t0 / st) * st; tt < t1; tt += st) {
            int x = plot_any.x + (int)((tt - t0) / (t1 - t0) * (double)(plot_any.w - 1));
            fmt_tick(b, sizeof(b), tt, st);
            lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), "s");
            gtext(ui, x - tw / 2, ax.y, tw, axis_h, b, th->muted, 1, 1);
        }
        if (v->has_cursor && v->cursor_t >= t0 && v->cursor_t <= t1) {
            int cx = plot_any.x + (int)((v->cursor_t - t0) / (t1 - t0) * (double)(plot_any.w - 1)), cw2;
            lt_snprintf(b, sizeof(b), " %.3f s ", v->cursor_t);
            cw2 = gtext_w(ui, b, 1) + (cell ? 0 : lt_ui_px(ui, 8));
            if (cx - cw2 / 2 < ax.x) { cx = ax.x + cw2 / 2; }
            if (cx + cw2 / 2 > ax.x + ax.w) { cx = ax.x + ax.w - cw2 / 2; }
            lt_ui_draw_box(ui, lt_rect_make(cx - cw2 / 2, ax.y + (cell ? 0 : lt_ui_px(ui, 2)), cw2, axis_h - (cell ? 0 : lt_ui_px(ui, 4))),
                           cell ? th->selected : th->elevated, th->border, cell ? 0 : lt_ui_px(ui, 4));
            gtext(ui, cx - cw2 / 2, ax.y, cw2, axis_h, b, th->text, 1, 1);
        }
    }
    /* ---- interaction ---- */
    if (plot_any.w > 8 && !lt_ui_popup_open(ui)) {
        double span = t1 - t0, full = tmax - tmin;
        double mt = t0 + (double)(m.x - plot_any.x) / (double)(plot_any.w - 1) * span;
        int wh = inside ? lt_ui_wheel(ui) : 0;
        if (full <= 0.0) { full = span; }
        if (wh != 0) {
            double f = wh > 0 ? 0.8 : 1.25, ns = span, a;
            int i;
            for (i = 0; i < (wh > 0 ? wh : -wh); i++) { ns *= f; }
            if (ns < 0.2) { ns = 0.2; }
            if (ns > full) { ns = full; }
            a = (mt - t0) / span;
            v->t0 = mt - a * ns;
            v->t1 = v->t0 + ns;
            changed = 1;
        }
        if (v->drag == 0 && inside) {
            int shift = (lt_ui_mods(ui) & LT_MOD_SHIFT) != 0;
            if (rdown || (down && shift && !v->prev_down)) {
                v->drag = 2; v->drag_x = m.x; v->drag_t0 = t0; v->drag_t1 = t1;
            } else if (down && !v->prev_down) {
                v->drag = 1;
            }
        }
        if (v->drag == 1) {
            if (!down) { v->drag = 0; }
            else {
                double ct = mt < t0 ? t0 : (mt > t1 ? t1 : mt);
                if (!v->has_cursor || ct != v->cursor_t) { changed = 1; }
                v->cursor_t = ct;
                v->has_cursor = 1;
            }
        } else if (v->drag == 2) {
            if (!down && !rdown) { v->drag = 0; }
            else {
                double dt = -(double)(m.x - v->drag_x) / (double)(plot_any.w - 1) * (v->drag_t1 - v->drag_t0);
                v->t0 = v->drag_t0 + dt;
                v->t1 = v->drag_t1 + dt;
                changed = 1;
            }
        }
        if (changed && v->drag != 1) {   /* keep the window inside the data */
            double s2 = v->t1 - v->t0;
            if (v->t0 < tmin) { v->t0 = tmin; v->t1 = tmin + s2; }
            if (v->t1 > tmax && tmax > tmin) { v->t1 = tmax; v->t0 = tmax - s2; if (v->t0 < tmin) { v->t0 = tmin; } }
            v->user_moved = 1;
        }
    }
    v->prev_down = down;
    return changed;
}
