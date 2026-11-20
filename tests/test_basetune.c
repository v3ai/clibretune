/* test_basetune.c - the built-in Base Tune library: embedded catalog, every tune decoded and
 * parsed against a Speeduino INI, catalog filtering, the browser and the new-project / load flows
 * in the headless app.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/tune/basetune.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"

#define SPEEDUINO_INI "tests/fixtures/ini/speeduino_202501.ini"

static lt_ini *g_ini;

LT_TEST(catalog_entries)
{
    size_t i, n = lt_basetune_count();
    LT_CHECK(n == 14);
    LT_CHECK(lt_basetune_at(n) == NULL);
    for (i = 0; i < n; i++) {
        const lt_basetune *b = lt_basetune_at(i);
        size_t len = 0;
        char *txt;
        LT_CHECK(b != NULL);
        if (b == NULL) { continue; }
        LT_CHECK(b->key[0] != '\0' && b->name[0] != '\0' && b->make[0] != '\0');
        LT_CHECK(b->provider[0] != '\0' && b->board[0] != '\0');
        LT_CHECK(b->firmware >= 201001L && b->firmware <= 209912L);
        LT_CHECK(strncmp(b->signature, "speeduino ", 10) == 0);
        LT_CHECK(lt_basetune_find(b->key) == b);
        txt = lt_basetune_text(b, &len);
        LT_CHECK(txt != NULL);
        if (txt == NULL) { continue; }
        LT_CHECK(len == b->size && strlen(txt) == len);
        {   /* byte for byte what upstream ships (assets/basetunes/speeduino/<key>) */
            char path[512];
            size_t flen = 0;
            char *f;
            lt_snprintf(path, sizeof(path), "assets/basetunes/speeduino/%s", b->key);
            f = lt_file_read(path, &flen);
            LT_CHECK(f != NULL);
            if (f != NULL) { LT_CHECK(flen == len && memcmp(f, txt, len) == 0); }
            lt_free(f);
        }
        LT_CHECK(strncmp(txt, "<?xml", 5) == 0);
        LT_CHECK(strstr(txt, "</msq>") != NULL);
        lt_free(txt);
    }
    LT_CHECK(lt_basetune_find("nope.msq") == NULL);
    LT_CHECK(lt_basetune_find(NULL) == NULL);
    LT_CHECK_STR(lt_basetune_at(0)->make, "Generic");
    LT_CHECK(strlen(lt_basetune_commit) == 40);
    LT_CHECK(strstr(lt_basetune_source, "speeduino/Tunes") != NULL);
}

/* every embedded file parses with the tune loader and maps onto the Speeduino INI */
LT_TEST(lzb_decoder)
{
    /* "abcabcabcab": 3 literals, then a 8-byte match at distance 3 (overlapping copy) */
    static const unsigned char ok[] = { 0x34, 'a', 'b', 'c', 3, 0, 0, 0x00 };
    /* long literal run (15 + 5 = 20) and a long match (4 + 15 + 255 + 1 = 275) */
    unsigned char big[64], out[400], junk[64];
    size_t i, n = 0;
    unsigned seed = 12345U;
    LT_CHECK(lt_lzb_decode(ok, sizeof(ok), out, 11) == 0);
    LT_CHECK(memcmp(out, "abcabcabcab", 11) == 0);
    big[n++] = 0xFF; big[n++] = 5;
    for (i = 0; i < 20; i++) { big[n++] = (unsigned char)('A' + i); }
    big[n++] = 20; big[n++] = 0; big[n++] = 0; big[n++] = 255; big[n++] = 1;
    big[n++] = 0x00;
    LT_CHECK(lt_lzb_decode(big, n, out, 295) == 0);
    LT_CHECK(out[294] == (unsigned char)('A' + 274 % 20));
    /* malformed: distance 0, distance before the start, truncated, overlong, short output */
    { static const unsigned char d0[] = { 0x10, 'x', 0, 0, 0 };
      static const unsigned char far_[] = { 0x10, 'x', 2, 0, 0 };
      static const unsigned char trunc[] = { 0x30, 'a' };
      LT_CHECK(lt_lzb_decode(d0, sizeof(d0), out, 10) == -1);
      LT_CHECK(lt_lzb_decode(far_, sizeof(far_), out, 10) == -1);
      LT_CHECK(lt_lzb_decode(trunc, sizeof(trunc), out, 3) == -1);
      LT_CHECK(lt_lzb_decode(ok, sizeof(ok), out, 10) == -1);
      LT_CHECK(lt_lzb_decode(ok, sizeof(ok), out, 12) == -1);
      LT_CHECK(lt_lzb_decode(ok, 0, out, 11) == -1); }
    /* random garbage never crashes (ASan/UBSan builds check the bounds) */
    for (i = 0; i < 20000; i++) {
        size_t k, len = (size_t)(i % sizeof(junk)) + 1;
        for (k = 0; k < len; k++) { seed = seed * 1103515245U + 12345U; junk[k] = (unsigned char)(seed >> 16); }
        (void)lt_lzb_decode(junk, len, out, (size_t)(seed % sizeof(out)));
    }
}

