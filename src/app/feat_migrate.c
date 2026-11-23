/* feat_migrate.c - change a project's INI and the "INI Version Migration" report. (G-12)
 *   File -> Update Project INI...   pick a definition (INI library or file), review the migration
 *                                   report against the project tune, then switch the project to it
 *   Tools -> Tune Migration Report  report of the open tune against the open definition
 *   lt_feat_migrate_show            the same dialog for any tune file vs INI report (base tunes),
 *                                   with a continuation run by "Continue with Tune"
 * The update re-resolves the tune: every constant keeps its value by name, constants new in the
 * INI get their defaults, the tune is re-stamped with the new INI version and saved, then the
 * project is reopened (the ECU connection is closed).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../project/inilib.h"

#define FD_MIG_PICK (LT_APP_FD_USER0 + 42)
#define MIG_LIST_MAX 20

typedef struct mig_dlg {
    int  update;           /* update flow (else: report of the open tune) */
    int  stage;            /* update flow: 0 choose definition, 1 report */
    lt_migration_report rep;
    int  has_changes;
    char summary[160];
    lt_ini_repo repo; int repo_ok;
    int  source;           /* 0 library, 1 file */
    int  repo_sel;
    char file[512];
    char target[512];      /* chosen INI path (stage 1) */
    char err[200];
    int  ext;              /* report handed over by lt_feat_migrate_show (a tune file vs an INI) */
    char intro[600];
    lt_ext_action_fn cont;
} mig_dlg;

typedef struct mig_state {
    int  pending;          /* deferred update, run from the tick (it replaces the whole session) */
    char path[512];
    lt_migration_report ext;   /* lt_feat_migrate_show: waiting for its dialog to open */
    int  has_ext;
    char ext_intro[600];
    lt_ext_action_fn ext_cont;
    char load[512];        /* lt_feat_load_tune_checked: the tune file the report is about */
    int  load_pending;     /* "Continue with Tune" pressed: load it from the tick */
} mig_state;

static void mig_state_free(lt_app *app, void *p)
{
    mig_state *s = (mig_state *)p;
    LT_UNUSED(app);
    if (s->has_ext) { lt_migration_clear(&s->ext); }
    s->has_ext = 0;
}

static mig_state *mig_st(lt_app *app)
{
    return (mig_state *)lt_ext_state(app, "migrate", sizeof(mig_state), mig_state_free);
}

void lt_feat_migrate_show(lt_app *app, const char *title, const char *intro, lt_migration_report *rep,
                          lt_ext_action_fn on_continue)
{
    mig_state *s = mig_st(app);
    if (s->has_ext) { lt_migration_clear(&s->ext); }
    s->ext = *rep;                 /* ownership moves into the state, then into the dialog */
    lt_migration_init(rep);
    s->has_ext = 1;
    lt_strlcpy(s->ext_intro, intro != NULL ? intro : "", sizeof(s->ext_intro));
    s->ext_cont = on_continue;
    (void)lt_app_dialog_push(app, "migration", "external", title != NULL ? title : "Tune Migration Report");
}

/* ---- Load Tune from a file, checked against the open definition ---- */
static void load_cont(lt_app *app)
{
    mig_state *s = mig_st(app);
    if (s->load[0] != '\0') { s->load_pending = 1; }     /* loaded from the tick, not the dialog */
}

int lt_feat_load_tune_checked(lt_app *app, const char *path)
{
    mig_state *s = mig_st(app);
    lt_migration_report rep;
    lt_tune *t;
    const char *isig;
    char intro[600], tsig[128];
    int same;
    if (app->ini == NULL || path == NULL) { return -1; }
    t = lt_tune_load(path);
    if (t == NULL) { lt_app_toast(app, 3, "Cannot load tune: %s", lt_err_get()); return -1; }
    isig = app->ini->signature != NULL ? app->ini->signature : "";
    lt_strlcpy(tsig, t->signature != NULL ? t->signature : "", sizeof(tsig));   /* t is freed below */
    same = tsig[0] == '\0' || lt_sig_compare(tsig, isig, NULL) == LT_SIG_EXACT;
    lt_migration_init(&rep);
    lt_migration_compare_tune(&rep, t, app->ini);
    if (same && rep.n_type == 0 && rep.n_missing_in_ini == 0) {
        lt_migration_clear(&rep);
        return lt_app_load_tune_mem(app, t, lt_path_base(path));
    }
    lt_tune_free(t);
    lt_strlcpy(s->load, path, sizeof(s->load));
    s->load_pending = 0;
    if (same) {
        lt_snprintf(intro, sizeof(intro), "\"%s\" matches this ECU definition, but some of its values do "
                    "not map onto it. They are listed below. Continue only if that is expected.",
                    lt_path_base(path));
    } else {
        lt_snprintf(intro, sizeof(intro), "\"%s\" was saved by firmware %s, but this project's ECU "
                    "definition is %s. Values are matched by name; the lists below show what does not "
                    "map. A tune from another ECU can make the engine run badly: check every page "
                    "before burning.", lt_path_base(path), tsig, isig[0] != '\0' ? isig : "(unknown)");
    }
    lt_feat_migrate_show(app, "Load Tune: Different ECU Definition", intro, &rep, load_cont);
    return 1;
}

