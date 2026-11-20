/* test_msqpart.c - partial tunes (.msqpart): write / read round trip, applying only the contained
 * constants, undo, differences from a reference, the constants behind a table, the dialogs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/tune/tune.h"

#define MP_DIR "build/lt_test_msqpart"

static lt_app *demo_app(void)
{
    char buf[512];
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    return app;
}

static double cur(lt_app *app, const char *name)
{
    lt_pagesrc ps = lt_app_pages(app);
    const lt_ini_const *c = lt_ini_find_const(app->ini, name);
    lt_tune_value v;
    double r = -1e9;
    if (c != NULL && lt_tune_read_const(app->ini, c, &ps, &v)) { r = v.scalar; }
    lt_tune_value_clear(&v);
    return r;
}

static void set_scalar(lt_tune *t, const char *name, double x)
{
    lt_tune_value v;
    lt_tune_value_init(&v);
    v.scalar = x;
    lt_tune_set_constant(t, name, &v, 0);
}

LT_TEST(msqpart_round_trip_apply_undo)
{
    static const char *const names[2] = { "cranking_rpm", "rpmHardLimit" };
    lt_app *app = demo_app();
    lt_pagesrc ps;
    lt_tune *part, *back;
    char **got = NULL;
    char *text;
    size_t n, i, applied = 0, skipped = 0, len = 0;
    double crank0, limit0, flow0;
    LT_CHECK(lt_plat_mkdirs(MP_DIR) == 0);
    crank0 = cur(app, "cranking_rpm");
    limit0 = cur(app, "rpmHardLimit");
    flow0 = cur(app, "injector_flow");
    LT_CHECK(crank0 > -1e9 && limit0 > -1e9 && flow0 > -1e9);

    /* capture the current values of two constants, change them, write the file */
    ps = lt_app_pages(app);
    part = lt_tune_part_capture(app->ini, &ps, app->tune, names, 2);
    LT_CHECK(part != NULL);
    LT_CHECK_INT((int)part->constants.len, 2);
    set_scalar(part, "cranking_rpm", crank0 + 123.0);
    set_scalar(part, "rpmHardLimit", 7300.0);
    LT_CHECK(lt_tune_save(part, MP_DIR "/two.msqpart") == 0);
    lt_tune_free(part);

    /* only the two constants are in the file and they read back */
    text = lt_file_read(MP_DIR "/two.msqpart", &len);
    LT_CHECK(text != NULL);
    if (text != NULL) {
        LT_CHECK(strstr(text, "name=\"cranking_rpm\"") != NULL);
        LT_CHECK(strstr(text, "name=\"rpmHardLimit\"") != NULL);
        LT_CHECK(strstr(text, "injector_flow") == NULL);
        LT_CHECK(strstr(text, "<msq") != NULL);
        lt_free(text);
    }
    back = lt_tune_load(MP_DIR "/two.msqpart");
    LT_CHECK(back != NULL);
    if (back != NULL) {
        n = lt_tune_part_names(back, &got);
        LT_CHECK_INT((int)n, 2);
        LT_CHECK_STR(got[0], "cranking_rpm");
        LT_CHECK_STR(got[1], "rpmHardLimit");
        for (i = 0; i < n; i++) { lt_free(got[i]); }
        lt_free(got);
        LT_CHECK_NEAR(lt_tune_get_value(back, "rpmHardLimit")->value.scalar, 7300.0, 1e-9);
        lt_tune_free(back);
    }

    /* apply: the two change, everything else stays */
    LT_CHECK(lt_feat_msqpart_can_undo(app) == 0);
    LT_CHECK(lt_feat_msqpart_apply_file(app, MP_DIR "/two.msqpart", &applied, &skipped) == 0);
    LT_CHECK_INT((int)applied, 2);
    LT_CHECK_INT((int)skipped, 0);
    LT_CHECK_NEAR(cur(app, "cranking_rpm"), crank0 + 123.0, 1e-6);
    LT_CHECK_NEAR(cur(app, "rpmHardLimit"), 7300.0, 1e-6);
    LT_CHECK_NEAR(cur(app, "injector_flow"), flow0, 1e-9);
    LT_CHECK(app->modified);
    /* the open tune follows the ECU */
    LT_CHECK(lt_tune_get_constant(app->tune, "rpmHardLimit") != NULL);
    LT_CHECK_NEAR(lt_tune_get_constant(app->tune, "rpmHardLimit")->value.scalar, 7300.0, 1e-6);

    /* undo restores the previous values */
    LT_CHECK(lt_feat_msqpart_can_undo(app) == 1);
    LT_CHECK(lt_feat_msqpart_undo(app) == 0);
    LT_CHECK_NEAR(cur(app, "cranking_rpm"), crank0, 1e-6);
    LT_CHECK_NEAR(cur(app, "rpmHardLimit"), limit0, 1e-6);
    LT_CHECK(lt_feat_msqpart_can_undo(app) == 0);
    LT_CHECK(lt_feat_msqpart_undo(app) != 0);

    /* a missing file and a constant this INI does not know */
    LT_CHECK(lt_feat_msqpart_apply_file(app, MP_DIR "/missing.msqpart", &applied, &skipped) != 0);
    part = lt_tune_new(app->ini->signature);
    set_scalar(part, "noSuchConstantHere", 1.0);
    set_scalar(part, "cranking_rpm", crank0 + 1.0);
    LT_CHECK(lt_tune_save(part, MP_DIR "/unknown.msqpart") == 0);
    lt_tune_free(part);
    LT_CHECK(lt_feat_msqpart_apply_file(app, MP_DIR "/unknown.msqpart", &applied, &skipped) == 0);
    LT_CHECK_INT((int)applied, 1);
    LT_CHECK_INT((int)skipped, 1);
    LT_CHECK_NEAR(cur(app, "cranking_rpm"), crank0 + 1.0, 1e-6);
    lt_app_free(app);
}

