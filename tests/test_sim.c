/* test_sim.c - engine physics, VE model, och codec, virtual ECU wire protocol and
 * an in-process demo-mode integration with lt_proto over demo.ini. */
#include "lt.h"
#include "sim/sim.h"
#include "proto/proto.h"
#include "lt_test.h"

#include <math.h>
#include <string.h>

#ifndef GOLDEN_SUM
#define GOLDEN_SUM 1014896L   /* cross-checked against an independent model of spec 6 */
#define GOLDEN_RPM 2120
#define GOLDEN_MODE 3
#endif
#define DEMO_INI "tests/fixtures/sim/demo.ini"

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */
static lt_ini *ini_str(const char *s)
{
    return lt_ini_load_string(s, strlen(s), NULL, NULL);
}

static double g_now;
static double clk_now(void *ud)
{
    LT_UNUSED(ud);
    return g_now;
}
static void clk_sleep(void *ud, int ms)
{
    LT_UNUSED(ud);
    g_now += ms;
}

/* send a plain byte string, return the number of reply bytes in out */
static size_t xfer(lt_ecusim *s, const void *cmd, size_t n, lt_u8 *out, size_t cap)
{
    long r;

    lt_ecusim_clear_input(s);
    lt_ecusim_write(s, (const lt_u8 *)cmd, n);
    r = lt_ecusim_read(s, out, cap);
    return r < 0 ? 0 : (size_t)r;
}

/* enveloped request; fills reply payload (incl. status byte). returns payload len, -1 bad */
static int env_xfer(lt_ecusim *s, lt_env_order o, const lt_u8 *p, size_t n, lt_u8 *out, size_t cap)
{
    lt_buf f, rx;
    const lt_u8 *pl;
    size_t pn, used;
    int rc;
    long r;

    lt_buf_init(&f);
    lt_buf_init(&rx);
    lt_env_encode(o, p, n, &f);
    lt_ecusim_clear_input(s);
    lt_ecusim_write(s, (const lt_u8 *)f.data, f.len);
    lt_buf_reserve(&rx, 70000);
    r = lt_ecusim_read(s, (lt_u8 *)rx.data, 70000);
    if (r <= 0) {
        lt_buf_free(&f);
        lt_buf_free(&rx);
        return -1;
    }
    rc = lt_env_decode(o, LT_FALSE, (const lt_u8 *)rx.data, (size_t)r, &pl, &pn, &used);
    if (rc == 0 && pn <= cap) {
        memcpy(out, pl, pn);
        rc = (int)pn;
    } else {
        rc = -1;
    }
    lt_buf_free(&f);
    lt_buf_free(&rx);
    return rc;
}

static const char *MINI_LE =
    "[MegaTune]\n signature = \"rusEFI test.1\"\n queryCommand = \"S\"\n versionInfo = \"V\"\n"
    "[Constants]\n messageEnvelopeFormat = msEnvelope_1.0\n endianness = little\n nPages = 2\n"
    " pageSize = 64, 32\n"
    " pageReadCommand = \"R%2i%2o%2c\", \"R%2i%2o%2c\"\n"
    "page = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "[OutputChannels]\n ochBlockSize = 16\n"
    " rpm = scalar, U16, 0, \"rpm\", 1, 0\n"
    " map = scalar, U08, 2, \"kPa\", 1, 0\n";

static const char *MINI_BE_PLAIN =
    "[MegaTune]\n signature = \"speeduino 202501-sim\"\n queryCommand = \"Q\"\n versionInfo = \"S\"\n"
    "[Constants]\n endianness = big\n nPages = 2\n pageSize = 64, 32\n"
    " pageReadCommand = \"p%2i%2o%2c\", \"p%2i%2o%2c\"\n"
    "page = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "[OutputChannels]\n ochBlockSize = 16\n"
    " rpm = scalar, U16, 0, \"rpm\", 1, 0\n";

/* ------------------------------------------------------------------ */
/* engine                                                               */
/* ------------------------------------------------------------------ */
LT_TEST(engine_initial_state)
{
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;

    lt_simengine_snapshot(e, &s);
    LT_CHECK(lt_simengine_mode(e) == LT_SIM_STARTUP);
    LT_CHECK(s.rpm == 0 && s.secl == 0 && s.map_kpa == 100 && s.baro_kpa == 100);
    LT_CHECK(s.coolant_c == 20 && s.iat_c == 20 && s.tps_percent == 2);
    LT_CHECK(s.battery_dv == 140 && s.cranking && !s.running);
    LT_CHECK_NEAR(s.afr, 14.7, 1e-12);
    LT_CHECK_NEAR(s.afr_target, 14.7, 1e-12);
    LT_CHECK_NEAR(s.ego_correction, 100.0, 1e-12);
    lt_simengine_free(e);
}

LT_TEST(engine_startup_to_idle)
{
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;
    int i;
    int saw_warm = 0;

    for (i = 0; i < 2000; i++) {
        lt_simengine_tick(e, 50);
        if (lt_simengine_mode(e) == LT_SIM_WARMUP_IDLE) {
            saw_warm = 1;
        }
    }
    lt_simengine_snapshot(e, &s);
    LT_CHECK(saw_warm);
    LT_CHECK(lt_simengine_mode(e) != LT_SIM_STARTUP);
    LT_CHECK(s.running && !s.cranking);
    LT_CHECK(s.coolant_c > 60);
    LT_CHECK(s.rpm >= 0 && s.rpm <= 7000);
    LT_CHECK(s.tps_percent >= 0 && s.tps_percent <= 100);
    LT_CHECK(s.map_kpa >= 0 && s.map_kpa <= 100);
    LT_CHECK(s.advance_deg >= 5 && s.advance_deg <= 35);
    LT_CHECK(s.battery_dv >= 137 && s.battery_dv <= 143);
    lt_simengine_free(e);
}

LT_TEST(engine_cranking_rpm_step)
{
    /* Startup: target 900, accel 500 -> +25 rpm per 50 ms step */
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;

    lt_simengine_tick(e, 50);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.rpm == 25);
    lt_simengine_tick(e, 49);       /* accumulates, no step */
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.rpm == 25);
    lt_simengine_tick(e, 1);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.rpm == 50);
    lt_simengine_free(e);
}

LT_TEST(engine_wot_and_modes)
{
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;
    int i;

    lt_simengine_set_mode(e, LT_SIM_WOT);
    lt_simengine_tick(e, 1000);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.rpm >= 1000);
    LT_CHECK(lt_simengine_mode(e) == LT_SIM_WOT);
    lt_simengine_tick(e, 3000);
    LT_CHECK(lt_simengine_mode(e) != LT_SIM_WOT);
    /* deceleration leaves within 600 ticks (30 s) */
    lt_simengine_set_mode(e, LT_SIM_DECELERATION);
    for (i = 0; i < 600 && lt_simengine_mode(e) == LT_SIM_DECELERATION; i++) {
        lt_simengine_tick(e, 50);
    }
    LT_CHECK(lt_simengine_mode(e) != LT_SIM_DECELERATION);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.rpm < 1100);     /* exit threshold; the decel target is 900..1049 */
    lt_simengine_free(e);
}

