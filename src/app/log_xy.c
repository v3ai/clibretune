/* log_xy.c - MegaLogViewer HD style analysis views of the Datalog Viewer: the scatter plot
 * (X channel vs Y channel, points coloured by a Z channel, optional filter expression) and the
 * histogram / table generator (average of a Z channel binned over X/Y axes, auto bins or the axes
 * of a tune table, with hit counts; "Copy values" puts the grid on the clipboard so it can be
 * pasted into the table editor, which keeps the edit undoable).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "log_view.h"
#include "view_kit.h"
#include "dialogs.h"

#define XY_MAXPTS 12000

typedef struct xy_ud { const lt_log *L; const lt_ini *ini; } xy_ud;

static const char *ch_get(void *ud, int i) { return ((xy_ud *)ud)->L->names[i]; }
static const char *ch_get_none(void *ud, int i)
{
    return i == 0 ? "(none)" : ((xy_ud *)ud)->L->names[i - 1];
}
static const char *ch_get_count(void *ud, int i)
{
    return i == 0 ? "(hit count)" : ((xy_ud *)ud)->L->names[i - 1];
}
static const char *tb_get(void *ud, int i)
{
    const lt_ini_table *t = ((xy_ud *)ud)->ini->tables[i];
    return t->title != NULL && t->title[0] ? t->title : t->name;
}

void lt_lxy_free(lt_lxy *s)
{
    lt_free(s->mask);
    s->mask = NULL;
    s->mask_n = 0;
    if (s->h_ok) { lt_hist2d_free(&s->h); }
    s->h_ok = 0;
}

static int find_ci(const lt_log *L, const char *const *words, int nw)
{
    int w, i, best = -1, best_rank = 99;
    for (w = 0; w < nw && best < 0; w++) {
        for (i = 0; i < L->n_ch; i++) {
            int rk = lt_graph_name_rank(L->names[i], words[w]);
            if (rk < 0) { continue; }
            if (best < 0 || rk < best_rank || (rk == best_rank && strlen(L->names[i]) < strlen(L->names[best]))) {
                best = i;
                best_rank = rk;
            }
        }
    }
    return best;
}

static void xy_defaults(lt_lxy *s, const lt_log *L)
{
    static const char *const wx[] = { "rpm" };
    static const char *const wy[] = { "map", "load", "tps" };
    static const char *const wz[] = { "afr", "lambda", "ego" };
    s->x = find_ci(L, wx, 1);
    s->y = find_ci(L, wy, 3);
    s->z = find_ci(L, wz, 3);
    if (s->x < 0) { s->x = 0; }
    if (s->y < 0) { s->y = L->n_ch > 1 ? 1 : -1; }
    s->nbx = 16; s->nby = 16; s->min_hits = 1;
    s->inited = 1;
}

/* recompute the filter mask (log replaced or filter applied) */
static void mask_update(lt_lxy *s, const lt_log *L, int log_gen, int force)
{
    if (!force && s->mask != NULL && s->mask_n == L->n && s->log_gen == log_gen) { return; }
    lt_free(s->mask);
    s->mask = (unsigned char *)lt_calloc((size_t)(L->n > 0 ? L->n : 1), 1);
    s->mask_n = L->n;
    s->passed = lt_log_filter(L, s->filter, s->mask, s->err, sizeof(s->err));
    if (s->passed < 0) {         /* bad expression: show everything, keep the message */
        long i;
        for (i = 0; i < L->n; i++) { s->mask[i] = 1; }
        s->passed = L->n;
    }
    lt_strlcpy(s->mask_filter, s->filter, sizeof(s->mask_filter));
    s->mask_gen++;
    s->log_gen = log_gen;
}

static void ftick(char *b, size_t cap, double v, double step)
{
    int d = step >= 1.0 ? 0 : (step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3));
    if (step >= 1.0 && fabs(step - floor(step)) > 1e-9) { d = 1; }
    if (fabs(v) < step * 1e-6) { v = 0.0; }
    lt_snprintf(b, cap, "%.*f", d, v);
}

