/* test_history.c - tune history branches, restore point cleanup, annotations, tune CSV and the
 * app-level history commands (G-10, G-14, G-15, G-16).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/project/annot.h"
#include "../src/tune/tune_csv.h"

static int n_dialogs_of(lt_app *app, const char *kind)
{
    int i, n = 0;
    for (i = 0; i < app->n_dialogs; i++) { if (strcmp(app->dialogs[i]->kind, kind) == 0) { n++; } }
    return n;
}

static char *read_tune(const char *dir)
{
    char *f = lt_path_join(dir, "CurrentTune.msq"), *t;
    size_t len = 0;
    t = lt_file_read(f, &len);
    lt_free(f);
    return t;
}

static void write_tune(const char *dir, const char *text)
{
    char *f = lt_path_join(dir, "CurrentTune.msq");
    LT_CHECK(lt_file_write(f, text, strlen(text)) == 0);
    lt_free(f);
}

static size_t hist_ids(const char *dir, char ids[8][9])
{
    lt_snap_info *h = NULL;
    size_t n = 0, i;
    LT_CHECK(lt_snap_history(dir, 8, &h, &n) == 0);
    for (i = 0; i < n && i < 8; i++) { memcpy(ids[i], h[i].id, 9); }
    lt_snap_info_free(h, n);
    return n;
}

/* unique per run: lt_plat_now_ms may be an uptime counter that repeats across runs */
static void unique_name(char *out, size_t cap, const char *prefix)
{
    char ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(out, cap, "%s%s_%lu", prefix, ts, (unsigned long)lt_plat_now_ms());
}

LT_TEST(content_equal_ignores_write_date)
{
    static const char a[] = "<msq><bibliography writeDate=\"Mon Nov 17 10:00:00 2026\"/><v>1</v></msq>";
    static const char b[] = "<msq><bibliography writeDate=\"Mon Nov 17 10:00:01 2026\"/><v>1</v></msq>";
    static const char c[] = "<msq><bibliography writeDate=\"Mon Nov 17 10:00:01 2026\"/><v>2</v></msq>";
    LT_CHECK(lt_snap_content_equal(a, strlen(a), b, strlen(b)) == 1);
    LT_CHECK(lt_snap_content_equal(a, strlen(a), c, strlen(c)) == 0);
    LT_CHECK(lt_snap_content_equal(a, strlen(a), a, strlen(a) - 1) == 0);
    LT_CHECK(lt_snap_content_equal("", 0, "", 0) == 1);
    {
        static const char m1[] = "<iniMetadata name=\"d.ini\" savedAt=\"2026-11-17T23:46:57+00:00\"/>";
        static const char m2[] = "<iniMetadata name=\"d.ini\" savedAt=\"2026-11-17T23:46:58+00:00\"/>";
        static const char m3[] = "<iniMetadata name=\"e.ini\" savedAt=\"2026-11-17T23:46:58+00:00\"/>";
        LT_CHECK(lt_snap_content_equal(m1, strlen(m1), m2, strlen(m2)) == 1);
        LT_CHECK(lt_snap_content_equal(m1, strlen(m1), m3, strlen(m3)) == 0);
    }
}

