/* test_dashmgmt.c - dashboard library (dash_lib), editing logic (dash_edit: history, clone, clip,
 * z-order, align, assets, INI sync, indicator grid), the NULL-safe full-screen backend hook and the
 * dashboard tab driven headless (selection, undo, copy/paste, kiosk).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/lt.h"
#include "../src/plat/plat.h"
#include "../src/dash/dash.h"
#include "../src/dash/dash_lib.h"
#include "../src/dash/dash_edit.h"
#include "../src/dash/dash_priv.h"
#include "../src/app/dash_tab.h"

#define BASIC "tests/fixtures/dash/basic.dash"
#define DEMO "tests/fixtures/ini/demo.ini"
#define TMP "build/test_dashmgmt"

static void fresh_dir(void)
{
    char **names = NULL;
    int n, i;
    (void)lt_plat_mkdirs(TMP);
    n = lt_plat_list_dir(TMP, &names);
    for (i = 0; i < n; i++) {
        char *p = lt_path_join(TMP, names[i]);
        (void)lt_plat_remove(p);
        lt_free(p);
    }
    if (names != NULL) { lt_plat_free_list(names, n); }
}

static lt_dash_file *two_gauges(void)
{
    lt_dash_file *d = lt_dash_new();
    lt_dash_gauge *a = lt_dash_add_gauge(d), *b = lt_dash_add_gauge(d);
    dash_set_str(&a->id, "g1"); dash_set_str(&a->output_channel, "rpm"); dash_set_str(&a->title, "RPM");
    dash_set_str(&b->id, "g2"); dash_set_str(&b->output_channel, "map"); dash_set_str(&b->title, "MAP");
    a->relative_x = 0.1; a->relative_y = 0.1; a->relative_width = 0.2; a->relative_height = 0.2;
    b->relative_x = 0.5; b->relative_y = 0.4; b->relative_width = 0.3; b->relative_height = 0.1;
    return d;
}

LT_TEST(lib_names)
{
    char out[96];
    LT_CHECK(lt_dashlib_name_ok("My Dash 2"));
    LT_CHECK(!lt_dashlib_name_ok(""));
    LT_CHECK(!lt_dashlib_name_ok("a/b"));
    LT_CHECK(!lt_dashlib_name_ok(".hidden"));
    LT_CHECK(!lt_dashlib_name_ok("trail "));
    LT_CHECK(!lt_dashlib_name_ok("what?"));
    lt_dashlib_safe_name("  a:b*c.  ", out, sizeof out);
    LT_CHECK(lt_dashlib_name_ok(out));
    LT_CHECK_STR(out, "a_b_c");
    lt_dashlib_safe_name("...", out, sizeof out);
    LT_CHECK_STR(out, "dashboard");
}

LT_TEST(lib_crud)
{
    lt_dash_file *d = two_gauges();
    char *p1 = NULL, *p2 = NULL, *p3 = NULL;
    lt_dashlib lib;
    char nm[96];
    fresh_dir();
    LT_CHECK(lt_dashlib_create(TMP, "Race", d, &p1) == 0);
    LT_CHECK(lt_file_exists(p1));
    LT_CHECK(lt_dashlib_create(TMP, "Race", d, NULL) == LT_EEXIST);
    LT_CHECK(lt_dashlib_create(TMP, "bad/name", d, NULL) == LT_EINVAL);
    LT_CHECK(lt_dashlib_duplicate(p1, "Street", &p2) == 0);
    LT_CHECK(lt_dashlib_duplicate(p1, "Street", NULL) == LT_EEXIST);
    LT_CHECK(lt_dashlib_rename(p2, "Track", &p3) == 0);
    LT_CHECK(!lt_file_exists(p2));
    LT_CHECK(lt_file_exists(p3));
    LT_CHECK(lt_dashlib_rename(p3, "Race", NULL) == LT_EEXIST);
    lt_dashlib_unique(TMP, "Race", nm, sizeof nm);
    LT_CHECK_STR(nm, "Race 2");
    lt_dashlib_init(&lib);
    LT_CHECK(lt_dashlib_scan(&lib, TMP, "User") == 2);
    lt_dashlib_sort(&lib);
    LT_CHECK_STR(lib.e[0].name, "Race");
    LT_CHECK_STR(lib.e[1].name, "Track");
    LT_CHECK(lib.e[0].size > 0);
    lt_dashlib_free(&lib);
    LT_CHECK(lt_dashlib_delete(p3) == 0);
    LT_CHECK(lt_dashlib_delete(p3) == LT_ENOENT);
    lt_free(p1); lt_free(p2); lt_free(p3);
    lt_dash_free(d);
}

LT_TEST(lib_sort_categories)
{
    lt_dashlib lib;
    int i;
    static const char *const cats[4] = { "Zeta", "Reference", "User", "Alpha" };
    lt_dashlib_init(&lib);
    lib.e = (lt_dashlib_entry *)lt_calloc(4, sizeof(lt_dashlib_entry));
    lib.n = lib.cap = 4;
    for (i = 0; i < 4; i++) { lib.e[i].name = lt_strdup("x"); lib.e[i].path = lt_strdup("p"); lib.e[i].category = lt_strdup(cats[i]); }
    lt_dashlib_sort(&lib);
    LT_CHECK_STR(lib.e[0].category, "User");
    LT_CHECK_STR(lib.e[1].category, "Reference");
    LT_CHECK_STR(lib.e[2].category, "Alpha");
    LT_CHECK_STR(lib.e[3].category, "Zeta");
    lt_dashlib_free(&lib);
}

LT_TEST(lib_export_import_compat)
{
    lt_ini *ini = lt_ini_load_file(DEMO, NULL);
    lt_dash_compat rep;
    char *dest = NULL, *dest2 = NULL;
    lt_dashlib lib;
    lt_dash_file *d;
    LT_CHECK(ini != NULL);
    fresh_dir();
    /* the fixture uses generic channel names: not all resolve against the demo INI */
    LT_CHECK(lt_dashlib_preview(BASIC, ini, TMP, &rep) == 0);
    LT_CHECK(rep.parsed && rep.gauges > 0 && rep.n_channels > 0);
    LT_CHECK(!rep.duplicate);
    LT_CHECK(rep.percent >= 0 && rep.percent <= 100);
    LT_CHECK(rep.n_unresolved > 0 && rep.unresolved[0] != NULL);
    lt_dash_compat_free(&rep);
    /* after resolving the names against the INI everything is found */
    d = lt_dash_load(BASIC);
    LT_CHECK(d != NULL);
    (void)lt_dash_resolve_channels(d, ini);
    lt_dash_compat_check(d, ini, &rep);
    LT_CHECK(rep.percent >= 50);
    lt_dash_compat_free(&rep);
    lt_dash_compat_check(d, NULL, &rep);       /* no INI: nothing can be missing */
    LT_CHECK(rep.percent == 100 && rep.n_unresolved == 0);
    lt_dash_compat_free(&rep);
    lt_dash_free(d);
    /* import, conflict, import as copy, replace */
    LT_CHECK(lt_dashlib_import(BASIC, TMP, 0, &dest) == 0);
    LT_CHECK(strstr(dest, "basic.dash") != NULL);
    LT_CHECK(lt_dashlib_preview(BASIC, ini, TMP, &rep) == 0);
    LT_CHECK(rep.duplicate);
    lt_dash_compat_free(&rep);
    LT_CHECK(lt_dashlib_import(BASIC, TMP, 0, &dest2) == 0);
    LT_CHECK(strstr(dest2, "basic 2.dash") != NULL);
    lt_free(dest2); dest2 = NULL;
    LT_CHECK(lt_dashlib_import(BASIC, TMP, 1, &dest2) == 0);
    LT_CHECK_STR(dest, dest2);
    lt_dashlib_init(&lib);
    LT_CHECK(lt_dashlib_scan(&lib, TMP, "User") == 2);
    lt_dashlib_free(&lib);
    /* export is a plain copy */
    LT_CHECK(lt_dashlib_export(dest, TMP "/exported.dash") == 0);
    LT_CHECK(lt_file_exists(TMP "/exported.dash"));
    /* garbage cannot be imported silently: the preview reports it */
    LT_CHECK(lt_file_write(TMP "/bad.dash", "<nope", 5) == 0);
    LT_CHECK(lt_dashlib_preview(TMP "/bad.dash", ini, TMP, &rep) == LT_EDATA);
    LT_CHECK(!rep.parsed && rep.err[0] != '\0');
    lt_free(dest); lt_free(dest2);
    lt_ini_free(ini);
}

