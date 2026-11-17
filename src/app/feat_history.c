/* feat_history.c - Tune History (G-10): the version history of a project's tune with branches,
 * checkpoints, per-version changes and restore.  The original keeps this in git; we use the
 * snapshot store of project/snap.c (snapshots + branches).
 *   File -> Tune History...        dialog "history" (timeline, branch bar, checkpoint, auto-commit)
 *   dialog "history_diff"          constants / files changed in one version
 *   dialog "history_branch"        new branch name
 *   dialog "history_commit"        message + note; target "save" = asked after a save
 *   Settings -> Version Control    auto-commit on save (never/always/ask), message format
 *   on save                        auto-commit according to the setting
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../plat/plat.h"

#define CH 8
#define CV 16
#define HIST_MAX 50

typedef struct hist_state { int guard; } hist_state;

static hist_state *hs(lt_app *app)
{
    return (hist_state *)lt_ext_state(app, "history", sizeof(hist_state), NULL);
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
static const char *active_table(lt_app *app)
{
    if (app->active_tab >= 0 && app->active_tab < app->n_tabs) {
        const lt_app_tab *t = app->tabs[app->active_tab];
        if (strcmp(t->kind, "table") == 0 || strcmp(t->kind, "curve") == 0) { return t->target; }
    }
    return "";
}

void lt_feat_history_default_message(lt_app *app, char *out, size_t cap)
{
    char stamp[32], *m;
    lt_plat_timestamp(stamp, sizeof(stamp));
    m = lt_snap_expand_format(app->settings.commit_fmt, active_table(app), stamp);
    lt_strlcpy(out, m, cap);
    lt_free(m);
}

int lt_feat_history_commit(lt_app *app, const char *message, const char *note, char id_out[9])
{
    int rc;
    char id[9];
    if (!app->has_project) { lt_err_set("no project open"); return -1; }
    if (app->modified && app->ini != NULL) {
        /* the version records what is on disk: save first, without triggering the auto-commit */
        hist_state *h = hs(app);
        h->guard = 1;
        (void)lt_app_save_tune(app);
        h->guard = 0;
    }
    rc = lt_snap_commit(app->project.path, message, note, "", id);
    if (rc == 0 && id_out != NULL) { memcpy(id_out, id, 9); }
    return rc;
}

/* reload the tune after the files of the project were replaced by a snapshot */
static void reload_tune(lt_app *app)
{
    char *p = lt_project_file(&app->project, "CurrentTune.msq");
    if (p != NULL && lt_file_exists(p)) {
        if (lt_app_load_tune(app, p) == 0) { app->modified = 1; }
    }
    lt_free(p);
}

int lt_feat_history_restore(lt_app *app, const char *id)
{
    int rc = lt_snap_restore(app->project.path, id, LT_TRUE);
    if (rc == 0) { reload_tune(app); }
    return rc;
}

int lt_feat_history_switch(lt_app *app, const char *branch)
{
    int rc = lt_snap_branch_switch(app->project.path, branch, LT_TRUE);
    if (rc == 0) { reload_tune(app); }
    return rc;
}

/* auto-commit after a save, per the setting */
static void on_save(lt_app *app)
{
    char msg[200];
    if (!app->has_project || hs(app)->guard) { return; }
    if (app->settings.auto_commit == 0) { return; }
    if (!lt_snap_enabled(app->project.path)) { return; }
    if (lt_snap_has_changes(app->project.path) <= 0) { return; }
    if (app->settings.auto_commit == 2) {
        (void)lt_app_dialog_push(app, "history_commit", "save", "Commit Tune Version");
        return;
    }
    lt_feat_history_default_message(app, msg, sizeof(msg));
    if (lt_feat_history_commit(app, msg, NULL, NULL) < 0) {
        lt_app_toast(app, 2, "Tune history commit failed: %s", lt_err_get());
    }
}

/* ------------------------------------------------------------------ */
/* commit dialog                                                       */
typedef struct commit_dlg { char msg[200], note[300]; int ask; } commit_dlg;

