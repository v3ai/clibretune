/* test_tune_safety.c - fault injection on the tune save and burn paths: a full disk while a
 * tune, pc variables, restore point, project.json or settings file is written must leave the
 * previous file byte-identical (no truncation, no stray temporary files); a link drop in the
 * middle of an ECU RAM write or a multi-page burn must be reported as such, must not touch
 * the tune file on disk, and must tell the user the ECU may hold a partial burn.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "lt_codes.h"
#include "plat/plat.h"
#include "tune/tune.h"
#include "project/project.h"
#include "proto/proto.h"
#include "sim/sim.h"
#include "app/app.h"
#include "lt_test.h"

#define DIR "build/tsafe"

static char *slurp(const char *path, size_t *n)
{
    *n = 0;
    return lt_file_read(path, n);
}

static int same_file(const char *path, const char *data, size_t n)
{
    size_t m = 0;
    char *d = slurp(path, &m);
    int same = d != NULL && m == n && memcmp(d, data, n) == 0;
    lt_free(d);
    return same;
}

/* entries of a directory whose name contains ".tmp" (left-over temporary files) */
static int n_tmp_files(const char *dir)
{
    char **names = NULL;
    int n = lt_plat_list_dir(dir, &names), i, k = 0;
    for (i = 0; i < n; i++) {
        if (strstr(names[i], ".tmp") != NULL) k++;
    }
    if (n >= 0) lt_plat_free_list(names, n);
    return k;
}

static void bump(lt_tune *t, const char *name, double v)
{
    lt_tune_value tv;
    lt_tune_value_init(&tv);
    tv.scalar = v;
    lt_tune_set_constant(t, name, &tv, 0);
    lt_tune_value_clear(&tv);
}

LT_TEST(tune_save_disk_full)
{
    const char *path = DIR "/tune.msq";
    lt_tune *t = lt_tune_load("tests/fixtures/tune/sample.msq");
    char *orig;
    size_t on = 0, newlen;
    lt_buf b;
    long limits[6];
    int i;
    LT_CHECK(t != NULL);
    if (t == NULL) return;
    lt_plat_mkdirs(DIR);
    LT_CHECK(lt_tune_save(t, path) == 0);
    orig = slurp(path, &on);
    LT_CHECK(orig != NULL && on > 100);
    /* a longer, different tune */
    lt_tune_set_signature(t, "a much longer signature that changes the size of the file 12345");
    bump(t, "safetyTestValue", 42.0);
    lt_buf_init(&b);
    lt_tune_to_msq(t, &b);
    newlen = b.len;
    limits[0] = 0; limits[1] = 1; limits[2] = 7; limits[3] = (long)(newlen / 2);
    limits[4] = (long)newlen - 1; limits[5] = (long)on;
    for (i = 0; i < 6; i++) {
        lt_plat_fault_write_limit(limits[i]);
        LT_CHECK(lt_tune_save(t, path) != 0);            /* "disk full" is reported */
        LT_CHECK(same_file(path, orig, on));             /* old tune intact, byte for byte */
        LT_CHECK(n_tmp_files(DIR) == 0);                 /* no stray temporary file */
    }
    lt_plat_fault_write_limit((long)newlen);             /* exactly enough room */
    LT_CHECK(lt_tune_save(t, path) == 0);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(same_file(path, b.data, b.len));
    {
        lt_tune *r = lt_tune_load(path);
        LT_CHECK(r != NULL && lt_tune_get_constant(r, "safetyTestValue") != NULL);
        if (r) lt_tune_free(r);
    }
    lt_buf_free(&b);
    lt_free(orig);
    lt_tune_free(t);
}

