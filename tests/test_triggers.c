/* test_triggers.c - user action triggers: edge detection, hold time, re-arm, once, storage, and
 * the actions they run in the app (message, marker, logging, partial tune with confirmation).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/feat.h"
#include "../src/app/trig_logic.h"
#include "../src/datalog/datalog.h"

#define TR_DIR "build/lt_test_triggers"

/* ---- the rule model against a map of variables ---- */
typedef struct tvars { lt_map map; double x; lt_expr_env env; } tvars;

static void tv_init(tvars *v)
{
    lt_map_init(&v->map, LT_FALSE);
    v->x = 0.0;
    lt_map_put(&v->map, "x", &v->x);
    lt_expr_env_from_map(&v->env, &v->map);
}

static int step(lt_trig_set *s, tvars *v, double x, double t)
{
    size_t fired[4];
    v->x = x;
    return (int)lt_trig_run(s, &v->env, t, fired, 4);
}

LT_TEST(trig_edges)
{
    lt_trig_set s;
    tvars v;
    lt_trig_rule *r;
    tv_init(&v);
    lt_trig_set_init(&s);
    r = lt_trig_add(&s);
    lt_strlcpy(r->expr, "x > 10", sizeof(r->expr));
    /* true from the start: no firing until it was false once */
    LT_CHECK_INT(step(&s, &v, 20, 0), 0);
    LT_CHECK_INT(step(&s, &v, 20, 100), 0);
    LT_CHECK_INT(step(&s, &v, 5, 200), 0);
    LT_CHECK_INT(step(&s, &v, 20, 300), 1);          /* false -> true */
    LT_CHECK_INT(step(&s, &v, 25, 400), 0);          /* staying true */
    LT_CHECK_INT(step(&s, &v, 30, 500), 0);
    LT_CHECK_INT(step(&s, &v, 0, 600), 0);
    LT_CHECK_INT(step(&s, &v, 11, 700), 1);          /* again */
    LT_CHECK_INT((int)s.r[0].fires, 2);
    /* disabled rule / disabled set */
    s.r[0].enabled = 0;
    LT_CHECK_INT(step(&s, &v, 0, 800), 0);
    LT_CHECK_INT(step(&s, &v, 20, 900), 0);
    s.r[0].enabled = 1;
    s.enabled = 0;
    LT_CHECK_INT(step(&s, &v, 0, 1000), 0);
    LT_CHECK_INT(step(&s, &v, 20, 1100), 0);
    lt_trig_set_free(&s);
    lt_map_free(&v.map);
}

LT_TEST(trig_hold)
{
    lt_trig_set s;
    tvars v;
    lt_trig_rule *r;
    tv_init(&v);
    lt_trig_set_init(&s);
    r = lt_trig_add(&s);
    lt_strlcpy(r->expr, "x > 10", sizeof(r->expr));
    r->hold_s = 1.0;
    LT_CHECK_INT(step(&s, &v, 0, 0), 0);
    LT_CHECK_INT(step(&s, &v, 20, 1000), 0);         /* holding from 1.0 s */
    LT_CHECK_INT(s.r[0].state, LT_TRIG_HOLDING);
    LT_CHECK_INT(step(&s, &v, 20, 1500), 0);
    LT_CHECK_INT(step(&s, &v, 0, 1600), 0);          /* interrupted: start over */
    LT_CHECK_INT(s.r[0].state, LT_TRIG_ARMED);
    LT_CHECK_INT(step(&s, &v, 20, 1700), 0);
    LT_CHECK_INT(step(&s, &v, 20, 2600), 0);         /* 0.9 s */
    LT_CHECK_INT(step(&s, &v, 20, 2700), 1);         /* 1.0 s */
    LT_CHECK_INT(step(&s, &v, 20, 4000), 0);         /* still the same episode */
    lt_trig_set_free(&s);
    lt_map_free(&v.map);
}

