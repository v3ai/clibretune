/* dlg_perf.h - performance / economy calculator maths (G-35, no UI): wheel/engine power and torque
 * from a speed / acceleration / rpm sample, drag and rolling resistance, acceleration estimates,
 * gearing (rpm <-> speed) and fuel economy from injector pulse width. Mirrors the original's
 * PerformanceFieldsDialog. Unit-tested in tests/test_perf.c; the dialog is feat_perf.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DLG_PERF_H
#define LT_DLG_PERF_H

#include "../lt.h"

LT_BEGIN_DECLS

#define LT_PERF_AIR_DENSITY 1.225      /* kg/m3 at sea level */
#define LT_PERF_ROLLING_CRR 0.015
#define LT_PERF_GRAVITY 9.81
#define LT_PERF_LBS_TO_KG 0.453592
#define LT_PERF_SQFT_TO_SQM 0.092903
#define LT_PERF_MPH_TO_MS 0.44704
#define LT_PERF_HP_W 745.7
#define LT_PERF_NM_TO_LBFT 0.737562
#define LT_PERF_MAX_GEARS 10
#define LT_PERF_CC_PER_GAL_US 3785.411784

/* vehicle specifications in SI units */
typedef struct lt_perf_specs {
    double mass_kg, frontal_m2, cd;
    double drivetrain_loss_pct;        /* 0..99 */
    double tire_diameter_in;
    double final_drive;
    double gears[LT_PERF_MAX_GEARS]; int ngears;
    double injector_cc_min;            /* flow per injector */
    int    n_injectors;
} lt_perf_specs;

typedef struct lt_perf_result {
    double hp, tq_nm, tq_lbft;
    double drag_n, rolling_n;
    double quarter_s, quarter_mph, zero60_s;
} lt_perf_result;

void lt_perf_specs_default(lt_perf_specs *s);   /* the original's defaults: 3000 lb, 22 sq ft, Cd 0.35 ... */
double lt_perf_drag_force(double v_ms, double frontal_m2, double cd);
double lt_perf_rolling_force(double mass_kg);
/* engine power in watts from the wheel force balance (m*a + drag + rolling) * v, over (1 - loss) */
double lt_perf_power_w(const lt_perf_specs *s, double v_ms, double accel_ms2);
double lt_perf_tq_nm(double power_w, double rpm);
/* empirical ET 5.825 (W/HP)^(1/3), trap 234 (HP/W)^(1/3) mph, 0-60 3.7 (W/HP)^0.42 (weight in lbs);
 * all 0 when hp or weight is not positive */
void   lt_perf_estimates(double hp, double weight_lbs, double *et, double *trap_mph, double *zero60);
/* the whole result for one sample (negative power/torque clamped to 0) */
void   lt_perf_compute(const lt_perf_specs *s, double v_ms, double accel_ms2, double rpm, lt_perf_result *out);
/* gearing: vehicle speed (m/s) at an rpm in a gear ratio (0 when a ratio or the tyre is 0) and back */
double lt_perf_speed_at_rpm(const lt_perf_specs *s, double gear, double rpm);
double lt_perf_rpm_at_speed(const lt_perf_specs *s, double gear, double v_ms);
/* "2.97, 2.07 1.43" -> ratios (> 0, at most cap); count, or -1 on a bad token */
int    lt_perf_parse_gears(const char *text, double *out, int cap);

/* fuel economy */
/* cc/min of all injectors from the pulse width (ms): events per minute = rpm / 2 * injectors for a
 * four-stroke (rpm * injectors for a two-stroke) */
double lt_perf_fuel_cc_min(double pw_ms, double rpm, int n_injectors, double inj_cc_min, int two_stroke);
/* distance per fuel: mpg (US) / km per litre / litres per 100 km from speed (m/s) and cc/min;
 * 0 when no fuel flows */
double lt_perf_mpg(double v_ms, double cc_min);
double lt_perf_kpl(double v_ms, double cc_min);
double lt_perf_l100(double v_ms, double cc_min);
/* acceleration (m/s2) from two speed samples dt seconds apart; 0 when dt <= 0 */
double lt_perf_accel(double v0_ms, double v1_ms, double dt_s);

LT_END_DECLS

#endif
