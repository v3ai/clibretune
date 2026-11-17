/* feat_actions.c - Action Recorder / Manager (G-82): record the table and constant edits made in the
 * editors as an action set, keep the sets (project actions/ folder, else the config folder), edit
 * their name / description / tags / compatible ECUs, review the timeline, validate against the open
 * ECU definition, play a set back on the open tune (pauses scaled by the playback speed) and
 * export / import .ltaction files. Tools -> Action Recorder...  (dialog kind "actions").
 * Recording survives closing the dialog: the editors call lt_feat_actions_note_table / _constant.
 * Lua steps are stored and validated, never executed; SendCommand steps are skipped on playback.
 * The model is project/action.c, the helpers are actions_logic.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "actions_logic.h"

#define FD_ACT_IMPORT (LT_APP_FD_USER0 + 72)
#define FD_ACT_EXPORT (LT_APP_FD_USER0 + 73)
#define ACT_FILTERS "LibreTune Action|*.ltaction|All Files|*"
#define ACT_EXT ".ltaction"

typedef struct act_entry { char *file; lt_action_set set; } act_entry;

typedef struct act_state {
    lt_arec rec;
    int rec_inited;
    char name[96], desc[200], tags[160];
    int restrict_ecu;
    act_entry *sets; size_t nsets;
    int loaded;
    char loaded_dir[512];
    int sel, step;
    char ed_desc[200], ed_tags[160], ed_ecus[200]; int ed_for;   /* editor buffers of set `ed_for` */
    char lua[400];
    int speed;                  /* index of the playback speed combo */
    char msg[300]; int msg_err;
    char *report;               /* validation / playback text */
    int confirm_play, confirm_del, close_req;
} act_state;

static void st_clear_sets(act_state *s)
{
    size_t i;
    for (i = 0; i < s->nsets; i++) { lt_action_set_free(&s->sets[i].set); lt_free(s->sets[i].file); }
    lt_free(s->sets);
    s->sets = NULL; s->nsets = 0; s->loaded = 0; s->ed_for = -1;
}

static void st_free(lt_app *app, void *p)
{
    act_state *s = (act_state *)p;
    LT_UNUSED(app);
    st_clear_sets(s);
    if (s->rec_inited) { lt_arec_free(&s->rec); }
    lt_free(s->report);
}

static act_state *st_get(lt_app *app)
{
    act_state *s = (act_state *)lt_ext_state(app, "actions", sizeof(act_state), st_free);
    if (!s->rec_inited) { lt_arec_init(&s->rec); s->rec_inited = 1; s->ed_for = -1; s->speed = 1; }
    return s;
}

/* ---- recording hooks used by the editors ---- */
int lt_feat_actions_recording(lt_app *app)
{
    return lt_arec_is_recording(&st_get(app)->rec) ? 1 : 0;
}

void lt_feat_actions_note_table(lt_app *app, const char *table, const lt_tblview *v)
{
    act_state *s = st_get(app);
    if (!lt_arec_is_recording(&s->rec) || v == NULL || v->z == NULL || v->z0 == NULL) { return; }
    (void)lt_actx_record_table(&s->rec, table, v->z, v->z0, v->rows, v->cols);
}

void lt_feat_actions_note_constant(lt_app *app, const char *name, double old_v, double new_v)
{
    act_state *s = st_get(app);
    if (!lt_arec_is_recording(&s->rec)) { return; }
    (void)lt_actx_record_constant(&s->rec, name, old_v, new_v);
}

/* ---- storage ---- */
static char *act_dir(lt_app *app)
{
    char *e = lt_plat_getenv("LT_ACTIONS_DIR");   /* scripted use: another folder for the action sets */
    if (e != NULL && e[0] != '\0') { return e; }
    lt_free(e);
    return lt_path_join(app->has_project ? app->project.path : app->config_dir, "actions");
}

static const char *cur_signature(lt_app *app)
{
    if (app->signature[0] != '\0') { return app->signature; }
    if (app->tune != NULL && app->tune->signature != NULL) { return app->tune->signature; }
    return "";
}

