/* test_integration.c - end-to-end flows across ini, tune, proto, sim, tableops,
 * datalog, autotune, project and dash. Run from the repository root. */
#include "lt.h"
#include "ini/ini.h"
#include "tune/tune.h"
#include "proto/proto.h"
#include "sim/sim.h"
#include "tableops/tableops.h"
#include "datalog/datalog.h"
#include "autotune/autotune.h"
#include "project/project.h"
#include "dash/dash.h"
#include "plat/plat.h"
#include "lt_test.h"

#include <stdlib.h>
#include <string.h>

#define FIX_DIR "tests/fixtures/ini/"
#define DEMO_INI FIX_DIR "demo.ini"
#define OUT_DIR "build/tmp"

/* ------------------------------------------------------------------ */
/* virtual clock so the protocol never really sleeps                    */
/* ------------------------------------------------------------------ */
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

typedef struct demo_rig {
    lt_ini *ini;
    lt_ecusim *sim;
    lt_proto_io io;
    lt_proto *proto;
    lt_ecu_mem *mem;
} demo_rig;

static int rig_open(demo_rig *r)
{
    lt_proto_cfg cfg;
    lt_proto_clock ck;

    memset(r, 0, sizeof *r);
    r->ini = lt_ini_load_file(DEMO_INI, NULL);
    if (!r->ini) {
        return -1;
    }
    g_now = 10000.0;
    r->sim = lt_ecusim_new(r->ini);
    lt_ecusim_set_clock(r->sim, clk_now, NULL);
    r->io = lt_ecusim_io(r->sim);
    lt_proto_cfg_default(&cfg);
    r->proto = lt_proto_new(&r->io, r->ini, &cfg);
    ck.ud = NULL;
    ck.now_ms = clk_now;
    ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(r->proto, &ck);
    if (lt_proto_connect(r->proto) != 0) {
        return -2;
    }
    r->mem = lt_ecu_mem_from_ini(r->ini);
    return 0;
}

