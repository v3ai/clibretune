/* feat_triggers.c - user action triggers (TunerStudio "Action Management"): rules "when <expression
 * over the live channels> becomes true (optionally for a hold time, with a re-arm delay)" that run
 * an action: a notification, a beep, a data log marker, start / stop logging, switch the dashboard,
 * load a partial tune (the Load Partial Tune preview is the confirmation) or play a recorded
 * action set. The rules are checked on every new realtime frame (tick hook).
 * Tools > Action Triggers... (dialog kind "triggers"; also from the Action Recorder).
 * Storage: <project>/projectCfg/triggers.properties, or triggers.properties in the configuration
 * folder without a project (LT_TRIGGERS_FILE overrides, for scripted use).
 * The rule logic (edges, hold, re-arm, storage format) is in trig_logic.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "trig_logic.h"
#include "dash_tab.h"
#include "../datalog/datalog.h"

#define FD_TRIG_ARG (LT_APP_FD_USER0 + 143)
#define TRIG_LOG_N 8

typedef struct trig_state {
    lt_trig_set set;
    int loaded;
    char path[600];
    unsigned long last_frames;
    char pending_dash[600];
    int sel, dirty;
    char log[TRIG_LOG_N][200];
    int log_n;
    char msg[240];
    int msg_err;
} trig_state;

static void st_free(lt_app *app, void *p)
{
    trig_state *s = (trig_state *)p;
    LT_UNUSED(app);
    lt_trig_set_free(&s->set);
}

static trig_state *st_get(lt_app *app)
{
    return (trig_state *)lt_ext_state(app, "triggers", sizeof(trig_state), st_free);
}

/* where the rules live (malloc'd) */
static char *rules_path(lt_app *app)
{
    char *e = lt_plat_getenv("LT_TRIGGERS_FILE"), *dir, *p;
    if (e != NULL && e[0] != '\0') { return e; }
    lt_free(e);
    if (app->has_project) {
        dir = lt_project_file(&app->project, "projectCfg");
    } else {
        dir = lt_strdup(app->config_dir != NULL ? app->config_dir : ".");
    }
    p = lt_path_join(dir, "triggers.properties");
    lt_free(dir);
    return p;
}

/* (re)load when the place changed: a project was opened or closed */
static void ensure_loaded(lt_app *app, trig_state *s)
{
    char *p = rules_path(app);
    if (!s->loaded || strcmp(p, s->path) != 0) {
        lt_strlcpy(s->path, p, sizeof(s->path));
        if (lt_trig_load(&s->set, p) != 0) {
            lt_snprintf(s->msg, sizeof(s->msg), "Cannot read the triggers: %s", lt_err_get());
            s->msg_err = 1;
        }
        s->loaded = 1;
        s->sel = 0;
        s->dirty = 0;
    }
    lt_free(p);
}

int lt_feat_triggers_save(lt_app *app)
{
    trig_state *s = st_get(app);
    char *dir;
    int rc;
    ensure_loaded(app, s);
    dir = lt_strdup(s->path);
    {
        const char *b = lt_path_base(dir);
        if (b > dir) { dir[b - dir - 1] = '\0'; (void)lt_plat_mkdirs(dir); }
    }
    lt_free(dir);
    rc = lt_trig_save(&s->set, s->path);
    if (rc == 0) { s->dirty = 0; }
    return rc;
}

lt_trig_set *lt_feat_triggers_rules(lt_app *app)
{
    trig_state *s = st_get(app);
    ensure_loaded(app, s);
    return &s->set;
}

const char *lt_feat_triggers_last(lt_app *app)
{
    trig_state *s = st_get(app);
    return s->log_n > 0 ? s->log[(s->log_n - 1) % TRIG_LOG_N] : "";
}

static void log_add(lt_app *app, trig_state *s, const char *rule, const char *what)
{
    char ts[32], *line = s->log[s->log_n % TRIG_LOG_N];
    LT_UNUSED(app);
    lt_plat_timestamp(ts, sizeof(ts));
    lt_snprintf(line, 200, "%s  %s: %s", strlen(ts) > 11 ? ts + 11 : ts, rule, what);
    s->log_n++;
}

