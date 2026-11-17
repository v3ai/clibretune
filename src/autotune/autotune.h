/* autotune.h - VE Analyze (AutoTune): live recommendations, filters, preflight,
 *              log replay, transport-delay measurement, accel-enrichment tuning,
 *              table anomaly/health scoring, zero-hit cell prediction.
 *
 * Spec: docs/spec/07-autotune.md (section numbers in comments refer to it).
 *
 * Data flow in the app (single threaded main loop):
 *   proto realtime frame -> lt_ini_rt -> lt_at_sample_from_rt() -> lt_ve_point
 *   -> lt_at_add_data_point()  (state keeps recommendations per cell)
 *   -> lt_at_apply_to_view()   (writes recommended values into a lt_tblview of
 *      the VE table when the user accepts; commit via lt_tblview_commit)
 * Replay runs the same engine over a lt_log (datalog.h).
 *
 * Tables here are plain double arrays in DISPLAY units, [row=y=load][col=x=rpm]
 * stored flat row-major z[y*nx + x] (same as tableops.h). Recommendation coords:
 * cell_x = col, cell_y = row.
 *
 * MEMORY: lt_at_state is opaque, heap allocated (lt_at_new/free). Reference
 * tables passed to lt_at_set_reference_tables are COPIED. Result arrays
 * returned by *_run functions are malloc'd and have matching *_free.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AUTOTUNE_H
#define LT_AUTOTUNE_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../ini/expr.h"
#include "../tableops/tableops.h"
#include "../datalog/datalog.h"

/* ---- constants (spec 1) ---- */
#define LT_AT_STOICH_AFR            14.7
#define LT_AT_LAMBDA_THRESHOLD      2.0     /* value < 2.0 is lambda, else AFR */
#define LT_AT_AFR_RAIL_LOW          10.0
#define LT_AT_AFR_RAIL_HIGH         19.5
#define LT_AT_STEADY_RPM_TOLERANCE  100.0
#define LT_AT_STEADY_LOAD_TOLERANCE 3.0
#define LT_AT_CENTRE_ONLY_MIN_WEIGHT 0.75
double lt_at_normalise_to_afr(double v);       /* v < 2 ? v*14.7 : v */

/* ---- configuration (spec 2) ---- */
typedef enum lt_hit_weighting {
    LT_HW_UNIFORM = 0, LT_HW_CELL_PROXIMITY, LT_HW_CELL_PROXIMITY_SQUARED, LT_HW_CELL_CENTRE_ONLY
} lt_hit_weighting;

typedef struct lt_at_settings {
    double target_afr;            /* 14.7 */
    unsigned long update_rate_ms; /* 100 (UI pacing only) */
    double lambda_delay_ms;       /* 0 = auto; >0 fixed transport delay */
    lt_bool lambda_delay_flow_scaled;
    double lambda_delay_floor_ms; /* 120 */
    lt_hit_weighting hit_weighting;
    double base_weight;           /* 20; <=0 disables the confidence ramp */
    double min_change;            /* 1.0 */
} lt_at_settings;
void lt_at_settings_default(lt_at_settings *s);

typedef struct lt_at_authority {
    double max_cell_value_change;       /* 10 */
    double max_cell_percentage_change;  /* 20 */
    double min_cell_value;              /* 0 */
    double max_cell_value;              /* 255 */
} lt_at_authority;
void   lt_at_authority_default(lt_at_authority *a);
double lt_at_apply_authority_limits(double beginning, double recommended, const lt_at_authority *a);
double lt_at_clamp_to_rails(const lt_at_authority *a, double v);

