/* tables_priv.h - helpers shared by tables_*.c (table, curve and AutoTune screens).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TABLES_PRIV_H
#define LT_TABLES_PRIV_H

#include "app.h"
#include "../tableops/tableops.h"
#include "../autotune/autotune.h"
#include "../basemap/basemap.h"

/* fixed-point format with `digits` decimals (clamped 0..6) */
void lt_tabs_fmt(char *buf, size_t cap, double v, int digits);
/* heat colour of v within [lo,hi] using the configured scheme, blended with the table bg */
lt_color lt_tabs_heat(const lt_app *app, const lt_theme *t, double lo, double hi, double v);
/* readable ink (near-black or white) for text on a heat-map cell colour */
lt_color lt_tabs_heat_text(lt_color bg);
/* number of decimals of an INI constant (default 0) and its value limits */
int  lt_tabs_const_digits(const lt_ini *ini, const char *name);
/* round to `digits` decimals */
double lt_tabs_round(double v, int digits);
/* key helper: ch pressed with or without shift, no other modifiers */
int  lt_tabs_key(lt_ui *ui, int ch);
/* parse "1, 2.5 3" into out (cap entries); returns count or -1 on a bad token */
int  lt_tabs_parse_list(const char *s, double *out, int cap);
/* nearest-index of the live channel value in bins; LT_FALSE when the channel is unavailable */
lt_bool lt_tabs_live_index(lt_app *app, const char *channel, const double *bins, size_t n,
                           double *value, size_t *idx);


/* ---- one edit operation on a table or curve: limits, rounding, no-op detection ----
 * The operation computes new values into a scratch copy; lt_tabs_apply writes back only what
 * really changes (an operation that changes nothing adds no undo step and marks nothing
 * modified), clamping each changed value into the constant's limits and counting the clamped
 * ones. */
typedef struct lt_tabs_lim {
    double lo, hi;              /* allowed range in display units (hi <= lo: none) */
    int digits;                 /* values are rounded to this many decimals */
    int n;                      /* values limited so far */
    double last;                /* what the last limited value became */
} lt_tabs_lim;
void   lt_tabs_lim_init(lt_tabs_lim *l, double lo, double hi, int digits);
/* clamp v into the range (counted when it was outside) and round it */
double lt_tabs_lim_fix(lt_tabs_lim *l, double v);
#define LT_TABS_Z 0             /* lt_tabs_apply targets */
#define LT_TABS_X 1
/* Write nv (rows*cols values for LT_TABS_Z, cols for LT_TABS_X) into the view: every value that
 * differs from the current one goes through lim (NULL: written exactly, e.g. a restore), and one
 * undo step is pushed before the first real change. Returns the number of values changed. */
size_t lt_tabs_apply(lt_tblview *v, int what, const double *nv, lt_tabs_lim *lim);

/* before a commit: warn (toast) when the largest pending cell change is large (logging alert) */
int  lt_tabs_check_change(lt_app *app, const lt_tblview *v);

/* ---- Set table size dialog (tables_size.c) ---- */
typedef struct lt_tabs_size_dlg {
    int open;
    double cols, rows;
    lt_table_size info;
} lt_tabs_size_dlg;
/* LT_ENOENT when t is not resizable; else 0 and the dialog is opened at the active size */
int  lt_tabs_size_begin(lt_app *app, const lt_ini_table *t, lt_tabs_size_dlg *d);
/* Draw the modal. Returns 1 when the user applied a size (tables already resized in the
 * page image, shared tables included), <0 on error (toast shown), else 0. */
int  lt_tabs_size_draw(lt_app *app, const lt_ini_table *t, lt_tabs_size_dlg *d);

/* ---- Generate table dialog (tables_gen.c) ---- */
typedef struct lt_tabs_gen_dlg {
    int open;
    int write_axes;
    int role;                   /* lt_table_role of the table being generated */
} lt_tabs_gen_dlg;
/* Generated Z for a table role (VE / ignition / AFR target) resampled onto the axes
 * (xb: nx, yb: ny) or, when write_axes, onto axes derived from the engine (out_x/out_y are
 * then filled; otherwise they are copies of xb/yb). out_z holds nx*ny doubles. */

int  lt_tabs_gen_values(const lt_engine_spec *spec, int role, int write_axes,
                        const double *xb, size_t nx, const double *yb, size_t ny,
                        double *out_x, double *out_y, double *out_z);
/* Draw the modal (engine specs persist in the app between uses). Returns 1 when applied:
 * out_x/out_y/out_z (nx, ny, nx*ny) hold the new values. */
int  lt_tabs_gen_draw(lt_app *app, lt_tabs_gen_dlg *d, const char *title, const double *xb,
                      size_t nx, const double *yb, size_t ny, int zdig, double *out_x,
                      double *out_y, double *out_z);

/* ---- .table file import/export (tables_io.c) ---- */
typedef struct lt_tabs_io_dlg {
    int open;
    int use_axes;
    int match;                  /* file shape equals the table shape */
    char path[256];
    lt_table_file tf;
    int have;
} lt_tabs_io_dlg;
#define LT_TABS_FD_LOAD (LT_APP_FD_USER0 + 60)
#define LT_TABS_FD_SAVE (LT_APP_FD_USER0 + 61)
void lt_tabs_io_free(lt_tabs_io_dlg *d);
/* Parse the file and open the confirmation modal; <0 on error (message in lt_err_get) */
int  lt_tabs_io_open_file(lt_tabs_io_dlg *d, const char *path, size_t cols, size_t rows);
/* Draw the modal; returns 1 when the user confirmed (d->tf and d->use_axes are valid) */
int  lt_tabs_io_draw(lt_app *app, lt_tabs_io_dlg *d, size_t cols, size_t rows);
/* Values of the loaded file fitted to a cols x rows table: copied when shapes match, otherwise
 * bilinearly resampled onto the table's axes (xb/yb). out_z holds rows*cols doubles. */
int  lt_tabs_io_fit(const lt_table_file *tf, const double *xb, size_t cols, const double *yb,
                    size_t rows, double *out_z);
/* Write v's table to a TunerStudio .table file; 0 or <0 */
int  lt_tabs_io_save(const char *path, const char *xname, const char *yname, const double *x,
                     size_t cols, const double *y, size_t rows, const double *z);

/* ---- lambda preview companion of an AFR table (tables_lambda.c, G-27) ----
 * Draws into the next layout cell: header, live lambda/AFR readouts, fuel blend combo, read-only grid */
void lt_tabs_lambda_draw(lt_app *app, const lt_tblview *v, int ybottom);

/* saved AFR delay model of the project (afr_delay_model.json); 0 when present (dlg_afrdelay.c) */
int  lt_tabs_delay_model_load(lt_app *app, double *floor_ms, double *anchor_ms);

/* autotune service (feeds samples each realtime frame while a session runs) */
void lt_tabs_autotune_service(lt_app *app);

/* tab kinds */
extern const lt_app_tab_kind lt_tabs_kind_table, lt_tabs_kind_curve, lt_tabs_kind_autotune;

#endif
