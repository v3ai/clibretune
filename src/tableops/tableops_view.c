/* tableops_view.c - lt_tblview (table/curve editor model) and INI-backed dynamic sizes.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tableops.h"

#define SNAP_LEN(v) ((v)->cols + (v)->rows + (v)->rows * (v)->cols)

static double *dup_d(const double *src, size_t n)
{
    double *p = (double *)lt_malloc((n ? n : 1) * sizeof(double));
    if (n) memcpy(p, src, n * sizeof(double));
    return p;
}

static void free_undo(lt_tblview *v)
{
    size_t i;
    for (i = 0; i < v->undo_n; i++) lt_free(v->undo[i]);
    v->undo_n = 0;
    v->undo_pos = 0;
}

void lt_tblview_init(lt_tblview *v)
{
    memset(v, 0, sizeof(*v));
}

static void free_data(lt_tblview *v)
{
    free_undo(v);
    lt_free(v->x); lt_free(v->y); lt_free(v->z);
    lt_free(v->x0); lt_free(v->y0); lt_free(v->z0);
    v->x = v->y = v->z = v->x0 = v->y0 = v->z0 = NULL;
    v->rows = v->cols = 0;
    v->x_dirty = v->y_dirty = v->z_dirty = LT_FALSE;
}

void lt_tblview_free(lt_tblview *v)
{
    free_data(v);
    v->table = NULL;
    v->curve = NULL;
}

/* ---- loading ---- */

/* Read constant `name` as an array into out[0..n) (padded with last value / zeros). */
static int read_axis(const lt_ini *ini, const char *name, const lt_pagesrc *src, size_t n,
                     double *out)
{
    const lt_ini_const *c;
    double *tmp;
    int cnt;
    if (!name || !*name) {
        size_t i;
        for (i = 0; i < n; i++) out[i] = (double)i;
        return 0;
    }
    c = lt_ini_find_const(ini, name);
    if (!c) {
        lt_err_set("constant '%s' not found", name);
        return LT_ENOENT;
    }
    cnt = lt_ini_const_get_array(ini, c, src, NULL, 0);
    if (cnt < 0) return cnt;
    tmp = (double *)lt_malloc(((size_t)cnt + 1) * sizeof(double));
    cnt = lt_ini_const_get_array(ini, c, src, tmp, (size_t)cnt);
    if (cnt < 0) {
        lt_free(tmp);
        return cnt;
    }
    lt_dyn_slice_bins(tmp, (size_t)cnt, n, out);
    lt_free(tmp);
    return 0;
}

static const char *const_units(const lt_ini *ini, const char *name)
{
    const lt_ini_const *c = (name && *name) ? lt_ini_find_const(ini, name) : NULL;
    return c ? c->units : NULL;
}

static void conv_arr(const lt_uconv *c, double *a, size_t n)
{
    size_t i;
    if (!lt_uconv_active(c)) return;
    for (i = 0; i < n; i++) a[i] = lt_uconv_to_disp(c, a[i]);
}

/* Set up the converters and convert freshly read (native unit) arrays to display units. */
static void convert_loaded(lt_tblview *v, const lt_ini *ini, const char *xn, const char *yn,
                           const char *zn)
{
    lt_uconv_make(&v->xu, const_units(ini, xn));
    lt_uconv_make(&v->yu, const_units(ini, yn));
    lt_uconv_make(&v->zu, const_units(ini, zn));
    conv_arr(&v->xu, v->x, v->cols);
    if (v->y) conv_arr(&v->yu, v->y, v->rows);
    conv_arr(&v->zu, v->z, v->rows * v->cols);
}

static void finish_load(lt_tblview *v)
{
    v->x0 = dup_d(v->x, v->cols);
    v->y0 = dup_d(v->y, v->rows);
    v->z0 = dup_d(v->z, v->rows * v->cols);
    v->x_dirty = v->y_dirty = v->z_dirty = LT_FALSE;
}