static void st_load(lt_app *app, act_state *s)
{
    char *dir = act_dir(app), **names = NULL;
    int n, i;
    st_clear_sets(s);
    lt_strlcpy(s->loaded_dir, dir, sizeof(s->loaded_dir));
    n = lt_plat_list_dir(dir, &names);
    for (i = 0; i < n; i++) {
        size_t l = strlen(names[i]), el = strlen(ACT_EXT);
        char *path;
        act_entry e;
        if (l <= el || strcmp(names[i] + l - el, ACT_EXT) != 0) { continue; }
        path = lt_path_join(dir, names[i]);
        memset(&e, 0, sizeof(e));
        lt_action_set_init(&e.set);
        if (lt_action_set_load(path, &e.set) == 0) {
            s->sets = (act_entry *)lt_realloc(s->sets, (s->nsets + 1) * sizeof(act_entry));
            e.file = path;
            s->sets[s->nsets++] = e;
        } else {
            lt_action_set_free(&e.set);
            lt_free(path);
        }
    }
    if (n >= 0) { lt_plat_free_list(names, n); }
    lt_free(dir);
    s->loaded = 1;
    s->ed_for = -1;
    if (s->sel >= (int)s->nsets) { s->sel = (int)s->nsets - 1; }
    if (s->sel < 0) { s->sel = 0; }
}

static void say(act_state *s, int err, const char *fmt, const char *a)
{
    lt_snprintf(s->msg, sizeof(s->msg), fmt, a);
    s->msg_err = err;
}

/* save a set into the folder under a new file name; the index of the new entry or -1 */
static int save_new(lt_app *app, act_state *s, lt_action_set *set)
{
    char *dir = act_dir(app), fname[160], *path;
    int rc;
    (void)lt_plat_mkdirs(dir);
    lt_actx_new_file(dir, set->name != NULL ? set->name : "actions", fname, sizeof(fname));
    path = lt_path_join(dir, fname);
    rc = lt_action_set_save(set, path);
    lt_free(dir);
    lt_free(path);
    if (rc != 0) { return -1; }
    st_load(app, s);
    return 0;
}

static void save_entry(act_state *s, size_t i)
{
    lt_actx_touch(&s->sets[i].set);
    if (lt_action_set_save(&s->sets[i].set, s->sets[i].file) != 0) { say(s, 1, "Cannot save: %s", lt_err_get()); }
}

/* ---- recorder ---- */
static void rec_start(lt_app *app, act_state *s)
{
    char *id;
    char *ecus[1];
    char **tags = NULL;
    size_t nt, i;
    if (s->name[0] == '\0') { say(s, 1, "%s", "Please enter a name for the action set"); return; }
    id = lt_arec_start(&s->rec, s->name, s->desc, "LibreTune");
    lt_free(id);
    nt = lt_actx_split_list(s->tags, &tags);
    for (i = 0; i < nt; i++) { (void)lt_arec_add_tag(&s->rec, tags[i]); lt_free(tags[i]); }
    lt_free(tags);
    if (s->restrict_ecu && cur_signature(app)[0] != '\0') {
        ecus[0] = (char *)cur_signature(app);
        (void)lt_arec_set_compatible_ecus(&s->rec, ecus, 1);
    }
    lt_app_toast(app, 0, "Recording actions: table and constant edits are captured");
    s->msg[0] = '\0';
}

static void rec_stop(lt_app *app, act_state *s)
{
    lt_action_set *set = lt_arec_stop(&s->rec);
    if (set == NULL) { return; }
    if (set->n == 0) {
        say(s, 0, "%s", "Recording stopped: nothing was recorded, so no action set was saved");
    } else if (save_new(app, s, set) == 0) {
        lt_snprintf(s->msg, sizeof(s->msg), "Saved action set '%s' with %lu action%s", set->name,
                    (unsigned long)set->n, set->n == 1 ? "" : "s");
        s->msg_err = 0;
        s->sel = (int)s->nsets - 1;
    } else {
        say(s, 1, "Cannot save the action set: %s", lt_err_get());
    }
    lt_action_set_free(set);
    lt_free(set);
    s->name[0] = '\0'; s->desc[0] = '\0';
}

