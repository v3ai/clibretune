/* feat_tablecmp.c - Table Comparison dialog (G-80): compare two tables cell by cell. Table B comes
 * from the open tune (another table, like the original), a tune file or a restore point (the same
 * or any other table of that tune). Result: summary, difference / percent / value grid with an
 * optional heat map, and the changed cells grouped by variance (>=10 %, 5-10 %, <5 %).
 * Tools -> Compare Tables...   (dialog kind "tablecmp"). The comparison maths is tableops/tablecmp.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "tables_priv.h"
#include "../tableops/tablecmp.h"

#define FD_TC_FILE (LT_APP_FD_USER0 + 70)
#define TC_FILTERS "Tune Files|*.msq;*.json|All Files|*"
#define TC_LIST_MAX 10
#define TC_SRC_CURRENT 0
#define TC_SRC_FILE 1
#define TC_SRC_RESTORE 2

typedef struct tc_dlg {
    int ta, tb, src, view, heat;
    char path[512];
    lt_restore_info *rp; size_t n_rp; int rp_sel;
    lt_tblcmp res; int have;
    double *x, *y; size_t nx, ny;        /* axes of table A (display units) */
    int zdig;
    char title_a[96], title_b[96];
    char err[240];
} tc_dlg;

static void tc_clear(tc_dlg *s)
{
    lt_tblcmp_free(&s->res);
    lt_free(s->x); lt_free(s->y);
    s->x = s->y = NULL; s->nx = s->ny = 0; s->have = 0;
}

static const char *table_get(void *ud, int i)
{
    const lt_ini *ini = (const lt_ini *)ud;
    const lt_ini_table *t = ini->tables[i];
    return t->title != NULL && t->title[0] != '\0' ? t->title : t->name;
}

/* index of the first table with a name, else 0 */
static int table_index(const lt_ini *ini, const char *name)
{
    size_t i;
    for (i = 0; i < ini->n_tables; i++) {
        if (strcmp(ini->tables[i]->name, name) == 0) { return (int)i; }
    }
    return 0;
}

static const char *rp_get(void *ud, int i)
{
    const tc_dlg *s = (const tc_dlg *)ud;
    static char buf[200];
    lt_snprintf(buf, sizeof(buf), "%s   %s", s->rp[i].created, s->rp[i].filename);
    return buf;
}

static void tc_open_dlg(lt_app *app, lt_app_dialog *d)
{
    tc_dlg *s = (tc_dlg *)lt_calloc(1, sizeof(*s));
    char *e = lt_plat_getenv("LT_TABLECMP");
    d->data = s;
    s->heat = 1;
    s->ta = 0;
    s->tb = app->ini != NULL && app->ini->n_tables > 1 ? 1 : 0;
    if (app->has_project) { (void)lt_restore_list(&app->project, &s->rp, &s->n_rp); }
    if (e != NULL && app->ini != NULL) {   /* scripted use (screenshots): LT_TABLECMP=tableA:tableB */
        char *colon = strchr(e, ':');
        if (colon != NULL) {
            *colon = '\0';
            s->ta = table_index(app->ini, e);
            s->tb = table_index(app->ini, colon + 1);
        }
    }
    lt_free(e);
}

/* reads table t of ini from the page source into v (display units) */
static int load_view(lt_tblview *v, const lt_ini *ini, const lt_ini_table *t, const lt_pagesrc *src)
{
    lt_tblview_init(v);
    return lt_tblview_load_table(v, ini, t, src);
}