LT_TEST(branches)
{
    char dir[96];
    char a[9], b[9], c[9], d[9], ids[8][9], cur[64], *t;
    lt_snap_branch *br = NULL;
    size_t nb = 0, n;
    unique_name(dir, sizeof(dir), "build/test_hist_br");
    (void)lt_plat_mkdirs(dir);
    write_tune(dir, "tune-A");
    LT_CHECK(!lt_snap_exists(dir));
    LT_CHECK(lt_snap_branch_create(dir, "exp") == LT_ENOENT);          /* nothing to branch from */
    LT_CHECK(lt_snap_commit(dir, "A", NULL, "", a) == 0);
    LT_CHECK(lt_snap_exists(dir) && lt_snap_enabled(dir));
    write_tune(dir, "tune-B");
    LT_CHECK(lt_snap_has_changes(dir) == 1);
    LT_CHECK(lt_snap_commit(dir, "B", NULL, "", b) == 0);
    LT_CHECK(lt_snap_has_changes(dir) == 0);
    lt_snap_branch_current(dir, cur);
    LT_CHECK_STR(cur, "main");

    LT_CHECK(!lt_snap_branch_name_ok("") && !lt_snap_branch_name_ok("-x") && !lt_snap_branch_name_ok("a b"));
    LT_CHECK(lt_snap_branch_name_ok("high-boost_1.2/x"));
    LT_CHECK(lt_snap_branch_create(dir, "a b") == LT_EINVAL);
    LT_CHECK(lt_snap_branch_create(dir, "exp") == 0);
    LT_CHECK(lt_snap_branch_create(dir, "exp") == LT_EEXIST);
    LT_CHECK(lt_snap_branches(dir, &br, &nb) == 0 && nb == 2);
    LT_CHECK(strcmp(br[0].name, "main") == 0 && br[0].is_current && !br[1].is_current);
    LT_CHECK_STR(br[0].tip, b);
    LT_CHECK_STR(br[1].tip, b);
    lt_free(br);

    /* work on exp */
    LT_CHECK(lt_snap_branch_switch(dir, "exp", LT_FALSE) == 0);
    write_tune(dir, "tune-C");
    LT_CHECK(lt_snap_branch_switch(dir, "main", LT_FALSE) == LT_EBUSY);   /* uncommitted change */
    LT_CHECK(lt_snap_commit(dir, "C", NULL, "", c) == 0);
    n = hist_ids(dir, ids);
    LT_CHECK(n == 3 && strcmp(ids[0], c) == 0 && strcmp(ids[1], b) == 0 && strcmp(ids[2], a) == 0);

    /* back on main: C is not part of its history, the files are B again */
    LT_CHECK(lt_snap_branch_switch(dir, "main", LT_FALSE) == 0);
    t = read_tune(dir);
    LT_CHECK(t != NULL && strcmp(t, "tune-B") == 0);
    lt_free(t);
    n = hist_ids(dir, ids);
    LT_CHECK(n == 2 && strcmp(ids[0], b) == 0 && strcmp(ids[1], a) == 0);
    LT_CHECK(lt_snap_has_changes(dir) == 0);
    write_tune(dir, "tune-D");
    LT_CHECK(lt_snap_commit(dir, "D", NULL, "", d) == 0);
    n = hist_ids(dir, ids);
    LT_CHECK(n == 3 && strcmp(ids[0], d) == 0 && strcmp(ids[1], b) == 0);

    LT_CHECK(lt_snap_branch_switch(dir, "exp", LT_FALSE) == 0);
    t = read_tune(dir);
    LT_CHECK(t != NULL && strcmp(t, "tune-C") == 0);
    lt_free(t);
    n = hist_ids(dir, ids);
    LT_CHECK(n == 3 && strcmp(ids[0], c) == 0);
    LT_CHECK(lt_snap_branch_switch(dir, "nope", LT_TRUE) == LT_ENOENT);

    /* pruning keeps every branch tip */
    LT_CHECK(lt_snap_prune(dir, 1) == 0);
    LT_CHECK(lt_snap_branch_switch(dir, "main", LT_TRUE) == 0);
    t = read_tune(dir);
    LT_CHECK(t != NULL && strcmp(t, "tune-D") == 0);
    lt_free(t);
    LT_CHECK(lt_snap_set_enabled(dir, LT_FALSE) == 0 && !lt_snap_enabled(dir) && lt_snap_exists(dir));
    LT_CHECK(lt_snap_set_enabled(dir, LT_TRUE) == 0 && lt_snap_enabled(dir));
}

LT_TEST(commit_format)
{
    char *m = lt_snap_expand_format("Tune saved on {date} at {time} ({table}) {x}", "veTable1",
                                    "2026-03-04 15:16:17");
    LT_CHECK_STR(m, "Tune saved on 2026-03-04 at 15:16:17 (veTable1) {x}");
    lt_free(m);
    m = lt_snap_expand_format("{table}|{date}", NULL, "2026-03-04 15:16:17");
    LT_CHECK_STR(m, "|2026-03-04");
    lt_free(m);
}