LT_TEST(trig_rearm_and_once)
{
    lt_trig_set s;
    tvars v;
    lt_trig_rule *r;
    size_t fired[4];
    tv_init(&v);
    lt_trig_set_init(&s);
    r = lt_trig_add(&s);
    lt_strlcpy(r->expr, "x > 10", sizeof(r->expr));
    r->rearm_s = 5.0;
    r = lt_trig_add(&s);
    lt_strlcpy(r->expr, "x > 10", sizeof(r->expr));
    r->once = 1;
    LT_CHECK_INT(step(&s, &v, 0, 0), 0);
    v.x = 20;
    LT_CHECK_INT((int)lt_trig_run(&s, &v.env, 1000, fired, 4), 2);   /* both fire */
    LT_CHECK_INT((int)fired[0], 0);
    LT_CHECK_INT((int)fired[1], 1);
    LT_CHECK_INT(s.r[1].state, LT_TRIG_DONE);
    LT_CHECK_INT(step(&s, &v, 0, 2000), 0);          /* false, but within the re-arm delay */
    LT_CHECK_INT(step(&s, &v, 20, 3000), 0);
    LT_CHECK_INT(step(&s, &v, 20, 6500), 0);         /* delay over but never went false since */
    LT_CHECK_INT(step(&s, &v, 0, 6600), 0);          /* re-armed now */
    LT_CHECK_INT(s.r[0].state, LT_TRIG_ARMED);
    LT_CHECK_INT(step(&s, &v, 20, 6700), 1);         /* only the first: the second fired once */
    LT_CHECK_INT((int)s.r[0].fires, 2);
    LT_CHECK_INT((int)s.r[1].fires, 1);
    lt_trig_reset(&s.r[1]);                          /* re-armed by hand */
    LT_CHECK_INT(step(&s, &v, 0, 20000), 0);
    LT_CHECK_INT(step(&s, &v, 20, 20100), 2);
    lt_trig_set_free(&s);
    lt_map_free(&v.map);
}

