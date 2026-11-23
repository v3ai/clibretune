/* test_guard.c - never lose work and the setup checklist, without UI: the question asked before
 * quit / close / disconnect (which buttons, what they do), the autosave recovery file (write,
 * offer only when it differs, remove), the setup checklist's links into real ECU definitions
 * (rusEFI demo, Speeduino) and its per-project state.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/guard_logic.h"
#include "../src/app/setup_logic.h"
#include "../src/plat/plat.h"
#include <string.h>
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/project/project.h"

static int count_char(const char *s, char c)
{
    int n = 0;
    for (; *s != '\0'; s++) { n += *s == c; }
    return n;
}

LT_TEST(test_question)
{
    lt_guard_q q;
    /* nothing to lose: no question */
    LT_CHECK_INT(lt_guard_question(LT_GUARD_QUIT, 0, 0, &q), 0);
    LT_CHECK_INT(lt_guard_question(LT_GUARD_DISCONNECT, 0, 1, &q), 0);  /* the tune keeps edits */
    /* unsaved only */
    LT_CHECK_INT(lt_guard_question(LT_GUARD_QUIT, 0, 1, &q), 1);
    LT_CHECK_STR(q.buttons, "Save and quit|Quit without saving|Cancel");
    LT_CHECK_INT(q.n, 3);
    LT_CHECK_INT(q.act[0], LT_GA_SAVE);
    LT_CHECK_INT(q.act[1], LT_GA_PROCEED);
    LT_CHECK_INT(q.act[2], LT_GA_CANCEL);
    LT_CHECK_INT(count_char(q.buttons, '|') + 1, q.n);
    /* unburned only, closing */
    LT_CHECK_INT(lt_guard_question(LT_GUARD_CLOSE, 3, 0, &q), 1);
    LT_CHECK_STR(q.buttons, "Burn and close|Close without burning|Cancel");
    LT_CHECK_INT(q.act[0], LT_GA_BURN);
    LT_CHECK(strstr(q.msg, "3 changed settings") != NULL);
    LT_CHECK(strstr(q.title, "Burn") != NULL);
    /* both */
    LT_CHECK_INT(lt_guard_question(LT_GUARD_QUIT, 1, 1, &q), 1);
    LT_CHECK_STR(q.buttons, "Burn and quit|Save and quit|Quit without saving|Cancel");
    LT_CHECK_INT(q.act[0], LT_GA_BURN);
    LT_CHECK_INT(q.act[1], LT_GA_SAVE);
    LT_CHECK_INT(q.act[2], LT_GA_PROCEED);
    LT_CHECK_INT(q.act[3], LT_GA_CANCEL);
    LT_CHECK(strstr(q.msg, "1 changed setting is") != NULL);
    LT_CHECK(strstr(q.msg, "not saved") != NULL);
    /* disconnect with unknown count */
    LT_CHECK_INT(lt_guard_question(LT_GUARD_DISCONNECT, LT_GUARD_SOME, 1, &q), 1);
    LT_CHECK_STR(q.buttons, "Burn and disconnect|Disconnect without burning|Cancel");
    LT_CHECK(strstr(q.msg, "Some changes") != NULL);
    LT_CHECK(strstr(q.msg, "keeps them in your tune") != NULL);
}

LT_TEST(test_unburned_text)
{
    char b[64];
    lt_guard_unburned_text(1, b, sizeof(b));
    LT_CHECK_STR(b, "1 unburned change");
    lt_guard_unburned_text(12, b, sizeof(b));
    LT_CHECK_STR(b, "12 unburned changes");
    lt_guard_unburned_text(LT_GUARD_SOME, b, sizeof(b));
    LT_CHECK_STR(b, "Unburned changes");
}

static lt_tune *mk_tune(double v)
{
    lt_tune *t = lt_tune_new("test sig");
    lt_tune_value tv;
    lt_tune_value_init(&tv);
    tv.scalar = v;
    lt_tune_set_constant(t, "reqFuel", &tv, 0);
    tv.scalar = 4.0;
    lt_tune_set_constant(t, "nCylinders", &tv, 0);
    return t;
}