LT_TEST(ts_scan)
{
    lt_dashlib lib;
    fresh_dir();
    LT_CHECK(lt_plat_mkdirs(TMP "/ts/Dash") == 0);
    LT_CHECK(lt_plat_copy_file(BASIC, TMP "/ts/Dash/a.dash") == 0);
    LT_CHECK(lt_plat_copy_file(BASIC, TMP "/ts/b.dash") == 0);
    lt_dashlib_init(&lib);
    LT_CHECK(lt_dashlib_scan_ts(&lib, TMP "/ts") == 2);
    LT_CHECK_STR(lib.e[0].category, "TunerStudio");
    lt_dashlib_free(&lib);
    (void)lt_plat_remove(TMP "/ts/Dash/a.dash");
    (void)lt_plat_remove(TMP "/ts/b.dash");
    (void)lt_plat_remove(TMP "/ts/Dash");
    (void)lt_plat_remove(TMP "/ts");
}

LT_TEST(history)
{
    lt_dash_hist h;
    lt_dash_file *d = two_gauges(), *r;
    lt_dash_hist_init(&h);
    LT_CHECK(lt_dash_hist_push(&h, d) == 1);          /* baseline */
    LT_CHECK(lt_dash_hist_push(&h, d) == 0);          /* unchanged */
    LT_CHECK(!lt_dash_hist_can_undo(&h));
    d->cluster.comps[0].gauge->relative_x = 0.3;
    LT_CHECK(lt_dash_hist_push(&h, d) == 1);
    (void)lt_dash_add_gauge(d);
    LT_CHECK(lt_dash_hist_push(&h, d) == 1);
    LT_CHECK(lt_dash_hist_can_undo(&h) && !lt_dash_hist_can_redo(&h));
    r = lt_dash_hist_undo(&h);
    LT_CHECK(r != NULL && r->cluster.n_comps == 2);
    LT_CHECK_NEAR(r->cluster.comps[0].gauge->relative_x, 0.3, 1e-9);
    lt_dash_free(r);
    r = lt_dash_hist_undo(&h);
    LT_CHECK(r != NULL);
    LT_CHECK_NEAR(r->cluster.comps[0].gauge->relative_x, 0.1, 1e-9);
    lt_dash_free(r);
    LT_CHECK(lt_dash_hist_undo(&h) == NULL);
    LT_CHECK(lt_dash_hist_can_redo(&h));
    r = lt_dash_hist_redo(&h);
    LT_CHECK(r != NULL && r->cluster.n_comps == 2);
    lt_dash_free(r);
    /* a new change drops the redo tail */
    d->cluster.comps[1].gauge->relative_y = 0.9;
    LT_CHECK(lt_dash_hist_push(&h, d) == 1);
    LT_CHECK(!lt_dash_hist_can_redo(&h));
    lt_dash_hist_free(&h);
    lt_dash_free(d);
}

