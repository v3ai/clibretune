/* tables_table.c - table editor tab: heat-mapped grid, live cursor, edit operations,
 * clipboard, undo/redo, 3D surface view.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "tables_priv.h"
#include "view_kit.h"
#include "tables_3d.h"
#include "feat.h"

#define TRAIL_N 20

typedef struct tbl_tab {
    lt_tblview v;
    int loaded;
    char msg[160];
    const lt_ini_table *t;
    int zdig, xdig, ydig;
    double lo, hi;                /* constant limits (display units) */
    int show3d, follow, shading, ybottom, lambda_on, is_afr;
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
    double *orig; size_t orig_n;  /* values at open: Esc reverts the selection to these */
    double step, step_mult, step_pct;   /* user increments: amount, Ctrl multiplier, PageUp/Dn % */
    int step_open;
    int paste_open, paste_mode;
    int prev_rdown, select_all;
    lt_rect grid_area;            /* viewport of the grid (right-click menu target) */
    int fd_load, fd_save;         /* waiting for a file dialog result */
    int resizable;
    lt_tabs_size_dlg size_dlg;
    lt_tabs_gen_dlg gen_dlg;
    lt_tabs_io_dlg io_dlg;
    int rebin_open, rebin_interp; char xtxt[1024], ytxt[1024];
    lt_t3d t3;
    int drag3d, lmx, lmy, prev_down, prev_rdown3, drag_moved, drag_x0, drag_y0, hover3d;
    int pick_pending, pick_r, pick_c, picked;   /* click-to-select from the 3D view */
    int frame;
    lt_color *cellcol;            /* rows*cols colours for the 3D view */
    lt_color col_edge, col_live, col_cur, col_text, col_trail;
    int dark3d;
    const lt_app *app_;
    lt_rect r3d;
    char xlab[48], ylab[48];
    /* overlay (live cell, trail) positions recorded while the grid is declared */
    double ov_xy[TRAIL_N][2];
    int ov_n, ov_live, ov_have_cur;
    lt_rect ov_live_r, ov_cur_r;
    lt_color ov_trail, ov_live_c;
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
    lt_free(st->orig); st->orig = NULL; st->orig_n = 0;
}

/* decimal places per array, widened when the display unit is finer than the native one */
static void tbl_digits(lt_app *app, tbl_tab *st)
{
    st->zdig = lt_uconv_digits(&st->v.zu, lt_tabs_const_digits(app->ini, st->t->z_const));
    st->xdig = lt_uconv_digits(&st->v.xu, lt_tabs_const_digits(app->ini, st->t->x_const));
    st->ydig = lt_uconv_digits(&st->v.yu, lt_tabs_const_digits(app->ini, st->t->y_const));
}

/* the unit preference changed: convert the open edits, the revert copy and the limits */
static void tbl_units_sync(lt_app *app, tbl_tab *st)
{
    lt_uconv zold = st->v.zu;
    size_t i;
    if (!st->loaded || !lt_tblview_units_sync(&st->v)) { return; }
    for (i = 0; i < st->orig_n; i++) {
        lt_uconv w = zold;
        st->orig[i] = lt_uconv_retarget(&w, st->orig[i]);
    }
    tbl_digits(app, st);
    lt_tblview_z_range(&st->v, app->ini, &st->lo, &st->hi);
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
    tbl_digits(app, st);
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
    lt_free(st->orig);
    st->orig = (double *)lt_calloc(n, sizeof(double));
    memcpy(st->orig, st->v.z, n * sizeof(double));
    st->orig_n = n;
    st->is_afr = st->t->role == LT_ROLE_AFR_TARGET;
    st->resizable = st->t->rows_size_const != NULL && st->t->rows_size_const[0] != '\0';
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
    if (!st->loaded || lt_tblview_is_dirty(&st->v)) { return; }
    if (st->resizable) {
        /* a resize (here, a shared table, a tune load) changes the active size */
        lt_table_size sz;
        lt_pagesrc ps = lt_app_pages(app);
        if (lt_table_size_info(app->ini, st->t, &ps, &sz) == 0 &&
            (sz.active_cols != st->v.cols || sz.active_rows != st->v.rows)) {
            tbl_load(app, tab, st);
        }
        return;
    }
    zc = lt_ini_find_const(app->ini, st->t->z_const);
    if (zc == NULL) { return; }
    n = st->v.rows * st->v.cols;
    tmp = (double *)lt_calloc(n + 1, sizeof(double));
    src = lt_app_pages(app);
    cnt = lt_ini_const_get_array(app->ini, zc, &src, tmp, n);
    if (cnt != (int)n) { diff = 1; }
    for (i = 0; i < n && !diff; i++) {
        if (fabs(lt_uconv_to_disp(&st->v.zu, tmp[i]) - st->v.z[i]) > 1e-6) { diff = 1; }
    }
    lt_free(tmp);
    if (diff) { tbl_load(app, tab, st); }
}

static void tbl_commit(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    int n;
    (void)lt_tabs_check_change(app, &st->v);
    lt_feat_actions_note_table(app, st->t->name, &st->v);
    n = lt_tblview_commit(&st->v, app->ini, &dst);
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
       OP_FILL_R, OP_FILL_D, OP_SET, OP_RESTORE, OP_CLEAR };

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
    case OP_RESTORE:
    case OP_CLEAR: {
        /* run the pure op on a scratch copy, then write the selected cells through the view */
        double *tmp = (double *)lt_calloc(st->v.rows * cols + 1, sizeof(double));
        memcpy(tmp, st->v.z, st->v.rows * cols * sizeof(double));
        if (op == OP_RESTORE && st->orig != NULL && st->orig_n == st->v.rows * cols) {
            lt_tbl_restore(tmp, st->v.rows, cols, sel, n, st->orig);
        } else if (op == OP_CLEAR) {
            lt_tbl_clear(tmp, st->v.rows, cols, sel, n, st->lo, st->hi);
        }
        {
            int changed = 0;
            for (i = 0; i < n; i++) {
                if (tmp[sel[i].y * cols + sel[i].x] != lt_tblview_get(&st->v, sel[i].y, sel[i].x)) { changed = 1; }
            }
            if (changed) {
                lt_tblview_push_undo(&st->v);
                for (i = 0; i < n; i++) {
                    lt_tblview_set(&st->v, sel[i].y, sel[i].x, tmp[sel[i].y * cols + sel[i].x]);
                }
            }
        }
        lt_free(tmp);
        break; }
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

/* combine value `val` with data cell (y,x) using a paste mode and write it through the view */
static void tbl_put(tbl_tab *st, size_t y, size_t x, double val, int mode)
{
    double cell = lt_tblview_get(&st->v, y, x);
    lt_tbl_paste_op(&cell, 1, 1, 0, 0, &val, 1, 1, mode);
    lt_tblview_set(&st->v, y, x, cell);
}