static void txt(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align)
{
    if (lt_ui_cell_mode(ui)) {
        int tw = (int)strlen(s) * 8;
        int x = align == 2 ? r.x + r.w - tw : (align == 1 ? r.x + (r.w - tw) / 2 : r.x);
        lt_ui_draw_text(ui, x, r.y + (r.h - 16) / 2, s, c);
    } else {
        lt_ui_draw_text_in_f(ui, r, s, c, align, (int)LT_FONT_MONO, LT_UI_FS_XS);
    }
}

/* ---- scatter ---- */
static void scatter_draw(lt_app *app, lt_lxy *s, const lt_log *L, lt_rect r, long i0, long i1)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int lm = cell ? 56 : lt_ui_px(ui, 56), bm = cell ? 32 : lt_ui_px(ui, 40);
    int rm = s->z >= 0 ? (cell ? 72 : lt_ui_px(ui, 70)) : (cell ? 8 : lt_ui_px(ui, 14));
    lt_rect pl = lt_rect_make(r.x + lm, r.y + (cell ? 8 : lt_ui_px(ui, 10)), r.w - lm - rm,
                              r.h - bm - (cell ? 8 : lt_ui_px(ui, 10)));
    double xl = 1e300, xh = -1e300, yl = 1e300, yh = -1e300, zl = 1e300, zh = -1e300, st, v, pad;
    long i, n = 0, stride;
    char b[48];
    int k;
    lt_ui_draw_box(ui, r, th->table_bg, th->border_subtle, cell ? 0 : lt_ui_px(ui, 6));
    if (pl.w < 20 || pl.h < 20 || s->x < 0 || s->y < 0) { return; }
    for (i = i0; i <= i1; i++) {
        double x, y;
        if (!s->mask[i]) { continue; }
        x = LT_LOG_AT(L, i, s->x); y = LT_LOG_AT(L, i, s->y);
        if (x < xl) { xl = x; }
        if (x > xh) { xh = x; }
        if (y < yl) { yl = y; }
        if (y > yh) { yh = y; }
        if (s->z >= 0) {
            double z = LT_LOG_AT(L, i, s->z);
            if (z < zl) { zl = z; }
            if (z > zh) { zh = z; }
        }
        n++;
    }
    if (n == 0) {
        txt(ui, pl, "No points (check the filter)", th->muted, 1);
        return;
    }
    pad = (xh - xl) * 0.04;
    if (pad <= 0.0) { pad = 0.5; }
    xl -= pad; xh += pad;
    pad = (yh - yl) * 0.04;
    if (pad <= 0.0) { pad = 0.5; }
    yl -= pad; yh += pad;
    if (zh <= zl) { zh = zl + 1.0; }
    /* grid + ticks */
    if (!cell) { lt_ui_draw_vgrad(ui, pl, LT_RGBA(255, 255, 255, th->dark ? 5 : 0), LT_RGBA(0, 0, 0, th->dark ? 36 : 6)); }
    st = lt_graph_nice_step(yh - yl, pl.h / (cell ? 32 : lt_ui_px(ui, 40)) + 1);
    for (v = ceil(yl / st) * st; v <= yh; v += st) {
        int y = pl.y + pl.h - 1 - (int)((v - yl) / (yh - yl) * (double)(pl.h - 1));
        lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, y, cell ? th->table_grid : (th->dark ? LT_RGBA(255, 255, 255, 18) : LT_RGBA(0, 0, 0, 20)));
        ftick(b, sizeof(b), v, st);
        txt(ui, lt_rect_make(r.x, y - 8, lm - 6, 16), b, th->text2, 2);
    }
    st = lt_graph_nice_step(xh - xl, pl.w / (cell ? 80 : lt_ui_px(ui, 90)) + 1);
    for (v = ceil(xl / st) * st; v <= xh; v += st) {
        int x = pl.x + (int)((v - xl) / (xh - xl) * (double)(pl.w - 1));
        lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h, cell ? th->table_grid : (th->dark ? LT_RGBA(255, 255, 255, 14) : LT_RGBA(0, 0, 0, 16)));
        ftick(b, sizeof(b), v, st);
        txt(ui, lt_rect_make(x - 40, pl.y + pl.h + 2, 80, 16), b, th->text2, 1);
    }
    lt_ui_draw_rect(ui, pl, th->border);
    txt(ui, lt_rect_make(pl.x, r.y + r.h - (cell ? 16 : lt_ui_px(ui, 18)), pl.w, 16), L->names[s->x], th->text, 1);
    if (!cell) {
        lt_ui_draw_vtext(ui, (double)(r.x + lt_ui_px(ui, 10)), (double)(pl.y + pl.h / 2), L->names[s->y], 10.0 * (double)lt_ui_scale_pct(ui) / 100.0,
                         -1.57079632679, 1, 1, 1.1, th->text);
    } else {
        lt_ui_draw_text(ui, r.x + 2, r.y, L->names[s->y], th->text);
    }
    /* points (decimated to XY_MAXPTS) */
    stride = n / XY_MAXPTS + 1;
    lt_ui_clip_push(ui, pl);
    {
        long seen = 0;
        double rad = cell ? 0.0 : 1.6 * (double)lt_ui_scale_pct(ui) / 100.0;
        for (i = i0; i <= i1; i++) {
            double x, y;
            lt_color c;
            if (!s->mask[i]) { continue; }
            if ((seen++ % stride) != 0) { continue; }
            x = pl.x + (LT_LOG_AT(L, i, s->x) - xl) / (xh - xl) * (double)(pl.w - 1);
            y = pl.y + pl.h - 1 - (LT_LOG_AT(L, i, s->y) - yl) / (yh - yl) * (double)(pl.h - 1);
            c = s->z >= 0 ? lt_heat_color(0, (LT_LOG_AT(L, i, s->z) - zl) / (zh - zl)) : th->primary;
            if (cell) { lt_ui_draw_line(ui, x, y, x + 1.0, y, 1.0, c); }
            else { lt_ui_draw_circle_fill(ui, x, y, rad, (c & 0x00FFFFFFUL) | 0xD0000000UL); }
        }
    }
    lt_ui_clip_pop(ui);
    /* colour scale */
    if (s->z >= 0) {
        lt_rect cb = lt_rect_make(pl.x + pl.w + (cell ? 16 : lt_ui_px(ui, 14)), pl.y + (cell ? 16 : lt_ui_px(ui, 18)),
                                  cell ? 16 : lt_ui_px(ui, 12), pl.h - (cell ? 32 : lt_ui_px(ui, 36)));
        int steps = cb.h > 0 ? cb.h : 1, y;
        for (k = 0; k < (cell ? cb.h / 16 + 1 : steps); k++) {
            double f = cell ? (double)k / (double)(cb.h / 16 + 1) : (double)k / (double)steps;
            y = cb.y + cb.h - (cell ? (k + 1) * 16 : k + 1);
            lt_ui_draw_fill(ui, lt_rect_make(cb.x, y, cb.w, cell ? 16 : 1), lt_heat_color(0, f));
        }
        lt_fmt_double(b, sizeof(b), zh, 2);
        txt(ui, lt_rect_make(cb.x - (cell ? 8 : lt_ui_px(ui, 10)), cb.y - (cell ? 16 : lt_ui_px(ui, 17)), rm, cell ? 16 : lt_ui_px(ui, 16)), b, th->text2, 0);
        lt_fmt_double(b, sizeof(b), zl, 2);
        txt(ui, lt_rect_make(cb.x - (cell ? 8 : lt_ui_px(ui, 10)), cb.y + cb.h + (cell ? 0 : lt_ui_px(ui, 1)), rm, cell ? 16 : lt_ui_px(ui, 16)), b, th->text2, 0);
        txt(ui, lt_rect_make(cb.x - 30, r.y + r.h - (cell ? 16 : lt_ui_px(ui, 18)), rm, 16), L->names[s->z], th->muted, 0);
    }
}