LT_TEST(project_files_disk_full)
{
    lt_project p;
    char *cur, *pcv, *pj, *rdir, *orig_cur, *orig_pcv, *orig_pj;
    size_t ncur = 0, npcv = 0, npj = 0, nrp = 0, nrp2 = 0;
    char name[64], *rp = NULL;
    lt_restore_info *ri = NULL;
    lt_tune_value tv;
    int rc;
    lt_plat_mkdirs(DIR "/projects");
    lt_snprintf(name, sizeof(name), "safe%lu", (unsigned long)lt_plat_now_ms());
    rc = lt_project_create(name, "tests/fixtures/tune/mini_speeduino.ini", "speeduino 202501",
                           DIR "/projects", &p);
    LT_CHECK(rc == LT_OK);
    if (rc != LT_OK) return;
    p.tune = lt_tune_load("tests/fixtures/tune/sample.msq");
    LT_CHECK(p.tune != NULL);
    if (p.tune == NULL) { lt_project_free(&p); return; }
    lt_tune_value_init(&tv);
    tv.scalar = 3.0;
    lt_tune_set_pc_variable(p.tune, "tsPcVar", &tv);
    LT_CHECK(lt_project_save_tune(&p) == LT_OK);
    LT_CHECK(lt_restore_create(&p, &rp) == LT_OK);
    lt_free(rp);
    cur = lt_project_file(&p, "CurrentTune.msq");
    pcv = lt_project_file(&p, "projectCfg/pcVariableValues.msq");
    pj = lt_project_file(&p, "project.json");
    rdir = lt_project_file(&p, "restorePoints");
    orig_cur = slurp(cur, &ncur);
    orig_pcv = slurp(pcv, &npcv);
    orig_pj = slurp(pj, &npj);
    LT_CHECK(orig_cur != NULL && orig_pcv != NULL && orig_pj != NULL);
    LT_CHECK(lt_restore_list(&p, &ri, &nrp) == LT_OK);
    lt_restore_info_free(ri, nrp);

    /* edit, then the disk fills up while saving: CurrentTune.msq untouched, still dirty */
    bump(p.tune, "safetyTestValue", 7.0);
    tv.scalar = 9.0;
    lt_tune_set_pc_variable(p.tune, "tsPcVar", &tv);
    p.dirty = LT_TRUE;
    lt_plat_fault_write_limit(100);
    LT_CHECK(lt_project_save_tune(&p) != LT_OK);
    LT_CHECK(p.dirty);
    LT_CHECK(same_file(cur, orig_cur, ncur));
    LT_CHECK(same_file(pcv, orig_pcv, npcv));
    /* room for the tune but not for the pc variables: the error is reported (not ignored),
     * the old pcVariableValues.msq survives and the project stays dirty for a retry */
    lt_plat_fault_write_limit(-1);
    {
        lt_buf b;
        lt_buf_init(&b);
        lt_tune_to_msq(p.tune, &b);
        lt_plat_fault_write_limit((long)b.len + 10);
        lt_buf_free(&b);
    }
    LT_CHECK(lt_project_save_tune(&p) != LT_OK);
    LT_CHECK(p.dirty);
    LT_CHECK(same_file(pcv, orig_pcv, npcv));
    lt_plat_fault_write_limit(-1);
    {
        lt_tune *r = lt_tune_load(cur);        /* whatever is on disk is a whole tune */
        LT_CHECK(r != NULL);
        if (r) lt_tune_free(r);
    }

    /* restore point: a failed one leaves no partial .msq behind */
    lt_plat_fault_write_limit(50);
    LT_CHECK(lt_restore_create(&p, &rp) != LT_OK);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(lt_restore_list(&p, &ri, &nrp2) == LT_OK);
    lt_restore_info_free(ri, nrp2);
    LT_CHECK(nrp2 == nrp);
    LT_CHECK(n_tmp_files(rdir) == 0);

    /* project.json: a failed save keeps the project openable */
    lt_plat_fault_write_limit(20);
    LT_CHECK(lt_project_save_config(&p) != LT_OK);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(same_file(pj, orig_pj, npj));
    {
        lt_project q;
        LT_CHECK(lt_project_open(p.path, &q) == LT_OK);
        lt_project_free(&q);
    }
    LT_CHECK(n_tmp_files(p.path) == 0);

    /* and with room again everything saves */
    LT_CHECK(lt_project_save_tune(&p) == LT_OK);
    LT_CHECK(!p.dirty);
    lt_tune_value_clear(&tv);
    lt_free(cur); lt_free(pcv); lt_free(pj); lt_free(rdir);
    lt_free(orig_cur); lt_free(orig_pcv); lt_free(orig_pj);
    lt_project_free(&p);
}

