/* tables_table.c - table editor tab: heat-mapped grid, live cursor, edit operations,
 * clipboard, undo/redo, 3D surface view.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "tables_priv.h"

#define TRAIL_N 20

typedef struct tbl_tab {
    lt_tblview v;
    int loaded;
    char msg[160];
    const lt_ini_table *t;
    int zdig, xdig, ydig;
    double lo, hi;                /* constant limits (display units) */
    int show3d, follow, shading, ybottom;
    unsigned char *sel;           /* rows*cols mask in data coordinates */
    size_t sel_cap;
    int cur_r, cur_c;             /* cursor in data coordinates */
    int nsel;
    double *clip; size_t clip_h, clip_w;
    int trail_r[TRAIL_N], trail_c[TRAIL_N], ntrail;
    int live_ok, live_r, live_c;
    double live_x, live_y;
    unsigned long last_frames;
    int goto_live;
    int scale_open, scale_mode; double scale_val;
    int rebin_open, rebin_interp; char xtxt[1024], ytxt[1024];
    double yaw, pitch;
    int drag3d, lmx, lmy, prev_down;
    int frame;
    lt_color *cellcol;            /* rows*cols colours for the 3D view */
    lt_color col_edge, col_live, col_cur, col_text;
    lt_rect r3d;
    char xlab[48], ylab[48];
} tbl_tab;

static int drow(const tbl_tab *st, int dr)
{
    return st->ybottom ? (int)st->v.rows - 1 - dr : dr;
}

static void tbl_free_data(tbl_tab *st)
{
    lt_tblview_free(&st->v);
    lt_free(st->sel); st->sel = NULL; st->sel_cap = 0;
    lt_free(st->cellcol); st->cellcol = NULL;
}

static void tbl_load(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    int rc;
    size_t n;
    lt_tblview_free(&st->v);
    lt_tblview_init(&st->v);
    st->t = lt_ini_find_table(app->ini, tab->target);
    st->loaded = 0;
    if (st->t == NULL) { lt_snprintf(st->msg, sizeof(st->msg), "Unknown table '%s'", tab->target); return; }
    rc = lt_tblview_load_table(&st->v, app->ini, st->t, &src);
    if (rc < 0) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load table: %s", lt_err_get());
        return;
    }
    st->zdig = lt_tabs_const_digits(app->ini, st->t->z_const);
    st->xdig = lt_tabs_const_digits(app->ini, st->t->x_const);
    st->ydig = lt_tabs_const_digits(app->ini, st->t->y_const);
    lt_tblview_z_range(&st->v, app->ini, &st->lo, &st->hi);
    n = st->v.rows * st->v.cols;
    if (n > st->sel_cap) {
        lt_free(st->sel); lt_free(st->cellcol);
        st->sel = (unsigned char *)lt_calloc(n, 1);
        st->cellcol = (lt_color *)lt_calloc(n, sizeof(lt_color));
        st->sel_cap = n;
    }
    memset(st->sel, 0, st->sel_cap);
    st->nsel = 0;
    st->xlab[0] = st->ylab[0] = '\0';
    if (st->t->x_label != NULL && st->t->x_label[0] != '{') { lt_strlcpy(st->xlab, st->t->x_label, sizeof(st->xlab)); }
    if (st->t->y_label != NULL && st->t->y_label[0] != '{') { lt_strlcpy(st->ylab, st->t->y_label, sizeof(st->ylab)); }
    st->loaded = 1;
}

/* reload when the page image changed behind our back (burn, tune load, autotune apply) */
static void tbl_check_external(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    const lt_ini_const *zc;
    lt_pagesrc src;
    double *tmp;
    size_t n, i;
    int cnt, diff = 0;
    if (!st->loaded || st->t->rows_size_const != NULL || lt_tblview_is_dirty(&st->v)) { return; }
    zc = lt_ini_find_const(app->ini, st->t->z_const);
    if (zc == NULL) { return; }
    n = st->v.rows * st->v.cols;
    tmp = (double *)lt_calloc(n + 1, sizeof(double));
    src = lt_app_pages(app);
    cnt = lt_ini_const_get_array(app->ini, zc, &src, tmp, n);
    if (cnt != (int)n) { diff = 1; }
    for (i = 0; i < n && !diff; i++) {
        if (fabs(tmp[i] - st->v.z[i]) > 1e-9) { diff = 1; }
    }
    lt_free(tmp);
    if (diff) { tbl_load(app, tab, st); }
}

static void tbl_commit(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    int n = lt_tblview_commit(&st->v, app->ini, &dst);
    if (n > 0) { lt_app_edited(app); tab->dirty = 1; }
    else if (n < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); }
}

/* ---- selection helpers ---- */
static size_t sel_cells(tbl_tab *st, lt_tcell **out)
{
    size_t rows = st->v.rows, cols = st->v.cols, y, x, n = 0;
    lt_tcell *c = (lt_tcell *)lt_calloc(rows * cols + 1, sizeof(lt_tcell));
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            if (st->sel[y * cols + x]) { c[n].y = y; c[n].x = x; n++; }
        }
    }
    if (n == 0 && st->cur_r >= 0 && st->cur_c >= 0 && (size_t)st->cur_r < rows &&
        (size_t)st->cur_c < cols) {
        c[0].y = (size_t)st->cur_r; c[0].x = (size_t)st->cur_c; n = 1;
    }
    *out = c;
    return n;
}

