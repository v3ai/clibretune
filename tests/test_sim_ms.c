/* test_sim_ms.c - demo mode with MegaSquirt definitions: lt_proto over the simulator, the way
 * the app's demo connects (connect, read every page, poll realtime data, edit, burn, power
 * cycle, reconnect), for the MS2/Extra, MS3 and MegaSquirt-II 2.x dialects.
 *
 * The fixtures in tests/fixtures/sim/ms*_sim.ini are synthetic definitions using the same
 * command formats as the real INIs (which carry no redistribution grant and so are not in
 * the tree); tools/ini_corpus_check.sh explains how to run the real ones locally. */
#include "lt.h"
#include "sim/sim.h"
#include "proto/proto.h"
#include "lt_test.h"

#include <string.h>

#define MS2X_INI "tests/fixtures/sim/ms2extra_sim.ini"
#define MS3_INI "tests/fixtures/sim/ms3_sim.ini"
#define MS2_INI "tests/fixtures/sim/ms2_sim.ini"

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

static lt_proto *open_proto(const lt_ini *ini, lt_ecusim *sim)
{
    lt_proto_io io = lt_ecusim_io(sim);
    lt_proto_cfg cfg;
    lt_proto_clock ck;
    lt_proto *p;

    lt_proto_cfg_default(&cfg);
    cfg.instant_link = LT_TRUE;
    p = lt_proto_new(&io, ini, &cfg);
    ck.ud = NULL;
    ck.now_ms = clk_now;
    ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(p, &ck);
    return p;
}

static double get_named(const lt_ini *ini, lt_ecu_mem *mem, const char *name, size_t idx)
{
    lt_pagesrc src = lt_ecu_mem_pagesrc(mem);
    const lt_ini_const *c = lt_ini_find_const(ini, name);
    double v = -12345.0;

    if (c == NULL || lt_ini_const_get(ini, c, &src, idx, &v) != 0) {
        return -12345.0;
    }
    return v;
}

static void set_named(const lt_ini *ini, lt_ecu_mem *mem, const char *name, size_t idx, double v)
{
    lt_pagesrc src = lt_ecu_mem_pagesrc(mem);
    const lt_ini_const *c = lt_ini_find_const(ini, name);

    LT_CHECK(c != NULL && lt_ini_const_set(ini, c, &src, idx, v) == 0);
}

static void pages_match_sim(const lt_ini *ini, lt_ecu_mem *mem, lt_ecusim *sim)
{
    size_t i;

    for (i = 0; i < ini->proto.n_page_sizes; i++) {
        size_t l1 = 0, l2 = 0;
        const lt_u8 *a = lt_ecu_mem_page(mem, (int)i, &l1);
        const lt_u8 *b = lt_ecusim_ram(sim, (int)i, &l2);

        LT_CHECK(a != NULL && b != NULL && l1 == l2 && l1 == ini->proto.page_sizes[i] &&
                 memcmp(a, b, l1) == 0);
    }
}

