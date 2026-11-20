/* test_atextra.c - AutoTune extras: heat map views, trace, load source, exports, delay test
 * planning and statistics, analysis report, heat map zoom/pan, dyno overlay, sample verdicts. */
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
    rep.samples_all = 60; rep.rejections = rj; rep.n_rejections = 2;
    rep.coverage = cov; rep.nx = 2; rep.ny = 2;
    rep.has_validation = LT_TRUE; rep.validation.gain_pct = 12.5; rep.validation.folds = 5;
    rep.validation.scored = 40;
    lt_buf_init(&b);
    lt_atx_replay_report(&b, &rep, 3);
    LT_CHECK(strstr(b.data, "Samples in log: 100") != NULL);   /* samples_all = accepted */
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

LT_TEST(zoom_pan)
{
    int z, i;
    LT_CHECK_INT(lt_atx_zoom_clamp(5), LT_ATX_ZOOM_MIN);
    LT_CHECK_INT(lt_atx_zoom_clamp(9000), LT_ATX_ZOOM_MAX);
    LT_CHECK_INT(lt_atx_zoom_step(100, 1), 125);
    LT_CHECK_INT(lt_atx_zoom_step(100, -1), 80);
    LT_CHECK_INT(lt_atx_zoom_step(125, -1), 100);
    LT_CHECK_INT(lt_atx_zoom_step(80, 1), 100);          /* back to 100 exactly */
    LT_CHECK_INT(lt_atx_zoom_step(100, 2), 156);
    LT_CHECK_INT(lt_atx_zoom_step(LT_ATX_ZOOM_MAX, 1), LT_ATX_ZOOM_MAX);
    LT_CHECK_INT(lt_atx_zoom_step(LT_ATX_ZOOM_MIN, -3), LT_ATX_ZOOM_MIN);
    LT_CHECK_INT(lt_atx_zoom_step(100, 0), 100);
    /* repeated steps reach both ends and never stall */
    z = 100;
    for (i = 0; i < 30; i++) { z = lt_atx_zoom_step(z, 1); }
    LT_CHECK_INT(z, LT_ATX_ZOOM_MAX);
    for (i = 0; i < 30; i++) { z = lt_atx_zoom_step(z, -1); }
    LT_CHECK_INT(z, LT_ATX_ZOOM_MIN);
    /* fit: 32 columns of 66 px into 990 px -> 46%; rows limit when they are tighter */
    LT_CHECK_INT(lt_atx_zoom_fit(990, 2000, 32, 16, 66, 24), 46);
    LT_CHECK_INT(lt_atx_zoom_fit(5000, 384, 8, 32, 66, 24), 50);
    LT_CHECK_INT(lt_atx_zoom_fit(5000, 5000, 8, 8, 66, 24), 100);   /* never zooms in to fit */
    LT_CHECK_INT(lt_atx_zoom_fit(10, 10, 64, 64, 66, 24), LT_ATX_ZOOM_MIN);
    LT_CHECK_INT(lt_atx_zoom_fit(0, 10, 4, 4, 66, 24), LT_ATX_ZOOM_DEF);
    LT_CHECK_INT(lt_atx_zoom_px(66, 50, 12), 33);
    LT_CHECK_INT(lt_atx_zoom_px(24, 25, 8), 8);
    LT_CHECK_INT(lt_atx_zoom_px(24, 200, 8), 48);
    LT_CHECK_INT(lt_atx_zoom_font(12, 100), 12);
    LT_CHECK_INT(lt_atx_zoom_font(12, 50), 8);
    LT_CHECK_INT(lt_atx_zoom_font(12, 150), 18);
    LT_CHECK_INT(lt_atx_zoom_font(12, 400), 22);
    /* pan limits */
    LT_CHECK_INT(lt_atx_pan_clamp(-5, 1000, 300), 0);
    LT_CHECK_INT(lt_atx_pan_clamp(900, 1000, 300), 700);
    LT_CHECK_INT(lt_atx_pan_clamp(50, 200, 300), 0);         /* content smaller than the view */
    /* anchored zoom: the content point under the mouse stays put.  Cell 50 -> 100 px, 20 cells,
     * 400 px view, scroll 100, mouse 150 px in: point 250 = 5 cells -> 500, scroll 350 */
    LT_CHECK_INT(lt_atx_zoom_anchor(100, 150, 50, 100, 20, 400), 350);
    LT_CHECK_INT(lt_atx_zoom_anchor(350, 150, 100, 50, 20, 400), 100);   /* and back */
    LT_CHECK_INT(lt_atx_zoom_anchor(0, 0, 50, 100, 20, 400), 0);
    LT_CHECK_INT(lt_atx_zoom_anchor(600, 400, 50, 25, 20, 400), 100);    /* clamps to the new max */
    LT_CHECK_INT(lt_atx_zoom_anchor(100, 10, 0, 50, 20, 400), 0);
    /* axis label stride */
    LT_CHECK_INT(lt_atx_label_stride(24, 16), 1);
    LT_CHECK_INT(lt_atx_label_stride(8, 16), 2);
    LT_CHECK_INT(lt_atx_label_stride(7, 16), 3);
    LT_CHECK_INT(lt_atx_label_stride(0, 16), 1);
}