LT_TEST(engine_thermal_secl)
{
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;

    lt_simengine_tick(e, 60000);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.coolant_c > 20);
    LT_CHECK(s.secl == 60);
    lt_simengine_reset_secl(e);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.secl == 0);
    lt_simengine_advance_secl(e, 250);
    lt_simengine_advance_secl(e, 10);
    lt_simengine_snapshot(e, &s);
    LT_CHECK(s.secl == 4);
    lt_simengine_free(e);
}

LT_TEST(engine_deterministic)
{
    lt_simengine *a = lt_simengine_new();
    lt_simengine *b = lt_simengine_new();
    lt_sim_snapshot sa, sb;
    unsigned dts[7] = {10, 50, 123, 7, 400, 50, 1000};
    int i, same = 1;

    for (i = 0; i < 300; i++) {
        unsigned dt = dts[i % 7];

        lt_simengine_tick(a, dt);
        lt_simengine_tick(b, dt);
        lt_simengine_snapshot(a, &sa);
        lt_simengine_snapshot(b, &sb);
        if (memcmp(&sa, &sb, sizeof(sa)) != 0) {
            same = 0;
        }
    }
    LT_CHECK(same);
    /* different chunking, same total time -> same state */
    lt_simengine_free(b);
    b = lt_simengine_new();
    lt_simengine_tick(b, 50 * 100);
    {
        lt_simengine *c = lt_simengine_new();
        int j;

        for (j = 0; j < 100; j++) {
            lt_simengine_tick(c, 50);
        }
        lt_simengine_snapshot(b, &sa);
        lt_simengine_snapshot(c, &sb);
        LT_CHECK(memcmp(&sa, &sb, sizeof(sa)) == 0);
        lt_simengine_free(c);
    }
    lt_simengine_free(a);
    lt_simengine_free(b);
}

LT_TEST(engine_golden)
{
    /* Regression pins for the exact xorshift draw order (seed 0x4F545531). */
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;
    int i;
    long sum = 0;

    for (i = 0; i < 400; i++) {
        lt_simengine_tick(e, 50);
        lt_simengine_snapshot(e, &s);
        sum += s.rpm + 3 * s.map_kpa + 5 * s.tps_percent + 7 * s.battery_dv + 11 * s.advance_deg;
    }
    lt_simengine_snapshot(e, &s);
    LT_CHECK(sum == GOLDEN_SUM);
    LT_CHECK(s.rpm == GOLDEN_RPM);
    LT_CHECK((int)lt_simengine_mode(e) == GOLDEN_MODE);
    lt_simengine_free(e);
}

/* ------------------------------------------------------------------ */
/* VE model                                                             */
/* ------------------------------------------------------------------ */
LT_TEST(true_ve_values)
{
    LT_CHECK(lt_sim_true_ve(3000, 80) > lt_sim_true_ve(3000, 40));
    LT_CHECK(lt_sim_true_ve(5000, 60) > lt_sim_true_ve(1000, 60));
    LT_CHECK_NEAR(lt_sim_true_ve(-1e9, -1e9), 20.0, 1e-12);
    LT_CHECK_NEAR(lt_sim_true_ve(1e9, 1e9), 110.0, 1e-12);
    LT_CHECK_NEAR(lt_sim_true_ve(3000, 60), 40.0 + 15.0 + 7.5, 1e-12);
}

/* afr = 14.7 * true_ve / current_ve, so a flat VE of 50 makes current_ve exact */
static double afr_with_ve(const double *rb, size_t nx, const double *lb, size_t ny,
                          const double *ve, int *rpm, int *map)
{
    lt_simengine *e = lt_simengine_new();
    lt_sim_snapshot s;
    double afr;

    lt_simengine_set_ve(e, rb, nx, lb, ny, ve);
    lt_simengine_tick(e, 2000);
    lt_simengine_snapshot(e, &s);
    *rpm = s.rpm;
    *map = s.map_kpa;
    afr = s.afr;
    lt_simengine_free(e);
    return afr;
}

LT_TEST(ve_context_interp_and_usable)
{
    double rb[2] = {1000, 2000}, lb[2] = {40, 80};
    double ve[4] = {50, 60, 70, 80};
    double flat[4] = {50, 50, 50, 50};
    double zero[4] = {0, 0, 0, 0};
    double bad[2] = {1000, 1000};
    int rpm, map;
    double afr, cur, want;

    /* flat 50 -> current = 50 everywhere */
    afr = afr_with_ve(rb, 2, lb, 2, flat, &rpm, &map);
    want = 14.7 * lt_sim_true_ve(rpm, map) / 50.0;
    LT_CHECK_NEAR(afr, want, 1e-9);
    /* bilinear: clamp outside the axes -> e.g. rpm 1500 map 60 equals 65; check formula */
    afr = afr_with_ve(rb, 2, lb, 2, ve, &rpm, &map);
    {
        double fx = (rpm - 1000.0) / 1000.0, fy = (map - 40.0) / 40.0, top, bot;

        fx = fx < 0 ? 0 : (fx > 1 ? 1 : fx);
        fy = fy < 0 ? 0 : (fy > 1 ? 1 : fy);
        top = 50 * (1 - fx) + 60 * fx;
        bot = 70 * (1 - fx) + 80 * fx;
        cur = top * (1 - fy) + bot * fy;
        want = 14.7 * lt_sim_true_ve(rpm, map) / cur;
        LT_CHECK_NEAR(afr, want, 1e-9);
    }
    /* unusable contexts leave afr at 14.7 */
    LT_CHECK_NEAR(afr_with_ve(rb, 2, lb, 2, zero, &rpm, &map), 14.7, 1e-12);
    LT_CHECK_NEAR(afr_with_ve(bad, 2, lb, 2, flat, &rpm, &map), 14.7, 1e-12);
    LT_CHECK_NEAR(afr_with_ve(rb, 1, lb, 2, flat, &rpm, &map), 14.7, 1e-12);
    LT_CHECK_NEAR(afr_with_ve(NULL, 0, NULL, 0, NULL, &rpm, &map), 14.7, 1e-12);
}

/* ------------------------------------------------------------------ */
/* och codec                                                            */
/* ------------------------------------------------------------------ */
static lt_sim_snapshot snap_of(int rpm, int map)
{
    lt_sim_snapshot s;

    memset(&s, 0, sizeof(s));
    s.rpm = rpm;
    s.map_kpa = map;
    s.afr = 14.35;
    s.afr_target = 14.7;
    s.baro_kpa = 100;
    s.battery_dv = 138;
    return s;
}