int lt_tblview_load_table(lt_tblview *v, const lt_ini *ini, const lt_ini_table *t,
                          const lt_pagesrc *src)
{
    const lt_ini_const *zc;
    lt_table_size sz;
    size_t rows, cols, n;
    double *flat;
    int rc, cnt;

    if (!v || !ini || !t || !src) return LT_EINVAL;
    zc = t->z_const ? lt_ini_find_const(ini, t->z_const) : NULL;
    if (!zc) {
        lt_err_set("table '%s': constant '%s' not found", t->name, t->z_const ? t->z_const : "");
        return LT_ENOENT;
    }
    cols = t->x_size;
    rows = t->y_size;
    if (lt_table_size_info(ini, t, src, &sz) == 0) {
        cols = sz.active_cols;
        rows = sz.active_rows;
    } else if (cols == 0 || rows == 0) {
        cols = zc->shape.kind == LT_SH_2D ? zc->shape.cols : lt_shape_count(&zc->shape);
        rows = zc->shape.kind == LT_SH_2D ? zc->shape.rows : 1;
    }
    if (cols == 0 || rows == 0 || cols > 65536 || rows > 65536 || cols * rows > 16777216UL)
        return LT_EDATA;

    cnt = lt_ini_const_get_array(ini, zc, src, NULL, 0);
    if (cnt < 0) return cnt;
    n = (size_t)cnt;
    if (n < rows * cols) {
        lt_err_set("table '%s': %lu cells stored, %lux%lu needed", t->name, (unsigned long)n,
                   (unsigned long)rows, (unsigned long)cols);
        return LT_EDATA;
    }
    flat = (double *)lt_malloc((n + 1) * sizeof(double));
    cnt = lt_ini_const_get_array(ini, zc, src, flat, n);
    if (cnt < 0) {
        lt_free(flat);
        return cnt;
    }

    free_data(v);
    v->table = t;
    v->curve = NULL;
    v->rows = rows;
    v->cols = cols;
    v->x = (double *)lt_malloc(cols * sizeof(double));
    v->y = (double *)lt_malloc(rows * sizeof(double));
    v->z = (double *)lt_malloc(rows * cols * sizeof(double));
    lt_dyn_unpack(flat, n, cols, rows, v->z);
    lt_free(flat);
    rc = read_axis(ini, t->x_const, src, cols, v->x);
    if (rc == 0) rc = read_axis(ini, t->y_const, src, rows, v->y);
    if (rc != 0) {
        free_data(v);
        return rc;
    }
    convert_loaded(v, ini, t->x_const, t->y_const, t->z_const);
    finish_load(v);
    return 0;
}

int lt_tblview_load_curve(lt_tblview *v, const lt_ini *ini, const lt_ini_curve *c,
                          const lt_pagesrc *src)
{
    const lt_ini_const *xc;
    size_t cols;
    int rc;

    if (!v || !ini || !c || !src) return LT_EINVAL;
    xc = c->x_const ? lt_ini_find_const(ini, c->x_const) : NULL;
    if (!xc || !c->y_const || !lt_ini_find_const(ini, c->y_const)) {
        lt_err_set("curve '%s': axis constant not found", c->name);
        return LT_ENOENT;
    }
    cols = c->size > 0 ? (size_t)c->size : lt_shape_count(&xc->shape);
    if (cols == 0 || cols > 65536) return LT_EDATA;

    free_data(v);
    v->table = NULL;
    v->curve = c;
    v->rows = 1;
    v->cols = cols;
    v->x = (double *)lt_malloc(cols * sizeof(double));
    v->y = NULL;
    v->z = (double *)lt_malloc(cols * sizeof(double));
    rc = read_axis(ini, c->x_const, src, cols, v->x);
    if (rc == 0) rc = read_axis(ini, c->y_const, src, cols, v->z);
    if (rc != 0) {
        free_data(v);
        return rc;
    }
    convert_loaded(v, ini, c->x_const, NULL, c->y_const);
    v->x0 = dup_d(v->x, cols);
    v->y0 = NULL;
    v->z0 = dup_d(v->z, cols);
    v->x_dirty = v->y_dirty = v->z_dirty = LT_FALSE;
    return 0;
}

