/* test_atextra.c - AutoTune extras: heat map views, trace, load source, exports, delay test
 * planning and statistics, analysis report. */
#include <string.h>
#include "lt.h"
#include "lt_test.h"
#include "autotune/at_extra.h"

static void mkrec(lt_at_rec *r, unsigned x, unsigned y, double b, double v, unsigned h, double w)
{
    memset(r, 0, sizeof(*r));
    r->cell_x = x; r->cell_y = y; r->beginning_value = b; r->recommended_value = v;
    r->hit_count = h; r->hit_weighting = w;
}

LT_TEST(heat_fraction)
{
    lt_at_rec r[2];
    double mh, mw, mc;
    mkrec(&r[0], 0, 0, 50.0, 55.0, 10, 4.0);
    mkrec(&r[1], 1, 0, 60.0, 58.0, 5, 2.0);
    lt_atx_map_maxima(r, 2, &mh, &mw, &mc);
    LT_CHECK_NEAR(mh, 10.0, 1e-9);
    LT_CHECK_NEAR(mc, 5.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_map_fraction(LT_ATX_MAP_COVERAGE, &r[1], mh, mw, mc), 0.5, 1e-9);
    LT_CHECK_NEAR(lt_atx_map_fraction(LT_ATX_MAP_CHANGE, &r[1], mh, mw, mc), 0.4, 1e-9);
    LT_CHECK_NEAR(lt_atx_map_fraction(LT_ATX_MAP_WEIGHT, &r[0], mh, mw, mc), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_map_fraction(LT_ATX_MAP_VALUE, &r[0], mh, mw, mc), 0.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_map_fraction(LT_ATX_MAP_CHANGE, NULL, mh, mw, mc), 0.0, 1e-9);
    LT_CHECK_STR(lt_atx_map_name(LT_ATX_MAP_COVERAGE), "Coverage");
}

LT_TEST(trace_ring)
{
    lt_atx_trace t;
    unsigned x = 0, y = 0;
    size_t i;
    lt_atx_trace_clear(&t);
    lt_atx_trace_push(&t, 1, 1);
    lt_atx_trace_push(&t, 1, 1);
    lt_atx_trace_push(&t, 2, 1);
    LT_CHECK(lt_atx_trace_count(&t) == 2);
    LT_CHECK(lt_atx_trace_at(&t, 0, &x, &y) && x == 1);
    for (i = 0; i < LT_ATX_TRACE_MAX + 10; i++) { lt_atx_trace_push(&t, (unsigned)i, 0); }
    LT_CHECK(lt_atx_trace_count(&t) == LT_ATX_TRACE_MAX);
    LT_CHECK(lt_atx_trace_at(&t, LT_ATX_TRACE_MAX - 1, &x, &y) && x == LT_ATX_TRACE_MAX + 9);
    LT_CHECK(lt_atx_trace_at(&t, 0, &x, &y) && x == 10);
    LT_CHECK(!lt_atx_trace_at(&t, LT_ATX_TRACE_MAX, &x, &y));
}

LT_TEST(load_source)
{
    lt_at_channels ch;
    memset(&ch, 0, sizeof(ch));
    lt_strlcpy(ch.map, "MAPValue", sizeof(ch.map));
    lt_strlcpy(ch.tps, "TPSValue", sizeof(ch.tps));
    LT_CHECK(lt_atx_load_detect("TPSValue") == LT_ATX_LOAD_TPS);
    LT_CHECK(lt_atx_load_detect("throttle") == LT_ATX_LOAD_TPS);
    LT_CHECK(lt_atx_load_detect("MAFValue") == LT_ATX_LOAD_MAF);
    LT_CHECK(lt_atx_load_detect("map") == LT_ATX_LOAD_MAP);
    LT_CHECK(lt_atx_load_detect(NULL) == LT_ATX_LOAD_MAP);
    LT_CHECK(lt_atx_load_apply(&ch, LT_ATX_LOAD_TPS) == 1);
    LT_CHECK_STR(ch.load, "TPSValue");
    LT_CHECK(lt_atx_load_apply(&ch, LT_ATX_LOAD_MAF) == 0);
    LT_CHECK_STR(ch.load, "TPSValue");
}