LT_TEST(every_tune_parses_and_maps)
{
    size_t i, n = lt_basetune_count();
    LT_CHECK(g_ini != NULL);
    if (g_ini == NULL) { return; }
    for (i = 0; i < n; i++) {
        const lt_basetune *b = lt_basetune_at(i);
        lt_tune *t = lt_basetune_load(b);
        lt_tune *d;
        lt_migration_report rep;
        size_t nc;
        int applied, pg;
        LT_CHECK(t != NULL);
        if (t == NULL) { fprintf(stderr, "  %s: %s\n", b->key, lt_err_get()); continue; }
        LT_CHECK_STR(t->signature, b->signature);
        nc = lt_tune_n_constants(t);
        LT_CHECK(nc > 300);
        LT_CHECK(lt_tune_get_constant(t, "veTable") != NULL);
        LT_CHECK(lt_tune_get_constant(t, "reqFuel") != NULL);
        /* what the browser shows before applying */
        lt_migration_init(&rep);
        lt_migration_compare_tune(&rep, t, g_ini);
        if (strcmp(b->signature, "speeduino 202501") == 0) {
            /* same firmware: every table and number fits the definition. What is left are quirks
             * of the files: one constant behind an #if of the INI, and three option labels the
             * INI does not spell that way (blank labels, a "." instead of a "," in the upstream
             * secondarySerialProtocol option list) */
            size_t k;
            LT_CHECK(rep.n_missing_in_ini <= 1);
            LT_CHECK(rep.n_type <= 3);
            for (k = 0; k < rep.n_type; k++) {
                LT_CHECK_STR(rep.type_changed[k].new_type, "Bits");
            }
        }
        /* older firmware: most constants still map, the rest is listed in the report */
        LT_CHECK(rep.n_missing_in_ini + rep.n_type < nc / 4);
        lt_migration_clear(&rep);
        /* load the way the app does: INI defaults first, then the named values */
        d = lt_tune_from_defaults(g_ini);
        for (pg = 0; pg < 256; pg++) {
            if (d->pages[pg] != NULL && t->pages[pg] == NULL) {
                lt_tune_set_page(t, pg, d->pages[pg], d->page_len[pg]);
            }
        }
        lt_tune_free(d);
        applied = lt_tune_apply_to_pages(t, g_ini);
        LT_CHECK(applied > 0 && (size_t)applied * 4 > nc * 3);
        LT_CHECK(lt_tune_capture_from_pages(t, g_ini) > 0);
        {
            const lt_tune_entry *e = lt_tune_get_constant(t, "reqFuel");
            LT_CHECK(e != NULL && e->value.kind == LT_TV_SCALAR && e->value.scalar > 0.5 &&
                     e->value.scalar < 50.0);
        }
        lt_tune_free(t);
    }
}

LT_TEST(same_firmware_values_survive)
{
    const lt_basetune *b = lt_basetune_find("Mazda/NA6 PNP base tune.msq");
    lt_tune *t, *orig;
    const lt_tune_entry *a, *c;
    LT_CHECK(b != NULL && g_ini != NULL);
    if (b == NULL || g_ini == NULL) { return; }
    orig = lt_basetune_load(b);
    t = lt_basetune_load(b);
    LT_CHECK(orig != NULL && t != NULL);
    if (orig == NULL || t == NULL) { lt_tune_free(orig); lt_tune_free(t); return; }
    LT_CHECK(lt_tune_apply_to_pages(t, g_ini) > 0);
    LT_CHECK(lt_tune_capture_from_pages(t, g_ini) > 0);
    a = lt_tune_get_constant(orig, "veTable");
    c = lt_tune_get_constant(t, "veTable");
    LT_CHECK(a != NULL && c != NULL && a->value.kind == LT_TV_ARRAY && c->value.kind == LT_TV_ARRAY);
    if (a != NULL && c != NULL) { LT_CHECK(lt_tune_value_equal(&a->value, &c->value)); }
    a = lt_tune_get_constant(orig, "nCylinders");
    c = lt_tune_get_constant(t, "nCylinders");
    LT_CHECK(a != NULL && c != NULL);
    if (a != NULL && c != NULL) { LT_CHECK(lt_tune_value_equal(&a->value, &c->value)); }
    lt_tune_free(orig);
    lt_tune_free(t);
}