static void tbl_paste(lt_app *app, lt_app_tab *tab, tbl_tab *st, int mode)
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
                    tbl_put(st, (size_t)r, (size_t)c, blk[0], mode);
                }
            }
        }
    } else {
        size_t i, j;
        for (i = 0; i < bh; i++) {
            for (j = 0; j < bw; j++) {
                int dr = r0 + (int)i, dc = c0 + (int)j;
                if (dr < rows && dc < cols) {
                    tbl_put(st, (size_t)drow(st, dr), (size_t)dc, blk[i * bw + j], mode);
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

/* ---- 3D ----
 * Perspective orbit camera around the table centre; the surface is drawn far
 * to near (painter's order for a height field), shaded by a directional
 * light, over a floor grid and a faint bounding box with axis captions. */

static void w3_line(lt_gfx *g, const lt_cam3 *c, double x0, double y0, double z0, double x1, double y1,
                    double z1, double w, lt_color col)
{
    double ax, ay, bx, by;
    lt_cam3_proj(c, x0, y0, z0, &ax, &ay);
    lt_cam3_proj(c, x1, y1, z1, &bx, &by);
    lt_gfx_line(g, ax, ay, bx, by, w, col);
}

static void w3_label(lt_gfx *g, const lt_cam3 *c, double x, double y, double z, const char *s,
                     lt_color col, int align)
{
    double sx, sy;
    lt_font *f = lt_font_get(LT_FONT_SANS_MEDIUM);
    lt_cam3_proj(c, x, y, z, &sx, &sy);
    if (f == NULL) { lt_gfx_text(g, (int)sx, (int)sy - 8, s, col, 1); return; }
    {
        double fp = lt_gfx_fpx(g, 11.0);
        double w = lt_gfx_text_measure(f, fp, s);
        double xx = align == 1 ? sx - w * 0.5 : (align == 2 ? sx - w : sx);
        lt_gfx_text_bl(g, f, fp, xx, (int)(sy + fp * 4.0 / 11.0), s, col);
    }
}

static double st_wx(const tbl_tab *st, int c)
{
    LT_UNUSED(st);
    return LT_T3D_W * (double)c;
}

static void draw3d(lt_gfx *g, lt_rect r, void *user)
{
    tbl_tab *st = (tbl_tab *)user;
    int rows = (int)st->v.rows, cols = (int)st->v.cols, i, j, k;
    double zlo = 1e300, zhi = -1e300, span, rr, cc;
    lt_cam3 cam;
    size_t n;
    int ri_first, ri_step, ci_first, ci_step, qi, qj;
    double lx = 0.50, ly = 0.78, lz = 0.37;   /* light direction (normalised below) */
    double ll = sqrt(lx * lx + ly * ly + lz * lz);
    lt_color bgtop = LT_RGB(0x18, 0x1B, 0x22), bgbot = LT_RGB(0x0C, 0x0E, 0x12);
    char b[64];
    if (rows < 2 || cols < 2) { return; }
    st->r3d = r;
    lt_gfx_fill_rect_vgrad(g, r, st->dark3d ? bgtop : LT_RGB(0xF4, 0xF5, 0xF8),
                           st->dark3d ? bgbot : LT_RGB(0xD9, 0xDC, 0xE3));
    lx /= ll; ly /= ll; lz /= ll;
    for (n = 0; n < st->v.rows * st->v.cols; n++) {
        if (st->v.z[n] < zlo) { zlo = st->v.z[n]; }
        if (st->v.z[n] > zhi) { zhi = st->v.z[n]; }
    }
    span = zhi > zlo ? zhi - zlo : 1.0;
    lt_cam3_setup(&st->t3, r.x, r.y, r.w, r.h, &cam);

    /* floor grid + bounding box */
    {
        lt_color gl = st->dark3d ? LT_RGBA(255, 255, 255, 26) : LT_RGBA(0, 0, 0, 30);
        lt_color bx = st->dark3d ? LT_RGBA(255, 255, 255, 70) : LT_RGBA(0, 0, 0, 80);
        for (i = 0; i <= 10; i++) {
            double t = (double)i;
            w3_line(g, &cam, t, 0.0, 0.0, t, 0.0, LT_T3D_W, 1.0, gl);
            w3_line(g, &cam, 0.0, 0.0, t, LT_T3D_W, 0.0, t, 1.0, gl);
        }
        w3_line(g, &cam, 0.0, 0.0, 0.0, LT_T3D_W, 0.0, 0.0, 1.0, bx);
        w3_line(g, &cam, LT_T3D_W, 0.0, 0.0, LT_T3D_W, 0.0, LT_T3D_W, 1.0, bx);
        w3_line(g, &cam, LT_T3D_W, 0.0, LT_T3D_W, 0.0, 0.0, LT_T3D_W, 1.0, bx);
        w3_line(g, &cam, 0.0, 0.0, LT_T3D_W, 0.0, 0.0, 0.0, 1.0, bx);
        /* back verticals: the two corners farthest from the eye */
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 2; j++) {
                double x = i ? LT_T3D_W : 0.0, z = j ? LT_T3D_W : 0.0;
                double dx = x - cam.ex, dz = z - cam.ez;
                if (dx * dx + dz * dz > (LT_T3D_W * 0.5 + 8.0) * (LT_T3D_W * 0.5 + 8.0) * 0.98) {
                    w3_line(g, &cam, x, 0.0, z, x, LT_T3D_H, z, 1.0, bx);
                }
            }
        }
    }

    /* far-to-near order of rows/columns */
    ci_first = cam.ex > LT_T3D_W * 0.5 ? 0 : cols - 2; ci_step = ci_first == 0 ? 1 : -1;
    {
        /* world z of row index: ybottom puts the lowest Y bin nearest the eye */
        int zflip = st->ybottom ? 1 : 0;
        int low_row_far = (cam.ez > LT_T3D_W * 0.5) ? (zflip ? 0 : 1) : (zflip ? 1 : 0);
        /* low_row_far == 1: row 0 is the farthest row from the eye */
        ri_first = low_row_far ? 0 : rows - 2; ri_step = low_row_far ? 1 : -1;
    }
    for (qi = 0, i = ri_first; qi < rows - 1; qi++, i += ri_step) {
        for (qj = 0, j = ci_first; qj < cols - 1; qj++, j += ci_step) {
            double px[4], py[4], poly[8], wx[4], wy[4], wz[4], nx, ny, nz, nl, lam, avg = 0.0;
            int cr[4], ccn[4];
            lt_color base, col;
            cr[0] = i; cr[1] = i; cr[2] = i + 1; cr[3] = i + 1;
            ccn[0] = j; ccn[1] = j + 1; ccn[2] = j + 1; ccn[3] = j;
            for (k = 0; k < 4; k++) {
                double v = st->v.z[(size_t)cr[k] * (size_t)cols + (size_t)ccn[k]];
                rr = (double)cr[k] / (double)(rows - 1);
                cc = (double)ccn[k] / (double)(cols - 1);
                wx[k] = st_wx(st, 0) + cc * LT_T3D_W;
                wz[k] = st->ybottom ? (1.0 - rr) * LT_T3D_W : rr * LT_T3D_W;
                wy[k] = (v - zlo) / span * LT_T3D_H;
                avg += v;
                lt_cam3_proj(&cam, wx[k], wy[k], wz[k], &px[k], &py[k]);
                poly[k * 2] = px[k]; poly[k * 2 + 1] = py[k];
            }
            /* normal from the two diagonals */
            {
                double ax = wx[2] - wx[0], ay = wy[2] - wy[0], az = wz[2] - wz[0];
                double bx2 = wx[3] - wx[1], by2 = wy[3] - wy[1], bz2 = wz[3] - wz[1];
                nx = ay * bz2 - az * by2; ny = az * bx2 - ax * bz2; nz = ax * by2 - ay * bx2;
                nl = sqrt(nx * nx + ny * ny + nz * nz);
                if (nl < 1e-12) { nl = 1.0; ny = 1.0; }
                if (ny < 0.0) { nx = -nx; ny = -ny; nz = -nz; }
                lam = (nx * lx + ny * ly + nz * lz) / nl;
                if (lam < 0.0) { lam = 0.0; }
            }
            base = st->cellcol[(size_t)i * (size_t)cols + (size_t)j];
            col = lt_color_mix(LT_RGB(0, 0, 0), base, (int)((0.50 + 0.50 * lam) * 256.0));
            if (!st->dark3d) { col = lt_color_mix(base, LT_RGB(255, 255, 255), (int)((1.0 - lam) * 40.0)); }
            if (st->t3.style == LT_T3D_SOLID) {
                lt_gfx_fill_poly(g, poly, 4, col);
            } else {
                /* wireframe / points: hide what lies behind with the background tone */
                lt_gfx_fill_poly(g, poly, 4, st->dark3d ? LT_RGB(0x12, 0x14, 0x19) : LT_RGB(0xE8, 0xEA, 0xEF));
            }
            if (st->t3.style == LT_T3D_WIRE) {
                for (k = 0; k < 4; k++) {
                    int n2 = (k + 1) & 3;
                    lt_gfx_line(g, px[k], py[k], px[n2], py[n2], 1.2, st->cellcol[(size_t)cr[k] * (size_t)cols + (size_t)ccn[k]]);
                }
            } else if (st->t3.style == LT_T3D_POINTS) {
                for (k = 0; k < 4; k++) {
                    lt_gfx_fill_circle(g, px[k], py[k], 2.5, st->cellcol[(size_t)cr[k] * (size_t)cols + (size_t)ccn[k]]);
                }
            } else if (st->t3.grid && rows * cols <= 1300) {
                lt_color el = st->dark3d ? LT_RGBA(255, 255, 255, 60) : LT_RGBA(0, 0, 0, 55);
                for (k = 0; k < 4; k++) {
                    int n2 = (k + 1) & 3;
                    lt_gfx_line(g, px[k], py[k], px[n2], py[n2], 1.0, el);
                }
            }
            (void)avg;
        }
    }

    /* axis captions */
    {
        lt_color cx_ = LT_RGB(0xFF, 0x88, 0x88), cy_ = LT_RGB(0x88, 0xFF, 0x88), cz_ = LT_RGB(0x88, 0x99, 0xFF);
        double near_z = (cam.ez > LT_T3D_W * 0.5) ? LT_T3D_W + 0.9 : -0.9;
        double near_x = (cam.ex > LT_T3D_W * 0.5) ? LT_T3D_W + 0.9 : -0.9;
        lt_color dim = st->dark3d ? LT_RGB(0x9A, 0xA0, 0xAC) : LT_RGB(0x55, 0x5B, 0x66);
        w3_label(g, &cam, LT_T3D_W * 0.5, -0.2, near_z + (near_z > 0 ? 0.8 : -0.8),
                 st->xlab[0] ? st->xlab : "X", cx_, 1);
        w3_label(g, &cam, near_x + (near_x > 0 ? 0.8 : -0.8), -0.2, LT_T3D_W * 0.5,
                 st->ylab[0] ? st->ylab : "Y", cy_, 1);
        w3_label(g, &cam, (cam.ex > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, LT_T3D_H * 0.5,
                 (cam.ez > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, "Z", cz_, 1);
        lt_tabs_fmt(b, sizeof(b), st->v.x[0], st->xdig);
        w3_label(g, &cam, 0.0, -0.1, near_z, b, dim, 1);
        lt_tabs_fmt(b, sizeof(b), st->v.x[cols - 1], st->xdig);
        w3_label(g, &cam, LT_T3D_W, -0.1, near_z, b, dim, 1);
        if (st->v.y != NULL) {
            lt_tabs_fmt(b, sizeof(b), st->v.y[0], st->ydig);
            w3_label(g, &cam, near_x, -0.1, st->ybottom ? LT_T3D_W : 0.0, b, dim, 1);
            lt_tabs_fmt(b, sizeof(b), st->v.y[rows - 1], st->ydig);
            w3_label(g, &cam, near_x, -0.1, st->ybottom ? 0.0 : LT_T3D_W, b, dim, 1);
        }
        lt_tabs_fmt(b, sizeof(b), zlo, st->zdig);
        w3_label(g, &cam, (cam.ex > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, 0.0,
                 (cam.ez > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, b, dim, 2);
        lt_tabs_fmt(b, sizeof(b), zhi, st->zdig);
        w3_label(g, &cam, (cam.ex > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, LT_T3D_H,
                 (cam.ez > LT_T3D_W * 0.5 ? 0.0 : LT_T3D_W) - 0.1, b, dim, 2);
    }

    /* markers: cursor cell, trail, live position */
    for (k = 0; k < 2; k++) {
        int mr = k ? st->live_r : st->cur_r, mc = k ? st->live_c : st->cur_c;
        double v, sx, sy, fx0, fz0, h;
        if ((k == 1 && !st->live_ok) || mr < 0 || mc < 0 || mr >= rows || mc >= cols) { continue; }
        v = st->v.z[(size_t)mr * (size_t)cols + (size_t)mc];
        rr = (double)mr / (double)(rows - 1); cc = (double)mc / (double)(cols - 1);
        fx0 = cc * LT_T3D_W; fz0 = st->ybottom ? (1.0 - rr) * LT_T3D_W : rr * LT_T3D_W;
        h = (v - zlo) / span * LT_T3D_H;
        lt_cam3_proj(&cam, fx0, h + 0.12, fz0, &sx, &sy);
        if (k) {
            lt_color gc = st->col_live;
            w3_line(g, &cam, fx0, 0.0, fz0, fx0, h + 0.12, fz0, 1.5, (gc & 0x00FFFFFFUL) | 0x90000000UL);
            lt_gfx_fill_circle(g, sx, sy, 9.0, (gc & 0x00FFFFFFUL) | 0x30000000UL);
            lt_gfx_fill_circle(g, sx, sy, 5.0, gc);
            lt_gfx_circle(g, sx, sy, 5.5, 1.2, LT_RGB(255, 255, 255));
            lt_tabs_fmt(b, sizeof(b), v, st->zdig);
            {
                lt_font *f = lt_font_get(LT_FONT_MONO_MEDIUM);
                if (f != NULL) {
                    double fp = lt_gfx_fpx(g, 11.0), tw = lt_gfx_text_measure(f, fp, b);
                    lt_rect chip = lt_rect_make((int)(sx - tw * 0.5) - lt_gfx_px(g, 5), (int)sy - lt_gfx_px(g, 28),
                                                (int)tw + lt_gfx_px(g, 10), lt_gfx_px(g, 18));
                    lt_gfx_fill_round_rect(g, chip, lt_gfx_px(g, 4), LT_RGBA(0, 0, 0, 200));
                    lt_gfx_text_bl(g, f, fp, sx - tw * 0.5, (int)sy - lt_gfx_px(g, 15), b, gc);
                }
            }
        } else {
            lt_gfx_fill_circle(g, sx, sy, 4.5, LT_RGBA(0, 0, 0, 120));
            lt_gfx_fill_circle(g, sx, sy, 3.5, st->col_cur);
            lt_gfx_circle(g, sx, sy, 4.5, 1.0, LT_RGB(255, 255, 255));
        }
    }
    {
        double pxo = 0.0, pyo = 0.0;
        int have = 0;
        for (k = 0; k < st->ntrail; k++) {
            int mr = st->trail_r[k], mc = st->trail_c[k];
            double v, sx, sy;
            if (mr < 0 || mc < 0 || mr >= rows || mc >= cols) { continue; }
            v = st->v.z[(size_t)mr * (size_t)cols + (size_t)mc];
            rr = (double)mr / (double)(rows - 1); cc = (double)mc / (double)(cols - 1);
            lt_cam3_proj(&cam, cc * LT_T3D_W, (v - zlo) / span * LT_T3D_H + 0.1,
                      st->ybottom ? (1.0 - rr) * LT_T3D_W : rr * LT_T3D_W, &sx, &sy);
            if (have) {
                int a = 220 - k * 170 / TRAIL_N;
                lt_gfx_line(g, pxo, pyo, sx, sy, 2.0, (st->col_trail & 0x00FFFFFFUL) | ((lt_u32)(a > 40 ? a : 40) << 24));
            }
            pxo = sx; pyo = sy; have = 1;
        }
    }

    /* legend: vertical heat bar with the value range */
    {
        lt_rect lg = lt_rect_make(r.x + r.w - 26, r.y + 40, 10, r.h > 220 ? 120 : r.h / 3);
        int steps = lg.h, q;
        for (q = 0; q < steps; q++) {
            double t = 1.0 - (double)q / (double)(steps > 1 ? steps - 1 : 1);
            lt_gfx_fill_rect(g, lt_rect_make(lg.x, lg.y + q, lg.w, 1), lt_tabs_heat(st->app_, NULL, 0.0, 1.0, t));
        }
        lt_gfx_rect(g, lg, st->dark3d ? LT_RGBA(255, 255, 255, 80) : LT_RGBA(0, 0, 0, 80));
        {
            lt_font *f = lt_font_get(LT_FONT_MONO);
            lt_color dim = st->dark3d ? LT_RGB(0xB8, 0xBE, 0xC9) : LT_RGB(0x44, 0x4A, 0x55);
            if (f != NULL) {
                lt_tabs_fmt(b, sizeof(b), zhi, st->zdig);
                lt_gfx_text_bl(g, f, 10.0, (double)lg.x - lt_gfx_text_measure(f, 10.0, b) - 4.0, lg.y + 8, b, dim);
                lt_tabs_fmt(b, sizeof(b), zlo, st->zdig);
                lt_gfx_text_bl(g, f, 10.0, (double)lg.x - lt_gfx_text_measure(f, 10.0, b) - 4.0, lg.y + lg.h, b, dim);
            }
        }
    }
    {
        lt_font *f = lt_font_get(LT_FONT_SANS);
        if (f != NULL) {
            lt_gfx_text_bl(g, f, 11.0, (double)r.x + 10.0, r.y + r.h - 9,
                           "Drag: rotate   Right-drag: pan   Wheel: zoom   Click: select cell   Home: reset",
                           st->dark3d ? LT_RGB(0x78, 0x7E, 0x8A) : LT_RGB(0x70, 0x76, 0x80));
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

/* percent step: scale the selection by 1 +/- pct/100 */
static void tbl_pct(lt_app *app, lt_app_tab *tab, tbl_tab *st, int dir)
{
    double f = 1.0 + (double)dir * st->step_pct / 100.0;
    tbl_op(app, tab, st, OP_SCALE, f);
}

/* ---- 3D input ---- */
static int in_rect(lt_point m, lt_rect r)
{
    return m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
}

/* keys over the 3D view (run before the 2D keys so they take the arrows): arrows rotate, Home
 * resets, PageUp/PageDown and [ ] zoom, + / - zoom while no cell was picked in 3D (afterwards they
 * adjust the picked cell like in the grid) */
static void tbl_3d_keys(lt_ui *ui, tbl_tab *st)
{
    double a = 0.08;
    if (!st->show3d || lt_ui_text_focused(ui) || lt_ui_popup_open(ui) || !in_rect(lt_ui_mouse(ui), st->r3d)) { return; }
    if (lt_ui_key_pressed(ui, LT_KEY_LEFT, 0)) { lt_t3d_orbit(&st->t3, a, 0.0); }
    if (lt_ui_key_pressed(ui, LT_KEY_RIGHT, 0)) { lt_t3d_orbit(&st->t3, -a, 0.0); }
    if (lt_ui_key_pressed(ui, LT_KEY_UP, 0)) { lt_t3d_orbit(&st->t3, 0.0, a); }
    if (lt_ui_key_pressed(ui, LT_KEY_DOWN, 0)) { lt_t3d_orbit(&st->t3, 0.0, -a); }
    if (lt_ui_key_pressed(ui, LT_KEY_HOME, 0)) { lt_t3d_reset_view(&st->t3); st->picked = 0; }
    if (lt_ui_key_pressed(ui, LT_KEY_PAGEUP, 0) || lt_ui_key_pressed(ui, ']', 0)) { lt_t3d_zoom(&st->t3, 0.9); }
    if (lt_ui_key_pressed(ui, LT_KEY_PAGEDOWN, 0) || lt_ui_key_pressed(ui, '[', 0)) { lt_t3d_zoom(&st->t3, 1.0 / 0.9); }
    if (!st->picked) {
        if (lt_tabs_key(ui, '+') || lt_ui_key_pressed(ui, '=', LT_MOD_SHIFT)) { lt_t3d_zoom(&st->t3, 0.9); }
        if (lt_tabs_key(ui, '-') || lt_tabs_key(ui, '_')) { lt_t3d_zoom(&st->t3, 1.0 / 0.9); }
    }
}

/* mouse over the 3D rect: left drag orbits, right drag pans, a left click (no drag) picks the
 * nearest vertex and selects that cell, the wheel zooms */
static void tbl_3d_mouse(lt_ui *ui, tbl_tab *st, lt_rect r3)
{
    lt_point m = lt_ui_mouse(ui);
    int down = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0, rdown = lt_ui_mouse_down(ui, LT_MB_RIGHT) ? 1 : 0;
    int inside = in_rect(m, r3), dx = m.x - st->lmx, dy = m.y - st->lmy;
    st->r3d = r3;
    if (down && !st->prev_down && inside) { st->drag3d = 1; st->drag_moved = 0; st->drag_x0 = m.x; st->drag_y0 = m.y; }
    if (rdown && !st->prev_rdown3 && inside) { st->drag3d = 2; }
    if (!down && st->drag3d == 1) {
        if (!st->drag_moved && inside && !lt_ui_cell_mode(ui)) {
            int pr = 0, pc = 0;
            lt_cam3 cam;
            double zlo = 1e300, zhi = -1e300, span;
            size_t k, n = st->v.rows * st->v.cols;
            for (k = 0; k < n; k++) {
                if (st->v.z[k] < zlo) { zlo = st->v.z[k]; }
                if (st->v.z[k] > zhi) { zhi = st->v.z[k]; }
            }
            span = zhi > zlo ? zhi - zlo : 1.0;
            lt_cam3_setup(&st->t3, r3.x, r3.y, r3.w, r3.h, &cam);
            if (lt_t3d_pick(&cam, (int)st->v.rows, (int)st->v.cols, st->ybottom, st->v.z, zlo, span, m.x, m.y,
                            (double)lt_ui_px(ui, 22), &pr, &pc)) {
                st->pick_pending = 1; st->pick_r = pr; st->pick_c = pc; st->picked = 1;
            }
        }
        st->drag3d = 0;
    }
    if (!rdown && st->drag3d == 2) { st->drag3d = 0; }
    if (st->drag3d == 1) {
        if (abs(m.x - st->drag_x0) + abs(m.y - st->drag_y0) > 3) { st->drag_moved = 1; }
        if (st->drag_moved) { lt_t3d_orbit(&st->t3, -(double)dx * 0.01, (double)dy * 0.01); }
    } else if (st->drag3d == 2) {
        lt_t3d_pan(&st->t3, (double)dx, (double)dy, r3.w < r3.h ? r3.w : r3.h);
    }
    if (inside && lt_ui_wheel(ui) != 0) { lt_t3d_wheel(&st->t3, lt_ui_wheel(ui)); }
    st->lmx = m.x; st->lmy = m.y; st->prev_down = down; st->prev_rdown3 = rdown;
}

static void tbl_keys(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    double ss = small_step(st);
    double up = st->step, dn = -st->step, cm = st->step * st->step_mult;
    if (lt_ui_text_focused(ui) || lt_ui_popup_open(ui)) { return; }
    if (lt_ui_key_pressed(ui, 'c', LT_MOD_CTRL)) { tbl_copy(app, st); }
    if (lt_ui_key_pressed(ui, 'v', LT_MOD_CTRL | LT_MOD_SHIFT)) { st->paste_open = 1; }
    if (lt_ui_key_pressed(ui, 'v', LT_MOD_CTRL)) { tbl_paste(app, tab, st, LT_TBL_PASTE_REPLACE); }
    if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL)) { tbl_undo_redo(app, tab, st, 0); }
    if (lt_ui_key_pressed(ui, 'y', LT_MOD_CTRL) || lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL | LT_MOD_SHIFT)) {
        tbl_undo_redo(app, tab, st, 1);
    }
    if (lt_ui_key_pressed(ui, 'r', LT_MOD_CTRL)) { tbl_op(app, tab, st, OP_FILL_R, 0); }
    if (lt_ui_key_pressed(ui, 'd', LT_MOD_CTRL)) { tbl_op(app, tab, st, OP_FILL_D, 0); }
    if (lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0)) { tbl_op(app, tab, st, OP_RESTORE, 0); }
    if (lt_ui_key_pressed(ui, LT_KEY_DELETE, 0) || lt_ui_key_pressed(ui, LT_KEY_BACKSPACE, 0)) {
        tbl_op(app, tab, st, OP_CLEAR, 0);
    }
    if (lt_ui_key_pressed(ui, '=', 0)) { tbl_op(app, tab, st, OP_EQUAL, 0); }
    /* step keys: user amount; Ctrl uses the multiplier */
    if (lt_ui_key_pressed(ui, '=', LT_MOD_SHIFT) || lt_tabs_key(ui, '+') || lt_tabs_key(ui, '>')) { tbl_op(app, tab, st, OP_ADD, up); }
    if (lt_tabs_key(ui, '-') || lt_tabs_key(ui, '_') || lt_tabs_key(ui, '<') ||
        lt_ui_key_pressed(ui, ',', 0)) { tbl_op(app, tab, st, OP_ADD, dn); }
    /* '.' also starts a cell edit in the grid, so only Ctrl+'.' is a step key */
    if (lt_ui_key_pressed(ui, '+', LT_MOD_CTRL) || lt_ui_key_pressed(ui, '.', LT_MOD_CTRL) ||
        lt_ui_key_pressed(ui, '=', LT_MOD_CTRL)) { tbl_op(app, tab, st, OP_ADD, cm); }
    if (lt_ui_key_pressed(ui, '-', LT_MOD_CTRL) || lt_ui_key_pressed(ui, ',', LT_MOD_CTRL)) {
        tbl_op(app, tab, st, OP_ADD, -cm);
    }
    if (lt_ui_key_pressed(ui, LT_KEY_PAGEUP, LT_MOD_CTRL) || lt_ui_key_pressed(ui, LT_KEY_PAGEUP, LT_MOD_SHIFT)) {
        tbl_pct(app, tab, st, 1);
    }
    if (lt_ui_key_pressed(ui, LT_KEY_PAGEDOWN, LT_MOD_CTRL) || lt_ui_key_pressed(ui, LT_KEY_PAGEDOWN, LT_MOD_SHIFT)) {
        tbl_pct(app, tab, st, -1);
    }
    if (lt_tabs_key(ui, 'q')) { tbl_op(app, tab, st, OP_ADD, ss); }
    if (lt_tabs_key(ui, 'w')) { tbl_op(app, tab, st, OP_ADD, -ss); }
    if (lt_tabs_key(ui, '*')) { st->scale_open = 1; st->scale_mode = 0; }
    if (lt_tabs_key(ui, '/')) { tbl_op(app, tab, st, OP_INTERP, 0); }
    if (lt_tabs_key(ui, 'h')) { tbl_op(app, tab, st, OP_INTERP_H, 0); }
    if (lt_tabs_key(ui, 'v')) { tbl_op(app, tab, st, OP_INTERP_V, 0); }
    if (lt_tabs_key(ui, 's')) { tbl_op(app, tab, st, OP_SMOOTH, 0); }
    if (lt_tabs_key(ui, 'f')) { st->follow = !st->follow; }
    if (lt_tabs_key(ui, 'g')) { st->goto_live = 1; }
    if (lt_ui_key_pressed(ui, '3', 0)) { st->show3d = !st->show3d; }
}


/* ---- UI pieces ---- */
enum { TB_EQ, TB_UP, TB_DOWN, TB_SCALE, TB_INTERP, TB_SMOOTH, TB_REBIN, TB_COPY, TB_PASTE,
       TB_UNDO, TB_REDO, TB_REVERT, TB_FILLR, TB_FILLD, TB_LIVE, TB_FOLLOW, TB_SHADE, TB_YBOT,
       TB_3D, TB_RESET3D, TB_STYLE3D, TB_COLOR3D, TB_GRID3D, TB_LAMBDA, TB_BURN, TB_SIZE, TB_GEN, TB_LOAD, TB_SAVE, TB_PSPECIAL, TB_STEPS, TB_N };

static int gen_ok(const tbl_tab *st)
{
    return st->t != NULL && (st->t->role == LT_ROLE_VE || st->t->role == LT_ROLE_IGNITION ||
                             st->t->role == LT_ROLE_AFR_TARGET);
}

static void tbl_open_size(lt_app *app, tbl_tab *st)
{
    if (lt_tabs_size_begin(app, st->t, &st->size_dlg) < 0) {
        lt_app_toast(app, 2, "This table has a fixed size");
    }
}

static void tbl_open_gen(tbl_tab *st)
{
    st->gen_dlg.role = (int)st->t->role;
    st->gen_dlg.open = 1;
}

static void tbl_file_load(lt_app *app, tbl_tab *st)
{
    st->fd_load = 1;
    lt_app_file_dialog(app, LT_TABS_FD_LOAD, LT_UI_FILE_OPEN, NULL, "", "TunerStudio table|*.table|All files|*");
}

static void tbl_file_save(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    char name[96];
    size_t i, k = 0;
    for (i = 0; tab->title[i] != '\0' && k + 7 < sizeof(name); i++) {
        char c = tab->title[i];
        name[k++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    }
    name[k] = '\0';
    lt_strlcpy(name + k, ".table", sizeof(name) - k);
    st->fd_save = 1;
    lt_app_file_dialog(app, LT_TABS_FD_SAVE, LT_UI_FILE_SAVE, NULL, name, "TunerStudio table|*.table|All files|*");
}

static void tb_item(lt_vk_item *it, int kind, int icon, const char *label, const char *key,
                    const char *tip)
{
    memset(it, 0, sizeof(*it));
    it->kind = kind; it->icon = icon; it->label = label; it->key = key; it->tip = tip;
}

static void tbl_toolbar(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    lt_vk_item it[40];
    int n = 0, hit, ids[40], cell = lt_ui_cell_mode(ui);
    /* ids[i] maps the item index to a TB_ command (-1 for separators) */
#define TB_ADD(cmd, icon, label, key, tip) \
    do { tb_item(&it[n], LT_VK_BTN, icon, label, key, tip); ids[n] = cmd; n++; } while (0)
#define TB_SEP() do { tb_item(&it[n], LT_VK_SEP, 0, NULL, NULL, NULL); ids[n] = -1; n++; } while (0)
#define TB_GAP() do { tb_item(&it[n], LT_VK_SPACER, 0, NULL, NULL, NULL); ids[n] = -1; n++; } while (0)
    TB_ADD(TB_EQ, LT_ICON_EQUAL, cell ? "Set=" : NULL, "=", "Set selected cells to their average (=)");
    TB_ADD(TB_UP, LT_ICON_PLUS, cell ? "+" : NULL, ">", "Increase by 1 (+ or >); q = small step");
    TB_ADD(TB_DOWN, LT_ICON_MINUS, cell ? "-" : NULL, "<", "Decrease by 1 (- or <); w = small step");
    TB_SEP();
    TB_ADD(TB_SCALE, LT_ICON_X, cell ? "Scale" : NULL, "*", "Multiply or add an amount (*)");
    TB_ADD(TB_INTERP, LT_ICON_TRENDING_UP, cell ? "Interp" : NULL, "/",
           "Interpolate between selection corners (/); h/v = horizontal/vertical");
    TB_ADD(TB_SMOOTH, LT_ICON_SPARKLES, cell ? "Smooth" : NULL, "s", "Smooth selected cells (s)");
    TB_ADD(TB_FILLR, LT_ICON_CHEVRONS_RIGHT, cell ? "Fill>" : NULL, NULL, "Fill right from the first selected column (Ctrl+R)");
    TB_ADD(TB_FILLD, LT_ICON_CHEVRONS_DOWN, cell ? "FillV" : NULL, NULL, "Fill down from the top selected row (Ctrl+D)");
    TB_ADD(TB_REBIN, LT_ICON_GRID_3X3, cell ? "Rebin" : NULL, NULL, "Re-bin: set new axis values and resample");
    if (st->resizable) { TB_ADD(TB_SIZE, LT_ICON_RULER, cell ? "Size" : NULL, NULL, "Set table size (columns x rows)"); }
    if (gen_ok(st)) { TB_ADD(TB_GEN, LT_ICON_WAND_SPARKLES, cell ? "Gen" : NULL, NULL, "Generate this table from engine specs"); }
    TB_SEP();
    TB_ADD(TB_COPY, LT_ICON_COPY, cell ? "Copy" : NULL, NULL, "Copy selected cells (Ctrl+C), tab separated");
    TB_ADD(TB_PASTE, LT_ICON_CLIPBOARD_PASTE, cell ? "Paste" : NULL, NULL, "Paste at the selection (Ctrl+V)");
    TB_ADD(TB_UNDO, LT_ICON_UNDO_2, cell ? "Undo" : NULL, NULL, "Undo (Ctrl+Z)");
    TB_ADD(TB_REDO, LT_ICON_REDO_2, cell ? "Redo" : NULL, NULL, "Redo (Ctrl+Y)");
    TB_ADD(TB_LOAD, LT_ICON_FILE_UP, cell ? "Load" : NULL, NULL, "Load a TunerStudio .table file");
    TB_ADD(TB_SAVE, LT_ICON_FILE_DOWN, cell ? "Save" : NULL, NULL, "Save this table as a TunerStudio .table file");
    TB_ADD(TB_REVERT, LT_ICON_ROTATE_CCW, cell ? "Revert" : NULL, NULL, "Undo every edit made in this tab");
    TB_SEP();
    TB_ADD(TB_LIVE, LT_ICON_MAP_PIN, cell ? "Live" : NULL, "g", "Jump to the live cell (g)");
    TB_ADD(TB_FOLLOW, LT_ICON_CROSSHAIR, cell ? "Follow" : NULL, "f", "Follow the live ECU position (f)");
    it[n - 1].on = st->follow;
    TB_GAP();
    TB_ADD(TB_SHADE, LT_ICON_PALETTE, cell ? "Shade" : NULL, NULL, "Colour shading: show the value heat map");
    it[n - 1].on = st->shading;
    TB_ADD(TB_YBOT, LT_ICON_ARROW_UP_DOWN, cell ? "Y-bot" : NULL, NULL,
           st->ybottom ? "Y axis: zero at the bottom (click for zero at the top)"
                       : "Y axis: zero at the top (click for zero at the bottom)");
    it[n - 1].on = st->ybottom;
    if (st->is_afr) {
        TB_ADD(TB_LAMBDA, LT_ICON_EQUAL, cell ? "Lambda" : NULL, NULL, "Lambda preview table next to the AFR table");
        it[n - 1].on = st->lambda_on;
    }
    TB_ADD(TB_3D, LT_ICON_BOX, cell ? "3D" : NULL, NULL, "Toggle the 3D view");
    it[n - 1].on = st->show3d;
    if (st->show3d) {
        static const char *const sty[LT_T3D_STYLES] = { "Surface style: solid (click for wireframe)",
            "Surface style: wireframe (click for points)", "Surface style: points (click for solid)" };
        static const char *const cm[LT_T3D_MODES] = { "Colour: by value (click for by change)",
            "Colour: by change from the values at open (click for a single colour)",
            "Colour: single colour (click for by value)" };
        TB_ADD(TB_STYLE3D, LT_ICON_BOXES, cell ? "Style" : NULL, NULL, sty[st->t3.style]);
        TB_ADD(TB_COLOR3D, LT_ICON_LAYERS, cell ? "Color" : NULL, NULL, cm[st->t3.color_mode]);
        TB_ADD(TB_RESET3D, LT_ICON_ROTATE_CW, cell ? "Reset3D" : NULL, "Home", "Reset the 3D camera (Home)");
    }
    TB_SEP();
    TB_ADD(TB_BURN, LT_ICON_FLAME, "Burn", NULL, "Write the changes to ECU flash");
    it[n - 1].accent = 1;
    it[n - 1].disabled = lt_app_can_burn(app) ? 0 : 1;
    tb_item(&it[n], LT_VK_END, 0, NULL, NULL, NULL);
#undef TB_ADD
#undef TB_SEP
#undef TB_GAP
    hit = lt_vk_toolbar(ui, it, cell ? 0 : 32);
    if (hit < 0) { return; }
    switch (ids[hit]) {
    case TB_EQ: tbl_op(app, tab, st, OP_EQUAL, 0); break;
    case TB_UP: tbl_op(app, tab, st, OP_ADD, 1.0); break;
    case TB_DOWN: tbl_op(app, tab, st, OP_ADD, -1.0); break;
    case TB_SCALE: st->scale_open = 1; break;
    case TB_INTERP: tbl_op(app, tab, st, OP_INTERP, 0); break;
    case TB_SMOOTH: tbl_op(app, tab, st, OP_SMOOTH, 0); break;
    case TB_FILLR: tbl_op(app, tab, st, OP_FILL_R, 0); break;
    case TB_FILLD: tbl_op(app, tab, st, OP_FILL_D, 0); break;
    case TB_REBIN: rebin_prefill(st); st->rebin_open = 1; break;
    case TB_COPY: tbl_copy(app, st); break;
    case TB_PASTE: tbl_paste(app, tab, st, LT_TBL_PASTE_REPLACE); break;
    case TB_SIZE: tbl_open_size(app, st); break;
    case TB_GEN: tbl_open_gen(st); break;
    case TB_LOAD: tbl_file_load(app, st); break;
    case TB_SAVE: tbl_file_save(app, tab, st); break;
    case TB_UNDO: tbl_undo_redo(app, tab, st, 0); break;
    case TB_REDO: tbl_undo_redo(app, tab, st, 1); break;
    case TB_REVERT: tbl_revert(app, tab, st); break;
    case TB_LIVE: st->goto_live = 1; break;
    case TB_FOLLOW: st->follow = !st->follow; break;
    case TB_SHADE: st->shading = !st->shading; break;
    case TB_YBOT: st->ybottom = !st->ybottom; break;
    case TB_3D: st->show3d = !st->show3d; break;
    case TB_LAMBDA: st->lambda_on = !st->lambda_on; break;
    case TB_RESET3D: lt_t3d_reset_view(&st->t3); break;
    case TB_STYLE3D: st->t3.style = (st->t3.style + 1) % LT_T3D_STYLES; break;
    case TB_COLOR3D: st->t3.color_mode = (st->t3.color_mode + 1) % LT_T3D_MODES; break;
    case TB_GRID3D: st->t3.grid = !st->t3.grid; break;
    case TB_BURN: lt_app_burn(app); tab->dirty = 0; break;
    default: break;
    }
}

/* write the whole grid (all cells) back clamped and rounded to the constant's precision */
static void tbl_fix_all(tbl_tab *st)
{
    size_t y, x, n = 0;
    lt_tcell *all = (lt_tcell *)lt_calloc(st->v.rows * st->v.cols + 1, sizeof(lt_tcell));
    for (y = 0; y < st->v.rows; y++) {
        for (x = 0; x < st->v.cols; x++) { all[n].y = y; all[n].x = x; n++; }
    }
    tbl_fix(st, all, n);
    lt_free(all);
}

static void tbl_apply_gen(lt_app *app, lt_app_tab *tab, tbl_tab *st, const double *nx,
                          const double *ny, const double *nz)
{
    size_t y, x;
    lt_tblview_push_undo(&st->v);
    if (st->gen_dlg.write_axes) {
        for (x = 0; x < st->v.cols; x++) { lt_tblview_set_x(&st->v, x, lt_tabs_round(nx[x], st->xdig)); }
        for (y = 0; y < st->v.rows; y++) { lt_tblview_set_y(&st->v, y, lt_tabs_round(ny[y], st->ydig)); }
    }
    for (y = 0; y < st->v.rows; y++) {
        for (x = 0; x < st->v.cols; x++) { lt_tblview_set(&st->v, y, x, nz[y * st->v.cols + x]); }
    }
    tbl_fix_all(st);
    tbl_commit(app, tab, st);
    lt_app_toast(app, 1, "Table generated from engine specs");
}

static void tbl_apply_io(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_tabs_io_dlg *d = &st->io_dlg;
    double *z = (double *)lt_calloc(st->v.rows * st->v.cols + 1, sizeof(double));
    int rc = lt_tabs_io_fit(&d->tf, st->v.x, st->v.cols, st->v.y, st->v.rows, z);
    if (rc == 0) {
        if (d->match) {
            rc = lt_tblview_import(&st->v, st->v.cols, st->v.rows, d->use_axes ? d->tf.x : NULL,
                                   d->use_axes ? d->tf.y : NULL, z);
        } else {
            size_t i;
            lt_tblview_push_undo(&st->v);
            for (i = 0; i < st->v.rows * st->v.cols; i++) {
                lt_tblview_set(&st->v, i / st->v.cols, i % st->v.cols, z[i]);
            }
        }
    }
    lt_free(z);
    if (rc < 0) {
        lt_app_toast(app, 3, "Load failed: %s", lt_err_get());
    } else {
        tbl_fix_all(st);
        tbl_commit(app, tab, st);
        lt_app_toast(app, 1, "Table loaded from file");
    }
    lt_tabs_io_free(d);
}

static void tbl_file_results(lt_app *app, tbl_tab *st)
{
    const char *path;
    if (st->fd_load && (path = lt_app_file_result(app, LT_TABS_FD_LOAD)) != NULL) {
        st->fd_load = 0;
        if (lt_tabs_io_open_file(&st->io_dlg, path, st->v.cols, st->v.rows) < 0) {
            lt_app_toast(app, 3, "Cannot load table: %s", lt_err_get());
        }
    }
    if (st->fd_save && (path = lt_app_file_result(app, LT_TABS_FD_SAVE)) != NULL) {
        st->fd_save = 0;
        if (lt_tabs_io_save(path, st->xlab[0] ? st->xlab : "X", st->ylab[0] ? st->ylab : "Y",
                            st->v.x, st->v.cols, st->v.y, st->v.rows, st->v.z) < 0) {
            lt_app_toast(app, 3, "Save failed: %s", lt_err_get());
        } else {
            lt_app_toast(app, 1, "Table saved to %s", path);
        }
    }
    if (!app->fd.open) { st->fd_load = st->fd_save = 0; }
}

static void tbl_dialogs(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    int w1 = -1, w2[2], wb[3];
    static const char *const modes[3] = { "Multiply by", "Add", "Set to" };
    static const char *const pmodes[6] = { "Replace", "Add to cells", "Subtract from cells",
                                           "Multiply cells", "Keep the lower", "Keep the higher" };
    tbl_file_results(app, st);
    if (st->scale_open && lt_ui_modal_begin(ui, "Adjust selection", 360, 150, &st->scale_open)) {
        w2[0] = 110; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_combo(ui, "mode", &st->scale_mode, modes, 3);
        lt_ui_number(ui, "val", &st->scale_val, -100000.0, 100000.0, 0.05, 3);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label(ui, "Applies to the selected cells, clamped to the allowed range.");
        wb[0] = -1; wb[1] = 90; wb[2] = 90;
        lt_ui_layout_row(ui, 3, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_DEFAULT)) {
            if (st->scale_mode == 0) { tbl_op(app, tab, st, OP_SCALE, st->scale_val); }
            else if (st->scale_mode == 1) { tbl_op(app, tab, st, OP_ADD, st->scale_val); }
            else { tbl_op(app, tab, st, OP_SET, st->scale_val); }
            st->scale_open = 0;
        }
        if (lt_ui_button(ui, "Cancel")) { st->scale_open = 0; }
        lt_ui_modal_end(ui);
    }
    if (st->step_open && lt_ui_modal_begin(ui, "Increment settings", 420, 190, &st->step_open)) {
        w2[0] = 250; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Step amount (+ - > < ,)");
        lt_ui_number(ui, "st_amt", &st->step, 0.0001, 100000.0, 0.1, 4);
        lt_ui_label(ui, "Step multiplier (with Ctrl)");
        lt_ui_number(ui, "st_mul", &st->step_mult, 1.0, 1000.0, 1.0, 2);
        lt_ui_label(ui, "Step percent (Ctrl/Shift+PageUp/Dn)");
        lt_ui_number(ui, "st_pct", &st->step_pct, 0.01, 1000.0, 0.5, 2);
        wb[0] = -1; wb[1] = 90;
        lt_ui_layout_row(ui, 2, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Done", LT_UI_BTN_DEFAULT)) { st->step_open = 0; }
        lt_ui_modal_end(ui);
    }
    if (st->paste_open && lt_ui_modal_begin(ui, "Paste special", 360, 140, &st->paste_open)) {
        w2[0] = 90; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Combine");
        lt_ui_combo(ui, "pmode", &st->paste_mode, pmodes, 6);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label(ui, "Pastes the clipboard at the selection, combining it with each cell.");
        wb[0] = -1; wb[1] = 90; wb[2] = 90;
        lt_ui_layout_row(ui, 3, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Paste", LT_UI_BTN_DEFAULT)) {
            tbl_paste(app, tab, st, st->paste_mode);
            st->paste_open = 0;
        }
        if (lt_ui_button(ui, "Cancel")) { st->paste_open = 0; }
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
    if (st->size_dlg.open && lt_tabs_size_draw(app, st->t, &st->size_dlg) == 1) {
        tbl_load(app, tab, st);
        tab->dirty = 1;
        lt_app_toast(app, 1, "Table size changed to %d x %d", (int)st->v.cols, (int)st->v.rows);
    }
    if (st->gen_dlg.open) {
        size_t nx = st->v.cols, ny = st->v.rows;
        double *ox = (double *)lt_calloc(nx + 1, sizeof(double));
        double *oy = (double *)lt_calloc(ny + 1, sizeof(double));
        double *oz = (double *)lt_calloc(nx * ny + 1, sizeof(double));
        if (lt_tabs_gen_draw(app, &st->gen_dlg, tab->title, st->v.x, nx, st->v.y, ny, st->zdig,
                             ox, oy, oz) == 1) {
            tbl_apply_gen(app, tab, st, ox, oy, oz);
        }
        lt_free(ox); lt_free(oy); lt_free(oz);
    }
    if (st->io_dlg.open && lt_tabs_io_draw(app, &st->io_dlg, st->v.cols, st->v.rows) == 1) {
        tbl_apply_io(app, tab, st);
    }
}

/* live cell, trail line and dots drawn over the cells (pixel mode) */
static void ov_draw(lt_gfx *g, lt_rect r, void *user)
{
    const tbl_tab *st = (const tbl_tab *)user;
    int k;
    LT_UNUSED(r);
    for (k = st->ov_n - 1; k > 0; k--) {
        int a = 215 - k * 150 / TRAIL_N;
        lt_color c = (st->ov_trail & 0x00FFFFFFUL) | ((lt_u32)a << 24);
        lt_gfx_line(g, st->ov_xy[k][0], st->ov_xy[k][1], st->ov_xy[k - 1][0], st->ov_xy[k - 1][1], lt_gfx_fpx(g, 2.5), c);
    }
    for (k = st->ov_n - 1; k >= 1; k--) {
        int a = 235 - k * 150 / TRAIL_N;
        lt_color c = (st->ov_trail & 0x00FFFFFFUL) | ((lt_u32)a << 24);
        lt_gfx_fill_circle(g, st->ov_xy[k][0], st->ov_xy[k][1], lt_gfx_fpx(g, 3.5), c);
    }
    if (st->ov_live) {
        lt_rect c = st->ov_live_r;
        lt_color tint = (st->ov_live_c & 0x00FFFFFFUL) | 0x33000000UL;
        lt_gfx_fill_rect(g, c, tint);
        lt_gfx_rect(g, c, st->ov_live_c);
        lt_gfx_rect(g, lt_rect_inset(c, 1, 1), st->ov_live_c);
        {
            /* marker in the corner so the cell value stays readable */
            double mx = (double)c.x + (double)c.w - 7.0, my = (double)c.y + 7.0;
            lt_gfx_fill_circle(g, mx, my, 4.5, LT_RGBA(0, 0, 0, 90));
            lt_gfx_fill_circle(g, mx, my, 3.5, st->ov_trail);
            lt_gfx_circle(g, mx, my, 4.0, 1.0, LT_RGB(255, 255, 255));
        }
    }
}

static void tbl_overlay_record(lt_ui *ui, tbl_tab *st, const lt_theme *th, int rows)
{
    int k, n = 0;
    st->ov_trail = th->cursor_trail;
    st->ov_live_c = th->cursor_live;
    if (st->app_ != NULL) {                              /* colours from the table display prefs (G-93) */
        lt_color pc;
        if (lt_feat_prefs_color(st->app_, 0, &pc)) { st->ov_live_c = pc; }
        if (lt_feat_prefs_color(st->app_, 1, &pc)) { st->ov_trail = pc; }
    }
    if (st->live_ok) {
        int lr = st->ybottom ? rows - 1 - st->live_r : st->live_r;
        lt_rect c = lt_ui_grid_cell_rect(ui, lr, st->live_c);
        st->ov_live_r = c;
        st->ov_live = 1;
        st->ov_xy[n][0] = (double)c.x + (double)c.w * 0.5;
        st->ov_xy[n][1] = (double)c.y + (double)c.h * 0.5;
        n++;
    }
    for (k = 0; k < st->ntrail && n < TRAIL_N; k++) {
        int dr2 = st->ybottom ? rows - 1 - st->trail_r[k] : st->trail_r[k];
        lt_rect c;
        if (k == 0 && st->live_ok) { continue; }
        c = lt_ui_grid_cell_rect(ui, dr2, st->trail_c[k]);
        st->ov_xy[n][0] = (double)c.x + (double)c.w * 0.5;
        st->ov_xy[n][1] = (double)c.y + (double)c.h * 0.5;
        n++;
    }
    st->ov_n = n;
    if (n > 0 || st->ov_live) { lt_ui_draw_custom(ui, lt_ui_clip_get(ui), ov_draw, st); }
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
    if (!cell && !st->show3d && !st->lambda_on && cols > 0) {
        /* stretch the columns to the viewport like the stock "fit" mode (not for tiny tables) */
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui) - lt_ui_theme(ui)->scrollbar_w) - rhw - 2;
        int fit = avail / cols;
        if (cols >= 12 && fit > cw) { cw = fit > 96 ? 96 : fit; }
    }
    for (k = 0; k < st->v.rows * st->v.cols; k++) {
        if (st->v.z[k] < zlo) { zlo = st->v.z[k]; }
        if (st->v.z[k] > zhi) { zhi = st->v.z[k]; }
    }
    if (lt_ui_grid_begin(ui, "grid", rows, cols, cw, ch, rhw, chh)) {
        int r0, r1, c0, c1, r, c, fl;
        st->grid_area = lt_ui_clip_get(ui);
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
                lt_ui_grid_cell(ui, r, c, b, bg, st->shading ? lt_tabs_heat_text(bg) : th->text);
            }
        }
        /* live trail and cursor */
        st->ov_n = 0; st->ov_live = 0; st->ov_have_cur = 0;
        if (cell) {
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
        } else {
            tbl_overlay_record(ui, st, th, rows);
        }
        if (st->goto_live || st->follow) {
            if (st->live_ok) {
                int lr = st->ybottom ? rows - 1 - st->live_r : st->live_r;
                lt_ui_grid_select(ui, lr, st->live_c, lr, st->live_c);
                lt_ui_grid_set_cursor(ui, lr, st->live_c);
            }
            st->goto_live = 0;
        }
        if (st->select_all) {
            lt_ui_grid_select(ui, 0, 0, rows - 1, cols - 1);
            st->select_all = 0;
        }
        if (st->pick_pending) {
            int pr = st->ybottom ? rows - 1 - st->pick_r : st->pick_r;
            if (pr >= 0 && pr < rows && st->pick_c >= 0 && st->pick_c < cols) {
                lt_ui_grid_select(ui, pr, st->pick_c, pr, st->pick_c);
                lt_ui_grid_set_cursor(ui, pr, st->pick_c);
            }
            st->pick_pending = 0;
        }
        /* right click on a cell outside the selection selects it first (like the original) */
        {
            int rd = lt_ui_mouse_down(ui, LT_MB_RIGHT) ? 1 : 0, hr, hc;
            if (rd && !st->prev_rdown && lt_ui_grid_hover(ui, &hr, &hc) && !lt_ui_grid_selected(ui, hr, hc)) {
                lt_ui_grid_select(ui, hr, hc, hr, hc);
                lt_ui_grid_set_cursor(ui, hr, hc);
            }
            st->prev_rdown = rd;
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
        double dmax = 0.0;
        int have_orig = st->orig != NULL && st->orig_n == st->v.rows * st->v.cols;
        if (st->t3.color_mode == LT_T3D_BY_CHANGE && have_orig) {
            for (k = 0; k < st->orig_n; k++) {
                double d = fabs(st->v.z[k] - st->orig[k]);
                if (d > dmax) { dmax = d; }
            }
        }
        for (k = 0; k < st->v.rows * st->v.cols; k++) {
            if (st->t3.color_mode == LT_T3D_SOLIDCOL) {
                st->cellcol[k] = th->accent;
            } else if (st->t3.color_mode == LT_T3D_BY_CHANGE) {
                /* grey = unchanged, red = raised, blue = lowered (relative to the values at open) */
                double d = have_orig ? st->v.z[k] - st->orig[k] : 0.0;
                double t = dmax > 0.0 ? fabs(d) / dmax : 0.0;
                lt_color grey = LT_RGB(0x80, 0x84, 0x8C);
                st->cellcol[k] = lt_color_mix(grey, d >= 0.0 ? LT_RGB(0xE5, 0x48, 0x4D) : LT_RGB(0x3B, 0x82, 0xF6),
                                              (int)(t * 255.0));
            } else {
                st->cellcol[k] = lt_tabs_heat(app, th, zlo, zhi, st->v.z[k]);
            }
        }
        st->col_edge = LT_RGBA(0, 0, 0, 90);
        st->col_live = th->cursor_live;
        st->col_cur = th->cursor_selected;
        st->col_text = th->text2;
        st->col_trail = th->cursor_trail;
        st->dark3d = th->dark;
        st->app_ = app;
    }
}

/* title strip above the toolbar: table title, size and dirty marker */
static void tbl_header(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, tw;
    lt_rect r;
    char b[96];
    if (lt_ui_cell_mode(ui)) { return; }
    lt_ui_layout_row(ui, 1, &w1, 30);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->bg_app);
    lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y + r.h - 1, th->border_subtle);
    tw = lt_vk_text_w(ui, (int)LT_FONT_SANS_SEMIBOLD, 13.0, tab->title);
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 12), r.y, r.w - lt_ui_px(ui, 160), r.h), (int)LT_FONT_SANS_SEMIBOLD, 13.0,
               tab->title, th->text, 0);
    lt_snprintf(b, sizeof(b), "%d x %d", (int)st->v.cols, (int)st->v.rows);
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 12) + tw + lt_ui_px(ui, 10), r.y, lt_ui_px(ui, 80), r.h), (int)LT_FONT_MONO, 11.0, b, th->muted, 0);
    if (lt_tblview_is_dirty(&st->v)) {
        lt_vk_pill(ui, lt_rect_make(r.x + r.w - lt_ui_px(ui, 86), r.y + lt_ui_px(ui, 7), lt_ui_px(ui, 74), lt_ui_px(ui, 16)), th->warning_bg, th->warning);
        lt_vk_text(ui, lt_rect_make(r.x + r.w - lt_ui_px(ui, 86), r.y + lt_ui_px(ui, 7), lt_ui_px(ui, 74), lt_ui_px(ui, 16)), (int)LT_FONT_SANS_SEMIBOLD, 10.0,
                   "MODIFIED", lt_color_contrast(th->warning_bg), 1);
    }
}

