/* test_calib_build.c - calibration dialog builders (thermistor bins, AFR curves, corrections)
 * and the std_ms2gentherm / std_ms2geno2 targets. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"

LT_TEST(calib_units)
{
    LT_CHECK_NEAR(lt_calib_c_to_f(100.0), 212.0, 1e-9);
    LT_CHECK_NEAR(lt_calib_f_to_c(-40.0), -40.0, 1e-9);
    LT_CHECK_NEAR(lt_calib_f_to_c(lt_calib_c_to_f(37.5)), 37.5, 1e-9);
}

LT_TEST(calib_therm_curve)
{
    double t[3] = { -40, 30, 99 }, r[3] = { 100700, 2238, 177 }, out[32], leg[32], bins[32];
    lt_therm_fit f;
    int i;

    LT_CHECK(lt_calib_therm_curve(2490, t, r, LT_TRUE, out) == 0);
    LT_CHECK(lt_therm_fit_points(&f, 2490, t, r) == 0);
    lt_cal_temperature_bins(LT_TRUE, bins);
    for (i = 0; i < 32; i++) LT_CHECK_NEAR(out[i], lt_therm_temp_at_adc(&f, (int)bins[i]), 1e-6);
    LT_CHECK(lt_calib_therm_curve(2490, t, r, LT_FALSE, leg) == 0);
    LT_CHECK(fabs(leg[5] - out[5]) > 1e-6);      /* legacy bins x*32, modern x*33 */
    LT_CHECK(out[2] > out[30]);                  /* NTC: higher ADC counts read colder */
    LT_CHECK(lt_calib_therm_curve(0, t, r, LT_TRUE, out) == LT_EDATA);
    r[2] = -1;
    LT_CHECK(lt_calib_therm_curve(2490, t, r, LT_TRUE, out) == LT_EDATA);
}

LT_TEST(calib_afr_build)
{
    static double afr[1024];
    double lin[4] = { 0, 10, 5, 20 }, corr[4] = { 14.2, 14.7, 19.0, 18.25 };
    char desc[200];
    lt_u8 wire[1024];

    LT_CHECK(lt_calib_afr_build(NULL, NULL, NULL, lin, 0, corr, afr, desc, sizeof desc) == 0);
    LT_CHECK_NEAR(afr[0], 10.0, 1e-9);
    LT_CHECK_NEAR(afr[1023], 20.0, 1e-9);
    LT_CHECK(!lt_calib_afr_clips(afr));
    lt_cal_encode_o2(afr, wire);
    LT_CHECK(wire[0] == 100 && wire[1023] == 200);
    LT_CHECK(lt_calib_afr_build(NULL, NULL, NULL, lin, 1, corr, afr, desc, sizeof desc) == 0);
    LT_CHECK_NEAR(afr[0], 10.5, 1e-9);           /* single point: offset only */
    LT_CHECK(strstr(desc, "corrected") != NULL);
    LT_CHECK(lt_calib_afr_build(NULL, NULL, NULL, lin, 2, corr, afr, desc, sizeof desc) == 0);
    LT_CHECK(fabs(afr[1023] - 20.0) > 0.01);     /* two points: gain + offset */
    lin[3] = 40;
    LT_CHECK(lt_calib_afr_build(NULL, NULL, NULL, lin, 0, corr, afr, desc, sizeof desc) == 0);
    LT_CHECK(lt_calib_afr_clips(afr));
    lin[2] = 0;
    LT_CHECK(lt_calib_afr_build(NULL, NULL, NULL, lin, 0, corr, afr, desc, sizeof desc) == LT_EDATA);
    LT_CHECK(lt_calib_afr_build(NULL, "x", NULL, lin, 0, corr, afr, desc, sizeof desc) == LT_ENOENT);
}

LT_TEST(calib_afr_preset)
{
    static double afr[1024];
    double lin[4] = { 0, 10, 5, 20 };
    char desc[200];
    lt_ini *ini = lt_ini_load_file("tests/fixtures/ini/speeduino_202501_na6.ini", NULL);
    const lt_ini_ref_table *t;
    lt_inc_cache cache;

    LT_CHECK(ini != NULL);
    t = lt_ini_find_ref_table(ini, "std_ms2geno2");
    LT_CHECK(t != NULL);
    lt_inc_cache_init(&cache);
    LT_CHECK(t->n_solutions > 0);
    LT_CHECK(lt_calib_afr_build(t, "14Point7", &cache, lin, 0, lin, afr, desc, sizeof desc) == 0);
    LT_CHECK(afr[1023] > afr[0]);
    lt_inc_cache_free(&cache);
    lt_ini_free(ini);
}

LT_TEST(calib_targets)
{
    lt_app *app = lt_app_new();
    LT_CHECK(lt_ext_has_target(app, "std_ms2gentherm"));
    LT_CHECK(lt_ext_has_target(app, "std_ms2geno2"));
    LT_CHECK(lt_app_open_ini(app, "tests/fixtures/ini/speeduino_202501_na6.ini") == 0);
    LT_CHECK(lt_app_open_target(app, "std_ms2gentherm", "x") == 0);
    LT_CHECK(lt_app_open_target(app, "std_ms2geno2", "x") == 0);
    LT_CHECK(app->n_dialogs == 2);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(calib_units);
    LT_RUN(calib_therm_curve);
    LT_RUN(calib_afr_build);
    LT_RUN(calib_afr_preset);
    LT_RUN(calib_targets);
    LT_TEST_MAIN_END
}