static void tbl_fix(tbl_tab *st, const lt_tcell *sel, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        double v = lt_tblview_get(&st->v, sel[i].y, sel[i].x), w = v;
        if (st->hi > st->lo) { w = w < st->lo ? st->lo : (w > st->hi ? st->hi : w); }
        w = lt_tabs_round(w, st->zdig);
        if (w != v) { lt_tblview_set(&st->v, sel[i].y, sel[i].x, w); }
    }
}

enum { OP_EQUAL = 1, OP_ADD, OP_SCALE, OP_INTERP, OP_INTERP_H, OP_INTERP_V, OP_SMOOTH,
       OP_FILL_R, OP_FILL_D, OP_SET };

static void tbl_op(lt_app *app, lt_app_tab *tab, tbl_tab *st, int op, double arg)
{
    lt_tcell *sel;
    size_t n = sel_cells(st, &sel), miny, maxy, minx, maxx, i, y, x;
    size_t cols = st->v.cols;
    if (n == 0) { lt_free(sel); return; }
    lt_tbl_bbox(sel, n, &miny, &maxy, &minx, &maxx);
    switch (op) {
    case OP_EQUAL: {
        double s = 0.0;
        for (i = 0; i < n; i++) { s += lt_tblview_get(&st->v, sel[i].y, sel[i].x); }
        lt_tblview_set_equal(&st->v, sel, n, lt_tabs_round(s / (double)n, st->zdig));
        break; }
    case OP_SET: lt_tblview_set_equal(&st->v, sel, n, arg); break;
    case OP_ADD: lt_tblview_offset(&st->v, sel, n, arg); break;
    case OP_SCALE: lt_tblview_scale(&st->v, sel, n, arg); break;
    case OP_INTERP:
        if (maxy == miny) { lt_tblview_interp_linear(&st->v, sel, n, LT_TBL_AXIS_ROW); }
        else if (maxx == minx) { lt_tblview_interp_linear(&st->v, sel, n, LT_TBL_AXIS_COL); }
        else { lt_tblview_interp2d(&st->v, sel, n); }
        break;
    case OP_INTERP_H: lt_tblview_interp_linear(&st->v, sel, n, LT_TBL_AXIS_ROW); break;
    case OP_INTERP_V: lt_tblview_interp_linear(&st->v, sel, n, LT_TBL_AXIS_COL); break;
    case OP_SMOOTH: lt_tblview_smooth(&st->v, sel, n, 1.0); break;
    case OP_FILL_R:
    case OP_FILL_D: {
        lt_tblview_push_undo(&st->v);
        for (y = miny; y <= maxy; y++) {
            for (x = minx; x <= maxx; x++) {
                size_t sy, sx;
                if (!st->sel[y * cols + x] && n > 1) { continue; }
                if (op == OP_FILL_R) { sy = y; sx = minx; }
                else { sy = st->ybottom ? maxy : miny; sx = x; }
                lt_tblview_set(&st->v, y, x, lt_tblview_get(&st->v, sy, sx));
            }
        }
        break; }
    default: break;
    }
    tbl_fix(st, sel, n);
    lt_free(sel);
    tbl_commit(app, tab, st);
}

static void tbl_undo_redo(lt_app *app, lt_app_tab *tab, tbl_tab *st, int redo)
{
    lt_bool ok = redo ? lt_tblview_redo(&st->v) : lt_tblview_undo(&st->v);
    if (ok) { tbl_commit(app, tab, st); }
    else { lt_app_toast(app, 0, "Nothing to %s", redo ? "redo" : "undo"); }
}

static void tbl_revert(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    /* undo everything back to the state at open: walk the undo ring */
    int guard = 0;
    while (lt_tblview_undo(&st->v) && guard++ < LT_TBLVIEW_UNDO_MAX) { }
    tbl_commit(app, tab, st);
    lt_app_toast(app, 0, "Reverted to the values at open (undo history)");
}

/* ---- clipboard ---- */
static void tbl_bbox_disp(const tbl_tab *st, int *r0, int *c0, int *r1, int *c1)
{
    int rows = (int)st->v.rows, cols = (int)st->v.cols, y, x, first = 1;
    *r0 = *c0 = 0; *r1 = *c1 = 0;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            if (st->sel[y * cols + x]) {
                int dr = st->ybottom ? rows - 1 - y : y;
                if (first) { *r0 = *r1 = dr; *c0 = *c1 = x; first = 0; }
                else {
                    if (dr < *r0) { *r0 = dr; }
                    if (dr > *r1) { *r1 = dr; }
                    if (x < *c0) { *c0 = x; }
                    if (x > *c1) { *c1 = x; }
                }
            }
        }
    }
    if (first) {
        *r0 = *r1 = st->ybottom ? rows - 1 - st->cur_r : st->cur_r;
        *c0 = *c1 = st->cur_c;
    }
}