LT_TEST(och_u16_scale_translate)
{
    lt_ini *ini = ini_str("[OutputChannels]\n rpm = scalar, U16, 2, \"rpm\", 0.5, 100\n");
    lt_u8 blk[8];
    lt_sim_snapshot s = snap_of(3000, 50);
    double v;

    LT_CHECK(ini != NULL);
    memset(blk, 0, sizeof(blk));
    lt_sim_encode_och(ini, &s, blk, sizeof(blk));
    LT_CHECK(lt_ini_channel_decode(lt_ini_find_channel(ini, "rpm"), LT_ENDIAN_BIG, blk, 8, &v) == 0);
    LT_CHECK_NEAR(v, 3000.0, 1e-9);
    LT_CHECK(blk[0] == 0 && blk[1] == 0 && blk[4] == 0);
    /* raw = (3000-100)/0.5 = 5800 = 0x16A8 */
    LT_CHECK(blk[2] == 0x16 && blk[3] == 0xA8);
    LT_CHECK(lt_sim_block_size(ini) == 4);
    lt_ini_free(ini);
}

LT_TEST(och_endianness)
{
    lt_ini *be = ini_str("[Constants]\n endianness = big\n[OutputChannels]\n"
                         " rpm = scalar, U16, 0, \"rpm\", 1, 0\n");
    lt_ini *le = ini_str("[Constants]\n endianness = little\n[OutputChannels]\n"
                         " rpm = scalar, U16, 0, \"rpm\", 1, 0\n");
    lt_u8 b[2];
    lt_sim_snapshot s = snap_of(4660, 0);

    lt_sim_encode_och(be, &s, b, 2);
    LT_CHECK(b[0] == 0x12 && b[1] == 0x34);
    lt_sim_encode_och(le, &s, b, 2);
    LT_CHECK(b[0] == 0x34 && b[1] == 0x12);
    lt_ini_free(be);
    lt_ini_free(le);
}

LT_TEST(och_float_and_saturation)
{
    lt_ini *ini = ini_str("[OutputChannels]\n"
                          " afr = scalar, F32, 0, \"\", 1, 0\n"
                          " tiny = scalar, F32, 4, \"\", 1e-30, 0\n"
                          " map = scalar, U08, 8, \"\", 0.1, 0\n"
                          " late = scalar, U16, 7, \"\", 1, 0\n");
    lt_u8 blk[8];
    lt_sim_snapshot s = snap_of(0, 30);
    double v;

    memset(blk, 0, sizeof(blk));
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(lt_ini_channel_decode(lt_ini_find_channel(ini, "afr"), LT_ENDIAN_BIG, blk, 8, &v) == 0);
    LT_CHECK_NEAR(v, 14.35, 1e-4);
    /* "tiny" is not a known name -> untouched; "late" is unknown too */
    LT_CHECK(blk[4] == 0 && blk[7] == 0);
    lt_ini_free(ini);

    ini = ini_str("[OutputChannels]\n afr = scalar, F32, 0, \"\", 1e-30, 0\n"
                  " rpm = scalar, U08, 4, \"\", 1, 0\n rpm2 = scalar, U16, 7, \"\", 1, 0\n"
                  " map = scalar, U16, 6, \"\", 1, 0\n");
    s = snap_of(300, 7);
    s.afr = 1.0;
    memset(blk, 0, sizeof(blk));
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(lt_ini_channel_decode(lt_ini_find_channel(ini, "afr"), LT_ENDIAN_BIG, blk, 8, &v) == 0);
    LT_CHECK(lt_isfinite(v));
    LT_CHECK(blk[4] == 255);            /* 300 saturates */
    LT_CHECK(blk[6] == 0 && blk[7] == 7);
    lt_ini_free(ini);

    /* U16 at offset 7 in 8 bytes: nothing written */
    ini = ini_str("[OutputChannels]\n rpm = scalar, U16, 7, \"\", 1, 0\n");
    memset(blk, 0, sizeof(blk));
    s = snap_of(0x1234, 0);
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(blk[7] == 0 && blk[6] == 0);
    lt_ini_free(ini);
}

LT_TEST(och_bits_and_computed)
{
    lt_ini *ini = ini_str("[OutputChannels]\n"
                          " running = bits, U08, 0, [3:3]\n"
                          " cranking = bits, U08, 0, [1:1]\n"
                          " rpm = scalar, U16, 4, \"\", 1, 0\n"
                          " rpm2 = { rpm * 2 }\n"
                          " engine = scalar, U08, 2, \"\", 1, 0\n");
    lt_u8 blk[8];
    lt_sim_snapshot s = snap_of(1000, 0);

    LT_CHECK(ini != NULL);
    memset(blk, 0, sizeof(blk));
    blk[0] = 0xA5;                       /* 1010 0101 */
    s.running = LT_TRUE;
    s.cranking = LT_FALSE;
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(blk[0] == 0xAD);            /* bit 3 set, bit 1 cleared (was 0), neighbours kept */
    LT_CHECK(blk[2] == 1);               /* engine = running */
    s.running = LT_FALSE;
    s.cranking = LT_TRUE;
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(blk[0] == 0xA7);
    LT_CHECK(blk[2] == 2);
    lt_ini_free(ini);

    /* bit 11 of a U32 at offset 0: byte 1 value 0x08 little endian, byte 2 big endian */
    ini = ini_str("[Constants]\n endianness = little\n[OutputChannels]\n"
                  " running = bits, U32, 0, [11:11]\n");
    memset(blk, 0, sizeof(blk));
    s.running = LT_TRUE;
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(blk[1] == 0x08 && blk[2] == 0);
    lt_ini_free(ini);
    ini = ini_str("[Constants]\n endianness = big\n[OutputChannels]\n"
                  " running = bits, U32, 0, [11:11]\n");
    memset(blk, 0, sizeof(blk));
    lt_sim_encode_och(ini, &s, blk, 8);
    LT_CHECK(blk[2] == 0x08 && blk[1] == 0);
    lt_ini_free(ini);
}

LT_TEST(och_block_size)
{
    lt_ini *ini = ini_str("[OutputChannels]\n a = scalar, U16, 4, \"\", 1, 0\n"
                          " b = scalar, F32, 10, \"\", 1, 0\n c = { a + 1 }\n");
    LT_CHECK(lt_sim_block_size(ini) == 14);
    lt_ini_free(ini);
    ini = ini_str("[OutputChannels]\n ochBlockSize = 99\n a = scalar, U16, 4, \"\", 1, 0\n");
    LT_CHECK(lt_sim_block_size(ini) == 99);
    lt_ini_free(ini);
    ini = ini_str("[Constants]\n");
    LT_CHECK(lt_sim_block_size(ini) == 0);
    lt_ini_free(ini);
    LT_CHECK(lt_sim_block_size(NULL) == 0);
}

