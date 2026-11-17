/* test_logcap.c - logger capture decode, tooth/composite analysis, CSV and the demo ECU's
 * trigger logger + console over lt_proto. */
#include "lt.h"
#include "sim/sim.h"
#include "proto/proto.h"
#include "datalog/logcap.h"
#include "lt_test.h"

#include <math.h>
#include <string.h>

#define DEMO_INI "tests/fixtures/sim/demo.ini"

static double g_now;
static double clk_now(void *ud) { LT_UNUSED(ud); return g_now; }
static void clk_sleep(void *ud, int ms) { LT_UNUSED(ud); g_now += ms; }

/* ---- synthetic logger: toothTime + time ---- */
static const char *const SYN_INI =
    "[MegaTune]\n signature = \"x\"\n[LoggerDefinition]\n"
    " loggerDef = toothLogger, \"Tooth\", tooth\n"
    "  startCommand = \"H\"\n  stopCommand = \"h\"\n  dataReadCommand = \"T\"\n"
    "  continuousRead = true\n  recordDef = 0, 0, 4\n"
    "  recordField = toothTime, \"ToothTime\", 0, 32, 1.0, \"uS\"\n";

static lt_ini *load_str(const char *s) { return lt_ini_load_string(s, strlen(s), NULL, NULL); }

static void put32(lt_u8 *p, unsigned long v)
{
    p[0] = (lt_u8)((v >> 24) & 0xFF); p[1] = (lt_u8)((v >> 16) & 0xFF);
    p[2] = (lt_u8)((v >> 8) & 0xFF);  p[3] = (lt_u8)(v & 0xFF);
}

LT_TEST(feed_and_tooth_stats)
{
    lt_ini *ini = load_str(SYN_INI);
    lt_logcap c;
    lt_u8 pay[4 * 40];
    double us[64];
    size_t n, i;
    lt_tooth_stats st;
    lt_buf csv;
    LT_CHECK(ini != NULL && ini->n_loggers == 1);
    if (ini == NULL) { return; }
    lt_logcap_init(&c, lt_logcap_pick(ini, "tooth", 0));
    LT_CHECK(lt_logcap_field(&c, "toothTime") == 0);
    LT_CHECK(lt_logcap_pick(ini, "composite", 0) == NULL);
    /* 36-1 at 1000 us/tooth: 35 normal teeth then a 2000 us gap, then zero-filled records */
    for (i = 0; i < 40; i++) {
        put32(pay + i * 4, i < 35 ? 1000UL : (i == 35 ? 2000UL : 0UL));
    }
    LT_CHECK(lt_logcap_feed(&c, pay, sizeof(pay)) == 36);
    LT_CHECK(lt_logcap_feed(&c, pay, sizeof(pay)) == 0);          /* same buffer again: not ready */
    LT_CHECK(c.empty_reads == 1 && c.n_seen == 36);
    n = lt_logcap_tooth_us(&c, us, 64);
    LT_CHECK(n == 36);
    LT_CHECK_NEAR(us[0], 1000.0, 1e-9);
    LT_CHECK(lt_tooth_stats_calc(us, n, &st) == 0);
    LT_CHECK(st.missing_idx == 35 && st.missing_n == 1 && st.extra_n == 0);
    LT_CHECK_NEAR(st.min, 1000.0, 1e-9);
    LT_CHECK_NEAR(st.max, 2000.0, 1e-9);
    LT_CHECK(st.variability > 5.0);
    LT_CHECK(lt_tooth_class(1000.0, 1000.0) == 0 && lt_tooth_class(1100.0, 1000.0) == 1 &&
             lt_tooth_class(2000.0, 1000.0) == 2);
    /* 35 teeth + 2000 gap = one revolution of 37000 us -> 1621 rpm */
    LT_CHECK_NEAR(lt_tooth_rpm(us, n, 36), 60000000.0 / 37000.0, 1e-6);
    LT_CHECK(lt_tooth_rpm(us, 10, 36) == 0.0);
    lt_buf_init(&csv);
    lt_tooth_csv(us, 2, &csv);
    LT_CHECK(strncmp(csv.data, "Tooth Number,Time (us),Crank Angle (deg)\n0,1000,\n1,1000,\n", 20) == 0);
    LT_CHECK(strstr(csv.data, "\n0,1000,\n1,1000,\n") != NULL);
    lt_buf_free(&csv);
    lt_logcap_free(&c);
    lt_ini_free(ini);
}

LT_TEST(capture_keeps_latest)
{
    lt_ini *ini = load_str(SYN_INI);
    lt_logcap c;
    lt_u8 pay[4 * 100];
    int round;
    size_t i;
    if (ini == NULL) { LT_CHECK(0); return; }
    lt_logcap_init(&c, &ini->loggers[0]);
    for (round = 0; round < 260; round++) {
        for (i = 0; i < 100; i++) { put32(pay + i * 4, (unsigned long)(round * 100 + (int)i + 1)); }
        (void)lt_logcap_feed(&c, pay, sizeof(pay));
    }
    LT_CHECK(c.n_seen == 26000UL);
    LT_CHECK(c.n <= LT_LOGCAP_MAX && c.n > LT_LOGCAP_MAX - 1100);
    LT_CHECK_NEAR(lt_logcap_val(&c, c.n - 1, 0), 26000.0, 1e-9);     /* newest kept */
    lt_logcap_free(&c);
    lt_ini_free(ini);
}