LT_TEST(history_cap)
{
    lt_dash_hist h;
    lt_dash_file *d = two_gauges();
    int i;
    lt_dash_hist_init(&h);
    for (i = 0; i < LT_DASH_HIST_MAX + 20; i++) {
        d->cluster.comps[0].gauge->relative_x = (double)(i % 50) / 100.0 + (double)i / 1000.0;
        (void)lt_dash_hist_push(&h, d);
    }
    LT_CHECK(h.n == LT_DASH_HIST_MAX);
    LT_CHECK(h.pos == LT_DASH_HIST_MAX - 1);
    lt_dash_hist_free(&h);
    lt_dash_free(d);
}

LT_TEST(clone_clip_zorder)
{
    lt_dash_file *d = two_gauges(), *e = lt_dash_new();
    size_t idx[2], first = 0, n;
    char *clip;
    int ni;
    lt_dash_indicator *in = lt_dash_add_indicator(d);
    dash_set_str(&in->id, "i1"); dash_set_str(&in->on_text, "ON");
    in->relative_x = 0.0; in->relative_y = 0.9; in->relative_width = 0.1; in->relative_height = 0.05;
    ni = lt_dash_comp_clone(d, 0);
    LT_CHECK(ni == 3 && d->cluster.n_comps == 4);
    LT_CHECK(d->cluster.comps[3].gauge != NULL);
    LT_CHECK(d->cluster.comps[3].gauge != d->cluster.comps[0].gauge);
    LT_CHECK(strcmp(d->cluster.comps[3].gauge->id, "g1") != 0);          /* fresh id */
    LT_CHECK_STR(d->cluster.comps[3].gauge->title, "RPM");               /* deep copy */
    LT_CHECK_NEAR(d->cluster.comps[3].gauge->relative_x, 0.12, 1e-9);
    dash_set_str(&d->cluster.comps[0].gauge->title, "changed");
    LT_CHECK_STR(d->cluster.comps[3].gauge->title, "RPM");               /* independent */
    /* clipboard of an indicator and a gauge, pasted into another dashboard */
    idx[0] = 1; idx[1] = 2;
    clip = lt_dash_clip_copy(d, idx, 2);
    LT_CHECK(clip != NULL && strstr(clip, "MAP") != NULL);
    n = lt_dash_clip_paste(e, clip, 0.05, 0.0, &first);
    LT_CHECK(n == 2 && first == 0 && e->cluster.n_comps == 2);
    LT_CHECK_NEAR(e->cluster.comps[0].gauge->relative_x, 0.55, 1e-9);
    LT_CHECK(e->cluster.comps[1].indicator != NULL);
    LT_CHECK_STR(e->cluster.comps[1].indicator->on_text, "ON");
    n = lt_dash_clip_paste(e, "not xml", 0.0, 0.0, &first);
    LT_CHECK(n == 0);
    lt_free(clip);
    /* paste keeps a copy inside the cluster */
    n = lt_dash_clip_paste(e, "<x", 0.0, 0.0, &first);
    LT_CHECK(n == 0);
    /* z-order */
    LT_CHECK(lt_dash_comp_move_z(d, 0, 3) == 3);
    LT_CHECK_STR(d->cluster.comps[3].gauge->title, "changed");
    LT_CHECK_STR(d->cluster.comps[0].gauge->id, "g2");
    LT_CHECK(lt_dash_comp_move_z(d, 3, 0) == 0);
    LT_CHECK_STR(d->cluster.comps[0].gauge->title, "changed");
    LT_CHECK(lt_dash_comp_move_z(d, 1, 99) == 3);
    lt_dash_free(d); lt_dash_free(e);
}

