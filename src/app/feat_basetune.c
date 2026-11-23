/* feat_basetune.c - the Base Tune library browser.
 *   File -> New Project from Base Tune...   pick a built-in Speeduino base tune, match the ECU
 *                                           definition from the INI library (or choose an INI file),
 *                                           name the project, create it with the tune
 *   File -> Load Base Tune...               load a base tune into the open project / definition
 *   New Project dialog -> "From a Base Tune..."   opens the browser in new-project mode
 * The tunes are embedded (src/tune/basetune.h; CC BY-NC 4.0, Speeduino project and contributors):
 * the browser names each tune's author and the licence. When the tune's firmware signature
 * differs from the definition, or values do not fit, the migration report (feat_migrate.c) shows
 * what does not map before anything is applied. Creating a project replaces the session, so the
 * work is queued and run from the tick hook.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../project/inilib.h"
#include "../tune/basetune.h"

#define FD_BT_INI (LT_APP_FD_USER0 + 150)
#define BT_MAX_LIST 64
#define BT_MAX_MAKES 24
#define BT_MAX_INIS 16

#define BT_CAUTION "Caution: a base tune is only a starting point. Wrong injector, trigger or ignition " \
                   "settings can damage an engine - check them before starting it."
#define BT_RUSEFI_NOTE "rusEFI, FOME, epicEFI/uaEFI and miata-ecus boards: the firmware ships its own " \
                       "base tune - connect to the ECU and read it."

/* queued work: staged by the browser, confirmed directly or by the migration report */
typedef struct bt_state {
    int  staged, pending;        /* 0 none, 1 new project, 2 load into the open definition */
    int  st_tune, pd_tune;       /* catalog index */
    char st_ini[512], pd_ini[512];
    char st_name[96], pd_name[96];
} bt_state;

static bt_state *bt_st(lt_app *app)
{
    return (bt_state *)lt_ext_state(app, "basetune", sizeof(bt_state), NULL);
}

/* ---- applying ---- */
int lt_feat_basetune_load(lt_app *app, const struct lt_basetune *b)
{
    lt_tune *t, *d;
    char origin[600];
    int pg;
    if (app->ini == NULL || b == NULL) { lt_err_set("No ECU definition open"); return -1; }
    t = lt_basetune_load(b);
    if (t == NULL) {
        lt_app_toast(app, 3, "Cannot read the base tune: %s", lt_err_get());
        return -1;
    }
    /* the definition's layout with its defaults, then every named value of the tune on top: constants
     * the tune lacks keep their defaults, values that do not fit are left at the default */
    d = lt_tune_from_defaults(app->ini);
    for (pg = 0; pg < 256; pg++) {
        if (d->pages[pg] != NULL) { lt_tune_set_page(t, pg, d->pages[pg], d->page_len[pg]); }
    }
    lt_tune_free(d);
    (void)lt_tune_apply_to_pages(t, app->ini);
    (void)lt_tune_capture_from_pages(t, app->ini);
    lt_free(t->signature);
    t->signature = lt_strdup(app->ini->signature != NULL ? app->ini->signature : "");
    lt_basetune_origin(b, origin, sizeof(origin));
    lt_free(t->description);
    t->description = lt_strdup(origin);
    lt_tune_touch(t);
    return lt_app_load_tune_mem(app, t, b->name);
}

int lt_feat_basetune_new_project(lt_app *app, const struct lt_basetune *b, const char *ini_path,
                                 const char *name)
{
    if (b == NULL || ini_path == NULL || ini_path[0] == '\0') {
        lt_err_set("No ECU definition chosen");
        return -1;
    }
    if (lt_app_new_project(app, name != NULL && name[0] != '\0' ? name : b->name, ini_path) != 0) {
        return -1;
    }
    if (lt_feat_basetune_load(app, b) != 0) { return -1; }
    if (lt_app_save_tune(app) != 0) { return -1; }
    lt_app_toast(app, 1, "Project created from the base tune %s", b->name);
    return 0;
}

static void bt_confirm(lt_app *app)
{
    bt_state *s = bt_st(app);
    if (s->staged == 0) { return; }
    s->pending = s->staged;
    s->pd_tune = s->st_tune;
    lt_strlcpy(s->pd_ini, s->st_ini, sizeof(s->pd_ini));
    lt_strlcpy(s->pd_name, s->st_name, sizeof(s->pd_name));
    s->staged = 0;
}

