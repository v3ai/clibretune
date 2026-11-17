/* tableops_edit.c - editor-level operations: paste modes, restore, delete/clear and
 * table resize (axis regeneration + Z resampling, including tables sharing the size).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tableops.h"

void lt_tbl_restore(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                    const double *orig)
{
    size_t i;
    if (z == NULL || orig == NULL) { return; }
    for (i = 0; i < n; i++) {
        if (sel[i].y < rows && sel[i].x < cols) {
            z[sel[i].y * cols + sel[i].x] = orig[sel[i].y * cols + sel[i].x];
        }
    }
}

void lt_tbl_clear(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double lo,
                  double hi)
{
    double v = 0.0;
    if (hi > lo) { v = v < lo ? lo : (v > hi ? hi : v); }
    lt_tbl_set_equal(z, rows, cols, sel, n, v);
}

static double paste_combine(double cur, double src, int mode)
{
    switch (mode) {
    case LT_TBL_PASTE_ADD: return cur + src;
    case LT_TBL_PASTE_SUB: return cur - src;
    case LT_TBL_PASTE_MUL: return cur * src;
    case LT_TBL_PASTE_MIN: return src < cur ? src : cur;
    case LT_TBL_PASTE_MAX: return src > cur ? src : cur;
    default: return src;
    }
}

void lt_tbl_paste_op(double *z, size_t rows, size_t cols, size_t y0, size_t x0,
                     const double *src, size_t bh, size_t bw, int mode)
{
    size_t y, x;
    for (y = 0; y < bh && y0 + y < rows; y++) {
        for (x = 0; x < bw && x0 + x < cols; x++) {
            double *d = &z[(y0 + y) * cols + x0 + x];
            *d = paste_combine(*d, src[y * bw + x], mode);
        }
    }
}

double lt_tbl_clamp_between(const double *x, size_t n, size_t i, double v)
{
    if (x == NULL || i >= n) { return v; }
    if (i > 0 && v < x[i - 1]) { v = x[i - 1]; }
    if (i + 1 < n && v > x[i + 1]) { v = x[i + 1]; }
    return v;
}

int lt_tbl_resize(const double *xb, size_t nx, const double *yb, size_t ny, const double *z,
                  size_t nnx, size_t nny, double *out_x, double *out_y, double *out_z)
{
    if (xb == NULL || nx == 0 || nnx == 0 || nny == 0 || out_x == NULL || out_y == NULL ||
        out_z == NULL) {
        return LT_EINVAL;
    }
    lt_tbl_linspace(xb[0], xb[nx - 1], nnx, out_x);
    if (yb != NULL && ny > 0) { lt_tbl_linspace(yb[0], yb[ny - 1], nny, out_y); }
    else { lt_tbl_linspace(0.0, (double)(nny - 1), nny, out_y); }
    if (z == NULL || yb == NULL || ny == 0) {
        size_t i;
        for (i = 0; i < nnx * nny; i++) { out_z[i] = 0.0; }
        return 0;
    }
    return lt_tbl_rebin(xb, nx, yb, ny, z, out_x, nnx, out_y, nny, LT_TRUE, out_z);
}

#define RESIZE_MAX_TABLES 16

int lt_table_resize(const lt_ini *ini, const lt_ini_table *t, lt_pagesrc *dst, size_t cols,
                    size_t rows)
{
    lt_table_size s;
    const lt_ini_table *tabs[RESIZE_MAX_TABLES];
    lt_tblview views[RESIZE_MAX_TABLES];
    double *nx[RESIZE_MAX_TABLES], *ny[RESIZE_MAX_TABLES], *nz[RESIZE_MAX_TABLES];
    size_t n, i, x, y;
    int rc, nloaded = 0, nres = 0;
    lt_bool has_self = LT_FALSE;

    rc = lt_table_size_info(ini, t, dst, &s);
    if (rc < 0) { return rc; }
    rc = lt_table_size_allows(&s, cols, rows);
    if (rc < 0) { return rc; }
    n = lt_tables_sharing_size(ini, s.cols_const, s.rows_const, tabs, RESIZE_MAX_TABLES - 1);
    if (n > RESIZE_MAX_TABLES - 1) { n = RESIZE_MAX_TABLES - 1; }
    for (i = 0; i < n; i++) { if (tabs[i] == t) { has_self = LT_TRUE; } }
    if (!has_self) { tabs[n++] = t; }
    /* resample every table while the old size is still active */
    for (i = 0; i < n && rc == 0; i++) {
        lt_tblview_init(&views[i]);
        nloaded++;
        rc = lt_tblview_load_table(&views[i], ini, tabs[i], dst);
        if (rc < 0) { break; }
        nx[i] = (double *)lt_calloc(cols, sizeof(double));
        ny[i] = (double *)lt_calloc(rows, sizeof(double));
        nz[i] = (double *)lt_calloc(rows * cols, sizeof(double));
        nres++;
        rc = lt_tbl_resize(views[i].x, views[i].cols, views[i].y, views[i].rows, views[i].z,
                           cols, rows, nx[i], ny[i], nz[i]);
    }
    if (rc == 0) { rc = lt_table_size_apply(ini, t, dst, cols, rows); }
    for (i = 0; i < n && rc == 0; i++) {
        rc = lt_tblview_load_table(&views[i], ini, tabs[i], dst);
        if (rc < 0) { break; }
        if (views[i].cols != cols || views[i].rows != rows) { rc = LT_EDATA; break; }
        for (x = 0; x < cols; x++) { lt_tblview_set_x(&views[i], x, nx[i][x]); }
        for (y = 0; y < rows; y++) {
            if (views[i].y != NULL) { lt_tblview_set_y(&views[i], y, ny[i][y]); }
            for (x = 0; x < cols; x++) { lt_tblview_set(&views[i], y, x, nz[i][y * cols + x]); }
        }
        rc = lt_tblview_commit(&views[i], ini, dst);
        if (rc > 0) { rc = 0; }
    }
    for (i = 0; i < (size_t)nloaded; i++) { lt_tblview_free(&views[i]); }
    for (i = 0; i < (size_t)nres; i++) { lt_free(nx[i]); lt_free(ny[i]); lt_free(nz[i]); }
    return rc < 0 ? rc : 0;
}
