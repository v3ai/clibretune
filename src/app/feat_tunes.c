/* feat_tunes.c - tune open dialog and New Tune (G-19).
 *   File -> Open Tune...   with a project: the tunes in the project folder (and any file browsed
 *                          to, e.g. from another project) with an info panel (signature, saved
 *                          date, author, constants, size) and Load.
 *                          without a project: pick a tune, match INIs from the INI library, name
 *                          the project and create it (the original OpenTuneDialog).
 *   File -> New Tune...    a tune with the INI's default values, saved as a new file of the project
 *                          and made the current tune.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "files_logic.h"
#include "sync_logic.h"

#define FD_TUNE_BROWSE (LT_APP_FD_USER0 + 50)
#define MAX_TUNES 64
#define MAX_INIS 24

/* a queued "create a project from a tune" (runs from the tick hook) */
typedef struct tune_queue { char tune[512], ini[512], name[96]; } tune_queue;

static tune_queue *tq(lt_app *app)
{
    return (tune_queue *)lt_ext_state(app, "tunes", sizeof(tune_queue), NULL);
}

typedef struct tune_ent { char path[512], label[128]; } tune_ent;

typedef struct tune_dlg {
    tune_ent e[MAX_TUNES];
    int n, sel, shown;               /* shown: entry whose info is displayed (-1 none) */
    lt_tuneinfo info;
    int info_ok;
    char err[200];
    /* no project: INI choice and name */
    lt_ini_repo repo; int repo_ok;
    lt_sync_ini_match m[MAX_INIS]; int n_m, ini_sel;
    char name[96];
    int project_mode;
} tune_dlg;

static void add_entry(tune_dlg *s, const char *path, const char *label)
{
    int i;
    if (s->n >= MAX_TUNES) { return; }
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->e[i].path, path) == 0) { s->sel = i; return; }
    }
    lt_strlcpy(s->e[s->n].path, path, sizeof(s->e[0].path));
    lt_strlcpy(s->e[s->n].label, label, sizeof(s->e[0].label));
    s->sel = s->n++;
}

static void show_info(tune_dlg *s)
{
    s->shown = s->sel;
    s->info_ok = 0;
    s->err[0] = '\0';
    s->n_m = 0;
    s->ini_sel = 0;
    if (s->sel < 0 || s->sel >= s->n) { return; }
    if (lt_tuneinfo_read(s->e[s->sel].path, &s->info) == 0) {
        s->info_ok = 1;
        if (s->repo_ok) {
            s->n_m = lt_sync_find_inis(&s->repo, s->info.signature, s->m, MAX_INIS);
        }
        if (s->name[0] == '\0' || !s->project_mode) {
            const char *b = lt_path_base(s->e[s->sel].path);
            size_t k = strlen(b);
            lt_strlcpy(s->name, b, sizeof(s->name));
            if (k > 4 && lt_stricmp(b + k - 4, ".msq") == 0) { s->name[k - 4] = '\0'; }
        }
    } else {
        lt_snprintf(s->err, sizeof(s->err), "Cannot read this tune file: %s", lt_err_get());
    }
}

static const char *tune_item(void *user, int i)
{
    static char buf[160];
    const tune_dlg *s = (const tune_dlg *)user;
    lt_snprintf(buf, sizeof(buf), "%s", s->e[i].label);
    return buf;
}

static const char *ini_item(void *user, int i)
{
    static char buf[300];
    const tune_dlg *s = (const tune_dlg *)user;
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s", s->m[i].name, s->m[i].signature,
                s->m[i].kind == LT_SIG_EXACT ? "Exact" : "Partial");
    return buf;
}

static void info_row(lt_ui *ui, const char *label, const char *val)
{
    int w2[2];
    w2[0] = 110; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, label, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, val[0] != '\0' ? val : "-", lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
}

static void info_panel(lt_ui *ui, const tune_dlg *s)
{
    char b[64];
    info_row(ui, "File:", s->info.file);
    info_row(ui, "Signature:", s->info.signature);
    info_row(ui, "Saved:", s->info.saved);
    info_row(ui, "Author:", s->info.author);
    lt_snprintf(b, sizeof(b), "%lu constants, %lu page(s), %lu bytes", (unsigned long)s->info.constants,
                (unsigned long)s->info.pages, s->info.size);
    info_row(ui, "Contents:", b);
    if (s->info.desc[0] != '\0') { info_row(ui, "Description:", s->info.desc); }
}