static void tbl_copy(lt_app *app, tbl_tab *st)
{
    int r0, c0, r1, c1, r, c, cols = (int)st->v.cols;
    lt_buf b;
    char tmp[48];
    lt_buf_init(&b);
    tbl_bbox_disp(st, &r0, &c0, &r1, &c1);
    st->clip_h = (size_t)(r1 - r0 + 1); st->clip_w = (size_t)(c1 - c0 + 1);
    lt_free(st->clip);
    st->clip = (double *)lt_calloc(st->clip_h * st->clip_w, sizeof(double));
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            double v = lt_tblview_get(&st->v, (size_t)drow(st, r), (size_t)c);
            st->clip[(size_t)(r - r0) * st->clip_w + (size_t)(c - c0)] = v;
            lt_tabs_fmt(tmp, sizeof(tmp), v, st->zdig);
            lt_buf_puts(&b, tmp);
            if (c < c1) { lt_buf_puts(&b, "\t"); }
        }
        lt_buf_puts(&b, "\n");
    }
    (void)cols;
    lt_ui_clipboard_set(app->ui, b.data != NULL ? (const char *)b.data : "");
    lt_buf_free(&b);
    lt_app_toast(app, 0, "Copied %dx%d cells", r1 - r0 + 1, c1 - c0 + 1);
}

static void tbl_paste(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    char *txt = lt_ui_clipboard_get(app->ui);
    double *blk = NULL, row[64];
    size_t bh = 0, bw = 0;
    int r0, c0, r1, c1, r, c, rows = (int)st->v.rows, cols = (int)st->v.cols;
    if (txt != NULL && txt[0] != '\0') {
        const char *p = txt;
        double *acc = (double *)lt_calloc(64 * 256, sizeof(double));
        int ok = 1;
        while (*p != '\0' && ok) {
            char line[1024];
            size_t k = 0;
            int cnt;
            while (*p != '\0' && *p != '\n') { if (k + 1 < sizeof(line)) { line[k++] = *p; } p++; }
            if (*p == '\n') { p++; }
            line[k] = '\0';
            if (k == 0 || (k == 1 && line[0] == '\r')) { continue; }
            cnt = lt_tabs_parse_list(line, row, 64);
            if (cnt <= 0 || bh >= 256) { ok = 0; break; }
            if ((size_t)cnt > bw) { bw = (size_t)cnt; }
            memcpy(&acc[bh * 64], row, (size_t)cnt * sizeof(double));
            if ((size_t)cnt < 64) { memset(&acc[bh * 64 + (size_t)cnt], 0, (64 - (size_t)cnt) * sizeof(double)); }
            bh++;
        }
        if (ok && bh > 0 && bw > 0) {
            size_t i, j;
            blk = (double *)lt_calloc(bh * bw, sizeof(double));
            for (i = 0; i < bh; i++) { for (j = 0; j < bw; j++) { blk[i * bw + j] = acc[i * 64 + j]; } }
        }
        lt_free(acc);
    }
    lt_free(txt);
    if (blk == NULL && st->clip != NULL) {
        bh = st->clip_h; bw = st->clip_w;
        blk = (double *)lt_calloc(bh * bw, sizeof(double));
        memcpy(blk, st->clip, bh * bw * sizeof(double));
    }
    if (blk == NULL) { lt_app_toast(app, 2, "Clipboard is empty"); return; }
    tbl_bbox_disp(st, &r0, &c0, &r1, &c1);
    lt_tblview_push_undo(&st->v);
    if (bh == 1 && bw == 1 && st->nsel > 1) {
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                if (st->sel[(size_t)r * (size_t)cols + (size_t)c]) {
                    lt_tblview_set(&st->v, (size_t)r, (size_t)c, blk[0]);
                }
            }
        }
    } else {
        size_t i, j;
        for (i = 0; i < bh; i++) {
            for (j = 0; j < bw; j++) {
                int dr = r0 + (int)i, dc = c0 + (int)j;
                if (dr < rows && dc < cols) {
                    lt_tblview_set(&st->v, (size_t)drow(st, dr), (size_t)dc, blk[i * bw + j]);
                }
            }
        }
    }
    {
        lt_tcell *all = (lt_tcell *)lt_calloc((size_t)(rows * cols), sizeof(lt_tcell));
        size_t y, x, n = 0;
        for (y = 0; y < (size_t)rows; y++) { for (x = 0; x < (size_t)cols; x++) { all[n].y = y; all[n].x = x; n++; } }
        tbl_fix(st, all, n);
        lt_free(all);
    }
    lt_free(blk);
    tbl_commit(app, tab, st);
}

/* ---- re-bin ---- */
static void rebin_prefill(tbl_tab *st)
{
    size_t i;
    char b[32];
    st->xtxt[0] = st->ytxt[0] = '\0';
    for (i = 0; i < st->v.cols; i++) {
        lt_tabs_fmt(b, sizeof(b), st->v.x[i], st->xdig);
        if (strlen(st->xtxt) + strlen(b) + 3 < sizeof(st->xtxt)) { strcat(st->xtxt, i ? ", " : ""); strcat(st->xtxt, b); }
    }
    for (i = 0; i < st->v.rows && st->v.y != NULL; i++) {
        lt_tabs_fmt(b, sizeof(b), st->v.y[i], st->ydig);
        if (strlen(st->ytxt) + strlen(b) + 3 < sizeof(st->ytxt)) { strcat(st->ytxt, i ? ", " : ""); strcat(st->ytxt, b); }
    }
}