LT_TEST(trig_errors_and_storage)
{
    lt_trig_set s, b;
    tvars v;
    lt_trig_rule *r;
    lt_props p;
    tv_init(&v);
    lt_trig_set_init(&s);
    r = lt_trig_add(&s);
    lt_strlcpy(r->expr, "x >", sizeof(r->expr));
    LT_CHECK_INT(lt_trig_eval(r, &v.env), -1);
    LT_CHECK(r->err[0] != '\0');
    LT_CHECK_INT(step(&s, &v, 0, 0), 0);
    lt_strlcpy(s.r[0].expr, "x > 1 && x < 3", sizeof(s.r[0].expr));   /* edited: recompiled */
    LT_CHECK_INT(lt_trig_eval(&s.r[0], &v.env), 0);
    v.x = 2;
    LT_CHECK_INT(lt_trig_eval(&s.r[0], &v.env), 1);
    LT_CHECK(s.r[0].err[0] == '\0');
    /* actions by key */
    LT_CHECK_INT(lt_trig_act_find("msqpart"), LT_TRIG_MSQPART);
    LT_CHECK_INT(lt_trig_act_find("nope"), -1);
    LT_CHECK(strcmp(lt_trig_act_key(LT_TRIG_LOG_START), "log_start") == 0);
    /* properties round trip */
    r = lt_trig_add(&s);
    lt_strlcpy(r->name, "Shift light", sizeof(r->name));
    lt_strlcpy(r->expr, "RPMValue > 6500", sizeof(r->expr));
    lt_strlcpy(r->arg, "race.dash", sizeof(r->arg));
    r->action = LT_TRIG_DASHBOARD; r->hold_s = 0.25; r->rearm_s = 10; r->once = 1; r->enabled = 0;
    s.enabled = 0;
    LT_CHECK(lt_plat_mkdirs(TR_DIR) == 0);
    LT_CHECK(lt_trig_save(&s, TR_DIR "/rules.properties") == 0);
    lt_trig_set_init(&b);
    LT_CHECK(lt_trig_load(&b, TR_DIR "/rules.properties") == 0);
    LT_CHECK_INT((int)b.n, 2);
    LT_CHECK_INT(b.enabled, 0);
    LT_CHECK_STR(b.r[1].name, "Shift light");
    LT_CHECK_STR(b.r[1].expr, "RPMValue > 6500");
    LT_CHECK_STR(b.r[1].arg, "race.dash");
    LT_CHECK_INT(b.r[1].action, LT_TRIG_DASHBOARD);
    LT_CHECK_NEAR(b.r[1].hold_s, 0.25, 1e-9);
    LT_CHECK_NEAR(b.r[1].rearm_s, 10.0, 1e-9);
    LT_CHECK_INT(b.r[1].once, 1);
    LT_CHECK_INT(b.r[1].enabled, 0);
    LT_CHECK_INT(b.r[1].state, LT_TRIG_WAIT_FALSE);
    /* missing file: empty enabled set */
    LT_CHECK(lt_trig_load(&b, TR_DIR "/missing.properties") == 0);
    LT_CHECK_INT((int)b.n, 0);
    LT_CHECK_INT(b.enabled, 1);
    /* bad values are clamped / defaulted */
    lt_props_init(&p);
    lt_props_set(&p, "trigger.count", "1");
    lt_props_set(&p, "trigger.0.hold", "-5");
    lt_props_set(&p, "trigger.0.action", "explode");
    lt_trig_from_props(&b, &p);
    LT_CHECK_INT((int)b.n, 1);
    LT_CHECK_NEAR(b.r[0].hold_s, 0.0, 1e-12);
    LT_CHECK_INT(b.r[0].action, LT_TRIG_MESSAGE);
    lt_props_free(&p);
    lt_trig_set_free(&b);
    lt_trig_set_free(&s);
    lt_map_free(&v.map);
}

/* ---- in the app: rules over the tune constants (they are in the same expression env) ---- */
static lt_app *demo_app(void)
{
    char buf[512];
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    return app;
}

static void set_limit(lt_app *app, double v)
{
    lt_tune *part = lt_tune_new(app->ini->signature);
    lt_tune_value tv;
    lt_tune_value_init(&tv);
    tv.scalar = v;
    lt_tune_set_constant(part, "rpmHardLimit", &tv, 0);
    LT_CHECK(lt_tune_save(part, TR_DIR "/limit.msqpart") == 0);
    lt_tune_free(part);
    LT_CHECK(lt_feat_msqpart_apply_file(app, TR_DIR "/limit.msqpart", NULL, NULL) == 0);
}

static int dialog_open(lt_app *app, const char *kind)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0 && app->dialogs[i]->open) { return 1; }
    }
    return 0;
}