int lt_tblview_revert(lt_tblview *v, const lt_ini *ini, const lt_pagesrc *src)
{
    const lt_ini_table *t = v->table;
    const lt_ini_curve *c = v->curve;
    if (t) return lt_tblview_load_table(v, ini, t, src);
    if (c) return lt_tblview_load_curve(v, ini, c, src);
    return LT_EINVAL;
}

/* ---- commit ---- */

/* Write changed elements of axis/values `cur` vs `old` into constant `name`. */
static int commit_array(const lt_ini *ini, const char *name, lt_pagesrc *dst, double *cur,
                        double *old, size_t n, size_t *written, const lt_uconv *u)
{
    const lt_ini_const *c;
    double back;
    size_t i;
    int rc;
    if (!name || !*name) return 0;
    c = lt_ini_find_const(ini, name);
    if (!c) return LT_ENOENT;
    for (i = 0; i < n; i++) {
        if (cur[i] == old[i]) continue;
        rc = lt_ini_const_set(ini, c, dst, i, lt_uconv_from_disp(u, cur[i]));
        if (rc < 0) return rc;
        (*written)++;
        /* adopt the quantised value actually stored */
        if (lt_ini_const_get(ini, c, dst, i, &back) == 0) cur[i] = lt_uconv_to_disp(u, back);
        old[i] = cur[i];
    }
    return 0;
}

int lt_tblview_commit(lt_tblview *v, const lt_ini *ini, lt_pagesrc *dst)
{
    size_t w = 0;
    int rc;
    const char *xn, *yn, *zn;
    if (!v || !ini || !dst || (!v->table && !v->curve) || !v->z) return LT_EINVAL;
    if (v->table) {
        xn = v->table->x_const;
        yn = v->table->y_const;
        zn = v->table->z_const;
    } else {
        xn = v->curve->x_const;
        yn = NULL;
        zn = v->curve->y_const;
    }
    rc = commit_array(ini, xn, dst, v->x, v->x0, v->cols, &w, &v->xu);
    if (rc == 0 && yn && v->y) rc = commit_array(ini, yn, dst, v->y, v->y0, v->rows, &w, &v->yu);
    if (rc == 0) rc = commit_array(ini, zn, dst, v->z, v->z0, v->rows * v->cols, &w, &v->zu);
    if (rc < 0) return rc;
    v->x_dirty = v->y_dirty = v->z_dirty = LT_FALSE;
    return (int)w;
}

/* ---- element access ---- */

static void refresh_flags(lt_tblview *v)
{
    size_t i;
    v->x_dirty = v->y_dirty = v->z_dirty = LT_FALSE;
    for (i = 0; i < v->cols; i++)
        if (v->x[i] != v->x0[i]) v->x_dirty = LT_TRUE;
    if (v->y)
        for (i = 0; i < v->rows; i++)
            if (v->y[i] != v->y0[i]) v->y_dirty = LT_TRUE;
    for (i = 0; i < v->rows * v->cols; i++)
        if (v->z[i] != v->z0[i]) v->z_dirty = LT_TRUE;
}

double lt_tblview_get(const lt_tblview *v, size_t row, size_t col)
{
    if (!v->z || row >= v->rows || col >= v->cols) return 0.0;
    return v->z[row * v->cols + col];
}

void lt_tblview_set(lt_tblview *v, size_t row, size_t col, double value)
{
    size_t k;
    if (!v->z || row >= v->rows || col >= v->cols) return;
    k = row * v->cols + col;
    v->z[k] = value;
    if (value != v->z0[k]) v->z_dirty = LT_TRUE;
    else refresh_flags(v);
}

void lt_tblview_set_x(lt_tblview *v, size_t col, double value)
{
    if (!v->x || col >= v->cols) return;
    v->x[col] = value;
    refresh_flags(v);
}

void lt_tblview_set_y(lt_tblview *v, size_t row, double value)
{
    if (!v->y || row >= v->rows) return;
    v->y[row] = value;
    refresh_flags(v);
}