static void rig_close(demo_rig *r)
{
    if (r->mem) lt_ecu_mem_free(r->mem);
    if (r->proto) lt_proto_free(r->proto);
    if (r->sim) lt_ecusim_free(r->sim);
    if (r->ini) lt_ini_free(r->ini);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ */
/* (a) fixtures: parse -> default pages -> .msq -> read back            */
/* ------------------------------------------------------------------ */
static void fixture_roundtrip(const char *file, lt_bool full)
{
    char path[256], out[256];
    lt_ini *ini;
    lt_tune *t, *back;
    lt_tune_diff *d;
    int p;

    lt_snprintf(path, sizeof path, FIX_DIR "%s", file);
    ini = lt_ini_load_file(path, NULL);
    LT_CHECK(ini != NULL);
    if (!ini) {
        fprintf(stderr, "  load %s: %s\n", file, lt_err_get());
        return;
    }
    /* the small calibration fixtures have no pages/constants: only the full INIs do */
    if (full) {
        LT_CHECK(ini->n_pages > 0);
        LT_CHECK(ini->n_constants > 50);
        LT_CHECK(ini->n_channels > 10);
    }

    t = lt_tune_from_defaults(ini);
    LT_CHECK(t != NULL);
    if (!t) {
        lt_ini_free(ini);
        return;
    }
    if (!t->signature || !t->signature[0]) {
        lt_tune_set_signature(t, ini->signature && ini->signature[0] ? ini->signature : "fixture");
    }
    for (p = 0; p < (int)ini->n_pages; p++) {
        size_t len = 0;
        const lt_u8 *pg = lt_tune_get_page(t, p, &len);

        LT_CHECK(pg != NULL);
        LT_CHECK((unsigned long)len == ini->proto.page_sizes[p]);
    }
    lt_tune_stamp_ini(t, ini, "deadbeef");
    lt_plat_mkdirs(OUT_DIR);
    lt_snprintf(out, sizeof out, OUT_DIR "/rt_%s.msq", file);
    LT_CHECK(lt_tune_save(t, out) == 0);

    back = lt_tune_load(out);
    LT_CHECK(back != NULL);
    if (back) {
        LT_CHECK_STR(back->signature, t->signature);
        for (p = 0; p < (int)ini->n_pages && full; p++) {
            size_t l1 = 0, l2 = 0;
            const lt_u8 *a = lt_tune_get_page(t, p, &l1);
            const lt_u8 *b = lt_tune_get_page(back, p, &l2);

            LT_CHECK(a && b && l1 == l2 && memcmp(a, b, l1) == 0);
        }
        LT_CHECK(lt_tune_n_constants(back) == lt_tune_n_constants(t));
        d = lt_tune_diff_compare(t, back);
        LT_CHECK(d != NULL);
        if (d) {
            if (!lt_tune_diff_identical(d)) {
                size_t i;
                for (i = 0; i < d->n && i < 5; i++)
                    fprintf(stderr, "  %s diff: %s\n", file, d->diffs[i].name);
            }
            LT_CHECK(lt_tune_diff_identical(d));
            lt_tune_diff_free(d);
        }
        /* reapply named values onto fresh pages: still equal to the originals */
        {
            lt_tune *re = lt_tune_clone(back);
            size_t l1 = 0, l2 = 0;
            const lt_u8 *a, *b;

            lt_tune_apply_to_pages(re, ini);
            a = lt_tune_get_page(t, 0, &l1);
            b = lt_tune_get_page(re, 0, &l2);
            if (full) LT_CHECK(a && b && l1 == l2 && memcmp(a, b, l1) == 0);
            lt_tune_free(re);
        }
        lt_tune_free(back);
    }
    lt_tune_free(t);
    lt_ini_free(ini);
}

LT_TEST(fixtures_msq_roundtrip)
{
    fixture_roundtrip("speeduino_202501_na6.ini", LT_FALSE);
    fixture_roundtrip("ms3.ini", LT_FALSE);
    fixture_roundtrip("ms2extra.ini", LT_FALSE);
    fixture_roundtrip("demo.ini", LT_TRUE);
}

/* ------------------------------------------------------------------ */
/* (b) demo: sim + proto + tableops, write + burn, verify               */
/* ------------------------------------------------------------------ */
LT_TEST(demo_edit_ve_write_burn)
{
    demo_rig r;
    const lt_ini_table *tbl;
    lt_pagesrc src;
    lt_tblview v;
    lt_tcell sel[1];
    double before, after, readback;
    int rc, page;
    size_t len;
    const lt_u8 *ram, *flash;
    const lt_ini_const *zc;
    lt_tune *tune;

    LT_CHECK(rig_open(&r) == 0);
    if (!r.mem) {
        rig_close(&r);
        return;
    }
    LT_CHECK(lt_proto_read_all(r.proto, r.mem, LT_TRUE) >= 0);
    LT_CHECK(lt_ecu_mem_fully_loaded(r.mem));
    LT_CHECK(!lt_ecu_mem_has_dirty(r.mem));

    /* snapshot into a tune (what the app would save) and check it matches the sim */
    tune = lt_tune_new(r.ini->signature);
    LT_CHECK(lt_tune_from_mem(tune, r.mem) >= 0);
    lt_tune_capture_from_pages(tune, r.ini);
    ram = lt_ecusim_ram(r.sim, 0, &len);
    {
        size_t tl = 0;
        const lt_u8 *tp = lt_tune_get_page(tune, 0, &tl);
        LT_CHECK(tp && tl == len && memcmp(tp, ram, len) == 0);
    }

    tbl = lt_ini_find_table(r.ini, "veTableTbl");
    LT_CHECK(tbl != NULL);
    zc = tbl ? lt_ini_find_const(r.ini, tbl->z_const) : NULL;
    LT_CHECK(zc != NULL);
    if (!tbl || !zc) {
        lt_tune_free(tune);
        rig_close(&r);
        return;
    }
    page = zc->page;

    src = lt_ecu_mem_pagesrc(r.mem);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, r.ini, tbl, &src) == 0);
    LT_CHECK(v.rows >= 8 && v.cols >= 8);
    before = lt_tblview_get(&v, 3, 4);
    sel[0].y = 3;
    sel[0].x = 4;
    lt_tblview_scale(&v, sel, 1, 1.10);
    after = lt_tblview_get(&v, 3, 4);
    LT_CHECK_NEAR(after, before * 1.10, 1e-9);
    LT_CHECK(lt_tblview_is_dirty(&v));
    LT_CHECK(lt_tblview_commit(&v, r.ini, &src) >= 1);
    LT_CHECK(lt_ecu_mem_has_dirty(r.mem));
    LT_CHECK(lt_ecu_mem_state(r.mem, page) == LT_PAGE_DIRTY);

    rc = lt_proto_write_dirty(r.proto, r.mem);
    LT_CHECK(rc > 0);
    LT_CHECK(lt_ecu_mem_has_pending_burn(r.mem));
    ram = lt_ecusim_ram(r.sim, page, &len);
    flash = lt_ecusim_flash(r.sim, page, &len);
    LT_CHECK(memcmp(ram, lt_ecu_mem_page(r.mem, page, NULL), len) == 0);
    LT_CHECK(memcmp(ram, flash, len) != 0);   /* not burned yet */
    rc = lt_proto_burn(r.proto, page, r.mem);
    LT_CHECK(rc >= 0);
    flash = lt_ecusim_flash(r.sim, page, &len);
    LT_CHECK(memcmp(ram, flash, len) == 0);
    LT_CHECK(lt_ecu_mem_state(r.mem, page) == LT_PAGE_CLEAN);

    /* read back from the sim into a fresh shadow and decode the edited cell */
    {
        lt_ecu_mem *m2 = lt_ecu_mem_from_ini(r.ini);
        lt_pagesrc s2;
        lt_tblview v2;

        LT_CHECK(lt_proto_read_all(r.proto, m2, LT_TRUE) >= 0);
        s2 = lt_ecu_mem_pagesrc(m2);
        lt_tblview_init(&v2);
        LT_CHECK(lt_tblview_load_table(&v2, r.ini, tbl, &s2) == 0);
        readback = lt_tblview_get(&v2, 3, 4);
        LT_CHECK_NEAR(readback, after, zc->scale * 1.01);
        LT_CHECK_NEAR(lt_tblview_get(&v2, 0, 0), lt_tblview_get(&v, 0, 0), 1e-9);
        lt_tblview_free(&v2);
        lt_ecu_mem_free(m2);
    }
    /* reboot: edit survived because it was burned */
    lt_ecusim_reboot(r.sim);
    {
        lt_ecu_mem *m3 = lt_ecu_mem_from_ini(r.ini);
        lt_pagesrc s3;
        double c = 0;

        LT_CHECK(lt_proto_read_page(r.proto, page, m3) == 0);
        s3 = lt_ecu_mem_pagesrc(m3);
        LT_CHECK(lt_ini_const_get(r.ini, zc, &s3, 3 * v.cols + 4, &c) == 0);
        LT_CHECK_NEAR(c, after, zc->scale * 1.01);
        lt_ecu_mem_free(m3);
    }
    lt_tblview_free(&v);
    lt_tune_free(tune);
    rig_close(&r);
}

