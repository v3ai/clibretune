/* test_inilib.c - INI library lookup by signature, project INI change + migration report,
 * tune compare logic and its dialogs (G-11, G-12, G-13).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/project/inilib.h"
#include "../src/project/proj_priv.h"
#include "../src/tune/tunecmp.h"

#define TMP "build/test_tmp/inilib_"

static char *fresh(const char *name)
{
    char *p = (char *)lt_malloc(strlen(TMP) + strlen(name) + 1);
    strcpy(p, TMP);
    strcat(p, name);
    lt_proj_rmtree(p);
    lt_plat_mkdirs(p);
    return p;
}

/* ---------------------------------------------------------------- signatures */
LT_TEST(sig_normalize)
{
    char b[64];
    LT_CHECK(lt_sig_normalize("  rusEFI 2025.06.26-epicECU_1 ", b, sizeof(b)) == 27);
    LT_CHECK_STR(b, "rusefi 2025 06 26 epicecu 1");
    LT_CHECK(lt_sig_normalize("abcdef", b, 4) == 3);
    LT_CHECK_STR(b, "abc");
    LT_CHECK(lt_sig_normalize(NULL, b, sizeof(b)) == 0);
}

LT_TEST(sig_compare)
{
    LT_CHECK(lt_sig_compare("speeduino 202501", "Speeduino  202501", NULL) == LT_SIG_EXACT);
    LT_CHECK(lt_sig_compare("speeduino 202501-a", "speeduino 202501", NULL) == LT_SIG_PARTIAL);
    LT_CHECK(lt_sig_compare("speeduino 202505", "speeduino 202501", "speeduino 2025") == LT_SIG_EXACT);
    LT_CHECK(lt_sig_compare("rusEFI master.2024.simulator.12345678", "rusEFI 2025.x.12345678",
                            NULL) == LT_SIG_EXACT);
    LT_CHECK(lt_sig_compare("rusEFI 2024", "rusEFI 2025 other", NULL) == LT_SIG_PARTIAL);
    LT_CHECK(lt_sig_compare("uaefi 1", "epicefi 2", NULL) == LT_SIG_MISMATCH);
    LT_CHECK(lt_sig_compare("MS3 1.5", "speeduino 2", NULL) == LT_SIG_MISMATCH);
    LT_CHECK(lt_sig_compare("", "speeduino", NULL) == LT_SIG_MISMATCH);
}

LT_TEST(inilib_find_sorted)
{
    char *dir = fresh("repo");
    lt_ini_repo r;
    lt_inilib_match m[4];
    size_t n, i;
    char *id = NULL;
    LT_CHECK(lt_ini_repo_open(&r, dir) == 0);
    LT_CHECK(lt_ini_repo_import(&r, "tests/fixtures/project/mini.ini", &id) == 0);
    lt_free(id);
    LT_CHECK(lt_ini_repo_import(&r, "tests/fixtures/ini/demo.ini", &id) == 0);
    lt_free(id);
    LT_CHECK(r.n == 2);
    n = lt_inilib_find(&r, "speeduino 202501-mini", m, 4);
    LT_CHECK(n == 1 && m[0].match == LT_SIG_EXACT);
    LT_CHECK(n >= 1 && r.e[m[0].index].signature != NULL &&
             strcmp(r.e[m[0].index].signature, "speeduino 202501-mini") == 0);
    LT_CHECK(lt_inilib_find(&r, "nothing like it", m, 4) == 0);
    LT_CHECK(lt_inilib_find(&r, "speeduino 202501-mini", NULL, 0) == 1);
    /* ordering: force two matches, exact first */
    lt_free(r.e[0].signature);
    r.e[0].signature = lt_strdup("rusEFI 2025 a");
    lt_free(r.e[1].signature);
    r.e[1].signature = lt_strdup("rusEFI 2025");
    n = lt_inilib_find(&r, "rusEFI 2025", m, 4);
    LT_CHECK(n == 2);
    LT_CHECK(m[0].match == LT_SIG_EXACT && m[1].match == LT_SIG_PARTIAL);
    for (i = 0; i < n; i++) { LT_CHECK(m[i].index < r.n); }
    lt_ini_repo_close(&r);
    lt_free(dir);
}

