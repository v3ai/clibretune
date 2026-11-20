/* at_wue.h - WUE Analyze: learns corrections to the warm-up enrichment curve from coolant
 * temperature, measured AFR/lambda and the target AFR (TunerStudio MS "WUE Analyze").
 *
 * The VE AutoTune machinery is reused: lt_at_filters (rpm window, throttle rate, accel
 * enrichment, fuel cut, sensor rails, custom expression, steady state), lt_at_authority (max
 * change per pass, value rails), the transport-delay buffer of an lt_at_state and the default
 * RPM delay curve.  WUE adds its own filters on top: cranking, after-start enrichment,
 * coolant rate of change, maximum throttle and a minimum sample count per bin.
 *
 * Curve values are in the units of the INI constant.  "Unity" is the value that means no
 * enrichment: multiplier = (value + offset) / unity (rusEFI cltFuelCorr: unity 1, offset 0;
 * Speeduino/MS percent curves: unity 100; zero-based curves (wuePercentOffset 100): offset 100,
 * unity 100).  The analyzer is warm-bin agnostic: bins at or above the warm temperature (by
 * default the first bin from which the curve is already neutral to the end, else the last bin)
 * are always driven back to unity, and the error measured on the warm engine (a VE problem, not
 * a warm-up one) is divided out of the colder bins.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AT_WUE_H
#define LT_AT_WUE_H

#include "autotune.h"

LT_BEGIN_DECLS

#define LT_WUE_MAX_BINS 64
#define LT_WUE_WARM_AUTO (-1000.0)

/* ---- shared lookup helpers ---- */
/* linear interpolation of y(x) on ascending bins, clamped at the ends (0 when n == 0) */
double lt_at_curve_lookup(const double *x, const double *y, size_t n, double v);
/* bilinear lookup of z (ny rows x nx cols, row-major) at (px on x, py on y), clamped */
double lt_at_table_lookup(const double *x, size_t nx, const double *y, size_t ny, const double *z,
                          double px, double py);

typedef struct lt_wue_config {
    lt_at_filters f;            /* rpm window, tps rate, accel enrichment, custom, steady (min_clt off) */
    lt_at_authority auth;       /* curve units: max change per pass (abs and %), value rails */
    double target_afr;          /* fallback target (AFR) when no table / sample target */
    double lambda_delay_ms;     /* transport delay, 0 = RPM curve */
    double cranking_rpm;        /* below = cranking (400) */
    double after_start_s;       /* ignore the first seconds after the engine started (30) */
    double max_tps;             /* % throttle, 0 = off (40) */
    double min_clt_rate;        /* deg/s; samples while coolant rises slower are ignored, 0 = off */
    double max_clt_rate;        /* deg/s; |rate| above is ignored (sensor jumps), 0 = off (2) */
    unsigned min_samples;       /* hits before a bin gets a recommendation (20) */
    double base_weight;         /* confidence ramp weight (40); <= 0 = full confidence */
    double min_change;          /* curve units; smaller changes are dropped (0) */
    lt_bool split_hits;         /* share a sample between the two neighbouring bins (TRUE) */
    lt_bool relative_to_warm;   /* divide out the warm-engine error (TRUE) */
    lt_bool monotonic;          /* a colder bin never gets less enrichment than a warmer one (TRUE) */
    lt_bool never_below_unity;  /* never recommend leaning below "no enrichment" (TRUE) */
    double warm_clt;            /* bins at/above are pinned to unity; LT_WUE_WARM_AUTO = detect */
} lt_wue_config;
void lt_wue_config_default(lt_wue_config *c);
void lt_wue_config_clear(lt_wue_config *c);   /* frees the filter strings */

typedef struct lt_wue_point {
    unsigned long t_ms;
    double rpm, load, clt;
    double afr;                 /* measured, AFR (lambda already normalised) */
    double target_afr;          /* AFR or lambda; 0 = state target table / config */
    double tps, tps_rate;
    double ego;                 /* closed-loop correction multiplier (1 = none, 0 = unknown) */
    double run_s;               /* seconds since the engine started, < 0 unknown */
    int accel_enrich_active, fuel_cut_active, cranking, after_start;   /* -1 unknown */
    const char *reject;         /* declared INI filter that rejects this sample, or NULL */
} lt_wue_point;
void lt_wue_point_default(lt_wue_point *p);