static void fill_project_files(lt_app *app, tune_dlg *s)
{
    char **names = NULL;
    size_t n = 0, i;
    if (lt_tunes_list(app->project.path, &names, &n) == 0) {
        for (i = 0; i < n; i++) {
            char *full = lt_path_join(app->project.path, names[i]);
            char label[128];
            lt_snprintf(label, sizeof(label), "%s%s", names[i],
                        strcmp(names[i], "CurrentTune.msq") == 0 ? "   (project tune)" : "");
            add_entry(s, full, label);
            lt_free(full);
        }
        lt_strv_free(names, n);
    }
    s->sel = 0;
    for (i = 0; i < (size_t)s->n; i++) {
        if (strstr(s->e[i].label, "(project tune)") != NULL) { s->sel = (int)i; }
    }
    if (s->n == 0) { s->sel = -1; }
}

static void tune_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    tune_dlg *s = (tune_dlg *)d->data;
    int open = d->open, w1 = -1, w3[3], act = 0, fh = lt_ui_cell_mode(ui) ? 16 : 0;
    const char *picked;
    LT_UNUSED(fh);
    if (s == NULL) {
        s = (tune_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->shown = -2;
        s->project_mode = app->has_project;
        s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
        if (s->project_mode) { fill_project_files(app, s); }
        else { s->sel = -1; }
    }
    picked = lt_app_file_result(app, FD_TUNE_BROWSE);
    if (picked != NULL) { add_entry(s, picked, picked); s->shown = -2; }
    if (s->shown != s->sel) { show_info(s); }
    if (lt_ui_modal_begin(ui, s->project_mode ? "Open Tune" : "Open Tune as New Project", 600, 500,
                          &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->project_mode
            ? "Tunes found in the project folder. Browse to load one from another project or folder."
            : "Pick a tune file. LibreTune matches an ECU definition from the INI library by the "
              "tune's signature and creates a project for it.", t->text2);
        lt_ui_layout_row(ui, 1, &w1, 100);
        if (s->n > 0) {
            if (lt_ui_listbox_cb(ui, "tunelist", &s->sel, s->n, tune_item, s) & LT_UI_ACTIVATED) {
                if (s->project_mode) { act = 1; }
            }
        } else {
            lt_ui_label_ex(ui, s->project_mode ? "No tune files in the project folder."
                                               : "No tune chosen yet.", t->muted, LT_UI_ALIGN_LEFT);
        }
        if (s->info_ok) {
            lt_ui_separator(ui);
            info_panel(ui, s);
        } else if (s->err[0] != '\0') {
            lt_ui_label_wrap(ui, s->err, t->error);
        }
        if (!s->project_mode && s->info_ok) {
            lt_ui_separator(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_heading(ui, s->n_m > 0 ? "Matching ECU definitions:" : "No matching ECU definition");
            if (s->n_m > 0) {
                lt_ui_layout_row(ui, 1, &w1, 64);
                (void)lt_ui_listbox_cb(ui, "tuneinis", &s->ini_sel, s->n_m, ini_item, s);
            } else {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_wrap(ui, "Import the matching INI into the INI library first "
                                     "(File -> Search for INI Online).", t->muted);
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Project name", t->text, LT_UI_ALIGN_LEFT);
            (void)lt_ui_text(ui, "tunename", s->name, sizeof(s->name), 0);
        }
        w3[0] = -1; w3[1] = 150; w3[2] = 90;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button(ui, "Browse...")) {
            lt_app_file_dialog(app, FD_TUNE_BROWSE, LT_UI_FILE_OPEN,
                               s->project_mode ? app->project.path : NULL, "",
                               "Tune files|*.msq;*.xml|All files|*");
        }
        {
            int can = s->info_ok && (s->project_mode || (s->n_m > 0 && s->name[0] != '\0'));
            if (lt_ui_button_ex(ui, s->project_mode ? "Load Tune" : "Create Project",
                                can ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) { act = 1; }
            if (!can && act == 1) { act = 0; }
        }
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    if (act == 1 && s->info_ok && s->sel >= 0) {
        char path[512], name[96], *ini = NULL;
        lt_strlcpy(path, s->e[s->sel].path, sizeof(path));
        lt_strlcpy(name, s->name, sizeof(name));
        if (s->project_mode) {
            if (lt_app_load_tune(app, path) == 0) { open = 0; }
        } else if (s->n_m > 0) {
            ini = lt_ini_repo_path(&s->repo, s->m[LT_CLAMP(s->ini_sel, 0, s->n_m - 1)].id);
            if (ini != NULL) {
                /* creating the project closes this dialog: the work is queued for the tick hook */
                tune_queue *q = tq(app);
                lt_strlcpy(q->tune, path, sizeof(q->tune));
                lt_strlcpy(q->ini, ini, sizeof(q->ini));
                lt_strlcpy(q->name, name, sizeof(q->name));
                open = 0;
            }
            lt_free(ini);
        }
    }
    d->open = open;
}

static void tunes_tick(lt_app *app)
{
    tune_queue q = *tq(app);
    if (q.tune[0] == '\0') { return; }
    tq(app)->tune[0] = '\0';
    if (lt_app_new_project(app, q.name, q.ini) == 0) { (void)lt_app_load_tune(app, q.tune); }
}

static void tune_close(lt_app *app, lt_app_dialog *d)
{
    tune_dlg *s = (tune_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
        lt_free(s);
    }
    d->data = NULL;
}

/* ---- New Tune ---- */
typedef struct new_dlg { char name[96]; char err[200]; } new_dlg;

/* create the file of a new default tune in the project folder and make it current;
 * returns 0 and the file name in out */
int lt_feat_tune_new(lt_app *app, const char *name, char *out, size_t cap)
{
    char file[128], *path;
    int rc;
    if (!app->has_project || app->ini == NULL) { lt_err_set("no project open"); return -1; }
    lt_tunes_new_name(app->project.path, name, file, sizeof(file));
    path = lt_path_join(app->project.path, file);
    rc = lt_app_reset_defaults(app);
    if (rc == 0) {
        rc = lt_app_save_tune_as(app, path);
        app->modified = 1;               /* CurrentTune.msq still holds the old tune */
    }
    if (rc == 0 && out != NULL) { lt_strlcpy(out, file, cap); }
    lt_free(path);
    return rc;
}

static void new_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    new_dlg *s = (new_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], act = 0;
    if (s == NULL) {
        s = (new_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_strlcpy(s->name, "New Tune", sizeof(s->name));
    }
    if (lt_ui_modal_begin(ui, "New Tune", 460, 250, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "Start a new tune from the default values of the ECU definition. It is "
                             "saved as a new file in the project folder and becomes the current tune. "
                             "The previous project tune stays on disk until you save.", t->text2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Tune name", t->text, LT_UI_ALIGN_LEFT);
        (void)lt_ui_text(ui, "newtune", s->name, sizeof(s->name), 0);
        if (s->err[0] != '\0') { lt_ui_label_wrap(ui, s->err, t->error); }
        w2[0] = -1; w2[1] = 110;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Create", s->name[0] != '\0' ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) {
            act = 1;
        }
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    if (act == 1) {
        char file[128];
        if (lt_feat_tune_new(app, s->name, file, sizeof(file)) == 0) {
            lt_app_toast(app, 1, "New tune %s created", file);
            open = 0;
        } else {
            lt_snprintf(s->err, sizeof(s->err), "Cannot create the tune: %s", lt_err_get());
        }
    }
    d->open = open;
}

static void new_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

static void act_open(lt_app *app) { (void)lt_app_dialog_push(app, "tune_open", "", "Open Tune"); }
static void act_new(lt_app *app) { (void)lt_app_dialog_push(app, "tune_new", "", "New Tune"); }

void lt_feat_tunes_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "tune_open"; dk.draw = tune_draw; dk.close = tune_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "tune_new"; dk.draw = new_draw; dk.close = new_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_tick(reg, tunes_tick);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Open &Tune...", NULL, act_open, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "&New Tune...", NULL, act_new, lt_ext_pred_project);
}