LT_TEST(settings_disk_full)
{
    const char *path = DIR "/settings.conf";
    lt_props pr;
    char *orig;
    size_t n = 0;
    lt_plat_mkdirs(DIR);
    lt_props_init(&pr);
    lt_props_set(&pr, "theme", "dark");
    lt_props_set(&pr, "port", "/dev/ttyACM0");
    LT_CHECK(lt_props_save(&pr, path) == LT_OK);
    orig = slurp(path, &n);
    lt_props_set(&pr, "last_project", "/a/rather/long/path/that/makes/the/file/longer");
    lt_plat_fault_write_limit(10);
    LT_CHECK(lt_props_save(&pr, path) != LT_OK);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(orig != NULL && same_file(path, orig, n));
    LT_CHECK(lt_props_save(&pr, path) == LT_OK);
    lt_props_free(&pr);
    lt_props_init(&pr);
    LT_CHECK(lt_props_load(&pr, path) == LT_OK);
    LT_CHECK(lt_props_get(&pr, "last_project") != NULL);
    lt_props_free(&pr);
    lt_free(orig);
}

/* ---- link drop in the middle of a write / a burn (simulated ECU, virtual clock) ---- */
static double g_now;
static double clk_now(void *ud) { LT_UNUSED(ud); return g_now; }
static void clk_sleep(void *ud, int ms) { LT_UNUSED(ud); g_now += ms; }

static const char *k_ini =
    "[MegaTune]\n signature = \"speeduino 202501\"\n"
    "[TunerStudio]\n queryCommand = \"Q\"\n"
    " messageEnvelopeFormat = msEnvelope_1.0\n endianness = little\n"
    "[Constants]\n pageSize = 128, 96\n blockingFactor = 40\n"
    " pageReadCommand = \"R%2i%2o%2c\", \"R%2i%2o%2c\"\n"
    " pageChunkWrite = \"C%2i%2o%2c%v\", \"C%2i%2o%2c%v\"\n"
    " burnCommand = \"B%2i\", \"B%2i\"\n"
    "page = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "page = 2\n b = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "[OutputChannels]\n ochGetCommand = \"r\\x00\\x30%2o%2c\"\n ochBlockSize = 20\n";

typedef struct rig { lt_ini *ini; lt_ecusim *sim; lt_proto_io io; lt_proto *p; lt_ecu_mem *mem; } rig;

static int rig_open(rig *r)
{
    lt_proto_cfg cfg;
    lt_proto_clock ck;
    memset(r, 0, sizeof *r);
    r->ini = lt_ini_load_string(k_ini, strlen(k_ini), NULL, NULL);
    if (r->ini == NULL) return -1;
    g_now = 1000.0;
    r->sim = lt_ecusim_new(r->ini);
    lt_ecusim_set_clock(r->sim, clk_now, NULL);
    r->io = lt_ecusim_io(r->sim);
    lt_proto_cfg_default(&cfg);
    cfg.auto_burn_on_page_change = LT_FALSE;
    r->p = lt_proto_new(&r->io, r->ini, &cfg);
    ck.ud = NULL; ck.now_ms = clk_now; ck.sleep_ms = clk_sleep;
    lt_proto_set_clock(r->p, &ck);
    if (lt_proto_connect(r->p) != LT_OK) return -1;
    r->mem = lt_ecu_mem_from_ini(r->ini);
    return lt_proto_read_all(r->p, r->mem, LT_TRUE);
}

static void rig_close(rig *r)
{
    if (r->mem) lt_ecu_mem_free(r->mem);
    if (r->p) lt_proto_free(r->p);
    if (r->sim) lt_ecusim_free(r->sim);
    if (r->ini) lt_ini_free(r->ini);
}

/* fill both pages with a new pattern through the shadow (marks them dirty) */
static void edit_all(rig *r, lt_u8 v)
{
    lt_pagesrc ps = lt_ecu_mem_pagesrc(r->mem);
    lt_u8 buf[128];
    memset(buf, v, sizeof buf);
    LT_CHECK(ps.write(ps.ud, 0, 0, buf, 128) == 0);
    LT_CHECK(ps.write(ps.ud, 1, 0, buf, 96) == 0);
}

static int all_bytes(const lt_u8 *d, size_t from, size_t to, lt_u8 v)
{
    size_t i;
    for (i = from; i < to; i++) {
        if (d[i] != v) return 0;
    }
    return 1;
}

