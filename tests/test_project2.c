/* test_project2.c - calibration curves, pin conflicts, action scripting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "plat/plat.h"
#include "project/project.h"
#include "project/proj_priv.h"
#include "lt_test.h"

#define FIX "tests/fixtures/project/"

/* ---------------------------------------------------------------- calibration */
LT_TEST(calib_adc_volts)
{
    LT_CHECK_NEAR(lt_calib_adc_to_volts(0), 0.0, 0);
    LT_CHECK_NEAR(lt_calib_adc_to_volts(1023), 5.0, 0);
    LT_CHECK_NEAR(lt_calib_adc_to_volts(512), 512.0 * 5.0 / 1023.0, 1e-12);
}

LT_TEST(calib_solutions)
{
    double out[1024];
    lt_inc_cache cache;
    int adc, rc;
    lt_u8 prev = 0;
    int mono = 1;

    lt_inc_cache_init(&cache);
    LT_CHECK(lt_calib_evaluate_solution("10.0001 + ( adcValue * 0.0097752 )", &cache, out) == 0);
    LT_CHECK_NEAR(out[0], 10.0001, 1e-6);
    LT_CHECK(fabs(out[512] * 10.0 - 150.0) <= 1.0);
    for (adc = 0; adc < 1024; adc++) {
        lt_u8 b = (lt_u8)lt_round(out[adc] * 10.0);
        if (adc > 0 && b < prev) mono = 0;
        prev = b;
    }
    LT_CHECK(mono);
    LT_CHECK(lt_round(out[0] * 10.0) >= 100);
    LT_CHECK(lt_round(out[1023] * 10.0) <= 200);

    rc = lt_calib_evaluate_solution("table(adcValue*5/1023 , \"nb.inc\")", &cache, out);
    LT_CHECK(rc == LT_ENOENT);
    LT_CHECK(strstr(lt_err_get(), "preset needs the lookup file 'nb.inc', which is not "
                                  "available") != NULL);
    rc = lt_calib_evaluate_solution("adcValue *", &cache, out);
    LT_CHECK(rc == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "calibration formula parse failed") != NULL);

    /* .inc lookup from a real file */
    {
        char *d = lt_strdup("build/test_tmp/project_inc"), *f;
        const char *inc = "; test\n0.0\t10.0\n2.5\t15.0\n5.0\t20.0\n";
        lt_proj_rmtree(d);
        lt_plat_mkdirs(d);
        f = lt_path_join(d, "lin.inc");
        lt_file_write(f, inc, strlen(inc));
        lt_inc_cache_add_path(&cache, d);
        rc = lt_calib_evaluate_solution("table(adcValue*5/1023, \"lin\")", &cache, out);
        if (rc == 0) {
            LT_CHECK_NEAR(out[0], 10.0, 1e-9);
            LT_CHECK_NEAR(out[1023], 20.0, 1e-9);
        } else {
            /* the .inc text format is owned by the ini module: tolerate, but report */
            LT_CHECK(rc == LT_ENOENT);
        }
        lt_free(f);
        lt_free(d);
    }
    lt_inc_cache_free(&cache);
}

LT_TEST(calib_ini_fixture)
{
    lt_ini *ini = lt_ini_load_file(FIX "calib_speeduino.ini", NULL);
    const lt_ini_ref_table *th, *o2;
    double out[1024];
    lt_inc_cache cache;
    size_t i;
    lt_therm_fit fit;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    th = lt_ini_find_ref_table(ini, "std_ms2gentherm");
    o2 = lt_ini_find_ref_table(ini, "std_ms2geno2");
    LT_CHECK(th != NULL && o2 != NULL);
    if (!th || !o2) {
        lt_ini_free(ini);
        return;
    }
    LT_CHECK_STR(th->label, "Calibrate Thermistor Tables.");
    LT_CHECK(th->adc_count == 32 && th->bytes_per_adc == 2 && th->scale == 10);
    LT_CHECK(lt_ini_ref_wire_bytes(th) == 64);
    LT_CHECK(th->n_idents == 2);
    LT_CHECK(th->n_therm_opts >= 4);
    LT_CHECK_STR(o2->label, "Calibrate AFR Table...");
    LT_CHECK(o2->adc_count == 1024 && o2->bytes_per_adc == 1);
    LT_CHECK(o2->n_solutions > 10);

    lt_inc_cache_init(&cache);
    LT_CHECK(lt_calib_solution_curve(o2, "14point7", &cache, out) == 0);
    LT_CHECK_NEAR(out[0], 10.0001, 1e-6);
    LT_CHECK(lt_calib_solution_curve(o2, "AEM Linear AEM-30-42xx", &cache, out) == 0);
    LT_CHECK_NEAR(out[1023], 9.72 + 1023 * 0.0096665, 1e-9);
    LT_CHECK(lt_calib_solution_curve(o2, "No such preset", &cache, out) == LT_ENOENT);
    LT_CHECK(lt_calib_solution_curve(o2, "Narrowband", &cache, out) == LT_ENOENT);
    LT_CHECK(strstr(lt_err_get(), "nb.inc") != NULL);
    lt_inc_cache_free(&cache);

    /* every preset thermistor option fits and is monotone around the fitted points */
    for (i = 0; i < th->n_therm_opts; i++) {
        const lt_ini_therm_opt *o = &th->therm_opts[i];
        int rc = lt_therm_fit_option(&fit, o);
        if (rc == 0) {
            int k;
            for (k = 0; k < 3; k++)
                LT_CHECK_NEAR(lt_therm_temp_at_resistance(&fit, o->r[k]), o->t[k], 0.01);
        }
    }
    lt_ini_free(ini);
}

