/* feat_msqpart.c - partial tunes (.msqpart, as in TunerStudio): save a subset of the open tune and
 * load one back, touching only the constants it contains.
 *   File > Save Partial Tune...   either the constants that differ from a reference (the saved
 *                                 project tune, a tune file, a restore point or the INI defaults)
 *                                 or the constants behind chosen tables, curves and dialogs;
 *                                 the values written are those of the open tune (ECU when online)
 *   File > Load Partial Tune...   preview (current value -> file value per constant), apply, and
 *                                 Undo of the last load (the previous values are kept as a partial
 *                                 tune of their own)
 * Dialog kinds "msqpart_save" and "msqpart_load". The file format and the subset logic live in
 * tune/tune_part.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "feat.h"
#include "../tune/tunecmp.h"

#define FD_MP_SAVE (LT_APP_FD_USER0 + 140)
#define FD_MP_LOAD (LT_APP_FD_USER0 + 141)
#define FD_MP_REF (LT_APP_FD_USER0 + 142)
#define MP_FILTERS "Partial Tune|*.msqpart|Tune Files|*.msq;*.msqpart|All Files|*"
#define MP_ROWS_MAX 300

enum { MP_REF_PROJECT = 0, MP_REF_FILE, MP_REF_RESTORE, MP_REF_DEFAULTS, MP_REF_N };

typedef struct mp_target { char kind; const char *name, *title; } mp_target;

typedef struct mp_state {
    /* the last load, for Undo */
    lt_tune *undo;
    char undo_what[200];
    /* save dialog */
    int s_ready, mode, ref, computed;
    char ref_path[512];
    lt_restore_info *rp; size_t n_rp; int rp_sel;
    char **dnames; size_t nd; unsigned char *dsel;
    lt_tune *dref, *dcur;           /* the differing values on both sides, for display */
    mp_target *tg; size_t ntg; unsigned char *tsel;
    char filter[64];
    int only_sel;
    char save_msg[300]; int save_err;
    /* load dialog */
    char load_path[512], loaded_path[512];
    lt_tune *part, *cur;
    char **lnames; size_t nl;
    char load_msg[300]; int load_err;
} mp_state;

static void strv_free(char **v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { lt_free(v[i]); }
    lt_free(v);
}

static void save_clear(mp_state *s)
{
    strv_free(s->dnames, s->nd);
    s->dnames = NULL; s->nd = 0;
    lt_free(s->dsel); s->dsel = NULL;
    if (s->dref != NULL) { lt_tune_free(s->dref); s->dref = NULL; }
    if (s->dcur != NULL) { lt_tune_free(s->dcur); s->dcur = NULL; }
    lt_free(s->tg); s->tg = NULL; s->ntg = 0;
    lt_free(s->tsel); s->tsel = NULL;
    if (s->rp != NULL) { lt_restore_info_free(s->rp, s->n_rp); s->rp = NULL; s->n_rp = 0; }
    s->s_ready = 0; s->computed = 0;
}

static void load_clear(mp_state *s)
{
    if (s->part != NULL) { lt_tune_free(s->part); s->part = NULL; }
    if (s->cur != NULL) { lt_tune_free(s->cur); s->cur = NULL; }
    strv_free(s->lnames, s->nl);
    s->lnames = NULL; s->nl = 0;
    s->loaded_path[0] = '\0';
}

static void st_free(lt_app *app, void *p)
{
    mp_state *s = (mp_state *)p;
    LT_UNUSED(app);
    save_clear(s);
    load_clear(s);
    if (s->undo != NULL) { lt_tune_free(s->undo); s->undo = NULL; }
}

static mp_state *st_get(lt_app *app)
{
    return (mp_state *)lt_ext_state(app, "msqpart", sizeof(mp_state), st_free);
}

static int online(const lt_app *app)
{
    return app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO);
}

/* the open tune brought up to date with the ECU shadow and its named values captured */
static lt_tune *current_copy(lt_app *app)
{
    if (app->tune == NULL || app->ini == NULL) { return NULL; }
    if (online(app)) { (void)lt_tune_from_mem(app->tune, app->mem); }
    (void)lt_tune_capture_from_pages(app->tune, app->ini);
    return lt_tune_clone(app->tune);
}