/* connect, read, poll, edit, burn, power cycle, reconnect: `modern` = msEnvelope framing */
static void ms_demo(const char *path, lt_bool modern, lt_rt_kind rt_kind)
{
    lt_ini *ini = lt_ini_load_file(path, NULL);
    lt_ecusim *sim;
    lt_proto *p;
    lt_ecu_mem *mem;
    lt_ini_rt *rt;
    lt_buf blk;
    int i, rc;
    double rpm = 0, map = 0, clt = 0, ready = 0, rpm_max = 0, map_min = 1e9, map_max = -1;
    const lt_ini_const *ve, *rq;
    size_t len = 0;

    LT_CHECK(ini != NULL);
    if (ini == NULL) {
        return;
    }
    g_now = 10000.0;
    sim = lt_ecusim_new(ini);
    lt_ecusim_set_clock(sim, clk_now, NULL);
    p = open_proto(ini, sim);

    /* handshake */
    LT_CHECK(lt_proto_connect(p) == 0);
    LT_CHECK_STR(lt_proto_signature(p), ini->signature);
    LT_CHECK(!lt_proto_signature_mismatch(p));
    LT_CHECK(lt_proto_is_modern(p) == modern);
    LT_CHECK(lt_proto_choose_runtime(p, NULL) == rt_kind);

    /* every page, through the INI's own page commands */
    mem = lt_ecu_mem_from_ini(ini);
    LT_CHECK(lt_proto_read_all(p, mem, LT_TRUE) == 0);
    LT_CHECK(lt_ecu_mem_fully_loaded(mem));
    pages_match_sim(ini, mem, sim);

    /* a configured engine: no INVALID options, a VE and AFR table without [VeAnalyze] */
    LT_CHECK_NEAR(get_named(ini, mem, "algorithm", 0), 1.0, 1e-9);        /* Speed Density */
    LT_CHECK_NEAR(get_named(ini, mem, "nCylinders", 0), 4.0, 1e-9);
    LT_CHECK_NEAR(get_named(ini, mem, "reqFuel", 0), 8.0, 1e-6);
    LT_CHECK_NEAR(get_named(ini, mem, "rpmLimit", 0), 1000.0, 1e-9);      /* its minimum */
    LT_CHECK_NEAR(get_named(ini, mem, "frpm_table1", 0), 500.0, 1e-9);
    LT_CHECK_NEAR(get_named(ini, mem, "frpm_table1", 15), 7000.0, 1e-9);
    LT_CHECK_NEAR(get_named(ini, mem, "fmap_table1", 15), 100.0, 0.05);
    LT_CHECK_NEAR(get_named(ini, mem, "veTable1", 0), lt_sim_true_ve(500, 20) * 0.85, 0.5);
    LT_CHECK_NEAR(get_named(ini, mem, "afrTable1", 0), 14.7, 0.05);
    LT_CHECK(lt_sim_ve_table(ini) != NULL && strcmp(lt_sim_ve_table(ini)->name, "veTable1Tbl") == 0);

    /* live data: the engine starts and idles; temperatures in degrees F */
    rt = lt_ini_rt_new(ini);
    lt_buf_init(&blk);
    for (i = 0; i < 100; i++) {
        g_now += 100.0;
        lt_buf_clear(&blk);
        rc = lt_proto_get_realtime(p, &blk);
        LT_CHECK(rc == 0 && blk.len == ini->proto.och_block_size);
        LT_CHECK(lt_ini_rt_update(rt, (const lt_u8 *)blk.data, blk.len) == 0);
        LT_CHECK(lt_ini_rt_get(rt, "rpm", &rpm) && lt_ini_rt_get(rt, "map", &map));
        rpm_max = rpm > rpm_max ? rpm : rpm_max;
        map_min = map < map_min ? map : map_min;
        map_max = map > map_max ? map : map_max;
    }
    LT_CHECK(rpm_max > 500.0 && rpm_max < 8000.0);
    LT_CHECK(map_min >= 0.0 && map_max <= 105.0 && map_min < 95.0);    /* S16 x 0.1 decoded */
    LT_CHECK(lt_ini_rt_get(rt, "coolant", &clt) && clt > 50.0 && clt < 250.0);
    LT_CHECK(lt_ini_rt_get(rt, "ready", &ready) && ready == 1.0);

    /* edit like the app: queued RAM writes, then burn */
    set_named(ini, mem, "reqFuel", 0, 9.5);
    set_named(ini, mem, "veTable1", 17, 77.0);
    LT_CHECK(lt_proto_write_dirty_begin(p, mem) > 0);
    lt_proto_async_finish(p);
    LT_CHECK(lt_proto_write_pump(p) == 1);
    pages_match_sim(ini, mem, sim);
    rq = lt_ini_find_const(ini, "reqFuel");
    ve = lt_ini_find_const(ini, "veTable1");
    LT_CHECK(rq != NULL && ve != NULL);
    if (ve != NULL) {
        /* the last page written is not burned yet (moving on from the first one burned it) */
        const lt_u8 *fl = lt_ecusim_flash(sim, (int)ve->page, &len);
        LT_CHECK(fl != NULL && memcmp(fl, lt_ecusim_ram(sim, (int)ve->page, &len), len) != 0);
    }
    LT_CHECK(lt_proto_burn(p, -1, mem) == 0);
    LT_CHECK(!lt_ecu_mem_has_pending_burn(mem));
    if (rq != NULL && ve != NULL) {
        const lt_u8 *fl = lt_ecusim_flash(sim, (int)rq->page, &len);
        LT_CHECK(fl != NULL && lt_rd_be(fl + rq->offset, 2) == 9500);
        fl = lt_ecusim_flash(sim, (int)ve->page, &len);
        LT_CHECK(fl != NULL && memcmp(fl, lt_ecusim_ram(sim, (int)ve->page, &len), len) == 0);
    }

    /* power cycle the ECU, reconnect, read again: the burned values are there */
    lt_proto_free(p);
    lt_ecu_mem_free(mem);
    lt_ecusim_reboot(sim);
    p = open_proto(ini, sim);
    LT_CHECK(lt_proto_connect(p) == 0);
    mem = lt_ecu_mem_from_ini(ini);
    LT_CHECK(lt_proto_read_all(p, mem, LT_TRUE) == 0);
    LT_CHECK_NEAR(get_named(ini, mem, "reqFuel", 0), 9.5, 1e-6);
    LT_CHECK_NEAR(get_named(ini, mem, "veTable1", 17), 77.0, 1e-6);
    LT_CHECK_NEAR(get_named(ini, mem, "nCylinders", 0), 4.0, 1e-9);

    /* a whole page written back with the CRC check where the INI declares one */
    if (ini->proto.crc32_check_commands.n > 0) {
        const lt_u8 *pg = lt_ecu_mem_page(mem, 0, &len);
        lt_u8 *copy = (lt_u8 *)lt_malloc(len);

        memcpy(copy, pg, len);
        copy[100] = 0x5A;
        LT_CHECK(lt_proto_write_page(p, 0, copy, len) == 0);
        LT_CHECK(lt_ecusim_ram(sim, 0, &len)[100] == 0x5A);
        LT_CHECK(lt_proto_verify_page_crc(p, 0, copy, len) == 0);
        copy[100] = 0x5B;
        LT_CHECK(lt_proto_verify_page_crc(p, 0, copy, len) == LT_ECRC);
        lt_free(copy);
    }

    lt_buf_free(&blk);
    lt_ini_rt_free(rt);
    lt_proto_free(p);
    lt_ecu_mem_free(mem);
    lt_ecusim_free(sim);
    lt_ini_free(ini);
}

