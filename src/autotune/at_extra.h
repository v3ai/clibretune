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
