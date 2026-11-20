/* at_trim.h - Trim Table Analyze: per-cylinder fuel trim auto-tune (TunerStudio Ultra).
 *
 * Each cylinder has its own VE AutoTune engine (lt_at_state) running on that cylinder's trim
 * table, so filters, transport delay, hit weighting, the confidence ramp, cell locks and
 * authority limits are exactly the VE ones.  The engine works in "multiplier percent" (100 =
 * no trim): the trim table's values are converted with lt_trim_units and the sample's AFR is
 * replaced by the cylinder's fuelling error relative to the main VE:
 *
 *   error = (measured / target) / ve_factor          (single wideband, moved between cylinders)
 *   error = measured / reference                     (with a collector/reference sensor)
 *
 * where ve_factor is the pending VE AutoTune correction at that operating point (recommended /
 * current VE, 1 when none), i.e. the trim is what is left after the main VE correction.
 * Optional balancing keeps the average multiplier of the analysed cylinders unchanged, so the
 * VE table stays the master and the trims only redistribute fuel.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AT_TRIM_H
#define LT_AT_TRIM_H

#include "at_wue.h"

LT_BEGIN_DECLS

#define LT_TRIM_MAX_CYL 12

/* trim value <-> fuel multiplier: mult = (value + offset) / scale */
typedef struct lt_trim_units { double offset, scale; } lt_trim_units;
/* percent add (min < 0): offset 100 scale 100; percent multiplier (max > 5): offset 0 scale 100;
 * else a ratio (offset 0, scale 1) */
void   lt_trim_units_detect(const lt_ini_const *z, lt_trim_units *u);
double lt_trim_to_mult(const lt_trim_units *u, double v);
double lt_trim_from_mult(const lt_trim_units *u, double m);

typedef struct lt_trim_config {
    lt_at_settings set;         /* hit weighting, base weight, min change, delay (target unused) */
    lt_at_filters f;
    lt_at_authority auth;       /* multiplier percent (100 = no trim) */
    lt_bool balance;            /* keep the cylinders' average multiplier per cell */
} lt_trim_config;
void lt_trim_config_default(lt_trim_config *c);
void lt_trim_config_clear(lt_trim_config *c);

typedef struct lt_trim_state lt_trim_state;
lt_trim_state *lt_trim_new(size_t ncyl);
void    lt_trim_free(lt_trim_state *s);
size_t  lt_trim_cylinders(const lt_trim_state *s);
/* axes and current values of one cylinder's trim table (ny rows x nx cols); copied */
int     lt_trim_set_table(lt_trim_state *s, size_t cyl, const double *x, size_t nx, const double *y,
                          size_t ny, const double *z, const lt_trim_units *u);
/* target AFR/lambda table (rpm x load) used when a sample carries no target; z NULL clears */
void    lt_trim_set_target_table(lt_trim_state *s, const double *x, size_t nx, const double *y,
                                 size_t ny, const double *z);
double  lt_trim_target_at(const lt_trim_state *s, double rpm, double load, double fallback);
/* pending VE correction factor table (rpm x load, 1 = none) for replays; z NULL clears */
void    lt_trim_set_ve_factor_table(lt_trim_state *s, const double *x, size_t nx, const double *y,
                                    size_t ny, const double *z);
double  lt_trim_ve_factor_at(const lt_trim_state *s, double rpm, double load);
/* collecting on/off; the data of every cylinder is kept (the wideband moves between runs) */
void    lt_trim_start(lt_trim_state *s);
void    lt_trim_stop(lt_trim_state *s);
void    lt_trim_reset(lt_trim_state *s);           /* clear every cylinder (keeps locks) */
void    lt_trim_reset_cyl(lt_trim_state *s, size_t cyl);
lt_bool lt_trim_is_running(const lt_trim_state *s);
lt_at_state *lt_trim_engine(lt_trim_state *s, size_t cyl);   /* locks, rejections, counters */
/* one sample of a cylinder. pt->afr = that cylinder's measured AFR (normalised);
 * target <= 0.1 = target table / settings; ve_factor <= 0 = 1; ref_afr <= 0.1 = no reference */
void    lt_trim_add(lt_trim_state *s, size_t cyl, const lt_ve_point *pt, double target,
                    double ve_factor, double ref_afr, const lt_trim_config *c);
/* recommendations in trim units (beginning/recommended), sorted by (y, x); returns count */
size_t  lt_trim_recommendations(const lt_trim_state *s, size_t cyl, const lt_trim_config *c,
                                lt_at_rec *out, size_t cap);
unsigned long lt_trim_samples(const lt_trim_state *s, size_t cyl);
/* rejection reasons of a cylinder (engine filters plus the raw sensor rail), count desc */
size_t  lt_trim_rejections(const lt_trim_state *s, size_t cyl, lt_at_reject *out, size_t cap);
/* replay a log for one cylinder (the wideband was on that cylinder for the whole log); the
 * cylinder is cleared first, the others are kept.
 * ch->afr is the cylinder's lambda/AFR channel; ref_channel optional. Returns accepted samples. */
long    lt_trim_replay(lt_trim_state *s, size_t cyl, const lt_log *log, const lt_at_channels *ch,
                       const char *ref_channel, const lt_trim_config *c);

/* ---- INI discovery ---- */
typedef struct lt_trim_table_ref { const char *table; int cylinder; } lt_trim_table_ref;
/* per-cylinder fuel trim tables (sorted by cylinder, at most cap); "fuelTrimTbl3" -> 3 */
size_t  lt_trim_find_tables(const lt_ini *ini, lt_trim_table_ref *out, size_t cap);
/* lambda/AFR output channels (for per-cylinder or reference sensors), at most cap */
size_t  lt_trim_lambda_channels(const lt_ini *ini, const char **out, size_t cap);

LT_END_DECLS

#endif