/* ---- playback ---- */
typedef struct play_ctx { double mult; } play_ctx;

static void play_sleep(void *ud, unsigned long ms)
{
    const play_ctx *c = (const play_ctx *)ud;
    unsigned long t = (unsigned long)((double)ms / c->mult);
    if (t > 10000UL) { t = 10000UL; }
    lt_plat_sleep_ms((int)t);
}

static void set_report(act_state *s, const char *text)
{
    lt_free(s->report);
    s->report = lt_strdup(text);
}

static void do_validate(lt_app *app, act_state *s, const lt_action_set *set)
{
    char **w = NULL, **e = NULL;
    size_t nw = 0, ne = 0, i;
    lt_buf b;
    int rc = lt_action_validate(set, app->ini, &w, &nw, &e, &ne);
    lt_buf_init(&b);
    if (rc == 0 && nw == 0) { lt_buf_puts(&b, "Valid: no errors or warnings\n"); }
    for (i = 0; i < ne; i++) { lt_buf_puts(&b, "Error: "); lt_buf_puts(&b, e[i]); lt_buf_puts(&b, "\n"); }
    for (i = 0; i < nw; i++) { lt_buf_puts(&b, "Warning: "); lt_buf_puts(&b, w[i]); lt_buf_puts(&b, "\n"); }
    if (!lt_actx_compatible(set, cur_signature(app))) {
        lt_buf_puts(&b, "Warning: this set lists compatible ECUs and the open one is not among them\n");
    }
    set_report(s, (b.data != NULL ? b.data : ""));
    lt_buf_free(&b);
    lt_act_strv_free(w, nw);
    lt_act_strv_free(e, ne);
}

/* replay the set on a copy of the open tune and push the result into the open tune / ECU shadow;
 * the report text is appended to rep; 0 or a negative error */
static int play_set(lt_app *app, const lt_action_set *set, double mult, lt_buf *rep, size_t *applied)
{
    lt_tune *tmp;
    lt_replay_env env;
    lt_replay_result res;
    play_ctx pc;
    lt_pagesrc ps;
    int rc, pg;
    size_t i;
    *applied = 0;
    if (app->tune == NULL || app->ini == NULL) { lt_err_set("Open a tune first"); return LT_EINVAL; }
    if (app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        (void)lt_tune_from_mem(app->tune, app->mem);
    }
    tmp = lt_tune_clone(app->tune);
    memset(&env, 0, sizeof(env));
    pc.mult = mult > 0.0 ? mult : 1.0;
    env.ini = app->ini; env.tune = tmp; env.ud = &pc; env.sleep_ms = play_sleep;
    rc = lt_action_replay(set, &env, &res);
    if (rc != 0) {
        lt_buf_puts(rep, "Playback failed: ");
        lt_buf_puts(rep, lt_err_get());
        lt_buf_puts(rep, "\n");
    }
    lt_buf_printf(rep, "Applied %lu action%s, skipped %lu\n", (unsigned long)res.applied, res.applied == 1 ? "" : "s",
                  (unsigned long)res.skipped);
    for (i = 0; i < res.n_warnings; i++) { lt_buf_puts(rep, "Warning: "); lt_buf_puts(rep, res.warnings[i]); lt_buf_puts(rep, "\n"); }
    if (res.applied > 0) {
        /* push the replayed pages into the open tune / ECU shadow */
        ps = lt_app_pages(app);
        for (pg = 0; pg < 256; pg++) {
            if (tmp->pages[pg] != NULL && ps.write != NULL) { (void)ps.write(ps.ud, pg, 0, tmp->pages[pg], tmp->page_len[pg]); }
        }
        for (i = 0; i < set->n; i++) {
            const lt_action *a = &set->actions[i];
            const lt_tune_entry *e;
            if (a->type == LT_ACT_CONSTANT_CHANGE && a->constant_name != NULL) {
                const lt_ini_const *c = lt_ini_find_const(app->ini, a->constant_name);
                if (c != NULL && c->is_pc_var) {
                    e = lt_tune_get_pc_variable(tmp, a->constant_name);
                    if (e != NULL) { lt_tune_set_pc_variable(app->tune, a->constant_name, &e->value); }
                }
            }
        }
        (void)lt_tune_capture_from_pages(app->tune, app->ini);
        lt_app_edited(app);
    }
    *applied = res.applied;
    lt_tune_free(tmp);
    lt_replay_result_free(&res);
    return rc;
}

