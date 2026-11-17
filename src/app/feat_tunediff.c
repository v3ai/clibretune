/* feat_tunediff.c - "Compare Tune Files": diff two tunes (the open tune, a tune file or a restore
 * point) constant by constant and merge selected constants from B into the open tune. (G-13)
 * Tools -> Compare Tune Files...   (dialog kind "tunediff")
 * Presentation logic (filter, sort, value text) is in tune/tunecmp.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../tune/tunecmp.h"

#define FD_TD_A (LT_APP_FD_USER0 + 43)
#define FD_TD_B (LT_APP_FD_USER0 + 44)
#define TD_ROWS_MAX 200
#define TD_CURRENT "(current tune)"
#define TD_FILTERS "Tune Files|*.msq;*.json|All Files|*"

typedef struct td_dlg {
    char path_a[512], path_b[512];
    lt_tune_diff *diff;
    lt_tune *tune_b;                 /* kept for the merge */
    unsigned char *sel;              /* per diff entry */
    size_t *idx; size_t nidx;        /* filtered + sorted view */
    int  filter, sort, asc, view_dirty;
    char search[64];
    char err[300];
    char merge_msg[200]; int merge_err;
    lt_restore_info *rp; size_t n_rp; int rp_sel;
} td_dlg;

static void td_clear_result(td_dlg *s)
{
    if (s->diff != NULL) { lt_tune_diff_free(s->diff); s->diff = NULL; }
    if (s->tune_b != NULL) { lt_tune_free(s->tune_b); s->tune_b = NULL; }
    lt_free(s->sel); s->sel = NULL;
    lt_free(s->idx); s->idx = NULL;
    s->nidx = 0;
}

static void td_init(lt_app *app, lt_app_dialog *d)
{
    td_dlg *s = (td_dlg *)lt_calloc(1, sizeof(*s));
    d->data = s;
    lt_strlcpy(s->path_a, TD_CURRENT, sizeof(s->path_a));
    s->asc = 1;
    if (app->has_project) { (void)lt_restore_list(&app->project, &s->rp, &s->n_rp); }
}

static void td_compare(lt_app *app, td_dlg *s);

/* scripted use (screenshots): LT_TUNEDIFF_B=<tune file> opens the dialog already compared */
static void td_preload(lt_app *app, td_dlg *s)
{
    char *b = lt_plat_getenv("LT_TUNEDIFF_B");
    if (b != NULL && b[0] != '\0') {
        lt_strlcpy(s->path_b, b, sizeof(s->path_b));
        td_compare(app, s);
    }
    lt_free(b);
}

/* the open tune, brought up to date with the ECU shadow */
static lt_tune *current_tune_copy(lt_app *app)
{
    if (app->tune == NULL) { return NULL; }
    if (app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        (void)lt_tune_from_mem(app->tune, app->mem);
    }
    if (app->ini != NULL) { (void)lt_tune_capture_from_pages(app->tune, app->ini); }
    return lt_tune_clone(app->tune);
}

static void td_compare(lt_app *app, td_dlg *s)
{
    lt_tune *a;
    td_clear_result(s);
    s->err[0] = '\0';
    s->merge_msg[0] = '\0';
    if (strcmp(s->path_a, TD_CURRENT) == 0) {
        a = current_tune_copy(app);
        if (a == NULL) { lt_strlcpy(s->err, "No tune is open", sizeof(s->err)); return; }
    } else {
        a = lt_tune_load(s->path_a);
        if (a == NULL) {
            lt_snprintf(s->err, sizeof(s->err), "Cannot load tune A: %s", lt_err_get());
            return;
        }
    }
    s->tune_b = lt_tune_load(s->path_b);
    if (s->tune_b == NULL) {
        lt_snprintf(s->err, sizeof(s->err), "Cannot load tune B: %s", lt_err_get());
        lt_tune_free(a);
        return;
    }
    s->diff = lt_tune_diff_compare(a, s->tune_b);
    lt_tune_free(a);
    s->sel = (unsigned char *)lt_calloc(s->diff->n + 1, 1);
    s->idx = (size_t *)lt_calloc(s->diff->n + 1, sizeof(size_t));
    s->view_dirty = 1;
}