static void commit_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    commit_dlg *s = (commit_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], ok = 0, skip = 0;
    if (s == NULL) {
        s = (commit_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->ask = strcmp(d->target, "save") == 0;
        if (s->ask) { lt_feat_history_default_message(app, s->msg, sizeof(s->msg)); }
        else { lt_strlcpy(s->msg, "Manual checkpoint", sizeof(s->msg)); }
    }
    if (lt_ui_modal_begin(ui, s->ask ? "Commit Tune Version" : "Create Checkpoint", 520, 250, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->ask ? "The tune was saved. Record this version in the tune history?"
                                    : "Record the current tune as a version in the tune history.", t->text2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Commit message", t->muted, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_text(ui, "cmsg", s->msg, sizeof(s->msg), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Annotation (optional)", t->muted, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_text(ui, "cnote", s->note, sizeof(s->note), 0);
        lt_ui_layout_space(ui, 8);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, s->ask ? "Skip" : "Cancel")) { skip = 1; }
        if (lt_ui_button_ex(ui, "Commit", s->msg[0] != '\0' ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) &&
            s->msg[0] != '\0') { ok = 1; }
        lt_ui_modal_end(ui);
    }
    if (ok) {
        char id[9];
        if (lt_feat_history_commit(app, s->msg, s->note, id) == 0) {
            lt_app_toast(app, 1, "Committed %s", id);
        } else {
            lt_app_toast(app, 3, "Failed to commit: %s", lt_err_get());
        }
        open = 0;
    }
    if (skip) { open = 0; }
    d->open = open;
}