LT_TEST(hide_keeps_condition)
{
    lt_dash_file *d = two_gauges();
    lt_dash_comp *c = &d->cluster.comps[0];
    dash_set_str(&c->gauge->enabled_condition, "rpm > 0");
    LT_CHECK(!lt_dash_comp_hidden(c));
    lt_dash_comp_set_hidden(c, LT_TRUE);
    LT_CHECK(lt_dash_comp_hidden(c));
    LT_CHECK_STR(c->gauge->enabled_condition, "false");
    lt_dash_comp_set_hidden(c, LT_TRUE);                 /* idempotent */
    LT_CHECK_STR(c->gauge->enabled_condition, "false");
    lt_dash_comp_set_hidden(c, LT_FALSE);
    LT_CHECK_STR(c->gauge->enabled_condition, "rpm > 0");   /* the real condition came back */
    lt_dash_comp_set_hidden(&d->cluster.comps[1], LT_TRUE);
    lt_dash_comp_set_hidden(&d->cluster.comps[1], LT_FALSE);
    LT_CHECK(d->cluster.comps[1].gauge->enabled_condition == NULL);
    lt_dash_free(d);
}

LT_TEST(alignment)
{
    lt_dash_file *d = two_gauges();
    lt_dash_gauge *c3 = lt_dash_add_gauge(d);
    size_t idx[3];
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    c3->relative_x = 0.9; c3->relative_y = 0.7; c3->relative_width = 0.05; c3->relative_height = 0.2;
    lt_dash_align(d, idx, 3, LT_ALIGN_LEFT);
    LT_CHECK_NEAR(d->cluster.comps[1].gauge->relative_x, 0.1, 1e-9);
    LT_CHECK_NEAR(d->cluster.comps[2].gauge->relative_x, 0.1, 1e-9);
    lt_dash_align(d, idx, 3, LT_ALIGN_TOP);
    LT_CHECK_NEAR(d->cluster.comps[2].gauge->relative_y, 0.1, 1e-9);
    lt_dash_align(d, idx, 3, LT_ALIGN_SAME_W);
    LT_CHECK_NEAR(d->cluster.comps[1].gauge->relative_width, 0.2, 1e-9);
    LT_CHECK_NEAR(d->cluster.comps[2].gauge->relative_width, 0.2, 1e-9);
    lt_dash_align(d, idx, 3, LT_ALIGN_RIGHT);
    LT_CHECK_NEAR(d->cluster.comps[1].gauge->relative_x + d->cluster.comps[1].gauge->relative_width,
                  d->cluster.comps[0].gauge->relative_x + d->cluster.comps[0].gauge->relative_width, 1e-9);
    /* distribute: equal gaps between three boxes spread over their bounding span */
    d->cluster.comps[0].gauge->relative_x = 0.0; d->cluster.comps[0].gauge->relative_width = 0.1;
    d->cluster.comps[1].gauge->relative_x = 0.2; d->cluster.comps[1].gauge->relative_width = 0.1;
    d->cluster.comps[2].gauge->relative_x = 0.9; d->cluster.comps[2].gauge->relative_width = 0.1;
    lt_dash_align(d, idx, 3, LT_ALIGN_DIST_H);
    LT_CHECK_NEAR(d->cluster.comps[1].gauge->relative_x, 0.45, 1e-9);
    LT_CHECK_NEAR(d->cluster.comps[0].gauge->relative_x, 0.0, 1e-9);
    LT_CHECK_NEAR(d->cluster.comps[2].gauge->relative_x, 0.9, 1e-9);
    /* a single component is left alone; the result stays inside the cluster */
    lt_dash_align(d, idx, 1, LT_ALIGN_RIGHT);
    LT_CHECK_NEAR(d->cluster.comps[0].gauge->relative_x, 0.0, 1e-9);
    LT_CHECK_NEAR(lt_dash_snap(0.123, 40), 0.125, 1e-9);
    LT_CHECK_NEAR(lt_dash_snap(0.123, 0), 0.123, 1e-12);
    lt_dash_free(d);
}

