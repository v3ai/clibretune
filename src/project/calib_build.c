/* calib_build.c - builds the curves the sensor calibration dialogs preview and write:
 * thermistor fit sampled at the ECU's ADC bins, and the AFR curve (INI preset or two-point
 * linear, optional reference correction). Pure; no UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "proj_priv.h"
#include "../proto/proto.h"

double lt_calib_c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }
double lt_calib_f_to_c(double f) { return (f - 32.0) * 5.0 / 9.0; }

int lt_calib_therm_curve(double bias, const double temp_c[3], const double ohms[3],
                         lt_bool modern, double temps[32])
{
    lt_therm_fit fit;
    double bins[32];
    int rc;

    if (!(bias > 0)) {
        lt_err_set("the bias resistor must be positive (was %g ohm)", bias);
        return LT_EDATA;
    }
    rc = lt_therm_fit_points(&fit, bias, temp_c, ohms);
    if (rc != LT_OK) return rc;
    lt_cal_temperature_bins(modern, bins);
    lt_therm_sample_at_bins(&fit, bins, temps);
    return LT_OK;
}

lt_bool lt_calib_afr_clips(const double afr[1024])
{
    int i;
    for (i = 0; i < LT_CALIB_ADC_COUNTS; i++)
        if (afr[i] < 0.0 || afr[i] > 25.5) return LT_TRUE;
    return LT_FALSE;
}

int lt_calib_afr_build(const lt_ini_ref_table *t, const char *preset, lt_inc_cache *cache,
                       const double lin[4], int ncorr, const double corr[4], double afr[1024],
                       char *desc, size_t cap)
{
    char tmp[160];
    int rc;

    desc[0] = '\0';
    if (preset != NULL) {
        if (t == NULL) {
            lt_err_set("this INI declares no AFR calibration table");
            return LT_ENOENT;
        }
        rc = lt_calib_solution_curve(t, preset, cache, afr);
        if (rc != LT_OK) return rc;
        lt_snprintf(desc, cap, "%.2f-%.2f AFR over 0-5 V", afr[0], afr[1023]);
    } else {
        lt_linear_wb w;
        rc = lt_linear_wb_new(&w, lin[0], lin[1], lin[2], lin[3]);
        if (rc != LT_OK) return rc;
        lt_linear_wb_curve(&w, afr);
        lt_linear_wb_describe(&w, desc, cap);
    }
    if (ncorr > 0) {
        lt_afr_corr c;
        rc = lt_afr_corr_from_points(&c, corr, ncorr > 2 ? 2 : ncorr);
        if (rc != LT_OK) return rc;
        lt_afr_corr_apply(&c, afr, LT_CALIB_ADC_COUNTS);
        lt_afr_corr_describe(&c, tmp, sizeof tmp);
        if (desc[0] != '\0') lt_strlcat(desc, "; ", cap);
        lt_strlcat(desc, tmp, cap);
    }
    return LT_OK;
}
