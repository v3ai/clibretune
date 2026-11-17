/* test_basemap.c - base map generator tests (spec 08 section 4). */
#include "lt.h"
#include "basemap/basemap.h"
#include "lt_test.h"

static int has_scalar(const lt_basemap *m, const char *n, double *v)
{
    size_t i;
    for (i = 0; i < m->n_scalars; i++) {
        if (strcmp(m->scalars[i].name, n) == 0) {
            if (v) {
                *v = m->scalars[i].value;
            }
            return 1;
        }
    }
    return 0;
}

LT_TEST(spec_defaults)
{
    lt_engine_spec s;
    lt_engine_spec_default(&s);
    LT_CHECK(s.cylinder_count == 4);
    LT_CHECK_NEAR(s.displacement_cc, 2000, 0);
    LT_CHECK_NEAR(s.injector_size_cc, 440, 0);
    LT_CHECK(s.inj_mode == LT_INJ_SEQUENTIAL);
    LT_CHECK(s.ign_mode == LT_IGN_WASTED_SPARK);
    LT_CHECK(s.idle_rpm == 800 && s.redline_rpm == 6500);
    LT_CHECK(!s.has_boost && !s.has_wot_afr && !s.has_octane && !s.has_cr && !s.has_chamber);
}

LT_TEST(stoich_and_wot)
{
    lt_engine_spec s;
    lt_engine_spec_default(&s);
    LT_CHECK_NEAR(lt_fuel_stoich_afr(LT_FUEL_GASOLINE), 14.7, 1e-9);
    LT_CHECK_NEAR(lt_fuel_stoich_afr(LT_FUEL_E85), 9.8, 1e-9);
    LT_CHECK_NEAR(lt_fuel_stoich_afr(LT_FUEL_E100), 9.0, 1e-9);
    LT_CHECK_NEAR(lt_fuel_stoich_afr(LT_FUEL_METHANOL), 6.5, 1e-9);
    LT_CHECK_NEAR(lt_fuel_stoich_afr(LT_FUEL_LPG), 15.7, 1e-9);
    LT_CHECK_NEAR(lt_engine_safe_wot_afr(&s), 12.5, 1e-9);
    s.fuel = LT_FUEL_E85;
    LT_CHECK_NEAR(lt_engine_safe_wot_afr(&s), 8.5, 1e-9);
    s.has_wot_afr = LT_TRUE; s.target_wot_afr = 11.8;
    LT_CHECK_NEAR(lt_engine_safe_wot_afr(&s), 11.8, 1e-9);
}

LT_TEST(max_load)
{
    lt_engine_spec s;
    lt_engine_spec_default(&s);
    LT_CHECK_NEAR(lt_engine_max_load_kpa(&s), 105.0, 1e-9);
    s.aspiration = LT_ASP_TURBO; s.has_boost = LT_TRUE; s.boost_target_kpa = 250.0;
    LT_CHECK_NEAR(lt_engine_max_load_kpa(&s), 250.0, 1e-9);
    s.boost_target_kpa = 100.0;
    LT_CHECK_NEAR(lt_engine_max_load_kpa(&s), 120.0, 1e-9);
    s.has_boost = LT_FALSE;
    LT_CHECK_NEAR(lt_engine_max_load_kpa(&s), 200.0, 1e-9);
}

LT_TEST(req_fuel)
{
    lt_engine_spec s;
    lt_engine_spec_default(&s);
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 25.5, 1e-9); /* 41.76 clamped */
    s.inj_mode = LT_INJ_SIMULTANEOUS; s.displacement_cc = 1600; s.injector_size_cc = 1000;
    LT_CHECK(lt_engine_req_fuel(&s) > 0 && lt_engine_req_fuel(&s) <= 25.5);
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 25.5, 1e-9);
    s.inj_mode = LT_INJ_SEQUENTIAL; s.displacement_cc = 400; s.injector_size_cc = 440;
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 100.0 * 147.0 / (440.0 * 4.0), 1e-9);
    s.inj_mode = LT_INJ_BATCH;
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 100.0 * 147.0 / (440.0 * 2.0), 1e-9);
    s.inj_mode = LT_INJ_THROTTLE_BODY;
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 25.5, 1e-9);
    s.cylinder_count = 1; s.inj_mode = LT_INJ_BATCH; s.displacement_cc = 100;
    s.injector_size_cc = 1e6;
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 0.1, 1e-9);
}

