/* at_extra.h - AutoTune extras with no UI: heat-map intensity and trace ring, load-source
 * detection, CSV / proposal export, delay model file, replay analysis report, accel
 * enrichment analysis of a log, delay test planning and statistics.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AT_EXTRA_H
#define LT_AT_EXTRA_H

#include "autotune.h"

LT_BEGIN_DECLS

/* ---- heat map views (G-52) ---- */
enum { LT_ATX_MAP_VALUE = 0, LT_ATX_MAP_WEIGHT, LT_ATX_MAP_CHANGE, LT_ATX_MAP_COVERAGE, LT_ATX_MAP_COUNT };
const char *lt_atx_map_name(int mode);
/* 0..1 intensity of a cell for the view; r NULL = cell without data (0).  The maxima
 * normalise (hits / weight / absolute change) and may be 0. */
double lt_atx_map_fraction(int mode, const lt_at_rec *r, double max_hits, double max_weight,
                           double max_change);
void lt_atx_map_maxima(const lt_at_rec *recs, size_t n, double *max_hits, double *max_weight,
                       double *max_change);

#define LT_ATX_TRACE_MAX 96
typedef struct lt_atx_trace {
    unsigned x[LT_ATX_TRACE_MAX], y[LT_ATX_TRACE_MAX];
    size_t n, head;               /* head = next write slot */
} lt_atx_trace;
void   lt_atx_trace_clear(lt_atx_trace *t);
void   lt_atx_trace_push(lt_atx_trace *t, unsigned x, unsigned y);   /* repeats are merged */
size_t lt_atx_trace_count(const lt_atx_trace *t);
int    lt_atx_trace_at(const lt_atx_trace *t, size_t i, unsigned *x, unsigned *y); /* 0 = oldest */

/* ---- heat map zoom and pan (G-52), implemented in at_view.c ----
 * The zoom is a percentage of the normal cell size.  Scroll offsets, anchors and views are in
 * device pixels; cell sizes passed to lt_atx_zoom_anchor are device pixels too. */
#define LT_ATX_ZOOM_MIN 25
#define LT_ATX_ZOOM_MAX 400
#define LT_ATX_ZOOM_DEF 100
int lt_atx_zoom_clamp(int pct);
/* zoom after `notches` steps (> 0 zooms in, x1.25 each); a result within 6% of 100 snaps to 100 */
int lt_atx_zoom_step(int pct, int notches);
/* largest zoom (never above 100) at which cols x rows cells of the base size fit the view */
int lt_atx_zoom_fit(int view_w, int view_h, int cols, int rows, int base_cw, int base_ch);
/* base_px scaled by the zoom, at least min_px */
int lt_atx_zoom_px(int base_px, int pct, int min_px);
/* font size of cell text at the zoom: base_px scaled, kept within 8..22 */
int lt_atx_zoom_font(int base_px, int pct);
/* scroll offset that keeps the content point under `anchor` (offset in the view) in place when
 * the cell size changes from old_cell to new_cell; clamped to the new content of count cells */
int lt_atx_zoom_anchor(int scroll, int anchor, int old_cell, int new_cell, int count, int view);
/* scroll offset limited to 0 .. content - view */
int lt_atx_pan_clamp(int scroll, int content, int view);
/* label every n-th axis cell so labels stay at least min_px apart (1 = every cell) */
int lt_atx_label_stride(int cell_px, int min_px);

/* ---- dyno overlay on the tuned table (G-53), at_view.c ---- */
enum { LT_ATX_DYNO_HP = 0, LT_ATX_DYNO_TQ, LT_ATX_DYNO_AFR, LT_ATX_DYNO_COUNT };
const char *lt_atx_dyno_name(int what);
/* idx[row * cols + col] = index of that cell in `cells` or -1 (idx holds rows * cols ints) */
void    lt_atx_dyno_index(const lt_dyno_cell *cells, size_t n, size_t rows, size_t cols, int *idx);
lt_bool lt_atx_dyno_value(const lt_dyno_cell *c, int what, double *v);
/* range of `what` over the cells that have it; LT_FALSE (lo = hi = 0) when none has it */
lt_bool lt_atx_dyno_range(const lt_dyno_cell *cells, size_t n, int what, double *lo, double *hi);
/* short cell label: "142 hp", "180 ft", "12.6" ("" when the cell lacks the value) */
void    lt_atx_dyno_label(const lt_dyno_cell *c, int what, char *out, size_t cap);

/* ---- per-sample verdicts of a replay (G-55), at_view.c ---- */
#define LT_ATX_VREASONS 16
typedef struct lt_atx_vrow {
    double t, rpm, load, afr;     /* log time (s), operating point, measured AFR */
    unsigned cx, cy;              /* nearest table cell of the operating point */
    int reason;                   /* -1 = counted, else index into lt_atx_verdicts.reason */
} lt_atx_vrow;
typedef struct lt_atx_verdicts {
    lt_atx_vrow *rows; size_t n;
    const char *reason[LT_ATX_VREASONS];        /* distinct reasons, count desc then name asc */
    unsigned long count[LT_ATX_VREASONS];
    size_t nreason;
    unsigned long counted;
} lt_atx_verdicts;
void lt_atx_verdicts_init(lt_atx_verdicts *v);
/* one row per log row from the replay's verdicts; the log supplies time and operating point.
 * Reasons beyond LT_ATX_VREASONS - 1 distinct ones share the last slot ("other"). 0 or <0. */
