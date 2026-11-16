/* tableops_ops.c - grid math, lookups, dynamic-size helpers, unit conversion.
 * Spec: docs/spec/08-sim-basemap-tableops.md sections 1-3.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <float.h>
#include <math.h>
#include <string.h>
#include "tableops.h"

/* ---------------------------------------------------------------- selection */

lt_bool lt_tbl_bbox(const lt_tcell *sel, size_t n, size_t *min_y, size_t *max_y,
                    size_t *min_x, size_t *max_x)
{
    size_t i, y0, y1, x0, x1;
    if (!sel || n == 0) return LT_FALSE;
    y0 = y1 = sel[0].y;
    x0 = x1 = sel[0].x;
    for (i = 1; i < n; i++) {
        if (sel[i].y < y0) y0 = sel[i].y;
        if (sel[i].y > y1) y1 = sel[i].y;
        if (sel[i].x < x0) x0 = sel[i].x;
        if (sel[i].x > x1) x1 = sel[i].x;
    }
    if (min_y) *min_y = y0;
    if (max_y) *max_y = y1;
    if (min_x) *min_x = x0;
    if (max_x) *max_x = x1;
    return LT_TRUE;
}

lt_bool lt_tbl_sel_has(const lt_tcell *sel, size_t n, size_t y, size_t x)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (sel[i].y == y && sel[i].x == x) return LT_TRUE;
    return LT_FALSE;
}

/* ---------------------------------------------------------------- simple ops */

void lt_tbl_scale(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double f)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (sel[i].y < rows && sel[i].x < cols) z[sel[i].y * cols + sel[i].x] *= f;
}

void lt_tbl_offset(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double off)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (sel[i].y < rows && sel[i].x < cols) z[sel[i].y * cols + sel[i].x] += off;
}

void lt_tbl_set_equal(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double v)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (sel[i].y < rows && sel[i].x < cols) z[sel[i].y * cols + sel[i].x] = v;
}

void lt_tbl_interp2d(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n)
{
    size_t miny, maxy, minx, maxx, y, x;
    double tl, tr, bl, br, yspan, xspan, yr, xr;
    if (n < 4 || !lt_tbl_bbox(sel, n, &miny, &maxy, &minx, &maxx)) return;
    if (maxy >= rows || maxx >= cols) return;
    tl = z[miny * cols + minx];
    tr = z[miny * cols + maxx];
    bl = z[maxy * cols + minx];
    br = z[maxy * cols + maxx];
    yspan = (double)(maxy - miny);
    xspan = (double)(maxx - minx);
    for (y = miny; y <= maxy; y++) {
        yr = yspan > 0.0 ? (double)(y - miny) / yspan : 0.0;
        for (x = minx; x <= maxx; x++) {
            xr = xspan > 0.0 ? (double)(x - minx) / xspan : 0.0;
            z[y * cols + x] = tl * (1.0 - yr) * (1.0 - xr) + tr * (1.0 - yr) * xr +
                              bl * yr * (1.0 - xr) + br * yr * xr;
        }
    }
}

void lt_tbl_interp_linear(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                          int axis)
{
    size_t miny, maxy, minx, maxx, y, x;
    double start, end, span;
    if (!lt_tbl_bbox(sel, n, &miny, &maxy, &minx, &maxx)) return;
    if (maxy >= rows || maxx >= cols) return;
    if (axis == LT_TBL_AXIS_ROW) {
        span = (double)(maxx - minx);
        if (span <= 0.0) return;
        for (y = miny; y <= maxy; y++) {
            start = z[y * cols + minx];
            end = z[y * cols + maxx];
            for (x = minx; x <= maxx; x++)
                if (lt_tbl_sel_has(sel, n, y, x))
                    z[y * cols + x] = start + (end - start) * (double)(x - minx) / span;
        }
    } else {
        span = (double)(maxy - miny);
        if (span <= 0.0) return;
        for (x = minx; x <= maxx; x++) {
            start = z[miny * cols + x];
            end = z[maxy * cols + x];
            for (y = miny; y <= maxy; y++)
                if (lt_tbl_sel_has(sel, n, y, x))
                    z[y * cols + x] = start + (end - start) * (double)(y - miny) / span;
        }
    }
}