/* ---- scripting / shared entry points ---- */
int lt_feat_msqpart_save(lt_app *app, const char *path, const char *const *names, size_t n)
{
    lt_tune *cur = current_copy(app), *part;
    int rc;
    if (cur == NULL) { lt_err_set("Open a tune first"); return LT_EINVAL; }
    if (n == 0) { lt_tune_free(cur); lt_err_set("No constants to save"); return LT_EINVAL; }
    part = lt_tune_part_extract(cur, names, n);
    rc = lt_tune_save(part, path);
    lt_tune_free(part);
    lt_tune_free(cur);
    return rc;
}

/* apply a partial tune to the open tune / ECU; the previous values become the Undo copy */
static int apply_part(lt_app *app, const lt_tune *part, const char *what, int keep_undo, size_t *applied,
                      size_t *skipped)
{
    mp_state *s = st_get(app);
    lt_pagesrc ps;
    lt_tune *un = NULL;
    size_t a = 0, k = 0;
    int rc;
    if (app->ini == NULL || app->tune == NULL) { lt_err_set("Open a tune first"); return LT_EINVAL; }
    ps = lt_app_pages(app);
    rc = lt_tune_part_apply(app->ini, &ps, app->tune, part, keep_undo ? &un : NULL, &a, &k);
    if (rc == 0 && a > 0) {
        if (keep_undo) {
            if (s->undo != NULL) { lt_tune_free(s->undo); }
            s->undo = un;
            un = NULL;
            lt_strlcpy(s->undo_what, what, sizeof(s->undo_what));
        }
        if (online(app)) { (void)lt_tune_from_mem(app->tune, app->mem); }
        (void)lt_tune_capture_from_pages(app->tune, app->ini);
        lt_app_edited(app);
    }
    if (un != NULL) { lt_tune_free(un); }
    if (applied != NULL) { *applied = a; }
    if (skipped != NULL) { *skipped = k; }
    return rc;
}

int lt_feat_msqpart_apply_file(lt_app *app, const char *path, size_t *applied, size_t *skipped)
{
    lt_tune *part = lt_tune_load(path);
    int rc;
    if (applied != NULL) { *applied = 0; }
    if (skipped != NULL) { *skipped = 0; }
    if (part == NULL) { return LT_EDATA; }
    rc = apply_part(app, part, lt_path_base(path), 1, applied, skipped);
    lt_tune_free(part);
    return rc;
}

int lt_feat_msqpart_undo(lt_app *app)
{
    mp_state *s = st_get(app);
    size_t a = 0;
    int rc;
    if (s->undo == NULL) { lt_err_set("Nothing to undo"); return LT_ENOENT; }
    rc = apply_part(app, s->undo, "", 0, &a, NULL);
    lt_tune_free(s->undo);
    s->undo = NULL;
    s->undo_what[0] = '\0';
    return rc;
}

int lt_feat_msqpart_can_undo(lt_app *app)
{
    return st_get(app)->undo != NULL ? 1 : 0;
}

/* ---- save dialog ---- */
static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (hay == NULL) { return 0; }
    for (; *hay != '\0'; hay++) {
        if (lt_strnicmp(hay, needle, n) == 0) { return 1; }
    }
    return n == 0;
}

static int cmp_target(const void *a, const void *b)
{
    const mp_target *x = (const mp_target *)a, *y = (const mp_target *)b;
    return lt_stricmp(x->title, y->title);
}