static void rebin_apply(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    double nx[128], ny[128], *out;
    int cx = lt_tabs_parse_list(st->xtxt, nx, 128), cy = lt_tabs_parse_list(st->ytxt, ny, 128);
    size_t y, x;
    if (cx != (int)st->v.cols || cy != (int)st->v.rows) {
        lt_app_toast(app, 3, "Need exactly %d X and %d Y bins", (int)st->v.cols, (int)st->v.rows);
        return;
    }
    out = (double *)lt_calloc(st->v.rows * st->v.cols, sizeof(double));
    if (lt_tbl_rebin(st->v.x, st->v.cols, st->v.y, st->v.rows, st->v.z, nx, st->v.cols, ny,
                     st->v.rows, st->rebin_interp ? LT_TRUE : LT_FALSE, out) < 0) {
        lt_app_toast(app, 3, "Re-bin failed: %s", lt_err_get());
        lt_free(out);
        return;
    }
    lt_tblview_push_undo(&st->v);
    for (x = 0; x < st->v.cols; x++) { lt_tblview_set_x(&st->v, x, nx[x]); }
    for (y = 0; y < st->v.rows; y++) { lt_tblview_set_y(&st->v, y, ny[y]); }
    for (y = 0; y < st->v.rows; y++) {
        for (x = 0; x < st->v.cols; x++) { lt_tblview_set(&st->v, y, x, lt_tabs_round(out[y * st->v.cols + x], st->zdig)); }
    }
    lt_free(out);
    tbl_commit(app, tab, st);
    lt_app_toast(app, 1, "Table re-binned");
}

/* ---- 3D ---- */
static void proj(const tbl_tab *st, double x, double y, double z, lt_rect r, double *sx,
                 double *sy, double *depth)
{
    double cy_ = cos(st->yaw), sy_ = sin(st->yaw), cp = cos(st->pitch), sp = sin(st->pitch);
    double x1 = x * cy_ - y * sy_, y1 = x * sy_ + y * cy_;
    double d = y1 * cp - z * sp, z2 = y1 * sp + z * cp;
    double s = (double)(r.w < r.h ? r.w : r.h) * 0.62;
    *sx = r.x + r.w * 0.5 + x1 * s;
    *sy = r.y + r.h * 0.52 - z2 * s;
    *depth = d;
}

static void draw3d(lt_gfx *g, lt_rect r, void *user)
{
    tbl_tab *st = (tbl_tab *)user;
    int rows = (int)st->v.rows, cols = (int)st->v.cols, i0, j0, di, dj, qi, qj;
    double span, dmax = -1e30, cdep, px, py;
    int ci, cj;
    if (rows < 2 || cols < 2) { return; }
    {
        double zlo = 1e300, zhi = -1e300;
        size_t k;
        for (k = 0; k < st->v.rows * st->v.cols; k++) {
            if (st->v.z[k] < zlo) { zlo = st->v.z[k]; }
            if (st->v.z[k] > zhi) { zhi = st->v.z[k]; }
        }
        st->r3d = r;
        /* use data range for the height so small variations are visible */
        span = zhi > zlo ? zhi - zlo : 1.0;
        /* far corner */
        i0 = 0; j0 = 0;
        for (ci = 0; ci < 2; ci++) {
            for (cj = 0; cj < 2; cj++) {
                double sx, sy;
                proj(st, (double)cj - 0.5, (double)ci - 0.5, 0.0, r, &sx, &sy, &cdep);
                if (cdep > dmax) { dmax = cdep; i0 = ci ? rows - 1 : 0; j0 = cj ? cols - 1 : 0; }
            }
        }
        di = i0 == 0 ? 1 : -1;
        dj = j0 == 0 ? 1 : -1;
        for (qi = 0; qi < rows - 1; qi++) {
            for (qj = 0; qj < cols - 1; qj++) {
                double xy[8], dd;
                int ri = i0 == 0 ? qi : rows - 2 - qi, rj = j0 == 0 ? qj : cols - 2 - qj;
                int cc[4][2];
                lt_color col;
                int k;
                double avg = 0.0;
                cc[0][0] = ri; cc[0][1] = rj; cc[1][0] = ri; cc[1][1] = rj + 1;
                cc[2][0] = ri + 1; cc[2][1] = rj + 1; cc[3][0] = ri + 1; cc[3][1] = rj;
                for (k = 0; k < 4; k++) {
                    double v = st->v.z[(size_t)cc[k][0] * (size_t)cols + (size_t)cc[k][1]];
                    double zz = (v - zlo) / span * 0.75 - 0.35;
                    avg += v;
                    proj(st, (double)cc[k][1] / (double)(cols - 1) - 0.5,
                         (double)cc[k][0] / (double)(rows - 1) - 0.5, zz, r, &xy[k * 2],
                         &xy[k * 2 + 1], &dd);
                }
                col = st->cellcol[(size_t)ri * (size_t)cols + (size_t)rj];
                lt_gfx_fill_poly(g, xy, 4, col);
                for (k = 0; k < 4; k++) {
                    int n2 = (k + 1) & 3;
                    lt_gfx_line(g, xy[k * 2], xy[k * 2 + 1], xy[n2 * 2], xy[n2 * 2 + 1], 1.0, st->col_edge);
                }
                (void)avg;
            }
        }
        (void)di; (void)dj;
        /* cursor and live markers */
        for (k = 0; k < 2; k++) {
            int mr = k ? st->live_r : st->cur_r, mc = k ? st->live_c : st->cur_c;
            if ((k == 1 && !st->live_ok) || mr < 0 || mc < 0 || mr >= rows || mc >= cols) { continue; }
            {
                double v = st->v.z[(size_t)mr * (size_t)cols + (size_t)mc], dd;
                proj(st, (double)mc / (double)(cols - 1) - 0.5, (double)mr / (double)(rows - 1) - 0.5,
                     (v - zlo) / span * 0.75 - 0.35, r, &px, &py, &dd);
                lt_gfx_fill_circle(g, px, py, 4.0, k ? st->col_live : st->col_cur);
            }
        }
        {
            char b[64];
            lt_snprintf(b, sizeof(b), "Z %g..%g", zlo, zhi);
            lt_gfx_text(g, r.x + 4, r.y + r.h - 16, b, st->col_text, 1);
            lt_gfx_text(g, r.x + 4, r.y + 2, st->xlab[0] ? st->xlab : "X", st->col_text, 1);
            lt_snprintf(b, sizeof(b), "Y: %s", st->ylab[0] ? st->ylab : "Y");
            lt_gfx_text(g, r.x + 4, r.y + 18, b, st->col_text, 1);
        }
    }
}

