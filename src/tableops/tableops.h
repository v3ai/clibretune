/* tableops.h - table math, table editor view model, dynamic table sizing, units.
 *
 * Spec: docs/spec/08-sim-basemap-tableops.md sections 1-3 and 2.11.
 *
 * Three layers:
 *   1. Pure math on flat row-major grids: z[y*cols + x], selections as arrays
 *      of lt_tcell (y = row = load axis, x = col = rpm axis). No INI knowledge.
 *   2. lt_tblview: a table (or curve) loaded from page images through the INI
 *      into doubles in DISPLAY units, with per-cell dirty tracking, undo, and
 *      commit back to the page image (via lt_pagesrc, which marks the bytes
 *      dirty in whoever owns them: ECU memory shadow or tune).
 *   3. Dynamic ("resizable") table size handling and unit conversion.
 *
 * Ops mutate in place; callers needing undo use lt_tblview_push_undo() first
 * (or copy the grid themselves). Out-of-range selections follow the "whole op
 * rejected / cell skipped" rules documented per function in the spec.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TABLEOPS_H
#define LT_TABLEOPS_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"

/* unit kinds (see the unit preference section at the end) */
#define LT_UK_NONE  0
#define LT_UK_TEMP  1     /* units: 0 C, 1 F, 2 K */
#define LT_UK_PRESS 2     /* units: 0 kPa, 1 psi, 2 bar, 3 inHg */
#define LT_UK_AFR   3     /* units: 0 AFR, 1 lambda */
#define LT_UK_SPEED 4     /* units: 0 km/h, 1 mph */
typedef struct lt_uconv { int kind, src, dst; } lt_uconv;

/* ---- selection ---- */
typedef struct lt_tcell { size_t y, x; } lt_tcell;   /* (row, col); not gui.h lt_cell */
/* Bounding box of a selection; returns LT_FALSE if n == 0. */
lt_bool lt_tbl_bbox(const lt_tcell *sel, size_t n, size_t *min_y, size_t *max_y,
                    size_t *min_x, size_t *max_x);
lt_bool lt_tbl_sel_has(const lt_tcell *sel, size_t n, size_t y, size_t x);

/* ---- operations (spec 08 section 2) ---- */
void lt_tbl_scale(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double f);
void lt_tbl_offset(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double off);
void lt_tbl_set_equal(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double v);
/* bilinear blend of the selection's bounding-box corners (needs >= 4 cells) */
void lt_tbl_interp2d(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n);
#define LT_TBL_AXIS_ROW 0   /* interpolate along rows (horizontal) */
#define LT_TBL_AXIS_COL 1
void lt_tbl_interp_linear(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                          int axis);
#define LT_TBL_FILL_RIGHT 0
#define LT_TBL_FILL_DOWN  1
void lt_tbl_fill(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, int dir);
/* 3x3 gaussian weighted average of the ORIGINAL grid (order independent). factor <= 0: no-op. */
void lt_tbl_smooth(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                   double factor);
/* Copy the selection's bounding box into out (bw*bh doubles, row-major, unselected
 * cells inside the box included); returns box size via bh/bw. */
void lt_tbl_copy(const double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                 double *out, size_t out_cap, size_t *bh, size_t *bw);
/* Paste a bh x bw block with its top-left at (y0,x0); cells falling outside are clipped. */
void lt_tbl_paste(double *z, size_t rows, size_t cols, size_t y0, size_t x0,
                  const double *src, size_t bh, size_t bw);

/* ---- editor operations (tableops_edit.c) ---- */
/* Copy the selected cells back from orig (same shape as z): "Esc = revert selection". */
void lt_tbl_restore(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n,
                    const double *orig);
/* Delete/Backspace: set the selection to 0 clamped into [lo,hi] (no clamp when hi <= lo). */
void lt_tbl_clear(double *z, size_t rows, size_t cols, const lt_tcell *sel, size_t n, double lo,
                  double hi);
