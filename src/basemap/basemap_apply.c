/* basemap_apply.c - write a generated base map into page images via the INI.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <float.h>
#include <string.h>
#include "basemap.h"

static void surround(double v, const double *b, size_t n, size_t *i0, size_t *i1, double *f)
{
    size_t i;

    *i0 = *i1 = 0;
    *f = 0.0;
    if (n == 0 || v <= b[0]) {
        return;
    }
    if (v >= b[n - 1]) {
        *i0 = *i1 = n - 1;
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        if (b[i] <= v && v <= b[i + 1]) {
            double span = b[i + 1] - b[i];
            *i0 = i;
            *i1 = i + 1;
            *f = fabs(span) < DBL_EPSILON ? 0.0 : (v - b[i]) / span;
            return;
        }
    }
    *i0 = *i1 = n - 1;
}

/* bilinear resample of src (ny rows x nx cols) onto new axes (spec 08 2.9) */
static void resample(const double *xb, size_t nx, const double *yb, size_t ny, const double *z,
                     const double *nxb, size_t nnx, const double *nyb, size_t nny, double *out)
{
    size_t r, c;

    for (r = 0; r < nny; r++) {
        for (c = 0; c < nnx; c++) {
            size_t x0, x1, y0, y1;
            double fx, fy, top, bot;
            surround(nxb[c], xb, nx, &x0, &x1, &fx);
            surround(nyb[r], yb, ny, &y0, &y1, &fy);
            top = z[y0 * nx + x0] + (z[y0 * nx + x1] - z[y0 * nx + x0]) * fx;
            bot = z[y1 * nx + x0] + (z[y1 * nx + x1] - z[y1 * nx + x0]) * fx;
            out[r * nnx + c] = top + (bot - top) * fy;
        }
    }
}

static void log_add(char **log, const char *line)
{
    size_t old, add;

    if (!log) {
        return;
    }
    old = *log ? strlen(*log) : 0;
    add = strlen(line);
    *log = (char *)lt_realloc(*log, old + add + 1);
    memcpy(*log + old, line, add + 1);
}

static int apply_table(const lt_basemap *m, const lt_ini *ini, lt_pagesrc *dst,
                       const lt_ini_table *t, lt_bool write_axes, char **log)
{
    const double (*src)[LT_BASEMAP_N];
    const lt_ini_const *zc, *xc, *yc;
    double *z, *nxb, *nyb, *out;
    size_t nx, ny, r, c;
    char line[96];
    int written = 0;

    if (t->role == LT_ROLE_VE) {
        src = m->ve;
    } else if (t->role == LT_ROLE_IGNITION) {
        src = m->ign;
    } else if (t->role == LT_ROLE_AFR_TARGET) {
        src = m->afr;
    } else {
        return 0;
    }
    if (!t->z_const || !t->x_const || !t->y_const) {
        return 0;
    }
    zc = lt_ini_find_const(ini, t->z_const);
    xc = lt_ini_find_const(ini, t->x_const);
    yc = lt_ini_find_const(ini, t->y_const);
    if (!zc || !xc || !yc) {
        return 0;
    }
    nx = lt_shape_count(&xc->shape);
    ny = lt_shape_count(&yc->shape);
    if (nx == 0 || ny == 0 || lt_shape_count(&zc->shape) != nx * ny) {
        return 0;
    }
    if (zc->shape.kind == LT_SH_2D && (zc->shape.cols != nx || zc->shape.rows != ny)) {
        return 0;   /* layout differs from the axes: refuse rather than scramble */
    }
    nxb = (double *)lt_calloc(nx + ny, sizeof(double));
    nyb = nxb + nx;
    if (write_axes) {
        double *tmp = (double *)lt_calloc(nx > ny ? nx : ny, sizeof(double));
        lt_basemap_rpm_bins((unsigned)(m->rpm_bins[0] + 0.5),
                            (unsigned)(m->rpm_bins[LT_BASEMAP_N - 1] + 0.5), nx, tmp);
        memcpy(nxb, tmp, nx * sizeof(double));
        lt_basemap_load_bins(m->load_bins[LT_BASEMAP_N - 1], ny, tmp);
        memcpy(nyb, tmp, ny * sizeof(double));
        lt_free(tmp);
    } else if (lt_ini_const_get_array(ini, xc, dst, nxb, nx) < 0 ||
               lt_ini_const_get_array(ini, yc, dst, nyb, ny) < 0) {
        lt_free(nxb);
        return LT_EDATA;
    }
    z = (double *)lt_malloc(LT_BASEMAP_N * LT_BASEMAP_N * sizeof(double));
    for (r = 0; r < LT_BASEMAP_N; r++) {
        for (c = 0; c < LT_BASEMAP_N; c++) {
            z[r * LT_BASEMAP_N + c] = src[r][c];
        }
    }
    out = (double *)lt_malloc(nx * ny * sizeof(double));
    resample(m->rpm_bins, LT_BASEMAP_N, m->load_bins, LT_BASEMAP_N, z, nxb, nx, nyb, ny, out);
    if (write_axes) {
        if (lt_ini_const_set_array(ini, xc, dst, nxb, nx) >= 0 &&
            lt_ini_const_set_array(ini, yc, dst, nyb, ny) >= 0) {
            written += 2;
        }
    }
    if (lt_ini_const_set_array(ini, zc, dst, out, nx * ny) >= 0) {
        written++;
        lt_snprintf(line, sizeof(line), "table %s: %lu cells\n", t->name, (unsigned long)(nx * ny));
        log_add(log, line);
    }
    lt_free(z);
    lt_free(out);
    lt_free(nxb);
    return written;
}

int lt_basemap_apply(const lt_basemap *m, const lt_ini *ini, lt_pagesrc *dst, lt_bool write_axes,
                     char **log)
{
    int total = 0, seen[5] = { 0, 0, 0, 0, 0 };
    size_t i;
    char line[96];

    if (log) {
        *log = NULL;
    }
    if (!m || !ini || !dst) {
        lt_err_set("basemap_apply: bad argument");
        return LT_EINVAL;
    }
    for (i = 0; i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        int n;
        if (t->role == LT_ROLE_OTHER || t->role == LT_ROLE_WARMUP || seen[t->role]) {
            continue;
        }
        n = apply_table(m, ini, dst, t, write_axes, log);
        if (n < 0) {
            return n;
        }
        if (n > 0) {
            seen[t->role] = 1;
            total += n;
        }
    }
    for (i = 0; i < m->n_scalars; i++) {
        const lt_ini_const *c = lt_ini_find_const(ini, m->scalars[i].name);
        if (c && c->shape.kind == LT_SH_SCALAR &&
            lt_ini_set(ini, dst, m->scalars[i].name, m->scalars[i].value) >= 0) {
            total++;
            lt_snprintf(line, sizeof(line), "const %s = %g\n", m->scalars[i].name,
                        m->scalars[i].value);
            log_add(log, line);
        }
    }
    return total;
}