/* status strip: cursor cell, selection, range and live position */
static void tbl_status(lt_app *app, lt_app_tab *tab, tbl_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, cr = st->cur_r, cc = st->cur_c, cols = (int)st->v.cols, x;
    char b[200], xs[32], ys[32], vs[32];
    double cv = (cr >= 0 && cc >= 0 && (size_t)cr < st->v.rows && cc < cols)
                    ? st->v.z[(size_t)cr * (size_t)cols + (size_t)cc] : 0.0;
    lt_rect r;
    lt_tabs_fmt(vs, sizeof(vs), cv, st->zdig);
    lt_tabs_fmt(xs, sizeof(xs), st->v.x[cc < cols ? cc : 0], st->xdig);
    lt_tabs_fmt(ys, sizeof(ys), st->v.y != NULL ? st->v.y[cr < (int)st->v.rows ? cr : 0] : 0.0, st->ydig);
    if (lt_ui_cell_mode(ui)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "%s | cell [%s, %s] = %s | %d selected | range %g..%g%s", tab->title,
                    xs, ys, vs, st->nsel, st->lo, st->hi, lt_tblview_is_dirty(&st->v) ? " | modified" : "");
        if (st->live_ok) {
            char l[80];
            lt_snprintf(l, sizeof(l), " | live x=%g", st->live_x);
            if (strlen(b) + strlen(l) < sizeof(b)) { strcat(b, l); }
        }
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
        return;
    }
    lt_ui_layout_row(ui, 1, &w1, 26);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->statusbar);
    lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y, th->border_subtle);
    x = r.x + lt_ui_px(ui, 12);
    lt_snprintf(b, sizeof(b), "[%s, %s]", xs, ys);
    lt_vk_text(ui, lt_rect_make(x, r.y, lt_ui_px(ui, 160), r.h), (int)LT_FONT_MONO, 11.0, b, th->text2, 0);
    x += lt_vk_text_w(ui, (int)LT_FONT_MONO, 11.0, b) + lt_ui_px(ui, 10);
    lt_vk_text(ui, lt_rect_make(x, r.y, lt_ui_px(ui, 90), r.h), (int)LT_FONT_MONO_MEDIUM, 12.0, vs, th->text, 0);
    x += lt_vk_text_w(ui, (int)LT_FONT_MONO_MEDIUM, 12.0, vs) + lt_ui_px(ui, 18);
    lt_snprintf(b, sizeof(b), "%d selected", st->nsel);
    lt_vk_text(ui, lt_rect_make(x, r.y, lt_ui_px(ui, 120), r.h), (int)LT_FONT_SANS, 11.0, b, th->muted, 0);
    x += lt_vk_text_w(ui, (int)LT_FONT_SANS, 11.0, b) + lt_ui_px(ui, 18);
    lt_snprintf(b, sizeof(b), "range %g .. %g", st->lo, st->hi);
    lt_vk_text(ui, lt_rect_make(x, r.y, lt_ui_px(ui, 200), r.h), (int)LT_FONT_SANS, 11.0, b, th->muted, 0);
    if (st->live_ok) {
        const char *ch = st->t->x_channel != NULL ? st->t->x_channel : "live";
        lt_snprintf(b, sizeof(b), "%s = %g", ch, st->live_x);
        lt_vk_text(ui, lt_rect_make(r.x + r.w - lt_ui_px(ui, 220), r.y, lt_ui_px(ui, 208), r.h), (int)LT_FONT_MONO, 11.0, b,
                   th->cursor_live, 2);
        lt_ui_draw_circle_fill(ui, (double)(r.x + r.w - lt_ui_px(ui, 226)), (double)(r.y + r.h / 2), 3.0 * (double)lt_ui_scale_pct(ui) / 100.0, th->cursor_live);
    }
}

