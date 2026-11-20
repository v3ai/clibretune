/* calc_logic.h - the maths of Tools > Calculators (feat_calc.c): two-point linear calibration,
 * unit conversions (temperature, pressure, AFR / lambda, speed, fuel flow, injector flow at
 * another pressure) and injector sizing (required size from power and BSFC, duty cycle).
 * Pure functions, no UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_CALC_LOGIC_H
#define LT_CALC_LOGIC_H

#include "../lt.h"

#define LT_CALC_LB_G 453.59237          /* grams per pound */

/* ---- two-point linear calibration: y = slope * x + offset ---- */
/* LT_FALSE when x1 == x2 (no line through the points) */
lt_bool lt_calc_linear(double x1, double y1, double x2, double y2, double *slope, double *offset);
double  lt_calc_linear_y(double slope, double offset, double x);
/* inverse (value -> input, e.g. engineering value -> volts); LT_FALSE when slope is 0 */
lt_bool lt_calc_linear_x(double slope, double offset, double y, double *x);

/* ---- unit conversion ---- */
enum { LT_CALC_TEMP_C = 0, LT_CALC_TEMP_F, LT_CALC_TEMP_K, LT_CALC_TEMP_N };
enum { LT_CALC_P_KPA = 0, LT_CALC_P_PSI, LT_CALC_P_BAR, LT_CALC_P_INHG, LT_CALC_P_ATM, LT_CALC_P_N };
enum { LT_CALC_V_KMH = 0, LT_CALC_V_MPH, LT_CALC_V_MS, LT_CALC_V_N };
enum { LT_CALC_F_CCMIN = 0, LT_CALC_F_LBHR, LT_CALC_F_GS, LT_CALC_F_N };
double lt_calc_temp(double v, int from, int to);
double lt_calc_pressure(double v, int from, int to);
double lt_calc_speed(double v, int from, int to);
/* fuel flow; density in g/cc (0.72 gives the usual 10.5 cc/min per lb/hr for gasoline) */
double lt_calc_flow(double v, int from, int to, double density_g_cc);
const char *lt_calc_temp_unit(int u);
const char *lt_calc_pressure_unit(int u);
const char *lt_calc_speed_unit(int u);
const char *lt_calc_flow_unit(int u);

/* fuels with their stoichiometric AFR and density */
typedef struct lt_calc_fuel { const char *name; double stoich; double density; } lt_calc_fuel;
int lt_calc_fuel_count(void);
const lt_calc_fuel *lt_calc_fuel_get(int i);    /* 0 = gasoline; NULL when out of range */
double lt_calc_afr_to_lambda(double afr, double stoich);
double lt_calc_lambda_to_afr(double lambda, double stoich);
/* injector flow at another fuel pressure: flow * sqrt(new / rated) */
double lt_calc_injector_at_pressure(double flow, double rated_pressure, double new_pressure);

/* ---- injector sizing ---- */
/* required static flow per injector in lb/hr: hp * bsfc / (injectors * duty); duty 0..1 */
double lt_calc_injector_required(double hp, double bsfc, int injectors, double duty);
/* duty cycle 0..1 the engine needs with injectors of flow_lbhr each */
double lt_calc_injector_duty(double hp, double bsfc, int injectors, double flow_lbhr);
/* power the injectors support at `duty` */
double lt_calc_injector_max_hp(double flow_lbhr, double bsfc, int injectors, double duty);
/* duty cycle 0..1 from pulse width (ms) and rpm: 4-stroke one pulse every two turns */
double lt_calc_duty_from_pw(double pw_ms, double rpm, int two_stroke);

#endif