LT_TEST(filtering)
{
    size_t idx[32], n, i;
    const char *makes[16], *prov[16];
    n = lt_basetune_filter(NULL, NULL, idx, 32);
    LT_CHECK(n == lt_basetune_count());
    n = lt_basetune_filter("", "", idx, 32);
    LT_CHECK(n == lt_basetune_count());
    n = lt_basetune_filter("Mazda", NULL, idx, 32);
    LT_CHECK(n == 5);
    for (i = 0; i < n && i < 32; i++) { LT_CHECK_STR(lt_basetune_at(idx[i])->make, "Mazda"); }
    LT_CHECK(lt_basetune_filter("bmw", "", idx, 32) == 6);
    LT_CHECK(lt_basetune_filter(NULL, "speedyefi", idx, 32) == 2);
    LT_CHECK(lt_basetune_filter(NULL, "  MIATA   pnp ", idx, 32) == 5);
    LT_CHECK(lt_basetune_filter("BMW", "miata", idx, 32) == 0);
    LT_CHECK(lt_basetune_filter(NULL, "4g93", idx, 32) == 1);
    LT_CHECK_STR(lt_basetune_at(idx[0])->make, "Mitsubishi");
    LT_CHECK(lt_basetune_filter(NULL, "202305", idx, 32) == 6);       /* by signature */
    LT_CHECK(lt_basetune_filter(NULL, "core4", idx, 32) == 2);        /* by board */
    LT_CHECK(lt_basetune_filter(NULL, "no such tune", idx, 32) == 0);
    LT_CHECK(lt_basetune_filter("Mazda", NULL, idx, 2) == 5);         /* total beyond cap */
    LT_CHECK(lt_basetune_filter(NULL, NULL, NULL, 0) == lt_basetune_count());
    n = lt_basetune_makes(makes, 16);
    LT_CHECK(n == 5);
    LT_CHECK_STR(makes[0], "Generic");
    LT_CHECK_STR(makes[1], "Audi");
    n = lt_basetune_providers(prov, 16);
    LT_CHECK(n >= 4);
    LT_CHECK(lt_basetune_match(lt_basetune_at(0), "generic", "base") == LT_TRUE);
    LT_CHECK(lt_basetune_match(NULL, NULL, NULL) == LT_FALSE);
}

LT_TEST(family_and_credit)
{
    char b[400];
    LT_CHECK(lt_basetune_family("speeduino 202501") == LT_BASETUNE_FAM_SPEEDUINO);
    LT_CHECK(lt_basetune_family("rusEFI master.2025.06.26.uaefi.123") == LT_BASETUNE_FAM_RUSEFI);
    LT_CHECK(lt_basetune_family("FOME 2024.10") == LT_BASETUNE_FAM_RUSEFI);
    LT_CHECK(lt_basetune_family("MS3 Format 0543.06") == LT_BASETUNE_FAM_UNKNOWN);
    LT_CHECK(lt_basetune_family("") == LT_BASETUNE_FAM_UNKNOWN);
    LT_CHECK(lt_basetune_family(NULL) == LT_BASETUNE_FAM_UNKNOWN);
    lt_basetune_credit(lt_basetune_find("BMW/M52_PnP.msq"), b, sizeof(b));
    LT_CHECK(strstr(b, "Pazi88") != NULL && strstr(b, "CC BY-NC 4.0") != NULL);
    lt_basetune_credit(NULL, b, sizeof(b));
    LT_CHECK(strstr(b, "Speeduino project and contributors") != NULL);
    lt_basetune_origin(lt_basetune_at(0), b, sizeof(b));
    LT_CHECK(strstr(b, "CC BY-NC 4.0") != NULL && strstr(b, lt_basetune_at(0)->name) != NULL);
}