static void bt_tick(lt_app *app)
{
    bt_state *s = bt_st(app);
    bt_state q;
    if (s->pending == 0) { return; }
    q = *s;
    s->pending = 0;
    if (q.pending == 1) {
        (void)lt_feat_basetune_new_project(app, lt_basetune_at((size_t)q.pd_tune), q.pd_ini, q.pd_name);
    } else {
        (void)lt_feat_basetune_load(app, lt_basetune_at((size_t)q.pd_tune));
    }
}

/* Stage the work and either queue it right away (same firmware, everything maps) or show the
 * migration report first. ini: the definition the tune goes into (already loaded). */
static void bt_submit(lt_app *app, int kind, const lt_basetune *b, const lt_ini *ini, const char *ini_path,
                      const char *name)
{
    bt_state *s = bt_st(app);
    lt_migration_report rep;
    lt_tune *t;
    const char *isig = ini->signature != NULL ? ini->signature : "";
    int exact = lt_sig_compare(b->signature, isig, NULL) == LT_SIG_EXACT;
    char intro[600];
    s->staged = kind;
    s->st_tune = (int)(b - lt_basetune_at(0));
    lt_strlcpy(s->st_ini, ini_path != NULL ? ini_path : "", sizeof(s->st_ini));
    lt_strlcpy(s->st_name, name != NULL ? name : "", sizeof(s->st_name));
    t = lt_basetune_load(b);
    if (t == NULL) {
        lt_app_toast(app, 3, "Cannot read the base tune: %s", lt_err_get());
        s->staged = 0;
        return;
    }
    lt_migration_init(&rep);
    lt_migration_compare_tune(&rep, t, ini);
    lt_tune_free(t);
    if (exact && rep.n_type == 0 && rep.n_missing_in_ini == 0) {
        lt_migration_clear(&rep);
        bt_confirm(app);
        return;
    }
    if (exact) {
        lt_snprintf(intro, sizeof(intro), "Base tune \"%s\" (%s) matches this ECU definition (%s), but "
                    "some values do not map onto it. They are listed below. " BT_CAUTION,
                    b->name, b->signature, isig);
    } else {
        lt_snprintf(intro, sizeof(intro), "Base tune \"%s\" was saved by firmware %s; the ECU definition "
                    "is %s. Values are matched by name, the lists below show what does not map. " BT_CAUTION,
                    b->name, b->signature[0] != '\0' ? b->signature : "(unknown)",
                    isig[0] != '\0' ? isig : "(unknown)");
    }
    lt_feat_migrate_show(app, "Base Tune Migration Report", intro, &rep, bt_confirm);
}

/* ---- browser ---- */
typedef struct bt_dlg {
    int  mode;                   /* 1 new project, 2 load */
    int  make_sel;               /* 0 = all makes */
    const char *makes[BT_MAX_MAKES + 1];
    int  n_makes;
    char query[64];
    size_t idx[BT_MAX_LIST];
    int  n_idx, pos, cur;        /* cur: catalog index of the selection (-1 none) */
    int  shown;                  /* catalog index the INI matches were computed for */
    lt_ini_repo repo; int repo_ok, gen;
    lt_inilib_match m[BT_MAX_INIS];
    int  n_m, ini_sel, ini_src;  /* ini_src: 0 library, 1 file */
    char ini_file[512];
    char name[96];
    char err[240];
} bt_dlg;

static const char *row_get(void *ud, int i)
{
    const bt_dlg *s = (const bt_dlg *)ud;
    static char buf[200];
    const lt_basetune *b = lt_basetune_at(s->idx[i]);
    lt_snprintf(buf, sizeof(buf), "%s  \xC2\xB7  %s", b->make, b->name);
    return buf;
}

static const char *ini_get(void *ud, int i)
{
    const bt_dlg *s = (const bt_dlg *)ud;
    static char buf[300];
    const lt_ini_entry *e = &s->repo.e[s->m[i].index];
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s",
                e->name != NULL ? e->name : (e->id != NULL ? e->id : ""),
                e->signature != NULL ? e->signature : "",
                s->m[i].match == LT_SIG_EXACT ? "exact match" : "different version");
    return buf;
}

static void repo_reload(lt_app *app, bt_dlg *s)
{
    if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
    s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
    s->gen = lt_feat_online_generation(app);
    s->shown = -2;
}

static int mode_auto(lt_app *app)
{
    if (app->ini != NULL && lt_basetune_family(app->ini->signature) == LT_BASETUNE_FAM_SPEEDUINO) {
        return 2;
    }
    return 1;
}

