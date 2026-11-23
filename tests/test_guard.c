/* test_guard.c - never lose work and the setup checklist, without UI: the question asked before
 * quit / close / disconnect (which buttons, what they do), the autosave recovery file (write,
 * offer only when it differs, remove), the setup checklist's links into real ECU definitions
 * (rusEFI demo, Speeduino) and its per-project state; in the app, a project closed from the
 * question.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/guard_logic.h"
#include "../src/app/setup_logic.h"
#include "../src/plat/plat.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"

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

/* ---------------------------------------------------------------- in the app (terminal UI) */
static char g_txt[60000];

/* render app in cell mode (120x40) after keys (LT_SHOT_KEYS syntax); the text lands in g_txt */
static void tty(lt_app *app, const char *keys)
{
    size_t n = 0;
    char *d;
    lt_plat_setenv("LT_SHOT_TEXT", "build/test_guard_tty.txt");
    lt_plat_setenv("LT_SHOT_KEYS", keys != NULL ? keys : "");
    (void)lt_app_screenshot(app, "", 120, 40, 3);
    lt_plat_setenv("LT_SHOT_TEXT", "");
    lt_plat_setenv("LT_SHOT_KEYS", "");
    d = lt_file_read("build/test_guard_tty.txt", &n);
    g_txt[0] = '\0';
    if (d == NULL) { return; }
    if (n >= sizeof(g_txt)) { n = sizeof(g_txt) - 1; }
    memcpy(g_txt, d, n);
    g_txt[n] = '\0';
    lt_free(d);
}

/* "click:col/row" on the middle of label in g_txt (columns count UTF-8 characters); 0 not found */
static int click_on(const char *label, char *out, size_t cap)
{
    const char *hit = strstr(g_txt, label), *p;
    int row = 0, col = 0;
    if (hit == NULL) { return 0; }
    for (p = g_txt; p < hit; p++) {
        if (*p == '\n') { row++; col = 0; }
        else if (((unsigned char)*p & 0xC0) != 0x80) { col++; }
    }
    lt_snprintf(out, cap, "click:%d/%d", col + (int)strlen(label) / 2, row);
    return 1;
}


/* a new project named tag_<time> from the demo INI, opened; its directory in dir */
static int make_project(lt_app *app, const char *tag, char *dir, size_t cap)
{
    char ini[512], name[64], stamp[32];
    size_t i;
    lt_plat_timestamp(stamp, sizeof(stamp));
    for (i = 0; stamp[i] != '\0'; i++) { if (stamp[i] < '0' || stamp[i] > '9') { stamp[i] = '_'; } }
    if (lt_app_find_demo_ini(ini, sizeof(ini)) == NULL) { return -1; }
    lt_snprintf(name, sizeof(name), "%s_%s", tag, stamp);
    if (lt_app_new_project(app, name, ini) != 0) { return -1; }
    lt_strlcpy(dir, app->project.path, cap);
    return 0;
}

/* change one scalar setting: the tune has unsaved changes */
static int edit_one(lt_app *app)
{
    const lt_ini_const *c = NULL;
    lt_pagesrc ps;
    size_t i;
    double v = 0.0;
    for (i = 0; i < app->ini->n_constants && c == NULL; i++) {
        const lt_ini_const *x = app->ini->constants[i];
        if (!x->is_pc_var && x->shape.cols <= 1 && x->shape.rows <= 1 && x->type != LT_DT_BITS &&
            x->size_bytes > 0 && x->max > x->min) { c = x; }
    }
    if (c == NULL) { return -1; }
    ps = lt_app_pages(app);
    lt_ini_get(app->ini, &ps, c->name, &v);
    if (lt_ini_set(app->ini, &ps, c->name, v == c->max ? c->min : c->max) != 0) { return -1; }
    lt_app_edited(app);
    return app->modified ? 0 : -1;
}

/* File > Close Project with unsaved changes, "Close without saving": the project closes from the
 * question drawn after the dashboard (whose gauges were already queued for drawing) */
LT_TEST(test_close_from_question)
{
    lt_app *app = lt_app_new();
    char dir[512], k[64];
    LT_CHECK(make_project(app, "cl", dir, sizeof(dir)) == 0);
    LT_CHECK(edit_one(app) == 0);
    tty(app, NULL);
    lt_feat_guard_close_project(app);
    LT_CHECK(app->has_project);
    tty(app, NULL);
    LT_CHECK(click_on("Close without saving", k, sizeof(k)));
    tty(app, k);
    LT_CHECK(!app->has_project && app->ini == NULL);
    tty(app, NULL);                    /* the start screen */
    LT_CHECK(strstr(g_txt, "Connect to my ECU") != NULL);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(test_question);
    LT_RUN(test_unburned_text);
    LT_RUN(test_recovery);
    LT_RUN(test_setup_links);
    LT_RUN(test_setup_state);
    LT_RUN(test_close_from_question);
    LT_TEST_MAIN_END
}