/* ------------------------------------------------------------------ */
/* (c) realtime poll -> datalog recorder -> MLG read back               */
/* ------------------------------------------------------------------ */
#define NFRAMES 200
#define MLG_PATH OUT_DIR "/integration.mlg"

static char **g_names;
static char **g_units;
static size_t g_nch;
static double g_rpm_max;

LT_TEST(realtime_to_mlg)
{
    demo_rig r;
    lt_ini_rt *rt;
    lt_recorder *rec;
    lt_buf blk;
    double *vals;
    int f;
    size_t i;
    lt_log log;
    double rpm_seen_max = 0.0;
    int idx;
    double t_first = 0.0, t_last = 0.0;

    LT_CHECK(rig_open(&r) == 0);
    if (!r.mem) {
        rig_close(&r);
        return;
    }
    g_nch = lt_datalog_channels_from_ini(r.ini, &g_names, &g_units);
    LT_CHECK(g_nch > 10);
    vals = (double *)lt_calloc(g_nch, sizeof(double));
    rt = lt_ini_rt_new(r.ini);
    lt_buf_init(&blk);
    lt_plat_mkdirs(OUT_DIR);

    rec = lt_rec_new((const char *const *)g_names, (const char *const *)g_units, (int)g_nch);
    lt_rec_set_sample_rate(rec, 20.0);
    LT_CHECK(lt_rec_start(rec, MLG_PATH) == 0);

    lt_ecusim_tick_engine(r.sim, 50);   /* switch to manual ticking */
    lt_ecusim_set_mode(r.sim, LT_SIM_LIGHT_LOAD);
    for (f = 0; f < NFRAMES; f++) {
        double rpm = 0.0;

        lt_ecusim_tick_engine(r.sim, 100);
        g_now += 100.0;
        lt_buf_clear(&blk);
        LT_CHECK(lt_proto_get_realtime(r.proto, &blk) == 0);
        LT_CHECK(blk.len == lt_ini_realtime_size(r.ini));
        LT_CHECK(lt_ini_rt_update(rt, (const lt_u8 *)blk.data, blk.len) == 0);
        for (i = 0; i < g_nch; i++) {
            double v = 0.0;

            if (!lt_ini_rt_get(rt, g_names[i], &v)) {
                v = 0.0;
            }
            vals[i] = v;
        }
        lt_ini_rt_get(rt, "RPMValue", &rpm);
        if (rpm > rpm_seen_max) rpm_seen_max = rpm;
        if (f == 0) t_first = g_now;
        t_last = g_now;
        LT_CHECK(lt_rec_record_at(rec, g_now, vals, (int)g_nch) == 1);
    }
    g_rpm_max = rpm_seen_max;
    LT_CHECK(rpm_seen_max > 500.0);
    LT_CHECK(lt_rec_count(rec) == NFRAMES);
    LT_CHECK(lt_rec_stop(rec) == 0);
    lt_rec_free(rec);

    lt_log_init(&log);
    LT_CHECK(lt_log_read(MLG_PATH, &log) == 0);
    LT_CHECK(log.n == NFRAMES);
    LT_CHECK((size_t)log.n_ch == g_nch);
    for (i = 0; i < g_nch && i < (size_t)log.n_ch; i++) {
        /* MLG field names are limited to 33 chars */
        char exp[40];

        lt_strlcpy(exp, g_names[i], 34);
        if (strcmp(exp, log.names[i]) != 0) {
            fprintf(stderr, "  channel %lu: '%s' vs '%s'\n", (unsigned long)i, exp, log.names[i]);
        }
        LT_CHECK_STR(log.names[i], exp);
    }
    idx = lt_log_channel_index(&log, "RPMValue");
    LT_CHECK(idx >= 0);
    if (idx >= 0) {
        double mx = 0.0;
        long k;

        for (k = 0; k < log.n; k++) {
            if (LT_LOG_AT(&log, k, idx) > mx) mx = LT_LOG_AT(&log, k, idx);
        }
        LT_CHECK_NEAR(mx, rpm_seen_max, 1.0);
    }
    LT_CHECK_NEAR(lt_log_duration(&log), (t_last - t_first) / 1000.0, 0.01);
    lt_log_free(&log);

    lt_buf_free(&blk);
    lt_free(vals);
    lt_ini_rt_free(rt);
    rig_close(&r);
}