static void bt_init(lt_app *app, lt_app_dialog *d)
{
    bt_dlg *s = (bt_dlg *)lt_calloc(1, sizeof(*s));
    d->data = s;
    s->mode = strcmp(d->target, "new") == 0 ? 1 : strcmp(d->target, "load") == 0 ? 2 : mode_auto(app);
    s->makes[0] = "All makes";
    s->n_makes = 1 + (int)lt_basetune_makes(s->makes + 1, BT_MAX_MAKES);
    if (s->n_makes > BT_MAX_MAKES + 1) { s->n_makes = BT_MAX_MAKES + 1; }
    s->cur = lt_basetune_count() > 0 ? 0 : -1;
    s->shown = -2;
    repo_reload(app, s);
}

static void detail_row(lt_ui *ui, const char *label, const char *val)
{
    int w2[2];
    const lt_theme *t = lt_ui_theme(ui);
    w2[0] = 96; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, label, t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, val != NULL && val[0] != '\0' ? val : "-", t->text, LT_UI_ALIGN_LEFT);
}

static void details(lt_ui *ui, const lt_basetune *b)
{
    const lt_theme *t = lt_ui_theme(ui);
    char buf[200];
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (b == NULL) {
        lt_ui_label_ex(ui, "No base tune matches the filter.", t->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
    lt_ui_label_ex(ui, b->name, t->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    detail_row(ui, "Make", b->make);
    detail_row(ui, "Board", b->board);
    lt_snprintf(buf, sizeof(buf), "%ld", b->firmware);
    if (b->firmware > 0 && strstr(b->signature, buf) == NULL) {
        lt_snprintf(buf, sizeof(buf), "%s  (upstream index: %ld)", b->signature, b->firmware);
    } else {
        lt_strlcpy(buf, b->signature, sizeof(buf));
    }
    detail_row(ui, "Firmware", buf);
    detail_row(ui, "Provider", lt_basetune_named(b->provider) ? b->provider : "not named upstream");
    detail_row(ui, "Type", b->kind);
    lt_snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)((b->size + 1023) / 1024));
    detail_row(ui, "Size", buf);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, b->description[0] != '\0' ? b->description : "No description given.",
                     b->description[0] != '\0' ? t->text2 : t->muted);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_basetune_credit(b, buf, sizeof(buf));
    lt_ui_label_wrap(ui, buf, t->muted);
}

/* "new project" part: INI choice and project name; returns the INI path to use ("" none) */
static void section_new(lt_app *app, bt_dlg *s, const lt_basetune *b, char *ini_out, size_t cap)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w2[2], w3[3];
    ini_out[0] = '\0';
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "ECU definition for the new project");
    w2[0] = 220; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_radio(ui, "From the INI library", &s->ini_src, 0);
    lt_ui_radio(ui, "From an INI file", &s->ini_src, 1);
    if (s->ini_src == 0) {
        if (s->n_m > 0) {
            lt_ui_layout_row(ui, 1, &w1, 64);
            (void)lt_ui_listbox_cb(ui, "btinis", &s->ini_sel, s->n_m, ini_get, s);
            if (s->repo_ok && s->ini_sel >= 0 && s->ini_sel < s->n_m) {
                char *p = lt_ini_repo_path(&s->repo, s->repo.e[s->m[s->ini_sel].index].id);
                if (p != NULL) { lt_strlcpy(ini_out, p, cap); }
                lt_free(p);
            }
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, b != NULL && b->signature[0] != '\0'
                ? "No Speeduino INI in the INI library. Choose the INI of your firmware, or connect to the "
                  "ECU to identify it (the wizard creates the project, then use File > Load Base Tune)."
                : "No matching INI in the INI library.", t->warning);
            w3[0] = -1; w3[1] = -1; w3[2] = -1;
            lt_ui_layout_row(ui, 3, w3, 0);
            if (lt_ui_button(ui, "Choose INI File...")) {
                s->ini_src = 1;
                lt_app_file_dialog(app, FD_BT_INI, LT_UI_FILE_OPEN, NULL, "", "INI Files|*.ini|All Files|*");
            }
            if (lt_ui_button(ui, "Search for INI Online...")) {
                lt_feat_online_open(app, b != NULL ? b->signature : "");
            }
            if (lt_ui_button(ui, "Connect ECU Wizard...")) {
                (void)lt_app_dialog_push(app, "connect_wizard", "", "Connect ECU");
            }
        }
    } else {
        w2[0] = 120; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Browse...")) {
            lt_app_file_dialog(app, FD_BT_INI, LT_UI_FILE_OPEN, NULL, "", "INI Files|*.ini|All Files|*");
        }
        (void)lt_ui_text(ui, "btinifile", s->ini_file, sizeof(s->ini_file), 0);
        lt_strlcpy(ini_out, s->ini_file, cap);
    }
    w2[0] = 120; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "Project name", t->text, LT_UI_ALIGN_LEFT);
    (void)lt_ui_text(ui, "btname", s->name, sizeof(s->name), 0);
}

