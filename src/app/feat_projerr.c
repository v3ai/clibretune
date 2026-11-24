/* feat_projerr.c - a project that cannot be opened (project.json missing, empty or invalid, the
 * folder gone, the ECU definition missing or unreadable) or that opens with a definition that
 * looks incomplete: one dialog that says what is wrong in plain words and where, with a way out
 * (Show Folder, Remove from Recent Projects, Replace Definition..., Close / Continue anyway)
 * instead of a status-bar line or a half-open project. See feat.h (LT_PROJERR_*).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../plat/plat.h"

#define PE_KIND "project_problem"

typedef struct pe_dlg {
    int  kind;
    char msg[700];
    char file[512];
    char dir[512];
    int  listed;            /* the folder is on Recent Projects (until removed) */
    int  folder_shown;      /* Show Folder was used (tests, the label) */
} pe_dlg;

static int is_listed(lt_app *app, const char *dir)
{
    int i;
    for (i = 0; i < app->settings.n_recent; i++) {
        if (lt_plat_path_same(app->settings.recent[i], dir)) { return 1; }
    }
    return 0;
}

lt_app_dialog *lt_feat_projerr_show(lt_app *app, int kind, const char *dir, const char *file,
                                    const char *msg)
{
    const char *title = kind == LT_PROJERR_INCOMPLETE ? "This ECU definition looks incomplete"
                                                      : "Cannot Open Project";
    lt_app_dialog *d = lt_app_dialog_push(app, PE_KIND, "", title);
    pe_dlg *s;
    if (d == NULL) { return NULL; }
    lt_free(d->data);                   /* one is showing already: the newest problem replaces it */
    lt_strlcpy(d->title, title, sizeof(d->title));
    d->open = 1;
    s = (pe_dlg *)lt_calloc(1, sizeof(*s));
    s->kind = kind;
    lt_strlcpy(s->dir, dir != NULL ? dir : "", sizeof(s->dir));
    lt_strlcpy(s->file, file != NULL ? file : s->dir, sizeof(s->file));
    lt_strlcpy(s->msg, msg != NULL ? msg : "", sizeof(s->msg));
    s->listed = is_listed(app, s->dir);
    d->data = s;
    return d;
}

int lt_feat_projerr_info(const lt_app_dialog *d, const char **msg, const char **file)
{
    const pe_dlg *s;
    if (d == NULL || strcmp(d->kind, PE_KIND) != 0 || d->data == NULL) { return -1; }
    s = (const pe_dlg *)d->data;
    if (msg != NULL) { *msg = s->msg; }
    if (file != NULL) { *file = s->file; }
    return s->kind;
}

int lt_feat_projerr_act(lt_app *app, lt_app_dialog *d, int action)
{
    pe_dlg *s;
    if (d == NULL || strcmp(d->kind, PE_KIND) != 0 || d->data == NULL) { return -1; }
    s = (pe_dlg *)d->data;
    switch (action) {
    case LT_PROJERR_ACT_SHOW_FOLDER:
        if (s->kind == LT_PROJERR_INCOMPLETE || !lt_plat_is_dir(s->dir)) { return -1; }
        /* the file manager is not started by tests and screenshots */
        if (!app->headless && lt_plat_open_url(s->dir) != 0) {
            lt_app_toast(app, 3, "Cannot open the folder %s", s->dir);
            return -1;
        }
        s->folder_shown = 1;
        return 0;
    case LT_PROJERR_ACT_FORGET:
        if (s->kind == LT_PROJERR_INCOMPLETE || !s->listed) { return -1; }
        (void)lt_feat_start_forget(app, s->dir);
        s->listed = 0;
        return 0;
    case LT_PROJERR_ACT_REPLACE:
        if (s->kind == LT_PROJERR_PROJECT) { return -1; }
        d->open = 0;
        if (s->kind == LT_PROJERR_INCOMPLETE) {          /* the project is open: the update flow */
            (void)lt_app_dialog_push(app, "migration", "update", "Update Project INI");
        } else {
            lt_feat_migrate_replace(app, s->dir);
        }
        return 0;
    case LT_PROJERR_ACT_CLOSE:
        d->open = 0;
        return 0;
    default:
        return -1;
    }
}