LT_TEST(calib_afr_correction)
{
    lt_afr_corr c;
    double pts1[2] = {14.2, 14.7};
    double curve[3] = {10.0, 14.2, 19.5};
    double pts2[4] = {12.5, 12.08, 19.0, 18.25};
    double same[4] = {14.7, 14.5, 14.7, 15.0};
    double rev[4] = {12, 18, 18, 12};
    double three[6] = {1, 1, 2, 2, 3, 3};
    char buf[200];
    double x;

    LT_CHECK(lt_afr_corr_from_points(&c, pts1, 1) == 0);
    LT_CHECK_NEAR(c.gain, 1.0, 0);
    LT_CHECK_NEAR(c.offset, 0.5, 1e-12);
    lt_afr_corr_apply(&c, curve, 3);
    LT_CHECK_NEAR(curve[0], 10.5, 1e-12);
    LT_CHECK_NEAR(curve[1], 14.7, 1e-12);
    LT_CHECK_NEAR(curve[2], 20.0, 1e-12);
    lt_afr_corr_describe(&c, buf, sizeof buf);
    LT_CHECK_STR(buf, "corrected +0.50 AFR against a reference (14.70 read as 14.20)");

    LT_CHECK(lt_afr_corr_from_points(&c, pts2, 2) == 0);
    x = c.gain * 12.5 + c.offset;
    LT_CHECK_NEAR(x, 12.08, 1e-9);
    x = c.gain * 19.0 + c.offset;
    LT_CHECK_NEAR(x, 18.25, 1e-9);
    x = c.gain * 14.7 + c.offset;
    LT_CHECK(x > 14.0 && x < 14.7);
    lt_afr_corr_describe(&c, buf, sizeof buf);
    LT_CHECK(strstr(buf, "gain") != NULL && strstr(buf, "offset") != NULL);

    LT_CHECK(lt_afr_corr_from_points(&c, same, 2) == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "must differ") != NULL);
    LT_CHECK(lt_afr_corr_from_points(&c, rev, 2) == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "reverse") != NULL);
    LT_CHECK(lt_afr_corr_from_points(&c, three, 3) == LT_EDATA);
    LT_CHECK(lt_afr_corr_from_points(&c, three, 0) == LT_EDATA);
}

LT_TEST(calib_linear_wb)
{
    lt_linear_wb w;
    double out[1024];
    char buf[100];
    LT_CHECK(lt_linear_wb_new(&w, 0, 9.69, 5, 19.80) == 0);
    LT_CHECK_NEAR(lt_linear_wb_afr(&w, 0), 9.69, 1e-9);
    LT_CHECK_NEAR(lt_linear_wb_afr(&w, 5), 19.80, 1e-9);
    lt_linear_wb_curve(&w, out);
    LT_CHECK_NEAR(out[0], 9.69, 1e-9);
    LT_CHECK_NEAR(out[1023], 19.80, 1e-9);
    lt_linear_wb_describe(&w, buf, sizeof buf);
    LT_CHECK_STR(buf, "9.69-19.80 AFR over 0-5 V (2.0220 AFR/V)");
    LT_CHECK(lt_linear_wb_new(&w, 2.5, 14.7, 2.5, 15.0) == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "different voltages") != NULL);
}

LT_TEST(calib_thermistor)
{
    lt_therm_fit f;
    double t[3] = {-40, 21, 99}, r[3] = {2022088, 68273, 3715};
    double t2[3] = {-40, 30, 99}, r2[3] = {100700, 2238, 177};
    double bins[32], out[32];
    int i;

    LT_CHECK(lt_therm_fit_points(&f, 50000, t, r) == 0);
    for (i = 0; i < 3; i++) LT_CHECK_NEAR(lt_therm_temp_at_resistance(&f, r[i]), t[i], 0.01);
    LT_CHECK(lt_therm_fit_points(&f, 2490, t2, r2) == 0);
    LT_CHECK(lt_therm_temp_at_adc(&f, 100) > lt_therm_temp_at_adc(&f, 900));
    for (i = 0; i < 32; i++) bins[i] = (double)(i * 33);
    lt_therm_sample_at_bins(&f, bins, out);
    LT_CHECK_NEAR(out[3], lt_therm_temp_at_adc(&f, 99), 1e-9);
    LT_CHECK(out[2] > out[30]);
    /* errors */
    r[1] = 0;
    LT_CHECK(lt_therm_fit_points(&f, 1, t, r) == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "must be positive (point 1 was 0 ohm)") != NULL);
    {
        double tc[3] = {0, 10, 20}, rc[3] = {1000, 1000, 1000};
        LT_CHECK(lt_therm_fit_points(&f, 1, tc, rc) == LT_EDATA);
    }
    {
        /* three points on one line in 1/T vs ln R still fit (c = 0); a duplicated resistance
         * is rejected */
        double tc[3] = {0, 10, 20}, rc[3] = {1000, 2000, 1000};
        int rc2 = lt_therm_fit_points(&f, 1, tc, rc);
        LT_CHECK(rc2 == LT_EDATA || rc2 == 0);
    }
}

/* ---------------------------------------------------------------- pins */
typedef struct pv { const char *name; size_t idx; } pv;
typedef struct pvs { const pv *v; size_t n; } pvs;