static void free_data(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
/* new branch dialog                                                   */
typedef struct branch_dlg { char name[64]; char err[160]; } branch_dlg;

static void branch_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    branch_dlg *s = (branch_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], ok = 0, can;
    if (s == NULL) { s = (branch_dlg *)lt_calloc(1, sizeof(*s)); d->data = s; }
    can = lt_snap_branch_name_ok(s->name);
    if (lt_ui_modal_begin(ui, "New Branch", 480, 200, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "Create a new branch for experimenting with tune changes.", t->text2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_text(ui, "bname", s->name, sizeof(s->name), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->err[0] != '\0' ? s->err : "Branch name (e.g., high-boost-experiment)",
                       s->err[0] != '\0' ? t->error : t->muted, LT_UI_ALIGN_LEFT);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        if (lt_ui_button_ex(ui, "Create", can ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && can) { ok = 1; }
        lt_ui_modal_end(ui);
    }
    if (ok) {
        if (lt_snap_branch_create(app->project.path, s->name) == 0) {
            lt_app_toast(app, 1, "Branch %s created", s->name);
            open = 0;
        } else {
            lt_strlcpy(s->err, lt_err_get(), sizeof(s->err));
        }
    }
    d->open = open;
}

/* ------------------------------------------------------------------ */
/* changes of one version                                              */
typedef struct diff_dlg { lt_snap_diff diff; int ok; char id[9]; char err[160]; } diff_dlg;

static void diff_init(lt_app *app, lt_app_dialog *d)
{
    diff_dlg *s = (diff_dlg *)lt_calloc(1, sizeof(*s));
    char from[9], to[9];
    d->data = s;
    memset(from, 0, sizeof(from));
    memset(to, 0, sizeof(to));
    if (strlen(d->target) >= 17 && d->target[8] == ',') {
        memcpy(from, d->target, 8);
        memcpy(to, d->target + 9, 8);
    }
    lt_strlcpy(s->id, to, sizeof(s->id));
    if (lt_snap_diff_compute(app->project.path, from, to, &s->diff) == 0) { s->ok = 1; }
    else { lt_strlcpy(s->err, lt_err_get(), sizeof(s->err)); }
}

static void diff_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    diff_dlg *s = (diff_dlg *)d->data;
    int open = d->open, w1 = -1, w3[3], wb = -1;
    char title[64];
    size_t i;
    if (s == NULL) { diff_init(app, d); s = (diff_dlg *)d->data; }
    lt_snprintf(title, sizeof(title), "Changes in %s", s->id);
    if (lt_ui_modal_begin(ui, title, 640, 440, &open)) {
        lt_ui_layout_row(ui, 1, &w1, -(lt_ui_cell_mode(ui) ? 20 : 36));
        if (lt_ui_scroll_begin(ui, "hdiff")) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (!s->ok) {
                lt_ui_label_ex(ui, s->err, t->error, LT_UI_ALIGN_LEFT);
            } else if (s->diff.n_files == 0) {
                lt_ui_label_ex(ui, "No file changes detected", t->muted, LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
                lt_ui_label_ex(ui, "Files changed:", t->text, LT_UI_ALIGN_LEFT);
                lt_ui_text_style_reset(ui);
                for (i = 0; i < s->diff.n_files; i++) {
                    char line[300];
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_snprintf(line, sizeof(line), "  %s", s->diff.files_changed[i]);
                    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
                }
                if (s->diff.n_changes > 0) {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_separator(ui);
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
                    lt_ui_label_ex(ui, "Constants changed:", t->text, LT_UI_ALIGN_LEFT);
                    w3[0] = -2; w3[1] = -1; w3[2] = -1;
                    lt_ui_layout_row(ui, 3, w3, 0);
                    lt_ui_label_ex(ui, "Name", t->text, LT_UI_ALIGN_LEFT);
                    lt_ui_label_ex(ui, "Old", t->text, LT_UI_ALIGN_LEFT);
                    lt_ui_label_ex(ui, "New", t->text, LT_UI_ALIGN_LEFT);
                    lt_ui_text_style_reset(ui);
                    for (i = 0; i < s->diff.n_changes && i < 500; i++) {
                        const lt_snap_change *c = &s->diff.changes[i];
                        lt_color col = strcmp(c->change_type, "added") == 0 ? t->success
                                       : strcmp(c->change_type, "removed") == 0 ? t->error : t->warning;
                        char o[96], n[96];
                        lt_strlcpy(o, c->old_value != NULL ? c->old_value : "-", sizeof(o));
                        lt_strlcpy(n, c->new_value != NULL ? c->new_value : "-", sizeof(n));
                        lt_ui_layout_row(ui, 3, w3, 0);
                        lt_ui_label_ex(ui, c->name, col, LT_UI_ALIGN_LEFT);
                        lt_ui_label_ex(ui, o, t->text2, LT_UI_ALIGN_LEFT);
                        lt_ui_label_ex(ui, n, t->text2, LT_UI_ALIGN_LEFT);
                    }
                }
            }
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &wb, 0);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void diff_close(lt_app *app, lt_app_dialog *d)
{
    diff_dlg *s = (diff_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) { lt_snap_diff_free(&s->diff); lt_free(s); }
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
/* history panel                                                       */
typedef struct hist_dlg {
    lt_snap_info *h; size_t n;
    lt_snap_branch *br; size_t nbr;
    int br_sel;
    int loaded, exists, enabled, changes;
    char cur[64];
    char confirm_id[9];             /* version awaiting "Restore" confirmation */
    char confirm_branch[64];        /* branch awaiting "Switch" confirmation */
    int  confirm_open;
    char msg[200]; int msg_err;
} hist_dlg;

static void hist_clear(hist_dlg *s)
{
    lt_snap_info_free(s->h, s->n);
    s->h = NULL; s->n = 0;
    lt_free(s->br);
    s->br = NULL; s->nbr = 0;
}

static void hist_load(lt_app *app, hist_dlg *s)
{
    size_t i;
    hist_clear(s);
    s->loaded = 1;
    s->exists = app->has_project && lt_snap_exists(app->project.path);
    s->enabled = s->exists && lt_snap_enabled(app->project.path);
    s->changes = 0;
    s->cur[0] = '\0';
    if (!s->exists) { return; }
    (void)lt_snap_history(app->project.path, HIST_MAX, &s->h, &s->n);
    (void)lt_snap_branches(app->project.path, &s->br, &s->nbr);
    lt_snap_branch_current(app->project.path, s->cur);
    s->changes = lt_snap_has_changes(app->project.path) > 0;
    s->br_sel = 0;
    for (i = 0; i < s->nbr; i++) { if (s->br[i].is_current) { s->br_sel = (int)i; } }
}

static const char *branch_get(void *ud, int i)
{
    hist_dlg *s = (hist_dlg *)ud;
    static char buf[96];
    lt_snprintf(buf, sizeof(buf), "%s%s", s->br[i].is_current ? "* " : "", s->br[i].name);
    return buf;
}

static void set_msg(hist_dlg *s, int err, const char *prefix, const char *text)
{
    lt_snprintf(s->msg, sizeof(s->msg), "%s%s", prefix, text);
    s->msg_err = err;
}

static void hist_enable(lt_app *app, hist_dlg *s)
{
    char id[9];
    if (lt_snap_exists(app->project.path)) {
        (void)lt_snap_set_enabled(app->project.path, LT_TRUE);
    } else if (lt_feat_history_commit(app, "Initial project commit", NULL, id) < 0) {
        set_msg(s, 1, "Failed to initialize version control: ", lt_err_get());
        return;
    }
    s->loaded = 0;
}

static void commit_row(lt_app *app, hist_dlg *s, size_t idx, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_snap_info *c = &s->h[idx];
    int w2[2], w1 = -1, wb[3];
    char line[260];
    LT_UNUSED(d);
    lt_ui_push_id_int(ui, (int)idx);
    w2[0] = 90; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_text_style(ui, (int)LT_FONT_MONO, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, c->id, c->is_head ? t->primary : t->accent, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_label_ex(ui, c->timestamp, t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, c->message != NULL ? c->message : "", t->text, LT_UI_ALIGN_LEFT);
    if (c->annotation != NULL && c->annotation[0] != '\0') {
        lt_snprintf(line, sizeof(line), "Note: %s", c->annotation);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, line, t->warning, LT_UI_ALIGN_LEFT);
    }
    wb[0] = 130; wb[1] = 110; wb[2] = -1;
    lt_ui_layout_row(ui, 3, wb, 0);
    if (idx + 1 < s->n) {
        if (lt_ui_button(ui, "View Changes")) {
            char target[24];
            lt_snprintf(target, sizeof(target), "%s,%s", s->h[idx + 1].id, c->id);
            (void)lt_app_dialog_push(app, "history_diff", target, "Changes");
        }
    } else {
        lt_ui_label(ui, "");
    }
    if (!c->is_head) {
        if (lt_ui_button(ui, "Restore")) {
            lt_strlcpy(s->confirm_id, c->id, sizeof(s->confirm_id));
            s->confirm_branch[0] = '\0';
            s->confirm_open = 1;
        }
    } else {
        lt_ui_label_ex(ui, "Current", t->success, LT_UI_ALIGN_CENTER);
    }
    lt_ui_label(ui, "");
    lt_ui_layout_row(ui, 1, &w1, 4);
    lt_ui_separator(ui);
    lt_ui_pop_id(ui);
}

static void hist_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    hist_dlg *s = (hist_dlg *)d->data;
    int open = d->open, w1 = -1, wb[5], fh, i;
    if (s == NULL) { s = (hist_dlg *)lt_calloc(1, sizeof(*s)); d->data = s; }
    if (!s->loaded) { hist_load(app, s); }
    fh = lt_ui_cell_mode(ui) ? 16 : 32;
    if (lt_ui_modal_begin(ui, "Tune History", 720, 560, &open)) {
        if (!app->has_project) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Tune history belongs to a project. Open or create a project first.",
                           t->warning, LT_UI_ALIGN_LEFT);
        } else if (!s->exists) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
            lt_ui_label_ex(ui, "Enable Version Control", t->text, LT_UI_ALIGN_CENTER);
            lt_ui_text_style_reset(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, "Track changes to your tune over time. Every version keeps the tune, the "
                                 "project settings and the dashboards, and can be compared and restored.",
                             t->text2);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_button_ex(ui, "Initialize Version Control", LT_UI_BTN_PRIMARY)) { hist_enable(app, s); }
        } else {
            int sw;
            wb[0] = -1; wb[1] = 120; wb[2] = 150; wb[3] = -1;
            lt_ui_layout_row(ui, 3, wb, 0);
            sw = lt_ui_combo_cb(ui, "hbranch", &s->br_sel, (int)s->nbr, branch_get, s);
            if (sw && s->br_sel >= 0 && s->br_sel < (int)s->nbr && !s->br[s->br_sel].is_current) {
                lt_strlcpy(s->confirm_branch, s->br[s->br_sel].name, sizeof(s->confirm_branch));
                s->confirm_id[0] = '\0';
                s->confirm_open = 1;
            }
            if (lt_ui_button(ui, "New Branch")) {
                (void)lt_app_dialog_push(app, "history_branch", "", "New Branch");
                s->loaded = 0;
            }
            if (lt_ui_button(ui, "Create Checkpoint")) {
                (void)lt_app_dialog_push(app, "history_commit", "manual", "Create Checkpoint");
                s->loaded = 0;
            }
            wb[0] = -1; wb[1] = 220;
            lt_ui_layout_row(ui, 2, wb, 0);
            {
                char line[160];
                lt_snprintf(line, sizeof(line), "Branch %s  -  %lu version%s%s", s->cur, (unsigned long)s->n,
                            s->n == 1 ? "" : "s",
                            app->modified ? "  -  unsaved edits" : s->changes ? "  -  uncommitted changes" : "");
                lt_ui_label_ex(ui, line, app->modified || s->changes ? t->warning : t->muted, LT_UI_ALIGN_LEFT);
            }
            {
                int on = s->enabled;
                if (lt_ui_checkbox(ui, "Auto-commit this project", &on)) {
                    (void)lt_snap_set_enabled(app->project.path, on ? LT_TRUE : LT_FALSE);
                    s->loaded = 0;
                }
            }
            lt_ui_layout_row(ui, 1, &w1, -(fh + 6));
            if (lt_ui_scroll_begin(ui, "hlist")) {
                if (s->n == 0) {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_wrap(ui, "No versions yet. Changes will be tracked when you save, or use "
                                         "Create Checkpoint.", t->muted);
                }
                for (i = 0; i < (int)s->n; i++) { commit_row(app, s, (size_t)i, d); }
                lt_ui_scroll_end(ui);
            }
        }
        if (s->msg[0] != '\0') {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
        }
        wb[0] = -1; wb[1] = 110;
        lt_ui_layout_row(ui, 2, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    if (s->confirm_open) {
        char text[200];
        int r;
        if (s->confirm_branch[0] != '\0') {
            lt_snprintf(text, sizeof(text), "Switch to branch \"%s\"? Unsaved changes will be lost.",
                        s->confirm_branch);
        } else {
            lt_snprintf(text, sizeof(text), "Restore tune to version %s? Unsaved changes will be lost.",
                        s->confirm_id);
        }
        r = lt_ui_msgbox(ui, "hconfirm", s->confirm_branch[0] != '\0' ? "Switch Branch" : "Restore Version",
                         text, "Yes|Cancel", &s->confirm_open);
        if (r == 1) {
            int rc = s->confirm_branch[0] != '\0' ? lt_feat_history_switch(app, s->confirm_branch)
                                                  : lt_feat_history_restore(app, s->confirm_id);
            if (rc < 0) { set_msg(s, 1, "Failed: ", lt_err_get()); }
            else { set_msg(s, 0, "", s->confirm_branch[0] != '\0' ? "Branch switched" : "Version restored"); }
            s->loaded = 0;
        } else if (r != 0) {
            s->loaded = 0;
        }
    }
    d->open = open;
}

static void hist_close(lt_app *app, lt_app_dialog *d)
{
    hist_dlg *s = (hist_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) { hist_clear(s); lt_free(s); }
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
/* menu / settings                                                     */
static void open_history(lt_app *app)
{
    (void)lt_app_dialog_push(app, "history", "", "Tune History");
}

static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const modes[] = {"Never", "Always", "Ask each time"};
    int w1 = -1, w2[2], chg = 0;
    w2[0] = 210; w2[1] = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Tune History (Version Control)", t->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "Auto-commit on save", t->text, LT_UI_ALIGN_LEFT);
    if (lt_ui_combo(ui, "autocommit", &app->settings.auto_commit, modes, 3)) { chg = 1; }
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "Commit message format", t->text, LT_UI_ALIGN_LEFT);
    if (lt_ui_text(ui, "commitfmt", app->settings.commit_fmt, sizeof(app->settings.commit_fmt), 0) &
        LT_UI_CHANGED) { chg = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Placeholders: {date}, {time}, {table}. Applies to projects with version control "
                       "enabled (File > Tune History).", t->muted, LT_UI_ALIGN_LEFT);
    if (app->has_project) {
        double keep = (double)app->project.cfg.settings.max_restore_points;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label_ex(ui, "Restore points to keep (0 = all)", t->text, LT_UI_ALIGN_LEFT);
        if (lt_ui_number(ui, "rpkeep", &keep, 0.0, 999.0, 1.0, 0) & LT_UI_CHANGED) {
            app->project.cfg.settings.max_restore_points = (unsigned)keep;
            (void)lt_project_save_config(&app->project);
        }
    }
    if (chg) { lt_app_settings_save(app); }
}

void lt_feat_history_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Tune &History...", NULL, open_history, lt_ext_pred_project);
    lt_ext_add_settings_page(reg, "Version Control", settings_page);
    lt_ext_add_on_save(reg, on_save);
    dk.kind = "history"; dk.draw = hist_draw; dk.close = hist_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "history_diff"; dk.draw = diff_draw; dk.close = diff_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "history_branch"; dk.draw = branch_draw; dk.close = free_data;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "history_commit"; dk.draw = commit_draw; dk.close = free_data;
    lt_ext_add_dialog_kind(reg, &dk);
}