/* ---------------------------------------------------------------- compare view logic */
LT_TEST(tunecmp_format_select)
{
    lt_tune *a = lt_tune_new("sig"), *b = lt_tune_new("sig");
    lt_tune_value v, w;
    lt_tune_diff *d;
    size_t idx[16], n;
    char buf[80];
    lt_tune_value_init(&v);
    lt_tune_value_init(&w);
    v.scalar = 10; w.scalar = 11;
    lt_tune_set_constant(a, "alpha", &v, 0);
    lt_tune_set_constant(b, "alpha", &w, 0);
    v.scalar = 100; w.scalar = 150;
    lt_tune_set_constant(a, "beta", &v, 0);
    lt_tune_set_constant(b, "beta", &w, 0);
    lt_tune_set_constant(b, "OnlyB", &w, 0);
    lt_tune_set_constant(a, "onlyA", &v, 0);
    lt_tune_value_parse(&v, "1 2 3");
    lt_tune_set_constant(a, "table", &v, 0);
    lt_tune_value_clear(&v);
    d = lt_tune_diff_compare(a, b);
    LT_CHECK(d->n == 5);
    n = lt_tcmp_select(d, LT_TCMP_ALL, "", LT_TCMP_SORT_NAME, 1, idx);
    LT_CHECK(n == 5 && strcmp(d->diffs[idx[0]].name, "alpha") == 0);
    LT_CHECK(strcmp(d->diffs[idx[1]].name, "beta") == 0);
    n = lt_tcmp_select(d, LT_TCMP_SCALAR, NULL, LT_TCMP_SORT_PCT, 0, idx);
    LT_CHECK(n == 2 && strcmp(d->diffs[idx[0]].name, "beta") == 0);
    LT_CHECK(lt_tcmp_severity(&d->diffs[idx[0]]) == 3);
    LT_CHECK(lt_tcmp_severity(&d->diffs[idx[1]]) == 2);
    n = lt_tcmp_select(d, LT_TCMP_ADDED, NULL, LT_TCMP_SORT_NAME, 1, idx);
    LT_CHECK(n == 1 && strcmp(d->diffs[idx[0]].name, "OnlyB") == 0);
    n = lt_tcmp_select(d, LT_TCMP_REMOVED, NULL, LT_TCMP_SORT_NAME, 1, idx);
    LT_CHECK(n == 2);
    n = lt_tcmp_select(d, LT_TCMP_ALL, "ONLY", LT_TCMP_SORT_NAME, 0, idx);
    LT_CHECK(n == 2 && strcmp(d->diffs[idx[0]].name, "OnlyB") == 0);
    lt_tcmp_format(NULL, buf, sizeof(buf));
    LT_CHECK_STR(buf, "(missing)");
    lt_tcmp_format(d->diffs[idx[1]].value_a, buf, sizeof(buf));
    LT_CHECK_STR(buf, "100.00");
    for (n = 0; n < d->n; n++) {
        if (strcmp(d->diffs[n].name, "table") == 0) {
            lt_tcmp_format(d->diffs[n].value_a, buf, sizeof(buf));
            LT_CHECK_STR(buf, "[1.0, 2.0, 3.0]");
        }
    }
    lt_tune_diff_free(d);
    lt_tune_free(a);
    lt_tune_free(b);
}

/* ---------------------------------------------------------------- project INI change + dialogs */
static void replace_once(char *text, const char *from, const char *to)
{
    char *p = strstr(text, from);
    LT_CHECK(p != NULL);
    LT_CHECK(strlen(from) == strlen(to));
    if (p != NULL && strlen(from) == strlen(to)) { memcpy(p, to, strlen(to)); }
}

static void shot(lt_app *app, const char *name)
{
    char path[200];
    lt_snprintf(path, sizeof(path), "build/test_inilib_%s.ppm", name);
    LT_CHECK(lt_app_screenshot(app, path, 1100, 760, 3) == 0);
}

static void close_all(lt_app *app)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    (void)lt_app_screenshot(app, "build/test_inilib_tmp.ppm", 400, 300, 2);
}