lt_bool lt_tblview_cell_dirty(const lt_tblview *v, size_t row, size_t col)
{
    size_t k;
    if (!v->z || row >= v->rows || col >= v->cols) return LT_FALSE;
    k = row * v->cols + col;
    return v->z[k] != v->z0[k];
}

lt_bool lt_tblview_is_dirty(const lt_tblview *v)
{
    return v->x_dirty || v->y_dirty || v->z_dirty;
}

void lt_tblview_z_range(const lt_tblview *v, const lt_ini *ini, double *lo, double *hi)
{
    const lt_ini_const *c = NULL;
    const char *zn = v->table ? v->table->z_const : (v->curve ? v->curve->y_const : NULL);
    double mn, mx;
    size_t i, n = v->rows * v->cols;
    if (ini && zn) c = lt_ini_find_const(ini, zn);
    if (c && c->range_resolved && c->max > c->min) {
        mn = c->min;
        mx = c->max;
        lt_uconv_range(&v->zu, &mn, &mx);
    } else if (n > 0 && v->z) {
        mn = mx = v->z[0];
        for (i = 1; i < n; i++) {
            if (v->z[i] < mn) mn = v->z[i];
            if (v->z[i] > mx) mx = v->z[i];
        }
    } else {
        mn = mx = 0.0;
    }
    if (lo) *lo = mn;
    if (hi) *hi = mx;
}

/* ---- undo ring: snapshots are [x | y | z] ---- */

static double *snapshot(const lt_tblview *v)
{
    size_t nx = v->cols, ny = v->rows, nz = v->rows * v->cols;
    double *s = (double *)lt_malloc(SNAP_LEN(v) * sizeof(double));
    memcpy(s, v->x, nx * sizeof(double));
    if (v->y) memcpy(s + nx, v->y, ny * sizeof(double));
    else memset(s + nx, 0, ny * sizeof(double));
    memcpy(s + nx + ny, v->z, nz * sizeof(double));
    return s;
}

static void restore(lt_tblview *v, const double *s)
{
    size_t nx = v->cols, ny = v->rows, nz = v->rows * v->cols;
    memcpy(v->x, s, nx * sizeof(double));
    if (v->y) memcpy(v->y, s + nx, ny * sizeof(double));
    memcpy(v->z, s + nx + ny, nz * sizeof(double));
    refresh_flags(v);
}

void lt_tblview_push_undo(lt_tblview *v)
{
    size_t i;
    if (!v->z) return;
    for (i = v->undo_pos; i < v->undo_n; i++) lt_free(v->undo[i]);   /* drop redo tail */
    v->undo_n = v->undo_pos;
    if (v->undo_n >= LT_TBLVIEW_UNDO_MAX - 1) {   /* keep one slot for the redo tip */
        lt_free(v->undo[0]);
        for (i = 1; i < v->undo_n; i++) v->undo[i - 1] = v->undo[i];
        v->undo_n--;
    }
    v->undo[v->undo_n++] = snapshot(v);
    v->undo_pos = v->undo_n;
}

lt_bool lt_tblview_undo(lt_tblview *v)
{
    if (!v->z || v->undo_pos == 0) return LT_FALSE;
    if (v->undo_pos == v->undo_n) v->undo[v->undo_n++] = snapshot(v);   /* redo tip */
    v->undo_pos--;
    restore(v, v->undo[v->undo_pos]);
    return LT_TRUE;
}

lt_bool lt_tblview_redo(lt_tblview *v)
{
    if (!v->z || v->undo_pos + 1 >= v->undo_n) return LT_FALSE;
    v->undo_pos++;
    restore(v, v->undo[v->undo_pos]);
    return LT_TRUE;
}

/* ---- unit preference changes ---- */

static void retarget_arr(lt_uconv c, double *a, size_t n)
{
    size_t i;
    lt_uconv w;
    for (i = 0; i < n; i++) {
        w = c;
        a[i] = lt_uconv_retarget(&w, a[i]);
    }
}