LT_TEST(exports)
{
    lt_at_rec r[2];
    double xb[2], yb[2], cur[4], prop[4];
    lt_buf b;
    size_t ch;
    xb[0] = 1000; xb[1] = 2000; yb[0] = 40; yb[1] = 80;
    cur[0] = 50; cur[1] = 52; cur[2] = 60; cur[3] = 62;
    mkrec(&r[0], 1, 0, 52.0, 55.0, 7, 3.0);
    mkrec(&r[1], 0, 1, 60.0, 60.0, 2, 1.0);
    lt_buf_init(&b);
    lt_atx_csv(&b, r, 2, xb, 2, yb, 2);
    LT_CHECK(strstr(b.data, "cell_x,cell_y") == b.data);
    LT_CHECK(strstr(b.data, "1,0,2000.0000,40.0000,52.0000,55.0000,3.0000,5.77,7,3.0000") != NULL);
    lt_buf_clear(&b);
    ch = lt_atx_proposal_json(&b, "veTbl\"x", "sig", cur, 2, 2, r, 2);
    LT_CHECK(ch == 1);
    LT_CHECK(strstr(b.data, "\"tableName\": \"veTbl\\\"x\"") != NULL);
    LT_CHECK(strstr(b.data, "\"cellsChanged\": 1") != NULL);
    LT_CHECK(strstr(b.data, "\"largestDelta\": 3.0000") != NULL);
    lt_buf_free(&b);
    LT_CHECK(lt_atx_proposed(cur, 2, 2, r, 2, prop) == 1);
    LT_CHECK_NEAR(prop[1], 55.0, 1e-9);
    LT_CHECK_NEAR(prop[0], 50.0, 1e-9);
}

LT_TEST(delay_planning)
{
    lt_atx_delays d;
    lt_flow_fit fit;
    lt_buf b;
    size_t i;
    LT_CHECK_NEAR(lt_atx_clamp_step(-5.0), 3.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_clamp_step(8.0), 8.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_clamp_step(99.0), 20.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_step_value(100.0, 8.0), 108.0, 1e-9);
    LT_CHECK_NEAR(lt_atx_step_value(100.0, 0.0), 103.0, 1e-9);   /* never a lean step */
    LT_CHECK(lt_atx_clamp_hold(10.0) == 500ul);
    LT_CHECK(lt_atx_clamp_hold(99999.0) == 5000ul);
    LT_CHECK(lt_atx_clamp_repeats(0.0) == 1);
    LT_CHECK(lt_atx_clamp_repeats(1000.0) == LT_ATX_MAX_REPEATS);
    lt_atx_delays_init(&d);
    LT_CHECK(lt_atx_delay_model(&d, &fit) < 0);
    LT_CHECK_NEAR(lt_atx_delays_median(&d), 0.0, 1e-9);
    for (i = 0; i < 12; i++) {
        double rpm = 1500.0 + 400.0 * (double)i, load = 30.0 + 5.0 * (double)(i % 5);
        lt_atx_delays_add(&d, rpm, load, 400.0 - 20.0 * (double)i);
    }
    LT_CHECK(d.n == 12);
    LT_CHECK_NEAR(lt_atx_delays_median(&d), 290.0, 1e-9);
    lt_buf_init(&b);
    if (lt_atx_delay_model(&d, &fit) == 0) {
        lt_atx_delay_model_json(&b, &fit);
        LT_CHECK(strstr(b.data, "\"floorMs\"") != NULL);
    }
    lt_buf_free(&b);
}

LT_TEST(replay_report_text)
{
    lt_replay_report rep;
    lt_at_reject rj[2];
    unsigned cov[4];
    lt_buf b;
    memset(&rep, 0, sizeof(rep));
    rj[0].reason = "clt below min_clt"; rj[0].count = 30;
    rj[1].reason = "rpm out of range"; rj[1].count = 10;
    cov[0] = 5; cov[1] = 0; cov[2] = 1; cov[3] = 0;
    rep.samples_all = 100; rep.rejections = rj; rep.n_rejections = 2;
    rep.coverage = cov; rep.nx = 2; rep.ny = 2;
    rep.has_validation = LT_TRUE; rep.validation.gain_pct = 12.5; rep.validation.folds = 5;
    rep.validation.scored = 40;
    lt_buf_init(&b);
    lt_atx_replay_report(&b, &rep, 3);
    LT_CHECK(strstr(b.data, "Counted: 60 (60%)") != NULL);
    LT_CHECK(strstr(b.data, "clt below min_clt: 30") != NULL);
    LT_CHECK(strstr(b.data, "Cells with samples: 2 of 4") != NULL);
    LT_CHECK(strstr(b.data, "error reduced by 12.5%") != NULL);
    lt_buf_free(&b);
}

LT_TEST(ae_report)
{
    lt_log log;
    lt_at_channels ch;
    lt_buf b;
    lt_log_init(&log);
    memset(&ch, 0, sizeof(ch));
    lt_strlcpy(ch.tps_rate, "tpsDOT", sizeof(ch.tps_rate));
    lt_strlcpy(ch.afr, "afr", sizeof(ch.afr));
    lt_buf_init(&b);
    LT_CHECK(lt_atx_ae_report(&b, &log, &ch, 14.7) < 0);        /* empty log */
    lt_buf_free(&b);
    lt_log_free(&log);
}

int main(void)
{
    LT_RUN(heat_fraction);
    LT_RUN(trace_ring);
    LT_RUN(load_source);
    LT_RUN(exports);
    LT_RUN(delay_planning);
    LT_RUN(replay_report_text);
    LT_RUN(ae_report);
    LT_TEST_MAIN_END
}