/* ---- live cursor ---- */
static void tbl_live(lt_app *app, tbl_tab *st)
{
    size_t xi = 0, yi = 0;
    st->live_ok = 0;
    if (lt_tabs_live_index(app, st->t->x_channel, st->v.x, st->v.cols, &st->live_x, &xi)) {
        st->live_ok = 1;
        if (st->v.y != NULL && st->t->y_channel != NULL &&
            lt_tabs_live_index(app, st->t->y_channel, st->v.y, st->v.rows, &st->live_y, &yi)) { }
        st->live_c = (int)xi; st->live_r = (int)yi;
    }
    if (st->live_ok && app->frames_ok != st->last_frames) {
        if (st->ntrail == 0 || st->trail_r[0] != st->live_r || st->trail_c[0] != st->live_c) {
            int k;
            for (k = TRAIL_N - 1; k > 0; k--) { st->trail_r[k] = st->trail_r[k - 1]; st->trail_c[k] = st->trail_c[k - 1]; }
            st->trail_r[0] = st->live_r; st->trail_c[0] = st->live_c;
            if (st->ntrail < TRAIL_N) { st->ntrail++; }
        }
    }
    if (!st->live_ok) { st->ntrail = 0; }
    st->last_frames = app->frames_ok;
}

/* ---- keys ---- */
static double small_step(const tbl_tab *st)
{
    double s = 1.0;
    int i;
    for (i = 0; i < st->zdig && i < 2; i++) { s /= 10.0; }
    return s;
}

static void tbl_keys(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    double ss = small_step(st);
    if (lt_ui_text_focused(ui)) { return; }
    if (lt_ui_key_pressed(ui, 'c', LT_MOD_CTRL)) { tbl_copy(app, st); }
    if (lt_ui_key_pressed(ui, 'v', LT_MOD_CTRL)) { tbl_paste(app, tab, st); }
    if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL)) { tbl_undo_redo(app, tab, st, 0); }
    if (lt_ui_key_pressed(ui, 'y', LT_MOD_CTRL) || lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL | LT_MOD_SHIFT)) {
        tbl_undo_redo(app, tab, st, 1);
    }
    if (lt_ui_key_pressed(ui, 'r', LT_MOD_CTRL)) { tbl_op(app, tab, st, OP_FILL_R, 0); }
    if (lt_ui_key_pressed(ui, 'd', LT_MOD_CTRL)) { tbl_op(app, tab, st, OP_FILL_D, 0); }
    if (lt_ui_key_pressed(ui, '=', 0)) { tbl_op(app, tab, st, OP_EQUAL, 0); }
    if (lt_ui_key_pressed(ui, '=', LT_MOD_SHIFT) || lt_tabs_key(ui, '+')) { tbl_op(app, tab, st, OP_ADD, 1.0); }
    if (lt_tabs_key(ui, '-') || lt_tabs_key(ui, '_')) { tbl_op(app, tab, st, OP_ADD, -1.0); }
    if (lt_tabs_key(ui, '>')) { tbl_op(app, tab, st, OP_ADD, 1.0); }
    if (lt_tabs_key(ui, '<')) { tbl_op(app, tab, st, OP_ADD, -1.0); }
    if (lt_tabs_key(ui, 'q')) { tbl_op(app, tab, st, OP_ADD, ss); }
    if (lt_tabs_key(ui, 'w')) { tbl_op(app, tab, st, OP_ADD, -ss); }
    if (lt_tabs_key(ui, '*')) { st->scale_open = 1; }
    if (lt_tabs_key(ui, '/')) { tbl_op(app, tab, st, OP_INTERP, 0); }
    if (lt_tabs_key(ui, 'h')) { tbl_op(app, tab, st, OP_INTERP_H, 0); }
    if (lt_tabs_key(ui, 'v')) { tbl_op(app, tab, st, OP_INTERP_V, 0); }
    if (lt_tabs_key(ui, 's')) { tbl_op(app, tab, st, OP_SMOOTH, 0); }
    if (lt_tabs_key(ui, 'f')) { st->follow = !st->follow; }
    if (lt_tabs_key(ui, 'g')) { st->goto_live = 1; }
}