LT_TEST(dyno_overlay)
{
    lt_dyno_cell c[3];
    int idx[6];
    double lo, hi, v = 0.0;
    char b[32];
    memset(c, 0, sizeof(c));
    c[0].row = 1; c[0].col = 0; c[0].has_hp = LT_TRUE; c[0].hp = 120.4; c[0].samples = 2;
    c[0].has_afr = LT_TRUE; c[0].afr = 12.64;
    c[1].row = 1; c[1].col = 2; c[1].has_hp = LT_TRUE; c[1].hp = 180.0; c[1].has_tq = LT_TRUE; c[1].tq_val = 201.0;
    c[2].row = 9; c[2].col = 0;                     /* outside the table: ignored */
    lt_atx_dyno_index(c, 3, 2, 3, idx);
    LT_CHECK_INT(idx[0], -1);
    LT_CHECK_INT(idx[3], 0);
    LT_CHECK_INT(idx[5], 1);
    LT_CHECK_INT(idx[4], -1);
    LT_CHECK(lt_atx_dyno_value(&c[0], LT_ATX_DYNO_HP, &v));
    LT_CHECK_NEAR(v, 120.4, 1e-9);
    LT_CHECK(!lt_atx_dyno_value(&c[0], LT_ATX_DYNO_TQ, &v));
    LT_CHECK(!lt_atx_dyno_value(NULL, LT_ATX_DYNO_HP, &v));
    LT_CHECK(lt_atx_dyno_range(c, 3, LT_ATX_DYNO_HP, &lo, &hi));
    LT_CHECK_NEAR(lo, 120.4, 1e-9);
    LT_CHECK_NEAR(hi, 180.0, 1e-9);
    LT_CHECK(lt_atx_dyno_range(c, 3, LT_ATX_DYNO_TQ, &lo, &hi));
    LT_CHECK_NEAR(lo, 201.0, 1e-9);
    LT_CHECK(!lt_atx_dyno_range(c + 2, 1, LT_ATX_DYNO_AFR, &lo, &hi));
    LT_CHECK_NEAR(hi, 0.0, 1e-9);
    lt_atx_dyno_label(&c[0], LT_ATX_DYNO_HP, b, sizeof(b));
    LT_CHECK_STR(b, "120 hp");
    lt_atx_dyno_label(&c[1], LT_ATX_DYNO_TQ, b, sizeof(b));
    LT_CHECK_STR(b, "201 ft");
    lt_atx_dyno_label(&c[0], LT_ATX_DYNO_AFR, b, sizeof(b));
    LT_CHECK_STR(b, "12.6");
    lt_atx_dyno_label(&c[1], LT_ATX_DYNO_AFR, b, sizeof(b));
    LT_CHECK_STR(b, "");
    LT_CHECK_STR(lt_atx_dyno_name(LT_ATX_DYNO_TQ), "Torque");
}

