/* tablecmp.h - cell-by-cell comparison of two equally sized tables (G-80, no UI).
 * Mirrors the original's compare_tables command: a cell differs when |b - a| > 0.0001, the
 * percent difference is relative to a (100 when a is ~0), summary = count, max and mean |diff|.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TABLECMP_H
#define LT_TABLECMP_H

#include "../lt.h"
#include "../lt_codes.h"

LT_BEGIN_DECLS

#define LT_TBC_EPS 0.0001
#define LT_TBC_NONE 0     /* severity buckets of a percent difference */
#define LT_TBC_LOW  1     /* < 5 % */
#define LT_TBC_MED  2     /* 5 .. 10 % */
#define LT_TBC_HIGH 3     /* >= 10 % */

typedef struct lt_tbc_cell {
    size_t row, col;
    double a, b, diff, pct;
} lt_tbc_cell;

typedef struct lt_tblcmp {
    size_t rows, cols;
    double *a, *b;            /* copies of the compared grids, rows*cols each */
    lt_tbc_cell *cells;       /* the differing cells, row-major; n of them */
    size_t n;
    double max_diff, avg_diff;
    size_t n_low, n_med, n_high;
    double lim_diff, lim_pct, lo_b, hi_b;   /* heat scale: symmetric limits and the range of b */
} lt_tblcmp;

/* percent difference of b relative to a (100 when a is ~0 and they differ, else 0) */
double lt_tbc_percent(double a, double b);
int    lt_tbc_severity(double pct);
/* 0 or LT_EINVAL (a size is 0 or an overflow); out is zeroed first, free it with lt_tblcmp_free */
int  lt_tblcmp_compute(const double *a, const double *b, size_t rows, size_t cols, lt_tblcmp *out);
void lt_tblcmp_free(lt_tblcmp *c);
/* the three views of the result grid: 0 difference (b - a), 1 percent, 2 values of b */
#define LT_TBC_VIEW_DIFF 0
#define LT_TBC_VIEW_PCT  1
#define LT_TBC_VIEW_B    2
double lt_tblcmp_value(const lt_tblcmp *c, int view, size_t row, size_t col);
/* heat position 0..1 of a cell for the view (0.5 = no change; diff/pct are symmetric around it,
 * B maps its own min..max); 0.5 for an empty or flat grid */
double lt_tblcmp_heat(const lt_tblcmp *c, int view, size_t row, size_t col);

LT_END_DECLS

#endif