typedef struct lt_at_filters {
    double min_rpm, max_rpm;       /* 1000, 7000 */
    char  *min_y_axis, *max_y_axis;/* numeric text or NULL (copied by lt_at_filters_set_*) */
    double min_clt;                /* 160 (F-shaped default; app substitutes the INI minCltFilter) */
    char  *custom_filter;          /* expression over rpm,map,maf,load,afr,ve,clt,tps,tps_rate,accel_enrich */
    double max_tps_rate;           /* 50 %/s */
    lt_bool exclude_accel_enrich;  /* true */
    unsigned long min_steady_ms;   /* 0 = off */
} lt_at_filters;
void lt_at_filters_default(lt_at_filters *f);
void lt_at_filters_clear(lt_at_filters *f);             /* frees owned strings */
void lt_at_filters_set_y_range(lt_at_filters *f, const char *min_y, const char *max_y);
void lt_at_filters_set_custom(lt_at_filters *f, const char *expr);
/* Take min_clt from the INI's declared minCltFilter (VeAnalyze filter) when the value is
 * still the untouched 160 default. */
void lt_at_filters_adopt_ini(lt_at_filters *f, const lt_ini *ini);

typedef struct lt_ve_point {      /* one sample (spec 2 VEDataPoint) */
    double rpm, map, maf, load, afr /* 14.7 */, ve, clt, tps, tps_rate;
    int accel_enrich_active;      /* -1 unknown, 0 false, 1 true */
    int fuel_cut_active;          /* -1 unknown, 0, 1 */
    unsigned long timestamp_ms;
} lt_ve_point;
void lt_ve_point_default(lt_ve_point *p);

/* Optional reference tables (rows ny x cols nx; NULL = not supplied). */
typedef struct lt_at_reftables {
    size_t nx, ny;
    const double *lambda_delay;   /* ms per cell */
    const double *target_afr;     /* AFR or lambda per cell */
    const double *ve;             /* the table being tuned (beginning values) */
} lt_at_reftables;

typedef struct lt_at_rec {
    unsigned cell_x, cell_y;
    double beginning_value, recommended_value;
    unsigned hit_count;
    double hit_weighting;          /* accumulated weight */
    double target_afr, hit_percentage;
} lt_at_rec;

typedef struct lt_at_reject { const char *reason; unsigned long count; } lt_at_reject;

/* ---- engine (spec 3) ---- */
typedef struct lt_at_state lt_at_state;
lt_at_state *lt_at_new(void);
void   lt_at_free(lt_at_state *s);
void   lt_at_start(lt_at_state *s);      /* clears recs/buffer/counters; keeps locks, tables, flags */
void   lt_at_stop(lt_at_state *s);
lt_bool lt_at_is_running(const lt_at_state *s);
void   lt_at_set_table_name(lt_at_state *s, const char *name);
const char *lt_at_table_name(const lt_at_state *s);
void   lt_at_set_reference_tables(lt_at_state *s, const lt_at_reftables *r);   /* copies */
void   lt_at_set_strict_lambda_match(lt_at_state *s, lt_bool on);              /* default true */
lt_bool lt_at_is_cell_locked(const lt_at_state *s, unsigned x, unsigned y);
void   lt_at_lock_cell(lt_at_state *s, unsigned x, unsigned y);
void   lt_at_unlock_cell(lt_at_state *s, unsigned x, unsigned y);
/* 0 = accepted, else a reason string ("clt below min_clt", "rpm out of range", ...) via *reason. */
int    lt_at_classify(const lt_at_state *s, const lt_ve_point *pt, const lt_at_filters *f,
                      const char **reason);
void   lt_at_add_data_point(lt_at_state *s, const lt_ve_point *pt, const double *xbins, size_t nx,
                            const double *ybins, size_t ny, const lt_at_settings *set,
                            const lt_at_filters *f, const lt_at_authority *auth);
/* Recommendations sorted by (y, x). Returns count (copies up to cap). */
size_t lt_at_recommendations(const lt_at_state *s, lt_at_rec *out, size_t cap);
size_t lt_at_rejections(const lt_at_state *s, lt_at_reject *out, size_t cap);   /* count desc, reason asc */
unsigned long lt_at_total_samples(const lt_at_state *s);
unsigned long lt_at_required_buffer_ms(const lt_at_state *s, const lt_at_settings *set,
                                       const lt_at_filters *f);
unsigned long lt_at_default_delay_ms(double rpm);          /* spec 3.1 curve (200ms@800 -> 50ms@6000) */
/* Per-cell weight of a sample (spec 3.5). */
double lt_at_hit_weight(lt_hit_weighting mode, double rpm, double load, size_t cx, size_t cy,
                        const double *xbins, size_t nx, const double *ybins, size_t ny);