/* ---- UI pieces ---- */
static int tb(lt_ui *ui, const char *label, const char *tip, int on)
{
    int fl = LT_UI_BTN_FLAT | (on ? LT_UI_BTN_SELECTED : 0);
    int hit = lt_ui_button_ex(ui, label, fl);
    if (tip != NULL) { lt_ui_tooltip(ui, tip); }
    return hit;
}

static void tbl_toolbar(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    int w[24], i;
    for (i = 0; i < 24; i++) { w[i] = 0; }
    lt_ui_layout_row(ui, 8, w, 0);
    if (tb(ui, "Set=", "Set selected cells to their average (=)", 0)) { tbl_op(app, tab, st, OP_EQUAL, 0); }
    if (tb(ui, "+", "Increase by 1 (+); > also, q = small step", 0)) { tbl_op(app, tab, st, OP_ADD, 1.0); }
    if (tb(ui, "-", "Decrease by 1 (-); < also, w = small step", 0)) { tbl_op(app, tab, st, OP_ADD, -1.0); }
    if (tb(ui, "Scale", "Multiply or add (*)", 0)) { st->scale_open = 1; }
    if (tb(ui, "Interp", "Interpolate between selection corners (/); h/v = horizontal/vertical", 0)) { tbl_op(app, tab, st, OP_INTERP, 0); }
    if (tb(ui, "Smooth", "Smooth selection (s)", 0)) { tbl_op(app, tab, st, OP_SMOOTH, 0); }
    if (tb(ui, "Fill>", "Fill right from the first selected column (Ctrl+R)", 0)) { tbl_op(app, tab, st, OP_FILL_R, 0); }
    if (tb(ui, "FillV", "Fill down from the top selected row (Ctrl+D)", 0)) { tbl_op(app, tab, st, OP_FILL_D, 0); }
    lt_ui_layout_row(ui, 13, w, 0);
    if (tb(ui, "Rebin", "Re-bin: set new axis values and resample", 0)) { rebin_prefill(st); st->rebin_open = 1; }
    if (tb(ui, "Copy", "Copy (Ctrl+C), tab separated", 0)) { tbl_copy(app, st); }
    if (tb(ui, "Paste", "Paste at selection (Ctrl+V)", 0)) { tbl_paste(app, tab, st); }
    if (tb(ui, "Undo", "Undo (Ctrl+Z)", 0)) { tbl_undo_redo(app, tab, st, 0); }
    if (tb(ui, "Redo", "Redo (Ctrl+Y)", 0)) { tbl_undo_redo(app, tab, st, 1); }
    if (tb(ui, "Revert", "Undo all edits made in this tab", 0)) { tbl_revert(app, tab, st); }
    if (tb(ui, "Follow", "Follow the live cell (f)", st->follow)) { st->follow = !st->follow; }
    if (tb(ui, "Live", "Jump to the live cell (g)", 0)) { st->goto_live = 1; }
    if (tb(ui, "Shade", "Colour shading", st->shading)) { st->shading = !st->shading; }
    if (tb(ui, "Y-bot", "Y axis zero at bottom", st->ybottom)) { st->ybottom = !st->ybottom; }
    if (tb(ui, "3D", "Toggle 3D view", st->show3d)) { st->show3d = !st->show3d; }
    if (st->show3d) {
        if (tb(ui, "Reset3D", "Reset the 3D view", 0)) { st->yaw = 0.6; st->pitch = 0.55; }
    } else { lt_ui_label(ui, ""); }
    if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT | (lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED))) { lt_app_burn(app); tab->dirty = 0; }
    lt_ui_tooltip(ui, "Write the changes to ECU flash");
}