LT_TEST(msqpart_diff_and_targets)
{
    static const char *const one[1] = { "rpmHardLimit" };
    lt_app *app = demo_app();
    lt_tune *ref = lt_tune_from_defaults(app->ini), *now, *part;
    lt_map set;
    char **names = NULL;
    size_t n, i;
    int found = 0;
    LT_CHECK(ref != NULL);
    /* change one constant and it shows up among the differences from the INI defaults */
    part = lt_tune_new(app->ini->signature);
    set_scalar(part, "rpmHardLimit", 6123.0);
    LT_CHECK(lt_tune_save(part, MP_DIR "/limit.msqpart") == 0);
    lt_tune_free(part);
    LT_CHECK(lt_feat_msqpart_apply_file(app, MP_DIR "/limit.msqpart", NULL, NULL) == 0);
    now = lt_tune_clone(app->tune);
    n = lt_tune_part_diff_names(now, ref, &names);
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], "rpmHardLimit") == 0) { found = 1; }
        lt_free(names[i]);
    }
    lt_free(names);
    LT_CHECK(found);
    /* the same tune against itself: nothing */
    n = lt_tune_part_diff_names(now, now, &names);
    LT_CHECK_INT((int)n, 0);
    lt_free(names);
    /* extract keeps only what was asked */
    part = lt_tune_part_extract(now, one, 1);
    LT_CHECK_INT((int)part->constants.len, 1);
    LT_CHECK_NEAR(lt_tune_get_constant(part, "rpmHardLimit")->value.scalar, 6123.0, 1e-6);
    lt_tune_free(part);
    lt_tune_free(now);
    lt_tune_free(ref);
    /* save through the feature writes the open tune's value */
    LT_CHECK(lt_feat_msqpart_save(app, MP_DIR "/saved.msqpart", one, 1) == 0);
    part = lt_tune_load(MP_DIR "/saved.msqpart");
    LT_CHECK(part != NULL && part->constants.len == 1);
    if (part != NULL) {
        LT_CHECK_NEAR(lt_tune_get_constant(part, "rpmHardLimit")->value.scalar, 6123.0, 1e-6);
        lt_tune_free(part);
    }
    LT_CHECK(lt_feat_msqpart_save(app, MP_DIR "/none.msqpart", one, 0) != 0);
    /* a table brings its values and both axes */
    lt_map_init(&set, LT_FALSE);
    LT_CHECK(lt_tune_part_target_consts(app->ini, "veTableTbl", &set) >= 3);
    LT_CHECK(lt_map_has(&set, "veTable"));
    LT_CHECK(lt_map_has(&set, "veRpmBins"));
    LT_CHECK(lt_map_has(&set, "veLoadBins"));
    LT_CHECK_INT((int)lt_tune_part_target_consts(app->ini, "noSuchThing", &set), 0);
    lt_map_free(&set);
    lt_app_free(app);
}

LT_TEST(msqpart_dialogs_render)
{
    lt_app *app = demo_app();
    LT_CHECK(lt_app_dialog_push(app, "msqpart_save", "", "Save Partial Tune") != NULL);
    lt_app_screenshot(app, "build/lt_test_msqpart_save.ppm", 1000, 760, 3);
    lt_app_free(app);
    app = demo_app();
    lt_feat_msqpart_open_load(app, MP_DIR "/two.msqpart");
    lt_app_screenshot(app, "build/lt_test_msqpart_load.ppm", 1000, 760, 3);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(msqpart_round_trip_apply_undo);
    LT_RUN(msqpart_diff_and_targets);
    LT_RUN(msqpart_dialogs_render);
    LT_TEST_MAIN_END
}
