/* calc_logic.c - calculator maths (see calc_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include "calc_logic.h"

lt_bool lt_calc_linear(double x1, double y1, double x2, double y2, double *slope, double *offset)
{
    double m;
    if (x1 == x2 || !lt_isfinite(x1) || !lt_isfinite(x2) || !lt_isfinite(y1) || !lt_isfinite(y2)) {
        return LT_FALSE;
    }
    m = (y2 - y1) / (x2 - x1);
    *slope = m;
    *offset = y1 - m * x1;
    return LT_TRUE;
}

double lt_calc_linear_y(double slope, double offset, double x)
{
    return slope * x + offset;
}

lt_bool lt_calc_linear_x(double slope, double offset, double y, double *x)
{
    if (slope == 0.0) { return LT_FALSE; }
    *x = (y - offset) / slope;
    return LT_TRUE;
}

/* ---- units ---- */
double lt_calc_temp(double v, int from, int to)
{
    double c;
    switch (from) {
    case LT_CALC_TEMP_F: c = (v - 32.0) * 5.0 / 9.0; break;
    case LT_CALC_TEMP_K: c = v - 273.15; break;
    default: c = v; break;
    }
    switch (to) {
    case LT_CALC_TEMP_F: return c * 9.0 / 5.0 + 32.0;
    case LT_CALC_TEMP_K: return c + 273.15;
    default: return c;
    }
}

/* kPa per unit */
static double p_factor(int u)
{
    switch (u) {
    case LT_CALC_P_PSI: return 6.894757293168;
    case LT_CALC_P_BAR: return 100.0;
    case LT_CALC_P_INHG: return 3.386388640341;
    case LT_CALC_P_ATM: return 101.325;
    default: return 1.0;
    }
}

double lt_calc_pressure(double v, int from, int to)
{
    return v * p_factor(from) / p_factor(to);
}

/* m/s per unit */
static double v_factor(int u)
{
    switch (u) {
    case LT_CALC_V_MPH: return 0.44704;
    case LT_CALC_V_MS: return 1.0;
    default: return 1.0 / 3.6;
    }
}

double lt_calc_speed(double v, int from, int to)
{
    return v * v_factor(from) / v_factor(to);
}

/* grams per minute per unit */
static double f_factor(int u, double density)
{
    switch (u) {
    case LT_CALC_F_LBHR: return LT_CALC_LB_G / 60.0;
    case LT_CALC_F_GS: return 60.0;
    default: return density;
    }
}

double lt_calc_flow(double v, int from, int to, double density_g_cc)
{
    double d = density_g_cc > 0.0 ? density_g_cc : 0.72;
    return v * f_factor(from, d) / f_factor(to, d);
}

const char *lt_calc_temp_unit(int u)
{
    static const char *const n[LT_CALC_TEMP_N] = { "\302\260C", "\302\260F", "K" };
    return u >= 0 && u < LT_CALC_TEMP_N ? n[u] : "";
}

const char *lt_calc_pressure_unit(int u)
{
    static const char *const n[LT_CALC_P_N] = { "kPa", "psi", "bar", "inHg", "atm" };
    return u >= 0 && u < LT_CALC_P_N ? n[u] : "";
}

const char *lt_calc_speed_unit(int u)
{
    static const char *const n[LT_CALC_V_N] = { "km/h", "mph", "m/s" };
    return u >= 0 && u < LT_CALC_V_N ? n[u] : "";
}

const char *lt_calc_flow_unit(int u)
{
    static const char *const n[LT_CALC_F_N] = { "cc/min", "lb/hr", "g/s" };
    return u >= 0 && u < LT_CALC_F_N ? n[u] : "";
}

/* stoichiometric ratios and typical densities (g/cc at about 15 C) */
static const lt_calc_fuel g_fuels[] = {
    { "Gasoline", 14.7, 0.74 },
    { "E10", 14.08, 0.75 },
    { "E85", 9.765, 0.78 },
    { "Ethanol (E100)", 9.0, 0.789 },
    { "Methanol", 6.47, 0.792 },
    { "Diesel", 14.5, 0.83 },
    { "LPG (propane)", 15.67, 0.51 },
    { "CNG (methane)", 17.2, 0.42 }
};

int lt_calc_fuel_count(void)
{
    return (int)LT_COUNTOF(g_fuels);
}

const lt_calc_fuel *lt_calc_fuel_get(int i)
{
    return i >= 0 && i < (int)LT_COUNTOF(g_fuels) ? &g_fuels[i] : NULL;
}

double lt_calc_afr_to_lambda(double afr, double stoich)
{
    return stoich > 0.0 ? afr / stoich : 0.0;
}

double lt_calc_lambda_to_afr(double lambda, double stoich)
{
    return lambda * stoich;
}

double lt_calc_injector_at_pressure(double flow, double rated_pressure, double new_pressure)
{
    if (rated_pressure <= 0.0 || new_pressure < 0.0) { return 0.0; }
    return flow * sqrt(new_pressure / rated_pressure);
}

/* ---- injector sizing ---- */
double lt_calc_injector_required(double hp, double bsfc, int injectors, double duty)
{
    if (injectors <= 0 || duty <= 0.0) { return 0.0; }
    return hp * bsfc / ((double)injectors * duty);
}

double lt_calc_injector_duty(double hp, double bsfc, int injectors, double flow_lbhr)
{
    if (injectors <= 0 || flow_lbhr <= 0.0) { return 0.0; }
    return hp * bsfc / ((double)injectors * flow_lbhr);
}

double lt_calc_injector_max_hp(double flow_lbhr, double bsfc, int injectors, double duty)
{
    if (bsfc <= 0.0) { return 0.0; }
    return flow_lbhr * (double)injectors * duty / bsfc;
}

double lt_calc_duty_from_pw(double pw_ms, double rpm, int two_stroke)
{
    double cycle_ms;
    if (rpm <= 0.0) { return 0.0; }
    cycle_ms = 60000.0 / rpm * (two_stroke ? 1.0 : 2.0);
    return pw_ms / cycle_ms;
}