static void build_targets(lt_app *app, mp_state *s)
{
    const lt_ini *ini = app->ini;
    size_t i, n = 0, cap = ini->n_tables + ini->n_curves + ini->n_dialogs + 1;
    s->tg = (mp_target *)lt_calloc(cap, sizeof(mp_target));
    for (i = 0; i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        if (t->name == NULL) { continue; }
        s->tg[n].kind = 'T'; s->tg[n].name = t->name; s->tg[n].title = t->title != NULL && t->title[0] ? t->title : t->name;
        n++;
    }
    for (i = 0; i < ini->n_curves; i++) {
        const lt_ini_curve *c = ini->curves[i];
        if (c->name == NULL) { continue; }
        s->tg[n].kind = 'C'; s->tg[n].name = c->name; s->tg[n].title = c->title != NULL && c->title[0] ? c->title : c->name;
        n++;
    }
    for (i = 0; i < ini->n_dialogs; i++) {
        const lt_ini_dialog *d = ini->dialogs[i];
        lt_map set;
        size_t k;
        if (d->name == NULL) { continue; }
        lt_map_init(&set, LT_FALSE);
        k = lt_tune_part_target_consts(ini, d->name, &set);
        lt_map_free(&set);
        if (k == 0) { continue; }                     /* dialogs without editable constants */
        s->tg[n].kind = 'D'; s->tg[n].name = d->name; s->tg[n].title = d->title != NULL && d->title[0] ? d->title : d->name;
        n++;
    }
    qsort(s->tg, n, sizeof(mp_target), cmp_target);
    s->ntg = n;
    s->tsel = (unsigned char *)lt_calloc(n + 1, 1);
}

static lt_tune *load_reference(lt_app *app, mp_state *s, char *err, size_t cap)
{
    lt_tune *t = NULL;
    char *p = NULL;
    switch (s->ref) {
    case MP_REF_PROJECT:
        if (!app->has_project) { lt_strlcpy(err, "No project is open", cap); return NULL; }
        p = lt_project_file(&app->project, "CurrentTune.msq");
        if (!lt_file_exists(p)) { lt_strlcpy(err, "The project has no saved tune yet", cap); lt_free(p); return NULL; }
        t = lt_tune_load(p);
        break;
    case MP_REF_FILE:
        if (s->ref_path[0] == '\0') { lt_strlcpy(err, "Choose a reference tune file", cap); return NULL; }
        t = lt_tune_load(s->ref_path);
        break;
    case MP_REF_RESTORE:
        if (s->n_rp == 0 || lt_restore_path(&app->project, s->rp[s->rp_sel].filename, &p) != 0) {
            lt_strlcpy(err, "No restore point selected", cap);
            lt_free(p);
            return NULL;
        }
        t = lt_tune_load(p);
        break;
    default:
        t = lt_tune_from_defaults(app->ini);
        break;
    }
    lt_free(p);
    if (t == NULL) { lt_snprintf(err, cap, "Cannot load the reference: %s", lt_err_get()); }
    return t;
}

static void compute_diff(lt_app *app, mp_state *s)
{
    lt_tune *ref, *cur;
    size_t i;
    strv_free(s->dnames, s->nd);
    s->dnames = NULL; s->nd = 0;
    lt_free(s->dsel); s->dsel = NULL;
    if (s->dref != NULL) { lt_tune_free(s->dref); s->dref = NULL; }
    if (s->dcur != NULL) { lt_tune_free(s->dcur); s->dcur = NULL; }
    s->save_msg[0] = '\0'; s->save_err = 0;
    s->computed = 1;
    ref = load_reference(app, s, s->save_msg, sizeof(s->save_msg));
    if (ref == NULL) { s->save_err = 1; return; }
    cur = current_copy(app);
    if (cur == NULL) { lt_tune_free(ref); lt_strlcpy(s->save_msg, "No tune is open", sizeof(s->save_msg)); s->save_err = 1; return; }
    s->nd = lt_tune_part_diff_names(cur, ref, &s->dnames);
    s->dsel = (unsigned char *)lt_calloc(s->nd + 1, 1);
    for (i = 0; i < s->nd; i++) { s->dsel[i] = 1; }
    s->dref = lt_tune_part_extract(ref, (const char *const *)s->dnames, s->nd);
    s->dcur = lt_tune_part_extract(cur, (const char *const *)s->dnames, s->nd);
    lt_tune_free(cur);
    lt_tune_free(ref);
}