static size_t sel_count(const td_dlg *s)
{
    size_t i, n = 0;
    if (s->diff == NULL) { return 0; }
    for (i = 0; i < s->diff->n; i++) { if (s->sel[i]) { n++; } }
    return n;
}

static void td_merge(lt_app *app, td_dlg *s)
{
    const char **names;
    size_t i, n = 0, merged;
    int pg;
    lt_pagesrc ps;
    if (s->diff == NULL || s->tune_b == NULL || app->tune == NULL || app->ini == NULL) { return; }
    names = (const char **)lt_calloc(s->diff->n + 1, sizeof(char *));
    for (i = 0; i < s->diff->n; i++) { if (s->sel[i]) { names[n++] = s->diff->diffs[i].name; } }
    if (n == 0) { lt_free((void *)names); return; }
    (void)current_tune_copy(app);   /* syncs app->tune with the ECU shadow; the copy is not needed */
    merged = lt_tune_merge_selected(app->tune, s->tune_b, names, n);
    lt_free((void *)names);
    (void)lt_tune_apply_to_pages(app->tune, app->ini);
    ps = lt_app_pages(app);
    if (app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        for (pg = 0; pg < 256; pg++) {
            if (app->tune->pages[pg] != NULL && ps.write != NULL) {
                (void)ps.write(ps.ud, pg, 0, app->tune->pages[pg], app->tune->page_len[pg]);
            }
        }
    }
    lt_app_edited(app);
    td_compare(app, s);
    lt_snprintf(s->merge_msg, sizeof(s->merge_msg), "Successfully merged %lu constants", (unsigned long)merged);
    s->merge_err = 0;
}

static void td_rebuild_view(td_dlg *s)
{
    if (s->diff == NULL) { return; }
    s->nidx = lt_tcmp_select(s->diff, s->filter, s->search, s->sort, s->asc, s->idx);
    s->view_dirty = 0;
}

static const char *rp_get(void *ud, int i)
{
    td_dlg *s = (td_dlg *)ud;
    static char buf[200];
    lt_snprintf(buf, sizeof(buf), "%s   %s", s->rp[i].created, s->rp[i].filename);
    return buf;
}

static void file_row(lt_app *app, td_dlg *s, const char *label, char *buf, size_t cap, int purpose,
                     const char *id)
{
    lt_ui *ui = app->ui;
    int w[3];
    w[0] = 130; w[1] = -1; w[2] = 110;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, label);
    if (lt_ui_text(ui, id, buf, cap, 0) & LT_UI_CHANGED) { s->view_dirty = 1; }
    if (lt_ui_button(ui, "Browse...")) {
        lt_app_file_dialog(app, purpose, LT_UI_FILE_OPEN, NULL, "", TD_FILTERS);
    }
}

static void sort_header(lt_ui *ui, td_dlg *s, const char *label, int field)
{
    char buf[40];
    const char *arrow = "";
    if (s->sort == field) { arrow = s->asc ? " ^" : " v"; }
    lt_snprintf(buf, sizeof(buf), "%s%s", label, arrow);
    if (lt_ui_button(ui, buf)) {
        if (s->sort == field) { s->asc = !s->asc; } else { s->sort = field; s->asc = 1; }
        s->view_dirty = 1;
    }
}

