/* dl_view.h - log viewing and analysis logic behind the Data Logging / Datalog Viewer tabs and the
 * curve editor X-Y overlay (MegaLogViewer / TunerStudio style).  Pure logic, no UI:
 *   - graph setup: pages of stacked panes, each pane a few traces with their own scale and
 *     axis side; JSON persistence
 *   - expression helpers on logs: per-row evaluation and filter masks (through the math hooks)
 *   - 2-D histogram / table generator: average of a Z channel binned over X/Y axes, hit counts
 *   - X-Y point ring with timestamps (live overlay points with age fade)
 *   - live math channels: evaluation of the project math channels against the realtime values
 *     and the virtual channel list that channel pickers and the recorder see
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DL_VIEW_H
#define LT_DL_VIEW_H

#include "datalog.h"

/* ====================================================================== */
/* Graph setup                                                             */
/* ====================================================================== */
#define LT_GRAPH_MAX_TRACES 8
#define LT_GRAPH_MAX_PANES  6
#define LT_GRAPH_MAX_PAGES  8

typedef struct lt_graph_trace {
    char   channel[64];
    int    autoscale;        /* 1: scale from the visible data, 0: fixed lo..hi */
    double lo, hi;
    int    right;            /* 1: drawn against the right axis */
} lt_graph_trace;

typedef struct lt_graph_pane {
    lt_graph_trace tr[LT_GRAPH_MAX_TRACES];
    int n;
} lt_graph_pane;

typedef struct lt_graph_page {
    char name[32];
    lt_graph_pane pane[LT_GRAPH_MAX_PANES];
    int n_panes;
} lt_graph_page;

typedef struct lt_graph_setup {
    lt_graph_page *pages;    /* heap array of n_pages */
    int n_pages;
} lt_graph_setup;

void lt_graph_init(lt_graph_setup *g);              /* empty */
void lt_graph_free(lt_graph_setup *g);
void lt_graph_copy(lt_graph_setup *dst, const lt_graph_setup *src);
/* One page "Main" with up to 4 panes picked from the available channel names (rpm; map/tps;
 * afr/lambda/ego; coolant/iat), falling back to the first channels. */
void lt_graph_defaults(lt_graph_setup *g, const char *const *names, int n);
/* returns the new page index, -1 when full */
int  lt_graph_add_page(lt_graph_setup *g, const char *name);
void lt_graph_remove_page(lt_graph_setup *g, int page);
/* returns the new pane index, -1 when full */
int  lt_graph_add_pane(lt_graph_page *p);
void lt_graph_remove_pane(lt_graph_page *p, int pane);
/* add a trace (autoscaled, left axis); index, or -1 when full; an existing one is returned as is */
int  lt_graph_pane_add(lt_graph_pane *p, const char *channel);
void lt_graph_pane_remove(lt_graph_pane *p, int trace);
int  lt_graph_pane_find(const lt_graph_pane *p, const char *channel);
/* scale of trace over the values (NaN/inf skipped): fixed lo..hi, or the data range padded 5%
 * (a flat line gets +-0.5, no data 0..1) */
void lt_graph_trace_range(const lt_graph_trace *t, const double *v, long n, double *lo, double *hi);
/* name match rank used to pick default channels: 0 equal (or word + "Value"), 1 prefix,
 * 2 contains, 3 contains but starts with "raw", -1 none (case-insensitive) */
int lt_graph_name_rank(const char *name, const char *word);
/* "nice" tick step for a span split into about `want` intervals (1, 2, 2.5, 5 x 10^k) */
double lt_graph_nice_step(double span, int want);

/* JSON: {"version":1,"pages":[{"name":..,"panes":[{"traces":[{"channel":..,"auto":true,
 * "min":..,"max":..,"axis":"left"|"right"}]}]}]} */
char *lt_graph_to_json(const lt_graph_setup *g);                       /* malloc'd */
int   lt_graph_from_json(const char *text, size_t len, lt_graph_setup *g);   /* 0 / LT_EDATA */
int   lt_graph_save(const char *path, const lt_graph_setup *g);
/* missing file -> LT_ENOENT (g untouched); bad file -> LT_EDATA */
int   lt_graph_load(const char *path, lt_graph_setup *g);

/* ====================================================================== */
/* Expressions on logs (uses the lt_math hooks)                            */
/* ====================================================================== */
/* Evaluate expr for every row (variables = the log's channels): out[i] (NaN on evaluation
 * error). 0 ok, LT_EDATA (err filled) when it does not compile, LT_EUNSUPPORTED without hooks. */