/* ------------------------------------------------------------------ */
/* VE seeding                                                           */
/* ------------------------------------------------------------------ */
static const char *VE_INI =
    "[MegaTune]\n signature = \"ve test\"\n"
    "[Constants]\n nPages = 1\n pageSize = 64\n"
    "page = 1\n"
    " rpmBins = array, U08, 0, [4], \"RPM\", 100, 0, 0, 25500, 0\n"
    " loadBins = array, U08, 4, [4], \"kPa\", 1, 0, 0, 255, 0\n"
    " veTable = array, U08, 8, [4x4], \"%\", 1, 0, 0, 255, 0\n"
    "[TableEditor]\n table = veTbl, veMap, \"VE\", 1\n xBins = rpmBins, rpm\n"
    " yBins = loadBins, map\n zBins = veTable\n"
    "[VeAnalyze]\n veAnalyzeMap = veTbl, veTbl, afr, ego, { 1 }\n";

typedef struct bufsrc { lt_u8 mem[64]; } bufsrc;
static const lt_u8 *bs_page(void *ud, int page, size_t *len)
{
    if (page != 0) {
        return NULL;
    }
    *len = 64;
    return ((bufsrc *)ud)->mem;
}
static int bs_write(void *ud, int page, size_t off, const lt_u8 *d, size_t n)
{
    if (page != 0 || off + n > 64) {
        return LT_ERANGE;
    }
    memcpy(((bufsrc *)ud)->mem + off, d, n);
    return 0;
}

LT_TEST(seed_ve_table_values)
{
    lt_ini *ini = ini_str(VE_INI);
    bufsrc b;
    lt_pagesrc src;
    double rpm[4], load[4], z[16];
    size_t i;

    LT_CHECK(ini != NULL);
    memset(&b, 0, sizeof(b));
    src.ud = &b; src.page = bs_page; src.write = bs_write;
    LT_CHECK(lt_sim_seed_ve_table(ini, &src));
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "rpmBins"), &src, rpm, 4) == 4);
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "loadBins"), &src, load, 4) == 4);
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "veTable"), &src, z, 16) == 16);
    /* spread(4,500,7000) = 500, 2666.7, 4833.3, 7000 ; U08 scale 100 -> 5,27,48,70 -> x100 */
    LT_CHECK_NEAR(rpm[0], 500, 1e-9);
    LT_CHECK_NEAR(rpm[3], 7000, 1e-9);
    LT_CHECK_NEAR(rpm[1], 2700, 1e-9);
    LT_CHECK_NEAR(load[0], 20, 1e-9);
    LT_CHECK_NEAR(load[1], 47, 1e-9);     /* 46.67 rounds */
    LT_CHECK_NEAR(load[3], 100, 1e-9);
    LT_CHECK_NEAR(z[0], lt_round(lt_sim_true_ve(500, 20) * 0.85), 1e-9);
    LT_CHECK_NEAR(z[15], lt_round(lt_sim_true_ve(7000, 100) * 0.85), 1e-9);
    for (i = 1; i < 4; i++) {
        LT_CHECK(z[i] >= z[i - 1]);      /* rises with rpm along a row */
    }
    LT_CHECK(z[4] > z[0]);                /* and with load */
    lt_ini_free(ini);
    /* no analyze section -> false */
    ini = ini_str("[Constants]\n nPages = 1\n pageSize = 64\npage = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 1, 0\n");
    src.ud = &b;
    LT_CHECK(!lt_sim_seed_ve_table(ini, &src));
    LT_CHECK(!lt_sim_seed_ve_table(NULL, &src));
    lt_ini_free(ini);
}

static const lt_u8 *simram_page(void *ud, int page, size_t *len)
{
    return lt_ecusim_ram((lt_ecusim *)ud, page, len);
}

LT_TEST(seed_ve_demo_ini)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *s;
    lt_pagesrc src;
    size_t len = 0, i;
    const lt_u8 *ram, *fl;
    double rpm[32], load[32], z[1024];

    LT_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    s = lt_ecusim_new(ini);
    ram = lt_ecusim_ram(s, 0, &len);
    fl = lt_ecusim_flash(s, 0, &len);
    LT_CHECK(len == 40560 && ram && fl && memcmp(ram, fl, len) == 0);
    LT_CHECK(lt_ecusim_ram(s, 1, &len) != NULL && len == 256);
    LT_CHECK(lt_ecusim_ram(s, 2, &len) != NULL && len == 2048);
    LT_CHECK(lt_ecusim_ram(s, 3, &len) == NULL);
    src.ud = s; src.page = simram_page; src.write = NULL;
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "veRpmBins"), &src, rpm, 32) == 32);
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "veLoadBins"), &src, load, 32) == 32);
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "veTable"), &src, z, 1024) == 1024);
    LT_CHECK_NEAR(rpm[0], 500, 1e-9);
    LT_CHECK_NEAR(rpm[31], 7000, 1e-9);
    LT_CHECK_NEAR(load[0], 20, 1e-9);
    LT_CHECK_NEAR(load[31], 100, 1e-9);
    for (i = 1; i < 32; i++) {
        LT_CHECK(rpm[i] > rpm[i - 1] && load[i] > load[i - 1]);
    }
    /* cell(row=load, col=rpm) = 0.85 * true_ve, stored with scale 0.01 */
    LT_CHECK_NEAR(z[0], lt_sim_true_ve(500, 20) * 0.85, 0.01);
    LT_CHECK_NEAR(z[1023], lt_sim_true_ve(7000, 100) * 0.85, 0.01);
    LT_CHECK_NEAR(z[5 * 32 + 7], lt_sim_true_ve(rpm[7], load[5]) * 0.85, 0.1);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

/* a blank page image is seeded like a configured engine: a rev limit (the tachometer's range
 * follows it) and no scalar below its minimum (a cylinder count of 0) */
LT_TEST(seed_engine_basics)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *s;
    lt_pagesrc src;
    size_t i, low = 0;
    double v = 0.0;

    LT_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    s = lt_ecusim_new(ini);
    src.ud = s; src.page = simram_page; src.write = NULL;
    LT_CHECK(lt_ini_get(ini, &src, "rpmHardLimit", &v) == 0 && v == 7000.0);
    LT_CHECK(lt_ini_get(ini, &src, "cylindersCount", &v) == 0 && v == 4.0);
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (c->is_pc_var || c->type == LT_DT_BITS || c->type == LT_DT_STRING ||
            c->shape.kind != LT_SH_SCALAR || !(c->max > c->min)) {
            continue;
        }
        if (lt_ini_get(ini, &src, c->name, &v) == 0 && v < c->min - 1e-9) { low++; }
    }
    LT_CHECK_INT((int)low, 0);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