/* a file named in a rule: as given, else in a folder (with ext added when missing) */
static char *resolve(const char *arg, const char *dir, const char *ext)
{
    char *p, *q;
    if (arg[0] == '\0') { return NULL; }
    if (lt_file_exists(arg)) { return lt_strdup(arg); }
    if (dir == NULL) { return NULL; }
    p = lt_path_join(dir, arg);
    if (lt_file_exists(p)) { return p; }
    q = (char *)lt_malloc(strlen(p) + strlen(ext) + 1);
    strcpy(q, p);
    strcat(q, ext);
    lt_free(p);
    if (lt_file_exists(q)) { return q; }
    lt_free(q);
    return NULL;
}

static char *actions_dir(lt_app *app)
{
    return lt_path_join(app->has_project ? app->project.path : (app->config_dir != NULL ? app->config_dir : "."), "actions");
}

static void show_dashboard(lt_app *app, trig_state *s, const char *path)
{
    dash_state *st;
    (void)lt_app_dashboard_tab_open(app);
    st = lt_dtab_state(app);
    if (st == NULL) {                            /* the tab draws for the first time next frame */
        lt_strlcpy(s->pending_dash, path, sizeof(s->pending_dash));
        return;
    }
    s->pending_dash[0] = '\0';
    if (lt_dtab_load_file(app, st, path) != 0) { lt_app_toast(app, 3, "Cannot load the dashboard: %s", lt_err_get()); }
}

/* run the action of rule i now; 0 done, -1 not possible (the reason goes to the log) */
int lt_feat_triggers_fire(lt_app *app, size_t i)
{
    trig_state *s = st_get(app);
    lt_trig_rule *r;
    const char *text;
    char what[200], *p = NULL, *dir;
    int rc = 0, rec;
    ensure_loaded(app, s);
    if (i >= s->set.n) { return -1; }
    r = &s->set.r[i];
    text = r->arg[0] != '\0' ? r->arg : r->name;
    rec = app->rec != NULL && lt_rec_is_recording(app->rec);
    what[0] = '\0';
    switch (r->action) {
    case LT_TRIG_MESSAGE:
        lt_app_toast(app, 2, "%s", text);
        lt_snprintf(what, sizeof(what), "message \"%s\"", text);
        break;
    case LT_TRIG_BEEP:
        if (!lt_backend_beep(app->backend)) { lt_app_toast(app, 2, "%s", r->name); }
        lt_strlcpy(what, "beep", sizeof(what));
        break;
    case LT_TRIG_MARKER:
        if (rec) {
            lt_rec_marker(app->rec, text);
            lt_snprintf(what, sizeof(what), "marker \"%s\"", text);
        } else { lt_strlcpy(what, "marker skipped (not logging)", sizeof(what)); rc = -1; }
        break;
    case LT_TRIG_LOG_START:
        if (rec) { lt_strlcpy(what, "already logging", sizeof(what)); }
        else if (lt_app_log_toggle(app) == 0 && app->rec != NULL && lt_rec_is_recording(app->rec)) {
            lt_strlcpy(what, "logging started", sizeof(what));
        } else { lt_strlcpy(what, "cannot start logging", sizeof(what)); rc = -1; }
        break;
    case LT_TRIG_LOG_STOP:
        if (!rec) { lt_strlcpy(what, "not logging", sizeof(what)); }
        else {
            (void)lt_app_log_toggle(app);
            lt_strlcpy(what, "logging stopped", sizeof(what));
        }
        break;
    case LT_TRIG_DASHBOARD:
        dir = lt_dtab_dir(app);
        p = resolve(r->arg, dir, ".dash");
        lt_free(dir);
        if (p != NULL) {
            show_dashboard(app, s, p);
            lt_snprintf(what, sizeof(what), "dashboard %s", lt_path_base(p));
        } else { lt_snprintf(what, sizeof(what), "dashboard \"%s\" not found", r->arg); rc = -1; }
        break;
    case LT_TRIG_MSQPART:
        p = resolve(r->arg, app->has_project ? app->project.path : NULL, ".msqpart");
        if (p != NULL) {
            lt_feat_msqpart_open_load(app, p);   /* preview: the user confirms with Apply */
            lt_snprintf(what, sizeof(what), "partial tune %s (waiting for confirmation)", lt_path_base(p));
        } else { lt_snprintf(what, sizeof(what), "partial tune \"%s\" not found", r->arg); rc = -1; }
        break;
    case LT_TRIG_SCRIPT:
        dir = actions_dir(app);
        p = resolve(r->arg, dir, ".ltaction");
        lt_free(dir);
        if (p != NULL) {
            size_t applied = 0;
            if (lt_feat_actions_play_file(app, p, &applied) == 0) {
                lt_snprintf(what, sizeof(what), "action script %s: %lu applied", lt_path_base(p), (unsigned long)applied);
            } else { lt_snprintf(what, sizeof(what), "action script failed: %s", lt_err_get()); rc = -1; }
        } else { lt_snprintf(what, sizeof(what), "action script \"%s\" not found", r->arg); rc = -1; }
        break;
    default:
        rc = -1;
        break;
    }
    lt_free(p);
    if (rc != 0 && r->action != LT_TRIG_MESSAGE) { lt_app_toast(app, 2, "Trigger %s: %s", r->name, what); }
    log_add(app, s, r->name, what);
    return rc;
}