static void do_play(lt_app *app, act_state *s, const lt_action_set *set)
{
    static const double mults[4] = { 0.5, 1.0, 2.0, 5.0 };
    lt_buf b;
    size_t applied = 0;
    int rc;
    lt_buf_init(&b);
    rc = play_set(app, set, mults[s->speed >= 0 && s->speed < 4 ? s->speed : 1], &b, &applied);
    set_report(s, (b.data != NULL ? b.data : ""));
    lt_buf_free(&b);
    if (rc == 0) { lt_app_toast(app, 1, "Action set played: %lu applied", (unsigned long)applied); }
    else { lt_app_toast(app, 3, "Playback failed: %s", lt_err_get()); }
}

/* ---- scripting entry points (tests, automation) ---- */
int lt_feat_actions_start(lt_app *app, const char *name, const char *desc)
{
    act_state *s = st_get(app);
    if (name == NULL || name[0] == '\0') { return LT_EINVAL; }
    lt_strlcpy(s->name, name, sizeof(s->name));
    lt_strlcpy(s->desc, desc != NULL ? desc : "", sizeof(s->desc));
    s->tags[0] = '\0';
    rec_start(app, s);
    return lt_arec_is_recording(&s->rec) ? 0 : LT_EINVAL;
}

int lt_feat_actions_stop(lt_app *app, size_t *count)
{
    act_state *s = st_get(app);
    const lt_action_set *cur = lt_arec_current(&s->rec);
    if (cur == NULL) { return LT_ENOENT; }
    if (count != NULL) { *count = cur->n; }
    rec_stop(app, s);
    return s->msg_err ? LT_EIO : 0;
}

int lt_feat_actions_play_file(lt_app *app, const char *path, size_t *applied)
{
    lt_action_set set;
    lt_buf rep;
    size_t n = 0;
    int rc;
    lt_action_set_init(&set);
    rc = lt_action_set_load(path, &set);
    if (rc == 0) {
        lt_buf_init(&rep);
        rc = play_set(app, &set, 1000.0, &rep, &n);
        lt_buf_free(&rep);
    }
    if (applied != NULL) { *applied = n; }
    lt_action_set_free(&set);
    return rc;
}

/* ---- UI ---- */
static const char *set_get(void *ud, int i)
{
    const act_state *s = (const act_state *)ud;
    static char buf[200];
    lt_snprintf(buf, sizeof(buf), "%s   (%lu actions)", s->sets[i].set.name != NULL ? s->sets[i].set.name : "(unnamed)",
                (unsigned long)s->sets[i].set.n);
    return buf;
}

static const char *step_get(void *ud, int i)
{
    const lt_action_set *set = (const lt_action_set *)ud;
    static char buf[260], d[200];
    lt_actx_describe(&set->actions[i], d, sizeof(d));
    lt_snprintf(buf, sizeof(buf), "%3d  %s", i + 1, d);
    return buf;
}