/* ---- histogram ---- */
static unsigned long hsig(const lt_lxy *s, long i0, long i1)
{
    unsigned long h = 2166136261UL;
    double parts[12];
    int k;
    parts[0] = s->x; parts[1] = s->y; parts[2] = s->z; parts[3] = s->axes_src; parts[4] = s->table_sel;
    parts[5] = s->nbx; parts[6] = s->nby; parts[7] = s->mask_gen; parts[8] = s->log_gen;
    parts[9] = (double)i0; parts[10] = (double)i1; parts[11] = 0;
    for (k = 0; k < 12; k++) {
        h = (h ^ (unsigned long)(long)(parts[k] * 7.0 + 3.0)) * 16777619UL;
        h &= 0xFFFFFFFFUL;
    }
    return h;
}

/* axis values of a tune table constant (count values), 0 when unavailable */
static int table_axis(lt_app *app, const char *cname, double *out, int count)
{
    lt_pagesrc src = lt_app_pages(app);
    const lt_ini_const *c = cname != NULL ? lt_ini_find_const(app->ini, cname) : NULL;
    if (c == NULL || count <= 0) { return 0; }
    return lt_ini_const_get_array(app->ini, c, &src, out, (size_t)count) == count ? count : 0;
}

static void hist_build(lt_app *app, lt_lxy *s, const lt_log *L, long i0, long i1)
{
    double *xb = NULL, *yb = NULL;
    int nx = 0, ny = 1;
    if (s->h_ok) { lt_hist2d_free(&s->h); s->h_ok = 0; }
    s->herr[0] = '\0';
    if (s->x < 0) { return; }
    if (s->axes_src == 1 && app->ini != NULL && s->table_sel >= 0 && (size_t)s->table_sel < app->ini->n_tables) {
        const lt_ini_table *t = app->ini->tables[s->table_sel];
        nx = (int)t->x_size;
        xb = (double *)lt_calloc((size_t)(nx > 0 ? nx : 1), sizeof(double));
        if (table_axis(app, t->x_const, xb, nx) != nx) { nx = 0; }
        if (t->y_const != NULL && s->y >= 0) {
            ny = (int)t->y_size;
            yb = (double *)lt_calloc((size_t)(ny > 0 ? ny : 1), sizeof(double));
            if (table_axis(app, t->y_const, yb, ny) != ny) { ny = 0; }
        }
        if (nx <= 0 || ny <= 0) {
            lt_snprintf(s->herr, sizeof(s->herr), "Cannot read the axes of table %s", t->name);
            lt_free(xb); lt_free(yb);
            return;
        }
    } else {
        double lo = 1e300, hi = -1e300, ylo = 1e300, yhi = -1e300;
        long i;
        for (i = i0; i <= i1; i++) {
            double v;
            if (!s->mask[i]) { continue; }
            v = LT_LOG_AT(L, i, s->x);
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
            if (s->y >= 0) {
                v = LT_LOG_AT(L, i, s->y);
                if (v < ylo) { ylo = v; }
                if (v > yhi) { yhi = v; }
            }
        }
        if (lo > hi) { lo = 0.0; hi = 1.0; }
        if (ylo > yhi) { ylo = 0.0; yhi = 1.0; }
        nx = LT_CLAMP((int)s->nbx, 2, 64);
        xb = (double *)lt_calloc((size_t)nx, sizeof(double));
        lt_hist_auto_bins(lo, hi, nx, xb);
        if (s->y >= 0) {
            ny = LT_CLAMP((int)s->nby, 2, 64);
            yb = (double *)lt_calloc((size_t)ny, sizeof(double));
            lt_hist_auto_bins(ylo, yhi, ny, yb);
        }
    }
    lt_hist2d_init(&s->h, xb, nx, yb, ny);
    lt_hist2d_from_log(&s->h, L, s->x, s->y, s->z, s->mask, i0, i1);
    s->h_ok = 1;
    lt_free(xb);
    lt_free(yb);
}