/* ---- the update itself ---- */
int lt_feat_migrate_apply(lt_app *app, const char *new_ini)
{
    lt_ini *ini;
    lt_tune *d;
    char *dir;
    int pg, rc;
    if (!app->has_project || app->tune == NULL || app->ini == NULL) { return -1; }
    ini = lt_ini_load_file(new_ini, NULL);
    if (ini == NULL) { lt_app_toast(app, 3, "Failed to parse INI file: %s", lt_err_get()); return -1; }
    /* current values out of the ECU shadow into the tune, then the old tune to disk */
    if (app->mem != NULL) { (void)lt_tune_from_mem(app->tune, app->mem); }
    (void)lt_tune_capture_from_pages(app->tune, app->ini);
    if (lt_project_save_tune(&app->project) < 0) {
        lt_app_toast(app, 3, "Cannot save the tune: %s", lt_err_get());
        lt_ini_free(ini);
        return -1;
    }
    rc = lt_project_set_ini(&app->project, new_ini);
    if (rc < 0) {
        lt_app_toast(app, 3, "Failed to update project INI: %s", lt_err_get());
        lt_ini_free(ini);
        return rc;
    }
    /* re-resolve: fresh pages of the new layout with defaults, named values written over them,
     * then everything decoded again (new constants take defaults) and stamped with the new INI */
    d = lt_tune_from_defaults(ini);
    for (pg = 0; pg < 256; pg++) {
        if (d->pages[pg] != NULL) { lt_tune_set_page(app->tune, pg, d->pages[pg], d->page_len[pg]); }
    }
    lt_tune_free(d);
    (void)lt_tune_apply_to_pages(app->tune, ini);
    (void)lt_tune_capture_from_pages(app->tune, ini);
    lt_free(app->tune->signature);
    app->tune->signature = lt_strdup(ini->signature != NULL ? ini->signature : "");
    lt_tune_touch(app->tune);
    lt_ini_free(ini);
    if (lt_project_save_tune(&app->project) < 0) {
        lt_app_toast(app, 3, "Cannot save the tune: %s", lt_err_get());
        return -1;
    }
    dir = lt_strdup(app->project.path);
    rc = lt_app_open_project(app, dir);
    lt_free(dir);
    if (rc == 0) { lt_app_toast(app, 1, "Project INI updated. Reconnect to the ECU to continue."); }
    return rc;
}

static void mig_tick(lt_app *app)
{
    mig_state *s = mig_st(app);
    if (s->load_pending) {
        char path[512];
        lt_strlcpy(path, s->load, sizeof(path));
        s->load_pending = 0;
        (void)lt_app_load_tune(app, path);
    }
    if (s->pending) {
        char path[512];
        lt_strlcpy(path, s->path, sizeof(path));
        s->pending = 0;
        (void)lt_feat_migrate_apply(app, path);
    }
}

/* ---- report ---- */
static void rep_build(lt_app *app, mig_dlg *s, const lt_ini *ini)
{
    char *sm;
    lt_migration_clear(&s->rep);
    lt_migration_init(&s->rep);
    if (app->tune != NULL && app->tune->has_manifest && ini != NULL) {
        lt_migration_compare(&s->rep, app->tune->manifest, app->tune->n_manifest, ini);
    }
    s->has_changes = lt_migration_has_changes(&s->rep);
    sm = lt_migration_summary(&s->rep);
    lt_strlcpy(s->summary, sm != NULL ? sm : "", sizeof(s->summary));
    lt_free(sm);
}

static int stage_report(lt_app *app, mig_dlg *s, const char *path)
{
    lt_ini *ini = lt_ini_load_file(path, NULL);
    if (ini == NULL) {
        lt_snprintf(s->err, sizeof(s->err), "Failed to parse INI file: %s", lt_err_get());
        return -1;
    }
    rep_build(app, s, ini);
    lt_ini_free(ini);
    lt_strlcpy(s->target, path, sizeof(s->target));
    s->err[0] = '\0';
    s->stage = 1;
    return 0;
}