int  lt_log_eval_expr(const lt_log *log, const char *expr, double *out, char *err, size_t errlen);
/* Filter mask: mask[i] = 1 where expr is nonzero (blank expr: all rows). Returns the number of
 * rows passing, or a negative error (err filled). */
long lt_log_filter(const lt_log *log, const char *expr, unsigned char *mask, char *err,
                   size_t errlen);

/* ====================================================================== */
/* 2-D histogram / table generator                                         */
/* ====================================================================== */
typedef struct lt_hist2d {
    int nx, ny;
    double *xb, *yb;         /* bin centres (axis values), ascending; ny = 1 for a 1-D histogram */
    double *sum, *avg;       /* ny*nx, row-major [y*nx + x]; avg NaN where no hits */
    long *hits;
    long n_binned;           /* samples binned */
    double zmin, zmax;       /* range of the cell averages (0..1 when none) */
    long max_hits;
} lt_hist2d;
/* nearest bin centre (ties -> the lower bin); -1 when n == 0 or v is NaN */
int  lt_hist_bin(const double *centres, int n, double v);
/* n evenly spaced centres from lo to hi (n >= 1) */
void lt_hist_auto_bins(double lo, double hi, int n, double *out);
void lt_hist2d_init(lt_hist2d *h, const double *xb, int nx, const double *yb, int ny);
void lt_hist2d_free(lt_hist2d *h);
void lt_hist2d_add(lt_hist2d *h, double x, double y, double z);   /* NaN z ignored */
void lt_hist2d_finish(lt_hist2d *h);
/* Bin rows [i0, i1] (clamped) of a log: cx/cy/cz channel indices (cy < 0: 1-D, all in row 0;
 * cz < 0: counts only, avg = hits). mask may be NULL. Calls lt_hist2d_finish. */
void lt_hist2d_from_log(lt_hist2d *h, const lt_log *log, int cx, int cy, int cz,
                        const unsigned char *mask, long i0, long i1);

/* ====================================================================== */
/* X-Y point ring (curve editor overlay)                                   */
/* ====================================================================== */
typedef struct lt_xyring {
    double *x, *y, *t;
    int cap, head, n;        /* head = slot of the oldest point */
} lt_xyring;
void lt_xyring_init(lt_xyring *r, int cap);
void lt_xyring_free(lt_xyring *r);
void lt_xyring_clear(lt_xyring *r);
/* append (the oldest point is dropped when full); NaN x/y ignored. Returns 1 stored, 0 not. */
int  lt_xyring_push(lt_xyring *r, double x, double y, double t);
int  lt_xyring_count(const lt_xyring *r);
/* i = 0 is the oldest */
void lt_xyring_get(const lt_xyring *r, int i, double *x, double *y, double *t);
/* drop points older than now - max_age */
void lt_xyring_expire(lt_xyring *r, double now, double max_age);
/* point opacity 0..1 for an age (seconds): 1 when new, falling linearly to 0.15 at fade_s,
 * 0 beyond; fade_s <= 0 = never fades (always 1) */
double lt_xy_age_alpha(double age, double fade_s);

/* ====================================================================== */
/* Live math channels and virtual channels                                 */
/* ====================================================================== */
typedef struct lt_math_live {
    double *vals; size_t n;  /* last values in stored order of the set */
    int busy;                /* evaluation in progress (re-entrancy guard) */
    int valid;
} lt_math_live;
void lt_math_live_free(lt_math_live *ml);
/* Evaluate every channel of s against env (realtime channels + constants). 0 ok, else the
 * lt_math_eval_all error (values still stored, NaN where a channel failed). */
int  lt_math_live_eval(lt_math_live *ml, lt_math_set *s, const lt_expr_env *env);
/* value of math channel `name` from the last evaluation (LT_FALSE: unknown, never evaluated
 * or NaN) */
lt_bool lt_math_live_get(const lt_math_live *ml, const lt_math_set *s, const char *name,
                         double *out);

/* Virtual (computed, non-INI) channels that lt_datalog_channels_from_ini appends after the INI
 * channels (names already in the INI are skipped): the app installs the project's math channels
 * here so channel pickers, the status bar choices and the recorder list them. Copied; n = 0
 * clears. */
void   lt_datalog_set_virtual_channels(const char *const *names, const char *const *units, size_t n);
size_t lt_datalog_virtual_count(void);
const char *lt_datalog_virtual_name(size_t i);
const char *lt_datalog_virtual_units(size_t i);

#endif