static void draw_recorder(lt_app *app, act_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[3];
    char b[200];
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Record actions", t->text, LT_UI_ALIGN_LEFT);
    if (!lt_arec_is_recording(&s->rec)) {
        w[0] = 130; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Name:");
        (void)lt_ui_text(ui, "actname", s->name, sizeof(s->name), 0);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Description:");
        (void)lt_ui_text(ui, "actdesc", s->desc, sizeof(s->desc), 0);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Tags (comma):");
        (void)lt_ui_text(ui, "acttags", s->tags, sizeof(s->tags), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "Only for this ECU (%s)", cur_signature(app)[0] != '\0' ? cur_signature(app) : "unknown");
        (void)lt_ui_checkbox(ui, b, &s->restrict_ecu);
        w[0] = 170; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        if (lt_ui_button_ex(ui, "Start Recording", s->name[0] != '\0' ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && s->name[0] != '\0') {
            rec_start(app, s);
        }
        lt_ui_label_ex(ui, "Edits to tables and dialog constants are recorded while recording.", t->muted, LT_UI_ALIGN_LEFT);
    } else {
        const lt_action_set *cur = lt_arec_current(&s->rec);
        w[0] = 170; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        if (lt_ui_button_ex(ui, "Stop Recording", LT_UI_BTN_DANGER)) { rec_stop(app, s); }
        lt_snprintf(b, sizeof(b), "Recording: %s   %lu action%s so far - edit tables or constants, then stop.",
                    cur != NULL && cur->name != NULL ? cur->name : "", (unsigned long)(cur != NULL ? cur->n : 0),
                    cur != NULL && cur->n == 1 ? "" : "s");
        lt_ui_label_ex(ui, b, t->error, LT_UI_ALIGN_LEFT);
    }
}

static void draw_manager(lt_app *app, act_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[5], cell = lt_ui_cell_mode(ui), have;
    lt_action_set *set = NULL;
    char b[300];
    static const char *const speeds[4] = { "0.5x", "1x", "2x", "5x" };
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(b, sizeof(b), "Action sets (%lu)", (unsigned long)s->nsets);
    lt_ui_label_ex(ui, b, t->text, LT_UI_ALIGN_LEFT);
    if (s->nsets == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "No action sets yet. Record one or import a .ltaction file.", t->muted, LT_UI_ALIGN_LEFT);
    } else {
        lt_ui_layout_row(ui, 1, &w1, cell ? 48 : 66);
        (void)lt_ui_listbox_cb(ui, "actsets", &s->sel, (int)s->nsets, set_get, s);
        if (s->sel >= (int)s->nsets) { s->sel = (int)s->nsets - 1; }
        if (s->sel >= 0) { set = &s->sets[s->sel].set; }
    }
    have = set != NULL;
    if (have && s->ed_for != s->sel) {
        char tl[160], el[200];
        lt_actx_join_list(set->meta.tags, set->meta.ntags, tl, sizeof(tl));
        lt_actx_join_list(set->meta.compatible_ecus, set->meta.necus, el, sizeof(el));
        lt_strlcpy(s->ed_desc, set->description != NULL ? set->description : "", sizeof(s->ed_desc));
        lt_strlcpy(s->ed_tags, tl, sizeof(s->ed_tags));
        lt_strlcpy(s->ed_ecus, el, sizeof(s->ed_ecus));
        s->ed_for = s->sel; s->step = 0;
        lt_free(s->report); s->report = NULL;
    }
    if (have) {
        w[0] = 130; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Description:");
        (void)lt_ui_text(ui, "edesc", s->ed_desc, sizeof(s->ed_desc), 0);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Tags:");
        (void)lt_ui_text(ui, "etags", s->ed_tags, sizeof(s->ed_tags), 0);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Compatible ECUs:");
        (void)lt_ui_text(ui, "eecus", s->ed_ecus, sizeof(s->ed_ecus), 0);
        w[0] = 130; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "Info:");
        lt_snprintf(b, sizeof(b), "by %s, %.10s, %lu actions%s", set->meta.created_by != NULL ? set->meta.created_by : "?",
                    set->meta.created_at != NULL ? set->meta.created_at : "", (unsigned long)set->n,
                    lt_actx_compatible(set, cur_signature(app)) ? "" : "   (not for the open ECU)");
        lt_ui_label_ex(ui, b, lt_actx_compatible(set, cur_signature(app)) ? t->text2 : t->warning, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, cell ? 64 : 100);
        if (set->n > 0) {
            (void)lt_ui_listbox_cb(ui, "actsteps", &s->step, (int)set->n, step_get, set);
        } else {
            lt_ui_label_ex(ui, "This set has no actions.", t->muted, LT_UI_ALIGN_LEFT);
        }
        w[0] = 120; w[1] = 120; w[2] = 150; w[3] = -1;
        lt_ui_layout_row(ui, 4, w, 0);
        if (lt_ui_button_ex(ui, "Remove step", set->n > 0 ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && set->n > 0 &&
            s->step >= 0 && s->step < (int)set->n) {
            (void)lt_actx_remove(set, (size_t)s->step);
            save_entry(s, (size_t)s->sel);
            if (s->step >= (int)set->n) { s->step = (int)set->n - 1; }
            if (s->step < 0) { s->step = 0; }
        }
        if (lt_ui_button(ui, "+ Pause 500 ms")) {
            lt_action a;
            lt_action_init(&a, LT_ACT_PAUSE);
            lt_action_pause(&a, 500UL);
            lt_actx_append(set, &a);
            lt_action_clear(&a);
            save_entry(s, (size_t)s->sel);
        }
        lt_ui_label_ex(ui, "Lua step (stored, not run):", t->text2, LT_UI_ALIGN_RIGHT);
        w[0] = -1; w[1] = 130;
        lt_ui_layout_row(ui, 2, w, 0);
        (void)lt_ui_text(ui, "actlua", s->lua, sizeof(s->lua), 0);
        if (lt_ui_button_ex(ui, "+ Lua script", s->lua[0] != '\0' ? 0 : LT_UI_BTN_DISABLED) && s->lua[0] != '\0') {
            lt_action a;
            lt_action_init(&a, LT_ACT_LUA);
            lt_action_lua(&a, s->lua, "Custom calculation");
            lt_actx_append(set, &a);
            lt_action_clear(&a);
            s->lua[0] = '\0';
            save_entry(s, (size_t)s->sel);
        }
        if (s->report != NULL) {
            const char *p = s->report;
            int shown = 0;
            while (*p != '\0' && shown++ < 4) {
                const char *e = strchr(p, '\n');
                size_t l = e != NULL ? (size_t)(e - p) : strlen(p);
                char line[300];
                if (l >= sizeof(line)) { l = sizeof(line) - 1; }
                memcpy(line, p, l);
                line[l] = '\0';
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, line, strncmp(line, "Error", 5) == 0 || strncmp(line, "Playback failed", 15) == 0 ? t->error :
                               (strncmp(line, "Warning", 7) == 0 ? t->warning : t->text2), LT_UI_ALIGN_LEFT);
                p += l;
                if (*p == '\n') { p++; }
            }
        }
    }
    if (s->msg[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
    }
    w[0] = 150; w[1] = 90; w[2] = 90; w[3] = -1; w[4] = 100;
    lt_ui_layout_row(ui, 5, w, 0);
    if (lt_ui_button_ex(ui, "Save changes", have ? 0 : LT_UI_BTN_DISABLED) && have) {
        lt_free(set->description);
        set->description = lt_strdup(s->ed_desc);
        lt_actx_set_tags(set, s->ed_tags);
        lt_actx_set_ecus(set, s->ed_ecus);
        save_entry(s, (size_t)s->sel);
        say(s, 0, "%s", "Saved");
    }
    if (lt_ui_button_ex(ui, "Validate", have ? 0 : LT_UI_BTN_DISABLED) && have) { do_validate(app, s, set); }
    (void)lt_ui_combo(ui, "actspeed", &s->speed, speeds, 4);
    lt_ui_label_ex(ui, "playback speed (pauses)", t->muted, LT_UI_ALIGN_LEFT);
    if (lt_ui_button_ex(ui, "Play", have && app->ini != NULL ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && have && app->ini != NULL) {
        s->confirm_play = 1;
    }
    w[0] = 150; w[1] = 150; w[2] = 150; w[3] = -1; w[4] = 110;
    lt_ui_layout_row(ui, 5, w, 0);
    if (lt_ui_button(ui, "Import .ltaction...")) {
        lt_app_file_dialog(app, FD_ACT_IMPORT, LT_UI_FILE_OPEN, NULL, "", ACT_FILTERS);
    }
    if (lt_ui_button_ex(ui, "Export...", have ? 0 : LT_UI_BTN_DISABLED) && have) {
        char nm[160];
        lt_snprintf(nm, sizeof(nm), "%s%s", set->name != NULL ? set->name : "actions", ACT_EXT);
        lt_app_file_dialog(app, FD_ACT_EXPORT, LT_UI_FILE_SAVE, NULL, nm, ACT_FILTERS);
    }
    if (lt_ui_button_ex(ui, "Delete set", have ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && have) { s->confirm_del = 1; }
    lt_ui_label(ui, "");
    if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { s->close_req = 1; }
}

static void act_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    act_state *s = st_get(app);
    int open = d->open;
    const char *picked;
    char *dir = act_dir(app);
    if (!s->loaded || strcmp(dir, s->loaded_dir) != 0) { st_load(app, s); }
    lt_free(dir);
    picked = lt_app_file_result(app, FD_ACT_IMPORT);
    if (picked != NULL) {
        lt_action_set in;
        lt_action_set_init(&in);
        if (lt_action_set_load(picked, &in) == 0) {
            if (in.name == NULL || in.name[0] == '\0') { lt_free(in.name); in.name = lt_strdup("Imported"); }
            if (save_new(app, s, &in) == 0) {
                lt_snprintf(s->msg, sizeof(s->msg), "Imported '%s' (%lu actions)", in.name, (unsigned long)in.n);
                s->msg_err = 0;
                s->sel = (int)s->nsets - 1;
            } else { say(s, 1, "Cannot save the imported set: %s", lt_err_get()); }
        } else { say(s, 1, "Cannot import: %s", lt_err_get()); }
        lt_action_set_free(&in);
    }
    picked = lt_app_file_result(app, FD_ACT_EXPORT);
    if (picked != NULL && s->sel >= 0 && s->sel < (int)s->nsets) {
        if (lt_action_set_save(&s->sets[s->sel].set, picked) == 0) { say(s, 0, "Exported to %s", picked); }
        else { say(s, 1, "Cannot export: %s", lt_err_get()); }
    }
    if (lt_ui_modal_begin(ui, "Action Recorder", 880, 720, &open)) {
        draw_recorder(app, s);
        draw_manager(app, s);
        if (s->close_req) { open = 0; s->close_req = 0; }
        lt_ui_modal_end(ui);
    }
    if (s->confirm_play && s->sel >= 0 && s->sel < (int)s->nsets) {
        char m[240];
        int r;
        lt_snprintf(m, sizeof(m), "Apply the %lu actions of '%s' to the open tune? Edits are not undoable.",
                    (unsigned long)s->sets[s->sel].set.n, s->sets[s->sel].set.name);
        r = lt_ui_msgbox(ui, "actplay", "Play Action Set", m, "Play|Cancel", &s->confirm_play);
        if (r == 1) { do_play(app, s, &s->sets[s->sel].set); }
    } else { s->confirm_play = 0; }
    if (s->confirm_del && s->sel >= 0 && s->sel < (int)s->nsets) {
        int r = lt_ui_msgbox(ui, "actdel", "Delete Action Set", "Delete this action set file? This cannot be undone.",
                             "Delete|Cancel", &s->confirm_del);
        if (r == 1) {
            (void)lt_plat_remove(s->sets[s->sel].file);
            st_load(app, s);
        }
    } else { s->confirm_del = 0; }
    d->open = open;
}

static void act_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    d->data = NULL;
}

static void act_open(lt_app *app)
{
    (void)lt_app_dialog_push(app, "actions", "", "Action Recorder");
}

void lt_feat_actions_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "actions"; dk.draw = act_draw; dk.close = act_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Action Recorder...", NULL, act_open, lt_ext_pred_ini);
}