/* ------------------------------------------------------------------ */
/* virtual ECU wire protocol                                            */
/* ------------------------------------------------------------------ */
LT_TEST(ecu_plain_default)
{
    lt_ecusim *s = lt_ecusim_new(NULL);
    lt_u8 r[64];
    size_t n;

    n = xfer(s, "Q", 1, r, sizeof(r));
    LT_CHECK(n == 20 && memcmp(r, "speeduino 202504-dev", 20) == 0);
    n = xfer(s, "S", 1, r, sizeof(r));
    LT_CHECK(n == 21 && memcmp(r, "Speeduino 2025.04-dev", 21) == 0);
    /* 'A' without definition: single secl byte */
    n = xfer(s, "A", 1, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == 0);
    /* unknown command in plain mode: silence (and read must not fail) */
    n = xfer(s, "p\0\0\0\0\4\0", 7, r, sizeof(r));
    LT_CHECK(n == 0);
    LT_CHECK(lt_ecusim_bytes_to_read(s) == 0);
    LT_CHECK(lt_ecusim_read(s, r, 8) == 0);
    lt_ecusim_free(s);
}

LT_TEST(ecu_def_backed_signature)
{
    lt_ini *ini = ini_str(MINI_BE_PLAIN);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[64];
    size_t n = xfer(s, "Q", 1, r, sizeof(r));

    LT_CHECK(n == 20 && memcmp(r, "speeduino 202501-sim", 20) == 0);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_rt_window_plain)
{
    lt_ini *ini = ini_str(MINI_BE_PLAIN);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[64];
    size_t n;
    lt_u8 cmd[7];

    lt_ecusim_tick_engine(s, 3000);
    /* O off=0 len=20 on a 16 byte block -> 20 bytes, last 4 zero (big endian params) */
    cmd[0] = 'O'; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 20;
    n = xfer(s, cmd, 5, r, sizeof(r));
    LT_CHECK(n == 20);
    LT_CHECK(r[16] == 0 && r[17] == 0 && r[18] == 0 && r[19] == 0);
    /* 'r' form with canId and 0x30 */
    cmd[0] = 'r'; cmd[1] = 0; cmd[2] = 0x30; cmd[3] = 0; cmd[4] = 0; cmd[5] = 0; cmd[6] = 20;
    n = xfer(s, cmd, 7, r, sizeof(r));
    LT_CHECK(n == 20 && r[19] == 0);
    cmd[2] = 0x99;
    n = xfer(s, cmd, 7, r, sizeof(r));
    LT_CHECK(n == 0);
    /* 'A' returns the whole block */
    n = xfer(s, "A", 1, r, sizeof(r));
    LT_CHECK(n == 16);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_enveloped_basic)
{
    lt_ini *ini = ini_str(MINI_LE);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[512], c[320];
    int n;
    size_t i;

    n = env_xfer(s, LT_ENV_LITTLE, (const lt_u8 *)"S", 1, r, sizeof(r));
    LT_CHECK(n == 14 && r[0] == 0 && memcmp(r + 1, "rusEFI test.1", 13) == 0);
    n = env_xfer(s, LT_ENV_LITTLE, (const lt_u8 *)"V", 1, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == LT_RC_UNKNOWN_CMD);
    n = env_xfer(s, LT_ENV_LITTLE, (const lt_u8 *)"Q", 1, r, sizeof(r));
    LT_CHECK(n == 14 && r[0] == 0);
    /* write 5A to page 1 (index 1) offset 0, read it back; page ids default to indices */
    c[0] = 'C'; c[1] = 1; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 1; c[6] = 0; c[7] = 0x5A;
    n = env_xfer(s, LT_ENV_LITTLE, c, 8, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == 0x00);
    c[0] = 'R';
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 2 && r[0] == 0 && r[1] == 0x5A);
    /* write outside the page */
    c[0] = 'C'; c[1] = 1; c[3] = 31; c[5] = 2;
    c[7] = 1; c[8] = 2;
    n = env_xfer(s, LT_ENV_LITTLE, c, 9, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == LT_RC_OUT_OF_RANGE);
    c[0] = 'R'; c[5] = 2;
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 3 && r[1] == 0 && r[2] == 0);
    /* unknown page */
    c[0] = 'R'; c[1] = 9; c[3] = 0; c[5] = 1;
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == LT_RC_OUT_OF_RANGE);
    /* big write payload > 255 acked */
    c[0] = 'M'; c[1] = 0; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 0x2C; c[6] = 0x01;   /* 300 */
    {
        lt_buf big;

        lt_buf_init(&big);
        lt_buf_append(&big, c, 7);
        for (i = 0; i < 300; i++) {
            lt_buf_putc(&big, 7);
        }
        n = env_xfer(s, LT_ENV_LITTLE, (const lt_u8 *)big.data, big.len, r, sizeof(r));
        lt_buf_free(&big);
        /* page 0 is only 64 bytes: out of range, memory untouched */
        LT_CHECK(n == 1 && r[0] == LT_RC_OUT_OF_RANGE);
    }
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_burn_reboot)
{
    lt_ini *ini = ini_str(MINI_LE);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[64], c[16];
    int n;
    size_t len;

    c[0] = 'C'; c[1] = 0; c[2] = 0; c[3] = 4; c[4] = 0; c[5] = 1; c[6] = 0; c[7] = 0x11;
    n = env_xfer(s, LT_ENV_LITTLE, c, 8, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == 0);
    c[3] = 5; c[7] = 0x22;
    n = env_xfer(s, LT_ENV_LITTLE, c, 8, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == 0);
    /* burn page 0 */
    c[0] = 'B'; c[1] = 0; c[2] = 0;
    n = env_xfer(s, LT_ENV_LITTLE, c, 3, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == LT_RC_OK_BURN);
    /* write more but do not burn */
    c[0] = 'C'; c[3] = 6; c[7] = 0x33;
    env_xfer(s, LT_ENV_LITTLE, c, 8, r, sizeof(r));
    LT_CHECK(lt_ecusim_ram(s, 0, &len)[6] == 0x33);
    LT_CHECK(lt_ecusim_flash(s, 0, &len)[6] == 0x00);
    LT_CHECK(lt_ecusim_flash(s, 0, &len)[4] == 0x11);
    lt_ecusim_reboot(s);
    LT_CHECK(lt_ecusim_ram(s, 0, &len)[6] == 0x00);
    LT_CHECK(lt_ecusim_ram(s, 0, &len)[4] == 0x11 && lt_ecusim_ram(s, 0, &len)[5] == 0x22);
    /* page CRC */
    c[0] = 'k'; c[1] = 0; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 8; c[6] = 0;
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 5 && r[0] == 0);
    {
        lt_u32 want = lt_proto_crc32(lt_ecusim_ram(s, 0, &len), 8);

        LT_CHECK(lt_rd_be(r + 1, 4) == want);
    }
    /* bad burn page */
    c[0] = 'B'; c[1] = 7;
    n = env_xfer(s, LT_ENV_LITTLE, c, 3, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == LT_RC_OUT_OF_RANGE);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_plain_write_read)
{
    lt_ini *ini = ini_str(MINI_BE_PLAIN);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[64], c[16];
    size_t n;

    /* M page0 off 2 count 2 data AB CD (big endian params): no reply */
    c[0] = 'M'; c[1] = 0; c[2] = 0; c[3] = 0; c[4] = 2; c[5] = 0; c[6] = 2; c[7] = 0xAB; c[8] = 0xCD;
    n = xfer(s, c, 9, r, sizeof(r));
    LT_CHECK(n == 0);
    c[0] = 'p';
    n = xfer(s, c, 7, r, sizeof(r));
    LT_CHECK(n == 2 && r[0] == 0xAB && r[1] == 0xCD);
    /* read past the end: zero padded to count */
    c[3] = 0; c[4] = 62; c[5] = 0; c[6] = 6;
    n = xfer(s, c, 7, r, sizeof(r));
    LT_CHECK(n == 6 && r[2] == 0 && r[5] == 0);
    /* partial command waits, later bytes complete it */
    c[0] = 'p'; c[1] = 0; c[2] = 0; c[3] = 0; c[4] = 2; c[5] = 0; c[6] = 2;
    lt_ecusim_clear_input(s);
    lt_ecusim_write(s, c, 4);
    LT_CHECK(lt_ecusim_bytes_to_read(s) == 0);
    lt_ecusim_write(s, c + 4, 3);
    LT_CHECK(lt_ecusim_bytes_to_read(s) == 2);
    /* a plain definition never treats a frame as an envelope; junk first byte sits */
    lt_ecusim_clear_input(s);
    lt_ecusim_write(s, (const lt_u8 *)"\x7F", 1);
    n = xfer(s, "Q", 1, r, sizeof(r));
    LT_CHECK(n == 0);                    /* 'Q' stuck behind the junk byte */
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_partial_timeout)
{
    lt_ini *ini = ini_str(MINI_BE_PLAIN);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[64];
    size_t n;

    lt_ecusim_set_clock(s, clk_now, NULL);
    g_now = 1000.0;
    lt_ecusim_write(s, (const lt_u8 *)"p\0\0", 3);     /* partial */
    g_now = 1100.0;
    n = xfer(s, "Q", 1, r, sizeof(r));                 /* within 400 ms: Q swallowed as data */
    LT_CHECK(n == 0);
    /* the second write restarted the idle clock; after 400 ms idle the stale bytes drop */
    g_now = 1600.0;
    n = xfer(s, "Q", 1, r, sizeof(r));
    LT_CHECK(n == 20 && memcmp(r, "speeduino 202501-sim", 20) == 0);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_envelope_latch_and_crc)
{
    lt_ini *ini = ini_str(MINI_LE);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[128], z[6] = {0, 0, 0, 0, 0, 0};
    lt_buf f;
    int n;
    long got;

    /* six zero bytes must not latch */
    lt_ecusim_write(s, z, 6);
    LT_CHECK(lt_ecusim_bytes_to_read(s) == 0);
    lt_ecusim_clear_output(s);
    /* a BE request on the LE definition is still answered, in BE */
    n = env_xfer(s, LT_ENV_BIG, (const lt_u8 *)"Q", 1, r, sizeof(r));
    LT_CHECK(n == 14 && r[0] == 0);
    /* now latched BE; a corrupted-CRC complete frame yields [0x82] */
    lt_buf_init(&f);
    lt_env_encode(LT_ENV_BIG, (const lt_u8 *)"Q", 1, &f);
    f.data[f.len - 1] ^= 0x55;
    lt_ecusim_clear_input(s);
    lt_ecusim_write(s, (const lt_u8 *)f.data, f.len);
    got = lt_ecusim_read(s, r, sizeof(r));
    LT_CHECK(got == 2 + 1 + 4);
    {
        const lt_u8 *pl;
        size_t pn, used;

        LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, r, (size_t)got, &pl, &pn, &used) == 0);
        LT_CHECK(pn == 1 && pl[0] == LT_RC_CRC_MISMATCH);
    }
    lt_buf_free(&f);
    /* LE frame after latching BE: not decodable (length scan), nothing answered */
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_rusefi_commands_enveloped)
{
    lt_ini *ini = ini_str(MINI_LE);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 r[128], c[8];
    int n;

    lt_ecusim_tick_engine(s, 2000);
    c[0] = 'O'; c[1] = 0; c[2] = 0; c[3] = 16; c[4] = 0;     /* little endian params */
    n = env_xfer(s, LT_ENV_LITTLE, c, 5, r, sizeof(r));
    LT_CHECK(n == 17 && r[0] == 0);
    c[0] = 'O'; c[3] = 4;
    n = env_xfer(s, LT_ENV_LITTLE, c, 5, r, sizeof(r));
    LT_CHECK(n == 5);
    /* bad 'r' selector gives ok([]) */
    c[0] = 'r'; c[1] = 0; c[2] = 0x31; c[3] = 0; c[4] = 0; c[5] = 4; c[6] = 0;
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 1 && r[0] == 0);
    c[2] = 0x30;
    n = env_xfer(s, LT_ENV_LITTLE, c, 7, r, sizeof(r));
    LT_CHECK(n == 5 && r[0] == 0);
    /* 'A' enveloped = ok(block) */
    n = env_xfer(s, LT_ENV_LITTLE, (const lt_u8 *)"A", 1, r, sizeof(r));
    LT_CHECK(n == 17);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(ecu_link_dropped)
{
    lt_ecusim *s = lt_ecusim_new(NULL);
    lt_u8 r[32];
    lt_proto_io io = lt_ecusim_io(s);
    int n;

    lt_ecusim_set_link_dropped(s, LT_TRUE);
    LT_CHECK(lt_ecusim_write(s, (const lt_u8 *)"Q", 1) == (size_t)-1);
    LT_CHECK(lt_ecusim_read(s, r, 32) == -1);
    LT_CHECK(io.write(io.ud, "Q", 1) == -1);
    LT_CHECK(io.read(io.ud, r, 32, 0) == -1);
    lt_ecusim_set_link_dropped(s, LT_FALSE);
    LT_CHECK(io.write(io.ud, "Q", 1) == 1);
    n = io.read(io.ud, r, 32, 0);
    LT_CHECK(n == 20);
    LT_CHECK(io.read(io.ud, r, 32, 0) == 0);         /* empty = would block, not EOF */
    lt_ecusim_free(s);
}