static void tc_compare(lt_app *app, tc_dlg *s)
{
    lt_tblview va, vb;
    const lt_ini_table *ta, *tb;
    lt_pagesrc pa = lt_app_pages(app), pb;
    lt_tune *other = NULL;
    int rc;
    tc_clear(s);
    s->err[0] = '\0';
    if (app->ini == NULL || app->ini->n_tables == 0) { lt_strlcpy(s->err, "No tables available", sizeof(s->err)); return; }
    ta = app->ini->tables[s->ta];
    tb = app->ini->tables[s->tb];
    if (s->src == TC_SRC_CURRENT) {
        if (s->ta == s->tb) { lt_strlcpy(s->err, "Please select two different tables", sizeof(s->err)); return; }
        pb = pa;
    } else {
        if (s->src == TC_SRC_RESTORE) {
            char *p = NULL;
            if (s->n_rp == 0 || lt_restore_path(&app->project, s->rp[s->rp_sel].filename, &p) != 0) {
                lt_strlcpy(s->err, "No restore point selected", sizeof(s->err));
                lt_free(p);
                return;
            }
            lt_strlcpy(s->path, p, sizeof(s->path));
            lt_free(p);
        }
        other = lt_tune_load(s->path);
        if (other == NULL) {
            lt_snprintf(s->err, sizeof(s->err), "Cannot load tune: %s", lt_err_get());
            return;
        }
        (void)lt_tune_apply_to_pages(other, app->ini);
        pb = lt_tune_pagesrc(other);
    }
    rc = load_view(&va, app->ini, ta, &pa);
    if (rc < 0) {
        lt_snprintf(s->err, sizeof(s->err), "Failed to load table A: %s", lt_err_get());
        if (other != NULL) { lt_tune_free(other); }
        return;
    }
    rc = load_view(&vb, app->ini, tb, &pb);
    if (rc < 0) {
        lt_snprintf(s->err, sizeof(s->err), "Failed to load table B: %s", lt_err_get());
        lt_tblview_free(&va);
        if (other != NULL) { lt_tune_free(other); }
        return;
    }
    if (va.rows != vb.rows || va.cols != vb.cols) {
        lt_snprintf(s->err, sizeof(s->err), "Table dimensions don't match: %lux%lu vs %lux%lu",
                    (unsigned long)va.rows, (unsigned long)va.cols, (unsigned long)vb.rows, (unsigned long)vb.cols);
    } else if (lt_tblcmp_compute(va.z, vb.z, va.rows, va.cols, &s->res) == 0) {
        s->have = 1;
        s->nx = va.cols; s->ny = va.rows;
        s->x = (double *)lt_calloc(s->nx, sizeof(double));
        s->y = (double *)lt_calloc(s->ny, sizeof(double));
        memcpy(s->x, va.x, s->nx * sizeof(double));
        if (va.y != NULL) { memcpy(s->y, va.y, s->ny * sizeof(double)); }
        s->zdig = lt_uconv_digits(&va.zu, lt_tabs_const_digits(app->ini, ta->z_const));
        if (s->zdig < 2) { s->zdig = 2; }
        lt_strlcpy(s->title_a, ta->title != NULL ? ta->title : ta->name, sizeof(s->title_a));
        lt_strlcpy(s->title_b, tb->title != NULL ? tb->title : tb->name, sizeof(s->title_b));
    } else {
        lt_strlcpy(s->err, lt_err_get(), sizeof(s->err));
    }
    lt_tblview_free(&va);
    lt_tblview_free(&vb);
    if (other != NULL) { lt_tune_free(other); }
}

static lt_color heat_color(const lt_theme *th, double t)
{
    double m = fabs(t - 0.5) * 2.0;
    lt_color base = th->table_cell;
    if (m < 0.01) { return base; }
    return lt_color_mix(base, t > 0.5 ? LT_RGB(0xE5, 0x48, 0x4D) : LT_RGB(0x3B, 0x82, 0xF6), (int)(m * 200.0));
}

static void stat_cell(lt_ui *ui, const lt_theme *th, lt_rect r, const char *value, const char *label)
{
    lt_ui_draw_box(ui, r, th->bg_app, th->border_subtle, lt_ui_px(ui, 4));
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 3), r.w, r.h / 2), value, th->text, 1);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + r.h / 2, r.w, r.h / 2 - lt_ui_px(ui, 2)), label, th->muted, 1);
}

static void draw_grid(lt_app *app, tc_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int rows = (int)s->res.rows, cols = (int)s->res.cols, cell = lt_ui_cell_mode(ui);
    int dig = s->view == LT_TBC_VIEW_PCT ? 1 : s->zdig;
    char b[48];
    if (lt_ui_grid_begin(ui, "tcgrid", rows, cols, cell ? 64 : 62, cell ? 16 : 22, cell ? 64 : 68, cell ? 16 : 24)) {
        int r0, r1, c0, c1, r, c;
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, "Y \\ X");
        for (c = c0; c <= c1; c++) { lt_tabs_fmt(b, sizeof(b), s->x[c], 0); lt_ui_grid_header(ui, 1, c, b); }
        for (r = r0; r <= r1; r++) { lt_tabs_fmt(b, sizeof(b), s->y[r], 0); lt_ui_grid_header(ui, 0, r, b); }
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) {
                double v = lt_tblcmp_value(&s->res, s->view, (size_t)r, (size_t)c);
                int changed = fabs(s->res.b[(size_t)r * (size_t)cols + (size_t)c] -
                                   s->res.a[(size_t)r * (size_t)cols + (size_t)c]) > LT_TBC_EPS;
                lt_color bg = th->table_cell, fg = th->text;
                if (s->heat) { bg = heat_color(th, lt_tblcmp_heat(&s->res, s->view, (size_t)r, (size_t)c)); }
                if (s->view == LT_TBC_VIEW_B && s->heat) { bg = lt_tabs_heat(app, th, 0.0, 1.0, lt_tblcmp_heat(&s->res, s->view, (size_t)r, (size_t)c)); }
                if (s->heat) { fg = lt_tabs_heat_text(bg); }
                if (s->view == LT_TBC_VIEW_B) { lt_tabs_fmt(b, sizeof(b), v, dig); }
                else if (!changed) { lt_strlcpy(b, "-", sizeof(b)); fg = th->muted; }
                else {
                    lt_tabs_fmt(b, sizeof(b), v, dig);
                    if (v > 0.0 && strlen(b) < sizeof(b) - 2) { memmove(b + 1, b, strlen(b) + 1); b[0] = '+'; }
                }
                lt_ui_grid_cell(ui, r, c, b, bg, fg);
            }
        }
        (void)lt_ui_grid_end(ui);
    }
}