static void change_row(lt_ui *ui, const char *name, const char *detail)
{
    int w[2];
    const lt_theme *t = lt_ui_theme(ui);
    w[0] = 280; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label_ex(ui, name, t->text, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, detail, t->muted, LT_UI_ALIGN_LEFT);
}

static void names_section(lt_ui *ui, const char *title, char **names, size_t n)
{
    char buf[160];
    size_t i;
    int w1 = -1;
    if (n == 0) { return; }
    if (lt_ui_tree_node(ui, title, -1, 0) & LT_UI_TREE_OPEN) {
        for (i = 0; i < n && i < MIG_LIST_MAX; i++) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, names[i], lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
        }
        if (n > MIG_LIST_MAX) {
            lt_snprintf(buf, sizeof(buf), "...and %lu more", (unsigned long)(n - MIG_LIST_MAX));
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, buf, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_tree_end(ui);
    }
}

static void change_section(lt_ui *ui, const char *title, int kind, const lt_constant_change *c, size_t n,
                           int open)
{
    char det[200];
    size_t i;
    if (n == 0) { return; }
    if (lt_ui_tree_node(ui, title, -1, open ? LT_UI_TREE_DEFAULT_OPEN : 0) & LT_UI_TREE_OPEN) {
        for (i = 0; i < n; i++) {
            if (kind == 0) {
                lt_snprintf(det, sizeof(det), "%s \xE2\x86\x92 %s", c[i].old_type != NULL ? c[i].old_type : "?",
                            c[i].new_type != NULL ? c[i].new_type : "?");
            } else if (kind == 3) {
                lt_snprintf(det, sizeof(det), "%s \xE2\x86\x92 %s: %s", c[i].old_type != NULL ? c[i].old_type : "?",
                            c[i].new_type != NULL ? c[i].new_type : "?",
                            c[i].description != NULL ? c[i].description : "");
            } else if (kind == 1) {
                lt_snprintf(det, sizeof(det), "scale: %.4f \xE2\x86\x92 %.4f", c[i].old_scale, c[i].new_scale);
            } else {
                lt_snprintf(det, sizeof(det), "page %d/offset %lu \xE2\x86\x92 page %d/offset %lu",
                            (int)c[i].old_page, (unsigned long)c[i].old_offset, (int)c[i].new_page,
                            (unsigned long)c[i].new_offset);
            }
            change_row(ui, c[i].name != NULL ? c[i].name : "", det);
        }
        lt_ui_tree_end(ui);
    }
}

static const char *severity_text(const char *sev)
{
    if (strcmp(sev, "high") == 0) { return "Significant Changes Detected"; }
    if (strcmp(sev, "medium") == 0) { return "Moderate Changes Detected"; }
    if (strcmp(sev, "low") == 0) { return "Minor Changes Detected"; }
    return "No Changes";
}

