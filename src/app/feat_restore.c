/* feat_restore.c - Restore Points dialog (G-14): list, create, restore, delete (one or many),
 * details of the selected point (size, signature, what differs from the open tune), automatic
 * cleanup with a keep count (applied after every new restore point, see lt_restore_create).
 * File -> Restore Points...   (dialog kind "restore", replaces the basic one of dialogs_misc.c)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../tune/tunecmp.h"

#define MAX_LINES 12

typedef struct rp_dlg {
    lt_restore_info *v; size_t n;
    unsigned char *chk;             /* bulk selection per row */
    int sel, loaded;
    int confirm_del, confirm_load;  /* msgbox flags */
    /* details of v[detail_idx] */
    int detail_idx;
    char signature[96], summary[160];
    size_t n_lines; char lines[MAX_LINES][120];
    char msg[200]; int msg_err;
} rp_dlg;

static void rp_clear(rp_dlg *s)
{
    if (s->v != NULL) { lt_restore_info_free(s->v, s->n); }
    lt_free(s->chk);
    s->v = NULL; s->chk = NULL; s->n = 0;
}

static void rp_reload(lt_app *app, rp_dlg *s)
{
    rp_clear(s);
    if (app->has_project) { (void)lt_restore_list(&app->project, &s->v, &s->n); }
    s->chk = (unsigned char *)lt_calloc(s->n + 1, 1);
    s->loaded = 1;
    s->detail_idx = -1;
    if (s->sel >= (int)s->n) { s->sel = (int)s->n - 1; }
    if (s->sel < 0) { s->sel = 0; }
}

static lt_tune *current_copy(lt_app *app)
{
    if (app->tune == NULL) { return NULL; }
    if (app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        (void)lt_tune_from_mem(app->tune, app->mem);
    }
    if (app->ini != NULL) { (void)lt_tune_capture_from_pages(app->tune, app->ini); }
    return lt_tune_clone(app->tune);
}

/* signature and the difference to the open tune of restore point i */
static void rp_details(lt_app *app, rp_dlg *s, int i)
{
    lt_tune *rp, *cur;
    s->detail_idx = i;
    s->signature[0] = '\0';
    s->summary[0] = '\0';
    s->n_lines = 0;
    rp = lt_tune_load(s->v[i].path);
    if (rp == NULL) { lt_strlcpy(s->summary, "This restore point cannot be read", sizeof(s->summary)); return; }
    lt_strlcpy(s->signature, rp->signature != NULL ? rp->signature : "", sizeof(s->signature));
    cur = current_copy(app);
    if (cur != NULL) {
        lt_tune_diff *d = lt_tune_diff_compare(rp, cur);
        if (d != NULL) {
            size_t k, sc = 0;
            if (d->n == 0) {
                lt_strlcpy(s->summary, "Identical to the open tune", sizeof(s->summary));
            } else {
                lt_snprintf(s->summary, sizeof(s->summary), "%lu constant%s differ from the open tune",
                            (unsigned long)d->n, d->n == 1 ? "" : "s");
            }
            for (k = 0; k < d->n && sc < MAX_LINES; k++) {
                char a[48], b[48];
                lt_tcmp_format(d->diffs[k].value_a, a, sizeof(a));
                lt_tcmp_format(d->diffs[k].value_b, b, sizeof(b));
                lt_snprintf(s->lines[sc++], sizeof(s->lines[0]), "%s:  %s  ->  %s", d->diffs[k].name, a, b);
            }
            s->n_lines = sc;
            lt_tune_diff_free(d);
        }
        lt_tune_free(cur);
    }
    lt_tune_free(rp);
}

static const char *size_text(unsigned long b, char *buf, size_t cap)
{
    if (b < 1024UL) { lt_snprintf(buf, cap, "%lu B", b); }
    else if (b < 1024UL * 1024UL) { lt_snprintf(buf, cap, "%.1f KB", (double)b / 1024.0); }
    else { lt_snprintf(buf, cap, "%.1f MB", (double)b / (1024.0 * 1024.0)); }
    return buf;
}