/* names chosen in the dialog (malloc'd vector of borrowed / owned strings: see own) */
static size_t chosen_names(lt_app *app, mp_state *s, char ***out)
{
    char **v;
    size_t i, n = 0;
    if (s->mode == 0) {
        v = (char **)lt_malloc((s->nd + 1) * sizeof(char *));
        for (i = 0; i < s->nd; i++) { if (s->dsel[i]) { v[n++] = lt_strdup(s->dnames[i]); } }
    } else {
        lt_map set;
        lt_map_init(&set, LT_FALSE);
        for (i = 0; i < s->ntg; i++) {
            if (s->tsel[i]) { (void)lt_tune_part_target_consts(app->ini, s->tg[i].name, &set); }
        }
        v = (char **)lt_malloc((set.len + 1) * sizeof(char *));
        for (i = 0; i < set.cap; i++) { if (set.slots[i].used == 1) { v[n++] = lt_strdup(set.slots[i].key); } }
        lt_map_free(&set);
    }
    *out = v;
    return n;
}

static const char *rp_get(void *ud, int i)
{
    const mp_state *s = (const mp_state *)ud;
    static char buf[200];
    lt_snprintf(buf, sizeof(buf), "%s   %s", s->rp[i].created, s->rp[i].filename);
    return buf;
}