/* report of a tune file against an INI (lt_feat_migrate_show): what does not map */
static void report_body_ext(lt_app *app, mig_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_migration_report *r = &s->rep;
    char buf[160];
    int w1 = -1;
    if (s->intro[0] != '\0') {
        lt_ui_label_wrap(ui, s->intro, t->text2);
        lt_ui_layout_row(ui, 1, &w1, 0);
    }
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
    if (!s->has_changes) {
        lt_ui_label_ex(ui, "Every value of the tune maps onto this ECU definition", t->success, LT_UI_ALIGN_LEFT);
        lt_ui_text_style_reset(ui);
        return;
    }
    lt_ui_label_ex(ui, r->n_type > 0 || r->n_missing_in_ini > 0 ? "Some values do not map" : "Minor differences",
                   r->n_type > 0 || r->n_missing_in_ini > 0 ? t->warning : t->info, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, s->summary, t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_snprintf(buf, sizeof(buf), "Values that do not fit (%lu)  -  Kept at the INI default",
                (unsigned long)r->n_type);
    lt_ui_layout_row(ui, 1, &w1, 0);
    change_section(ui, buf, 3, r->type_changed, r->n_type, 1);
    lt_snprintf(buf, sizeof(buf), "Not in this INI (%lu)  -  Not Applied", (unsigned long)r->n_missing_in_ini);
    lt_ui_layout_row(ui, 1, &w1, 0);
    names_section(ui, buf, r->missing_in_ini, r->n_missing_in_ini);
    lt_snprintf(buf, sizeof(buf), "Not in the tune (%lu)  -  Using INI Defaults", (unsigned long)r->n_missing_in_tune);
    lt_ui_layout_row(ui, 1, &w1, 0);
    names_section(ui, buf, r->missing_in_tune, r->n_missing_in_tune);
}

static void report_body(lt_app *app, mig_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_migration_report *r = &s->rep;
    char buf[160];
    int w1 = -1;
    lt_color c;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->ext) {
        report_body_ext(app, s);
        return;
    }
    if (!s->has_changes) {
        lt_ui_label_ex(ui, "No migration report available.", t->text, LT_UI_ALIGN_CENTER);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "This tune was either created with the current INI version, or is a pre-1.1 "
                             "format tune that doesn't include version tracking.", t->muted);
        return;
    }
    c = strcmp(r->severity, "high") == 0 ? t->error : strcmp(r->severity, "medium") == 0 ? t->warning : t->info;
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
    lt_ui_label_ex(ui, severity_text(r->severity), c, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "This tune was created with a different INI version. Some constants may have changed.",
                     t->text2);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, s->summary, t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_snprintf(buf, sizeof(buf), "Type Changed (%lu)  -  Review Required", (unsigned long)r->n_type);
    lt_ui_layout_row(ui, 1, &w1, 0);
    change_section(ui, buf, 0, r->type_changed, r->n_type, 1);
    lt_snprintf(buf, sizeof(buf), "Scale/Offset Changed (%lu)  -  May Affect Values", (unsigned long)r->n_scale);
    lt_ui_layout_row(ui, 1, &w1, 0);
    change_section(ui, buf, 1, r->scale_changed, r->n_scale, 0);
    lt_snprintf(buf, sizeof(buf), "Storage Location Changed (%lu)  -  Handled Automatically",
                (unsigned long)r->n_loc);
    lt_ui_layout_row(ui, 1, &w1, 0);
    change_section(ui, buf, 2, r->location_changed, r->n_loc, 0);
    lt_snprintf(buf, sizeof(buf), "Removed from INI (%lu)  -  Values Preserved", (unsigned long)r->n_missing_in_ini);
    lt_ui_layout_row(ui, 1, &w1, 0);
    names_section(ui, buf, r->missing_in_ini, r->n_missing_in_ini);
    lt_snprintf(buf, sizeof(buf), "New in INI (%lu)  -  Using Defaults", (unsigned long)r->n_missing_in_tune);
    lt_ui_layout_row(ui, 1, &w1, 0);
    names_section(ui, buf, r->missing_in_tune, r->n_missing_in_tune);
}

/* ---- dialog ---- */
static const char *repo_get(void *ud, int i)
{
    mig_dlg *s = (mig_dlg *)ud;
    static char buf[300];
    lt_snprintf(buf, sizeof(buf), "%s   [%s]", s->repo.e[i].name != NULL ? s->repo.e[i].name : s->repo.e[i].id,
                s->repo.e[i].signature != NULL ? s->repo.e[i].signature : "");
    return buf;
}

static void mig_init(lt_app *app, lt_app_dialog *d)
{
    mig_dlg *s = (mig_dlg *)lt_calloc(1, sizeof(*s));
    d->data = s;
    lt_migration_init(&s->rep);
    s->update = strcmp(d->target, "update") == 0;
    if (strcmp(d->target, "external") == 0) {
        mig_state *st = mig_st(app);
        s->ext = 1;
        if (st->has_ext) {
            s->rep = st->ext;            /* take ownership */
            lt_migration_init(&st->ext);
            st->has_ext = 0;
        }
        lt_strlcpy(s->intro, st->ext_intro, sizeof(s->intro));
        s->cont = st->ext_cont;
        st->ext_cont = NULL;
        s->has_changes = lt_migration_has_changes(&s->rep);
        lt_snprintf(s->summary, sizeof(s->summary), "%lu values do not fit, %lu not in this INI, "
                    "%lu INI constants not in the tune", (unsigned long)s->rep.n_type,
                    (unsigned long)s->rep.n_missing_in_ini, (unsigned long)s->rep.n_missing_in_tune);
    } else if (s->update) {
        s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
        if (!s->repo_ok || s->repo.n == 0) { s->source = 1; }
    } else {
        rep_build(app, s, app->ini);
    }
}