lt_bool lt_tblview_units_sync(lt_tblview *v)
{
    size_t i, nx, ny, nz;
    lt_bool xs, ys, zs;
    if (!v->x || !v->z) return LT_FALSE;
    xs = lt_uconv_stale(&v->xu);
    ys = v->y ? lt_uconv_stale(&v->yu) : LT_FALSE;
    zs = lt_uconv_stale(&v->zu);
    if (!xs && !ys && !zs) return LT_FALSE;
    nx = v->cols; ny = v->y ? v->rows : 0; nz = v->rows * v->cols;
    for (i = 0; i < v->undo_n; i++) {
        double *s = v->undo[i];
        if (xs) retarget_arr(v->xu, s, nx);
        if (ys) retarget_arr(v->yu, s + nx, ny);
        if (zs) retarget_arr(v->zu, s + v->cols + v->rows, nz);
    }
    if (xs) { retarget_arr(v->xu, v->x, nx); retarget_arr(v->xu, v->x0, nx); }
    if (ys) { retarget_arr(v->yu, v->y, ny); retarget_arr(v->yu, v->y0, ny); }
    if (zs) { retarget_arr(v->zu, v->z, nz); retarget_arr(v->zu, v->z0, nz); }
    if (xs) (void)lt_uconv_retarget(&v->xu, 0.0);
    if (ys) (void)lt_uconv_retarget(&v->yu, 0.0);
    if (zs) (void)lt_uconv_retarget(&v->zu, 0.0);
    return LT_TRUE;
}

/* ---- op wrappers ---- */

void lt_tblview_scale(lt_tblview *v, const lt_tcell *sel, size_t n, double f)
{
    lt_tblview_push_undo(v);
    lt_tbl_scale(v->z, v->rows, v->cols, sel, n, f);
    refresh_flags(v);
}

void lt_tblview_offset(lt_tblview *v, const lt_tcell *sel, size_t n, double off)
{
    lt_tblview_push_undo(v);
    lt_tbl_offset(v->z, v->rows, v->cols, sel, n, off);
    refresh_flags(v);
}

void lt_tblview_set_equal(lt_tblview *v, const lt_tcell *sel, size_t n, double val)
{
    lt_tblview_push_undo(v);
    lt_tbl_set_equal(v->z, v->rows, v->cols, sel, n, val);
    refresh_flags(v);
}

void lt_tblview_interp2d(lt_tblview *v, const lt_tcell *sel, size_t n)
{
    lt_tblview_push_undo(v);
    lt_tbl_interp2d(v->z, v->rows, v->cols, sel, n);
    refresh_flags(v);
}

void lt_tblview_interp_linear(lt_tblview *v, const lt_tcell *sel, size_t n, int axis)
{
    lt_tblview_push_undo(v);
    lt_tbl_interp_linear(v->z, v->rows, v->cols, sel, n, axis);
    refresh_flags(v);
}

void lt_tblview_fill(lt_tblview *v, const lt_tcell *sel, size_t n, int dir)
{
    lt_tblview_push_undo(v);
    lt_tbl_fill(v->z, v->rows, v->cols, sel, n, dir);
    refresh_flags(v);
}

void lt_tblview_smooth(lt_tblview *v, const lt_tcell *sel, size_t n, double factor)
{
    lt_tblview_push_undo(v);
    lt_tbl_smooth(v->z, v->rows, v->cols, sel, n, factor);
    refresh_flags(v);
}

int lt_tblview_import(lt_tblview *v, size_t cols, size_t rows, const double *x,
                      const double *y, const double *z)
{
    if (!v->z || cols != v->cols || rows != v->rows || !z) {
        lt_err_set("table shape %lux%lu does not match the view", (unsigned long)rows,
                   (unsigned long)cols);
        return LT_EINVAL;
    }
    lt_tblview_push_undo(v);
    if (x) memcpy(v->x, x, cols * sizeof(double));
    if (y && v->y) memcpy(v->y, y, rows * sizeof(double));
    memcpy(v->z, z, rows * cols * sizeof(double));
    refresh_flags(v);
    return 0;
}

/* ---- dynamic table size (INI backed) ---- */