static void tbl_dialogs(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    int w1 = -1, w2[2], wb[3];
    static const char *const modes[2] = { "Multiply by", "Add" };
    if (st->scale_open && lt_ui_modal_begin(ui, "Scale / adjust selection", 360, 150, &st->scale_open)) {
        w2[0] = 110; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_combo(ui, "mode", &st->scale_mode, modes, 2);
        lt_ui_number(ui, "val", &st->scale_val, -100000.0, 100000.0, 0.05, 3);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label(ui, "Applies to the selected cells, clamped to the allowed range.");
        wb[0] = -1; wb[1] = 90; wb[2] = 90;
        lt_ui_layout_row(ui, 3, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_DEFAULT)) {
            if (st->scale_mode == 0) { tbl_op(app, tab, st, OP_SCALE, st->scale_val); }
            else { tbl_op(app, tab, st, OP_ADD, st->scale_val); }
            st->scale_open = 0;
        }
        if (lt_ui_button(ui, "Cancel")) { st->scale_open = 0; }
        lt_ui_modal_end(ui);
    }
    if (st->rebin_open && lt_ui_modal_begin(ui, "Re-bin table", 560, 200, &st->rebin_open)) {
        w2[0] = 70; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "X bins");
        lt_ui_text(ui, "xb", st->xtxt, sizeof(st->xtxt), 0);
        lt_ui_label(ui, "Y bins");
        lt_ui_text(ui, "yb", st->ytxt, sizeof(st->ytxt), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_checkbox(ui, "Interpolate Z onto the new bins", &st->rebin_interp);
        wb[0] = -1; wb[1] = 90; wb[2] = 90;
        lt_ui_layout_row(ui, 3, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_DEFAULT)) { rebin_apply(app, tab, st); st->rebin_open = 0; }
        if (lt_ui_button(ui, "Cancel")) { st->rebin_open = 0; }
        lt_ui_modal_end(ui);
    }
}

static void tbl_grid(lt_app *app, lt_app_tab *tab, tbl_tab *st, int vh)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int rows = (int)st->v.rows, cols = (int)st->v.cols;
    int cell = lt_ui_cell_mode(ui);
    int ch = cell ? 16 : 22, chh = cell ? 16 : 24, rhw = cell ? 64 : 72;
    int chars = 3 + st->zdig + (st->zdig > 0), cw;
    double zlo = 1e300, zhi = -1e300, big;
    size_t k;
    char b[48];
    (void)vh;
    big = fabs(st->hi) > fabs(st->lo) ? fabs(st->hi) : fabs(st->lo);
    if (big >= 10000.0) { chars += 2; } else if (big >= 1000.0) { chars += 1; }
    chars += 1;
    cw = chars * 8 + (cell ? 8 : 12);
    if (cw < 48) { cw = 48; }
    for (k = 0; k < st->v.rows * st->v.cols; k++) {
        if (st->v.z[k] < zlo) { zlo = st->v.z[k]; }
        if (st->v.z[k] > zhi) { zhi = st->v.z[k]; }
    }
    if (lt_ui_grid_begin(ui, "grid", rows, cols, cw, ch, rhw, chh)) {
        int r0, r1, c0, c1, r, c, fl;
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, st->ylab[0] ? st->ylab : "Y \\ X");
        for (c = c0; c <= c1; c++) {
            lt_tabs_fmt(b, sizeof(b), st->v.x[c], st->xdig);
            lt_ui_grid_header(ui, 1, c, b);
        }
        for (r = r0; r <= r1; r++) {
            lt_tabs_fmt(b, sizeof(b), st->v.y != NULL ? st->v.y[drow(st, r)] : 0.0, st->ydig);
            lt_ui_grid_header(ui, 0, r, b);
        }
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) {
                size_t dr = (size_t)drow(st, r);
                double v = st->v.z[dr * (size_t)cols + (size_t)c];
                lt_color bg = st->shading ? lt_tabs_heat(app, th, zlo, zhi, v) : th->table_cell;
                if (lt_tblview_cell_dirty(&st->v, dr, (size_t)c)) { bg = lt_color_mix(bg, th->warning, 60); }
                lt_tabs_fmt(b, sizeof(b), v, st->zdig);
                lt_ui_grid_cell(ui, r, c, b, bg, st->shading ? lt_color_contrast(bg) : th->text);
            }
        }
        /* live trail and cursor */
        for (k = (size_t)st->ntrail; k-- > 1;) {
            int dr2 = st->ybottom ? rows - 1 - st->trail_r[k] : st->trail_r[k];
            lt_color tc = th->cursor_trail;
            lt_u32 a = (lt_u32)(200 - (int)k * 200 / TRAIL_N);
            tc = (tc & 0x00FFFFFFUL) | (a << 24);
            lt_ui_grid_mark(ui, dr2, st->trail_c[k], tc, 1);
        }
        if (st->live_ok) {
            lt_ui_grid_mark(ui, st->ybottom ? rows - 1 - st->live_r : st->live_r, st->live_c,
                            th->cursor_live, 2);
        }
        if (st->goto_live || st->follow) {
            if (st->live_ok) {
                int lr = st->ybottom ? rows - 1 - st->live_r : st->live_r;
                lt_ui_grid_select(ui, lr, st->live_c, lr, st->live_c);
                lt_ui_grid_set_cursor(ui, lr, st->live_c);
            }
            st->goto_live = 0;
        }
        /* cache selection in data coordinates */
        memset(st->sel, 0, st->sel_cap);
        st->nsel = 0;
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                if (lt_ui_grid_selected(ui, r, c)) {
                    st->sel[(size_t)drow(st, r) * (size_t)cols + (size_t)c] = 1;
                    st->nsel++;
                }
            }
        }
        lt_ui_grid_cursor(ui, &r, &c);
        st->cur_r = r >= 0 ? drow(st, r) : 0;
        st->cur_c = c >= 0 ? c : 0;
        fl = lt_ui_grid_end(ui);
        if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
            int er, ec;
            lt_ui_grid_editing(ui, &er, &ec);
            lt_tabs_fmt(b, sizeof(b), st->v.z[(size_t)drow(st, er) * (size_t)cols + (size_t)ec], st->zdig);
            lt_ui_grid_edit_set_text(ui, b);
        }
        if (fl & LT_UI_GRID_EDIT_COMMIT) {
            double v;
            if (lt_parse_double(lt_ui_grid_edit_text(ui), &v)) {
                if (st->hi > st->lo) { v = v < st->lo ? st->lo : (v > st->hi ? st->hi : v); }
                tbl_op(app, tab, st, OP_SET, lt_tabs_round(v, st->zdig));
            } else { lt_app_toast(app, 2, "Not a number"); }
        }
    }
    /* colours for the 3D view */
    if (st->show3d) {
        for (k = 0; k < st->v.rows * st->v.cols; k++) {
            st->cellcol[k] = lt_tabs_heat(app, th, zlo, zhi, st->v.z[k]);
        }
        st->col_edge = LT_RGBA(0, 0, 0, 90);
        st->col_live = th->cursor_live;
        st->col_cur = th->cursor_selected;
        st->col_text = th->text2;
    }
}