LT_TEST(test_recovery)
{
    const char *dir = "build/test_guard_proj";
    lt_tune *cur = mk_tune(10.5), *edit = mk_tune(11.25);
    char when[24], *p;
    int nd = -1;
    lt_plat_mkdirs(dir);
    (void)lt_guard_recovery_remove(dir);
    p = lt_guard_recovery_path(dir);
    LT_CHECK(p != NULL && strstr(p, "recovery") != NULL);
    /* nothing autosaved: nothing to offer */
    LT_CHECK_INT(lt_guard_recovery_pending(dir, cur, when, sizeof(when), &nd), 0);
    /* an autosave equal to the saved tune is stale: removed, not offered */
    LT_CHECK_INT(lt_guard_recovery_write(dir, cur), 0);
    LT_CHECK(lt_file_exists(p));
    LT_CHECK_INT(lt_guard_recovery_pending(dir, cur, when, sizeof(when), &nd), 0);
    LT_CHECK(!lt_file_exists(p));
    /* an autosave with edits is offered, with its time */
    LT_CHECK_INT(lt_guard_recovery_write(dir, edit), 0);
    LT_CHECK_INT(lt_guard_recovery_pending(dir, cur, when, sizeof(when), &nd), 1);
    LT_CHECK_INT(nd, 1);
    LT_CHECK_INT((int)strlen(when), 19);
    LT_CHECK(lt_file_exists(p));
    {
        lt_tune *back = lt_tune_load(p);
        const lt_tune_entry *e = back != NULL ? lt_tune_get_constant(back, "reqFuel") : NULL;
        LT_CHECK(e != NULL);
        if (e != NULL) { LT_CHECK_NEAR(e->value.scalar, 11.25, 1e-9); }
        lt_tune_free(back);
    }
    /* no saved tune at all: any autosave is offered */
    LT_CHECK_INT(lt_guard_recovery_pending(dir, NULL, when, sizeof(when), NULL), 1);
    LT_CHECK_INT(lt_guard_recovery_remove(dir), 0);
    LT_CHECK(!lt_file_exists(p));
    LT_CHECK_INT(lt_guard_recovery_remove(dir), 0);       /* absent is fine */
    lt_free(p);
    lt_tune_free(cur);
    lt_tune_free(edit);
}

static lt_ini *load_fixture(const char *name)
{
    char path[256];
    lt_snprintf(path, sizeof(path), "tests/fixtures/ini/%s", name);
    if (!lt_file_exists(path)) { lt_snprintf(path, sizeof(path), "../tests/fixtures/ini/%s", name); }
    return lt_ini_load_file(path, NULL);
}

static const char *link_of(const lt_setup_link *l, const char *id)
{
    int i;
    for (i = 0; i < LT_SETUP_STEPS; i++) {
        if (strcmp(lt_setup_step_at(i)->id, id) == 0) { return l[i].target; }
    }
    return "?";
}

LT_TEST(test_setup_links)
{
    lt_setup_link l[LT_SETUP_STEPS];
    lt_ini *ini = load_fixture("demo.ini");                /* rusEFI */
    LT_CHECK(ini != NULL);
    if (ini != NULL) {
        LT_CHECK_INT(lt_setup_match(ini, l), LT_SETUP_STEPS);
        LT_CHECK_STR(link_of(l, "engine"), "engineChars");
        LT_CHECK_STR(link_of(l, "injectors"), "injectionSettings");
        LT_CHECK_STR(link_of(l, "trigger"), "triggerConfiguration");
        LT_CHECK_STR(link_of(l, "map"), "mapSettings");
        LT_CHECK_STR(link_of(l, "tps"), "tpsSensor");
        LT_CHECK_STR(link_of(l, "temps"), "cltSensor");
        LT_CHECK_STR(link_of(l, "wideband"), "analogEgoSettings");
        LT_CHECK_STR(link_of(l, "ignition"), "ignitionSettings");
        LT_CHECK(strstr(l[0].where, " > ") != NULL);
        LT_CHECK(strchr(l[0].where, '&') == NULL);
        lt_ini_free(ini);
    }
    ini = load_fixture("speeduino_202501.ini");
    LT_CHECK(ini != NULL);
    if (ini != NULL) {
        LT_CHECK(lt_setup_match(ini, l) >= 7);
        LT_CHECK_STR(link_of(l, "engine"), "engine_constants");
        LT_CHECK_STR(link_of(l, "injectors"), "injChars");
        LT_CHECK_STR(link_of(l, "trigger"), "triggerSettings");
        LT_CHECK_STR(link_of(l, "map"), "mapCal");
        LT_CHECK_STR(link_of(l, "temps"), "std_ms2gentherm");
        LT_CHECK_STR(link_of(l, "wideband"), "egoControl");
        LT_CHECK_STR(link_of(l, "ignition"), "sparkSettings");
        lt_ini_free(ini);
    }
    LT_CHECK_INT(lt_setup_match(NULL, l), 0);
    LT_CHECK_STR(l[0].target, "");
}

