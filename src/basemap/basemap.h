/* basemap.h - starting-tune generator from an engine description.
 *
 * Spec: docs/spec/08-sim-basemap-tableops.md section 4. Produces 16x16 VE,
 * ignition and AFR tables, enrichment curves, reqFuel and a few scalar
 * constants, as plain doubles; lt_basemap_apply() writes them into page images
 * through the INI (tables found by role / VeAnalyze names).
 *
 * Pure computation + one INI-aware apply step. No allocation except the
 * scalars list in the result (freed with lt_basemap_free).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_BASEMAP_H
#define LT_BASEMAP_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"

#define LT_BASEMAP_N 16   /* table size: 16 x 16 */

typedef enum lt_fuel_type { LT_FUEL_GASOLINE = 0, LT_FUEL_E85, LT_FUEL_E100, LT_FUEL_METHANOL, LT_FUEL_LPG } lt_fuel_type;
typedef enum lt_aspiration { LT_ASP_NA = 0, LT_ASP_TURBO, LT_ASP_SUPERCHARGED } lt_aspiration;
typedef enum lt_stroke { LT_STROKE_FOUR = 0, LT_STROKE_TWO } lt_stroke;
typedef enum lt_inj_mode { LT_INJ_SIMULTANEOUS = 0, LT_INJ_BATCH, LT_INJ_SEQUENTIAL, LT_INJ_THROTTLE_BODY } lt_inj_mode;
typedef enum lt_ign_mode { LT_IGN_WASTED_SPARK = 0, LT_IGN_COIL_ON_PLUG, LT_IGN_DISTRIBUTOR } lt_ign_mode;
typedef enum lt_chamber { LT_CH_OPEN = 0, LT_CH_QUENCH_TWO_VALVE, LT_CH_SWIRL_MULTI_VALVE } lt_chamber;

typedef struct lt_engine_spec {
    int cylinder_count;                 /* 4 */
    double displacement_cc;             /* 2000 */
    double injector_size_cc;            /* 440 cc/min */
    lt_fuel_type fuel; lt_aspiration aspiration; lt_stroke engine_stroke;
    lt_inj_mode inj_mode; lt_ign_mode ign_mode;
    unsigned idle_rpm, redline_rpm;     /* 800, 6500 */
    lt_bool has_boost; double boost_target_kpa;
    lt_bool has_wot_afr; double target_wot_afr;
    lt_bool has_octane; double octane;
    lt_bool has_cr; double compression_ratio;
    lt_bool has_chamber; lt_chamber chamber;
} lt_engine_spec;
void lt_engine_spec_default(lt_engine_spec *s);

/* derived engine values */
double lt_fuel_stoich_afr(lt_fuel_type f);          /* 14.7 gas, 9.8 E85, 9.0 E100, 6.5 methanol, 15.7 LPG */
double lt_engine_req_fuel(const lt_engine_spec *s); /* ms, clamped 0.1..25.5 */
double lt_engine_safe_wot_afr(const lt_engine_spec *s);
double lt_engine_max_load_kpa(const lt_engine_spec *s);
double lt_engine_effective_octane(const lt_engine_spec *s);
double lt_engine_max_spark_advance(const lt_engine_spec *s);

/* axes (count entries, usually 16) */
void lt_basemap_rpm_bins(unsigned idle, unsigned redline, size_t count, double *out);
void lt_basemap_load_bins(double max_load, size_t count, double *out);

typedef struct lt_curve_pt { double coolant_c, value; } lt_curve_pt;
typedef struct lt_basemap_scalar { char name[32]; double value; } lt_basemap_scalar;

typedef struct lt_basemap {
    double rpm_bins[LT_BASEMAP_N], load_bins[LT_BASEMAP_N];
    double ve[LT_BASEMAP_N][LT_BASEMAP_N];      /* [load row][rpm col] */
    double ign[LT_BASEMAP_N][LT_BASEMAP_N];
    double afr[LT_BASEMAP_N][LT_BASEMAP_N];
    lt_curve_pt cranking[8], warmup[8];          /* % at -40..100 C */
    double prime_pulse_ms;
    double req_fuel;
    /* accel enrichment defaults (tps %/s, pct, cycles, taper pct) and IAC */
    double accel_tps_threshold, accel_enrichment_pct, accel_taper_pct; int accel_duration_cycles;
    double iac_cold_start_pct, iac_warm_idle_pct, iac_warm_threshold_c;
    lt_basemap_scalar *scalars; size_t n_scalars;   /* reqFuel, nCylinders, injType, sparkMode, stoich ... */
} lt_basemap;
void lt_basemap_generate(const lt_engine_spec *spec, lt_basemap *out);
void lt_basemap_free(lt_basemap *m);

/* Write the generated tables/scalars/curves into page images:
 *  - the VE table (role LT_ROLE_VE), ignition (LT_ROLE_IGNITION), AFR target (LT_ROLE_AFR_TARGET),
 *    resampled onto the table's own axes if its size differs (lt_tbl_rebin), axes overwritten
 *    when `write_axes`;
 *  - scalar constants whose names appear in m->scalars (skipped when the INI lacks them).
 * Returns number of constants written or <0. Report of what was written goes to *log (optional, malloc'd). */
int lt_basemap_apply(const lt_basemap *m, const lt_ini *ini, lt_pagesrc *dst, lt_bool write_axes,
                     char **log);

#endif /* LT_BASEMAP_H */