int  lt_atx_verdicts_build(lt_atx_verdicts *v, const lt_replay_report *rep, const lt_log *log,
                           const lt_at_channels *ch, const double *xbins, size_t nx,
                           const double *ybins, size_t ny);
void lt_atx_verdicts_free(lt_atx_verdicts *v);
enum { LT_ATX_VF_ALL = 0, LT_ATX_VF_COUNTED, LT_ATX_VF_DROPPED, LT_ATX_VF_REASON };
typedef struct lt_atx_vfilter {
    int kind;                     /* LT_ATX_VF_* */
    int reason;                   /* LT_ATX_VF_REASON: reason index */
    int cx, cy;                   /* only rows in this cell; -1 = any cell */
} lt_atx_vfilter;
lt_bool lt_atx_verdict_match(const lt_atx_verdicts *v, size_t i, const lt_atx_vfilter *f);
/* indices of matching rows in log order (up to cap written); returns the number of matches */
size_t  lt_atx_verdicts_select(const lt_atx_verdicts *v, const lt_atx_vfilter *f, size_t *idx,
                               size_t cap);
/* the verdict most rows of [from, to) share (ties: lower index; counted is -1 and ranks first):
 * a reason index, -1 = counted, -2 = no rows */
int     lt_atx_verdict_dominant(const lt_atx_verdicts *v, size_t from, size_t to);
/* timeline column of a row and back: column = row * w / n, row = column * n / w */
size_t  lt_atx_verdict_row_at(size_t n, int w, int x);

/* ---- load source (G-50) ---- */
enum { LT_ATX_LOAD_MAP = 0, LT_ATX_LOAD_MAF, LT_ATX_LOAD_TPS };
const char *lt_atx_load_name(int src);
/* guess from the load axis channel name of the table ("tps", "throttle" -> TPS; "maf" -> MAF) */
int  lt_atx_load_detect(const char *y_channel);
/* point ch->load at the channel of the chosen source; 0 when that channel is unavailable */
int  lt_atx_load_apply(lt_at_channels *ch, int src);

/* ---- export (G-54) ---- */
void   lt_atx_csv(lt_buf *out, const lt_at_rec *recs, size_t n, const double *xbins, size_t nx,
                  const double *ybins, size_t ny);
/* current table with every recommendation applied (rows*cols); returns cells changed */
size_t lt_atx_proposed(const double *cur, size_t rows, size_t cols, const lt_at_rec *recs,
                       size_t n, double *out);
/* JSON proposal; returns cells changed */
size_t lt_atx_proposal_json(lt_buf *out, const char *table, const char *signature, const double *cur,
                            size_t rows, size_t cols, const lt_at_rec *recs, size_t n);

/* ---- delay test (G-51) ---- */
#define LT_ATX_STEP_MIN_PCT 3.0
#define LT_ATX_STEP_MAX_PCT 20.0
#define LT_ATX_MAX_REPEATS 200
#define LT_ATX_HOLD_MIN_MS 500
#define LT_ATX_HOLD_MAX_MS 5000
#define LT_ATX_TPS_DOT_LIMIT 10.0
/* enrichment only, bounded */
double        lt_atx_clamp_step(double pct);
unsigned long lt_atx_clamp_hold(double ms);
int           lt_atx_clamp_repeats(double n);
/* applied slot value: baseline * (1 + pct/100) */
double lt_atx_step_value(double baseline, double pct);
typedef struct lt_atx_delays {
    lt_delay_table table;
    double rpm[LT_ATX_MAX_REPEATS], load[LT_ATX_MAX_REPEATS], ms[LT_ATX_MAX_REPEATS];
    size_t n;
    double sum, mn, mx;
} lt_atx_delays;
void lt_atx_delays_init(lt_atx_delays *d);
void lt_atx_delays_add(lt_atx_delays *d, double rpm, double load, double delay_ms);
double lt_atx_delays_median(const lt_atx_delays *d);   /* 0 when empty */
/* JSON of the flow fit (afr_delay_model.json); returns 0 or -1 when a fit is not possible */
int  lt_atx_delay_model(const lt_atx_delays *d, lt_flow_fit *fit);
void lt_atx_delay_model_json(lt_buf *out, const lt_flow_fit *fit);

/* ---- offline analysis report (G-55) ---- */
void lt_atx_replay_report(lt_buf *out, const lt_replay_report *rep, size_t n_declared_filters);
/* accel-enrichment analysis of a log: events and per-bin recommendations as text; returns the
 * event count or -1 when the log lacks the channels */
int  lt_atx_ae_report(lt_buf *out, const lt_log *log, const lt_at_channels *ch, double target_afr);

LT_END_DECLS

#endif
