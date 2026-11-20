/* test_calc.c - Tools > Calculators math, the table trail fade timing and the backend beep hook.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/app/calc_logic.h"
#include "../src/app/trail.h"
#include "../src/gui/gui.h"

LT_TEST(calc_linear)
{
    double m = 0.0, b = 0.0, x = 0.0;
    /* 0.5 V -> 0 kPa, 4.5 V -> 300 kPa */
    LT_CHECK(lt_calc_linear(0.5, 0.0, 4.5, 300.0, &m, &b));
    LT_CHECK_NEAR(m, 75.0, 1e-9);
    LT_CHECK_NEAR(b, -37.5, 1e-9);
    LT_CHECK_NEAR(lt_calc_linear_y(m, b, 2.5), 150.0, 1e-9);
    LT_CHECK(lt_calc_linear_x(m, b, 150.0, &x));
    LT_CHECK_NEAR(x, 2.5, 1e-9);
    LT_CHECK(!lt_calc_linear(1.0, 0.0, 1.0, 5.0, &m, &b));     /* same x twice */
    LT_CHECK(!lt_calc_linear_x(0.0, 3.0, 1.0, &x));            /* flat line */
}

LT_TEST(calc_units)
{
    LT_CHECK_NEAR(lt_calc_temp(100.0, LT_CALC_TEMP_C, LT_CALC_TEMP_F), 212.0, 1e-9);
    LT_CHECK_NEAR(lt_calc_temp(32.0, LT_CALC_TEMP_F, LT_CALC_TEMP_K), 273.15, 1e-9);
    LT_CHECK_NEAR(lt_calc_temp(0.0, LT_CALC_TEMP_K, LT_CALC_TEMP_C), -273.15, 1e-9);
    LT_CHECK_NEAR(lt_calc_pressure(101.325, LT_CALC_P_KPA, LT_CALC_P_ATM), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_calc_pressure(1.0, LT_CALC_P_BAR, LT_CALC_P_PSI), 14.5038, 1e-3);
    LT_CHECK_NEAR(lt_calc_pressure(29.92, LT_CALC_P_INHG, LT_CALC_P_KPA), 101.32, 0.01);
    LT_CHECK_NEAR(lt_calc_pressure(lt_calc_pressure(250.0, LT_CALC_P_KPA, LT_CALC_P_PSI), LT_CALC_P_PSI, LT_CALC_P_KPA),
                  250.0, 1e-9);
    LT_CHECK_NEAR(lt_calc_speed(100.0, LT_CALC_V_KMH, LT_CALC_V_MPH), 62.137, 1e-3);
    LT_CHECK_NEAR(lt_calc_speed(36.0, LT_CALC_V_KMH, LT_CALC_V_MS), 10.0, 1e-9);
    /* 100 cc/min of gasoline (0.74 g/cc) = 4440 g/h = 9.79 lb/hr */
    LT_CHECK_NEAR(lt_calc_flow(100.0, LT_CALC_F_CCMIN, LT_CALC_F_LBHR, 0.74), 9.789, 1e-3);
    LT_CHECK_NEAR(lt_calc_flow(60.0, LT_CALC_F_CCMIN, LT_CALC_F_GS, 1.0), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_calc_flow(lt_calc_flow(550.0, LT_CALC_F_CCMIN, LT_CALC_F_LBHR, 0.74), LT_CALC_F_LBHR, LT_CALC_F_CCMIN,
                               0.74), 550.0, 1e-9);
    LT_CHECK(strcmp(lt_calc_pressure_unit(LT_CALC_P_PSI), "psi") == 0);
}

LT_TEST(calc_fuel_and_injectors)
{
    const lt_calc_fuel *g = lt_calc_fuel_get(0), *e85 = NULL;
    int i;
    LT_CHECK(g != NULL && g->stoich > 14.6 && g->stoich < 14.8);
    for (i = 0; i < lt_calc_fuel_count(); i++) {
        if (strcmp(lt_calc_fuel_get(i)->name, "E85") == 0) { e85 = lt_calc_fuel_get(i); }
    }
    LT_CHECK(e85 != NULL && e85->stoich < 10.0);
    LT_CHECK(lt_calc_fuel_get(lt_calc_fuel_count()) == NULL);
    LT_CHECK_NEAR(lt_calc_afr_to_lambda(14.7, 14.7), 1.0, 1e-12);
    LT_CHECK_NEAR(lt_calc_lambda_to_afr(0.8, 14.7), 11.76, 1e-9);
    /* 4x 440 cc at 3 bar on 4 bar: * sqrt(4/3) */
    LT_CHECK_NEAR(lt_calc_injector_at_pressure(440.0, 3.0, 4.0), 508.07, 0.01);
    /* 400 hp, BSFC 0.5, 4 injectors, 80 % duty -> 62.5 lb/hr */
    LT_CHECK_NEAR(lt_calc_injector_required(400.0, 0.5, 4, 0.8), 62.5, 1e-9);
    LT_CHECK_NEAR(lt_calc_injector_duty(400.0, 0.5, 4, 62.5), 0.8, 1e-9);
    LT_CHECK_NEAR(lt_calc_injector_max_hp(62.5, 0.5, 4, 0.8), 400.0, 1e-9);
    LT_CHECK_NEAR(lt_calc_injector_required(400.0, 0.5, 0, 0.8), 0.0, 1e-12);
    /* 10 ms at 6000 rpm, 4-stroke: one 20 ms cycle -> 50 %; 2-stroke 100 % */
    LT_CHECK_NEAR(lt_calc_duty_from_pw(10.0, 6000.0, 0), 0.5, 1e-9);
    LT_CHECK_NEAR(lt_calc_duty_from_pw(10.0, 6000.0, 1), 1.0, 1e-9);
}