/* ------------------------------------------------------------------ */
/* (d) replay the log through autotune                                  */
/* ------------------------------------------------------------------ */
LT_TEST(replay_autotune)
{
    demo_rig r;
    lt_log log;
    lt_at_channels ch;
    lt_replay_config cfg;
    lt_replay_report rep;
    const lt_ini_table *tbl;
    lt_pagesrc src;
    lt_tblview ve;
    int rc;

    LT_CHECK(lt_file_exists(MLG_PATH));
    LT_CHECK(rig_open(&r) == 0);
    if (!r.mem) {
        rig_close(&r);
        return;
    }
    LT_CHECK(lt_proto_read_all(r.proto, r.mem, LT_TRUE) >= 0);
    src = lt_ecu_mem_pagesrc(r.mem);
    tbl = lt_ini_find_table(r.ini, "veTableTbl");
    LT_CHECK(tbl != NULL);
    lt_tblview_init(&ve);
    LT_CHECK(tbl && lt_tblview_load_table(&ve, r.ini, tbl, &src) == 0);

    lt_log_init(&log);
    LT_CHECK(lt_log_read(MLG_PATH, &log) == 0);
    lt_at_channels_from_ini(r.ini, &ch);
    fprintf(stderr, "  autotune channels: rpm=%s load=%s afr=%s ve=%s clt=%s lambda=%d\n", ch.rpm,
            ch.load, ch.afr, ch.ve, ch.clt, (int)ch.afr_is_lambda);
    LT_CHECK(lt_log_channel_index(&log, ch.rpm) >= 0);
    LT_CHECK(lt_log_channel_index(&log, ch.load) >= 0);
    LT_CHECK(lt_log_channel_index(&log, ch.afr) >= 0);

    lt_replay_config_default(&cfg);
    cfg.filters.min_clt = -40.0;      /* demo coolant is reported in the INI's units */
    cfg.filters.min_rpm = 300.0;
    cfg.filters.max_tps_rate = 1000.0;
    cfg.validate = LT_FALSE;
    memset(&rep, 0, sizeof rep);
    rc = lt_replay(&log, &ch, ve.x, ve.cols, ve.y, ve.rows, NULL, &cfg, &rep);
    LT_CHECK(rc == 0);
    if (rc == 0) {
        size_t i;

        fprintf(stderr, "  replay: samples=%lu cells=%lu rejections=%lu\n", rep.total_samples,
                (unsigned long)rep.ncells, (unsigned long)rep.n_rejections);
        for (i = 0; i < rep.n_rejections && i < 6; i++)
            fprintf(stderr, "    reject %lu x %s\n", rep.rejections[i].count,
                    rep.rejections[i].reason);
        {
            unsigned long rej = 0;

            for (i = 0; i < rep.n_rejections; i++) rej += rep.rejections[i].count;
            LT_CHECK(rep.total_samples + rej == (unsigned long)log.n);
        }
        LT_CHECK(rep.ncells > 0);
        for (i = 0; i < rep.ncells; i++) {
            LT_CHECK(rep.cells[i].x < ve.cols && rep.cells[i].y < ve.rows);
            LT_CHECK(rep.cells[i].hits > 0);
            LT_CHECK(lt_isfinite(rep.cells[i].proposed_ve));
        }
        lt_replay_report_free(&rep);
    }
    lt_log_free(&log);
    lt_tblview_free(&ve);
    rig_close(&r);
}