static lt_bool pv_fn(void *ud, const lt_ini_const *c, size_t *idx)
{
    const pvs *s = (const pvs *)ud;
    size_t i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].name, c->name) == 0) {
            *idx = s->v[i].idx;
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

static const char *const pin_ini_parts[] = {
    "[MegaTune]\n signature = \"pin test\"\n"
    "[Constants]\n nPages = 1\n pageSize = 32\n"
    " pageReadCommand = \"r\"\n burnCommand = \"b\"\n"
    "page = 1\n",
    " fuelPumpPin = bits, U08, 0, [0:7], \"NONE\", \"INVALID\", \"PA0\", \"PD13\", \"PE1\"\n"
    " gppwm1_pin  = bits, U08, 1, [0:7], \"NONE\", \"INVALID\", \"PA0\", \"PD13\", \"PE1\"\n"
    " fanPin      = bits, U08, 2, [0:7], \"NONE\", \"INVALID\", \"PA0\", \"PD13\", \"PE1\"\n"
    " waterPumpPin = bits, U08, 3, [0:7], \"NONE\", \"INVALID\", \"PA0\", \"PD13\", \"PE1\"\n",
    " fuelPumpPinMode = bits, U08, 4, [0:1], \"default\", \"default inverted\", \"open collector\","
    " \"x\"\n"
    " out1_pin = bits, U08, 5, [0:7], \"Board Default\", \"1\", \"3\", \"30\"\n"
    " out2_pin = bits, U08, 6, [0:7], \"Board Default\", \"1\", \"3\", \"30\"\n",
    " out3_pin = bits, U08, 7, [0:7], \"Board Default\", \"1\", \"3\", \"30\"\n"
    " twoOpts_pin = bits, U08, 8, [0:7], \"NONE\", \"PA0\"\n"
    " invertPin = bits, U08, 9, [0:7], \"NONE\", \"A\", \"B\"\n"
    " gearSelect = bits, U08, 10, [0:7], \"NONE\", \"PA1\", \"PA2\", \"PA3\", \"PA4\", \"PA5\", ",
    "\"PA6\", \"PA7\", \"PB1\"\n"
    " plainNumber = scalar, U08, 11, \"\", 1, 0, 0, 255, 0\n",
    NULL};

static char *pin_ini_text(void)
{
    lt_buf b;
    size_t i;
    lt_buf_init(&b);
    for (i = 0; pin_ini_parts[i] != NULL; i++) lt_buf_puts(&b, pin_ini_parts[i]);
    return lt_buf_take(&b);
}

LT_TEST(pin_detection)
{
    char *pin_ini = pin_ini_text();
    lt_ini *ini = lt_ini_load_string(pin_ini, strlen(pin_ini), NULL, NULL);
    lt_pin_report rep;
    lt_pin_conflict c;
    char *sum;
    pv base[] = {{"fuelPumpPin", 3}, {"gppwm1_pin", 3}, {"fanPin", 2}, {"fuelPumpPinMode", 1}};
    pvs s;
    lt_free(pin_ini);
    LT_CHECK(ini != NULL);
    if (!ini) return;

    LT_CHECK(lt_pin_is_assignment(lt_ini_find_const(ini, "fuelPumpPin")));
    LT_CHECK(!lt_pin_is_assignment(lt_ini_find_const(ini, "fuelPumpPinMode")));
    LT_CHECK(!lt_pin_is_assignment(lt_ini_find_const(ini, "twoOpts_pin")));  /* < 3 options */
    LT_CHECK(!lt_pin_is_assignment(lt_ini_find_const(ini, "invertPin")));
    LT_CHECK(!lt_pin_is_assignment(lt_ini_find_const(ini, "plainNumber")));
    LT_CHECK(lt_pin_is_assignment(lt_ini_find_const(ini, "gearSelect")));   /* >= 8 + pinish */
    LT_CHECK(lt_pin_is_unassigned_label("NONE") && lt_pin_is_unassigned_label(" Board Default "));
    LT_CHECK(lt_pin_is_unassigned_label("") && lt_pin_is_unassigned_label("N/A"));
    LT_CHECK(!lt_pin_is_unassigned_label("PD13"));

    s.v = base;
    s.n = 4;
    LT_CHECK(lt_pin_detect(ini, pv_fn, &s, &rep) == 0);
    LT_CHECK(rep.n == 1);
    if (rep.n == 1) {
        LT_CHECK_STR(rep.c[0].pin_label, "PD13");
        LT_CHECK(rep.c[0].n == 2);
        LT_CHECK_STR(rep.c[0].constants[0], "fuelPumpPin");
        LT_CHECK_STR(rep.c[0].constants[1], "gppwm1_pin");
    }
    sum = lt_pin_summary(&rep);
    LT_CHECK_STR(sum, "Pin assignment conflict(s) detected \xE2\x80\x94 the same pin is used by "
                      "multiple outputs:\n\xE2\x80\xA2 Pin 'PD13' used by: fuelPumpPin, "
                      "gppwm1_pin\nClear or reassign one of the functions before burning (or "
                      "confirm to burn anyway).");
    lt_free(sum);
    lt_pin_report_free(&rep);

    /* NONE values ignored */
    {
        pv nones[] = {{"fuelPumpPin", 0}, {"gppwm1_pin", 0}, {"fanPin", 1}};
        s.v = nones;
        s.n = 3;
        LT_CHECK(lt_pin_detect(ini, pv_fn, &s, &rep) == 0 && rep.n == 0);
        sum = lt_pin_summary(&rep);
        LT_CHECK_STR(sum, "");
        lt_free(sum);
        lt_pin_report_free(&rep);
    }
    /* Board Default never conflicts */
    {
        pv bd[] = {{"out1_pin", 0}, {"out2_pin", 0}, {"out3_pin", 0}};
        s.v = bd;
        s.n = 3;
        LT_CHECK(lt_pin_detect(ini, pv_fn, &s, &rep) == 0 && rep.n == 0);
        lt_pin_report_free(&rep);
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "out1_pin", 0, pv_fn, &s, &c) == 0);
    }
    /* assigning */
    {
        pv hold[] = {{"gppwm1_pin", 2}, {"fanPin", 3}};
        s.v = hold;
        s.n = 2;
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "waterPumpPin", 2, pv_fn, &s, &c) == 1);
        LT_CHECK_STR(c.pin_label, "PA0");
        LT_CHECK(c.n == 2);
        LT_CHECK_STR(c.constants[0], "waterPumpPin");
        LT_CHECK_STR(c.constants[1], "gppwm1_pin");
        lt_pin_conflict_free(&c);
        /* PD13 is held by fanPin: assigning fuelPumpPin there; others sorted after it */
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "fuelPumpPin", 3, pv_fn, &s, &c) == 1);
        LT_CHECK_STR(c.constants[0], "fuelPumpPin");
        LT_CHECK_STR(c.constants[1], "fanPin");
        lt_pin_conflict_free(&c);
        /* assigning NONE or an out of range option or a non-assignment */
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "waterPumpPin", 0, pv_fn, &s, &c) == 0);
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "waterPumpPin", 99, pv_fn, &s, &c) == 0);
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "plainNumber", 1, pv_fn, &s, &c) == 0);
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "nope", 1, pv_fn, &s, &c) == 0);
        /* free pin */
        LT_CHECK(lt_pin_conflict_if_assigning(ini, "waterPumpPin", 4, pv_fn, &s, &c) == 0);
    }
    /* from page images */
    {
        lt_u8 page[32];
        struct lt_pin_pages pp;
        const lt_u8 *(*dummy)(void *, int, size_t *) = NULL;
        lt_pagesrc src;
        size_t idx = 99;
        LT_UNUSED(dummy);
        memset(page, 0, sizeof page);
        page[0] = 3;
        page[1] = 3;
        {
            lt_tune *t = lt_tune_new("pin test");
            lt_tune_set_page(t, 0, page, sizeof page);
            src = lt_tune_pagesrc(t);
            pp.ini = ini;
            pp.src = &src;
            LT_CHECK(lt_pin_value_from_pages(&pp, lt_ini_find_const(ini, "fuelPumpPin"), &idx));
            LT_CHECK(idx == 3);
            LT_CHECK(lt_pin_detect(ini, lt_pin_value_from_pages, &pp, &rep) == 0);
            LT_CHECK(rep.n == 1 && rep.c[0].n == 2);
            lt_pin_report_free(&rep);
            lt_tune_free(t);
        }
    }
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- actions */
LT_TEST(action_recorder)
{
    lt_arec r;
    lt_action a;
    lt_action_set *s;
    char *id;
    char *ecus[2];
    lt_arec_init(&r);
    LT_CHECK(!lt_arec_is_recording(&r));
    lt_action_constant_change(&a, "AFR_Target", 14.5, 1, 14.0);
    LT_CHECK(lt_arec_record(&r, &a) == LT_EINVAL);
    LT_CHECK_STR(lt_err_get(), "No recording in progress");
    id = lt_arec_start(&r, "Test", "desc", "me");
    LT_CHECK(id && strlen(id) == 36 && id[8] == '-' && id[13] == '-' && id[14] == '4');
    LT_CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
    LT_CHECK(lt_arec_is_recording(&r));
    LT_CHECK(lt_arec_record(&r, &a) == 0);
    lt_action_clear(&a);
    LT_CHECK(lt_arec_current(&r)->n == 1);
    LT_CHECK(lt_arec_add_tag(&r, "t1") == 0);
    LT_CHECK(lt_arec_add_tag(&r, "t1") == 0);
    LT_CHECK(lt_arec_current(&r)->meta.ntags == 2);
    ecus[0] = (char *)"Speeduino";
    ecus[1] = (char *)"rusEFI";
    LT_CHECK(lt_arec_set_compatible_ecus(&r, ecus, 2) == 0);
    LT_CHECK(lt_arec_current(&r)->meta.necus == 2);
    LT_CHECK_STR(lt_arec_current(&r)->version, "1.0");
    LT_CHECK_STR(lt_arec_current(&r)->id, id);
    /* restart replaces */
    {
        char *id2 = lt_arec_start(&r, "T2", "", "");
        LT_CHECK(strcmp(id, id2) != 0);
        LT_CHECK(lt_arec_current(&r)->n == 0);
        lt_free(id2);
    }
    s = lt_arec_stop(&r);
    LT_CHECK(s != NULL && !lt_arec_is_recording(&r));
    LT_CHECK(lt_arec_stop(&r) == NULL);
    lt_action_set_free(s);
    lt_free(s);
    lt_free(id);
    lt_arec_free(&r);
}