LT_TEST(assets)
{
    lt_dash_file *d = two_gauges();
    char out[16];
    int i, j;
    LT_CHECK(lt_dash_b64_encode("", 0, out, sizeof out) == 0);
    LT_CHECK(lt_dash_b64_encode("f", 1, out, sizeof out) == 4);
    LT_CHECK_STR(out, "Zg==");
    LT_CHECK(lt_dash_b64_encode("fo", 2, out, sizeof out) == 4);
    LT_CHECK_STR(out, "Zm8=");
    LT_CHECK(lt_dash_b64_encode("foo", 3, out, sizeof out) == 4);
    LT_CHECK_STR(out, "Zm9v");
    LT_CHECK(lt_dash_b64_encode("foobar", 6, out, sizeof out) == 8);
    LT_CHECK_STR(out, "Zm9vYmFy");
    LT_CHECK(lt_dash_b64_encode("foobar", 6, out, 5) == 0);        /* too small */
    i = lt_dash_asset_add(d, "/some/dir/bg.png", "abc", 3);
    j = lt_dash_asset_add(d, "Font.TTF", "xyz", 3);
    LT_CHECK(i == 0 && j == 1 && d->cluster.n_images == 2);
    LT_CHECK_STR(d->cluster.images[0].image_id, "bg.png");
    LT_CHECK(d->cluster.images[0].type == LT_RES_PNG);
    LT_CHECK(d->cluster.images[1].type == LT_RES_TTF);
    LT_CHECK_STR(d->cluster.images[0].data_base64, "YWJj");
    LT_CHECK(lt_dash_asset_add(d, "bg.png", "abcd", 4) == 0 && d->cluster.n_images == 2);   /* replaced */
    LT_CHECK(!lt_dash_asset_used(d, 0));
    dash_set_str(&d->cluster.comps[0].gauge->background_image_file_name, "bg.png");
    LT_CHECK(lt_dash_asset_used(d, 0));
    LT_CHECK(!lt_dash_asset_used(d, 1));
    dash_set_str(&d->cluster.comps[0].gauge->background_image_file_name, NULL);
    dash_set_str(&d->cluster.background_image_file_name, "bg.png");
    LT_CHECK(lt_dash_asset_used(d, 0));
    lt_dash_asset_remove(d, 0);
    LT_CHECK(d->cluster.n_images == 1 && strcmp(d->cluster.images[0].image_id, "Font.TTF") == 0);
    /* survives a round trip through the file format */
    {
        lt_buf b;
        char err[160];
        lt_dash_file *r;
        lt_buf_init(&b);
        lt_dash_write(d, &b);
        r = lt_dash_parse(b.data, b.len, err, sizeof err);
        LT_CHECK(r != NULL && r->cluster.n_images == 1);
        if (r != NULL) { lt_dash_free(r); }
        lt_buf_free(&b);
    }
    lt_dash_free(d);
}

LT_TEST(sync_ranges)
{
    lt_ini *ini = lt_ini_load_file(DEMO, NULL);
    lt_dash_file *d;
    lt_dash_gauge *g;
    size_t k;
    LT_CHECK(ini != NULL && ini->n_gauges > 0);
    d = lt_dash_new();
    g = lt_dash_add_gauge(d);
    dash_set_str(&g->output_channel, ini->gauges[0]->channel);
    g->min = -5.0; g->max = -4.0;
    k = lt_dash_sync_ranges(d, ini, NULL);
    LT_CHECK(k == 1);
    LT_CHECK(g->max > g->min);
    LT_CHECK(g->min != -5.0 || g->max != -4.0);
    LT_CHECK(lt_dash_sync_ranges(d, ini, NULL) == 0);               /* nothing left to change */
    dash_set_str(&g->output_channel, "noSuchChannelAnywhere");
    dash_set_str(&g->title, "noSuchGaugeEither");
    g->min = 1.0; g->max = 2.0;
    LT_CHECK(lt_dash_sync_ranges(d, ini, NULL) == 0);
    LT_CHECK_NEAR(g->max, 2.0, 1e-12);                                /* unknown gauges are kept */
    LT_CHECK(lt_dash_sync_ranges(d, NULL, NULL) == 0);
    lt_dash_free(d);
    lt_ini_free(ini);
}

static void ind_rect(const lt_dash_file *d, size_t i, double v[4])
{
    lt_dash_comp_get_rect(&d->cluster.comps[i], v);
}