/* ------------------------------------------------------------------ */
/* (e) project, save tune, restore points                               */
/* ------------------------------------------------------------------ */
LT_TEST(project_restore_points)
{
    lt_project p;
    lt_project_info *list = NULL;
    size_t n = 0, i, nr = 0;
    lt_restore_info *rl = NULL;
    lt_ini *ini;
    lt_tune *t;
    char *path = NULL, *tune_path;
    const lt_ini_const *c;
    lt_tune_value v;
    const lt_tune_entry *e;
    lt_project p2;
    int rc;

    lt_plat_mkdirs(OUT_DIR "/projects");
    {
        /* clean leftovers of a previous run */
        char *old = lt_path_join(OUT_DIR "/projects", "ItProject");
        char **names = NULL;
        int cnt, k;

        if (lt_plat_is_dir(old)) {
            char *rp = lt_path_join(old, "restorePoints");
            cnt = lt_plat_list_dir(rp, &names);
            for (k = 0; k < cnt; k++) {
                char *f = lt_path_join(rp, names[k]);
                lt_plat_remove(f);
                lt_free(f);
            }
            if (cnt > 0) lt_plat_free_list(names, cnt);
            lt_free(rp);
        }
        lt_free(old);
    }
    memset(&p, 0, sizeof p);
    rc = lt_project_create("ItProject", DEMO_INI, "speeduino 202501-T41", OUT_DIR "/projects", &p);
    if (rc == LT_EEXIST) {
        rc = lt_project_open(OUT_DIR "/projects/ItProject", &p);
    }
    LT_CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  project: %s\n", lt_err_get());
        return;
    }
    ini = lt_project_load_ini(&p, NULL);
    LT_CHECK(ini != NULL);
    if (!ini) {
        lt_project_free(&p);
        return;
    }
    t = lt_tune_from_defaults(ini);
    LT_CHECK(t != NULL);
    c = lt_ini_find_const(ini, "compressionRatio");
    LT_CHECK(c != NULL);
    if (p.tune) lt_tune_free(p.tune);
    p.tune = t;
    p.dirty = LT_TRUE;
    lt_tune_value_init(&v);
    v.kind = LT_TV_SCALAR;
    v.scalar = 10.5;
    lt_tune_set_constant(t, "compressionRatio", &v, c ? c->page : 0);
    LT_CHECK(lt_project_save_tune(&p) == 0);
    tune_path = lt_project_file(&p, "CurrentTune.msq");
    LT_CHECK(lt_file_exists(tune_path));
    lt_free(tune_path);

    LT_CHECK(lt_restore_create(&p, &path) == 0);
    LT_CHECK(path != NULL && lt_file_exists(path));
    lt_free(path);

    /* change the tune, create a second point, list */
    v.scalar = 8.5;
    lt_tune_set_constant(p.tune, "compressionRatio", &v, c ? c->page : 0);
    LT_CHECK(lt_restore_create(&p, &path) == 0);
    lt_free(path);
    LT_CHECK(lt_restore_list(&p, &rl, &nr) == 0);
    LT_CHECK(nr >= 2);
    for (i = 0; i + 1 < nr; i++) {
        LT_CHECK(strcmp(rl[i].created, rl[i + 1].created) >= 0);   /* newest first */
    }
    /* restore the OLDEST (reqFuel 10.5) */
    if (nr >= 2) {
        char fname[256];

        lt_strlcpy(fname, rl[nr - 1].filename, sizeof fname);
        LT_CHECK(lt_restore_load(&p, fname) == 0);
        LT_CHECK(p.dirty);
        e = lt_tune_get_constant(p.tune, "compressionRatio");
        LT_CHECK(e != NULL);
        if (e) LT_CHECK_NEAR(e->value.scalar, 10.5, 1e-9);
    }
    lt_restore_info_free(rl, nr);
    LT_CHECK(lt_project_save_tune(&p) == 0);
    lt_project_free(&p);

    /* reopen and list projects */
    memset(&p2, 0, sizeof p2);
    LT_CHECK(lt_project_open(OUT_DIR "/projects/ItProject", &p2) == 0);
    LT_CHECK(p2.tune != NULL);
    if (p2.tune) {
        e = lt_tune_get_constant(p2.tune, "compressionRatio");
        LT_CHECK(e != NULL);
        if (e) LT_CHECK_NEAR(e->value.scalar, 10.5, 1e-9);
    }
    lt_project_free(&p2);
    LT_CHECK(lt_project_list(&list, &n, OUT_DIR "/projects") == 0);
    {
        lt_bool found = LT_FALSE;
        for (i = 0; i < n; i++) {
            if (strcmp(list[i].name, "ItProject") == 0) found = LT_TRUE;
        }
        LT_CHECK(found);
    }
    lt_project_info_free(list, n);
    lt_ini_free(ini);
}