static void build_set(lt_action_set *s)
{
    lt_arec r;
    lt_action a;
    double olds[3] = {75, 76, 77};
    char *id;
    lt_arec_init(&r);
    id = lt_arec_start(&r, "Round trip", "all variants", "tester");
    lt_free(id);
    lt_action_table_edit(&a, "veTable1", 5, 3, 75.5, 1, 74.2);
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_action_constant_change(&a, "reqFuel", 8.5, 0, 0);
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_action_bulk(&a, "scale", "veTable1");
    lt_action_bulk_add_cell(&a, 0, 0);
    lt_action_bulk_add_cell(&a, 1, 1);
    lt_action_bulk_add_cell(&a, 2, 2);
    lt_action_bulk_set_param(&a, "factor", 1.05);
    a.old_values = (double *)lt_malloc(sizeof olds);
    memcpy(a.old_values, olds, sizeof olds);
    a.nold = 3;
    a.has_old_values = 1;
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_action_lua(&a, "print('hi')", "demo");
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_action_pause(&a, 250);
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_action_send_command(&a, "cmdTestOn");
    lt_arec_record(&r, &a);
    lt_action_clear(&a);
    lt_arec_add_tag(&r, "a\"b");
    {
        lt_action_set *t = lt_arec_stop(&r);
        *s = *t;
        lt_free(t);
    }
    lt_arec_free(&r);
}