LT_TEST(indicator_grid)
{
    lt_dash_file *d = lt_dash_new();
    double v[4], w[4];
    size_t i;
    for (i = 0; i < 7; i++) {
        lt_dash_indicator *in = lt_dash_add_indicator(d);
        in->relative_x = 0.01 * (double)i; in->relative_y = 0.8; in->relative_width = 0.05; in->relative_height = 0.1;
    }
    /* keep the bounding box 0..0.8 x 0.8..0.9 by anchoring two corners */
    d->cluster.comps[6].indicator->relative_x = 0.75;
    lt_dash_layout_indicators(d, 3, LT_FALSE);                       /* 3 columns -> 3 rows */
    ind_rect(d, 0, v); ind_rect(d, 1, w);
    LT_CHECK(w[0] > v[0] && v[1] == w[1]);
    ind_rect(d, 3, w);
    LT_CHECK(w[1] > v[1] && fabs(w[0] - v[0]) < 1e-9);
    ind_rect(d, 6, v);                                              /* alone in row 3, one cell wide */
    ind_rect(d, 0, w);
    LT_CHECK_NEAR(v[2], w[2], 1e-9);
    lt_dash_layout_indicators(d, 3, LT_TRUE);                        /* last row stretched */
    ind_rect(d, 6, v);
    LT_CHECK(v[2] > w[2] * 2.5);
    lt_dash_layout_indicators(d, 0, LT_FALSE);                       /* auto keeps the widest row */
    lt_dash_free(d);
}

LT_TEST(backend_fullscreen_null_safe)
{
    lt_backend be;
    memset(&be, 0, sizeof be);
    LT_CHECK(lt_backend_set_fullscreen(NULL, 1) == 0);
    LT_CHECK(lt_backend_set_fullscreen(&be, 1) == 0);               /* hook not provided */
}

/* ---- the tab, headless ---- */
static lt_app *app_with_project(void)
{
    lt_app *app;
    lt_project p;
    char *pdir;
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", TMP "/cfg") == 0);
    (void)lt_plat_mkdirs(TMP "/proj");
    app = lt_app_new();
    if (lt_project_create("dashproj", DEMO, "sig", TMP "/proj", &p) < 0) {
        LT_CHECK(lt_project_open(TMP "/proj/dashproj", &p) == 0);
    }
    pdir = lt_strdup(p.path);
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, pdir) == 0);
    lt_free(pdir);
    return app;
}

LT_TEST(tab_edit_undo_kiosk)
{
    lt_app *app;
    dash_state *st;
    dash_cfg *cfg;
    size_t n0, idx[8];
    int i;
    fresh_dir();
    app = app_with_project();
    LT_CHECK(lt_app_dashboard_tab_open(app) == 0);
    LT_CHECK(lt_app_screenshot(app, TMP "/a.ppm", 1100, 700, 3) == 0);
    st = lt_dtab_state(app);
    LT_CHECK(st != NULL && st->file != NULL);
    if (st == NULL || st->file == NULL) { lt_app_free(app); return; }
    cfg = lt_dashx_cfg(app);
    LT_CHECK(cfg->refresh_hz == 60 && cfg->snap && !cfg->lock);
    n0 = st->file->cluster.n_comps;
    LT_CHECK(n0 > 0);
    /* add, undo, redo */
    st->designer = 1;
    lt_dtab_add_gauge(app, st, 0.5, 0.5, LT_GP_HBAR, NULL);
    lt_dtab_flush(st);
    LT_CHECK(st->file->cluster.n_comps == n0 + 1);
    LT_CHECK(st->sel == (int)n0 && lt_dtab_is_sel(st, (int)n0));
    LT_CHECK(st->file->cluster.comps[n0].gauge->painter == LT_GP_HBAR);
    lt_dtab_undo(app, st);
    LT_CHECK(st->file->cluster.n_comps == n0);
    lt_dtab_redo(app, st);
    LT_CHECK(st->file->cluster.n_comps == n0 + 1);
    /* multi-selection, copy, paste, delete */
    lt_dtab_select(st, 0, 0);
    lt_dtab_select(st, 1, 1);
    LT_CHECK(lt_dtab_sel_list(st, idx, 8) == 2 && idx[0] == 0 && idx[1] == 1);
    lt_dtab_select(st, 1, 1);                                          /* toggle off */
    LT_CHECK(lt_dtab_sel_list(st, idx, 8) == 1);
    lt_dtab_select(st, 1, 1);
    lt_dtab_copy(app, st);
    LT_CHECK(st->clip != NULL);
    lt_dtab_paste(app, st);
    LT_CHECK(st->file->cluster.n_comps == n0 + 3);
    LT_CHECK(lt_dtab_sel_list(st, idx, 8) == 2);                       /* the pasted ones are selected */
    lt_dtab_remove_sel(st);
    LT_CHECK(st->file->cluster.n_comps == n0 + 1);
    lt_dtab_select_all(st);
    LT_CHECK(lt_dtab_sel_list(st, idx, 8) == 8 || st->n_msel > 8);
    lt_dtab_clear_sel(st);
    /* hidden component stays in the file but validation keeps working */
    lt_dtab_revalidate(app, st);
    LT_CHECK(st->rep_valid && st->compat_valid);
    /* undo everything */
    for (i = 0; i < 10; i++) { lt_dtab_undo(app, st); }
    LT_CHECK(st->file->cluster.n_comps == n0);
    /* full screen: only with a definition, hides the chrome, leaves again */
    LT_CHECK(lt_dashx_set_kiosk(app, 1) == 0);
    LT_CHECK(cfg->kiosk);
    LT_CHECK(lt_app_screenshot(app, TMP "/k.ppm", 1100, 700, 3) == 0);
    LT_CHECK(cfg->kiosk);
    LT_CHECK(lt_dashx_set_kiosk(app, 0) == 0);
    LT_CHECK(!cfg->kiosk);
    /* settings round trip through dashboard.conf */
    cfg->refresh_hz = 15; cfg->right_align = 1; cfg->ind_cols = 7; cfg->lock = 1;
    lt_dashx_cfg_save(app);
    lt_app_free(app);
    app = lt_app_new();
    cfg = lt_dashx_cfg(app);
    LT_CHECK(cfg->refresh_hz == 15 && cfg->right_align == 1 && cfg->ind_cols == 7 && cfg->lock == 1);
    cfg->refresh_hz = 30; cfg->right_align = 0; cfg->ind_cols = 0; cfg->lock = 0;
    lt_dashx_cfg_save(app);
    {   /* a hand-edited rate of INT_MIN overflowed the nearest-rate search (UBSan) */
        char *p = lt_path_join(app->config_dir, "dashboard.conf");
        lt_props pr;
        lt_props_init(&pr);
        LT_CHECK(lt_props_load(&pr, p) == 0);
        lt_props_set(&pr, "refresh_hz", "-2147483648");
        LT_CHECK(lt_props_save(&pr, p) == 0);
        lt_props_free(&pr);
        lt_app_free(app);
        app = lt_app_new();
        LT_CHECK(lt_dashx_cfg(app)->refresh_hz == 10);
        lt_dashx_cfg(app)->refresh_hz = 30;
        lt_dashx_cfg_save(app);
        lt_free(p);
    }
    lt_app_free(app);
}