/* ------------------------------------------------------------------ */
/* (f) dashboard templates vs demo.ini output channels                  */
/* ------------------------------------------------------------------ */
static size_t check_template(const char *label, lt_dash_file *d, const lt_ini *ini)
{
    size_t i, bad = 0, total = 0;
    lt_dash_report rep;

    LT_CHECK(d != NULL);
    if (!d) return 0;
    for (i = 0; i < d->cluster.n_comps; i++) {
        const lt_dash_comp *c = &d->cluster.comps[i];
        const char *ch = c->gauge ? c->gauge->output_channel
                                  : (c->indicator ? c->indicator->output_channel : NULL);

        if (!ch || !ch[0]) continue;
        total++;
        if (!lt_ini_find_channel(ini, ch)) {
            bad++;
            fprintf(stderr, "  UNRESOLVED [%s] %s '%s'\n", label,
                    c->gauge ? "gauge" : "indicator", ch);
        }
    }
    {
        /* same through the validator */
        char **names = (char **)lt_calloc(ini->n_channels + 1, sizeof(char *));
        size_t k;
        size_t unknown = 0;

        for (k = 0; k < ini->n_channels; k++) names[k] = ini->channels[k]->name;
        lt_dash_validate(d, (const char *const *)names, ini->n_channels, &rep);
        for (k = 0; k < rep.n; k++) {
            if (rep.issues[k].kind == LT_DI_UNKNOWN_CHANNEL) unknown++;
        }
        LT_CHECK(unknown == bad);
        lt_dash_report_free(&rep);
        lt_free(names);
    }
    fprintf(stderr, "  template %s: %lu channel refs, %lu unresolved\n", label,
            (unsigned long)total, (unsigned long)bad);
    LT_CHECK(total > 0);
    lt_dash_free(d);
    return bad;
}