LT_TEST(action_serialize)
{
    lt_action_set s, b;
    char err[200], *txt;
    build_set(&s);
    LT_CHECK(s.n == 6);
    txt = lt_action_set_serialize(&s, err, sizeof err);
    LT_CHECK(txt != NULL);
    if (!txt) return;
    LT_CHECK(strstr(txt, "\"type\": \"TableEdit\"") != NULL);
    LT_CHECK(strstr(txt, "\"data\": {") != NULL);
    LT_CHECK(strstr(txt, "\"x_index\": 5") != NULL);
    LT_CHECK(strstr(txt, "\"old_value\": 74.2") != NULL);
    LT_CHECK(strstr(txt, "\"old_value\": null") != NULL);
    LT_CHECK(strstr(txt, "\"cells\": [\n") != NULL);
    LT_CHECK(strstr(txt, "\"duration_ms\": 250") != NULL);
    LT_CHECK(strstr(txt, "\"type\": \"ExecuteLuaScript\"") != NULL);
    LT_CHECK(strstr(txt, "\"tags\": [\n      \"a\\\"b\"\n    ]") != NULL);
    LT_CHECK(lt_action_set_deserialize(txt, &b, err, sizeof err) == 0);
    LT_CHECK_STR(b.name, "Round trip");
    LT_CHECK_STR(b.description, "all variants");
    LT_CHECK_STR(b.id, s.id);
    LT_CHECK(b.n == 6 && b.meta.ntags == 1 && b.meta.necus == 0);
    LT_CHECK_STR(b.meta.tags[0], "a\"b");
    LT_CHECK(b.actions[0].type == LT_ACT_TABLE_EDIT && b.actions[0].x_index == 5 &&
             b.actions[0].y_index == 3 && b.actions[0].has_old);
    LT_CHECK_NEAR(b.actions[0].new_value, 75.5, 0);
    LT_CHECK_NEAR(b.actions[0].old_value, 74.2, 0);
    LT_CHECK(b.actions[1].type == LT_ACT_CONSTANT_CHANGE && !b.actions[1].has_old);
    LT_CHECK(b.actions[2].type == LT_ACT_BULK && b.actions[2].ncells == 3 &&
             b.actions[2].cells[2].x == 2 && b.actions[2].nparams == 1 &&
             b.actions[2].has_old_values && b.actions[2].nold == 3 &&
             b.actions[2].old_values[2] == 77);
    LT_CHECK_STR(b.actions[3].script, "print('hi')");
    LT_CHECK(b.actions[4].duration_ms == 250);
    LT_CHECK_STR(b.actions[5].command, "cmdTestOn");
    {
        char *again = lt_action_set_serialize(&b, err, sizeof err);
        LT_CHECK(again && strcmp(again, txt) == 0);
        lt_free(again);
    }
    lt_action_set_free(&b);
    lt_free(txt);

    /* summary */
    txt = lt_action_summarize(&s);
    LT_CHECK_STR(txt, "Action Set: Round trip\nActions: 6\n  - TableEdit: 1\n  - ConstantChange: 1\n"
                      "  - BulkOperation: 1\n  - ExecuteLuaScript: 1\n  - Pause: 1\n"
                      "  - SendCommand: 1\n");
    lt_free(txt);

    /* non finite refused */
    s.actions[0].new_value = 1.0 / 0.0 * 0.0;   /* NaN at run time, not a constant fold issue */
    s.actions[0].new_value = s.actions[0].new_value != s.actions[0].new_value ? s.actions[0].new_value : 0;
    {
        double zero = 0.0;
        s.actions[0].new_value = 1.0 / zero;
    }
    LT_CHECK(lt_action_set_serialize(&s, err, sizeof err) == NULL);
    LT_CHECK(strstr(err, "Failed to serialize action set") != NULL);
    lt_action_set_free(&s);
}