static void tbl_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    tbl_tab *st = (tbl_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2];
    char b[200], xs[32], ys[32], vs[32];
    LT_UNUSED(r);
    if (st == NULL) {
        st = (tbl_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        st->shading = 1; st->ybottom = 1; st->yaw = 0.6; st->pitch = 0.55;
        st->rebin_interp = 1; st->scale_val = 1.05; st->cur_r = st->cur_c = 0;
        tbl_load(app, tab, st);
    }
    st->frame++;
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, tab->title);
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    if (st->frame % 15 == 0) { tbl_check_external(app, tab, st); }
    if (!st->loaded) { return; }
    tbl_live(app, st);
    tbl_keys(app, tab, st);
    tbl_toolbar(app, tab, st);
    if (st->show3d && !lt_ui_cell_mode(ui)) {
        w2[0] = -3; w2[1] = -2;
        lt_ui_layout_row(ui, 2, w2, -(lt_ui_row_height(ui) + 8));
    } else if (st->show3d) {
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, -(lt_ui_row_height(ui) + 2));
    } else {
        lt_ui_layout_row(ui, 1, &w1, -(lt_ui_row_height(ui) + 8));
    }
    tbl_grid(app, tab, st, 0);
    if (st->show3d) {
        lt_rect r3 = lt_ui_layout_next(ui);
        lt_point m = lt_ui_mouse(ui);
        int down = lt_ui_mouse_down(ui, 0) ? 1 : 0;
        int inside = m.x >= r3.x && m.x < r3.x + r3.w && m.y >= r3.y && m.y < r3.y + r3.h;
        if (down && !st->prev_down && inside) { st->drag3d = 1; }
        if (!down) { st->drag3d = 0; }
        if (st->drag3d) {
            st->yaw += (double)(m.x - st->lmx) * 0.01;
            st->pitch += (double)(m.y - st->lmy) * 0.01;
            if (st->pitch < 0.05) { st->pitch = 0.05; }
            if (st->pitch > 1.5) { st->pitch = 1.5; }
        }
        st->lmx = m.x; st->lmy = m.y; st->prev_down = down;
        lt_ui_draw_box(ui, r3, th->table_bg, th->border, 0);
        if (lt_ui_cell_mode(ui)) {
            lt_ui_draw_text_in(ui, r3, "3D view needs pixel mode", th->muted, 1);
        } else {
            lt_ui_draw_custom(ui, r3, draw3d, st);
        }
    }
    /* status line */
    lt_ui_layout_row(ui, 1, &w1, 0);
    {
        int cr = st->cur_r, cc = st->cur_c, cols = (int)st->v.cols;
        double cv = (cr >= 0 && cc >= 0 && (size_t)cr < st->v.rows && cc < cols)
                        ? st->v.z[(size_t)cr * (size_t)cols + (size_t)cc] : 0.0;
        lt_tabs_fmt(vs, sizeof(vs), cv, st->zdig);
        lt_tabs_fmt(xs, sizeof(xs), st->v.x[cc < cols ? cc : 0], st->xdig);
        lt_tabs_fmt(ys, sizeof(ys), st->v.y != NULL ? st->v.y[cr < (int)st->v.rows ? cr : 0] : 0.0, st->ydig);
        lt_snprintf(b, sizeof(b), "%s | cell [%s, %s] = %s | %d selected | range %g..%g%s", tab->title,
                    xs, ys, vs, st->nsel, st->lo, st->hi, lt_tblview_is_dirty(&st->v) ? " | modified" : "");
        if (st->live_ok) {
            char l[80];
            lt_snprintf(l, sizeof(l), " | live x=%g%s", st->live_x, "");
            if (strlen(b) + strlen(l) < sizeof(b)) { strcat(b, l); }
        }
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
    }
    tbl_dialogs(app, tab, st);
}

static void tbl_close(lt_app *app, lt_app_tab *tab)
{
    tbl_tab *st = (tbl_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    tbl_free_data(st);
    lt_free(st->clip);
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_tabs_kind_table = { "table", tbl_draw, tbl_close };