static void stage_pick(lt_app *app, lt_app_dialog *d, mig_dlg *s, int *open)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w2[2], ok;
    char line[700], *p;
    const char *picked = lt_app_file_result(app, FD_MIG_PICK);
    LT_UNUSED(d);
    if (picked != NULL) { lt_strlcpy(s->file, picked, sizeof(s->file)); s->source = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(line, sizeof(line), "Current INI: %s  [%s]", app->ini_path,
                app->project.cfg.signature != NULL ? app->project.cfg.signature : "");
    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "Switch this project to another ECU definition. Tune values are kept by name; "
                         "constants new in the INI use their defaults.", t->muted);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_radio(ui, "From the INI library", &s->source, 0);
    lt_ui_radio(ui, "From an INI file", &s->source, 1);
    if (s->source == 0) {
        lt_ui_layout_row(ui, 1, &w1, 200);
        if (s->repo_ok && s->repo.n > 0) {
            lt_ui_listbox_cb(ui, "migrepo", &s->repo_sel, (int)s->repo.n, repo_get, s);
        } else {
            lt_ui_label_ex(ui, "The INI library is empty. Import an INI file in Settings > ECU Definitions.",
                           t->muted, LT_UI_ALIGN_LEFT);
        }
    } else {
        w2[0] = 120; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Browse...")) {
            lt_app_file_dialog(app, FD_MIG_PICK, LT_UI_FILE_OPEN, NULL, "", "INI Files|*.ini|All Files|*");
        }
        lt_ui_text(ui, "migfile", s->file, sizeof(s->file), 0);
    }
    if (s->err[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->err, t->error, LT_UI_ALIGN_LEFT);
    }
    ok = s->source == 1 ? s->file[0] != '\0' : (s->repo_ok && s->repo.n > 0);
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button_ex(ui, "Review Changes", ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && ok) {
        if (s->source == 1) {
            (void)stage_report(app, s, s->file);
        } else {
            p = lt_ini_repo_path(&s->repo, s->repo.e[s->repo_sel].id);
            if (p != NULL) { (void)stage_report(app, s, p); }
            lt_free(p);
        }
    }
    if (lt_ui_button(ui, "Cancel")) { *open = 0; }
}

static void mig_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    mig_dlg *s = (mig_dlg *)d->data;
    int open = d->open, w1 = -1, wf[3], fh;
    if (s == NULL) { mig_init(app, d); s = (mig_dlg *)d->data; }
    if (lt_ui_modal_begin(ui, s->update && s->stage == 0 ? "Update Project INI"
                              : s->ext ? d->title : "INI Version Migration", 760, 540, &open)) {
        fh = lt_ui_cell_mode(ui) ? 16 : 30;
        if (s->update && s->stage == 0) {
            stage_pick(app, d, s, &open);
        } else {
            lt_ui_layout_row(ui, 1, &w1, -(fh + 8));
            if (lt_ui_scroll_begin(ui, "migbody")) {
                if (s->update) {
                    char line[700];
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_snprintf(line, sizeof(line), "New INI: %s", s->target);
                    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
                }
                report_body(app, s);
                lt_ui_scroll_end(ui);
            }
            wf[0] = -1; wf[1] = 110; wf[2] = 170;
            lt_ui_layout_row(ui, 3, wf, 0);
            if (s->ext && s->cont != NULL) {
                lt_ui_label_ex(ui, "Check the values listed above after loading", t->warning, LT_UI_ALIGN_LEFT);
            } else if (s->rep.requires_user_review) {
                lt_ui_label_ex(ui, "Review type changes before burning to ECU", t->warning, LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_label(ui, "");
            }
            if (lt_ui_button(ui, s->ext && s->cont != NULL ? "Cancel" : "Dismiss")) { open = 0; }
            if (lt_ui_button_ex(ui, "Continue with Tune", LT_UI_BTN_DEFAULT)) {
                if (s->update) {
                    mig_state *ms = mig_st(app);
                    lt_strlcpy(ms->path, s->target, sizeof(ms->path));
                    ms->pending = 1;
                }
                if (s->ext && s->cont != NULL) { s->cont(app); }
                open = 0;
            }
        }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void mig_close(lt_app *app, lt_app_dialog *d)
{
    mig_dlg *s = (mig_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        lt_migration_clear(&s->rep);
        if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
        lt_free(s);
    }
    d->data = NULL;
}

static void act_update(lt_app *app) { (void)lt_app_dialog_push(app, "migration", "update", "Update Project INI"); }
static void act_report(lt_app *app) { (void)lt_app_dialog_push(app, "migration", "", "INI Version Migration"); }

void lt_feat_migrate_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "migration"; dk.draw = mig_draw; dk.close = mig_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Update Project &INI...", NULL, act_update, lt_ext_pred_project);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Tune &Migration Report...", NULL, act_report, lt_ext_pred_ini);
    lt_ext_add_tick(reg, mig_tick);
}