size_t lt_feat_triggers_eval(lt_app *app, double now_ms)
{
    trig_state *s = st_get(app);
    size_t fired[32], n, i;
    ensure_loaded(app, s);
    if (app->ini == NULL || s->set.n == 0) { return 0; }
    n = lt_trig_run(&s->set, &app->varenv.env, now_ms, fired, 32);
    for (i = 0; i < n && i < 32; i++) { (void)lt_feat_triggers_fire(app, fired[i]); }
    return n;
}

static void trig_tick(lt_app *app)
{
    trig_state *s = st_get(app);
    if (s->pending_dash[0] != '\0' && lt_dtab_state(app) != NULL) {
        char p[600];
        lt_strlcpy(p, s->pending_dash, sizeof(p));
        show_dashboard(app, s, p);
    }
    if (app->ini == NULL || app->rt == NULL) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return; }
    if (app->frames_ok == s->last_frames) { return; }   /* only on new realtime data */
    s->last_frames = app->frames_ok;
    (void)lt_feat_triggers_eval(app, lt_plat_now_ms());
}

/* ---- editor ---- */
static const char *state_text(const lt_trig_rule *r, double now, char *b, size_t cap)
{
    if (!r->enabled) { return "off"; }
    if (r->last < 0) { return "error"; }
    switch (r->state) {
    case LT_TRIG_ARMED: return r->last ? "true" : "armed";
    case LT_TRIG_HOLDING:
        lt_snprintf(b, cap, "holding %.1f s", (now - r->t_true) / 1000.0);
        return b;
    case LT_TRIG_DONE: return "done (once)";
    default: return r->last ? "wait for false" : "re-arm delay";
    }
}

static const char *const *act_labels(void)
{
    static const char *v[LT_TRIG_N];
    int i;
    for (i = 0; i < LT_TRIG_N; i++) { v[i] = lt_trig_act_label(i); }
    return v;
}

static const char *arg_hint(int a)
{
    switch (a) {
    case LT_TRIG_MESSAGE: return "Message text (the rule name when empty)";
    case LT_TRIG_MARKER: return "Marker text (the rule name when empty); only while logging";
    case LT_TRIG_DASHBOARD: return "Dashboard file (.dash), or its name in the project dashboards folder";
    case LT_TRIG_MSQPART: return "Partial tune file (.msqpart); opens the preview, you confirm with Apply";
    case LT_TRIG_SCRIPT: return "Action set (.ltaction), or its name in the actions folder";
    default: return "";
    }
}

static int needs_file(int a)
{
    return a == LT_TRIG_DASHBOARD || a == LT_TRIG_MSQPART || a == LT_TRIG_SCRIPT;
}