static void hist_copy(lt_app *app, lt_lxy *s)
{
    lt_buf b;
    int ix, iy;
    char t[48];
    lt_buf_init(&b);
    for (iy = s->h.ny - 1; iy >= 0; iy--) {
        for (ix = 0; ix < s->h.nx; ix++) {
            size_t k = (size_t)iy * (size_t)s->h.nx + (size_t)ix;
            if (ix > 0) { lt_buf_putc(&b, '\t'); }
            if (s->h.hits[k] >= (long)s->min_hits && s->h.hits[k] > 0) {
                lt_fmt_double(t, sizeof(t), s->h.avg[k], 3);
                lt_buf_puts(&b, t);
            }
        }
        lt_buf_putc(&b, '\n');
    }
    lt_ui_clipboard_set(app->ui, b.data != NULL ? b.data : "");
    lt_buf_free(&b);
    lt_app_toast(app, 1, "Copied %d x %d values: paste them into the table editor (Ctrl+V)", s->h.nx, s->h.ny);
}

static void hist_draw(lt_app *app, lt_lxy *s, const lt_log *L, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int lm = cell ? 64 : lt_ui_px(ui, 60), bm = cell ? 32 : lt_ui_px(ui, 40);
    lt_rect g = lt_rect_make(r.x + lm, r.y + (cell ? 0 : lt_ui_px(ui, 8)), r.w - lm - (cell ? 8 : lt_ui_px(ui, 10)),
                             r.h - bm - (cell ? 0 : lt_ui_px(ui, 8)));
    int ix, iy, cw, chh;
    char b[48];
    lt_ui_draw_box(ui, r, th->table_bg, th->border_subtle, cell ? 0 : lt_ui_px(ui, 6));
    if (!s->h_ok || g.w < 20 || g.h < 16) {
        txt(ui, r, s->herr[0] ? s->herr : "Pick an X channel", th->muted, 1);
        return;
    }
    cw = g.w / s->h.nx;
    chh = g.h / s->h.ny;
    if (cell) { chh = chh < 16 ? 16 : chh / 16 * 16; cw = cw / 8 * 8; }
    if (cw < 4 || chh < 4) { txt(ui, r, "Too many bins for this size", th->muted, 1); return; }
    for (iy = 0; iy < s->h.ny; iy++) {
        int y = g.y + (s->h.ny - 1 - iy) * chh;
        for (ix = 0; ix < s->h.nx; ix++) {
            size_t k = (size_t)iy * (size_t)s->h.nx + (size_t)ix;
            lt_rect c = lt_rect_make(g.x + ix * cw, y, cw - (cell ? 0 : 1), chh - (cell ? 0 : 1));
            long hits = s->h.hits[k];
            if (hits > 0 && hits >= (long)s->min_hits) {
                double f = (s->h.avg[k] - s->h.zmin) / (s->h.zmax > s->h.zmin ? s->h.zmax - s->h.zmin : 1.0);
                lt_color bg = lt_heat_color(0, f), fg = lt_color_contrast(bg);
                lt_ui_draw_fill(ui, c, bg);
                lt_fmt_double(b, sizeof(b), s->h.avg[k], s->z < 0 ? 0 : (fabs(s->h.avg[k]) >= 100.0 ? 0 : 2));
                if (cell) {                 /* fewer decimals until it fits with a gap, else no text */
                    int d = s->z < 0 ? 0 : 2;
                    while (d > 0 && (int)strlen(b) * 8 > cw - 8) {
                        d--;
                        lt_fmt_double(b, sizeof(b), s->h.avg[k], d);
                    }
                    if ((int)strlen(b) * 8 <= cw - 8) { txt(ui, c, b, fg, 1); }
                }
                else {
                    int big = chh >= lt_ui_px(ui, 30);
                    lt_ui_draw_text_in_f(ui, lt_rect_make(c.x, c.y, c.w, big ? c.h * 3 / 5 : c.h), b, fg, 1,
                                         (int)LT_FONT_MONO_MEDIUM, cw >= lt_ui_px(ui, 44) ? LT_UI_FS_SM : 9);
                    if (big && s->z >= 0) {
                        lt_snprintf(b, sizeof(b), "%ld", hits);
                        lt_ui_draw_text_in_f(ui, lt_rect_make(c.x, c.y + c.h * 3 / 5 - lt_ui_px(ui, 2), c.w, c.h * 2 / 5), b,
                                             (fg & 0x00FFFFFFUL) | 0xA0000000UL, 1, (int)LT_FONT_MONO, 8);
                    }
                }
            } else {
                lt_ui_draw_fill(ui, c, hits > 0 ? th->table_hover : th->table_cell);
            }
        }
        lt_fmt_double(b, sizeof(b), s->h.ny > 1 ? s->h.yb[iy] : 0.0, 1);
        if (s->h.ny > 1 && (chh >= (cell ? 16 : lt_ui_px(ui, 14)))) {
            txt(ui, lt_rect_make(r.x + 2, y, lm - 6, chh), b, th->text2, 2);
        }
    }
    for (ix = 0; ix < s->h.nx; ix++) {
        int every = cw >= (cell ? 48 : lt_ui_px(ui, 40)) ? 1 : (cell ? 48 : lt_ui_px(ui, 40)) / (cw > 0 ? cw : 1) + 1;
        if (ix % every != 0) { continue; }
        lt_fmt_double(b, sizeof(b), s->h.xb[ix], s->h.xb[s->h.nx - 1] - s->h.xb[0] >= 100.0 ? 0 : 1);
        txt(ui, lt_rect_make(g.x + ix * cw, g.y + s->h.ny * chh + 2, cw * every, 16), b, th->text2, every > 1 ? 0 : 1);
    }
    txt(ui, lt_rect_make(g.x, r.y + r.h - (cell ? 16 : lt_ui_px(ui, 18)), g.w, 16), L->names[s->x], th->text, 1);
    if (s->y >= 0 && !cell) {
        lt_ui_draw_vtext(ui, (double)(r.x + lt_ui_px(ui, 10)), (double)(g.y + g.h / 2), L->names[s->y],
                         10.0 * (double)lt_ui_scale_pct(ui) / 100.0, -1.57079632679, 1, 1, 1.1, th->text);
    }
}