static size_t n_checked(const rp_dlg *s)
{
    size_t i, c = 0;
    for (i = 0; i < s->n; i++) { if (s->chk[i]) { c++; } }
    return c;
}

static void rp_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    rp_dlg *s = (rp_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], w4[5], have, i, fh;
    char line[300], sz[32];
    if (s == NULL) { s = (rp_dlg *)lt_calloc(1, sizeof(*s)); d->data = s; }
    if (!s->loaded) { rp_reload(app, s); }
    fh = lt_ui_cell_mode(ui) ? 16 : 32;
    have = app->has_project && s->n > 0 && s->sel >= 0 && s->sel < (int)s->n;
    if (lt_ui_modal_begin(ui, "Restore Points", 760, 580, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (!app->has_project) {
            lt_ui_label_ex(ui, "Restore points belong to a project. Open or create a project first.",
                           t->warning, LT_UI_ALIGN_LEFT);
        } else {
            lt_snprintf(line, sizeof(line), "A restore point is saved automatically before every burn. "
                        "%lu stored%s.", (unsigned long)s->n,
                        app->project.cfg.settings.max_restore_points > 0 ? "" : ", none removed automatically");
            lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
        }
        lt_ui_layout_row(ui, 1, &w1, 190);
        if (lt_ui_scroll_begin(ui, "rplist")) {
            if (s->n == 0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "No restore points yet.", t->muted, LT_UI_ALIGN_LEFT);
            }
            for (i = 0; i < (int)s->n; i++) {
                int chk = s->chk[i];
                w2[0] = 26; w2[1] = -1;
                lt_ui_push_id_int(ui, i);
                lt_ui_layout_row(ui, 2, w2, 0);
                if (lt_ui_checkbox(ui, "", &chk)) { s->chk[i] = (unsigned char)chk; }
                lt_snprintf(line, sizeof(line), "%.10s  %.8s      %s", s->v[i].created, s->v[i].created + 11,
                            size_text(s->v[i].size_bytes, sz, sizeof(sz)));
                if (lt_ui_button_ex(ui, line, LT_UI_BTN_FLAT | (i == s->sel ? LT_UI_BTN_SELECTED : 0))) {
                    s->sel = i;
                }
                lt_ui_pop_id(ui);
            }
            lt_ui_scroll_end(ui);
        }
        if (s->sel < (int)s->n && s->n > 0 && s->detail_idx != s->sel) { rp_details(app, s, s->sel); }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_separator(ui);
        if (have && s->detail_idx == s->sel) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(line, sizeof(line), "Selected: %s   (%s)", s->v[s->sel].filename,
                        size_text(s->v[s->sel].size_bytes, sz, sizeof(sz)));
            lt_ui_label_ex(ui, line, t->text, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(line, sizeof(line), "Signature: %s", s->signature[0] != '\0' ? s->signature : "unknown");
            lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, s->summary, t->text2, LT_UI_ALIGN_LEFT);
            for (i = 0; i < (int)s->n_lines; i++) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, s->lines[i], t->muted, LT_UI_ALIGN_LEFT);
            }
        }
        lt_ui_layout_space(ui, 4);
        if (s->msg[0] != '\0') {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
        }
        w4[0] = 120; w4[1] = 120; w4[2] = -1; w4[3] = 90;
        lt_ui_layout_row(ui, 4, w4, 0);
        if (lt_ui_button(ui, "Select All")) { memset(s->chk, 1, s->n); }
        if (lt_ui_button(ui, "Select None")) { memset(s->chk, 0, s->n); }
        {
            double keep = (double)app->project.cfg.settings.max_restore_points;
            lt_ui_label_ex(ui, "Keep newest (0 = all)", t->text2, LT_UI_ALIGN_RIGHT);
            if (app->has_project && (lt_ui_number(ui, "rpkeep", &keep, 0.0, 999.0, 1.0, 0) & LT_UI_CHANGED)) {
                app->project.cfg.settings.max_restore_points = (unsigned)keep;
                (void)lt_project_save_config(&app->project);
                if (keep > 0.0) { (void)lt_restore_prune(&app->project, (size_t)keep, NULL); s->loaded = 0; }
            }
        }
        w4[0] = 130; w4[1] = 110; w4[2] = 170; w4[3] = -1; w4[4] = 90;
        lt_ui_layout_row(ui, 5, w4, 0);
        if (lt_ui_button_ex(ui, "Create now", app->has_project && app->tune != NULL ? 0 : LT_UI_BTN_DISABLED) &&
            app->has_project && app->tune != NULL) {
            char *path = NULL;
            if (app->mem != NULL) { (void)lt_tune_from_mem(app->tune, app->mem); }
            if (lt_restore_create(&app->project, &path) == 0) {
                lt_app_toast(app, 1, "Restore point created");
                lt_free(path);
            } else { lt_app_toast(app, 3, "Cannot create restore point: %s", lt_err_get()); }
            s->loaded = 0;
        }
        if (lt_ui_button_ex(ui, "Restore", have ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && have) {
            if (app->modified) { s->confirm_load = 1; }
            else {
                char *path = NULL;
                if (lt_restore_path(&app->project, s->v[s->sel].filename, &path) == 0) {
                    lt_app_load_tune(app, path);
                    lt_free(path);
                    open = 0;
                } else { lt_app_toast(app, 3, "Cannot restore: %s", lt_err_get()); }
            }
        }
        {
            size_t nc = n_checked(s);
            lt_snprintf(line, sizeof(line), nc > 0 ? "Delete Selected (%lu)" : "Delete Selected", (unsigned long)nc);
            if (lt_ui_button_ex(ui, line, nc > 0 ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && nc > 0) {
                s->confirm_del = 1;
            }
        }
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        LT_UNUSED(fh);
        lt_ui_modal_end(ui);
    }
    if (s->confirm_del) {
        size_t nc = n_checked(s);
        int r;
        lt_snprintf(line, sizeof(line), "Delete %lu restore point%s? This cannot be undone.", (unsigned long)nc,
                    nc == 1 ? "" : "s");
        r = lt_ui_msgbox(ui, "rpdel", "Delete Restore Points", line, "Delete|Cancel", &s->confirm_del);
        if (r == 1) {
            size_t k, deleted = 0;
            for (k = 0; k < s->n; k++) {
                if (s->chk[k] && lt_restore_delete(&app->project, s->v[k].filename) == 0) { deleted++; }
            }
            lt_snprintf(s->msg, sizeof(s->msg), "Deleted %lu restore point%s", (unsigned long)deleted,
                        deleted == 1 ? "" : "s");
            s->msg_err = 0;
            s->loaded = 0;
        }
    }
    if (s->confirm_load) {
        int r = lt_ui_msgbox(ui, "rpload", "Load Restore Point",
                             "The open tune has unsaved changes. Load this restore point anyway?",
                             "Load|Cancel", &s->confirm_load);
        if (r == 1 && have) {
            char *path = NULL;
            if (lt_restore_path(&app->project, s->v[s->sel].filename, &path) == 0) {
                lt_app_load_tune(app, path);
                lt_free(path);
                open = 0;
            } else { lt_app_toast(app, 3, "Cannot restore: %s", lt_err_get()); }
        }
    }
    d->open = open;
}

static void rp_close(lt_app *app, lt_app_dialog *d)
{
    rp_dlg *s = (rp_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) { rp_clear(s); lt_free(s); }
    d->data = NULL;
}

void lt_feat_restore_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "restore"; dk.draw = rp_draw; dk.close = rp_close;
    lt_ext_add_dialog_kind(reg, &dk);
}