/* nearest-axis bin index with out-of-axis rejection (spec 3.4); returns -1 if none. */
int    lt_at_find_bin(double v, const double *bins, size_t n);
/* Flow scaled delay table (spec 3.10): out has ny*nx entries (ms). */
void   lt_at_flow_scaled_delay_table(const double *ve, const double *xbins, size_t nx,
                                     const double *ybins, size_t ny, double idle_delay_ms,
                                     double floor_ms, double *out);

/* ---- diagnostics / test hooks (additive) ---- */
size_t        lt_at_buffer_count(const lt_at_state *s);
unsigned long lt_at_buffer_oldest_ms(const lt_at_state *s);
unsigned long lt_at_buffer_max_age_ms(const lt_at_state *s);
void          lt_at_set_buffer_max_age_ms(lt_at_state *s, unsigned long ms);
unsigned long lt_at_mean_sample_gap_ms(const lt_at_state *s);
/* Push a sample into the delay buffer without classifying (prunes by max age). */
void          lt_at_push_sample(lt_at_state *s, const lt_ve_point *pt);
/* Spec 3.9: buffered sample nearest to now-delay within tolerance; LT_FALSE if none. */
lt_bool       lt_at_find_delayed(const lt_at_state *s, unsigned long now_ms,
                                 unsigned long delay_ms, lt_ve_point *out);
unsigned long lt_at_configured_or_curve_delay_ms(const lt_at_settings *set, double rpm);

/* ---- glue to the rest of the app ---- */
typedef struct lt_at_channels {    /* channel names used to build samples */
    char rpm[48], load[48], afr[48], ve[48], clt[48], tps[48], tps_rate[48], map[48], maf[48];
    char fuel_cut[48], accel_enrich[48];
    lt_bool afr_is_lambda;         /* normalise (v<2 -> *14.7) */
} lt_at_channels;
/* Pick channel names from the INI (VeAnalyze lambda channel + common names rpm/RPMValue,
 * map/MAPValue, coolant, tps...). */
void lt_at_channels_from_ini(const lt_ini *ini, lt_at_channels *out);
/* Build a sample from the current realtime values; LT_FALSE if rpm/afr unavailable. */
lt_bool lt_at_sample_from_rt(const lt_ini_rt *rt, const lt_at_channels *ch, unsigned long now_ms,
                             lt_ve_point *out);
/* Fill the table-related inputs: axes + reference tables from the table view of the VE table
 * and an optional AFR target view. */
void lt_at_reftables_from_views(const lt_tblview *ve, const lt_tblview *target, lt_at_reftables *out);
/* Which tables may be tuned (spec 4). NULL = allowed, else refusal text (static buffer). */
const char *lt_fuel_tune_refusal(const lt_ini *ini, const char *table_name);
size_t      lt_fuel_tunable_tables(const lt_ini *ini, const char **names, size_t cap);   /* sorted */
/* proposals (spec 5): write recommended values into the view (marks cells dirty). */
size_t lt_at_apply_to_view(const lt_at_state *s, lt_tblview *view);
/* (changed cells, largest signed change) vs current view values. */
size_t lt_at_proposal_summary(const lt_at_rec *recs, size_t n, const double *z, size_t nx,
                              size_t ny, double *largest);

/* ---- declared INI filters (spec 6) ---- */
lt_bool lt_at_filter_rejects(const lt_ini_filter *f, lt_bool enabled, lt_bool have, double value);
/* first rejecting filter index, or -1; values via env (channel lookups). */
int     lt_at_first_rejecting(const lt_ini_filter *f, size_t n, const lt_expr_env *env);