LT_TEST(annotations)
{
    lt_annot_set s, s2;
    size_t idx[8], n;
    const lt_annot *a;
    char cell[64];
    lt_annot_init(&s);
    lt_annot_init(&s2);
    LT_CHECK(lt_annot_put(&s, "", "x", LT_ANNOT_INFO, "") == LT_EINVAL);
    LT_CHECK(lt_annot_put(&s, "veTable1", "  ", LT_ANNOT_INFO, "") == LT_EINVAL);
    LT_CHECK(lt_annot_put(&s, "veTable1", "lean at idle", LT_ANNOT_WARNING, "rh") == 0);
    lt_annot_cell_key(cell, sizeof(cell), "veTable1", 3, 5);
    LT_CHECK_STR(cell, "veTable1:3:5");
    LT_CHECK(lt_annot_put(&s, cell, "check this cell", LT_ANNOT_CRITICAL, "") == 0);
    LT_CHECK(lt_annot_put(&s, "veTable10", "other table", LT_ANNOT_NONE, "") == 0);
    LT_CHECK(lt_annot_put(&s, "reqFuel", "calculated", LT_ANNOT_SUCCESS, "") == 0);
    LT_CHECK(lt_annot_count_table(&s, "veTable1") == 2);          /* veTable10 is another table */
    n = lt_annot_order(&s, NULL, idx);
    LT_CHECK(n == 4 && strcmp(s.v[idx[0]].key, cell) == 0);       /* critical first, untagged last */
    LT_CHECK(strcmp(s.v[idx[3]].key, "veTable10") == 0);
    n = lt_annot_order(&s, "veTable1", idx);
    LT_CHECK(n == 2);
    a = lt_annot_get(&s, "veTable1");
    LT_CHECK(a != NULL && a->tag == LT_ANNOT_WARNING && a->modified[0] == '\0');
    LT_CHECK(lt_annot_put(&s, "veTable1", "richer now", LT_ANNOT_INFO, "") == 0);
    a = lt_annot_get(&s, "veTable1");
    LT_CHECK(a != NULL && strcmp(a->text, "richer now") == 0 && a->modified[0] != '\0' &&
             strcmp(a->author, "rh") == 0 && s.n == 4);
    LT_CHECK(lt_annot_tag_parse("TODO") == LT_ANNOT_TODO && lt_annot_tag_parse("zzz") == LT_ANNOT_NONE);

    LT_CHECK(lt_plat_mkdirs("build/test_annot") == 0);
    LT_CHECK(lt_annot_save(&s, "build/test_annot/annotations.json") == 0);
    LT_CHECK(lt_annot_load(&s2, "build/test_annot/annotations.json") == 0);
    LT_CHECK(s2.n == 4);
    a = lt_annot_get(&s2, cell);
    LT_CHECK(a != NULL && a->tag == LT_ANNOT_CRITICAL && strcmp(a->text, "check this cell") == 0 &&
             a->created[0] != '\0');
    LT_CHECK(lt_annot_delete(&s2, cell) && !lt_annot_delete(&s2, cell) && s2.n == 3);
    LT_CHECK(lt_file_write("build/test_annot/bad.json", "{ nope", 6) == 0);
    LT_CHECK(lt_annot_load(&s2, "build/test_annot/bad.json") == LT_EDATA && s2.n == 0);
    LT_CHECK(lt_annot_load(&s2, "build/test_annot/missing.json") == 0 && s2.n == 0);
    lt_annot_free(&s);
    lt_annot_free(&s2);
}

LT_TEST(csv_split)
{
    char *f[8];
    size_t n, i;
    const char *line = "name,\"[1,2,3]\",\"say \"\"hi\"\"\", spaced ,gear N\xC2\xB0,";
    n = lt_csv_split(line, strlen(line), f, 8);
    LT_CHECK(n == 6);
    LT_CHECK_STR(f[0], "name");
    LT_CHECK_STR(f[1], "[1,2,3]");
    LT_CHECK_STR(f[2], "say \"hi\"");
    LT_CHECK_STR(f[3], "spaced");
    LT_CHECK_STR(f[4], "gear N\xC2\xB0");
    LT_CHECK_STR(f[5], "");
    for (i = 0; i < n; i++) { lt_free(f[i]); }
}

static const lt_ini_const *find_scalar(const lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *k = ini->constants[i];
        if (!k->is_pc_var && k->shape.kind == LT_SH_SCALAR && k->type != LT_DT_BITS && k->type != LT_DT_STRING &&
            k->size_bytes > 0 && k->max > k->min && k->range_resolved) { return k; }
    }
    return NULL;
}