#define LT_TBL_PASTE_REPLACE 0
#define LT_TBL_PASTE_ADD     1
#define LT_TBL_PASTE_SUB     2
#define LT_TBL_PASTE_MUL     3
#define LT_TBL_PASTE_MIN     4
#define LT_TBL_PASTE_MAX     5
/* Paste special: like lt_tbl_paste but combines each pasted value with the cell. */
void lt_tbl_paste_op(double *z, size_t rows, size_t cols, size_t y0, size_t x0,
                     const double *src, size_t bh, size_t bw, int mode);
/* Keep a dragged axis point x[i] between its neighbours (curve editor X drag). */
double lt_tbl_clamp_between(const double *x, size_t n, size_t i, double v);
/* New axes: nnx/nny points evenly spaced between the old end points; z bilinearly resampled.
 * out_x holds nnx, out_y nny, out_z nny*nnx doubles. */
int  lt_tbl_resize(const double *xb, size_t nx, const double *yb, size_t ny, const double *z,
                   size_t nnx, size_t nny, double *out_x, double *out_y, double *out_z);

/* Bilinear lookup of z (ny rows x nx cols) at (tx,ty), clamping at the edges. */
double lt_tbl_bilinear(double tx, double ty, const double *xb, size_t nx, const double *yb,
                       size_t ny, const double *z);
/* VE-model style lookup (spec 08 2.11); returns LT_FALSE if shapes are unusable. */
lt_bool lt_tbl_lookup(double x, double y, const double *xb, size_t nx, const double *yb,
                      size_t ny, const double *z, double *out);
/* Resample a table onto new axes. interp: bilinear on the old grid, else copy by
 * index (replicating the last row/col when growing). out_z holds nny*nnx doubles. */
int lt_tbl_rebin(const double *xb, size_t nx, const double *yb, size_t ny, const double *z,
                 const double *nxb, size_t nnx, const double *nyb, size_t nny, lt_bool interp,
                 double *out_z);
/* Index of the nearest axis value (first wins ties; 0 for an empty axis). */
size_t lt_tbl_nearest(const double *bins, size_t n, double v);
/* Axis helper: first + step*i for count points. */
void   lt_tbl_linspace(double first, double last, size_t count, double *out);

/* ====================================================================== */
/* Table / curve view model for the editor                                 */
/* ====================================================================== */
#define LT_TBLVIEW_UNDO_MAX 32

typedef struct lt_tblview {
    const lt_ini_table *table;    /* exactly one of table/curve is set */
    const lt_ini_curve *curve;
    size_t rows, cols;            /* active size (curve: rows = 1) */
    double *x;                    /* cols axis values (display units) */
    double *y;                    /* rows axis values (NULL for curves) */
    double *z;                    /* rows*cols cells; curve: z[0..cols-1] are the y values */
    /* last committed/loaded copies for diffing */
    double *x0, *y0, *z0;
    lt_bool x_dirty, y_dirty, z_dirty;
    /* unit conversion of the three arrays (native INI unit -> preferred display unit) */
    lt_uconv xu, yu, zu;
    /* undo ring of z snapshots (axes edits push both) */
    double *undo[LT_TBLVIEW_UNDO_MAX]; size_t undo_n, undo_pos;
} lt_tblview;

void lt_tblview_init(lt_tblview *v);
void lt_tblview_free(lt_tblview *v);
/* Read axes and cells from the page images. Uses dynamic active size when the
 * table is resizable. LT_ENOENT if a constant is missing or its page is not loaded. */
int  lt_tblview_load_table(lt_tblview *v, const lt_ini *ini, const lt_ini_table *t,
                           const lt_pagesrc *src);
int  lt_tblview_load_curve(lt_tblview *v, const lt_ini *ini, const lt_ini_curve *c,
                           const lt_pagesrc *src);
/* Write back only the cells/axis entries that changed since load/commit. The
 * pagesrc marks the touched bytes dirty. Returns number of elements written or <0. */