static void draw_list(lt_app *app, trig_state *s, int h)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w[6], w1 = -1;
    size_t i;
    double now = lt_plat_now_ms();
    char b[64], f[24];
    w[0] = 28; w[1] = 200; w[2] = -3; w[3] = -2; w[4] = 130; w[5] = 60;
    lt_ui_layout_row(ui, 6, w, 0);
    lt_ui_label(ui, "");
    lt_ui_label_ex(ui, "Rule", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "When", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Then", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "State", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Fired", t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_layout_row(ui, 1, &w1, h);
    if (lt_ui_scroll_begin(ui, "trlist")) {
        if (s->set.n == 0) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "No rules yet: Add one, e.g. when coolant > 105 then show a message.", t->muted, LT_UI_ALIGN_LEFT);
        }
        for (i = 0; i < s->set.n; i++) {
            lt_trig_rule *r = &s->set.r[i];
            const char *st = state_text(r, now, b, sizeof(b));
            lt_color c = r->last < 0 && r->enabled ? t->error : (r->last > 0 && r->enabled ? t->success : t->text2);
            lt_ui_push_id_int(ui, (int)i);
            lt_ui_layout_row(ui, 6, w, 0);
            if (lt_ui_checkbox(ui, "", &r->enabled)) { s->dirty = 1; lt_trig_reset(r); }
            if (lt_ui_button_ex(ui, r->name[0] ? r->name : "(unnamed)",
                                (int)i == s->sel ? LT_UI_BTN_SELECTED : LT_UI_BTN_FLAT)) { s->sel = (int)i; }
            lt_ui_label_ex(ui, r->expr[0] ? r->expr : "(no condition)", r->expr[0] ? t->text : t->muted, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, lt_trig_act_label(r->action), t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, st, c, LT_UI_ALIGN_LEFT);
            lt_snprintf(f, sizeof(f), "%lu", r->fires);
            lt_ui_label_ex(ui, f, t->text2, LT_UI_ALIGN_RIGHT);
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
}