LT_TEST(trig_app_actions)
{
    lt_app *app;
    lt_trig_set *s;
    lt_trig_rule *r;
    lt_tune *part;
    lt_tune_value tv;
    (void)lt_plat_mkdirs(TR_DIR);
    (void)lt_plat_remove(TR_DIR "/app.properties");
    LT_CHECK(lt_plat_setenv("LT_TRIGGERS_FILE", TR_DIR "/app.properties") == 0);
    app = demo_app();
    lt_free(app->config_dir);
    app->config_dir = lt_strdup(TR_DIR);           /* data logs go here */
    s = lt_feat_triggers_rules(app);
    LT_CHECK(s != NULL && s->n == 0);

    r = lt_trig_add(s);
    lt_strlcpy(r->name, "Limit", sizeof(r->name));
    lt_strlcpy(r->expr, "rpmHardLimit > 7000", sizeof(r->expr));
    lt_strlcpy(r->arg, "Limit raised", sizeof(r->arg));
    set_limit(app, 6000);
    LT_CHECK_INT((int)lt_feat_triggers_eval(app, 0), 0);      /* armed */
    set_limit(app, 7500);
    LT_CHECK_INT((int)lt_feat_triggers_eval(app, 100), 1);    /* fires: a message */
    LT_CHECK(strstr(lt_feat_triggers_last(app), "Limit raised") != NULL);
    LT_CHECK_INT((int)lt_feat_triggers_eval(app, 200), 0);

    /* marker: only while logging; start / stop logging */
    s->r[0].action = LT_TRIG_MARKER;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == -1);
    LT_CHECK(strstr(lt_feat_triggers_last(app), "not logging") != NULL);
    s->r[0].action = LT_TRIG_LOG_START;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == 0);
    LT_CHECK(app->rec != NULL && lt_rec_is_recording(app->rec));
    s->r[0].action = LT_TRIG_MARKER;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == 0);
    LT_CHECK(strstr(lt_feat_triggers_last(app), "marker") != NULL);
    s->r[0].action = LT_TRIG_LOG_STOP;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == 0);
    LT_CHECK(!lt_rec_is_recording(app->rec));

    /* beep with the offscreen backend: falls back to a notification, no crash */
    s->r[0].action = LT_TRIG_BEEP;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == 0);

    /* partial tune: opens the preview, nothing changes until Apply */
    part = lt_tune_new(app->ini->signature);
    lt_tune_value_init(&tv);
    tv.scalar = 6800;
    lt_tune_set_constant(part, "rpmHardLimit", &tv, 0);
    LT_CHECK(lt_tune_save(part, TR_DIR "/race.msqpart") == 0);
    lt_tune_free(part);
    s->r[0].action = LT_TRIG_MSQPART;
    lt_strlcpy(s->r[0].arg, TR_DIR "/race.msqpart", sizeof(s->r[0].arg));
    LT_CHECK(lt_feat_triggers_fire(app, 0) == 0);
    LT_CHECK(dialog_open(app, "msqpart_load"));
    LT_CHECK_NEAR(lt_tune_get_constant(app->tune, "rpmHardLimit")->value.scalar, 7500.0, 1e-6);
    lt_strlcpy(s->r[0].arg, TR_DIR "/nothing.msqpart", sizeof(s->r[0].arg));
    LT_CHECK(lt_feat_triggers_fire(app, 0) == -1);

    /* script / dashboard that do not exist */
    s->r[0].action = LT_TRIG_SCRIPT;
    lt_strlcpy(s->r[0].arg, "no-such-set", sizeof(s->r[0].arg));
    LT_CHECK(lt_feat_triggers_fire(app, 0) == -1);
    LT_CHECK(strstr(lt_feat_triggers_last(app), "not found") != NULL);
    s->r[0].action = LT_TRIG_DASHBOARD;
    LT_CHECK(lt_feat_triggers_fire(app, 0) == -1);

    /* the editor draws; the rules are saved and come back */
    LT_CHECK(lt_app_dialog_push(app, "triggers", "", "Action Triggers") != NULL);
    lt_app_screenshot(app, "build/lt_test_triggers.ppm", 1100, 820, 3);
    s->r[0].action = LT_TRIG_MESSAGE;
    LT_CHECK(lt_feat_triggers_save(app) == 0);
    lt_app_free(app);
    app = demo_app();
    s = lt_feat_triggers_rules(app);
    LT_CHECK_INT((int)s->n, 1);
    LT_CHECK_STR(s->r[0].expr, "rpmHardLimit > 7000");
    lt_app_free(app);
    LT_CHECK(lt_plat_setenv("LT_TRIGGERS_FILE", "") == 0);
}

int main(void)
{
    LT_RUN(trig_edges);
    LT_RUN(trig_hold);
    LT_RUN(trig_rearm_and_once);
    LT_RUN(trig_errors_and_storage);
    LT_RUN(trig_app_actions);
    LT_TEST_MAIN_END
}