void lt_lxy_draw(lt_app *app, lt_lxy *s, const lt_log *L, int log_gen, int mode, double win_t0,
                 double win_t1)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    xy_ud ud;
    int w1 = -1, wr[8], sel, apply = 0;
    long i0, i1;
    char b[96];
    if (L == NULL || L->n == 0 || L->n_ch == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Open a log to analyse it.", th->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    if (!s->inited || s->log_gen != log_gen) {
        if (!s->inited || s->x >= L->n_ch || s->y >= L->n_ch || s->z >= L->n_ch) { xy_defaults(s, L); }
    }
    ud.L = L; ud.ini = app->ini;
    mask_update(s, L, log_gen, 0);
    /* controls */
    wr[0] = 20; wr[1] = -1; wr[2] = 20; wr[3] = -1; wr[4] = mode == 1 ? 46 : 20; wr[5] = -1;
    lt_ui_layout_row(ui, 6, wr, 0);
    lt_ui_label(ui, "X");
    sel = s->x;
    if (lt_ui_combo_cb(ui, "lx_x", &sel, L->n_ch, ch_get, &ud)) { s->x = sel; }
    lt_ui_label(ui, "Y");
    sel = s->y + 1;
    if (lt_ui_combo_cb(ui, "lx_y", &sel, L->n_ch + 1, ch_get_none, &ud)) { s->y = sel - 1; }
    lt_ui_label(ui, mode == 1 ? "Color" : "Z");
    sel = s->z + 1;
    if (lt_ui_combo_cb(ui, "lx_z", &sel, L->n_ch + 1, mode == 1 ? ch_get_none : ch_get_count, &ud)) { s->z = sel - 1; }
    if (mode == 2) {
        static const char *const srcs[] = { "Auto bins", "Table axes" };
        wr[0] = 46; wr[1] = 130; wr[2] = -1; wr[3] = 70; wr[4] = 70;
        lt_ui_layout_row(ui, 5, wr, 0);
        lt_ui_label(ui, "Axes");
        if (lt_ui_combo(ui, "lx_src", &s->axes_src, srcs, 2) && s->axes_src == 1 && app->ini != NULL &&
            (size_t)s->table_sel < app->ini->n_tables) {
            s->table_sel = s->table_sel < 0 ? 0 : s->table_sel;
        }
        if (s->axes_src == 1 && app->ini != NULL && app->ini->n_tables > 0) {
            sel = s->table_sel;
            if (lt_ui_combo_cb(ui, "lx_tb", &sel, (int)app->ini->n_tables, tb_get, &ud)) {
                const lt_ini_table *t = app->ini->tables[sel];
                int cx = t->x_channel != NULL ? lt_log_channel_index(L, t->x_channel) : -1;
                int cy = t->y_channel != NULL ? lt_log_channel_index(L, t->y_channel) : -1;
                s->table_sel = sel;
                if (cx >= 0) { s->x = cx; }
                if (cy >= 0) { s->y = cy; }
            }
            lt_ui_label(ui, ""); lt_ui_label(ui, "");
        } else if (s->axes_src == 1) {
            lt_ui_label_ex(ui, "No tables in this definition", th->muted, LT_UI_ALIGN_LEFT);
            lt_ui_label(ui, ""); lt_ui_label(ui, "");
        } else {
            lt_ui_label_ex(ui, "Bins X / Y", th->muted, LT_UI_ALIGN_RIGHT);
            lt_ui_number(ui, "lx_nx", &s->nbx, 2, 64, 1, 0);
            lt_ui_number(ui, "lx_ny", &s->nby, 2, 64, 1, 0);
        }
    }
    wr[0] = 46; wr[1] = -1; wr[2] = 70; wr[3] = mode == 2 ? 70 : 150; wr[4] = mode == 2 ? 70 : 1;
    wr[5] = mode == 2 ? 110 : 1;
    lt_ui_layout_row(ui, mode == 2 ? 6 : 4, wr, 0);
    lt_ui_label(ui, "Filter");
    if (lt_ui_text(ui, "lx_f", s->filter, sizeof(s->filter), 0) & LT_UI_SUBMIT) { apply = 1; }
    if (lt_ui_button(ui, "Apply")) { apply = 1; }
    if (mode == 2) {
        lt_ui_label_ex(ui, "Min hits", th->muted, LT_UI_ALIGN_RIGHT);
        lt_ui_number(ui, "lx_mh", &s->min_hits, 1, 100000, 1, 0);
        if (lt_ui_button_ex(ui, "Copy values", s->h_ok ? 0 : LT_UI_BTN_DISABLED) && s->h_ok) { hist_copy(app, s); }
    } else {
        lt_ui_checkbox(ui, "Visible window only", &s->window_only);
    }
    if (apply) { mask_update(s, L, log_gen, 1); }
    i0 = 0; i1 = L->n - 1;
    if (s->window_only && mode == 1) { i0 = lt_lg_find(L, win_t0); i1 = lt_lg_find(L, win_t1); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->err[0] != '\0') { lt_ui_label_ex(ui, s->err, th->error, LT_UI_ALIGN_LEFT); }
    else {
        if (mode == 2 && s->h_ok) {
            lt_snprintf(b, sizeof(b), "%ld of %ld samples pass the filter, %ld binned", s->passed, L->n, s->h.n_binned);
        } else {
            lt_snprintf(b, sizeof(b), "%ld of %ld samples pass the filter", s->passed, L->n);
        }
        lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
    }
    if (mode == 2) {
        unsigned long sg = hsig(s, i0, i1);
        if (!s->h_ok || sg != s->h_sig) {
            hist_build(app, s, L, i0, i1);
            s->h_sig = sg;
        }
    }
    lt_ui_layout_row(ui, 1, &w1, -1);
    {
        lt_rect pr = lt_ui_layout_next(ui);
        if (mode == 1) { scatter_draw(app, s, L, pr, i0, i1); }
        else { hist_draw(app, s, L, pr); }
    }
}