static void list_buttons(lt_app *app, trig_state *s)
{
    lt_ui *ui = app->ui;
    int w[7], have = s->sel >= 0 && (size_t)s->sel < s->set.n;
    size_t i;
    w[0] = 90; w[1] = 110; w[2] = 90; w[3] = 90; w[4] = 110; w[5] = -1; w[6] = 170;
    lt_ui_layout_row(ui, 7, w, 0);
    if (lt_ui_button(ui, "Add")) {
        lt_trig_rule *r = lt_trig_add(&s->set);
        lt_strlcpy(r->expr, "coolant > 105", sizeof(r->expr));
        lt_strlcpy(r->arg, "Engine hot", sizeof(r->arg));
        s->sel = (int)s->set.n - 1;
        s->dirty = 1;
    }
    if (lt_ui_button_ex(ui, "Duplicate", have ? 0 : LT_UI_BTN_DISABLED) && have) {
        lt_trig_rule *r = lt_trig_add(&s->set), *o = &s->set.r[s->sel];
        lt_strlcpy(r->expr, o->expr, sizeof(r->expr));
        lt_strlcpy(r->arg, o->arg, sizeof(r->arg));
        lt_snprintf(r->name, sizeof(r->name), "%.56s copy", o->name);
        r->enabled = o->enabled; r->action = o->action; r->once = o->once;
        r->hold_s = o->hold_s; r->rearm_s = o->rearm_s;
        s->sel = (int)s->set.n - 1;
        s->dirty = 1;
    }
    if (lt_ui_button_ex(ui, "Up", have && s->sel > 0 ? 0 : LT_UI_BTN_DISABLED) && have && s->sel > 0) {
        lt_trig_rule tmp = s->set.r[s->sel];
        s->set.r[s->sel] = s->set.r[s->sel - 1];
        s->set.r[s->sel - 1] = tmp;
        s->sel--;
        s->dirty = 1;
    }
    if (lt_ui_button_ex(ui, "Down", have && (size_t)s->sel + 1 < s->set.n ? 0 : LT_UI_BTN_DISABLED) && have &&
        (size_t)s->sel + 1 < s->set.n) {
        lt_trig_rule tmp = s->set.r[s->sel];
        s->set.r[s->sel] = s->set.r[s->sel + 1];
        s->set.r[s->sel + 1] = tmp;
        s->sel++;
        s->dirty = 1;
    }
    if (lt_ui_button_ex(ui, "Delete", have ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && have) {
        lt_trig_remove(&s->set, (size_t)s->sel);
        if ((size_t)s->sel >= s->set.n && s->sel > 0) { s->sel--; }
        s->dirty = 1;
    }
    lt_ui_label(ui, "");
    if (lt_ui_button(ui, "Re-arm all rules")) {
        for (i = 0; i < s->set.n; i++) { lt_trig_reset(&s->set.r[i]); }
    }
}

static void draw_rule(lt_app *app, trig_state *s, lt_trig_rule *r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w[5], c, prev;
    char b[200];
    const char *picked = lt_app_file_result(app, FD_TRIG_ARG);
    if (picked != NULL) { lt_strlcpy(r->arg, picked, sizeof(r->arg)); s->dirty = 1; }
    w[0] = 150; w[1] = -1; w[2] = 150;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "Name:");
    if (lt_ui_text(ui, "trname", r->name, sizeof(r->name), 0) & LT_UI_CHANGED) { s->dirty = 1; }
    if (lt_ui_checkbox(ui, "Enabled", &r->enabled)) { s->dirty = 1; lt_trig_reset(r); }
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "When true:");
    if (lt_ui_text(ui, "trexpr", r->expr, sizeof(r->expr), 0) & LT_UI_CHANGED) { s->dirty = 1; }
    c = app->ini != NULL ? lt_trig_eval(r, &app->varenv.env) : 0;
    if (c < 0) { lt_ui_label_ex(ui, "error", t->error, LT_UI_ALIGN_LEFT); }
    else { lt_ui_label_ex(ui, c ? "now: true" : "now: false", c ? t->success : t->text2, LT_UI_ALIGN_LEFT); }
    if (c < 0) {
        w[0] = 150; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "");
        lt_ui_label_ex(ui, r->err, t->error, LT_UI_ALIGN_LEFT);
    }
    w[0] = 150; w[1] = 110; w[2] = 230; w[3] = 110; w[4] = -1;
    lt_ui_layout_row(ui, 5, w, 0);
    lt_ui_label(ui, "Hold for:");
    if (lt_ui_number(ui, "trhold", &r->hold_s, 0.0, 3600.0, 0.1, 1)) { s->dirty = 1; }
    lt_ui_label_ex(ui, "s (must stay true this long)", t->muted, LT_UI_ALIGN_LEFT);
    if (lt_ui_number(ui, "trrearm", &r->rearm_s, 0.0, 86400.0, 1.0, 1)) { s->dirty = 1; }
    lt_ui_label_ex(ui, "s re-arm delay after firing (and the condition must go false)", t->muted, LT_UI_ALIGN_LEFT);
    w[0] = 150; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "");
    if (lt_ui_checkbox(ui, "Fire only once (until re-armed by hand or the rules are reloaded)", &r->once)) {
        s->dirty = 1;
        lt_trig_reset(r);
    }
    w[0] = 150; w[1] = 300; w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "Then:");
    prev = r->action;
    if (lt_ui_combo(ui, "tract", &r->action, act_labels(), LT_TRIG_N)) { s->dirty = 1; }
    if (prev != r->action && needs_file(r->action) != needs_file(prev)) { r->arg[0] = '\0'; }
    lt_ui_label_ex(ui, arg_hint(r->action), t->muted, LT_UI_ALIGN_LEFT);
    if (r->action != LT_TRIG_BEEP && r->action != LT_TRIG_LOG_START && r->action != LT_TRIG_LOG_STOP) {
        w[0] = 150; w[1] = -1; w[2] = 110;
        lt_ui_layout_row(ui, needs_file(r->action) ? 3 : 2, w, 0);
        lt_ui_label(ui, needs_file(r->action) ? "File:" : "Text:");
        if (lt_ui_text(ui, "trarg", r->arg, sizeof(r->arg), 0) & LT_UI_CHANGED) { s->dirty = 1; }
        if (needs_file(r->action) && lt_ui_button(ui, "Browse...")) {
            const char *flt = r->action == LT_TRIG_DASHBOARD ? "Dashboards|*.dash|All Files|*"
                              : (r->action == LT_TRIG_MSQPART ? "Partial Tune|*.msqpart|All Files|*" : "Action Sets|*.ltaction|All Files|*");
            lt_app_file_dialog(app, FD_TRIG_ARG, LT_UI_FILE_OPEN, NULL, "", flt);
        }
    }
    w[0] = 150; w[1] = 150; w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button(ui, "Test the action")) {
        (void)lt_feat_triggers_fire(app, (size_t)s->sel);
    }
    lt_snprintf(b, sizeof(b), "fired %lu time%s since loading", r->fires, r->fires == 1 ? "" : "s");
    lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
}