LT_TEST(spark_advance)
{
    lt_engine_spec s;
    lt_engine_spec_default(&s);
    /* gasoline 93 octane, CR 10, no chamber => 34 */
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 34.0, 1e-9);
    LT_CHECK_NEAR(lt_engine_effective_octane(&s), 93.0, 1e-9);
    s.fuel = LT_FUEL_E85;
    LT_CHECK_NEAR(lt_engine_effective_octane(&s), 100.0, 1e-9);
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 34.0 + 4.2, 1e-9);
    s.fuel = LT_FUEL_GASOLINE;
    s.has_cr = LT_TRUE; s.compression_ratio = 8.0;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 37.0, 1e-9);
    s.compression_ratio = 14.0; /* (10-14)*1.5=-6 */
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 28.0, 1e-9);
    s.compression_ratio = 20.0; /* clamp -8 */
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 26.0, 1e-9);
    s.has_cr = LT_FALSE;
    s.has_chamber = LT_TRUE; s.chamber = LT_CH_OPEN;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 36.0, 1e-9);
    s.chamber = LT_CH_SWIRL_MULTI_VALVE;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 32.0, 1e-9);
    s.chamber = LT_CH_QUENCH_TWO_VALVE;
    s.engine_stroke = LT_STROKE_TWO;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 28.0, 1e-9);
    s.has_octane = LT_TRUE; s.octane = 50.0; s.has_cr = LT_TRUE; s.compression_ratio = 20.0;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 18.0, 1e-9); /* clamp low */
    s.engine_stroke = LT_STROKE_FOUR; s.octane = 130.0; s.compression_ratio = 5.0;
    LT_CHECK_NEAR(lt_engine_max_spark_advance(&s), 42.0, 1e-9); /* clamp high */
}

LT_TEST(rpm_bins)
{
    double b[16];
    int i;
    lt_basemap_rpm_bins(800, 6500, 16, b);
    LT_CHECK(b[0] >= 800);
    LT_CHECK(fabs(b[15] - 6500) < 100);
    for (i = 1; i < 16; i++) {
        LT_CHECK(b[i] >= b[i - 1]);
    }
    LT_CHECK_NEAR(b[0], 800, 0);
    LT_CHECK_NEAR(b[15], 6500, 0);
    lt_basemap_rpm_bins(850, 6500, 1, b);
    LT_CHECK_NEAR(b[0], 850, 0);
    /* narrow range forces dedup then padding by +100 */
    lt_basemap_rpm_bins(1000, 1200, 8, b);
    LT_CHECK_NEAR(b[0], 1000, 0);
    for (i = 1; i < 8; i++) {
        LT_CHECK(b[i] > b[i - 1]);
    }
}

LT_TEST(load_bins)
{
    double b[16];
    int i;
    lt_basemap_load_bins(105.0, 16, b);
    LT_CHECK_NEAR(b[0], 15, 0);
    LT_CHECK_NEAR(b[15], 105, 0);
    for (i = 1; i < 16; i++) {
        LT_CHECK(b[i] > b[i - 1]);
    }
    LT_CHECK_NEAR(b[1], 21, 0);
    lt_basemap_load_bins(250.0, 16, b);
    LT_CHECK_NEAR(b[15], 250, 0);
    lt_basemap_load_bins(99.0, 1, b);
    LT_CHECK_NEAR(b[0], 99, 0);
}