LT_TEST(ms2extra_demo)
{
    ms_demo(MS2X_INI, LT_FALSE, LT_RT_KIND_BURST);
}

LT_TEST(ms3_demo)
{
    ms_demo(MS3_INI, LT_TRUE, LT_RT_KIND_OCH);
}

LT_TEST(ms2_original_demo)
{
    ms_demo(MS2_INI, LT_FALSE, LT_RT_KIND_BURST);
}

/* the bytes on the wire, as a real MS2/Extra would see them */
LT_TEST(ms2extra_wire_format)
{
    lt_ini *ini = lt_ini_load_file(MS2X_INI, NULL);
    lt_ecusim *sim;
    lt_u8 r[64];
    long n;
    static const lt_u8 rd[7] = { 'r', 0x00, 0x04, 0x00, 0x04, 0x00, 0x02 };     /* reqFuel */
    static const lt_u8 wr[9] = { 'w', 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x23, 0x28 };
    static const lt_u8 bn[3] = { 'b', 0x00, 0x04 };

    LT_CHECK(ini != NULL);
    if (ini == NULL) {
        return;
    }
    sim = lt_ecusim_new(ini);
    LT_CHECK(lt_ecusim_write(sim, rd, 4) == 4);          /* split command: waits */
    LT_CHECK(lt_ecusim_bytes_to_read(sim) == 0);
    lt_ecusim_write(sim, rd + 4, 3);
    n = lt_ecusim_read(sim, r, sizeof(r));
    LT_CHECK(n == 2 && r[0] == 0x1F && r[1] == 0x40);   /* 8.000 ms */
    lt_ecusim_write(sim, wr, sizeof(wr));                 /* writes and burns are silent */
    LT_CHECK(lt_ecusim_bytes_to_read(sim) == 0);
    lt_ecusim_write(sim, bn, sizeof(bn));
    LT_CHECK(lt_ecusim_bytes_to_read(sim) == 0);
    LT_CHECK(lt_rd_be(lt_ecusim_flash(sim, 0, NULL) + 4, 2) == 9000);
    lt_ecusim_write(sim, (const lt_u8 *)"S", 1);
    n = lt_ecusim_read(sim, r, sizeof(r));
    LT_CHECK(n == 18 && memcmp(r, "MS2Extra comms-sim", 18) == 0);
    lt_ecusim_write(sim, (const lt_u8 *)"A", 1);
    n = lt_ecusim_read(sim, r, sizeof(r));
    LT_CHECK(n == 48);
    lt_ecusim_free(sim);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(ms2extra_demo);
    LT_RUN(ms3_demo);
    LT_RUN(ms2_original_demo);
    LT_RUN(ms2extra_wire_format);
    LT_TEST_MAIN_END
}