LT_TEST(tab_library_roundtrip)
{
    lt_app *app;
    dash_state *st;
    char *dir, *np = NULL;
    fresh_dir();
    app = app_with_project();
    LT_CHECK(lt_app_dashboard_tab_open(app) == 0);
    LT_CHECK(lt_app_screenshot(app, TMP "/b.ppm", 1100, 700, 3) == 0);
    st = lt_dtab_state(app);
    LT_CHECK(st != NULL);
    if (st == NULL) { lt_app_free(app); return; }
    dir = lt_dtab_dir(app);
    LT_CHECK(dir != NULL);
    {
        char *old = lt_dashlib_path(dir, "Mine");     /* left over from an earlier run */
        (void)lt_plat_remove(old);
        lt_free(old);
    }
    LT_CHECK(lt_dashlib_create(dir, "Mine", st->file, &np) == 0);
    LT_CHECK(lt_dtab_load_file(app, st, np) == 0);
    LT_CHECK(st->src == DS_FILE);
    LT_CHECK_STR(app->project.cfg.dashboard, "Mine.dash");            /* the choice is remembered */
    lt_dtab_lib_refresh(app, st);
    LT_CHECK(st->lib.n >= 5);                                         /* 1 user + 4 built-in */
    LT_CHECK_STR(st->lib.e[0].category, "User");
    lt_dtab_reset_defaults(app, st);
    LT_CHECK(st->src != DS_FILE && st->path[0] == '\0');
    LT_CHECK(app->project.cfg.dashboard != NULL && app->project.cfg.dashboard[0] == '@');
    lt_free(np); lt_free(dir);
    lt_app_free(app);
}

static void ui_frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static void mouse(lt_app *app, int type, int x, int y)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.x = x; ev.y = y; ev.button = LT_MB_LEFT;
    lt_ui_input(app->ui, &ev);
    ui_frame(app);
}

/* Designer: double-click opens an item's properties, the panel follows the selection, and what it
 * edits (title, channel, range, warning and danger levels) survives Save and a reload */