static void draw_diff_list(lt_app *app, mp_state *s, int h)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[2], wr[4], all = 1, on;
    size_t i, shown = s->nd < MP_ROWS_MAX ? s->nd : MP_ROWS_MAX, nsel = 0;
    char b[160], va[96], vb[96];
    for (i = 0; i < s->nd; i++) { if (!s->dsel[i]) { all = 0; } else { nsel++; } }
    w[0] = 28; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    if (lt_ui_checkbox(ui, "", &all)) { for (i = 0; i < s->nd; i++) { s->dsel[i] = (unsigned char)all; } }
    lt_snprintf(b, sizeof(b), "%lu of %lu differing constant%s selected", (unsigned long)nsel, (unsigned long)s->nd,
                s->nd == 1 ? "" : "s");
    lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
    wr[0] = 28; wr[1] = -3; wr[2] = -2; wr[3] = -2;
    lt_ui_layout_row(ui, 4, wr, 0);
    lt_ui_label(ui, "");
    lt_ui_label_ex(ui, "Constant", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Open tune", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Reference", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, h);
    if (lt_ui_scroll_begin(ui, "mpdiff")) {
        if (s->nd == 0) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "No differences: the open tune matches the reference.", t->muted, LT_UI_ALIGN_LEFT);
        }
        for (i = 0; i < shown; i++) {
            const lt_tune_entry *ea = s->dcur != NULL ? lt_tune_get_value(s->dcur, s->dnames[i]) : NULL;
            const lt_tune_entry *eb = s->dref != NULL ? lt_tune_get_value(s->dref, s->dnames[i]) : NULL;
            on = s->dsel[i];
            lt_ui_push_id_int(ui, (int)i);
            lt_tcmp_format(ea != NULL ? &ea->value : NULL, va, sizeof(va));
            lt_tcmp_format(eb != NULL ? &eb->value : NULL, vb, sizeof(vb));
            lt_ui_layout_row(ui, 4, wr, 0);
            if (lt_ui_checkbox(ui, "", &on)) { s->dsel[i] = (unsigned char)(on ? 1 : 0); }
            lt_ui_label(ui, s->dnames[i]);
            lt_ui_label_ex(ui, va, t->success, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, vb, t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
}

static void draw_target_list(lt_app *app, mp_state *s, int h)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[4], on;
    size_t i, shown = 0;
    static const char *const kinds[3] = { "Table", "Curve", "Dialog" };
    w[0] = 160; w[1] = -1; w[2] = 190;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "Filter:");
    (void)lt_ui_text(ui, "mpfilter", s->filter, sizeof(s->filter), 0);
    (void)lt_ui_checkbox(ui, "Only the chosen ones", &s->only_sel);
    lt_ui_layout_row(ui, 1, &w1, h);
    if (lt_ui_scroll_begin(ui, "mptargets")) {
        w[0] = 28; w[1] = 70; w[2] = -2; w[3] = -1;
        for (i = 0; i < s->ntg && shown < MP_ROWS_MAX; i++) {
            const mp_target *g = &s->tg[i];
            if (s->filter[0] != '\0' && !contains_ci(g->title, s->filter) && !contains_ci(g->name, s->filter)) {
                continue;
            }
            if (s->only_sel && !s->tsel[i]) { continue; }
            shown++;
            on = s->tsel[i];
            lt_ui_push_id_int(ui, (int)i);
            lt_ui_layout_row(ui, 4, w, 0);
            if (lt_ui_checkbox(ui, "", &on)) { s->tsel[i] = (unsigned char)(on ? 1 : 0); }
            lt_ui_label_ex(ui, kinds[g->kind == 'T' ? 0 : (g->kind == 'C' ? 1 : 2)], t->muted, LT_UI_ALIGN_LEFT);
            lt_ui_label(ui, g->title);
            lt_ui_label_ex(ui, g->name, t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
}

static void do_save(lt_app *app, mp_state *s, const char *picked)
{
    char path[600], **names = NULL;
    size_t n = chosen_names(app, s, &names), l;
    int rc;
    lt_strlcpy(path, picked, sizeof(path));
    l = strlen(path);
    if (lt_path_ext(path)[0] == '\0' && l + 9 < sizeof(path)) { lt_strlcat(path, ".msqpart", sizeof(path)); }
    rc = lt_feat_msqpart_save(app, path, (const char *const *)names, n);
    if (rc == 0) {
        lt_snprintf(s->save_msg, sizeof(s->save_msg), "Saved %lu constant%s to %s", (unsigned long)n, n == 1 ? "" : "s",
                    lt_path_base(path));
        s->save_err = 0;
        lt_app_toast(app, 1, "Partial tune saved: %s", lt_path_base(path));
    } else {
        lt_snprintf(s->save_msg, sizeof(s->save_msg), "Cannot save: %s", lt_err_get());
        s->save_err = 1;
    }
    strv_free(names, n);
}

static void save_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    mp_state *s = st_get(app);
    static const char *const modes[2] = { "Differences from a reference", "Chosen tables, curves and dialogs" };
    static const char *const refs[MP_REF_N] = { "Saved project tune", "Tune file...", "Restore point", "INI defaults" };
    int open = d->open, w1 = -1, w[4], cell = lt_ui_cell_mode(ui), prev_ref, prev_mode, can;
    const char *picked;
    char **names = NULL;
    size_t n;
    char b[200];
    if (app->ini == NULL) { d->open = 0; return; }
    if (!s->s_ready) {
        char *e = lt_plat_getenv("LT_MSQPART_MODE");    /* scripted use (screenshots): 1 = chosen targets */
        s->s_ready = 1;
        s->ref = app->has_project ? MP_REF_PROJECT : MP_REF_DEFAULTS;
        if (app->has_project) { (void)lt_restore_list(&app->project, &s->rp, &s->n_rp); }
        build_targets(app, s);
        if (e != NULL && e[0] == '1') { s->mode = 1; }
        lt_free(e);
        e = lt_plat_getenv("LT_MSQPART_PICK");          /* scripted use: preselect targets by name */
        if (e != NULL) {
            size_t i;
            for (i = 0; i < s->ntg; i++) { if (strstr(e, s->tg[i].name) != NULL) { s->tsel[i] = 1; s->only_sel = 1; } }
            lt_free(e);
        }
    }
    picked = lt_app_file_result(app, FD_MP_REF);
    if (picked != NULL) { lt_strlcpy(s->ref_path, picked, sizeof(s->ref_path)); s->ref = MP_REF_FILE; s->computed = 0; }
    picked = lt_app_file_result(app, FD_MP_SAVE);
    if (picked != NULL) { do_save(app, s, picked); }
    if (s->mode == 0 && !s->computed) { compute_diff(app, s); }
    if (lt_ui_modal_begin(ui, "Save Partial Tune", 900, 640, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Write some constants of the open tune to a .msqpart file (TunerStudio partial tune format).",
                       t->text2, LT_UI_ALIGN_LEFT);
        w[0] = 160; w[1] = 330; w[2] = -1;
        lt_ui_layout_row(ui, 3, w, 0);
        lt_ui_label(ui, "Save:");
        prev_mode = s->mode;
        (void)lt_ui_combo(ui, "mpmode", &s->mode, modes, 2);
        lt_ui_label(ui, "");
        if (s->mode != prev_mode) { s->computed = 0; s->save_msg[0] = '\0'; }
        if (s->mode == 0) {
            w[0] = 160; w[1] = 220; w[2] = -1; w[3] = 110;
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label(ui, "Reference:");
            prev_ref = s->ref;
            (void)lt_ui_combo(ui, "mpref", &s->ref, refs, MP_REF_N);
            if (s->ref == MP_REF_FILE) {
                (void)lt_ui_text(ui, "mprefpath", s->ref_path, sizeof(s->ref_path), 0);
                if (lt_ui_button(ui, "Browse...")) {
                    lt_app_file_dialog(app, FD_MP_REF, LT_UI_FILE_OPEN, NULL, "", "Tune Files|*.msq;*.msqpart|All Files|*");
                }
            } else if (s->ref == MP_REF_RESTORE && s->n_rp > 0) {
                int prev = s->rp_sel;
                (void)lt_ui_combo_cb(ui, "mprp", &s->rp_sel, (int)s->n_rp, rp_get, s);
                if (prev != s->rp_sel) { s->computed = 0; }
                lt_ui_label(ui, "");
            } else {
                lt_ui_label_ex(ui, s->ref == MP_REF_DEFAULTS ? "values from the INI [Defaults]"
                               : (s->ref == MP_REF_PROJECT ? "CurrentTune.msq of the project" : "no restore points"),
                               t->muted, LT_UI_ALIGN_LEFT);
                lt_ui_label(ui, "");
            }
            if (s->ref != prev_ref) { s->computed = 0; }
            w[0] = 160; w[1] = -1;
            lt_ui_layout_row(ui, 2, w, 0);
            if (lt_ui_button(ui, "Compare again")) { s->computed = 0; }
            lt_ui_label_ex(ui, "Values written are those of the open tune (the ECU when connected).", t->muted,
                           LT_UI_ALIGN_LEFT);
            if (s->computed) { draw_diff_list(app, s, -(cell ? 40 : 76)); }
        } else {
            draw_target_list(app, s, -(cell ? 40 : 76));
        }
        n = chosen_names(app, s, &names);
        can = n > 0;
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->save_msg[0] != '\0') { lt_ui_label_ex(ui, s->save_msg, s->save_err ? t->error : t->success, LT_UI_ALIGN_LEFT); }
        else {
            lt_snprintf(b, sizeof(b), "%lu constant%s will be written", (unsigned long)n, n == 1 ? "" : "s");
            lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
        }
        strv_free(names, n);
        w[0] = -1; w[1] = 150; w[2] = 100;
        lt_ui_layout_row(ui, 3, w, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Save As...", can ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && can) {
            lt_app_file_dialog(app, FD_MP_SAVE, LT_UI_FILE_SAVE, NULL, "partial.msqpart", MP_FILTERS);
        }
        if (lt_ui_button(ui, "Close")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void save_close(lt_app *app, lt_app_dialog *d)
{
    save_clear(st_get(app));
    d->data = NULL;
}

/* ---- load dialog ---- */
static void load_preview(lt_app *app, mp_state *s)
{
    lt_pagesrc ps;
    load_clear(s);
    s->load_msg[0] = '\0'; s->load_err = 0;
    lt_strlcpy(s->loaded_path, s->load_path, sizeof(s->loaded_path));
    if (s->load_path[0] == '\0') { return; }
    s->part = lt_tune_load(s->load_path);
    if (s->part == NULL) {
        lt_snprintf(s->load_msg, sizeof(s->load_msg), "Cannot read: %s", lt_err_get());
        s->load_err = 1;
        return;
    }
    s->nl = lt_tune_part_names(s->part, &s->lnames);
    ps = lt_app_pages(app);
    s->cur = lt_tune_part_capture(app->ini, &ps, app->tune, (const char *const *)s->lnames, s->nl);
}

void lt_feat_msqpart_open_load(lt_app *app, const char *path)
{
    mp_state *s = st_get(app);
    lt_strlcpy(s->load_path, path != NULL ? path : "", sizeof(s->load_path));
    s->loaded_path[0] = '\1';                           /* force a fresh preview */
    (void)lt_app_dialog_push(app, "msqpart_load", "", "Load Partial Tune");
}

static void load_rows(lt_app *app, mp_state *s, int h, size_t *n_change, size_t *n_unknown)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[4];
    size_t i;
    char a[96], b[96];
    *n_change = 0; *n_unknown = 0;
    w[0] = -3; w[1] = -2; w[2] = -2; w[3] = 110;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label_ex(ui, "Constant", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Current value", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Value in the file", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, h);
    if (lt_ui_scroll_begin(ui, "mprows")) {
        for (i = 0; i < s->nl; i++) {
            const lt_tune_entry *nv = lt_tune_get_value(s->part, s->lnames[i]);
            const lt_tune_entry *cv = s->cur != NULL ? lt_tune_get_value(s->cur, s->lnames[i]) : NULL;
            int known = app->ini != NULL && lt_ini_find_const(app->ini, s->lnames[i]) != NULL;
            int same = known && cv != NULL && nv != NULL && lt_tune_value_equal(&cv->value, &nv->value);
            if (!known) { (*n_unknown)++; } else if (!same) { (*n_change)++; }
            if (i >= MP_ROWS_MAX) { continue; }
            lt_tcmp_format(cv != NULL ? &cv->value : NULL, a, sizeof(a));
            lt_tcmp_format(nv != NULL ? &nv->value : NULL, b, sizeof(b));
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label_ex(ui, s->lnames[i], known ? (same ? t->text2 : t->text) : t->muted, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, known ? a : "-", t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, b, same ? t->text2 : t->success, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, !known ? "not in this INI" : (same ? "unchanged" : "changes"),
                           !known ? t->warning : (same ? t->muted : t->primary), LT_UI_ALIGN_LEFT);
        }
        lt_ui_scroll_end(ui);
    }
}

static void load_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    mp_state *s = st_get(app);
    int open = d->open, w1 = -1, w[4], cell = lt_ui_cell_mode(ui), can;
    const char *picked;
    size_t nch = 0, nunk = 0;
    char b[300];
    if (app->ini == NULL) { d->open = 0; return; }
    picked = lt_app_file_result(app, FD_MP_LOAD);
    if (picked != NULL) { lt_strlcpy(s->load_path, picked, sizeof(s->load_path)); }
    if (strcmp(s->load_path, s->loaded_path) != 0) { load_preview(app, s); }
    if (lt_ui_modal_begin(ui, "Load Partial Tune", 900, 640, &open)) {
        w[0] = 110; w[1] = -1; w[2] = 110;
        lt_ui_layout_row(ui, 3, w, 0);
        lt_ui_label(ui, "File:");
        (void)lt_ui_text(ui, "mplpath", s->load_path, sizeof(s->load_path), 0);
        if (lt_ui_button(ui, "Browse...")) {
            lt_app_file_dialog(app, FD_MP_LOAD, LT_UI_FILE_OPEN, NULL, "", MP_FILTERS);
        }
        if (s->part != NULL) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "Signature: %s%s", s->part->signature != NULL && s->part->signature[0] ? s->part->signature : "(none)",
                        s->part->signature != NULL && app->ini->signature != NULL && s->part->signature[0] &&
                        strcmp(s->part->signature, app->ini->signature) != 0 ? "   (differs from the open ECU definition)" : "");
            lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
            load_rows(app, s, -(cell ? 40 : 76), &nch, &nunk);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "%lu value%s in the file: %lu change%s, %lu unchanged, %lu unknown to this INI (skipped)",
                        (unsigned long)s->nl, s->nl == 1 ? "" : "s", (unsigned long)nch, nch == 1 ? "" : "s",
                        (unsigned long)(s->nl - nch - nunk), (unsigned long)nunk);
            lt_ui_label_ex(ui, s->load_msg[0] != '\0' ? s->load_msg : b,
                           s->load_msg[0] != '\0' ? (s->load_err ? t->error : t->success) : t->text2, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_layout_row(ui, 1, &w1, -(cell ? 20 : 40));
            lt_ui_label_ex(ui, s->load_msg[0] != '\0' ? s->load_msg : "Choose a .msqpart file to preview the values it changes.",
                           s->load_err ? t->error : t->muted, LT_UI_ALIGN_LEFT);
        }
        can = s->part != NULL && nch > 0;
        w[0] = -1; w[1] = 190; w[2] = 140; w[3] = 100;
        lt_ui_layout_row(ui, 4, w, 0);
        if (s->undo != NULL) {
            lt_snprintf(b, sizeof(b), "Undo available for: %s", s->undo_what);
            lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
        } else { lt_ui_label(ui, ""); }
        if (lt_ui_button_ex(ui, "Undo last load", s->undo != NULL ? 0 : LT_UI_BTN_DISABLED) && s->undo != NULL) {
            if (lt_feat_msqpart_undo(app) == 0) {
                lt_strlcpy(s->load_msg, "Previous values restored", sizeof(s->load_msg)); s->load_err = 0;
                lt_app_toast(app, 1, "Partial tune load undone");
            } else { lt_snprintf(s->load_msg, sizeof(s->load_msg), "Undo failed: %s", lt_err_get()); s->load_err = 1; }
            s->loaded_path[0] = '\1';
        }
        if (lt_ui_button_ex(ui, "Apply", can ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && can) {
            size_t a = 0, k = 0;
            if (apply_part(app, s->part, lt_path_base(s->load_path), 1, &a, &k) == 0) {
                lt_snprintf(s->load_msg, sizeof(s->load_msg), "Applied %lu value%s%s", (unsigned long)a, a == 1 ? "" : "s",
                            k > 0 ? " (some skipped)" : "");
                s->load_err = 0;
                lt_app_toast(app, 1, "Partial tune applied: %lu values", (unsigned long)a);
            } else { lt_snprintf(s->load_msg, sizeof(s->load_msg), "Cannot apply: %s", lt_err_get()); s->load_err = 1; }
            lt_strlcpy(b, s->load_msg, sizeof(b));
            load_preview(app, s);
            lt_strlcpy(s->load_msg, b, sizeof(s->load_msg));
        }
        if (lt_ui_button(ui, "Close")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void load_close(lt_app *app, lt_app_dialog *d)
{
    load_clear(st_get(app));
    d->data = NULL;
}

static void act_save(lt_app *app) { (void)lt_app_dialog_push(app, "msqpart_save", "", "Save Partial Tune"); }

static void act_load(lt_app *app)
{
    char *e = lt_plat_getenv("LT_MSQPART_FILE");     /* scripted use (screenshots): preview this file */
    lt_feat_msqpart_open_load(app, e != NULL ? e : "");
    lt_free(e);
}

/* LT_SHOT_OPEN=dlg:msqpart_load goes straight to the dialog: pick up LT_MSQPART_FILE there too */
static void load_draw_entry(lt_app *app, lt_app_dialog *d)
{
    mp_state *s = st_get(app);
    if (d->data == NULL) {
        char *e = lt_plat_getenv("LT_MSQPART_FILE");
        d->data = (void *)s;
        if (e != NULL && s->load_path[0] == '\0') { lt_strlcpy(s->load_path, e, sizeof(s->load_path)); }
        lt_free(e);
    }
    load_draw(app, d);
}

void lt_feat_msqpart_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "msqpart_save"; dk.draw = save_draw; dk.close = save_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "msqpart_load"; dk.draw = load_draw_entry; dk.close = load_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Save &Partial Tune (.msqpart)...", NULL, act_save, lt_ext_pred_ini);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "&Load Partial Tune (.msqpart)...", NULL, act_load, lt_ext_pred_ini);
}