/* the name-based report spots what does not map */
LT_TEST(report_lists_misfits)
{
    lt_tune *t = lt_basetune_load(lt_basetune_at(0));
    lt_tune_value v;
    lt_migration_report rep;
    size_t i;
    int found_missing = 0, found_opt = 0, found_new = 0;
    LT_CHECK(t != NULL && g_ini != NULL);
    if (t == NULL || g_ini == NULL) { lt_tune_free(t); return; }
    lt_tune_value_init(&v);
    v.scalar = 3;
    lt_tune_set_constant(t, "notAConstantOfThisIni", &v, 1);
    lt_tune_value_parse(&v, "\"No such option\"");
    lt_tune_set_constant(t, "algorithm", &v, 1);
    lt_tune_value_clear(&v);
    lt_tune_value_init(&v);
    lt_migration_init(&rep);
    lt_migration_compare_tune(&rep, t, g_ini);
    for (i = 0; i < rep.n_missing_in_ini; i++) {
        if (strcmp(rep.missing_in_ini[i], "notAConstantOfThisIni") == 0) { found_missing = 1; }
    }
    for (i = 0; i < rep.n_type; i++) {
        if (strcmp(rep.type_changed[i].name, "algorithm") == 0) { found_opt = 1; }
    }
    LT_CHECK(found_missing && found_opt);
    LT_CHECK(strcmp(rep.severity, "high") == 0 && rep.requires_user_review);
    lt_migration_clear(&rep);
    lt_tune_free(t);
    /* an empty tune: everything of the INI is new */
    t = lt_tune_new("speeduino 202501");
    lt_migration_compare_tune(&rep, t, g_ini);
    for (i = 0; i < rep.n_missing_in_tune; i++) {
        if (strcmp(rep.missing_in_tune[i], "reqFuel") == 0) { found_new = 1; }
    }
    LT_CHECK(found_new && strcmp(rep.severity, "low") == 0);
    lt_migration_clear(&rep);
    lt_tune_free(t);
}

/* ---------------------------------------------------------------- the browser in the app */
static void frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static void press(lt_app *app, int key)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_KEY_DOWN; ev.key = key;
    lt_ui_input(app->ui, &ev);
    frame(app);
    ev.type = LT_EV_KEY_UP;
    lt_ui_input(app->ui, &ev);
    frame(app);
    frame(app);
}

static const char *last_dialog(const lt_app *app)
{
    return app->n_dialogs > 0 ? app->dialogs[app->n_dialogs - 1]->kind : "";
}

static lt_bool same_const(const lt_tune *a, const lt_tune *b, const char *name)
{
    const lt_tune_entry *x = lt_tune_get_constant(a, name), *y = lt_tune_get_constant(b, name);
    return x != NULL && y != NULL && lt_tune_value_equal(&x->value, &y->value);
}