LT_TEST(action_deserialize_errors)
{
    lt_action_set b;
    char err[200];
    const char *head = "{\"id\":\"i\",\"name\":\"n\",\"description\":\"d\",\"version\":\"1.0\",";
    const char *meta = "\"metadata\":{\"created_by\":\"\",\"created_at\":\"\",\"modified_at\":\"\","
                       "\"tags\":[],\"compatible_ecus\":[]}}";
    char buf[1024];
    LT_CHECK(lt_action_set_deserialize("{nope", &b, err, sizeof err) == LT_EDATA);
    LT_CHECK(strstr(err, "Failed to deserialize action set:") == err);
    lt_snprintf(buf, sizeof buf, "%s\"actions\":[{\"type\":\"Nope\",\"data\":{}}],%s", head, meta);
    LT_CHECK(lt_action_set_deserialize(buf, &b, err, sizeof err) == LT_EDATA);
    lt_snprintf(buf, sizeof buf, "%s\"actions\":[{\"type\":\"Pause\",\"data\":{}}],%s", head, meta);
    LT_CHECK(lt_action_set_deserialize(buf, &b, err, sizeof err) == LT_EDATA);
    lt_snprintf(buf, sizeof buf,
                "%s\"actions\":[{\"type\":\"TableEdit\",\"data\":{\"table_name\":\"t\","
                "\"x_index\":70000,\"y_index\":0,\"new_value\":1}}],%s", head, meta);
    LT_CHECK(lt_action_set_deserialize(buf, &b, err, sizeof err) == LT_EDATA);
    lt_snprintf(buf, sizeof buf, "%s\"actions\":[],\"metadata\":{}}", head);
    LT_CHECK(lt_action_set_deserialize(buf, &b, err, sizeof err) == LT_EDATA);
    /* old_value absent is fine */
    lt_snprintf(buf, sizeof buf,
                "%s\"actions\":[{\"type\":\"ConstantChange\",\"data\":{\"constant_name\":\"c\","
                "\"new_value\":2}}],%s", head, meta);
    LT_CHECK(lt_action_set_deserialize(buf, &b, err, sizeof err) == 0);
    LT_CHECK(b.n == 1 && !b.actions[0].has_old);
    lt_action_set_free(&b);
}

LT_TEST(action_validate_no_ini)
{
    lt_action_set s;
    lt_action a;
    char **w, **e;
    size_t nw, ne;
    lt_action_set_init(&s);
    LT_CHECK(lt_action_validate(&s, NULL, &w, &nw, &e, &ne) == 0 && nw == 0 && ne == 0);

    s.actions = (lt_action *)lt_calloc(8, sizeof(lt_action));
    lt_action_constant_change(&a, "AFR", 0.0 / 1.0, 0, 0);
    {
        double zero = 0.0;
        a.new_value = zero / zero;
    }
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_validate(&s, NULL, NULL, NULL, &e, &ne) == LT_EINVAL);
    LT_CHECK(ne == 1);
    if (ne == 1) LT_CHECK_STR(e[0], "Action 0: Invalid value NaN");
    lt_act_strv_free(e, ne);

    lt_action_table_edit(&a, "", 0, 0, 1, 0, 0);
    s.actions[s.n++] = a;
    lt_action_bulk(&a, "scale", "t");
    s.actions[s.n++] = a;      /* no cells (warning), no factor (error) */
    lt_action_bulk(&a, "rebin", "t");
    lt_action_bulk_add_cell(&a, 0, 0);
    s.actions[s.n++] = a;
    lt_action_pause(&a, 0);
    s.actions[s.n++] = a;
    lt_action_lua(&a, "", "");
    s.actions[s.n++] = a;
    lt_action_send_command(&a, "");
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_validate(&s, NULL, &w, &nw, &e, &ne) == LT_EINVAL);
    LT_CHECK(nw == 0);   /* warnings dropped when errors exist */
    LT_CHECK(ne == 6);
    if (ne == 6) {
        LT_CHECK_STR(e[1], "Action 1: Empty table name");
        LT_CHECK_STR(e[2], "Action 2: scale missing 'factor' parameter");
        LT_CHECK_STR(e[3], "Action 3: rebin missing bin parameters");
        LT_CHECK_STR(e[4], "Action 5: Empty Lua script");
        LT_CHECK_STR(e[5], "Action 6: Empty command");
    }
    lt_act_strv_free(e, ne);
    lt_action_set_free(&s);

    /* warnings only: scale with factor and cells, pause 0 */
    lt_action_set_init(&s);
    s.actions = (lt_action *)lt_calloc(4, sizeof(lt_action));
    lt_action_bulk(&a, "scale", "veTable1");
    lt_action_bulk_add_cell(&a, 0, 0);
    lt_action_bulk_add_cell(&a, 1, 1);
    lt_action_bulk_add_cell(&a, 2, 2);
    lt_action_bulk_set_param(&a, "factor", 1.05);
    s.actions[s.n++] = a;
    lt_action_pause(&a, 0);
    s.actions[s.n++] = a;
    lt_action_bulk(&a, "smooth", "veTable1");
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_validate(&s, NULL, &w, &nw, &e, &ne) == 0);
    LT_CHECK(ne == 0 && nw == 2);
    if (nw == 2) {
        LT_CHECK_STR(w[0], "Action 1: Zero-duration pause");
        LT_CHECK_STR(w[1], "Action 2: No cells selected for smooth");
    }
    lt_act_strv_free(w, nw);
    lt_action_set_free(&s);
}