int  lt_tblview_commit(lt_tblview *v, const lt_ini *ini, lt_pagesrc *dst);
/* Discard edits: reload from the page image. */
int  lt_tblview_revert(lt_tblview *v, const lt_ini *ini, const lt_pagesrc *src);
double lt_tblview_get(const lt_tblview *v, size_t row, size_t col);
void   lt_tblview_set(lt_tblview *v, size_t row, size_t col, double value);   /* marks dirty */
lt_bool lt_tblview_cell_dirty(const lt_tblview *v, size_t row, size_t col);
lt_bool lt_tblview_is_dirty(const lt_tblview *v);
/* Value limits of the Z constant in display units (for clamping / colour scale). */
void   lt_tblview_z_range(const lt_tblview *v, const lt_ini *ini, double *lo, double *hi);
/* Edit an axis entry (marks x/y dirty; no undo push). */
void   lt_tblview_set_x(lt_tblview *v, size_t col, double value);
void   lt_tblview_set_y(lt_tblview *v, size_t row, double value);
/* Re-convert all arrays (and undo snapshots) after the unit preference changed; edits are
 * kept. Returns LT_TRUE when anything changed. Cheap no-op otherwise. */
lt_bool lt_tblview_units_sync(lt_tblview *v);
void   lt_tblview_push_undo(lt_tblview *v);     /* call before a mutating op */
lt_bool lt_tblview_undo(lt_tblview *v);
lt_bool lt_tblview_redo(lt_tblview *v);
/* Convenience: apply an op to the view's z after pushing undo (sel in view coords). */
void   lt_tblview_scale(lt_tblview *v, const lt_tcell *sel, size_t n, double f);
void   lt_tblview_offset(lt_tblview *v, const lt_tcell *sel, size_t n, double off);
void   lt_tblview_set_equal(lt_tblview *v, const lt_tcell *sel, size_t n, double val);
void   lt_tblview_interp2d(lt_tblview *v, const lt_tcell *sel, size_t n);
void   lt_tblview_interp_linear(lt_tblview *v, const lt_tcell *sel, size_t n, int axis);
void   lt_tblview_fill(lt_tblview *v, const lt_tcell *sel, size_t n, int dir);
void   lt_tblview_smooth(lt_tblview *v, const lt_tcell *sel, size_t n, double factor);
/* Load a TunerStudio .table grid (see tune.h lt_table_file) into the view; shape must match. */
int    lt_tblview_import(lt_tblview *v, size_t cols, size_t rows, const double *x,
                         const double *y, const double *z);

/* ====================================================================== */
/* Dynamic (resizable) tables (spec 08 section 3)                          */
/* ====================================================================== */
typedef struct lt_table_size {
    char cols_const[64], rows_const[64];
    size_t min_cols, max_cols, min_rows, max_rows, max_elements, active_cols, active_rows;
} lt_table_size;
/* 0 ok else LT_ERANGE with message ("Columns %d out of range", "%dx%d exceeds cell budget") */
int  lt_table_size_allows(const lt_table_size *s, size_t cols, size_t rows);
void lt_table_size_clamp_to_budget(lt_table_size *s);
/* Read the size constants from the pages; LT_ENOENT if the table is not resizable. */
int  lt_table_size_info(const lt_ini *ini, const lt_ini_table *t, const lt_pagesrc *src,
                        lt_table_size *out);
/* Change the active size by writing the two size constants (page marked dirty). */
int  lt_table_size_apply(const lt_ini *ini, const lt_ini_table *t, lt_pagesrc *dst,
                         size_t cols, size_t rows);
/* Other tables that use the same pair of size constants (they resize together). */
size_t lt_tables_sharing_size(const lt_ini *ini, const char *cols_const, const char *rows_const,
                              const lt_ini_table **out, size_t cap);
/* Resize t and every table sharing its size constants: resample axes/Z, write the size
 * constants and the new data into dst. Needs the table's size limits (LT_ENOENT if fixed). */
int  lt_table_resize(const lt_ini *ini, const lt_ini_table *t, lt_pagesrc *dst, size_t cols,
                     size_t rows);
/* Re-pack a row-major rows x cols grid into an allocation-sized flat buffer
 * with stride = active cols, preserving the unused tail (spec 08 3). */