LT_TEST(calc_expression_on_demo)
{
    char buf[512], out[128];
    double d = 0.0;
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_feat_calc_eval(app, "14.7 * 0.85 + pow(2, 3)", out, sizeof(out)) == 0);
    LT_CHECK(lt_parse_double(out, &d));
    LT_CHECK_NEAR(d, 20.495, 1e-6);
    LT_CHECK(lt_feat_calc_eval(app, "rpmHardLimit + 0", out, sizeof(out)) == 0);     /* a constant */
    LT_CHECK(lt_feat_calc_eval(app, "RPMValue >= 0", out, sizeof(out)) == 0);         /* a live channel */
    LT_CHECK(lt_feat_calc_eval(app, "1 +", out, sizeof(out)) == -1);
    LT_CHECK(out[0] != '\0');
    LT_CHECK(lt_app_dialog_push(app, "calc", "", "Calculators") != NULL);
    lt_app_screenshot(app, "build/lt_test_calc.ppm", 1000, 760, 2);
    lt_app_free(app);
}

LT_TEST(trail_fade_timing)
{
    lt_trail tr;
    int i;
    lt_trail_clear(&tr);
    lt_trail_push(&tr, 1, 1, 0.0);
    lt_trail_push(&tr, 1, 1, 500.0);           /* did not move: head refreshed */
    LT_CHECK_INT(tr.n, 1);
    LT_CHECK_NEAR(tr.t[0], 500.0, 1e-9);
    lt_trail_push(&tr, 1, 2, 1000.0);
    lt_trail_push(&tr, 2, 2, 2000.0);
    LT_CHECK_INT(tr.n, 3);
    /* fade 2 s at t = 2.5 s: (2,2) age 0.5 -> 0.75, (1,2) age 1.5 -> 0.25, (1,1) age 2.0 -> gone */
    LT_CHECK_NEAR(lt_trail_alpha(&tr, 0, 2500.0, 2), 0.75, 1e-9);
    LT_CHECK_NEAR(lt_trail_alpha(&tr, 1, 2500.0, 2), 0.25, 1e-9);
    LT_CHECK_NEAR(lt_trail_alpha(&tr, 2, 2500.0, 2), 0.0, 1e-9);
    lt_trail_prune(&tr, 2500.0, 2);
    LT_CHECK_INT(tr.n, 2);
    lt_trail_prune(&tr, 4000.0, 2);
    LT_CHECK_INT(tr.n, 0);
    /* fade 0: never by age, the last LT_TRAIL_KEEP cells dimmer by position */
    lt_trail_clear(&tr);
    for (i = 0; i < 30; i++) { lt_trail_push(&tr, i, 0, (double)i); }
    lt_trail_prune(&tr, 1e9, 0);
    LT_CHECK_INT(tr.n, LT_TRAIL_KEEP);
    LT_CHECK_NEAR(lt_trail_alpha(&tr, 0, 1e9, 0), 1.0, 1e-9);
    LT_CHECK(lt_trail_alpha(&tr, LT_TRAIL_KEEP - 1, 1e9, 0) > 0.1);
    LT_CHECK(lt_trail_alpha(&tr, LT_TRAIL_KEEP - 1, 1e9, 0) < lt_trail_alpha(&tr, 1, 1e9, 0));
    LT_CHECK_NEAR(lt_trail_alpha(&tr, LT_TRAIL_KEEP + 3, 1e9, 0), 0.0, 1e-9);
    /* a full trail drops the oldest */
    lt_trail_clear(&tr);
    for (i = 0; i < LT_TRAIL_MAX + 5; i++) { lt_trail_push(&tr, i, 0, 0.0); }
    LT_CHECK_INT(tr.n, LT_TRAIL_MAX);
    LT_CHECK_INT(tr.r[0], LT_TRAIL_MAX + 4);
}

static int g_beeps;
static void count_beep(lt_backend *b) { LT_UNUSED(b); g_beeps++; }

LT_TEST(beep_hook)
{
    lt_backend b;
    LT_CHECK_INT(lt_backend_beep(NULL), 0);
    memset(&b, 0, sizeof(b));
    LT_CHECK_INT(lt_backend_beep(&b), 0);          /* offscreen: no hook, no crash */
    b.beep = count_beep;
    LT_CHECK_INT(lt_backend_beep(&b), 1);
    LT_CHECK_INT(g_beeps, 1);
}

int main(void)
{
    LT_RUN(calc_linear);
    LT_RUN(calc_units);
    LT_RUN(calc_fuel_and_injectors);
    LT_RUN(calc_expression_on_demo);
    LT_RUN(trail_fade_timing);
    LT_RUN(beep_hook);
    LT_TEST_MAIN_END
}