/* ---- preflight (spec 7) ---- */
typedef enum lt_severity { LT_SEV_BLOCKER = 0, LT_SEV_WARNING = 1, LT_SEV_INFO = 2 } lt_severity;
typedef struct lt_finding {
    lt_severity sev;
    char code[32], title[96], detail[512], current[48], suggested[48];
} lt_finding;
typedef struct lt_preflight_input {
    const char *target_table;                  /* NULL = unresolved */
    const double *target_values; size_t t_rows, t_cols;
    size_t ve_rows, ve_cols;
    const double *ve_values;                   /* ve_rows*ve_cols */
    lt_bool celsius;
    lt_bool will_write_to_ecu;
} lt_preflight_input;
/* Sorted blocker < warning < info. Returns count (<= cap). */
size_t  lt_preflight_check(const lt_preflight_input *in, const lt_at_settings *s,
                           const lt_at_filters *f, const lt_at_authority *a, lt_finding *out,
                           size_t cap);
lt_bool lt_preflight_has_blocker(const lt_finding *f, size_t n);

/* ---- replay from logs (spec 8) ---- */
typedef struct lt_replay_config {
    lt_at_settings settings; lt_at_filters filters; lt_at_authority authority;
    lt_bool lambda_match_strict;   /* true */
    lt_bool validate;              /* true */
} lt_replay_config;
void lt_replay_config_default(lt_replay_config *c);
typedef struct lt_cell_result {
    size_t x, y; double rpm, load, current_ve, proposed_ve, delta; unsigned hits;
    double weight, confidence, target_afr, mean_afr;
} lt_cell_result;
typedef struct lt_validation { double gain_pct, worsened_pct; size_t scored; unsigned long folds; } lt_validation;
typedef struct lt_replay_report {
    lt_cell_result *cells; size_t ncells;
    unsigned long samples_all;
    lt_at_reject *rejections; size_t n_rejections;     /* reason strings static; count desc */
    const char **verdicts;                             /* per log row: NULL accepted, else reason (static) */
    size_t n_verdicts;
    lt_bool has_validation; lt_validation validation;
    unsigned *coverage;                                /* ny*nx hit counts */
    size_t nx, ny;
} lt_replay_report;
/* Channel names come from `ch`; log values are in the units of the log. */
int  lt_replay(const lt_log *log, const lt_at_channels *ch, const double *xbins, size_t nx,
               const double *ybins, size_t ny, const lt_at_reftables *ref,
               const lt_replay_config *cfg, lt_replay_report *out);
void lt_replay_report_free(lt_replay_report *r);

/* ---- transport delay measurement (spec 9) ---- */
typedef struct lt_afr_sample { unsigned long t_ms; double afr; } lt_afr_sample;
typedef struct lt_delay_measurement { double delay_ms, excursion, baseline_afr, leading_edge_ms; } lt_delay_measurement;
typedef enum lt_delay_rejection {
    LT_DR_INSUFFICIENT_BASELINE = 0, LT_DR_UNSTABLE_BASELINE, LT_DR_NO_RESPONSE,
    LT_DR_RESPONSE_PRECEDES_STEP, LT_DR_RESPONSE_NOT_SETTLED
} lt_delay_rejection;
const char *lt_delay_rejection_text(lt_delay_rejection r);
int  lt_detect_delay(unsigned long anchor_ms, const lt_afr_sample *pre, size_t npre,
                     const lt_afr_sample *post, size_t npost, lt_delay_measurement *out,
                     lt_delay_rejection *why);
/* aggregate delay table 6 rpm bins x 5 load bins + flow fit */
typedef struct lt_delay_cell { unsigned n; double mean_ms, range_ms, min_ms, max_ms; } lt_delay_cell;
typedef struct lt_delay_table { lt_delay_cell cells[5][6]; } lt_delay_table;
void lt_delay_table_init(lt_delay_table *t);
void lt_delay_table_add(lt_delay_table *t, double rpm, double load, double delay_ms);
typedef struct lt_flow_fit { double floor_ms, k, anchor_ms, rms_ms; size_t samples; } lt_flow_fit;
lt_bool lt_fit_flow_delay(const double *rpm, const double *load, const double *ms, size_t n,
                          lt_flow_fit *out);