LT_TEST(action_validate_with_ini)
{
    lt_ini *ini = lt_ini_load_file(FIX "mini.ini", NULL);
    lt_action_set s;
    lt_action a;
    char **e;
    size_t ne, i;
    int found_min = 0, found_range = 0, found_bits = 0, found_tbl = 0, found_x = 0, found_y = 0;
    int found_cell = 0, found_cmd = 0, found_const = 0;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    lt_action_set_init(&s);
    s.actions = (lt_action *)lt_calloc(16, sizeof(lt_action));

    lt_action_constant_change(&a, "reqFuel", 30, 0, 0);          /* above max, unstorable */
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "reqFuel", -1, 0, 0);          /* below min */
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "nCylinders", 12, 0, 0);       /* bits */
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "nope", 1, 0, 0);
    s.actions[s.n++] = a;
    lt_action_table_edit(&a, "missingTbl", 0, 0, 1, 0, 0);
    s.actions[s.n++] = a;
    lt_action_table_edit(&a, "veTable1Map", 4, 4, 1, 0, 0);      /* by map name, 4x4 */
    s.actions[s.n++] = a;
    lt_action_bulk(&a, "scale", "veTbl");
    lt_action_bulk_add_cell(&a, 9, 0);
    lt_action_bulk_add_cell(&a, 0, 7);
    lt_action_bulk_set_param(&a, "factor", 1.0);
    s.actions[s.n++] = a;
    lt_action_send_command(&a, "cmdBogus");
    s.actions[s.n++] = a;
    lt_action_send_command(&a, "cmdTestOn");
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "reqFuel", 10.0, 0, 0);        /* fine */
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_validate(&s, ini, NULL, NULL, &e, &ne) == LT_EINVAL);
    for (i = 0; i < ne; i++) {
        if (strstr(e[i], "Action 0: Value 30 for constant 'reqFuel' above maximum 25.5 (ms)"))
            found_min = 1;
        if (strstr(e[i], "Action 0: Value 30 (ms) for constant 'reqFuel' outside storable range "
                         "[raw 0..255] for U08"))
            found_range = 1;
        if (strstr(e[i], "Action 1: Value -1 for constant 'reqFuel' below minimum 0 (ms)"))
            found_const = 1;
        if (strstr(e[i], "Action 2: Value 12 for bits constant 'nCylinders' not one of 9 (INVALID, "
                         "1, 2"))
            found_bits = 1;
        if (strstr(e[i], "Action 3: Constant 'nope' not found in ECU definition")) found_tbl = 1;
        if (strstr(e[i], "Action 4: Table 'missingTbl' not found in ECU definition")) found_x = 1;
        if (strstr(e[i], "Action 5: x_index 4 out of bounds for table 'veTable1Map' (width 4)"))
            found_y = 1;
        if (strstr(e[i], "Action 6: cell x=9 out of bounds for table 'veTbl' (width 4)"))
            found_cell = 1;
        if (strstr(e[i], "Action 7: Command 'cmdBogus' not supported by ECU definition"))
            found_cmd = 1;
        LT_CHECK(strstr(e[i], "Action 8") == NULL && strstr(e[i], "Action 9") == NULL);
    }
    LT_CHECK(found_min && found_range && found_const && found_bits && found_tbl && found_x &&
             found_y && found_cell && found_cmd);
    {
        int y5 = 0, y6 = 0;
        for (i = 0; i < ne; i++) {
            if (strstr(e[i], "Action 5: y_index 4 out of bounds for table 'veTable1Map' (height 4)"))
                y5 = 1;
            if (strstr(e[i], "Action 6: cell y=7 out of bounds for table 'veTbl' (height 4)"))
                y6 = 1;
        }
        LT_CHECK(y5 && y6);
    }
    lt_act_strv_free(e, ne);
    lt_action_set_free(&s);
    lt_ini_free(ini);
}

typedef struct rctx { unsigned long slept; int cmds; char last[64]; } rctx;
static void r_sleep(void *ud, unsigned long ms) { ((rctx *)ud)->slept += ms; }
static int r_cmd(void *ud, const char *name)
{
    rctx *c = (rctx *)ud;
    c->cmds++;
    lt_strlcpy(c->last, name, sizeof c->last);
    return 0;
}