static void list_group(lt_ui *ui, const lt_theme *th, const tc_dlg *s, int sev, const char *title, lt_color tc, int *w1)
{
    size_t i, shown = 0, total = sev == LT_TBC_HIGH ? s->res.n_high : (sev == LT_TBC_MED ? s->res.n_med : s->res.n_low);
    char b[160];
    if (total == 0) { return; }
    lt_ui_layout_row(ui, 1, w1, 0);
    lt_snprintf(b, sizeof(b), "%s  (%lu)", title, (unsigned long)total);
    lt_ui_label_ex(ui, b, tc, LT_UI_ALIGN_LEFT);
    if (sev == LT_TBC_LOW) {
        lt_ui_layout_row(ui, 1, w1, 0);
        lt_snprintf(b, sizeof(b), "%lu cells with minor differences", (unsigned long)total);
        lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    for (i = 0; i < s->res.n && shown < TC_LIST_MAX; i++) {
        const lt_tbc_cell *c = &s->res.cells[i];
        char va[32], vb[32];
        if (lt_tbc_severity(c->pct) != sev) { continue; }
        lt_tabs_fmt(va, sizeof(va), c->a, s->zdig);
        lt_tabs_fmt(vb, sizeof(vb), c->b, s->zdig);
        lt_snprintf(b, sizeof(b), "[%lu, %lu]   %s -> %s   %s%.1f%%", (unsigned long)c->col, (unsigned long)c->row, va, vb,
                    c->diff > 0.0 ? "+" : "", c->pct);
        lt_ui_layout_row(ui, 1, w1, 0);
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
        shown++;
    }
    if (total > shown) {
        lt_snprintf(b, sizeof(b), "...and %lu more", (unsigned long)(total - shown));
        lt_ui_layout_row(ui, 1, w1, 0);
        lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
    }
}

static void tc_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    tc_dlg *s = (tc_dlg *)d->data;
    int open = d->open, w1 = -1, w[4], cell, i;
    const char *picked;
    static const char *const srcs[3] = { "Open tune", "Tune file", "Restore point" };
    static const char *const views[3] = { "Difference", "Percent", "Values (B)" };
    if (s == NULL) { tc_open_dlg(app, d); s = (tc_dlg *)d->data; if (app->ini != NULL) { tc_compare(app, s); } }
    picked = lt_app_file_result(app, FD_TC_FILE);
    if (picked != NULL) { lt_strlcpy(s->path, picked, sizeof(s->path)); s->src = TC_SRC_FILE; }
    cell = lt_ui_cell_mode(ui);
    if (lt_ui_modal_begin(ui, "Table Comparison", 920, 700, &open)) {
        if (app->ini == NULL || app->ini->n_tables == 0) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Open an ECU definition with tables first.", th->warning, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Compare two tables cell by cell: base table A against table B.", th->text2, LT_UI_ALIGN_LEFT);
            w[0] = 150; w[1] = -1;
            lt_ui_layout_row(ui, 2, w, 0);
            lt_ui_label(ui, "Table A (Base):");
            if (lt_ui_combo_cb(ui, "tca", &s->ta, (int)app->ini->n_tables, table_get, (void *)app->ini)) { tc_clear(s); }
            w[0] = 150; w[1] = 170; w[2] = -1; w[3] = 110;
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label(ui, "Compare against:");
            if (lt_ui_combo(ui, "tcsrc", &s->src, srcs, app->has_project ? 3 : 2)) { tc_clear(s); }
            if (s->src == TC_SRC_FILE) {
                (void)lt_ui_text(ui, "tcpath", s->path, sizeof(s->path), 0);
                if (lt_ui_button(ui, "Browse...")) {
                    lt_app_file_dialog(app, FD_TC_FILE, LT_UI_FILE_OPEN, NULL, "", TC_FILTERS);
                }
            } else if (s->src == TC_SRC_RESTORE) {
                if (s->n_rp > 0) { (void)lt_ui_combo_cb(ui, "tcrp", &s->rp_sel, (int)s->n_rp, rp_get, s); }
                else { lt_ui_label_ex(ui, "No restore points yet.", th->muted, LT_UI_ALIGN_LEFT); }
            } else {
                lt_ui_label_ex(ui, "(another table of the open tune)", th->muted, LT_UI_ALIGN_LEFT);
            }
            w[0] = 150; w[1] = -1;
            lt_ui_layout_row(ui, 2, w, 0);
            lt_ui_label(ui, s->src == TC_SRC_CURRENT ? "Table B (Compare):" : "Table B (in that tune):");
            if (lt_ui_combo_cb(ui, "tcb", &s->tb, (int)app->ini->n_tables, table_get, (void *)app->ini)) { tc_clear(s); }
            w[0] = 150; w[1] = 130; w[2] = 130; w[3] = -1;
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label(ui, "");
            if (lt_ui_button_ex(ui, "Compare Tables", LT_UI_BTN_PRIMARY)) { tc_compare(app, s); }
            if (lt_ui_button(ui, "Swap")) {
                int t = s->ta; s->ta = s->tb; s->tb = t;
                tc_clear(s);
            }
            lt_ui_label(ui, "");
            if (s->err[0] != '\0') {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, s->err, th->error, LT_UI_ALIGN_LEFT);
            }
            if (s->have) {
                lt_rect r;
                char v1[32], v2[32], v3[32], v4[32];
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_separator(ui);
                lt_snprintf(v1, sizeof(v1), "%lu", (unsigned long)s->res.n);
                lt_snprintf(v2, sizeof(v2), "%lu", (unsigned long)(s->res.rows * s->res.cols));
                lt_snprintf(v3, sizeof(v3), "%.2f", s->res.max_diff);
                lt_snprintf(v4, sizeof(v4), "%.2f", s->res.avg_diff);
                if (cell) {
                    char line[160];
                    lt_snprintf(line, sizeof(line), "Cells changed %s of %s | max diff %s | avg diff %s", v1, v2, v3, v4);
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, line, th->text, LT_UI_ALIGN_LEFT);
                } else {
                    w[0] = -1; w[1] = -1; w[2] = -1; w[3] = -1;
                    lt_ui_layout_row(ui, 4, w, 44);
                    r = lt_ui_layout_next(ui); stat_cell(ui, th, r, v1, "Cells Changed");
                    r = lt_ui_layout_next(ui); stat_cell(ui, th, r, v2, "Total Cells");
                    r = lt_ui_layout_next(ui); stat_cell(ui, th, r, v3, "Max Difference");
                    r = lt_ui_layout_next(ui); stat_cell(ui, th, r, v4, "Avg Difference");
                }
                if (s->res.n == 0) {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, "Tables are identical", th->success, LT_UI_ALIGN_LEFT);
                } else {
                    char t[200];
                    w[0] = 150; w[1] = 170; w[2] = 130; w[3] = -1;
                    lt_ui_layout_row(ui, 4, w, 0);
                    lt_ui_label_ex(ui, "View:", th->text2, LT_UI_ALIGN_RIGHT);
                    (void)lt_ui_combo(ui, "tcview", &s->view, views, 3);
                    (void)lt_ui_checkbox(ui, "Heat map", &s->heat);
                    lt_snprintf(t, sizeof(t), "A: %s  vs  B: %s", s->title_a, s->title_b);
                    lt_ui_label_ex(ui, t, th->muted, LT_UI_ALIGN_LEFT);
                }
                lt_ui_layout_row(ui, 1, &w1, cell ? 112 : 210);
                draw_grid(app, s);
                if (s->res.n > 0) {
                    lt_ui_layout_row(ui, 1, &w1, -(cell ? 20 : 40));
                    if (lt_ui_scroll_begin(ui, "tclist")) {
                        list_group(ui, th, s, LT_TBC_HIGH, "High Variance (>=10%)", th->error, &w1);
                        list_group(ui, th, s, LT_TBC_MED, "Medium Variance (5-10%)", th->warning, &w1);
                        list_group(ui, th, s, LT_TBC_LOW, "Low Variance (<5%)", th->muted, &w1);
                        lt_ui_scroll_end(ui);
                    }
                }
            }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        LT_UNUSED(i);
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void tc_close(lt_app *app, lt_app_dialog *d)
{
    tc_dlg *s = (tc_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        tc_clear(s);
        if (s->rp != NULL) { lt_restore_info_free(s->rp, s->n_rp); }
        lt_free(s);
    }
    d->data = NULL;
}

static void act_tablecmp(lt_app *app)
{
    (void)lt_app_dialog_push(app, "tablecmp", "", "Table Comparison");
}

void lt_feat_tablecmp_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "tablecmp"; dk.draw = tc_draw; dk.close = tc_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Compare &Tables...", NULL, act_tablecmp, lt_ext_pred_ini);
}