/* ---- acceleration enrichment tuning (spec 10) ---- */
typedef struct lt_ae_sample { unsigned long timestamp_ms; double tps_dot, afr, afr_target, clt; } lt_ae_sample;
typedef struct lt_ae_config {
    double tae_bins[8]; size_t nbins; double tae_rates[8]; size_t nrates;
    double ae_time_ms, delay_ms, event_floor, min_clt; size_t min_events;
    double authority_pct, rate_gain_pct_per_afr, target_afr_fallback;
} lt_ae_config;
void lt_ae_config_default(lt_ae_config *c);
typedef struct lt_ae_event { double peak_tps_dot, residual_afr; } lt_ae_event;
typedef struct lt_ae_bin_rec {
    size_t bin; double lo; lt_bool has_hi; double hi; size_t events; lt_bool has_median;
    double median_residual_afr, current_rate, recommended_rate; lt_bool sufficient_data; char note[96];
} lt_ae_bin_rec;
size_t lt_ae_analyze(const lt_ae_sample *s, size_t n, const lt_ae_config *c, lt_ae_event *out,
                     size_t cap);
size_t lt_ae_recommend(const lt_ae_event *ev, size_t n, const lt_ae_config *c, lt_ae_bin_rec *out,
                       size_t cap);

/* ---- table anomaly detection (spec 11.1) ---- */
typedef enum lt_anomaly_type {
    LT_AN_OUTLIER = 0, LT_AN_MONOTONICITY, LT_AN_GRADIENT, LT_AN_UNREASONABLE, LT_AN_FLAT
} lt_anomaly_type;
typedef struct lt_anomaly {
    size_t row, col; double value, expected; lt_anomaly_type type; double severity; char description[128];
} lt_anomaly;
typedef struct lt_anomaly_config {
    double outlier_sigma, gradient_threshold, min_reasonable_ve, max_reasonable_ve;
    size_t min_flat_region_size; double flat_tolerance, monotonicity_load_floor_kpa, monotonicity_min_rpm;
} lt_anomaly_config;
void   lt_anomaly_config_default(lt_anomaly_config *c);
/* sorted by severity desc, one per cell; returns count (<= cap) */
size_t lt_anomaly_detect(const double *z, size_t rows, size_t cols, const double *xbins,
                         const double *ybins, const lt_anomaly_config *c, lt_anomaly *out, size_t cap);

/* ---- table health scoring (spec 11.2) ---- */
typedef enum lt_region_type { LT_REG_IDLE = 0, LT_REG_PART_THROTTLE, LT_REG_CRUISE, LT_REG_WOT, LT_REG_TRANSITION } lt_region_type;
typedef struct lt_region_health {
    char name[24]; lt_region_type type; unsigned score, coverage_score, smoothness_score, monotonicity_score;
    size_t cell_count, row0, row1, col0, col1;
} lt_region_health;
typedef struct lt_health_report {
    unsigned overall_score; char grade; lt_region_health regions[8]; size_t n_regions;
    char recommendations[16][160]; size_t n_recommendations;
    size_t cells_all, coverage_cells; double coverage_percent;
} lt_health_report;
void lt_health_score(const double *z, const unsigned *hits, size_t rows, size_t cols,
                     const double *xbins, const double *ybins, lt_health_report *out);

/* ---- zero-hit cell prediction (spec 11.3) ---- */
typedef enum lt_predict_method {
    LT_PM_BILINEAR = 0, LT_PM_NEIGHBOR, LT_PM_EXTRAPOLATE, LT_PM_ONE_D, LT_PM_PHYSICS
} lt_predict_method;
typedef struct lt_predicted_cell {
    size_t row, col; double predicted, current, confidence; lt_predict_method method; size_t neighbors;
} lt_predicted_cell;
typedef struct lt_predict_config {
    double min_confidence; size_t max_search_radius, min_hit_count; double distance_decay,
           physics_peak_torque_rpm, physics_min_ve, physics_max_ve;
} lt_predict_config;
void   lt_predict_config_default(lt_predict_config *c);
/* sorted by confidence desc; returns count (<= cap) */
size_t lt_predict_cells(const double *z, const unsigned *hits, size_t rows, size_t cols,
                        const double *xbins, const double *ybins, const lt_predict_config *c,
                        lt_predicted_cell *out, size_t cap);

#endif /* LT_AUTOTUNE_H */