/* "load" part: where the tune goes and whether it matches; returns nonzero when loading is possible */
static int section_load(lt_app *app, const lt_basetune *b)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const char *isig;
    char line[700];
    int w1 = -1, fam, m;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (app->ini == NULL) {
        lt_ui_label_wrap(ui, "No ECU definition is open. Open a project, or create one from the base tune.",
                         t->warning);
        return 0;
    }
    isig = app->ini->signature != NULL ? app->ini->signature : "";
    lt_snprintf(line, sizeof(line), "Load into: %s  [%s]",
                app->has_project ? app->project.cfg.name : lt_path_base(app->ini_path), isig);
    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    fam = lt_basetune_family(isig);
    if (fam != LT_BASETUNE_FAM_SPEEDUINO) {
        lt_ui_label_wrap(ui, fam == LT_BASETUNE_FAM_RUSEFI
            ? "This is not a Speeduino definition. " BT_RUSEFI_NOTE
            : "This is not a Speeduino definition: the Speeduino base tunes do not apply to it.",
            t->warning);
        return 0;
    }
    if (b == NULL) { return 0; }
    m = lt_sig_compare(b->signature, isig, NULL);
    lt_ui_label_wrap(ui, m == LT_SIG_EXACT
        ? "Same firmware version. The current tune is replaced (save to keep it); values that do not "
          "map are listed first."
        : "Different firmware version: the migration report shows what does not map before loading.",
        m == LT_SIG_EXACT ? t->success : t->info);
    return 1;
}

static void bt_act(lt_app *app, bt_dlg *s, const lt_basetune *b, const char *ini_path, int *open)
{
    lt_ini *ini;
    if (s->mode == 2) {
        if (app->ini == NULL) { return; }
        bt_submit(app, 2, b, app->ini, "", "");
        *open = 0;
        return;
    }
    ini = lt_ini_load_file(ini_path, NULL);
    if (ini == NULL) {
        lt_snprintf(s->err, sizeof(s->err), "Cannot load INI: %s", lt_err_get());
        return;
    }
    if (lt_basetune_family(ini->signature) == LT_BASETUNE_FAM_RUSEFI) {
        lt_strlcpy(s->err, "This INI is not a Speeduino definition. " BT_RUSEFI_NOTE, sizeof(s->err));
        lt_ini_free(ini);
        return;
    }
    bt_submit(app, 1, b, ini, ini_path, s->name);
    lt_ini_free(ini);
    *open = 0;
}