static void pe_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    pe_dlg *s = (pe_dlg *)d->data;
    int open = d->open, w1 = -1, wb[5], fh = lt_ui_cell_mode(ui) ? 16 : 30, nb = 0;
    char line[700];
    if (s == NULL) {                    /* LT_SHOT_OPEN=dlg:project_problem preview */
        s = (pe_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->kind = LT_PROJERR_PROJECT;
        lt_strlcpy(s->dir, "/home/me/clibretune Projects/Miata", sizeof(s->dir));
        lt_strlcpy(s->file, "/home/me/clibretune Projects/Miata/project.json", sizeof(s->file));
        lt_strlcpy(s->msg, "The project settings file (project.json) is empty. It was probably cut off "
                   "when the disk filled up or the computer lost power.", sizeof(s->msg));
        s->listed = 1;
        lt_strlcpy(d->title, "Cannot Open Project", sizeof(d->title));
    }
    if (lt_ui_modal_begin(ui, d->title, 680, 272, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->msg, s->kind == LT_PROJERR_INCOMPLETE ? t->warning : t->error);
        lt_ui_layout_space(ui, 4);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(line, sizeof(line), "%s %s", s->kind == LT_PROJERR_PROJECT && strcmp(s->file, s->dir) == 0
                    ? "Folder:" : "File:", s->file);
        lt_ui_label_wrap(ui, line, t->text2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->kind == LT_PROJERR_PROJECT) {
            lt_ui_label_wrap(ui, "Nothing was changed. Open the folder to look at the files, or take the "
                                 "project off the list if it is gone for good. Tunes saved in the folder "
                                 "(CurrentTune.msq, restorePoints) can be loaded into a new project.", t->muted);
        } else if (s->kind == LT_PROJERR_DEFINITION) {
            lt_ui_label_wrap(ui, "The project needs the ECU definition (INI) made for the firmware on the "
                                 "ECU. Replace it with a bundled one, one from the INI library or a file; "
                                 "the tune in the project is kept.", t->muted);
        } else {
            lt_ui_label_wrap(ui, "Settings may be missing from the menus, and writing to the ECU could change "
                                 "the wrong bytes. Replace it with the definition made for the firmware on "
                                 "the ECU, or continue to look around without burning.", t->muted);
        }
        lt_ui_layout_space(ui, 8);
        wb[nb++] = -1;
        if (s->kind != LT_PROJERR_PROJECT) { wb[nb++] = 170; }
        if (s->kind != LT_PROJERR_INCOMPLETE) { wb[nb++] = 110; wb[nb++] = 220; }
        wb[nb++] = s->kind == LT_PROJERR_INCOMPLETE ? 150 : 80;
        lt_ui_layout_row(ui, nb, wb, fh);
        lt_ui_label(ui, "");
        if (s->kind != LT_PROJERR_PROJECT &&
            lt_ui_button_ex(ui, "Replace Definition...", LT_UI_BTN_PRIMARY)) {
            (void)lt_feat_projerr_act(app, d, LT_PROJERR_ACT_REPLACE);
            open = d->open;
        }
        if (s->kind != LT_PROJERR_INCOMPLETE) {
            int can = lt_plat_is_dir(s->dir);
            if (lt_ui_button_ex(ui, s->folder_shown ? "Folder Shown" : "Show Folder", can ? 0 : LT_UI_BTN_DISABLED) &&
                can) {
                (void)lt_feat_projerr_act(app, d, LT_PROJERR_ACT_SHOW_FOLDER);
            }
            if (lt_ui_button_ex(ui, s->listed ? "Remove from Recent Projects" : "Not in Recent Projects",
                                s->listed ? 0 : LT_UI_BTN_DISABLED) && s->listed) {
                (void)lt_feat_projerr_act(app, d, LT_PROJERR_ACT_FORGET);
            }
        }
        if (lt_ui_button_ex(ui, s->kind == LT_PROJERR_INCOMPLETE ? "Continue anyway" : "Close",
                            s->kind == LT_PROJERR_PROJECT ? LT_UI_BTN_DEFAULT : 0)) {
            open = 0;
        }
        lt_ui_modal_end(ui);
    }
    if (d->open) { d->open = open; }
}

static void pe_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

void lt_feat_projerr_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = PE_KIND; dk.draw = pe_draw; dk.close = pe_close;
    lt_ext_add_dialog_kind(reg, &dk);
}