static void trig_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    trig_state *s = st_get(app);
    int open = d->open, w1 = -1, w[4], cell = lt_ui_cell_mode(ui), k, shown;
    char b[700];
    ensure_loaded(app, s);
    app->anim_ms = 250;                                  /* live states */
    if (lt_ui_modal_begin(ui, "Action Triggers", 1060, 800, &open)) {
        w[0] = 200; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        if (lt_ui_checkbox(ui, "Run the triggers", &s->set.enabled)) { s->dirty = 1; }
        lt_snprintf(b, sizeof(b), "Checked on every realtime frame while connected. Rules: %s", s->path);
        lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
        draw_list(app, s, cell ? 100 : 200);
        list_buttons(app, s);
        lt_ui_separator(ui);
        if (s->sel >= 0 && (size_t)s->sel < s->set.n) {
            draw_rule(app, s, &s->set.r[s->sel]);
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Select a rule to edit it.", t->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Recent firings", t->muted, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, -(cell ? 2 : 44));
        if (lt_ui_scroll_begin(ui, "trlog")) {
            shown = 0;
            for (k = s->log_n - 1; k >= 0 && k >= s->log_n - TRIG_LOG_N; k--) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, s->log[k % TRIG_LOG_N], t->text2, LT_UI_ALIGN_LEFT);
                shown++;
            }
            if (shown == 0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "Nothing fired yet.", t->muted, LT_UI_ALIGN_LEFT);
            }
            lt_ui_scroll_end(ui);
        }
        w[0] = -1; w[1] = 120; w[2] = 100;
        lt_ui_layout_row(ui, 3, w, 0);
        if (s->msg[0] != '\0') { lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT); }
        else { lt_ui_label_ex(ui, s->dirty ? "Unsaved changes (saved on Close)" : "", t->muted, LT_UI_ALIGN_LEFT); }
        if (lt_ui_button_ex(ui, "Save", s->dirty ? LT_UI_BTN_PRIMARY : 0)) {
            if (lt_feat_triggers_save(app) == 0) { lt_strlcpy(s->msg, "Saved", sizeof(s->msg)); s->msg_err = 0; }
            else { lt_snprintf(s->msg, sizeof(s->msg), "Cannot save: %s", lt_err_get()); s->msg_err = 1; }
        }
        if (lt_ui_button(ui, "Close")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void trig_close(lt_app *app, lt_app_dialog *d)
{
    trig_state *s = st_get(app);
    if (s->dirty && lt_feat_triggers_save(app) != 0) { lt_app_toast(app, 3, "Cannot save the triggers: %s", lt_err_get()); }
    s->msg[0] = '\0';
    d->data = NULL;
}

void lt_feat_triggers_open(lt_app *app)
{
    (void)lt_app_dialog_push(app, "triggers", "", "Action Triggers");
}

void lt_feat_triggers_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "triggers"; dk.draw = trig_draw; dk.close = trig_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Action &Triggers...", NULL, lt_feat_triggers_open, lt_ext_pred_ini);
    lt_ext_add_tick(reg, trig_tick);
}
