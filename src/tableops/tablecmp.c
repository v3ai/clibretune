/* tablecmp.c - table comparison (see tablecmp.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "tablecmp.h"

double lt_tbc_percent(double a, double b)
{
    double d = b - a;
    if (fabs(a) > LT_TBC_EPS) { return d / a * 100.0; }
    return fabs(d) > LT_TBC_EPS ? 100.0 : 0.0;
}

int lt_tbc_severity(double pct)
{
    double p = fabs(pct);
    if (p >= 10.0) { return LT_TBC_HIGH; }
    if (p >= 5.0) { return LT_TBC_MED; }
    return p > 0.0 ? LT_TBC_LOW : LT_TBC_NONE;
}

int lt_tblcmp_compute(const double *a, const double *b, size_t rows, size_t cols, lt_tblcmp *out)
{
    size_t n, i;
    double sum = 0.0;
    memset(out, 0, sizeof(*out));
    if (a == NULL || b == NULL || rows == 0 || cols == 0) { lt_err_set("Nothing to compare"); return LT_EINVAL; }
    if (!lt_mul_size(rows, cols, &n)) { lt_err_set("Table too large"); return LT_EINVAL; }
    out->rows = rows; out->cols = cols;
    out->a = (double *)lt_malloc(n * sizeof(double));
    out->b = (double *)lt_malloc(n * sizeof(double));
    memcpy(out->a, a, n * sizeof(double));
    memcpy(out->b, b, n * sizeof(double));
    out->cells = (lt_tbc_cell *)lt_calloc(n, sizeof(lt_tbc_cell));
    for (i = 0; i < n; i++) {
        double d = b[i] - a[i];
        if (fabs(d) > LT_TBC_EPS) {
            lt_tbc_cell *c = &out->cells[out->n++];
            int sev;
            c->row = i / cols; c->col = i % cols;
            c->a = a[i]; c->b = b[i]; c->diff = d; c->pct = lt_tbc_percent(a[i], b[i]);
            if (fabs(d) > out->max_diff) { out->max_diff = fabs(d); }
            sum += fabs(d);
            sev = lt_tbc_severity(c->pct);
            if (sev == LT_TBC_HIGH) { out->n_high++; }
            else if (sev == LT_TBC_MED) { out->n_med++; }
            else { out->n_low++; }
        }
    }
    out->avg_diff = out->n > 0 ? sum / (double)out->n : 0.0;
    out->lo_b = 1e300; out->hi_b = -1e300;
    for (i = 0; i < n; i++) {
        if (b[i] < out->lo_b) { out->lo_b = b[i]; }
        if (b[i] > out->hi_b) { out->hi_b = b[i]; }
    }
    out->lim_diff = out->max_diff;
    for (i = 0; i < out->n; i++) {
        if (fabs(out->cells[i].pct) > out->lim_pct) { out->lim_pct = fabs(out->cells[i].pct); }
    }
    return 0;
}

void lt_tblcmp_free(lt_tblcmp *c)
{
    if (c == NULL) { return; }
    lt_free(c->a); lt_free(c->b); lt_free(c->cells);
    memset(c, 0, sizeof(*c));
}

double lt_tblcmp_value(const lt_tblcmp *c, int view, size_t row, size_t col)
{
    size_t i;
    if (c == NULL || c->a == NULL || row >= c->rows || col >= c->cols) { return 0.0; }
    i = row * c->cols + col;
    if (view == LT_TBC_VIEW_B) { return c->b[i]; }
    if (view == LT_TBC_VIEW_PCT) { return fabs(c->b[i] - c->a[i]) > LT_TBC_EPS ? lt_tbc_percent(c->a[i], c->b[i]) : 0.0; }
    return c->b[i] - c->a[i];
}

double lt_tblcmp_heat(const lt_tblcmp *c, int view, size_t row, size_t col)
{
    double v, lim;
    if (c == NULL || c->a == NULL) { return 0.5; }
    v = lt_tblcmp_value(c, view, row, col);
    if (view == LT_TBC_VIEW_B) {
        return c->hi_b > c->lo_b ? (v - c->lo_b) / (c->hi_b - c->lo_b) : 0.5;
    }
    lim = view == LT_TBC_VIEW_PCT ? c->lim_pct : c->lim_diff;
    return lim > 0.0 ? 0.5 + 0.5 * v / lim : 0.5;
}