enum { LT_WUE_NO_DATA = 0, LT_WUE_FEW, LT_WUE_REC, LT_WUE_WARM, LT_WUE_LOCKED };
typedef struct lt_wue_bin {
    double clt, current, recommended;
    double factor;              /* mean measured/target after warm normalisation (1 = on target) */
    double weight;
    unsigned hits;
    double afr_mean, target_mean;
    int status;                 /* LT_WUE_* */
} lt_wue_bin;
const char *lt_wue_status_name(int status);

typedef struct lt_wue_state lt_wue_state;
lt_wue_state *lt_wue_new(void);
void   lt_wue_free(lt_wue_state *s);
/* bins (CLT, ascending) and current curve values; clears collected data. -1 when n < 2 */
int    lt_wue_set_curve(lt_wue_state *s, const double *clt, const double *vals, size_t n,
                        double offset, double unity);
/* only the current values (after a write); keeps the collected data */
void   lt_wue_update_values(lt_wue_state *s, const double *vals, size_t n);
size_t lt_wue_bin_count(const lt_wue_state *s);
double lt_wue_unity_value(const lt_wue_state *s);           /* curve value meaning no enrichment */
/* AFR target offset vs coolant (AFR units, added to the target); n == 0 clears */
void   lt_wue_set_afr_offset(lt_wue_state *s, const double *clt, const double *off, size_t n);
double lt_wue_afr_offset_at(const lt_wue_state *s, double clt);
/* target AFR/lambda table over rpm (x) and load (y); z NULL clears. Copied. */
void   lt_wue_set_target_table(lt_wue_state *s, const double *x, size_t nx, const double *y,
                               size_t ny, const double *z);
/* target AFR of a sample: its own target, else the table at (rpm, load), else the fallback */
double lt_wue_target_at(const lt_wue_state *s, double rpm, double load, double fallback);
void   lt_wue_start(lt_wue_state *s);      /* clears collected data; keeps curve and locks */
void   lt_wue_stop(lt_wue_state *s);
lt_bool lt_wue_is_running(const lt_wue_state *s);
void   lt_wue_lock(lt_wue_state *s, size_t bin, lt_bool on);
lt_bool lt_wue_is_locked(const lt_wue_state *s, size_t bin);
void   lt_wue_add(lt_wue_state *s, const lt_wue_point *p, const lt_wue_config *c);
/* per-bin results with recommendations (curve units); returns the bin count */
size_t lt_wue_bins(const lt_wue_state *s, const lt_wue_config *c, lt_wue_bin *out, size_t cap);
/* first bin pinned to unity */
size_t lt_wue_warm_index(const lt_wue_state *s, const lt_wue_config *c);
/* mean warm-engine factor (1 when not enough warm samples); *n warm samples */
double lt_wue_warm_factor(const lt_wue_state *s, const lt_wue_config *c, unsigned long *n);
unsigned long lt_wue_accepted(const lt_wue_state *s);
size_t lt_wue_rejections(const lt_wue_state *s, lt_at_reject *out, size_t cap);   /* count desc */

/* ---- glue: channels, realtime samples, log replay ---- */
typedef struct lt_wue_channels {
    lt_at_channels at;          /* rpm, load, afr (+lambda flag), clt, tps, tps rate, cut, accel */
    char ego[48], cranking[48], engine[48], run_s[48];
} lt_wue_channels;
/* [WueAnalyze] coolant/lambda/ego channels when declared, else the VE AutoTune picks */
void    lt_wue_channels_from_ini(const lt_ini *ini, lt_wue_channels *out);
lt_bool lt_wue_sample_from_rt(const lt_ini_rt *rt, const lt_wue_channels *ch, unsigned long now_ms,
                              lt_wue_point *out);
/* Feed every row of a log (declared filters: optional, enabled NULL = all); returns the
 * number of accepted samples or < 0.  The state is started (cleared) first. */
long    lt_wue_replay(lt_wue_state *s, const lt_log *log, const lt_wue_channels *ch,
                      const lt_wue_config *c, const lt_ini_filter *flt, size_t nflt,
                      const unsigned char *enabled);
/* unity/offset of a warm-up curve constant: (offset from wuePercentOffset) */
void    lt_wue_units_for(const lt_ini_const *z, double percent_offset, double *offset, double *unity);

LT_END_DECLS

#endif
