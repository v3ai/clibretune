/* dlg_perf.c - performance / economy calculator maths (see dlg_perf.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "dlg_perf.h"

#define PERF_PI 3.14159265358979323846

void lt_perf_specs_default(lt_perf_specs *s)
{
    memset(s, 0, sizeof(*s));
    s->mass_kg = 3000.0 * LT_PERF_LBS_TO_KG;
    s->frontal_m2 = 22.0 * LT_PERF_SQFT_TO_SQM;
    s->cd = 0.35;
    s->drivetrain_loss_pct = 15.0;
    s->tire_diameter_in = 26.0;
    s->final_drive = 3.55;
    s->gears[0] = 2.97; s->gears[1] = 2.07; s->gears[2] = 1.43; s->gears[3] = 1.00; s->gears[4] = 0.84;
    s->ngears = 5;
    s->injector_cc_min = 440.0;
    s->n_injectors = 4;
}

double lt_perf_drag_force(double v_ms, double frontal_m2, double cd)
{
    return 0.5 * LT_PERF_AIR_DENSITY * v_ms * v_ms * cd * frontal_m2;
}

double lt_perf_rolling_force(double mass_kg)
{
    return LT_PERF_ROLLING_CRR * mass_kg * LT_PERF_GRAVITY;
}

double lt_perf_power_w(const lt_perf_specs *s, double v_ms, double accel_ms2)
{
    double force = s->mass_kg * accel_ms2 + lt_perf_drag_force(v_ms, s->frontal_m2, s->cd) +
                   lt_perf_rolling_force(s->mass_kg);
    double keep = 1.0 - s->drivetrain_loss_pct / 100.0;
    if (keep < 0.01) { keep = 0.01; }
    return force * v_ms / keep;
}

double lt_perf_tq_nm(double power_w, double rpm)
{
    double w = 2.0 * PERF_PI * rpm / 60.0;
    return w > 0.0 ? power_w / w : 0.0;
}

void lt_perf_estimates(double hp, double weight_lbs, double *et, double *trap_mph, double *zero60)
{
    if (!(hp > 0.0) || !(weight_lbs > 0.0)) { *et = 0.0; *trap_mph = 0.0; *zero60 = 0.0; return; }
    *et = 5.825 * pow(weight_lbs / hp, 1.0 / 3.0);
    *trap_mph = 234.0 * pow(hp / weight_lbs, 1.0 / 3.0);
    *zero60 = 3.7 * pow(weight_lbs / hp, 0.42);
}

void lt_perf_compute(const lt_perf_specs *s, double v_ms, double accel_ms2, double rpm, lt_perf_result *out)
{
    double pw = lt_perf_power_w(s, v_ms, accel_ms2), tq = lt_perf_tq_nm(pw, rpm);
    memset(out, 0, sizeof(*out));
    out->hp = pw > 0.0 ? pw / LT_PERF_HP_W : 0.0;
    out->tq_nm = tq > 0.0 ? tq : 0.0;
    out->tq_lbft = out->tq_nm * LT_PERF_NM_TO_LBFT;
    out->drag_n = lt_perf_drag_force(v_ms, s->frontal_m2, s->cd);
    out->rolling_n = lt_perf_rolling_force(s->mass_kg);
    lt_perf_estimates(out->hp, s->mass_kg / LT_PERF_LBS_TO_KG, &out->quarter_s, &out->quarter_mph, &out->zero60_s);
}

double lt_perf_speed_at_rpm(const lt_perf_specs *s, double gear, double rpm)
{
    double ratio = gear * s->final_drive, circ_m = PERF_PI * s->tire_diameter_in * 0.0254;
    if (!(ratio > 0.0) || !(circ_m > 0.0)) { return 0.0; }
    return rpm / ratio * circ_m / 60.0;
}

double lt_perf_rpm_at_speed(const lt_perf_specs *s, double gear, double v_ms)
{
    double ratio = gear * s->final_drive, circ_m = PERF_PI * s->tire_diameter_in * 0.0254;
    if (!(ratio > 0.0) || !(circ_m > 0.0)) { return 0.0; }
    return v_ms * 60.0 / circ_m * ratio;
}

int lt_perf_parse_gears(const char *text, double *out, int cap)
{
    const char *p = text;
    int n = 0;
    while (*p != '\0') {
        char tok[32];
        size_t k = 0;
        double v = 0.0;
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '\t') { p++; }
        if (*p == '\0') { break; }
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != ';' && *p != '\t' && k + 1 < sizeof(tok)) { tok[k++] = *p++; }
        tok[k] = '\0';
        if (!lt_parse_double(tok, &v) || !(v > 0.0) || n >= cap) { return -1; }
        out[n++] = v;
    }
    return n;
}

double lt_perf_fuel_cc_min(double pw_ms, double rpm, int n_injectors, double inj_cc_min, int two_stroke)
{
    double events = (two_stroke ? rpm : rpm / 2.0) * (double)(n_injectors > 0 ? n_injectors : 0);
    if (!(pw_ms > 0.0) || !(rpm > 0.0) || !(inj_cc_min > 0.0)) { return 0.0; }
    return events * (pw_ms / 1000.0) * (inj_cc_min / 60.0);
}

double lt_perf_mpg(double v_ms, double cc_min)
{
    double miles_min = v_ms / LT_PERF_MPH_TO_MS / 60.0, gal_min = cc_min / LT_PERF_CC_PER_GAL_US;
    return cc_min > 0.0 && v_ms > 0.0 ? miles_min / gal_min : 0.0;
}

double lt_perf_kpl(double v_ms, double cc_min)
{
    double km_min = v_ms * 60.0 / 1000.0, l_min = cc_min / 1000.0;
    return cc_min > 0.0 && v_ms > 0.0 ? km_min / l_min : 0.0;
}

double lt_perf_l100(double v_ms, double cc_min)
{
    double kpl = lt_perf_kpl(v_ms, cc_min);
    return kpl > 0.0 ? 100.0 / kpl : 0.0;
}

double lt_perf_accel(double v0_ms, double v1_ms, double dt_s)
{
    return dt_s > 0.0 ? (v1_ms - v0_ms) / dt_s : 0.0;
}