void lt_tbl_fill(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, int dir)
{
    size_t miny, maxy, minx, maxx, y, x;
    double src;
    if (!lt_tbl_bbox(sel, n, &miny, &maxy, &minx, &maxx)) return;
    if (maxy >= rows || maxx >= cols) return;
    if (dir == LT_TBL_FILL_RIGHT) {
        for (y = miny; y <= maxy; y++) {
            src = z[y * cols + minx];
            for (x = minx; x <= maxx; x++)
                if (lt_tbl_sel_has(sel, n, y, x)) z[y * cols + x] = src;
        }
    } else {
        for (x = minx; x <= maxx; x++) {
            src = z[miny * cols + x];
            for (y = miny; y <= maxy; y++)
                if (lt_tbl_sel_has(sel, n, y, x)) z[y * cols + x] = src;
        }
    }
}

void lt_tbl_smooth(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                   double factor)
{
    double *orig, two_sigma_sq, sum, wsum, w;
    size_t i, y, x;
    int dy, dx;
    if (n == 0 || rows == 0 || cols == 0 || !(factor > 0.0)) return;
    for (i = 0; i < n; i++)
        if (sel[i].y >= rows || sel[i].x >= cols) return;
    orig = (double *)lt_malloc(rows * cols * sizeof(double));
    memcpy(orig, z, rows * cols * sizeof(double));
    two_sigma_sq = 2.0 * factor * factor;
    for (i = 0; i < n; i++) {
        y = sel[i].y;
        x = sel[i].x;
        sum = 0.0;
        wsum = 0.0;
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                if ((dy < 0 && y == 0) || (dy > 0 && y + 1 >= rows)) continue;
                if ((dx < 0 && x == 0) || (dx > 0 && x + 1 >= cols)) continue;
                w = exp(-(double)(dy * dy + dx * dx) / two_sigma_sq);
                sum += orig[(size_t)((long)y + dy) * cols + (size_t)((long)x + dx)] * w;
                wsum += w;
            }
        }
        if (wsum > 0.0) z[y * cols + x] = sum / wsum;
    }
    lt_free(orig);
}

void lt_tbl_copy(const double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                 double *out, size_t out_cap, size_t *bh, size_t *bw)
{
    size_t miny, maxy, minx, maxx, y, x, h, w;
    if (bh) *bh = 0;
    if (bw) *bw = 0;
    if (!lt_tbl_bbox(sel, n, &miny, &maxy, &minx, &maxx)) return;
    if (miny >= rows || minx >= cols) return;
    if (maxy >= rows) maxy = rows - 1;
    if (maxx >= cols) maxx = cols - 1;
    h = maxy - miny + 1;
    w = maxx - minx + 1;
    if (h * w > out_cap) return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) out[y * w + x] = z[(miny + y) * cols + minx + x];
    if (bh) *bh = h;
    if (bw) *bw = w;
}

void lt_tbl_paste(double *z, size_t rows, size_t cols, size_t y0, size_t x0,
                  const double *src, size_t bh, size_t bw)
{
    size_t y, x;
    for (y = 0; y < bh && y0 + y < rows; y++)
        for (x = 0; x < bw && x0 + x < cols; x++) z[(y0 + y) * cols + x0 + x] = src[y * bw + x];
}

/* ------------------------------------------------------------------ lookups */

/* spec 2.9 find_surrounding */
static void find_surrounding(double v, const double *b, size_t n, size_t *i0, size_t *i1,
                             double *f)
{
    size_t i, last;
    double span;
    *i0 = *i1 = 0;
    *f = 0.0;
    if (n == 0 || v <= b[0]) return;
    last = n - 1;
    if (v >= b[last]) {
        *i0 = *i1 = last;
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        if (b[i] <= v && v <= b[i + 1]) {
            span = b[i + 1] - b[i];
            *i0 = i;
            *i1 = i + 1;
            *f = fabs(span) < DBL_EPSILON ? 0.0 : (v - b[i]) / span;
            return;
        }
    }
    *i0 = *i1 = last;
}

double lt_tbl_bilinear(double tx, double ty, const double *xb, size_t nx, const double *yb,
                       size_t ny, const double *z)
{
    size_t x0, x1, y0, y1;
    double fx, fy, v00, v10, v01, v11, top, bot;
    find_surrounding(tx, xb, nx, &x0, &x1, &fx);
    find_surrounding(ty, yb, ny, &y0, &y1, &fy);
    if (nx == 0 || ny == 0) return 0.0;
    v00 = z[y0 * nx + x0];
    v10 = z[y0 * nx + x1];
    v01 = z[y1 * nx + x0];
    v11 = z[y1 * nx + x1];
    top = v00 + (v10 - v00) * fx;
    bot = v01 + (v11 - v01) * fx;
    return top + (bot - top) * fy;
}