LT_TEST(link_drop_mid_write)
{
    rig r;
    size_t n0 = 0, n1 = 0;
    const lt_u8 *f0, *f1;
    LT_CHECK(rig_open(&r) == LT_OK);
    if (r.mem == NULL) { rig_close(&r); return; }
    edit_all(&r, 0x11);
    LT_CHECK(lt_proto_write_dirty(r.p, r.mem) > 0);
    LT_CHECK(lt_proto_burn(r.p, -1, r.mem) == LT_OK);
    f0 = lt_ecusim_flash(r.sim, 0, &n0);
    LT_CHECK(f0 != NULL && all_bytes(f0, 0, n0, 0x11));
    /* the link drops after the second chunk: the RAM write fails, nothing is burned */
    edit_all(&r, 0x22);
    lt_ecusim_fault_write(r.sim, 2);
    LT_CHECK(lt_proto_write_dirty(r.p, r.mem) < 0);
    LT_CHECK(lt_proto_burn(r.p, -1, r.mem) < 0);
    f0 = lt_ecusim_flash(r.sim, 0, &n0);
    f1 = lt_ecusim_flash(r.sim, 1, &n1);
    LT_CHECK(all_bytes(f0, 0, n0, 0x11) && all_bytes(f1, 0, n1, 0x11));   /* flash untouched */
    LT_CHECK(lt_ecu_mem_has_dirty(r.mem) || lt_ecu_mem_has_pending_burn(r.mem));
    rig_close(&r);
}

LT_TEST(link_drop_mid_burn)
{
    rig r;
    size_t n0 = 0, n1 = 0;
    const lt_u8 *f0, *f1;
    int pages[4];
    LT_CHECK(rig_open(&r) == LT_OK);
    if (r.mem == NULL) { rig_close(&r); return; }
    edit_all(&r, 0x33);
    LT_CHECK(lt_proto_write_dirty(r.p, r.mem) > 0);
    LT_CHECK(lt_proto_dirty_pages(r.p, pages, 4) == 2);
    /* page 0 burns, then power/link is lost half way through page 1 */
    lt_ecusim_fault_burn(r.sim, 2, LT_TRUE);
    LT_CHECK(lt_proto_burn(r.p, -1, r.mem) < 0);
    f0 = lt_ecusim_flash(r.sim, 0, &n0);
    f1 = lt_ecusim_flash(r.sim, 1, &n1);
    LT_CHECK(all_bytes(f0, 0, n0, 0x33));                      /* burned */
    LT_CHECK(all_bytes(f1, 0, n1 / 2, 0x33) && all_bytes(f1, n1 / 2, n1, 0));   /* torn */
    /* the protocol knows which page is not confirmed and does not claim success */
    LT_CHECK(lt_proto_dirty_pages(r.p, pages, 4) == 1 && pages[0] == 1);
    LT_CHECK(lt_ecu_mem_has_pending_burn(r.mem));
    rig_close(&r);
}

static int n_dialogs_of(lt_app *app, const char *kind)
{
    int i, k = 0;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0) k++;
    }
    return k;
}

/* a plain scalar on page 0 (the burnable page of the demo INI) */
static const lt_ini_const *scalar_const(const lt_ini *ini, int skip)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (!c->is_pc_var && c->page == 0 && c->shape.cols <= 1 && c->shape.rows <= 1 &&
            c->type != LT_DT_BITS && c->size_bytes > 0 && c->max > c->min && skip-- <= 0) return c;
    }
    return NULL;
}