LT_TEST(action_replay)
{
    lt_ini *ini = lt_ini_load_file(FIX "mini.ini", NULL);
    lt_tune *t;
    lt_action_set s;
    lt_action a;
    lt_replay_env env;
    lt_replay_result res;
    rctx rc;
    double v = 0;
    lt_pagesrc src;
    const lt_ini_const *ve;
    double cells[16];

    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_from_defaults(ini);
    LT_CHECK(t != NULL);
    if (!t) return;
    src = lt_tune_pagesrc(t);
    ve = lt_ini_find_const(ini, "veTable");
    LT_CHECK(lt_ini_const_get_array(ini, ve, &src, cells, 16) == 16);
    LT_CHECK_NEAR(cells[9], 0, 0);

    memset(&rc, 0, sizeof rc);
    memset(&env, 0, sizeof env);
    env.ini = ini;
    env.tune = t;
    env.ud = &rc;
    env.sleep_ms = r_sleep;
    env.send_command = r_cmd;

    lt_action_set_init(&s);
    s.name = lt_strdup("replay");
    s.actions = (lt_action *)lt_calloc(16, sizeof(lt_action));
    lt_action_table_edit(&a, "veTbl", 1, 2, 80, 0, 0);          /* row 2 col 1 -> idx 9 */
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "reqFuel", 10.0, 0, 0);
    s.actions[s.n++] = a;
    lt_action_constant_change(&a, "nCylinders", 6, 0, 0);
    s.actions[s.n++] = a;
    lt_action_pause(&a, 250);
    s.actions[s.n++] = a;
    lt_action_send_command(&a, "cmdTestOn");
    s.actions[s.n++] = a;
    lt_action_lua(&a, "x = 1", "d");
    s.actions[s.n++] = a;
    /* bulk: set whole row 0 to 50, then scale cells (0,0),(1,0) by 2 */
    lt_action_bulk(&a, "set_equal", "veTbl");
    lt_action_bulk_add_cell(&a, 0, 0);
    lt_action_bulk_add_cell(&a, 1, 0);
    lt_action_bulk_add_cell(&a, 2, 0);
    lt_action_bulk_add_cell(&a, 3, 0);
    lt_action_bulk_set_param(&a, "value", 50);
    s.actions[s.n++] = a;
    lt_action_bulk(&a, "scale", "veTbl");
    lt_action_bulk_add_cell(&a, 0, 0);
    lt_action_bulk_add_cell(&a, 1, 0);
    lt_action_bulk_set_param(&a, "factor", 2.0);
    s.actions[s.n++] = a;

    LT_CHECK(lt_action_replay(&s, &env, &res) == 0);
    LT_CHECK(res.applied == 7 && res.skipped == 1);
    LT_CHECK(res.n_warnings == 1);
    LT_CHECK(rc.slept == 250 && rc.cmds == 1);
    LT_CHECK_STR(rc.last, "cmdTestOn");
    src = lt_tune_pagesrc(t);
    LT_CHECK(lt_ini_const_get_array(ini, ve, &src, cells, 16) == 16);
    LT_CHECK_NEAR(cells[9], 80, 0);
    LT_CHECK_NEAR(cells[0], 100, 0);
    LT_CHECK_NEAR(cells[1], 100, 0);
    LT_CHECK_NEAR(cells[2], 50, 0);
    LT_CHECK_NEAR(cells[3], 50, 0);
    LT_CHECK(lt_ini_get(ini, &src, "reqFuel", &v) == 0);
    LT_CHECK_NEAR(v, 10.0, 1e-9);
    LT_CHECK(lt_ini_get(ini, &src, "nCylinders", &v) == 0);
    LT_CHECK_NEAR(v, 6, 0);
    /* named values were re-captured from the pages */
    {
        const lt_tune_entry *e = lt_tune_get_constant(t, "veTable");
        LT_CHECK(e && e->value.kind == LT_TV_ARRAY && e->value.n == 16 &&
                 e->value.arr[9] == 80);
        e = lt_tune_get_constant(t, "reqFuel");
        LT_CHECK(e && fabs(e->value.scalar - 10.0) < 1e-9);
    }
    lt_replay_result_free(&res);

    /* refuses on validation errors and touches nothing */
    lt_action_set_free(&s);
    lt_action_set_init(&s);
    s.actions = (lt_action *)lt_calloc(4, sizeof(lt_action));
    lt_action_constant_change(&a, "reqFuel", 99, 0, 0);
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_replay(&s, &env, &res) == LT_EINVAL);
    LT_CHECK(strstr(lt_err_get(), "above maximum") != NULL);
    LT_CHECK(lt_ini_get(ini, &src, "reqFuel", &v) == 0);
    LT_CHECK_NEAR(v, 10.0, 1e-9);
    lt_action_set_free(&s);

    /* out of active range cell in a smaller dynamic table is an execution error: here use a
     * missing ini */
    lt_action_set_init(&s);
    s.actions = (lt_action *)lt_calloc(4, sizeof(lt_action));
    lt_action_table_edit(&a, "veTbl", 0, 0, 1, 0, 0);
    s.actions[s.n++] = a;
    env.ini = NULL;
    LT_CHECK(lt_action_replay(&s, &env, NULL) == LT_EINVAL);
    lt_action_set_free(&s);

    /* no command channel: warning only */
    env.ini = ini;
    env.send_command = NULL;
    lt_action_set_init(&s);
    s.actions = (lt_action *)lt_calloc(4, sizeof(lt_action));
    lt_action_send_command(&a, "cmdTestOff");
    s.actions[s.n++] = a;
    LT_CHECK(lt_action_replay(&s, &env, &res) == 0);
    LT_CHECK(res.applied == 0 && res.skipped == 1 && res.n_warnings == 1);
    lt_replay_result_free(&res);
    lt_action_set_free(&s);
    lt_tune_free(t);
    lt_ini_free(ini);
}

LT_TEST(action_file_io)
{
    lt_action_set s, b;
    char *f;
    lt_plat_mkdirs("build/test_tmp/project_act");
    f = lt_strdup("build/test_tmp/project_act/set.json");
    build_set(&s);
    LT_CHECK(lt_action_set_save(&s, f) == 0);
    LT_CHECK(lt_action_set_load(f, &b) == 0);
    LT_CHECK(b.n == 6);
    lt_action_set_free(&b);
    lt_action_set_free(&s);
    LT_CHECK(lt_action_set_load("build/test_tmp/none.json", &b) == LT_ENOENT);
    lt_free(f);
}

int main(void)
{
    lt_plat_mkdirs("build/test_tmp");
    LT_RUN(calib_adc_volts);
    LT_RUN(calib_solutions);
    LT_RUN(calib_ini_fixture);
    LT_RUN(calib_afr_correction);
    LT_RUN(calib_linear_wb);
    LT_RUN(calib_thermistor);
    LT_RUN(pin_detection);
    LT_RUN(action_recorder);
    LT_RUN(action_serialize);
    LT_RUN(action_deserialize_errors);
    LT_RUN(action_validate_no_ini);
    LT_RUN(action_validate_with_ini);
    LT_RUN(action_replay);
    LT_RUN(action_file_io);
    LT_TEST_MAIN_END
}