LT_TEST(update_project_ini_and_dialogs)
{
    char buf[512], *parent = fresh("proj"), *pdir, *txt, *np;
    size_t len;
    lt_app *app = lt_app_new();
    lt_project p;
    lt_migration_report rep;
    lt_ini *ini2;
    const lt_tune_entry *e;
    lt_tune_value v;
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    LT_CHECK(lt_project_create("inilibtest", buf, app->ini->signature != NULL ? app->ini->signature : "",
                               parent, &p) == 0);
    pdir = lt_strdup(p.path);
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, pdir) == 0);

    /* edit a constant in the project tune */
    lt_tune_value_init(&v);
    v.scalar = 4321;
    lt_tune_set_constant(app->tune, "rpmHardLimit", &v, 0);
    (void)lt_tune_apply_to_pages(app->tune, app->ini);

    /* a changed copy of the INI: type change, scale change, one constant renamed */
    txt = lt_file_read("tests/fixtures/ini/demo.ini", &len);
    LT_CHECK(txt != NULL);
    if (txt == NULL) { lt_free(pdir); lt_free(parent); lt_app_free(app); return; }
    replace_once(txt, "rpmHardLimit = scalar, U16,", "rpmHardLimit = scalar, S16,");
    replace_once(txt, "\"rpm\", 50.0, 0, 0, 3000, 0", "\"rpm\", 25.0, 0, 0, 3000, 0");
    replace_once(txt, "maxAcTps =", "maxAcTpx =");
    np = lt_path_join(parent, "changed.ini");
    LT_CHECK(lt_file_write(np, txt, len) == 0);
    lt_free(txt);

    /* report against the new INI (what the dialog shows before applying) */
    (void)lt_tune_capture_from_pages(app->tune, app->ini);
    LT_CHECK(app->tune->has_manifest == LT_TRUE);
    ini2 = lt_ini_load_file(np, NULL);
    LT_CHECK(ini2 != NULL);
    lt_migration_init(&rep);
    lt_migration_compare(&rep, app->tune->manifest, app->tune->n_manifest, ini2);
    LT_CHECK(lt_migration_has_changes(&rep));
    LT_CHECK(rep.n_type >= 1 && rep.n_scale >= 1);
    LT_CHECK(rep.n_missing_in_ini >= 1 && rep.n_missing_in_tune >= 1);
    lt_migration_clear(&rep);
    lt_ini_free(ini2);

    LT_CHECK(lt_app_dialog_push(app, "migration", "update", "u") != NULL);
    shot(app, "update_pick");
    close_all(app);
    LT_CHECK(lt_app_dialog_push(app, "tunediff", "", "d") != NULL);
    shot(app, "tunediff");
    close_all(app);

    LT_CHECK(lt_feat_migrate_apply(app, np) == 0);
    lt_free(np);
    /* project now uses the new INI; value kept by name, tune stamped with the new definition */
    LT_CHECK(app->has_project && app->ini != NULL);
    LT_CHECK(lt_ini_find_const(app->ini, "maxAcTpx") != NULL);
    LT_CHECK(lt_ini_find_const(app->ini, "maxAcTps") == NULL);
    e = lt_tune_get_constant(app->tune, "rpmHardLimit");
    LT_CHECK(e != NULL && e->value.kind == LT_TV_SCALAR && e->value.scalar > 4320.5 &&
             e->value.scalar < 4321.5);
    LT_CHECK(lt_tune_get_constant(app->tune, "maxAcTpx") != NULL);
    LT_CHECK(app->tune->has_manifest);
    {
        lt_migration_report r2;
        lt_migration_init(&r2);
        lt_migration_compare(&r2, app->tune->manifest, app->tune->n_manifest, app->ini);
        LT_CHECK(!lt_migration_has_changes(&r2));
        lt_migration_clear(&r2);
    }
    LT_CHECK(app->project.cfg.signature != NULL);
    LT_CHECK(lt_app_dialog_push(app, "migration", "", "m") != NULL);
    shot(app, "migration_empty");
    lt_free(pdir);
    lt_free(parent);
    lt_app_free(app);
}

LT_TEST(merge_into_current_tune)
{
    char buf[512];
    lt_app *app = lt_app_new();
    lt_tune *other;
    lt_tune_value v;
    const lt_tune_entry *e;
    lt_tune_diff *df;
    const char *names[1];
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    other = lt_tune_clone(app->tune);
    lt_tune_value_init(&v);
    v.scalar = 3000;
    lt_tune_set_constant(other, "rpmHardLimit", &v, 0);
    LT_CHECK(lt_app_dialog_push(app, "tunediff", "", "d") != NULL);
    shot(app, "tunediff_open");
    df = lt_tune_diff_compare(app->tune, other);
    LT_CHECK(df->n >= 1);
    names[0] = "rpmHardLimit";
    LT_CHECK(lt_tune_merge_selected(app->tune, other, names, 1) == 1);
    lt_tune_diff_free(df);
    e = lt_tune_get_constant(app->tune, "rpmHardLimit");
    LT_CHECK(e != NULL && e->value.scalar > 2999.5 && e->value.scalar < 3000.5);
    lt_tune_free(other);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(sig_normalize);
    LT_RUN(sig_compare);
    LT_RUN(inilib_find_sorted);
    LT_RUN(tunecmp_format_select);
    LT_RUN(update_project_ini_and_dialogs);
    LT_RUN(merge_into_current_tune);
    LT_TEST_MAIN_END
}