LT_TEST(app_burn_interrupted)
{
    char buf[512], name[64], *cur, *orig;
    size_t n = 0;
    lt_app *app = lt_app_new();
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v = 0.0;
    int nd;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_PROJECTS", DIR "/appproj") == 0);
    LT_CHECK(lt_plat_mkdirs(DIR "/appproj") == 0);
    lt_snprintf(name, sizeof(name), "burn%lu", (unsigned long)lt_plat_now_ms());
    LT_CHECK(lt_app_new_project(app, name, buf) == 0);
    LT_CHECK(lt_app_connect_demo(app) == 0 && app->sim != NULL);
    nd = n_dialogs_of(app, "error_details");
    c = scalar_const(app->ini, 0);
    LT_CHECK(c != NULL);
    if (c == NULL || app->sim == NULL) { lt_app_free(app); return; }
    LT_CHECK(lt_app_save_tune(app) == 0);
    cur = lt_project_file(&app->project, "CurrentTune.msq");
    orig = slurp(cur, &n);
    LT_CHECK(orig != NULL);

    /* an edit, then the burn is cut short: the user is told about a possible partial burn */
    ps = lt_app_pages(app);
    lt_ini_get(app->ini, &ps, c->name, &v);
    LT_CHECK(lt_ini_set(app->ini, &ps, c->name, v == c->min ? c->max : c->min) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_can_burn(app));
    lt_ecusim_fault_burn(app->sim, 1, LT_TRUE);
    LT_CHECK(lt_app_burn(app) < 0);
    LT_CHECK(strstr(app->status, "partial burn") != NULL);
    LT_CHECK(n_dialogs_of(app, "error_details") == nd + 1);
    LT_CHECK(same_file(cur, orig, n));                 /* tune file not rewritten */
    LT_CHECK(lt_app_can_burn(app) || app->conn == LT_APP_OFFLINE);

    /* RAM write cut short: told that the ECU RAM may hold part of the changes */
    lt_app_disconnect(app);
    LT_CHECK(lt_app_connect_demo(app) == 0 && app->sim != NULL);
    nd = n_dialogs_of(app, "error_details");
    if (app->sim != NULL) {
        c = scalar_const(app->ini, 1);
        ps = lt_app_pages(app);
        if (c != NULL) {
            lt_ini_get(app->ini, &ps, c->name, &v);
            lt_ini_set(app->ini, &ps, c->name, v == c->min ? c->max : c->min);
        }
        lt_ecusim_fault_write(app->sim, 1);
        LT_CHECK(lt_app_burn(app) < 0);
        LT_CHECK(strstr(app->status, "partial update") != NULL);
        LT_CHECK(n_dialogs_of(app, "error_details") >= 1 && nd >= 0);   /* (one dialog of a kind) */
        LT_CHECK(same_file(cur, orig, n));
    }
    lt_free(cur);
    lt_free(orig);
    lt_app_free(app);
}

/* A cut-off CurrentTune.msq (crash of an older version, copied half-way, ...) is not loaded as
 * if it were whole: the project opens with defaults, the file is kept aside, the user is told,
 * and a later save cannot overwrite it. */
LT_TEST(app_open_truncated_tune)
{
    char buf[512], name[64], *dir, *cur, *kept, *orig, *back;
    size_t n = 0, m = 0;
    lt_app *app = lt_app_new();
    int nd;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_PROJECTS", DIR "/appproj") == 0);
    LT_CHECK(lt_plat_mkdirs(DIR "/appproj") == 0);
    lt_snprintf(name, sizeof(name), "trunc%lu", (unsigned long)lt_plat_now_ms());
    LT_CHECK(lt_app_new_project(app, name, buf) == 0);
    LT_CHECK(lt_app_save_tune(app) == 0);
    dir = lt_strdup(app->project.path);
    cur = lt_path_join(dir, "CurrentTune.msq");
    kept = lt_path_join(dir, "CurrentTune.unreadable.msq");
    orig = slurp(cur, &n);
    LT_CHECK(orig != NULL && n > 100);
    lt_app_close_project(app);
    if (orig == NULL) { lt_free(dir); lt_free(cur); lt_free(kept); lt_app_free(app); return; }
    LT_CHECK(lt_file_write(cur, orig, n / 2) == 0);
    {   /* the project layer refuses it */
        lt_project p;
        LT_CHECK(lt_project_open(dir, &p) == 0);
        LT_CHECK(p.tune == NULL);
        LT_CHECK(lt_project_load_tune(&p) == LT_EDATA);
        lt_project_close(&p);
    }
    nd = n_dialogs_of(app, "error_details");
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(app->project.tune != NULL);
    LT_CHECK(n_dialogs_of(app, "error_details") == nd + 1);
    LT_CHECK(!lt_file_exists(cur));
    LT_CHECK(same_file(kept, orig, n / 2));
    LT_CHECK(lt_app_save_tune(app) == 0);              /* a new tune; the old one stays aside */
    LT_CHECK(same_file(kept, orig, n / 2));
    back = slurp(cur, &m);
    LT_CHECK(back != NULL && m > 100);
    lt_free(back);
    lt_free(orig);
    lt_free(kept);
    lt_free(cur);
    lt_free(dir);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(tune_save_disk_full);
    LT_RUN(project_files_disk_full);
    LT_RUN(settings_disk_full);
    LT_RUN(link_drop_mid_write);
    LT_RUN(link_drop_mid_burn);
    LT_RUN(app_burn_interrupted);
    LT_RUN(app_open_truncated_tune);
    LT_TEST_MAIN_END
}