LT_TEST(ecu_och_animates_and_secl)
{
    lt_ini *ini = ini_str(MINI_LE);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 a[16];
    lt_sim_snapshot sn;
    int secl0;

    lt_ecusim_tick_engine(s, 5000);
    lt_ecusim_set_mode(s, LT_SIM_WOT);
    lt_ecusim_tick_engine(s, 1500);
    lt_simengine_snapshot(lt_ecusim_engine(s), &sn);
    LT_CHECK(sn.rpm >= 1000);
    LT_CHECK(xfer(s, "A", 1, a, 16) == 0);          /* LE+enveloped def: bare A never dispatches */
    lt_ecusim_clear_output(s);
    lt_simengine_snapshot(lt_ecusim_engine(s), &sn);
    secl0 = sn.secl;
    lt_ecusim_advance_secl(s, 250);
    lt_ecusim_advance_secl(s, 10);
    lt_simengine_snapshot(lt_ecusim_engine(s), &sn);
    LT_CHECK(sn.secl == (secl0 + 250 + 10) % 256);
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

/* ------------------------------------------------------------------ */
/* cosmetic demo                                                        */
/* ------------------------------------------------------------------ */
LT_TEST(demo_channels)
{
    lt_demo *d = lt_demo_new(12345);
    double v, v2;
    unsigned long t;
    int ok = 1;

    lt_demo_update(d, 0);
    LT_CHECK(lt_demo_get(d, "rpm", &v) && lt_demo_get(d, "map", &v) &&
             lt_demo_get(d, "afr", &v) && lt_demo_get(d, "coolant", &v));
    LT_CHECK(!lt_demo_get(d, "nonsense", &v));
    for (t = 0; t <= 3000; t += 100) {
        lt_demo_update(d, t);
        lt_demo_get(d, "rpm", &v);
        if (!(v > 700 && v < 1000)) {
            ok = 0;
        }
    }
    LT_CHECK(ok);
    lt_demo_get(d, "rpm", &v);
    lt_demo_get(d, "RPM", &v2);
    LT_CHECK(v == v2);
    lt_demo_get(d, "TPS", &v);
    lt_demo_get(d, "throttle", &v2);
    LT_CHECK(v == v2 && v >= 1.5 && v <= 2.5);
    LT_CHECK(lt_demo_get(d, "baroPressure", &v) && v == 101.0);
    lt_demo_free(d);

    d = lt_demo_new(1);
    lt_demo_update(d, 1000);
    lt_demo_get(d, "coolant", &v);
    lt_demo_update(d, 61000);
    lt_demo_get(d, "coolant", &v2);
    LT_CHECK(v2 > v && v2 < 90.0);
    lt_demo_free(d);
}

LT_TEST(demo_blip_and_env)
{
    lt_demo *d = lt_demo_new(777);
    unsigned long t;
    double v, maxrpm = 0;
    lt_expr_env env;
    lt_expr *ex;
    double r = 0;

    for (t = 0; t <= 60000; t += 20) {
        lt_demo_update(d, t ? t : 1);
        lt_demo_get(d, "rpm", &v);
        if (v > maxrpm) {
            maxrpm = v;
        }
    }
    LT_CHECK(maxrpm > 1500 && maxrpm <= 4100);       /* at least one blip in a minute */
    lt_demo_env(d, &env);
    ex = lt_expr_compile("rpm / 2", NULL, 0);
    LT_CHECK(ex != NULL);
    LT_CHECK(lt_expr_eval_f64(ex, &env, &r) == 0);
    lt_demo_get(d, "rpm", &v);
    LT_CHECK_NEAR(r, v / 2, 1e-9);
    lt_expr_free(ex);
    lt_demo_free(d);
}

/* ------------------------------------------------------------------ */
/* integration: lt_proto over the simulator with demo.ini               */
/* ------------------------------------------------------------------ */
LT_TEST(demo_mode_integration)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *sim;
    lt_proto_io io;
    lt_proto_cfg cfg;
    lt_proto *p;
    lt_proto_clock ck;
    lt_ecu_mem *mem;
    lt_ini_rt *rt;
    lt_buf blk;
    int i, rc;
    double rpm, rpm_min = 1e9, rpm_max = -1, map, map_min = 1e9, map_max = -1, secl0 = 0;
    const lt_ini_const *cve;
    double v;
    lt_pagesrc src;

    LT_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    g_now = 10000.0;
    sim = lt_ecusim_new(ini);
    lt_ecusim_set_clock(sim, clk_now, NULL);
    io = lt_ecusim_io(sim);
    lt_proto_cfg_default(&cfg);
    p = lt_proto_new(&io, ini, &cfg);
    ck.ud = NULL; ck.now_ms = clk_now; ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(p, &ck);

    rc = lt_proto_connect(p);
    LT_CHECK(rc == 0);
    LT_CHECK_STR(lt_proto_signature(p), ini->signature);
    LT_CHECK(lt_proto_is_modern(p));
    LT_CHECK(!lt_proto_signature_mismatch(p));

    mem = lt_ecu_mem_from_ini(ini);
    rc = lt_proto_read_all(p, mem, LT_TRUE);
    LT_CHECK(rc >= 0);
    LT_CHECK(lt_ecu_mem_fully_loaded(mem));
    for (i = 0; i < 3; i++) {
        size_t l1, l2;
        const lt_u8 *a = lt_ecu_mem_page(mem, i, &l1);
        const lt_u8 *b = lt_ecusim_ram(sim, i, &l2);

        LT_CHECK(a && b && l1 == l2 && memcmp(a, b, l1) == 0);
    }
    /* the seeded VE table is visible through the codec on the downloaded image */
    src = lt_ecu_mem_pagesrc(mem);
    cve = lt_ini_find_const(ini, "veTable");
    LT_CHECK(cve != NULL);
    if (cve) {
        LT_CHECK(lt_ini_const_get(ini, cve, &src, 0, &v) == 0);
        LT_CHECK_NEAR(v, lt_round(lt_sim_true_ve(500, 20) * 0.85), 1.0);
    }

    /* write a value, burn, check the sim's RAM + flash */
    {
        const lt_ini_const *c = lt_ini_find_const(ini, "veRpmBins");
        lt_u8 two[2];
        size_t l;
        const lt_u8 *fl;
        const lt_u8 *ram;

        LT_CHECK(c != NULL);
        two[0] = 0x34; two[1] = 0x12;
        rc = lt_proto_write_memory(p, c->page, c->offset, two, 2);
        LT_CHECK(rc >= 0);
        ram = lt_ecusim_ram(sim, c->page, &l);
        LT_CHECK(ram[c->offset] == 0x34 && ram[c->offset + 1] == 0x12);
        fl = lt_ecusim_flash(sim, c->page, &l);
        LT_CHECK(fl[c->offset] != 0x34 || fl[c->offset + 1] != 0x12);
        rc = lt_proto_burn(p, c->page, NULL);
        LT_CHECK(rc >= 0);
        fl = lt_ecusim_flash(sim, c->page, &l);
        LT_CHECK(fl[c->offset] == 0x34 && fl[c->offset + 1] == 0x12);
        /* page 1 and 2 are small: write_page with CRC verify round-trips */
        {
            size_t l1;
            const lt_u8 *pg = lt_ecu_mem_page(mem, 1, &l1);
            lt_u8 *copy = (lt_u8 *)lt_malloc(l1);

            memcpy(copy, pg, l1);
            copy[10] = 0x77;
            rc = lt_proto_write_page(p, 1, copy, l1);
            LT_CHECK(rc >= 0);
            LT_CHECK(lt_ecusim_ram(sim, 1, &l)[10] == 0x77);
            lt_free(copy);
        }
        lt_ecusim_reboot(sim);
        ram = lt_ecusim_ram(sim, c->page, &l);
        LT_CHECK(ram[c->offset] == 0x34);            /* burned: survives */
        LT_CHECK(lt_ecusim_ram(sim, 1, &l)[10] != 0x77);   /* unburned: lost */
    }

    /* realtime polling */
    rt = lt_ini_rt_new(ini);
    lt_buf_init(&blk);
    for (i = 0; i < 100; i++) {
        g_now += 100.0;
        lt_buf_clear(&blk);
        rc = lt_proto_get_realtime(p, &blk);
        LT_CHECK(rc == 0);
        LT_CHECK(blk.len == lt_sim_block_size(ini));
        LT_CHECK(lt_ini_rt_update(rt, (const lt_u8 *)blk.data, blk.len) == 0);
        LT_CHECK(lt_ini_rt_get(rt, "RPMValue", &rpm));
        LT_CHECK(lt_ini_rt_get(rt, "MAPValue", &map));
        if (i == 0) {
            lt_ini_rt_get(rt, "seconds", &secl0);
        }
        if (rpm < rpm_min) {
            rpm_min = rpm;
        }
        if (rpm > rpm_max) {
            rpm_max = rpm;
        }
        if (map < map_min) {
            map_min = map;
        }
        if (map > map_max) {
            map_max = map;
        }
        LT_CHECK(rpm >= 0 && rpm <= 7000);
        LT_CHECK(map >= 0 && map <= 100);
    }
    LT_CHECK(rpm_max > rpm_min + 100);
    LT_CHECK(rpm_max > 600);
    LT_CHECK(map_max > map_min);
    {
        double clt, bat, secs, baro, lam;

        LT_CHECK(lt_ini_rt_get(rt, "coolant", &clt) && clt >= 19.9 && clt < 100);
        LT_CHECK(lt_ini_rt_get(rt, "VBatt", &bat) && bat > 9.5 && bat < 15);
        LT_CHECK(lt_ini_rt_get(rt, "seconds", &secs) && secs >= secl0 + 8);   /* 100 polls * 0.1 s */
        LT_CHECK(lt_ini_rt_get(rt, "baroPressure", &baro) && baro > 99.9 && baro < 100.1);
        LT_CHECK(lt_ini_rt_get(rt, "lambdaValue", &lam) && lam > 0.5 && lam < 2.0);
    }
    lt_buf_free(&blk);
    lt_ini_rt_free(rt);
    lt_ecu_mem_free(mem);
    lt_proto_free(p);
    lt_ecusim_free(sim);
    lt_ini_free(ini);
}