LT_TEST(generate_complete)
{
    lt_engine_spec s;
    lt_basemap m;
    double v;
    int i;
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    LT_CHECK(m.ve[0][0] < 50);
    LT_CHECK(m.ve[15][8] > 50);
    LT_CHECK_NEAR(m.ve[0][0], 30, 0);
    LT_CHECK(m.req_fuel > 0 && m.prime_pulse_ms > 0);
    LT_CHECK_NEAR(m.prime_pulse_ms, 3.0, 1e-9);
    LT_CHECK(has_scalar(&m, "reqFuel", &v) && v == m.req_fuel);
    LT_CHECK(has_scalar(&m, "nCylinders", &v) && v == 4);
    LT_CHECK(has_scalar(&m, "injType", &v) && v == 1);
    LT_CHECK(has_scalar(&m, "sparkMode", &v) && v == 0);
    LT_CHECK(has_scalar(&m, "stoich", &v) && fabs(v - 14.7) < 1e-9);
    for (i = 0; i < 256; i++) {
        double iv = m.ign[i / 16][i % 16];
        double vv = m.ve[i / 16][i % 16];
        LT_CHECK(iv >= 0 && iv <= 45);
        LT_CHECK(vv >= 15 && vv <= 120 && vv == floor(vv));
    }
    LT_CHECK(m.ign[8][15] > m.ign[8][0]);
    LT_CHECK(m.afr[15][8] < m.afr[8][3]);
    LT_CHECK_NEAR(m.afr[15][8], 12.5, 1e-9);
    LT_CHECK_NEAR(m.afr[0][0], 14.3, 1e-9);   /* 14.7*0.97=14.259 */
    LT_CHECK_NEAR(m.afr[0][15], 15.0, 1e-9);  /* 14.7*1.02=14.994 */
    LT_CHECK_NEAR(m.accel_tps_threshold, 10, 0);
    LT_CHECK_NEAR(m.accel_enrichment_pct, 30, 0);
    LT_CHECK(m.accel_duration_cycles == 3);
    LT_CHECK_NEAR(m.accel_taper_pct, 50, 0);
    LT_CHECK_NEAR(m.iac_cold_start_pct, 70, 0);
    LT_CHECK_NEAR(m.iac_warm_idle_pct, 20, 0);
    LT_CHECK_NEAR(m.iac_warm_threshold_c, 75, 0);
    lt_basemap_free(&m);
    LT_CHECK(m.scalars == NULL && m.n_scalars == 0);
}

LT_TEST(curves)
{
    lt_engine_spec s;
    lt_basemap m;
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    LT_CHECK(m.cranking[0].value > m.cranking[7].value);
    LT_CHECK_NEAR(m.cranking[0].coolant_c, -40, 0);
    LT_CHECK_NEAR(m.cranking[7].coolant_c, 100, 0);
    LT_CHECK_NEAR(m.cranking[0].value, 400, 1e-9);
    LT_CHECK(m.warmup[0].value > 100);
    LT_CHECK_NEAR(m.warmup[7].value, 100.0, 0);
    lt_basemap_free(&m);
    s.fuel = LT_FUEL_E85;
    lt_basemap_generate(&s, &m);
    LT_CHECK_NEAR(m.cranking[0].value, 560, 1e-9);
    LT_CHECK_NEAR(m.warmup[0].value, 216, 1e-9);
    LT_CHECK_NEAR(m.warmup[5].value, 132, 1e-9);
    LT_CHECK_NEAR(m.warmup[6].value, 100, 0);
    LT_CHECK_NEAR(m.prime_pulse_ms, 4.5, 1e-9);
    LT_CHECK_NEAR(m.iac_cold_start_pct, 80, 0);
    lt_basemap_free(&m);
    s.fuel = LT_FUEL_LPG; s.injector_size_cc = 50; /* clamps to 100 */
    lt_basemap_generate(&s, &m);
    LT_CHECK_NEAR(m.prime_pulse_ms, 2.0 * 4.4, 1e-9);
    LT_CHECK_NEAR(m.warmup[0].value, 162, 1e-9);
    lt_basemap_free(&m);
    s.fuel = LT_FUEL_METHANOL; s.injector_size_cc = 100;
    lt_basemap_generate(&s, &m);
    LT_CHECK_NEAR(m.prime_pulse_ms, 15.0, 1e-9); /* clamped */
    lt_basemap_free(&m);
}