LT_TEST(test_setup_state)
{
    const char *dir = "build/test_setup_proj";
    lt_setup_state s, s2;
    char *f = lt_path_join(dir, "setup.conf");
    lt_plat_mkdirs(dir);
    if (lt_file_exists(f)) { lt_plat_remove(f); }
    LT_CHECK_INT(lt_setup_load(&s, dir), 1);              /* fresh */
    LT_CHECK_INT(lt_setup_count_done(&s), 0);
    s.done[2] = 1;
    s.done[5] = 1;
    s.shown = 1;
    LT_CHECK_INT(lt_setup_save(&s, dir), 0);
    LT_CHECK_INT(lt_setup_load(&s2, dir), 0);
    LT_CHECK_INT(lt_setup_count_done(&s2), 2);
    LT_CHECK_INT(s2.done[2], 1);
    LT_CHECK_INT(s2.done[0], 0);
    LT_CHECK_INT(s2.shown, 1);
    LT_CHECK_INT(s2.dismissed, 0);
    LT_CHECK(lt_setup_step_at(LT_SETUP_STEPS) == NULL);
    LT_CHECK(lt_setup_step_at(0) != NULL && lt_setup_step_at(0)->title[0] != '\0');
    lt_free(f);
}

static void g_frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static void g_click(lt_app *app, int x, int y)
{
    lt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_MOUSE_MOVE; ev.x = x; ev.y = y; lt_ui_input(app->ui, &ev);
    g_frame(app);
    ev.type = LT_EV_MOUSE_DOWN; ev.button = LT_MB_LEFT; lt_ui_input(app->ui, &ev);
    g_frame(app);
    ev.type = LT_EV_MOUSE_UP; lt_ui_input(app->ui, &ev);
    g_frame(app);
}

static double g_cell(lt_app *app)
{
    lt_pagesrc ps = lt_app_pages(app);
    const lt_ini_const *c = lt_ini_find_const(app->ini, "veTable");
    static double v[4096];
    v[0] = -1.0;
    if (c != NULL) { (void)lt_ini_const_get_array(app->ini, c, &ps, v, 4096); }
    return v[0];
}

/* "Close without saving" must not save: the project's "save the tune when the project closes"
 * setting (on by default) used to write the discarded edits anyway */
LT_TEST(test_close_without_saving)
{
    char buf[512];
    lt_app *app = lt_app_new();
    lt_project p;
    char *dir = NULL;
    const lt_ini_const *c;
    lt_pagesrc ps;
    static double arr[4096];
    double v0, v1;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    lt_plat_mkdirs("build/test_guard_proj");
    if (lt_project_create("discard", buf, "", "build/test_guard_proj", &p) < 0) {
        LT_CHECK(lt_project_open("build/test_guard_proj/discard", &p) == 0);
    }
    dir = lt_strdup(p.path);
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(app->project.cfg.settings.auto_save_tune);
    v0 = g_cell(app);
    v1 = v0 + 5.0;
    c = lt_ini_find_const(app->ini, "veTable");
    ps = lt_app_pages(app);
    LT_CHECK(c != NULL && lt_ini_const_get_array(app->ini, c, &ps, arr, 4096) > 0);
    arr[0] = v1;
    LT_CHECK(c != NULL && lt_ini_const_set_array(app->ini, c, &ps, arr, c->shape.rows * c->shape.cols) == 0);
    lt_app_edited(app);
    LT_CHECK(app->modified);
    lt_app_screenshot(app, "build/test_guard_close.ppm", 1100, 800, 2);
    lt_feat_guard_close_project(app);
    g_frame(app);
    g_frame(app);
    LT_CHECK(app->has_project);                        /* the question is open */
    g_click(app, 673, 437);                            /* Close without saving */
    LT_CHECK(!app->has_project);
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK_NEAR(g_cell(app), v0, 1e-9);
    LT_CHECK(app->project.cfg.settings.auto_save_tune);  /* the setting itself is kept */
    lt_app_free(app);
    lt_free(dir);
}

int main(void)
{
    LT_RUN(test_question);
    LT_RUN(test_unburned_text);
    LT_RUN(test_recovery);
    LT_RUN(test_setup_links);
    LT_RUN(test_setup_state);
    LT_RUN(test_close_without_saving);
    LT_TEST_MAIN_END
}