void lt_dyn_pack(double *alloc, size_t alloc_n, const double *z, size_t rows, size_t cols);
void lt_dyn_unpack(const double *flat, size_t flat_n, size_t cols, size_t rows, double *z);
/* Resolve an active axis count from a stored size scalar (spec 08 3): below-min
 * (incl. 0) or absent uses the default (else min), above-max clamps to max. */
size_t lt_dyn_resolve_axis_count(lt_bool has_raw, double raw, size_t min, size_t max,
                                 lt_bool has_def, double def);
/* Allocated element count of a constant shape (>= 1). */
size_t lt_dyn_allocated_elements(const lt_shape *s);
/* Axis slice/pad to `active` points (pads with the last value; empty -> zeros). */
void lt_dyn_slice_bins(const double *bins, size_t n, size_t active, double *out);

/* ====================================================================== */
/* Unit conversion (spec 08 section 1)                                     */
/* ====================================================================== */
double lt_c_to_f(double c);        double lt_f_to_c(double f);
double lt_kpa_to_psi(double kpa);  double lt_psi_to_kpa(double psi);
double lt_bar_to_psi(double bar);  double lt_psi_to_bar(double psi);
double lt_kmh_to_mph(double kmh);  double lt_mph_to_kmh(double mph);
double lt_l_to_gal_us(double l);   double lt_gal_us_to_l(double g);
double lt_l_to_gal_imp(double l);  double lt_gal_imp_to_l(double g);
double lt_l_to_gal(double l, lt_bool imperial);
double lt_gal_to_l(double g, lt_bool imperial);
double lt_lbs_to_kg(double lbs);   double lt_kg_to_lbs(double kg);
/* fuel: "gasoline","e85","methanol","ethanol","lpg","cng","diesel" (case-insens; else 14.7) */
double lt_fuel_stoich(const char *fuel);
double lt_lambda_to_afr(double lambda, const char *fuel);
double lt_afr_to_lambda(double afr, const char *fuel);

/* ---- unit preferences applied to displayed values (G-90) ----
 * ECU / INI values keep their native unit (detected from the INI unit string); everything
 * the user sees or types is converted to the preferred unit. Preferences are process-wide
 * (set from Settings); lt_units_generation() changes whenever they do. */

/* temp 0..2, press 0..3, afr 0..1, metric (speed: 1 km/h, 0 mph); out of range clamps. */
void lt_units_set(int temp, int press, int afr, int metric);
void lt_units_get(int *temp, int *press, int *afr, int *metric);
unsigned lt_units_generation(void);
/* Detect kind and native unit from an INI unit string ("C", "deg C", "kPa", "AFR", "lambda",
 * "km/h" ...). Returns LT_UK_NONE for anything else, including "{expr}" units. */
int  lt_unit_detect(const char *units, int *src);
/* Value conversion between two units of one kind (via a common base unit). */
double lt_unit_convert(int kind, int from, int to, double v);
const char *lt_unit_name(int kind, int unit);
/* Converter from the native unit string to the current preference. */
void   lt_uconv_make(lt_uconv *c, const char *units);
lt_bool lt_uconv_active(const lt_uconv *c);          /* converts something */
lt_bool lt_uconv_stale(const lt_uconv *c);           /* preference changed since make/retarget */
double lt_uconv_to_disp(const lt_uconv *c, double ecu);
double lt_uconv_from_disp(const lt_uconv *c, double disp);
/* Label to show: the preferred unit name, or `units` itself when nothing is converted. */
const char *lt_uconv_label(const lt_uconv *c, const char *units);
/* Re-aim at the current preference; returns `disp` (shown in the old unit) in the new one. */
double lt_uconv_retarget(lt_uconv *c, double disp);
/* Decimal places to show/round to for a value whose native unit had `digits`: more when the
 * display unit is smaller per step (kPa -> psi/bar, AFR -> lambda). */
int    lt_uconv_digits(const lt_uconv *c, int digits);
/* Convert a (lo,hi) range to display units, keeping lo <= hi. */
void   lt_uconv_range(const lt_uconv *c, double *lo, double *hi);

#endif /* LT_TABLEOPS_H */