/* right-click menu over the grid, same command set as the toolbar and hotkeys */
static void tbl_context(lt_app *app, lt_app_tab *tab, tbl_tab *st, lt_rect area)
{
    lt_ui *ui = app->ui;
    if (!lt_ui_context_begin(ui, "tbl_ctx", area)) { return; }
    if (lt_ui_menu_item(ui, "Reset to original", "Esc", 0)) { tbl_op(app, tab, st, OP_RESTORE, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Set equal (average)", "=", 0)) { tbl_op(app, tab, st, OP_EQUAL, 0); }
    if (lt_ui_menu_item(ui, "Set value...", NULL, 0)) { st->scale_open = 1; st->scale_mode = 2; }
    if (lt_ui_menu_item(ui, "Step up", "> or +", 0)) { tbl_op(app, tab, st, OP_ADD, st->step); }
    if (lt_ui_menu_item(ui, "Step down", "< or -", 0)) { tbl_op(app, tab, st, OP_ADD, -st->step); }
    if (lt_ui_menu_item(ui, "Add amount...", NULL, 0)) { st->scale_open = 1; st->scale_mode = 1; }
    if (lt_ui_menu_item(ui, "Multiply by...", "*", 0)) { st->scale_open = 1; st->scale_mode = 0; }
    if (lt_ui_menu_item(ui, "Clear", "Del", 0)) { tbl_op(app, tab, st, OP_CLEAR, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Interpolate", "/", 0)) { tbl_op(app, tab, st, OP_INTERP, 0); }
    if (lt_ui_menu_item(ui, "Interpolate horizontally", "H", 0)) { tbl_op(app, tab, st, OP_INTERP_H, 0); }
    if (lt_ui_menu_item(ui, "Interpolate vertically", "V", 0)) { tbl_op(app, tab, st, OP_INTERP_V, 0); }
    if (lt_ui_menu_item(ui, "Smooth", "S", 0)) { tbl_op(app, tab, st, OP_SMOOTH, 0); }
    if (lt_ui_menu_item(ui, "Fill right", "Ctrl+R", 0)) { tbl_op(app, tab, st, OP_FILL_R, 0); }
    if (lt_ui_menu_item(ui, "Fill down", "Ctrl+D", 0)) { tbl_op(app, tab, st, OP_FILL_D, 0); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Copy", "Ctrl+C", 0)) { tbl_copy(app, st); }
    if (lt_ui_menu_item(ui, "Paste", "Ctrl+V", 0)) { tbl_paste(app, tab, st, LT_TBL_PASTE_REPLACE); }
    if (lt_ui_menu_item(ui, "Paste special...", "Ctrl+Shift+V", 0)) { st->paste_open = 1; }
    if (lt_ui_menu_item(ui, "Select all", "Ctrl+A", 0)) {
        st->select_all = 1;
    }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Set increments...", NULL, 0)) { st->step_open = 1; }
    if (st->resizable && lt_ui_menu_item(ui, "Set table size...", NULL, 0)) { tbl_open_size(app, st); }
    if (gen_ok(st) && lt_ui_menu_item(ui, "Generate from engine specs...", NULL, 0)) { tbl_open_gen(st); }
    if (lt_ui_menu_item(ui, "Load table file...", NULL, 0)) { tbl_file_load(app, st); }
    if (lt_ui_menu_item(ui, "Save table file...", NULL, 0)) { tbl_file_save(app, tab, st); }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "Cell color by value", NULL, st->shading ? LT_UI_MENU_CHECKED : 0)) { st->shading = !st->shading; }
    if (lt_ui_menu_item(ui, "3D view", "3", st->show3d ? LT_UI_MENU_CHECKED : 0)) { st->show3d = !st->show3d; }
    if (st->show3d) {
        static const char *const sty[LT_T3D_STYLES] = { "3D style: solid", "3D style: wireframe", "3D style: points" };
        static const char *const cm[LT_T3D_MODES] = { "3D colour: by value", "3D colour: by change", "3D colour: single" };
        int i;
        for (i = 0; i < LT_T3D_STYLES; i++) {
            if (lt_ui_menu_item(ui, sty[i], NULL, st->t3.style == i ? LT_UI_MENU_CHECKED : 0)) { st->t3.style = i; }
        }
        for (i = 0; i < LT_T3D_MODES; i++) {
            if (lt_ui_menu_item(ui, cm[i], NULL, st->t3.color_mode == i ? LT_UI_MENU_CHECKED : 0)) { st->t3.color_mode = i; }
        }
        if (lt_ui_menu_item(ui, "3D grid lines", NULL, st->t3.grid ? LT_UI_MENU_CHECKED : 0)) { st->t3.grid = !st->t3.grid; }
        if (lt_ui_menu_item(ui, "Reset 3D view", "Home", 0)) { lt_t3d_reset_view(&st->t3); }
    }
    lt_ui_context_end(ui);
}

/* scripted preview (screenshots): LT_TABLE_VIEW=3d[,wire|points][,change|single][,nogrid][,zoom][,pick],
 * lambda */
static void tbl_preview_env(tbl_tab *st)
{
    char *e = lt_plat_getenv("LT_TABLE_VIEW");
    if (e == NULL) { return; }
    if (strstr(e, "3d") != NULL) { st->show3d = 1; }
    if (strstr(e, "nolambda") != NULL) { st->lambda_on = 0; }
    else if (strstr(e, "lambda") != NULL) { st->lambda_on = st->is_afr; }
    if (strstr(e, "wire") != NULL) { st->t3.style = LT_T3D_WIRE; }
    if (strstr(e, "points") != NULL) { st->t3.style = LT_T3D_POINTS; }
    if (strstr(e, "change") != NULL) { st->t3.color_mode = LT_T3D_BY_CHANGE; }
    if (strstr(e, "single") != NULL) { st->t3.color_mode = LT_T3D_SOLIDCOL; }
    if (strstr(e, "nogrid") != NULL) { st->t3.grid = 0; }
    if (strstr(e, "zoom") != NULL) { lt_t3d_zoom(&st->t3, 0.6); }
    if (strstr(e, "pick") != NULL && st->loaded) {
        st->pick_pending = 1; st->picked = 1;
        st->pick_r = (int)st->v.rows / 2; st->pick_c = (int)st->v.cols / 2;
    }
    lt_free(e);
}

static void tbl_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    tbl_tab *st = (tbl_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], w3[3];
    LT_UNUSED(r);
    if (st == NULL) {
        st = (tbl_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        st->shading = 1; st->ybottom = app->settings.prefs.y_bottom ? 1 : 0; lt_t3d_init(&st->t3);
        st->rebin_interp = 1; st->scale_val = 1.05; st->cur_r = st->cur_c = 0;
        st->step = 1.0; st->step_mult = 5.0; st->step_pct = 5.0;
        tbl_load(app, tab, st);
        st->lambda_on = st->is_afr && !lt_ui_cell_mode(ui);
        tbl_preview_env(st);
    }
    st->frame++;
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, tab->title);
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    tbl_units_sync(app, st);
    if (st->frame % 15 == 0) { tbl_check_external(app, tab, st); }
    if (!st->loaded) { return; }
    tbl_live(app, st);
    tbl_3d_keys(ui, st);
    tbl_keys(app, tab, st);
    tbl_header(app, tab, st);
    tbl_toolbar(app, tab, st);
    if ((st->show3d || st->lambda_on) && !lt_ui_cell_mode(ui)) {
        int n2 = 1;
        w3[0] = -3;
        if (st->lambda_on) { w3[n2++] = -2; }
        if (st->show3d) { w3[n2++] = -2; }
        lt_ui_layout_row(ui, n2, w3, -28);
    } else if (st->show3d || st->lambda_on) {
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row_px(ui, 2, w2, -(lt_ui_row_height(ui) + 2));
    } else {
        lt_ui_layout_row(ui, 1, &w1, lt_ui_cell_mode(ui) ? -(lt_ui_row_height(ui) + 8) : -28);
    }
    tbl_grid(app, tab, st, 0);
    tbl_context(app, tab, st, st->grid_area);
    if (st->lambda_on) { lt_tabs_lambda_draw(app, &st->v, st->ybottom); }
    if (st->show3d) {
        lt_rect r3 = lt_ui_layout_next(ui);
        tbl_3d_mouse(ui, st, r3);
        lt_ui_draw_box(ui, r3, th->table_bg, th->border_subtle, 0);
        if (lt_ui_cell_mode(ui)) {
            lt_ui_draw_text_in(ui, r3, "3D view needs pixel mode", th->muted, 1);
        } else {
            lt_ui_draw_custom(ui, r3, draw3d, st);
        }
    }
    tbl_status(app, tab, st);
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