/* spec 2.11 segment */
static void segment(const double *b, size_t n, double v, size_t *lo, double *frac)
{
    size_t i, last = n - 1;
    double span;
    *lo = 0;
    *frac = 0.0;
    if (last == 0) return;
    if (v <= b[0]) return;
    if (v >= b[last]) {
        *lo = last - 1;
        *frac = 1.0;
        return;
    }
    for (i = 0; i < last; i++) {
        if (b[i] <= v && v <= b[i + 1]) {
            span = b[i + 1] - b[i];
            *lo = i;
            *frac = span > 0.0 ? (v - b[i]) / span : 0.0;
            return;
        }
    }
    *lo = last - 1;
    *frac = 1.0;
}

lt_bool lt_tbl_lookup(double x, double y, const double *xb, size_t nx, const double *yb,
                      size_t ny, const double *z, double *out)
{
    size_t x0, y0, x1, y1;
    double fx, fy, top, bot;
    if (!xb || !yb || !z || nx == 0 || ny == 0) return LT_FALSE;
    segment(xb, nx, x, &x0, &fx);
    segment(yb, ny, y, &y0, &fy);
    x1 = LT_MIN(x0 + 1, nx - 1);
    y1 = LT_MIN(y0 + 1, ny - 1);
    top = z[y0 * nx + x0] * (1.0 - fx) + z[y0 * nx + x1] * fx;
    bot = z[y1 * nx + x0] * (1.0 - fx) + z[y1 * nx + x1] * fx;
    if (out) *out = top * (1.0 - fy) + bot * fy;
    return LT_TRUE;
}

int lt_tbl_rebin(const double *xb, size_t nx, const double *yb, size_t ny, const double *z,
                 const double *nxb, size_t nnx, const double *nyb, size_t nny, lt_bool interp,
                 double *out_z)
{
    size_t y, x, oy, ox;
    if (!out_z || (nnx && !nxb) || (nny && !nyb)) return LT_EINVAL;
    for (y = 0; y < nny * nnx; y++) out_z[y] = 0.0;
    if (nx == 0 || ny == 0 || !z) return 0;
    if (interp) {
        if (!xb || !yb) return LT_EINVAL;
        for (y = 0; y < nny; y++)
            for (x = 0; x < nnx; x++)
                out_z[y * nnx + x] = lt_tbl_bilinear(nxb[x], nyb[y], xb, nx, yb, ny, z);
    } else {
        for (y = 0; y < nny; y++) {
            oy = LT_MIN(y, ny - 1);
            for (x = 0; x < nnx; x++) {
                ox = LT_MIN(x, nx - 1);
                out_z[y * nnx + x] = z[oy * nx + ox];
            }
        }
    }
    return 0;
}