LT_TEST(csv_roundtrip)
{
    lt_app *app = lt_app_new();
    char ini[512], *text, line[400];
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v0, v1, v;
    size_t n = 0, imported = 0;
    lt_csv_import res;
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    LT_CHECK(lt_app_start_demo(app, ini) == 0);
    c = find_scalar(app->ini);
    LT_CHECK(c != NULL);
    if (c == NULL) { lt_app_free(app); return; }
    LT_CHECK(lt_plat_mkdirs("build/test_csv") == 0);
    LT_CHECK(lt_feat_csv_export(app, "build/test_csv/t.csv", &n) == 0);
    LT_CHECK(n == app->ini->n_constants && n > 10);
    text = lt_file_read("build/test_csv/t.csv", &imported);
    LT_CHECK(text != NULL && strncmp(text, "Name,Page,Offset,Shape,Value,Units,Min,Max,Scale,Translate,DataType,IsPcVariable\n", 80) == 0);
    lt_free(text);

    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_const_get(app->ini, c, &ps, 0, &v0) == 0);
    v1 = (v0 == c->min) ? c->max : c->min;
    LT_CHECK(lt_ini_const_set(app->ini, c, &ps, 0, v1) == 0);
    LT_CHECK(lt_feat_csv_import(app, "build/test_csv/t.csv", &imported) == 0);   /* puts v0 back */
    LT_CHECK(imported > 10);
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_const_get(app->ini, c, &ps, 0, &v) == 0);
    LT_CHECK_NEAR(v, v0, 1e-6);

    /* hand written file: legacy 11 columns, unknown name, bad value, out of range, short line */
    lt_snprintf(line, sizeof(line), "Name,Page,Offset,Value\n%s,0,0,%g\nnoSuchConstant,0,0,5\n%s,0,0,abc\nbroken,1\n",
                c->name, c->max + 1000.0, c->name);
    ps = lt_app_pages(app);
    LT_CHECK(lt_tune_csv_import(app->ini, &ps, line, strlen(line), &res) == 0);
    LT_CHECK(res.imported == 1 && res.clamped == 1 && res.unknown == 1 && res.n_messages == 3);
    LT_CHECK(lt_ini_const_get(app->ini, c, &ps, 0, &v) == 0);
    LT_CHECK_NEAR(v, c->max, 1e-6);
    lt_csv_import_free(&res);
    LT_CHECK(lt_feat_csv_import(app, "build/test_csv/missing.csv", NULL) < 0);
    lt_app_free(app);
}

LT_TEST(restore_cleanup)
{
    lt_app *app = lt_app_new();
    char ini[512], name[64];
    lt_restore_info *v = NULL;
    size_t n = 0;
    int i;
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_PROJECTS", "build/test_hist_proj") == 0);
    LT_CHECK(lt_plat_mkdirs("build/test_hist_proj") == 0);
    unique_name(name, sizeof(name), "rp");
    LT_CHECK(lt_app_new_project(app, name, ini) == 0);
    app->project.cfg.settings.max_restore_points = 3;
    for (i = 0; i < 6; i++) { LT_CHECK(lt_restore_create(&app->project, NULL) == 0); }
    LT_CHECK(lt_restore_list(&app->project, &v, &n) == 0 && n == 3);
    lt_restore_info_free(v, n);
    app->project.cfg.settings.max_restore_points = 0;                   /* unlimited */
    for (i = 0; i < 4; i++) { LT_CHECK(lt_restore_create(&app->project, NULL) == 0); }
    LT_CHECK(lt_restore_list(&app->project, &v, &n) == 0 && n == 7);
    lt_restore_info_free(v, n);
    lt_app_free(app);
}

LT_TEST(app_history)
{
    lt_app *app = lt_app_new();
    char ini[512], name[64], id1[9], ids[8][9];
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v0, v1, v;
    size_t n;
    lt_snap_info *h = NULL;
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_PROJECTS", "build/test_hist_proj") == 0);
    LT_CHECK(lt_plat_mkdirs("build/test_hist_proj") == 0);
    unique_name(name, sizeof(name), "hist");
    LT_CHECK(lt_app_new_project(app, name, ini) == 0);
    LT_CHECK(lt_ext_n_settings_pages(app) > 0);
    c = find_scalar(app->ini);
    LT_CHECK(c != NULL);
    if (c == NULL) { lt_app_free(app); return; }
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_const_get(app->ini, c, &ps, 0, &v0) == 0);
    v1 = (v0 == c->min) ? c->max : c->min;

    /* defaults: ask each time; without a store nothing happens on save */
    LT_CHECK(app->settings.auto_commit == 2);
    LT_CHECK(lt_app_save_tune(app) == 0);
    LT_CHECK(n_dialogs_of(app, "history_commit") == 0);
    LT_CHECK(!lt_snap_exists(app->project.path));
    LT_CHECK(lt_feat_history_commit(app, "Initial project commit", NULL, id1) == 0);
    LT_CHECK(lt_snap_exists(app->project.path));

    /* a manual checkpoint keeps the note */
    LT_CHECK(lt_ini_const_set(app->ini, c, &ps, 0, v1) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_feat_history_commit(app, "Manual checkpoint", "changed one value", NULL) == 0);
    LT_CHECK(lt_snap_history(app->project.path, 10, &h, &n) == 0 && n == 2);
    LT_CHECK(h[0].annotation != NULL && strcmp(h[0].annotation, "changed one value") == 0);
    lt_snap_info_free(h, n);

    /* ask: a dialog; always: a commit; never: nothing */
    app->settings.auto_commit = 2;
    LT_CHECK(lt_ini_const_set(app->ini, c, &ps, 0, v0) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_save_tune(app) == 0);
    LT_CHECK(n_dialogs_of(app, "history_commit") == 1);
    app->n_dialogs = 0;
    app->settings.auto_commit = 1;
    LT_CHECK(lt_app_save_tune(app) == 0);                      /* the pending change is committed */
    LT_CHECK(hist_ids(app->project.path, ids) == 3);
    lt_plat_sleep_ms(1100);                     /* the re-save gets new time stamps */
    LT_CHECK(lt_app_save_tune(app) == 0);                      /* nothing changed since: no empty commit */
    LT_CHECK(hist_ids(app->project.path, ids) == 3);
    LT_CHECK(lt_ini_const_set(app->ini, c, &ps, 0, v1) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_save_tune(app) == 0);
    LT_CHECK(hist_ids(app->project.path, ids) == 4);
    app->settings.auto_commit = 0;
    LT_CHECK(lt_ini_const_set(app->ini, c, &ps, 0, v0) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_save_tune(app) == 0);
    LT_CHECK(hist_ids(app->project.path, ids) == 4);
    LT_CHECK(n_dialogs_of(app, "history_commit") == 0);

    /* restore the first version: the tune value is the original again, marked as unsaved */
    LT_CHECK(lt_feat_history_restore(app, id1) == 0);
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_const_get(app->ini, c, &ps, 0, &v) == 0);
    LT_CHECK_NEAR(v, v0, 1e-6);
    LT_CHECK(app->modified);

    /* branches through the app */
    LT_CHECK(lt_snap_branch_create(app->project.path, "exp") == 0);
    LT_CHECK(lt_feat_history_switch(app, "exp") == 0);
    lt_app_free(app);
}