static void results(lt_app *app, td_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const fnames[4] = { "All", "Scalar", "Added", "Removed" };
    int w1 = -1, ws[8], wf[6], i, allsel, chg;
    size_t k, shown, nsel_view;
    char buf[200];
    lt_color c;

    ws[0] = 150; ws[1] = 110; ws[2] = 140; ws[3] = 140;
    lt_ui_layout_row(ui, 4, ws, 0);
    lt_snprintf(buf, sizeof(buf), "Signature Match: %s", s->diff->signature_match ? "Yes" : "No");
    lt_ui_label_ex(ui, buf, s->diff->signature_match ? t->success : t->error, LT_UI_ALIGN_LEFT);
    lt_snprintf(buf, sizeof(buf), "Differences: %lu", (unsigned long)s->diff->n);
    lt_ui_label(ui, buf);
    lt_snprintf(buf, sizeof(buf), "Constants A: %lu", (unsigned long)s->diff->sum_a);
    lt_ui_label(ui, buf);
    lt_snprintf(buf, sizeof(buf), "Constants B: %lu", (unsigned long)s->diff->sum_b);
    lt_ui_label(ui, buf);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->diff->n == 0) {
        lt_ui_label_ex(ui, "Tunes are identical!", t->success, LT_UI_ALIGN_CENTER);
        return;
    }
    for (i = 0; i < 4; i++) { wf[i] = 80; }
    wf[4] = -1;
    lt_ui_layout_row(ui, 5, wf, 0);
    for (i = 0; i < 4; i++) {
        if (lt_ui_button_ex(ui, fnames[i], s->filter == i ? LT_UI_BTN_SELECTED : 0)) {
            s->filter = i; s->view_dirty = 1;
        }
    }
    if (lt_ui_text(ui, "tdsearch", s->search, sizeof(s->search), 0) & LT_UI_CHANGED) { s->view_dirty = 1; }
    if (s->search[0] == '\0') { lt_ui_tooltip(ui, "Search constants..."); }
    if (s->view_dirty) { td_rebuild_view(s); }
    shown = s->nidx < TD_ROWS_MAX ? s->nidx : TD_ROWS_MAX;
    nsel_view = 0;
    for (k = 0; k < s->nidx; k++) { if (s->sel[s->idx[k]]) { nsel_view++; } }
    allsel = s->nidx > 0 && nsel_view == s->nidx;

    ws[0] = 28; ws[1] = -4; ws[2] = -3; ws[3] = -3; ws[4] = 80; ws[5] = 64;
    lt_ui_layout_row(ui, 6, ws, 0);
    chg = lt_ui_checkbox(ui, "", &allsel);
    if (chg) { for (k = 0; k < s->nidx; k++) { s->sel[s->idx[k]] = (unsigned char)(allsel ? 1 : 0); } }
    sort_header(ui, s, "Constant", LT_TCMP_SORT_NAME);
    lt_ui_label_ex(ui, "Value A", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Value B", t->text2, LT_UI_ALIGN_LEFT);
    sort_header(ui, s, "Diff", LT_TCMP_SORT_DIFF);
    sort_header(ui, s, "%", LT_TCMP_SORT_PCT);

    lt_ui_layout_row(ui, 1, &w1, -(lt_ui_cell_mode(ui) ? 18 : 36));
    if (lt_ui_scroll_begin(ui, "tdrows")) {
        for (k = 0; k < shown; k++) {
            const lt_tune_difference *df = &s->diff->diffs[s->idx[k]];
            int on = s->sel[s->idx[k]];
            char va[96], vb[96];
            switch (lt_tcmp_severity(df)) {
            case 3: c = t->error; break;
            case 2: c = t->warning; break;
            case 1: c = t->text; break;
            default: c = t->success; break;
            }
            lt_ui_push_id_int(ui, (int)s->idx[k]);
            lt_ui_layout_row(ui, 6, ws, 0);
            if (lt_ui_checkbox(ui, "", &on)) { s->sel[s->idx[k]] = (unsigned char)(on ? 1 : 0); }
            lt_ui_label_ex(ui, df->name, c, LT_UI_ALIGN_LEFT);
            lt_tcmp_format(df->value_a, va, sizeof(va));
            lt_tcmp_format(df->value_b, vb, sizeof(vb));
            lt_ui_label_ex(ui, va, t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, vb, t->text2, LT_UI_ALIGN_LEFT);
            if (df->has_numeric) { lt_snprintf(buf, sizeof(buf), "%.2f", df->numeric_diff); }
            else { lt_strlcpy(buf, "-", sizeof(buf)); }
            lt_ui_label_ex(ui, buf, t->text2, LT_UI_ALIGN_RIGHT);
            if (df->has_percent) { lt_snprintf(buf, sizeof(buf), "%.1f%%", df->percent_diff); }
            else { lt_strlcpy(buf, "-", sizeof(buf)); }
            lt_ui_label_ex(ui, buf, t->text2, LT_UI_ALIGN_RIGHT);
            lt_ui_pop_id(ui);
        }
        if (s->nidx > TD_ROWS_MAX) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(buf, sizeof(buf), "Showing %d of %lu differences", TD_ROWS_MAX, (unsigned long)s->nidx);
            lt_ui_label_ex(ui, buf, t->muted, LT_UI_ALIGN_CENTER);
        }
        lt_ui_scroll_end(ui);
    }
}