size_t lt_tbl_nearest(const double *bins, size_t n, double v)
{
    size_t i, best = 0;
    double d, bd;
    if (n == 0) return 0;
    bd = fabs(bins[0] - v);
    for (i = 1; i < n; i++) {
        d = fabs(bins[i] - v);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

void lt_tbl_linspace(double first, double last, size_t count, double *out)
{
    size_t i;
    double step;
    if (count == 0) return;
    if (count == 1) {
        out[0] = first;
        return;
    }
    step = (last - first) / (double)(count - 1);
    for (i = 0; i < count; i++) out[i] = first + step * (double)i;
}

/* ------------------------------------------------------------ dynamic tables */

static size_t sat_mul(size_t a, size_t b)
{
    if (a != 0 && b > ((size_t)-1) / a) return (size_t)-1;
    return a * b;
}

int lt_table_size_allows(const lt_table_size *s, size_t cols, size_t rows)
{
    if (cols < s->min_cols || cols > s->max_cols) {
        lt_err_set("Columns %lu out of range %lu..%lu", (unsigned long)cols,
                   (unsigned long)s->min_cols, (unsigned long)s->max_cols);
        return LT_ERANGE;
    }
    if (rows < s->min_rows || rows > s->max_rows) {
        lt_err_set("Rows %lu out of range %lu..%lu", (unsigned long)rows,
                   (unsigned long)s->min_rows, (unsigned long)s->max_rows);
        return LT_ERANGE;
    }
    if (sat_mul(cols, rows) > s->max_elements) {
        lt_err_set("%lux%lu exceeds cell budget %lu", (unsigned long)rows,
                   (unsigned long)cols, (unsigned long)s->max_elements);
        return LT_ERANGE;
    }
    return 0;
}

void lt_table_size_clamp_to_budget(lt_table_size *s)
{
    while (sat_mul(s->active_cols, s->active_rows) > s->max_elements &&
           s->active_cols > s->min_cols)
        s->active_cols--;
    while (sat_mul(s->active_cols, s->active_rows) > s->max_elements &&
           s->active_rows > s->min_rows)
        s->active_rows--;
}

size_t lt_dyn_resolve_axis_count(lt_bool has_raw, double raw, size_t min, size_t max,
                                 lt_bool has_def, double def)
{
    double fb = (has_def && lt_isfinite(def)) ? lt_round(def) : (double)min;
    double n;
    if (max < min) max = min;
    if (fb < (double)min) fb = (double)min;
    if (fb > (double)max) fb = (double)max;
    if (!has_raw || raw != raw) return (size_t)fb;
    if (raw > (double)max) return max;   /* includes +inf, like a saturating cast */
    n = lt_round(raw);
    if (n < (double)min) return (size_t)fb;
    if (n > (double)max) return max;
    return (size_t)n;
}

size_t lt_dyn_allocated_elements(const lt_shape *s)
{
    size_t n = lt_shape_count(s);
    return n < 1 ? 1 : n;
}

void lt_dyn_pack(double *alloc, size_t alloc_n, const double *z, size_t rows, size_t cols)
{
    size_t y, x, k;
    for (y = 0; y < rows; y++)
        for (x = 0; x < cols; x++) {
            k = y * cols + x;
            if (k < alloc_n) alloc[k] = z[k];
        }
}

void lt_dyn_unpack(const double *flat, size_t flat_n, size_t cols, size_t rows, double *z)
{
    size_t k, n = rows * cols;
    for (k = 0; k < n; k++) z[k] = k < flat_n ? flat[k] : 0.0;
}

void lt_dyn_slice_bins(const double *bins, size_t n, size_t active, double *out)
{
    size_t i;
    if (n == 0) {
        for (i = 0; i < (active < 1 ? 1 : active); i++) out[i] = 0.0;
        return;
    }
    for (i = 0; i < active; i++) out[i] = i < n ? bins[i] : bins[n - 1];
}

/* ---------------------------------------------------------- unit conversion */

#define K_PSI   0.14503773773020923
#define K_BAR   14.503773773020923
#define K_MPH   0.62137119223733
#define K_GALUS 0.26417205235815
#define K_GALIM 0.21996924829909
#define K_LBS   0.45359237

double lt_c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }
double lt_f_to_c(double f) { return (f - 32.0) * 5.0 / 9.0; }
double lt_kpa_to_psi(double kpa) { return kpa * K_PSI; }
double lt_psi_to_kpa(double psi) { return psi / K_PSI; }
double lt_bar_to_psi(double bar) { return bar * K_BAR; }
double lt_psi_to_bar(double psi) { return psi / K_BAR; }
double lt_kmh_to_mph(double kmh) { return kmh * K_MPH; }
double lt_mph_to_kmh(double mph) { return mph / K_MPH; }
double lt_l_to_gal_us(double l) { return l * K_GALUS; }
double lt_gal_us_to_l(double g) { return g / K_GALUS; }
double lt_l_to_gal_imp(double l) { return l * K_GALIM; }
double lt_gal_imp_to_l(double g) { return g / K_GALIM; }
double lt_l_to_gal(double l, lt_bool imperial)
{
    return imperial ? lt_l_to_gal_imp(l) : lt_l_to_gal_us(l);
}
double lt_gal_to_l(double g, lt_bool imperial)
{
    return imperial ? lt_gal_imp_to_l(g) : lt_gal_us_to_l(g);
}
double lt_lbs_to_kg(double lbs) { return lbs * K_LBS; }
double lt_kg_to_lbs(double kg) { return kg / K_LBS; }

/* NOTE: deliberately separate from basemap's fuel table (methanol 6.5, LPG 15.7). */
double lt_fuel_stoich(const char *fuel)
{
    static const struct { const char *name; double s; } tab[] = {
        { "gasoline", 14.7 }, { "petrol", 14.7 }, { "e85", 9.8 }, { "methanol", 6.4 },
        { "ethanol", 9.0 }, { "lpg", 15.5 }, { "propane", 15.5 }, { "cng", 17.2 },
        { "natural_gas", 17.2 }, { "diesel", 14.5 }
    };
    size_t i;
    if (!fuel) return 14.7;
    for (i = 0; i < LT_COUNTOF(tab); i++)
        if (lt_stricmp(fuel, tab[i].name) == 0) return tab[i].s;
    return 14.7;
}

double lt_lambda_to_afr(double lambda, const char *fuel) { return lambda * lt_fuel_stoich(fuel); }
double lt_afr_to_lambda(double afr, const char *fuel) { return afr / lt_fuel_stoich(fuel); }