LT_TEST(dash_templates_resolve)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    size_t bad = 0;

    LT_CHECK(ini != NULL);
    if (!ini) return;
    /* raw templates use generic names (rpm, afr...) - report how many the INI knows as-is */
    bad += check_template("basic", lt_dash_template_basic(), ini);
    bad += check_template("tuning", lt_dash_template_tuning(), ini);
    bad += check_template("telemetry", lt_dash_template_telemetry(), ini);
    fprintf(stderr, "  raw unresolved template channels vs demo.ini: %lu\n", (unsigned long)bad);
    /* after lt_dash_resolve_channels every gauge the INI can supply must resolve */
    {
        lt_dash_file *(*mk[3])(void);
        size_t k, left = 0;

        mk[0] = lt_dash_template_basic;
        mk[1] = lt_dash_template_tuning;
        mk[2] = lt_dash_template_telemetry;
        for (k = 0; k < 3; k++) {
            lt_dash_file *d = mk[k]();
            size_t u = lt_dash_resolve_channels(d, ini);

            left += u;
            check_template("resolved", d, ini);
        }
        fprintf(stderr, "  unresolved after alias remap: %lu\n", (unsigned long)left);
        /* demo.ini has no 'sync' status channel (rusEFI-style output set) */
        LT_CHECK(left <= 1);
    }
    /* the frontpage generator must resolve everything by construction */
    {
        lt_dash_file *fp = lt_dash_from_frontpage(ini, NULL);
        LT_CHECK(fp != NULL);
        if (fp) {
            LT_CHECK(check_template("frontpage", fp, ini) == 0);
        }
    }
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(fixtures_msq_roundtrip);
    LT_RUN(demo_edit_ve_write_burn);
    LT_RUN(realtime_to_mlg);
    LT_RUN(replay_autotune);
    LT_RUN(project_restore_points);
    LT_RUN(dash_templates_resolve);
    if (g_names) lt_strv_free(g_names, g_nch);
    if (g_units) lt_strv_free(g_units, g_nch);
    (void)g_rpm_max;
    LT_TEST_MAIN_END
}