LT_TEST(turbo_and_e85)
{
    lt_engine_spec s;
    lt_basemap m;
    double v;
    lt_engine_spec_default(&s);
    s.aspiration = LT_ASP_TURBO; s.has_boost = LT_TRUE; s.boost_target_kpa = 200.0;
    lt_basemap_generate(&s, &m);
    LT_CHECK(m.load_bins[15] > 101);
    LT_CHECK_NEAR(m.load_bins[15], 200, 0);
    LT_CHECK(m.ve[15][8] > m.ve[0][8]);
    LT_CHECK(m.ign[15][8] < m.ign[0][8]);  /* boost retard */
    lt_basemap_free(&m);
    lt_engine_spec_default(&s);
    s.fuel = LT_FUEL_E85;
    lt_basemap_generate(&s, &m);
    LT_CHECK(m.afr[15][8] < 10.0);
    LT_CHECK(has_scalar(&m, "stoich", &v) && fabs(v - 9.8) < 1e-9);
    lt_basemap_free(&m);
}

LT_TEST(scalar_modes)
{
    lt_engine_spec s;
    lt_basemap m;
    double v;
    lt_engine_spec_default(&s);
    s.inj_mode = LT_INJ_SIMULTANEOUS; s.ign_mode = LT_IGN_COIL_ON_PLUG; s.cylinder_count = 6;
    lt_basemap_generate(&s, &m);
    LT_CHECK(has_scalar(&m, "injType", &v) && v == 0);
    LT_CHECK(has_scalar(&m, "sparkMode", &v) && v == 1);
    LT_CHECK(has_scalar(&m, "nCylinders", &v) && v == 6);
    lt_basemap_free(&m);
    s.inj_mode = LT_INJ_BATCH; s.ign_mode = LT_IGN_DISTRIBUTOR;
    lt_basemap_generate(&s, &m);
    LT_CHECK(has_scalar(&m, "injType", &v) && v == 2);
    LT_CHECK(has_scalar(&m, "sparkMode", &v) && v == 2);
    lt_basemap_free(&m);
    s.inj_mode = LT_INJ_THROTTLE_BODY;
    lt_basemap_generate(&s, &m);
    LT_CHECK(has_scalar(&m, "injType", &v) && v == 3);
    lt_basemap_free(&m);
}

LT_TEST(degenerate_inputs)
{
    lt_engine_spec s;
    lt_basemap m;
    double b[3];
    lt_engine_spec_default(&s);
    s.displacement_cc = 0.0; s.injector_size_cc = 0.0;
    LT_CHECK_NEAR(lt_engine_req_fuel(&s), 0.1, 1e-12);   /* NaN -> floor */
    lt_engine_spec_default(&s);
    s.cylinder_count = 0; s.idle_rpm = 7000; s.redline_rpm = 500;
    lt_basemap_generate(&s, &m);                         /* must not crash or divide by 0 */
    LT_CHECK(m.ve[0][0] >= 15.0 && m.ve[15][15] <= 120.0);
    lt_basemap_free(&m);
    lt_basemap_rpm_bins(800, 6500, 0, b);
    lt_basemap_rpm_bins(800, 6500, 1, b);
    LT_CHECK_NEAR(b[0], 800, 0);
    lt_basemap_load_bins(105, 2, b);
    LT_CHECK_NEAR(b[0], 15, 0);
    LT_CHECK_NEAR(b[1], 105, 0);
}

int main(void)
{
    LT_RUN(degenerate_inputs);
    LT_RUN(spec_defaults);
    LT_RUN(stoich_and_wot);
    LT_RUN(max_load);
    LT_RUN(req_fuel);
    LT_RUN(spark_advance);
    LT_RUN(rpm_bins);
    LT_RUN(load_bins);
    LT_RUN(generate_complete);
    LT_RUN(curves);
    LT_RUN(turbo_and_e85);
    LT_RUN(scalar_modes);
    LT_TEST_MAIN_END
}