LT_TEST(designer_properties)
{
    lt_app *app;
    dash_state *st;
    lt_dash_file *back;
    lt_dash_gauge *g;
    char *dir, *np = NULL;
    const char *chan;
    lt_rect r;
    size_t gi, i;
    int x, y;
    fresh_dir();
    app = app_with_project();
    LT_CHECK(lt_app_dashboard_tab_open(app) == 0);
    LT_CHECK(lt_app_screenshot(app, TMP "/p0.ppm", 1100, 700, 3) == 0);
    st = lt_dtab_state(app);
    LT_CHECK(st != NULL && st->file != NULL && st->file->cluster.n_comps >= 3);
    if (st == NULL || st->file == NULL || st->file->cluster.n_comps < 3) { lt_app_free(app); return; }
    dir = lt_dtab_dir(app);
    {
        char *old = lt_dashlib_path(dir, "Props");
        (void)lt_plat_remove(old);
        lt_free(old);
    }
    LT_CHECK(lt_dashlib_create(dir, "Props", st->file, &np) == 0);
    LT_CHECK(lt_dtab_load_file(app, st, np) == 0);
    ui_frame(app);
    ui_frame(app);
    /* the first gauge */
    for (gi = 0; gi < st->file->cluster.n_comps && st->file->cluster.comps[gi].gauge == NULL; gi++) {}
    LT_CHECK(gi < st->file->cluster.n_comps);
    if (gi >= st->file->cluster.n_comps) { lt_app_free(app); lt_free(dir); lt_free(np); return; }
    /* double-click it in the designer */
    st->designer = 1;
    ui_frame(app);
    r = lt_dash_view_component_rect(st->view, gi);
    x = r.x + r.w / 2; y = r.y + r.h / 2;
    mouse(app, LT_EV_MOUSE_MOVE, x, y);
    mouse(app, LT_EV_MOUSE_DOWN, x, y);
    mouse(app, LT_EV_MOUSE_UP, x, y);
    LT_CHECK(!st->props_open);                       /* one click selects */
    LT_CHECK(st->sel == (int)gi);
    mouse(app, LT_EV_MOUSE_DOWN, x, y);
    mouse(app, LT_EV_MOUSE_UP, x, y);
    LT_CHECK(st->props_open && st->props_idx == (int)gi);
    /* the panel follows the selection; Properties (toolbar) closes and opens it */
    lt_dtab_select(st, (int)(gi + 1), 0);
    ui_frame(app);
    LT_CHECK(st->props_idx == (int)gi + 1);
    lt_dtab_props_open(st, -1);
    LT_CHECK(!st->props_open);
    lt_dtab_select(st, (int)gi, 0);
    lt_dtab_props_open(st, st->sel);
    LT_CHECK(st->props_open && st->props_idx == (int)gi);
    lt_dtab_props_open(st, 9999);                    /* out of range closes */
    LT_CHECK(!st->props_open);
    lt_dtab_props_open(st, (int)gi);
    /* edit what the panel edits, draw it with every level on, save */
    g = st->file->cluster.comps[gi].gauge;
    chan = app->ini->channels[app->ini->n_channels - 1]->name;
    dash_set_str(&g->title, "Oil pressure");
    dash_set_str(&g->output_channel, chan);
    g->min = -5.0; g->max = 250.0;
    g->has_low_critical = LT_TRUE; g->low_critical = 5.0;
    g->has_low_warning = LT_TRUE; g->low_warning = 15.0;
    g->has_high_warning = LT_TRUE; g->high_warning = 200.0;
    g->has_high_critical = LT_TRUE; g->high_critical = 230.0;
    lt_dtab_touch(st);
    st->rebuild = 1;
    for (i = 0; i < 3; i++) { ui_frame(app); }
    LT_CHECK(lt_app_screenshot(app, TMP "/props.ppm", 1100, 760, 2) == 0);
    LT_CHECK(lt_dtab_save(app, st) == 0);
    LT_CHECK(!st->dirty);
    back = lt_dash_load(np);
    LT_CHECK(back != NULL && back->cluster.n_comps > gi && back->cluster.comps[gi].gauge != NULL);
    if (back != NULL && back->cluster.n_comps > gi && back->cluster.comps[gi].gauge != NULL) {
        const lt_dash_gauge *b = back->cluster.comps[gi].gauge;
        LT_CHECK_STR(b->title, "Oil pressure");
        LT_CHECK_STR(b->output_channel, chan);
        LT_CHECK_NEAR(b->min, -5.0, 1e-9);
        LT_CHECK_NEAR(b->max, 250.0, 1e-9);
        LT_CHECK(b->has_low_critical && b->has_low_warning && b->has_high_warning && b->has_high_critical);
        LT_CHECK_NEAR(b->low_critical, 5.0, 1e-9);
        LT_CHECK_NEAR(b->low_warning, 15.0, 1e-9);
        LT_CHECK_NEAR(b->high_warning, 200.0, 1e-9);
        LT_CHECK_NEAR(b->high_critical, 230.0, 1e-9);
    }
    if (back != NULL) { lt_dash_free(back); }
    lt_free(dir); lt_free(np);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(lib_names);
    LT_RUN(lib_crud);
    LT_RUN(lib_sort_categories);
    LT_RUN(lib_export_import_compat);
    LT_RUN(ts_scan);
    LT_RUN(history);
    LT_RUN(history_cap);
    LT_RUN(clone_clip_zorder);
    LT_RUN(hide_keeps_condition);
    LT_RUN(alignment);
    LT_RUN(assets);
    LT_RUN(sync_ranges);
    LT_RUN(indicator_grid);
    LT_RUN(backend_fullscreen_null_safe);
    LT_RUN(tab_edit_undo_kiosk);
    LT_RUN(tab_library_roundtrip);
    LT_RUN(designer_properties);
    LT_TEST_MAIN_END
}