static void td_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    td_dlg *s = (td_dlg *)d->data;
    int open = d->open, w1 = -1, wc[4];
    const char *picked;
    if (s == NULL) { td_init(app, d); s = (td_dlg *)d->data; td_preload(app, s); }
    picked = lt_app_file_result(app, FD_TD_A);
    if (picked != NULL) { lt_strlcpy(s->path_a, picked, sizeof(s->path_a)); }
    picked = lt_app_file_result(app, FD_TD_B);
    if (picked != NULL) { lt_strlcpy(s->path_b, picked, sizeof(s->path_b)); }
    if (lt_ui_modal_begin(ui, "Compare Tune Files", 940, 640, &open)) {
        int can, fh = lt_ui_cell_mode(ui) ? 16 : 30;
        file_row(app, s, "Base Tune (A):", s->path_a, sizeof(s->path_a), FD_TD_A, "tdpa");
        file_row(app, s, "Compare Tune (B):", s->path_b, sizeof(s->path_b), FD_TD_B, "tdpb");
        if (s->n_rp > 0) {
            int w[3];
            w[0] = 130; w[1] = -1; w[2] = 110;
            lt_ui_layout_row(ui, 3, w, 0);
            lt_ui_label(ui, "Restore point:");
            (void)lt_ui_combo_cb(ui, "tdrp", &s->rp_sel, (int)s->n_rp, rp_get, s);
            if (lt_ui_button(ui, "Use as B")) {
                char *p = NULL;
                if (lt_restore_path(&app->project, s->rp[s->rp_sel].filename, &p) == 0) {
                    lt_strlcpy(s->path_b, p, sizeof(s->path_b));
                }
                lt_free(p);
            }
        }
        can = s->path_a[0] != '\0' && s->path_b[0] != '\0';
        wc[0] = 130; wc[1] = -1;
        lt_ui_layout_row(ui, 2, wc, 0);
        if (lt_ui_button_ex(ui, "Compare", can ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && can) {
            td_compare(app, s);
        }
        if (s->err[0] != '\0') { lt_ui_label_ex(ui, s->err, t->error, LT_UI_ALIGN_LEFT); }
        else { lt_ui_label(ui, ""); }
        if (s->diff != NULL) { results(app, s); }
        else { lt_ui_layout_row(ui, 1, &w1, -(fh + 8)); lt_ui_label(ui, ""); }
        {
            size_t n = s->diff != NULL ? sel_count(s) : 0;
            int wf[4];
            char buf[96];
            wf[0] = 180; wf[1] = 250; wf[2] = -1; wf[3] = 90;
            lt_ui_layout_row(ui, 4, wf, 0);
            if (s->diff != NULL && s->diff->n > 0) {
                lt_snprintf(buf, sizeof(buf), "%lu constant%s selected", (unsigned long)n, n == 1 ? "" : "s");
                lt_ui_label_ex(ui, buf, t->text2, LT_UI_ALIGN_LEFT);
                lt_snprintf(buf, sizeof(buf), "Merge %lu from B \xE2\x86\x92 Current", (unsigned long)n);
                if (lt_ui_button_ex(ui, buf, n > 0 ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && n > 0) {
                    td_merge(app, s);
                }
                lt_ui_label_ex(ui, s->merge_msg, s->merge_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_label(ui, ""); lt_ui_label(ui, ""); lt_ui_label(ui, "");
            }
            if (lt_ui_button(ui, "Close")) { open = 0; }
        }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void td_close(lt_app *app, lt_app_dialog *d)
{
    td_dlg *s = (td_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        td_clear_result(s);
        if (s->rp != NULL) { lt_restore_info_free(s->rp, s->n_rp); }
        lt_free(s);
    }
    d->data = NULL;
}

static void act_tunediff(lt_app *app) { (void)lt_app_dialog_push(app, "tunediff", "", "Compare Tune Files"); }

void lt_feat_tunediff_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "tunediff"; dk.draw = td_draw; dk.close = td_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Compare Tune Files...", NULL, act_tunediff, lt_ext_pred_ini);
}