LT_TEST(demo_mode_wot_and_modes)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *sim;
    lt_proto_io io;
    lt_proto_cfg cfg;
    lt_proto *p;
    lt_proto_clock ck;
    lt_ini_rt *rt;
    lt_buf blk;
    double rpm = 0;
    int i;

    if (!ini) {
        LT_CHECK(0);
        return;
    }
    g_now = 5000.0;
    sim = lt_ecusim_new(ini);
    lt_ecusim_set_clock(sim, clk_now, NULL);
    io = lt_ecusim_io(sim);
    lt_proto_cfg_default(&cfg);
    p = lt_proto_new(&io, ini, &cfg);
    ck.ud = NULL; ck.now_ms = clk_now; ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    rt = lt_ini_rt_new(ini);
    lt_buf_init(&blk);
    lt_ecusim_set_mode(sim, LT_SIM_WOT);
    for (i = 0; i < 20; i++) {
        g_now += 100.0;
        lt_buf_clear(&blk);
        LT_CHECK(lt_proto_get_realtime(p, &blk) == 0);
        lt_ini_rt_update(rt, (const lt_u8 *)blk.data, blk.len);
        lt_ini_rt_get(rt, "RPMValue", &rpm);
    }
    LT_CHECK(rpm > 1000);
    lt_buf_free(&blk);
    lt_ini_rt_free(rt);
    lt_proto_free(p);
    lt_ecusim_free(sim);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(engine_initial_state);
    LT_RUN(engine_startup_to_idle);
    LT_RUN(engine_cranking_rpm_step);
    LT_RUN(engine_wot_and_modes);
    LT_RUN(engine_thermal_secl);
    LT_RUN(engine_deterministic);
    LT_RUN(engine_golden);
    LT_RUN(true_ve_values);
    LT_RUN(ve_context_interp_and_usable);
    LT_RUN(och_u16_scale_translate);
    LT_RUN(och_endianness);
    LT_RUN(och_float_and_saturation);
    LT_RUN(och_bits_and_computed);
    LT_RUN(och_block_size);
    LT_RUN(seed_ve_table_values);
    LT_RUN(seed_ve_demo_ini);
    LT_RUN(seed_engine_basics);
    LT_RUN(ecu_plain_default);
    LT_RUN(ecu_def_backed_signature);
    LT_RUN(ecu_rt_window_plain);
    LT_RUN(ecu_enveloped_basic);
    LT_RUN(ecu_burn_reboot);
    LT_RUN(ecu_plain_write_read);
    LT_RUN(ecu_partial_timeout);
    LT_RUN(ecu_envelope_latch_and_crc);
    LT_RUN(ecu_rusefi_commands_enveloped);
    LT_RUN(ecu_link_dropped);
    LT_RUN(ecu_och_animates_and_secl);
    LT_RUN(demo_channels);
    LT_RUN(demo_blip_and_env);
    LT_RUN(demo_mode_integration);
    LT_RUN(demo_mode_wot_and_modes);
    LT_TEST_MAIN_END
}