LT_TEST(app_new_project_and_load)
{
    lt_app *app = lt_app_new();
    lt_ini_repo r;
    char *id = NULL, *p, *txt;
    const lt_basetune *gen = lt_basetune_at(0), *bmw = lt_basetune_find("BMW/M52_PnP.msq");
    lt_tune *ref;
    size_t len = 0;
    /* nothing to load into yet */
    LT_CHECK(lt_feat_basetune_load(app, gen) == -1);
    /* a Speeduino INI in the INI library: the browser matches it */
    LT_CHECK(lt_ini_repo_open(&r, NULL) == 0);
    LT_CHECK(lt_ini_repo_import(&r, SPEEDUINO_INI, &id) == 0);
    lt_free(id);
    lt_ini_repo_close(&r);
    LT_CHECK(lt_app_dialog_push(app, "newproject", "", "New Project") != NULL);
    (void)lt_app_screenshot(app, "build/test_basetune_newproj.ppm", 1000, 760, 2);
    app->dialogs[0]->open = 0;
    frame(app);
    LT_CHECK(app->n_dialogs == 0);
    lt_feat_basetune_open(app, "new");
    LT_CHECK_STR(last_dialog(app), "basetunes");
    (void)lt_app_screenshot(app, "build/test_basetune_browser.ppm", 1280, 900, 3);
    /* Enter = Create Project: same firmware, but a few values of the Generic tune do not map, so the
     * migration report comes first; Enter there = Continue with Tune */
    press(app, LT_KEY_ENTER);
    LT_CHECK_STR(last_dialog(app), "migration");
    (void)lt_app_screenshot(app, "build/test_basetune_report.ppm", 1280, 900, 3);
    press(app, LT_KEY_ENTER);
    LT_CHECK(app->n_dialogs == 0);
    (void)lt_app_service(app);
    LT_CHECK(app->has_project && app->ini != NULL && app->tune != NULL);
    if (!app->has_project || app->tune == NULL) { lt_app_free(app); return; }
    LT_CHECK_STR(app->ini->signature, "speeduino 202501");
    LT_CHECK_STR(app->project.cfg.name, gen->name);
    LT_CHECK(app->tune->description != NULL && strstr(app->tune->description, "CC BY-NC 4.0") != NULL);
    ref = lt_basetune_load(gen);
    LT_CHECK(ref != NULL);
    if (ref != NULL) {
        LT_CHECK(same_const(app->tune, ref, "veTable"));
        LT_CHECK(same_const(app->tune, ref, "reqFuel"));
        LT_CHECK(same_const(app->tune, ref, "TrigPattern"));
        lt_tune_free(ref);
    }
    /* saved with the project, origin recorded in the file */
    p = lt_path_join(app->project.path, "CurrentTune.msq");
    txt = lt_file_read(p, &len);
    LT_CHECK(txt != NULL && strstr(txt, "Speeduino base tune") != NULL && strstr(txt, "veTable") != NULL);
    lt_free(txt);
    lt_free(p);
    LT_CHECK(app->modified == 0);

    /* Load Base Tune: older firmware (202305) into the 202501 project, through the report */
    lt_feat_basetune_open(app, "load");
    frame(app);
    LT_CHECK_STR(last_dialog(app), "basetunes");
    app->dialogs[app->n_dialogs - 1]->open = 0;     /* the browser selects the first tune: use the API */
    frame(app);
    LT_CHECK(bmw != NULL);
    if (bmw == NULL) { lt_app_free(app); return; }
    LT_CHECK(lt_feat_basetune_load(app, bmw) == 0);
    ref = lt_basetune_load(bmw);
    LT_CHECK(ref != NULL);
    if (ref != NULL) {
        LT_CHECK(same_const(app->tune, ref, "nCylinders"));
        LT_CHECK(same_const(app->tune, ref, "veTable"));
        LT_CHECK(same_const(app->tune, ref, "injOpen"));
        lt_tune_free(ref);
    }
    LT_CHECK(app->modified != 0);
    LT_CHECK(lt_app_save_tune(app) == 0);
    LT_CHECK(app->modified == 0);
    LT_CHECK_STR(app->tune->signature, "speeduino 202501");

    /* the browser in load mode: the report, then Cancel leaves the tune alone */
    lt_feat_basetune_open(app, "");                  /* Speeduino INI open: load mode */
    (void)lt_app_screenshot(app, "build/test_basetune_load.ppm", 1280, 900, 3);
    press(app, LT_KEY_ENTER);
    LT_CHECK_STR(last_dialog(app), "migration");
    press(app, LT_KEY_ESCAPE);
    LT_CHECK(app->n_dialogs == 0);
    (void)lt_app_service(app);
    LT_CHECK(app->modified == 0);
    /* the same with Continue: the Generic tune replaces the BMW one */
    lt_feat_basetune_open(app, "load");
    (void)lt_app_screenshot(app, "build/test_basetune_load2.ppm", 1280, 900, 3);
    LT_CHECK_STR(last_dialog(app), "basetunes");
    press(app, LT_KEY_ENTER);
    LT_CHECK_STR(last_dialog(app), "migration");
    (void)lt_app_screenshot(app, "build/test_basetune_report2.ppm", 1280, 900, 3);
    press(app, LT_KEY_ENTER);
    LT_CHECK(app->n_dialogs == 0);
    (void)lt_app_service(app);
    LT_CHECK(app->modified != 0);
    ref = lt_basetune_load(gen);
    if (ref != NULL) {
        LT_CHECK(same_const(app->tune, ref, "nCylinders"));
        lt_tune_free(ref);
    }
    LT_CHECK(lt_app_dialog_push(app, "about", "", "About") != NULL);
    (void)lt_app_screenshot(app, "build/test_basetune_about.ppm", 900, 700, 2);
    lt_app_free(app);
}

int main(void)
{
    g_ini = lt_ini_load_file(SPEEDUINO_INI, NULL);
    if (g_ini == NULL) { fprintf(stderr, "cannot load %s: %s\n", SPEEDUINO_INI, lt_err_get()); }
    LT_RUN(lzb_decoder);
    LT_RUN(catalog_entries);
    LT_RUN(every_tune_parses_and_maps);
    LT_RUN(same_firmware_values_survive);
    LT_RUN(filtering);
    LT_RUN(family_and_credit);
    LT_RUN(report_lists_misfits);
    LT_RUN(app_new_project_and_load);
    lt_ini_free(g_ini);
    LT_TEST_MAIN_END
}