LT_TEST(restore_reloads_config)
{
    lt_app *app = lt_app_new();
    char ini[512], name[64], id1[9], id2[9], *dst, *txt;
    size_t len = 0;
    LT_CHECK(lt_app_find_demo_ini(ini, sizeof(ini)) != NULL);
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_PROJECTS", "build/test_hist_proj") == 0);
    LT_CHECK(lt_plat_mkdirs("build/test_hist_proj") == 0);
    unique_name(name, sizeof(name), "cfg");
    LT_CHECK(lt_app_new_project(app, name, ini) == 0);
    lt_free(app->project.cfg.conn.port);
    app->project.cfg.conn.port = lt_strdup("PORT_A");
    LT_CHECK(lt_project_save_config(&app->project) == 0);
    LT_CHECK(lt_feat_history_commit(app, "A", NULL, id1) == 0);
    lt_free(app->project.cfg.conn.port);
    app->project.cfg.conn.port = lt_strdup("PORT_B");
    LT_CHECK(lt_project_save_config(&app->project) == 0);
    LT_CHECK(lt_feat_history_commit(app, "B", NULL, id2) == 0);
    LT_CHECK(strcmp(app->project.cfg.conn.port, "PORT_B") == 0);
    LT_CHECK(lt_feat_history_restore(app, id1) == 0);
    LT_CHECK(app->project.cfg.conn.port != NULL && strcmp(app->project.cfg.conn.port, "PORT_A") == 0);
    /* a snapshot with another definition INI: the project is reopened from the tick hook */
    dst = lt_project_ini_path(&app->project);
    txt = lt_file_read(dst, &len);
    LT_CHECK(txt != NULL);
    if (txt != NULL) {
        lt_free(txt);
        txt = lt_file_read("tests/fixtures/ini/ms3.ini", &len);
    }
    LT_CHECK(txt != NULL && lt_file_write(dst, txt, len) == 0);
    lt_free(txt);
    lt_free(dst);
    LT_CHECK(lt_feat_history_commit(app, "C", NULL, id2) == 0);
    LT_CHECK(lt_feat_history_restore(app, id1) == 0);
    lt_ext_tick(app);
    LT_CHECK(app->has_project && app->ini != NULL);
    LT_CHECK(app->project.cfg.conn.port != NULL && strcmp(app->project.cfg.conn.port, "PORT_A") == 0);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(content_equal_ignores_write_date);
    LT_RUN(branches);
    LT_RUN(commit_format);
    LT_RUN(annotations);
    LT_RUN(csv_split);
    LT_RUN(csv_roundtrip);
    LT_RUN(restore_cleanup);
    LT_RUN(app_history);
    LT_RUN(restore_reloads_config);
    LT_TEST_MAIN_END
}