/* ---- composite samples ---- */
LT_TEST(composite_stats)
{
    lt_comp_sample s[8];
    lt_comp_stats st;
    lt_buf csv;
    size_t i;
    int pri[8] = { 0, 1, 0, 1, 0, 1, 0, 1 }, sec[8] = { 0, 0, 1, 1, 0, 0, 0, 0 };
    int syn[8] = { 0, 0, 0, 1, 1, 0, 1, 1 };
    memset(s, 0, sizeof(s));
    for (i = 0; i < 8; i++) {
        s[i].t_us = 1000.0 * (double)i;
        s[i].lvl[0] = pri[i]; s[i].lvl[1] = sec[i]; s[i].lvl[2] = syn[i];
    }
    LT_CHECK(lt_comp_stats_calc(s, 8, &st) == 0);
    LT_CHECK(st.pri_pulses == 4 && st.sec_pulses == 1 && st.sync_lost == 1 && st.sync_acquired);
    LT_CHECK_NEAR(st.duration_ms, 7.0, 1e-9);
    LT_CHECK_NEAR(st.rate_hz, 1000.0, 1e-6);
    LT_CHECK(lt_comp_stats_calc(s, 1, &st) < 0);
    lt_buf_init(&csv);
    lt_comp_csv(s, 2, &csv);
    LT_CHECK(strstr(csv.data, "Time (us),Primary,Secondary,Sync,Trigger,Coil,Injector\n0,0,0,0,0,0,0\n1000,1,0,0,0,0,0\n") != NULL);
    lt_buf_free(&csv);
}

/* ---- the demo ECU: composite logger and console over lt_proto ---- */
LT_TEST(demo_trigger_logger_and_console)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *sim;
    lt_proto_io io;
    lt_proto_cfg cfg;
    lt_proto *p;
    lt_proto_clock ck;
    lt_logcap c;
    const lt_ini_logger *lg;
    lt_buf rep;
    double *us;
    lt_comp_sample *cs;
    size_t n, k;
    int round, got;
    lt_tooth_stats st;
    lt_comp_stats cst;
    char *out = NULL;
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    g_now = 10000.0;
    sim = lt_ecusim_new(ini);
    lt_ecusim_set_clock(sim, clk_now, NULL);
    io = lt_ecusim_io(sim);
    lt_proto_cfg_default(&cfg);
    p = lt_proto_new(&io, ini, &cfg);
    ck.ud = NULL; ck.now_ms = clk_now; ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    lt_ecusim_set_mode(sim, LT_SIM_IDLE);
    for (round = 0; round < 40; round++) { lt_ecusim_tick_engine(sim, 50); }

    lg = lt_logcap_pick(ini, "composite", 0);
    LT_CHECK(lg != NULL);
    if (lg == NULL) { return; }
    lt_logcap_init(&c, lg);
    LT_CHECK(lt_proto_logger_start(p, lg) == 0);
    lt_buf_init(&rep);
    for (round = 0, got = 0; round < 6; round++) {
        lt_buf_clear(&rep);
        LT_CHECK(lt_proto_logger_read(p, lg, &rep) == 0);
        LT_CHECK(rep.len == 500);
        (void)lt_logcap_feed(&c, (const lt_u8 *)rep.data, rep.len);
        got++;
    }
    LT_CHECK(got == 6);
    LT_CHECK(lt_proto_logger_stop(p, lg) == 0);
    lt_buf_clear(&rep);
    LT_CHECK(lt_proto_logger_read(p, lg, &rep) == 0);
    LT_CHECK(rep.len == 0);                                       /* stopped: nothing more to read */
    lt_buf_free(&rep);
    LT_CHECK(c.n == 600);

    cs = (lt_comp_sample *)lt_calloc(c.n, sizeof(*cs));
    n = lt_logcap_composite(&c, cs, c.n);
    LT_CHECK(n == c.n);
    for (k = 1; k < n; k++) { if (cs[k].t_us <= cs[k - 1].t_us) { break; } }
    LT_CHECK(k == n);                                             /* time strictly increases */
    LT_CHECK(lt_comp_stats_calc(cs, n, &cst) == 0);
    LT_CHECK(cst.pri_pulses > 100 && cst.sec_pulses >= 2 && cst.sync_acquired);
    LT_CHECK(lt_logcap_comp_mask(&c) == 0x3Fu);

    us = (double *)lt_calloc(c.n, sizeof(double));
    n = lt_logcap_tooth_us(&c, us, c.n);
    LT_CHECK(n > 100);
    LT_CHECK(lt_tooth_stats_calc(us, n, &st) == 0);
    LT_CHECK(st.missing_n >= 2 && st.extra_n == 0);               /* the 36-1 gap shows up each rev */
    {
        double rpm = lt_tooth_rpm(us, n, 35), set_rpm;
        lt_sim_snapshot sn;
        lt_simengine_snapshot(lt_ecusim_engine(sim), &sn);
        set_rpm = (double)sn.rpm;
        LT_CHECK(set_rpm > 50.0);
        LT_CHECK(fabs(rpm - set_rpm) / set_rpm < 0.03);
    }
    lt_free(us);
    lt_free(cs);
    lt_logcap_free(&c);

    /* console right after the logger: no stale replies in the way */
    LT_CHECK(lt_proto_console(p, "hello", 1000, &out) == 0);
    LT_CHECK(out != NULL && strstr(out, "demo ECU") != NULL);
    lt_free(out);
    out = NULL;
    LT_CHECK(lt_proto_console(p, "frobnicate", 1000, &out) == 0);
    LT_CHECK(out != NULL && strstr(out, "Unknown command: frobnicate") != NULL);
    lt_free(out);

    lt_proto_free(p);
    lt_ecusim_free(sim);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(feed_and_tooth_stats);
    LT_RUN(capture_keeps_latest);
    LT_RUN(composite_stats);
    LT_RUN(demo_trigger_logger_and_console);
    LT_TEST_MAIN_END
}