static void bt_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    bt_dlg *s = (bt_dlg *)d->data;
    const lt_basetune *b;
    const char *picked;
    int open = d->open, w1 = -1, wf[4], wl[2], wb[3], i, can = 0, cell, activated = 0;
    char ini_path[512];
    if (s == NULL) { bt_init(app, d); s = (bt_dlg *)d->data; }
    cell = lt_ui_cell_mode(ui);
    picked = lt_app_file_result(app, FD_BT_INI);
    if (picked != NULL) { lt_strlcpy(s->ini_file, picked, sizeof(s->ini_file)); s->ini_src = 1; }
    if (s->gen != lt_feat_online_generation(app)) { repo_reload(app, s); }

    /* filter, keep the selection when it is still listed */
    s->n_idx = (int)lt_basetune_filter(s->make_sel > 0 ? s->makes[s->make_sel] : NULL, s->query, s->idx,
                                       BT_MAX_LIST);
    if (s->n_idx > BT_MAX_LIST) { s->n_idx = BT_MAX_LIST; }
    s->pos = -1;
    for (i = 0; i < s->n_idx; i++) {
        if ((int)s->idx[i] == s->cur) { s->pos = i; }
    }
    if (s->pos < 0) { s->pos = 0; s->cur = s->n_idx > 0 ? (int)s->idx[0] : -1; }
    b = s->cur >= 0 ? lt_basetune_at((size_t)s->cur) : NULL;
    if (s->shown != s->cur) {
        s->shown = s->cur;
        s->n_m = 0;
        s->ini_sel = 0;
        s->err[0] = '\0';
        if (b != NULL && s->repo_ok) {   /* exact signature first, then other versions */
            size_t nm = lt_inilib_find(&s->repo, b->signature, s->m, BT_MAX_INIS);
            s->n_m = nm > BT_MAX_INIS ? BT_MAX_INIS : (int)nm;
        }
        if (b != NULL) { lt_strlcpy(s->name, b->name, sizeof(s->name)); }
    }

    if (lt_ui_modal_begin(ui, s->mode == 1 ? "New Project from Base Tune" : "Load Base Tune",
                          940, s->mode == 1 ? 640 : 560, &open)) {
        wf[0] = 44; wf[1] = 170; wf[2] = 56; wf[3] = -1;
        lt_ui_layout_row(ui, 4, wf, 0);
        lt_ui_label_ex(ui, "Make", t->text2, LT_UI_ALIGN_LEFT);
        (void)lt_ui_combo(ui, "btmake", &s->make_sel, s->makes, s->n_makes);
        lt_ui_label_ex(ui, "Search", t->text2, LT_UI_ALIGN_LEFT);
        (void)lt_ui_text(ui, "btsearch", s->query, sizeof(s->query), 0);

        wl[0] = -2; wl[1] = -3;
        lt_ui_layout_row(ui, 2, wl, cell ? 160 : 270);
        if (s->n_idx > 0) {
            int p = s->pos;
            /* Enter / double click on a tune = the dialog's action */
            if (lt_ui_listbox_cb(ui, "btlist", &p, s->n_idx, row_get, s) & LT_UI_ACTIVATED) {
                activated = 1;
            }
            if (p >= 0 && p < s->n_idx && p != s->pos) {
                s->pos = p;
                s->cur = (int)s->idx[p];
                b = lt_basetune_at((size_t)s->cur);
            }
        } else {
            lt_ui_label_ex(ui, "No base tune matches.", t->muted, LT_UI_ALIGN_CENTER);
        }
        if (lt_ui_scroll_begin(ui, "btdetail")) {
            details(ui, s->n_idx > 0 ? b : NULL);
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, BT_RUSEFI_NOTE, t->muted);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_separator(ui);

        ini_path[0] = '\0';
        if (s->mode == 1) {
            section_new(app, s, s->n_idx > 0 ? b : NULL, ini_path, sizeof(ini_path));
            can = s->n_idx > 0 && ini_path[0] != '\0' && s->name[0] != '\0';
        } else {
            can = section_load(app, s->n_idx > 0 ? b : NULL) && s->n_idx > 0;
        }

        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, BT_CAUTION, t->warning);
        if (s->err[0] != '\0') {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, s->err, t->error);
        }
        wb[0] = -1; wb[1] = 170; wb[2] = 96;
        lt_ui_layout_row(ui, 3, wb, 0);
        {
            char credit[200];
            lt_snprintf(credit, sizeof(credit), "Speeduino Tunes \xC2\xA9 %s, %s", LT_BASETUNE_AUTHORS,
                        LT_BASETUNE_LICENSE);
            lt_ui_label_ex(ui, credit, t->muted, LT_UI_ALIGN_LEFT);
        }
        if (lt_ui_button_ex(ui, s->mode == 1 ? "Create Project" : "Load Base Tune",
                            can ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) { activated = 1; }
        if (activated && can && b != NULL && s->shown == s->cur) {
            bt_act(app, s, b, ini_path, &open);
        }
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void bt_close(lt_app *app, lt_app_dialog *d)
{
    bt_dlg *s = (bt_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
        lt_free(s);
    }
    d->data = NULL;
}

void lt_feat_basetune_open(lt_app *app, const char *mode)
{
    const char *m = mode != NULL ? mode : "";
    (void)lt_app_dialog_push(app, "basetunes", m, strcmp(m, "new") == 0 ? "New Project from Base Tune"
                                                                          : "Base Tunes");
}

static void sw_new(lt_app *app, const char *mode) { lt_feat_basetune_open(app, mode); }
static void act_new(lt_app *app) { lt_feat_guard_switch(app, sw_new, "new"); }
static void act_load(lt_app *app) { lt_feat_basetune_open(app, "load"); }

void lt_feat_basetune_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "basetunes"; dk.draw = bt_draw; dk.close = bt_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_tick(reg, bt_tick);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "New Project from &Base Tune...", NULL, act_new, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Load Base T&une...", NULL, act_load, lt_ext_pred_ini);
}