LT_TEST(sample_verdicts)
{
    static const char *const names[3] = { "RPMValue", "MAPValue", "AFRValue" };
    static const double xb[3] = { 1000.0, 2000.0, 3000.0 };
    static const double yb[2] = { 30.0, 90.0 };
    const char *vd[8];
    lt_replay_report rep;
    lt_log log;
    lt_at_channels ch;
    lt_atx_verdicts v;
    lt_atx_vfilter f;
    size_t idx[8], n, i;
    double row[3];
    lt_log_init(&log);
    lt_log_set_channels(&log, names, NULL, 3);
    for (i = 0; i < 8; i++) {
        row[0] = 900.0 + 300.0 * (double)i;      /* 900 .. 3000 rpm */
        row[1] = i < 4 ? 35.0 : 85.0;
        row[2] = 14.0;
        lt_log_append(&log, 0.1 * (double)i, row);
    }
    vd[0] = "rpm out of range"; vd[1] = NULL; vd[2] = "clt below min_clt"; vd[3] = NULL;
    vd[4] = "clt below min_clt"; vd[5] = NULL; vd[6] = "clt below min_clt"; vd[7] = "rpm out of range";
    memset(&rep, 0, sizeof(rep));
    rep.verdicts = vd; rep.n_verdicts = 8;
    memset(&ch, 0, sizeof(ch));
    lt_strlcpy(ch.rpm, "RPMValue", sizeof(ch.rpm));
    lt_strlcpy(ch.load, "MAPValue", sizeof(ch.load));
    lt_strlcpy(ch.afr, "AFRValue", sizeof(ch.afr));
    lt_atx_verdicts_init(&v);
    LT_CHECK_INT(lt_atx_verdicts_build(&v, &rep, &log, &ch, xb, 3, yb, 2), 0);
    LT_CHECK(v.n == 8);
    LT_CHECK(v.counted == 3);
    LT_CHECK(v.nreason == 2);
    LT_CHECK_STR(v.reason[0], "clt below min_clt");       /* most frequent first */
    LT_CHECK(v.count[0] == 3 && v.count[1] == 2);
    LT_CHECK_INT(v.rows[0].reason, 1);
    LT_CHECK_INT(v.rows[2].reason, 0);
    LT_CHECK_INT(v.rows[1].reason, -1);
    LT_CHECK_NEAR(v.rows[3].t, 0.3, 1e-9);
    LT_CHECK_NEAR(v.rows[3].rpm, 1800.0, 1e-9);
    LT_CHECK(v.rows[3].cx == 1 && v.rows[3].cy == 0);     /* 1800 rpm -> 2000, 35 kPa -> 30 */
    LT_CHECK(v.rows[7].cx == 2 && v.rows[7].cy == 1);
    /* filters */
    f.kind = LT_ATX_VF_ALL; f.reason = 0; f.cx = -1; f.cy = -1;
    LT_CHECK(lt_atx_verdicts_select(&v, &f, NULL, 0) == 8);
    f.kind = LT_ATX_VF_COUNTED;
    n = lt_atx_verdicts_select(&v, &f, idx, 8);
    LT_CHECK(n == 3 && idx[0] == 1 && idx[1] == 3 && idx[2] == 5);
    f.kind = LT_ATX_VF_DROPPED;
    LT_CHECK(lt_atx_verdicts_select(&v, &f, idx, 8) == 5);
    f.kind = LT_ATX_VF_REASON; f.reason = 1;
    n = lt_atx_verdicts_select(&v, &f, idx, 1);         /* count is complete even past cap */
    LT_CHECK(n == 2 && idx[0] == 0);
    f.kind = LT_ATX_VF_ALL; f.cx = 0; f.cy = 0;
    n = lt_atx_verdicts_select(&v, &f, idx, 8);
    LT_CHECK(n == 3 && idx[0] == 0 && idx[2] == 2);      /* 900..1500 rpm (a tie -> lower bin) */
    f.kind = LT_ATX_VF_COUNTED;
    LT_CHECK(lt_atx_verdicts_select(&v, &f, idx, 8) == 1);
    LT_CHECK(!lt_atx_verdict_match(&v, 99, &f));
    /* timeline columns */
    LT_CHECK_INT(lt_atx_verdict_dominant(&v, 2, 8), 0);
    LT_CHECK_INT(lt_atx_verdict_dominant(&v, 0, 8), -1);   /* 3 counted vs 3 clt: counted wins */
    LT_CHECK_INT(lt_atx_verdict_dominant(&v, 0, 2), -1);   /* tie: counted ranks first */
    LT_CHECK_INT(lt_atx_verdict_dominant(&v, 7, 8), 1);
    LT_CHECK_INT(lt_atx_verdict_dominant(&v, 8, 9), -2);
    LT_CHECK(lt_atx_verdict_row_at(8, 4, 0) == 0);
    LT_CHECK(lt_atx_verdict_row_at(8, 4, 3) == 6);
    LT_CHECK(lt_atx_verdict_row_at(8, 4, 99) == 6);
    LT_CHECK(lt_atx_verdict_row_at(8, 400, 399) == 7);
    LT_CHECK(lt_atx_verdict_row_at(0, 4, 2) == 0);
    rep.verdicts = NULL;
    LT_CHECK(lt_atx_verdicts_build(&v, &rep, &log, &ch, xb, 3, yb, 2) < 0);
    LT_CHECK(v.n == 0 && v.rows == NULL);
    lt_atx_verdicts_free(&v);
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
    LT_RUN(zoom_pan);
    LT_RUN(dyno_overlay);
    LT_RUN(sample_verdicts);
    LT_TEST_MAIN_END
}