static size_t const_bound(const lt_ini_const *c, lt_bool want_max)
{
    double d = want_max ? c->max : c->min;
    if (!(d == d) || d < 1.0) d = 1.0;   /* NaN / below 1 */
    else if (d > 65536.0) d = 65536.0;
    else d = lt_round(d);
    return (size_t)d;
}

int lt_table_size_info(const lt_ini *ini, const lt_ini_table *t, const lt_pagesrc *src,
                       lt_table_size *out)
{
    const lt_ini_const *cc = NULL, *rc = NULL, *zc;
    double raw = 0.0, def = 0.0;
    size_t mx;
    lt_bool has_def;
    void *p;
    lt_bool has;

    if (!ini || !t || !out) return LT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!t->rows_size_const || !*t->rows_size_const) return LT_ENOENT;
    rc = lt_ini_find_const(ini, t->rows_size_const);
    if (!rc) return LT_ENOENT;
    if (t->cols_size_const && *t->cols_size_const) {
        cc = lt_ini_find_const(ini, t->cols_size_const);
        if (!cc) return LT_ENOENT;
        lt_strlcpy(out->cols_const, t->cols_size_const, sizeof out->cols_const);
        out->min_cols = const_bound(cc, LT_FALSE);
        out->max_cols = const_bound(cc, LT_TRUE);
    } else {
        out->min_cols = out->max_cols = t->x_size > 0 ? t->x_size : 1;
    }
    lt_strlcpy(out->rows_const, t->rows_size_const, sizeof out->rows_const);
    out->min_rows = const_bound(rc, LT_FALSE);
    out->max_rows = const_bound(rc, LT_TRUE);

    mx = t->max_elements;
    if (mx == 0 && t->map_name) {
        p = lt_map_get(&ini->maximum_elements, t->map_name);
        if (p) mx = (size_t)p;
    }
    if (mx == 0 && t->z_const) {
        zc = lt_ini_find_const(ini, t->z_const);
        if (zc) mx = lt_shape_count(&zc->shape);
    }
    out->max_elements = mx < 1 ? 1 : mx;

    if (cc) {
        has = src && lt_ini_get(ini, src, cc->name, &raw) == 0;
        has_def = lt_ini_default_value(ini, cc->name, &def);
        out->active_cols = lt_dyn_resolve_axis_count(has, raw, out->min_cols, out->max_cols,
                                                     has_def, def);
    } else {
        out->active_cols = out->min_cols;
    }
    has = src && lt_ini_get(ini, src, rc->name, &raw) == 0;
    has_def = lt_ini_default_value(ini, rc->name, &def);
    out->active_rows = lt_dyn_resolve_axis_count(has, raw, out->min_rows, out->max_rows,
                                                 has_def, def);
    lt_table_size_clamp_to_budget(out);
    return 0;
}

int lt_table_size_apply(const lt_ini *ini, const lt_ini_table *t, lt_pagesrc *dst,
                        size_t cols, size_t rows)
{
    lt_table_size s;
    int rc = lt_table_size_info(ini, t, dst, &s);
    if (rc < 0) return rc;
    rc = lt_table_size_allows(&s, cols, rows);
    if (rc < 0) return rc;
    if (s.cols_const[0]) {
        rc = lt_ini_set(ini, dst, s.cols_const, (double)cols);
        if (rc < 0) return rc;
    }
    return lt_ini_set(ini, dst, s.rows_const, (double)rows);
}

static lt_bool str_eq(const char *a, const char *b)
{
    if (!a || !*a) return !b || !*b;
    return b && strcmp(a, b) == 0;
}

size_t lt_tables_sharing_size(const lt_ini *ini, const char *cols_const, const char *rows_const,
                              const lt_ini_table **out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; ini && i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        if (!t->rows_size_const || !*t->rows_size_const) continue;
        if (!str_eq(t->cols_size_const, cols_const) || !str_eq(t->rows_size_const, rows_const))
            continue;
        if (out && n < cap) out[n] = t;
        n++;
    }
    return n;
}
