/* feat_dyno.c - Dyno Overlay (G-81): import dyno run CSV files (header detection), power and torque
 * curves with peak markers, compare two runs (gain/loss shading and averages) and overlay a run on
 * a table (VE) to see which cells the run exercised. Tools -> Dyno Overlay...
 * (dialog kind "dyno"). The data model and CSV parsing are datalog/dl_dyno.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "tables_priv.h"
#include "view_kit.h"

#define FD_DYNO (LT_APP_FD_USER0 + 71)
#define DYNO_FILTERS "CSV Files|*.csv;*.txt;*.tsv|All Files|*"
#define DYNO_MAX 6
#define DYNO_CELLS_MAX 4096

typedef struct dyno_dlg {
    lt_dyno_run runs[DYNO_MAX];
    int nruns;
    int channel;                       /* 0 power, 1 torque, 2 both */
    int tab;                           /* 0 curves, 1 table overlay */
    int ca, cb;                        /* compare selection */
    lt_dyno_cmp cmp; int have_cmp, cmp_a, cmp_b;
    char err[300];
    char **hdr; size_t nhdr;           /* column headers of the last file */
    char last_file[512];
    /* table overlay */
    int table, run, use_boost; double boost_load;
    lt_dyno_cell *cells; size_t ncells; int mapped;
    unsigned char *hit;                /* rows*cols: cell has data */
    size_t hit_n;
    lt_color c_grid, c_axis, c_text, c_bg;   /* chart colours (the draw callback runs later) */
} dyno_dlg;

static const lt_u32 run_palette[DYNO_MAX] = { 0x4FC3F7UL, 0xFF7043UL, 0x66BB6AUL, 0xAB47BCUL, 0xFFA726UL, 0x26C6DAUL };

static lt_color run_color(int i)
{
    lt_u32 c = run_palette[(i >= 0 ? i : 0) % DYNO_MAX];
    return LT_RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static void run_color_text(int i, char *out, size_t cap)
{
    lt_snprintf(out, cap, "#%06lx", (unsigned long)run_palette[(i >= 0 ? i : 0) % DYNO_MAX]);
}

static void dd_clear_cmp(dyno_dlg *s)
{
    if (s->have_cmp) { lt_dyno_cmp_free(&s->cmp); }
    s->have_cmp = 0;
}

static void dd_clear_map(dyno_dlg *s)
{
    lt_free(s->cells); s->cells = NULL; s->ncells = 0;
    lt_free(s->hit); s->hit = NULL; s->hit_n = 0;
    s->mapped = 0;
}

static void dd_free(dyno_dlg *s)
{
    int i;
    for (i = 0; i < s->nruns; i++) { lt_dyno_run_free(&s->runs[i]); }
    s->nruns = 0;
    dd_clear_cmp(s);
    dd_clear_map(s);
    if (s->hdr != NULL) { lt_strv_free(s->hdr, s->nhdr); s->hdr = NULL; s->nhdr = 0; }
}

/* load a CSV as the next run; 0 or <0 (message in s->err) */
static int dd_add(dyno_dlg *s, const char *path)
{
    lt_dyno_run run;
    char name[48];
    int rc;
    s->err[0] = '\0';
    if (s->nruns >= DYNO_MAX) {
        lt_snprintf(s->err, sizeof(s->err), "At most %d runs can be loaded; remove one first", DYNO_MAX);
        return -1;
    }
    if (s->hdr != NULL) { lt_strv_free(s->hdr, s->nhdr); s->hdr = NULL; s->nhdr = 0; }
    (void)lt_dyno_csv_headers(path, &s->hdr, &s->nhdr);
    lt_snprintf(name, sizeof(name), "Run %d", s->nruns + 1);
    rc = lt_dyno_from_csv(path, name, &run);
    if (rc < 0) {
        lt_snprintf(s->err, sizeof(s->err), "Failed to load dyno CSV: %s", lt_err_get());
        return rc;
    }
    run_color_text(s->nruns, run.color, sizeof(run.color));
    s->runs[s->nruns++] = run;
    lt_strlcpy(s->last_file, path, sizeof(s->last_file));
    dd_clear_cmp(s);
    dd_clear_map(s);
    return 0;
}

static void dd_remove(dyno_dlg *s, int i)
{
    int k;
    lt_dyno_run_free(&s->runs[i]);
    for (k = i; k + 1 < s->nruns; k++) { s->runs[k] = s->runs[k + 1]; }
    s->nruns--;
    memset(&s->runs[s->nruns], 0, sizeof(s->runs[0]));
    for (k = 0; k < s->nruns; k++) { run_color_text(k, s->runs[k].color, sizeof(s->runs[k].color)); }
    if (s->ca >= s->nruns) { s->ca = 0; }
    if (s->cb >= s->nruns) { s->cb = s->nruns > 1 ? 1 : 0; }
    if (s->run >= s->nruns) { s->run = 0; }
    dd_clear_cmp(s);
    dd_clear_map(s);
}

/* ---- chart ---- */
static void chart_draw(lt_gfx *g, lt_rect r, void *user)
{
    const dyno_dlg *s = (const dyno_dlg *)user;
    double rpm0, rpm1, vmax, pl, pt, pw, ph, v, x, y, vstep = 50.0;
    int i, k, nl;
    lt_font *f = lt_font_get(LT_FONT_SANS);
    char b[48];
    lt_gfx_fill_rect(g, r, s->c_bg);
    if (s->nruns == 0 || f == NULL) { return; }
    if (!lt_dyno_chart_range(s->runs, (size_t)s->nruns, &rpm0, &rpm1, &vmax)) { return; }
    pl = (double)lt_gfx_px(g, 48); pt = (double)lt_gfx_px(g, 14);
    pw = (double)r.w - pl - (double)lt_gfx_px(g, 16);
    ph = (double)r.h - pt - (double)lt_gfx_px(g, 30);
    if (pw < 40.0 || ph < 40.0) { return; }
    /* grid and labels */
    /* counted loops: adding a step to a huge value from a hostile file does not change it */
    nl = (int)LT_MIN((rpm1 - rpm0) / 500.0 + 0.5, 400.0);
    for (i = 0; i <= nl; i++) {
        v = rpm0 + 500.0 * (double)i;
        x = (double)r.x + pl + (v - rpm0) / (rpm1 - rpm0) * pw;
        lt_gfx_line(g, x, (double)r.y + pt, x, (double)r.y + pt + ph, 1.0, s->c_grid);
        if (fmod(v - rpm0, 1000.0) < 1.0) {
            lt_snprintf(b, sizeof(b), "%.0f", v);
            lt_gfx_text_bl(g, f, lt_gfx_fpx(g, 10.0), x - lt_gfx_text_measure(f, lt_gfx_fpx(g, 10.0), b) * 0.5,
                           (int)((double)r.y + pt + ph + lt_gfx_fpx(g, 14.0)), b, s->c_text);
        }
    }
    while (vmax / vstep > 40.0) { vstep *= 2.0; }   /* at most ~40 lines for absurd values */
    nl = (int)(vmax / vstep + 0.5);
    for (i = 0; i <= nl; i++) {
        v = vstep * (double)i;
        y = (double)r.y + pt + ph - v / vmax * ph;
        lt_gfx_line(g, (double)r.x + pl, y, (double)r.x + pl + pw, y, 1.0, s->c_grid);
        lt_snprintf(b, sizeof(b), "%.0f", v);
        lt_gfx_text_bl(g, f, lt_gfx_fpx(g, 10.0), (double)r.x + pl - lt_gfx_text_measure(f, lt_gfx_fpx(g, 10.0), b) - 5.0,
                       (int)(y + lt_gfx_fpx(g, 3.5)), b, s->c_text);
    }
    lt_gfx_text_bl(g, f, lt_gfx_fpx(g, 10.0), (double)r.x + pl + pw * 0.5 - 8.0,
                   (int)((double)r.y + (double)r.h - 3.0), "RPM", s->c_axis);
    /* gain / loss shading of a comparison */
    if (s->have_cmp && s->channel != 1 && s->cmp.n_hp > 1) {
        double base = (double)r.y + pt + ph;
        for (i = 0; i + 1 < (int)s->cmp.n_hp; i++) {
            double r1 = s->cmp.hp_diff[2 * i], d1 = s->cmp.hp_diff[2 * i + 1], r2 = s->cmp.hp_diff[2 * i + 2];
            double x1 = (double)r.x + pl + (r1 - rpm0) / (rpm1 - rpm0) * pw, x2 = (double)r.x + pl + (r2 - rpm0) / (rpm1 - rpm0) * pw;
            double hgt = fabs(d1) / vmax * ph;
            lt_rect bar = lt_rect_make((int)x1, (int)(d1 >= 0.0 ? base - hgt : base), (int)(x2 - x1) + 1, (int)hgt + 1);
            lt_gfx_fill_rect(g, bar, d1 >= 0.0 ? LT_RGBA(0x66, 0xBB, 0x6A, 60) : LT_RGBA(0xEF, 0x53, 0x50, 60));
        }
    }
    /* curves */
    for (k = 0; k < s->nruns; k++) {
        lt_color c = run_color(k);
        int w;
        for (w = 0; w < 2; w++) {
            double px = 0.0, py = 0.0;
            int have = 0;
            if ((w == 0 && s->channel == 1) || (w == 1 && s->channel == 0)) { continue; }
            for (i = 0; i < (int)s->runs[k].n; i++) {
                const lt_dyno_point *p = &s->runs[k].data[i];
                double val;
                if ((w == 0 && !p->has_hp) || (w == 1 && !p->has_tq)) { continue; }
                val = w == 0 ? p->hp : p->tq_val;
                x = (double)r.x + pl + (p->rpm - rpm0) / (rpm1 - rpm0) * pw;
                y = (double)r.y + pt + ph - val / vmax * ph;
                if (have) { lt_gfx_line(g, px, py, x, y, lt_gfx_fpx(g, w == 0 ? 2.2 : 1.6), w == 0 ? c : (c & 0x00FFFFFFUL) | 0xB0000000UL); }
                px = x; py = y; have = 1;
            }
        }
    }
    /* peaks */
    for (k = 0; k < s->nruns; k++) {
        const lt_dyno_run *run = &s->runs[k];
        lt_color c = run_color(k);
        int w;
        for (w = 0; w < 2; w++) {
            double pv = w == 0 ? run->peak_hp : run->peak_tq, pr = w == 0 ? run->peak_hp_rpm : run->peak_tq_rpm;
            if ((w == 0 && (!run->has_peak_hp || s->channel == 1)) || (w == 1 && (!run->has_peak_tq || s->channel == 0))) { continue; }
            x = (double)r.x + pl + (pr - rpm0) / (rpm1 - rpm0) * pw;
            y = (double)r.y + pt + ph - pv / vmax * ph;
            lt_gfx_fill_circle(g, x, y, lt_gfx_fpx(g, 4.0), c);
            lt_snprintf(b, sizeof(b), w == 0 ? "%.0f HP" : "%.0f ft-lb", pv);
            lt_gfx_text_bl(g, f, lt_gfx_fpx(g, 10.0), x - lt_gfx_text_measure(f, lt_gfx_fpx(g, 10.0), b) * 0.5,
                           (int)(y - lt_gfx_fpx(g, 8.0)), b, s->c_text);
        }
    }
    /* legend */
    y = (double)r.y + pt + 6.0;
    for (k = 0; k < s->nruns; k++) {
        lt_gfx_fill_rect(g, lt_rect_make((int)((double)r.x + pl + 10.0), (int)y, lt_gfx_px(g, 12), lt_gfx_px(g, 3)), run_color(k));
        lt_gfx_text_bl(g, f, lt_gfx_fpx(g, 11.0), (double)r.x + pl + 10.0 + (double)lt_gfx_px(g, 18),
                       (int)(y + lt_gfx_fpx(g, 5.0)), s->runs[k].name != NULL ? s->runs[k].name : "", s->c_text);
        y += lt_gfx_fpx(g, 16.0);
    }
}

/* ---- table overlay ---- */
static const char *table_get(void *ud, int i)
{
    const lt_ini *ini = (const lt_ini *)ud;
    const lt_ini_table *t = ini->tables[i];
    return t->title != NULL && t->title[0] != '\0' ? t->title : t->name;
}

static const char *run_get(void *ud, int i)
{
    const dyno_dlg *s = (const dyno_dlg *)ud;
    return s->runs[i].name != NULL ? s->runs[i].name : "";
}

static void dd_map(lt_app *app, dyno_dlg *s)
{
    lt_tblview v;
    lt_pagesrc ps = lt_app_pages(app);
    const lt_ini_table *t;
    size_t i;
    dd_clear_map(s);
    s->err[0] = '\0';
    if (app->ini == NULL || s->nruns == 0 || s->table < 0 || (size_t)s->table >= app->ini->n_tables) { return; }
    t = app->ini->tables[s->table];
    lt_tblview_init(&v);
    if (lt_tblview_load_table(&v, app->ini, t, &ps) < 0 || v.y == NULL) {
        lt_snprintf(s->err, sizeof(s->err), "Cannot load the table: %s", lt_err_get());
        lt_tblview_free(&v);
        return;
    }
    s->cells = (lt_dyno_cell *)lt_calloc(v.rows * v.cols + 1, sizeof(lt_dyno_cell));
    s->ncells = lt_dyno_map_to_table(&s->runs[s->run], v.x, v.cols, v.y, v.rows,
                                     s->use_boost ? s->boost_load : 0.0, s->cells, v.rows * v.cols);
    s->hit = (unsigned char *)lt_calloc(v.rows * v.cols + 1, 1);
    s->hit_n = v.rows * v.cols;
    for (i = 0; i < s->ncells; i++) {
        size_t k = s->cells[i].row * v.cols + s->cells[i].col;
        if (k < s->hit_n) { s->hit[k] = 1; }
    }
    s->mapped = 1;
    lt_tblview_free(&v);
}

static const lt_dyno_cell *cell_at(const dyno_dlg *s, size_t row, size_t col)
{
    size_t i;
    for (i = 0; i < s->ncells; i++) {
        if (s->cells[i].row == row && s->cells[i].col == col) { return &s->cells[i]; }
    }
    return NULL;
}

static void draw_overlay_tab(lt_app *app, dyno_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w[4], cell = lt_ui_cell_mode(ui);
    lt_tblview v;
    lt_pagesrc ps = lt_app_pages(app);
    char b[64];
    if (app->ini == NULL || app->ini->n_tables == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Open an ECU definition with tables first.", th->warning, LT_UI_ALIGN_LEFT);
        return;
    }
    if (s->nruns == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Import a dyno run first: the overlay shows which table cells it exercised.", th->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    w[0] = 110; w[1] = -1; w[2] = 90; w[3] = 160;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Table:");
    if (lt_ui_combo_cb(ui, "dytab", &s->table, (int)app->ini->n_tables, table_get, (void *)app->ini)) { dd_clear_map(s); }
    lt_ui_label(ui, "Run:");
    if (lt_ui_combo_cb(ui, "dyrun", &s->run, s->nruns, run_get, s)) { dd_clear_map(s); }
    w[0] = 180; w[1] = 110; w[2] = -1; w[3] = 140;
    lt_ui_layout_row(ui, 4, w, 0);
    if (lt_ui_checkbox(ui, "Boost as load, factor", &s->use_boost)) { dd_clear_map(s); }
    if (lt_ui_number(ui, "dyboost", &s->boost_load, 0.01, 100.0, 0.1, 2) & LT_UI_CHANGED) { dd_clear_map(s); }
    lt_ui_label_ex(ui, s->use_boost ? "" : "(no boost: top load row)", th->muted, LT_UI_ALIGN_LEFT);
    if (lt_ui_button_ex(ui, "Map run to table", LT_UI_BTN_PRIMARY)) { dd_map(app, s); }
    if (s->err[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->err, th->error, LT_UI_ALIGN_LEFT);
    }
    if (!s->mapped) { return; }
    lt_snprintf(b, sizeof(b), "%lu cell%s hold dyno samples (HP / AFR)", (unsigned long)s->ncells, s->ncells == 1 ? "" : "s");
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
    lt_tblview_init(&v);
    if (app->ini != NULL && lt_tblview_load_table(&v, app->ini, app->ini->tables[s->table], &ps) == 0) {
        lt_ui_layout_row(ui, 1, &w1, -(cell ? 20 : 40));
        if (lt_ui_grid_begin(ui, "dygrid", (int)v.rows, (int)v.cols, cell ? 64 : 66, cell ? 16 : 22, cell ? 64 : 68, cell ? 16 : 24)) {
            int r0, r1, c0, c1, r, c;
            lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
            lt_ui_grid_corner(ui, "Y \\ X");
            for (c = c0; c <= c1; c++) { lt_tabs_fmt(b, sizeof(b), v.x[c], 0); lt_ui_grid_header(ui, 1, c, b); }
            for (r = r0; r <= r1; r++) { lt_tabs_fmt(b, sizeof(b), v.y[(int)v.rows - 1 - r], 0); lt_ui_grid_header(ui, 0, r, b); }
            for (r = r0; r <= r1; r++) {
                for (c = c0; c <= c1; c++) {
                    size_t dr = v.rows - 1 - (size_t)r;   /* highest load first: that is where a dyno pull lands */
                    const lt_dyno_cell *dc = cell_at(s, dr, (size_t)c);
                    lt_color bg = th->table_cell, fg = th->muted;
                    if (dc != NULL) {
                        bg = lt_color_mix(th->table_cell, run_color(s->run), 190);
                        fg = lt_tabs_heat_text(bg);
                        if (dc->has_hp) { lt_snprintf(b, sizeof(b), "%.0f hp", dc->hp); }
                        else if (dc->has_afr) { lt_snprintf(b, sizeof(b), "%.1f afr", dc->afr); }
                        else { lt_snprintf(b, sizeof(b), "%.0f ftlb", dc->tq_val); }
                    } else {
                        lt_tabs_fmt(b, sizeof(b), lt_tblview_get(&v, dr, (size_t)c), 1);
                    }
                    lt_ui_grid_cell(ui, r, c, b, bg, fg);
                }
            }
            (void)lt_ui_grid_end(ui);
        }
    }
    lt_tblview_free(&v);
}

static void draw_curves_tab(lt_app *app, dyno_dlg *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w[5], i, cell = lt_ui_cell_mode(ui), rm = -1;
    char b[240];
    if (s->nruns == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Import a dyno CSV file to get started.", th->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Supported columns: RPM, HP/Power, Torque, AFR, Boost, Time (detected by header).", th->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    if (!cell) {
        lt_rect r;
        lt_ui_layout_row(ui, 1, &w1, 250);
        r = lt_ui_layout_next(ui);
        s->c_grid = th->border_subtle; s->c_axis = th->text2; s->c_text = th->text2; s->c_bg = th->table_bg;
        lt_ui_draw_box(ui, r, th->table_bg, th->border_subtle, 0);
        lt_ui_draw_custom(ui, r, chart_draw, s);
    }
    for (i = 0; i < s->nruns; i++) {
        const lt_dyno_run *run = &s->runs[i];
        char pk[120] = "";
        lt_ui_push_id_int(ui, i);
        w[0] = 14; w[1] = 90; w[2] = -1; w[3] = 80;
        lt_ui_layout_row(ui, 4, w, 0);
        lt_ui_label_ex(ui, "*", run_color(i), LT_UI_ALIGN_CENTER);
        lt_ui_label_ex(ui, run->name != NULL ? run->name : "", th->text, LT_UI_ALIGN_LEFT);
        if (run->has_peak_hp) { lt_snprintf(pk, sizeof(pk), "  %.0f HP @ %.0f", run->peak_hp, run->peak_hp_rpm); }
        if (run->has_peak_tq) {
            size_t l = strlen(pk);
            lt_snprintf(pk + l, sizeof(pk) - l, "  %.0f ft-lb @ %.0f", run->peak_tq, run->peak_tq_rpm);
        }
        lt_snprintf(b, sizeof(b), "%lu pts%s", (unsigned long)run->n, pk);
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
        if (lt_ui_button(ui, "Remove")) { rm = i; }
        lt_ui_pop_id(ui);
    }
    if (rm >= 0) { dd_remove(s, rm); return; }
    if (s->nruns >= 2) {
        w[0] = 110; w[1] = 200; w[2] = 30; w[3] = 200; w[4] = -1;
        lt_ui_layout_row(ui, 5, w, 0);
        lt_ui_label_ex(ui, "Compare:", th->text2, LT_UI_ALIGN_RIGHT);
        (void)lt_ui_combo_cb(ui, "dyca", &s->ca, s->nruns, run_get, s);
        lt_ui_label_ex(ui, "vs", th->muted, LT_UI_ALIGN_CENTER);
        (void)lt_ui_combo_cb(ui, "dycb", &s->cb, s->nruns, run_get, s);
        if (lt_ui_button_ex(ui, "Compare", s->ca != s->cb ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && s->ca != s->cb) {
            dd_clear_cmp(s);
            if (lt_dyno_compare(&s->runs[s->ca], &s->runs[s->cb], &s->cmp) == 0) {
                s->have_cmp = 1; s->cmp_a = s->ca; s->cmp_b = s->cb;
            }
        }
        if (s->have_cmp) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "%s vs %s:  average power change %s%.1f HP,  average torque change %s%.1f ft-lb",
                        s->runs[s->cmp_a].name, s->runs[s->cmp_b].name,
                        s->cmp.hp_change_sum >= 0.0 ? "+" : "", s->cmp.has_total_hp ? s->cmp.hp_change_sum : 0.0,
                        s->cmp.tq_change_sum >= 0.0 ? "+" : "", s->cmp.has_total_tq ? s->cmp.tq_change_sum : 0.0);
            lt_ui_label_ex(ui, b, s->cmp.hp_change_sum >= 0.0 ? th->success : th->error, LT_UI_ALIGN_LEFT);
            if (s->runs[s->cmp_a].has_peak_hp && s->runs[s->cmp_b].has_peak_hp) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_snprintf(b, sizeof(b), "Peak power %.0f -> %.0f HP (%s%.1f)", s->runs[s->cmp_a].peak_hp, s->runs[s->cmp_b].peak_hp,
                            s->runs[s->cmp_b].peak_hp >= s->runs[s->cmp_a].peak_hp ? "+" : "",
                            s->runs[s->cmp_b].peak_hp - s->runs[s->cmp_a].peak_hp);
                lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
            }
        }
    }
}

static void dd_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    dyno_dlg *s = (dyno_dlg *)d->data;
    int open = d->open, w1 = -1, w[4];
    const char *picked;
    static const char *const chans[3] = { "Power", "Torque", "Both" };
    static const char *const tabs[2] = { "Power / torque curves", "Table overlay" };
    if (s == NULL) {
        char *e = lt_plat_getenv("LT_DYNO_RUNS");   /* scripted use (screenshots): path|path|... */
        s = (dyno_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->channel = 2; s->boost_load = 1.0; s->cb = 1;
        if (app->ini != NULL) {
            size_t i;
            for (i = 0; i < app->ini->n_tables; i++) {
                if (app->ini->tables[i]->role == LT_ROLE_VE) { s->table = (int)i; break; }
            }
        }
        if (e != NULL) {
            char *p = e;
            while (p != NULL && *p != '\0') {
                char *bar = strchr(p, '|');
                if (bar != NULL) { *bar = '\0'; }
                (void)dd_add(s, p);
                p = bar != NULL ? bar + 1 : NULL;
            }
        }
        lt_free(e);
        e = lt_plat_getenv("LT_DYNO_TAB");
        if (e != NULL && e[0] == '1') { s->tab = 1; if (s->nruns > 0) { dd_map(app, s); } }
        if (e != NULL && e[0] == 'c' && s->nruns > 1) {
            s->ca = 0; s->cb = 1;
            if (lt_dyno_compare(&s->runs[0], &s->runs[1], &s->cmp) == 0) { s->have_cmp = 1; s->cmp_a = 0; s->cmp_b = 1; }
        }
        lt_free(e);
    }
    picked = lt_app_file_result(app, FD_DYNO);
    if (picked != NULL) { (void)dd_add(s, picked); }
    if (lt_ui_modal_begin(ui, "Dyno Overlay", 940, 700, &open)) {
        w[0] = 140; w[1] = 130; w[2] = 120; w[3] = -1;
        lt_ui_layout_row(ui, 4, w, 0);
        if (lt_ui_button_ex(ui, "+ Import CSV...", LT_UI_BTN_PRIMARY)) {
            lt_app_file_dialog(app, FD_DYNO, LT_UI_FILE_OPEN, NULL, "", DYNO_FILTERS);
        }
        (void)lt_ui_combo(ui, "dychan", &s->channel, chans, 3);
        if (lt_ui_button_ex(ui, "Remove all", s->nruns > 0 ? 0 : LT_UI_BTN_DISABLED) && s->nruns > 0) {
            while (s->nruns > 0) { dd_remove(s, s->nruns - 1); }
        }
        if (s->hdr != NULL && s->nhdr > 0) {
            char line[300] = "Columns: ";
            size_t i;
            for (i = 0; i < s->nhdr && strlen(line) < 240; i++) {
                if (i > 0) { lt_strlcat(line, ", ", sizeof(line)); }
                lt_strlcat(line, s->hdr[i], sizeof(line));
            }
            lt_ui_label_ex(ui, line, th->muted, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label(ui, "");
        }
        if (s->err[0] != '\0') {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, s->err, th->error, LT_UI_ALIGN_LEFT);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        (void)lt_ui_tabs_flat(ui, "dytabs", tabs, 2, &s->tab);
        if (s->tab == 0) { draw_curves_tab(app, s); } else { draw_overlay_tab(app, s); }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void dd_destroy(void *p)
{
    dd_free((dyno_dlg *)p);
    lt_free(p);
}

static void dd_close(lt_app *app, lt_app_dialog *d)
{
    dyno_dlg *s = (dyno_dlg *)d->data;
    if (s != NULL) { lt_ui_after_frame(app->ui, dd_destroy, s); }   /* the chart callback reads it */
    d->data = NULL;
}

static void act_dyno(lt_app *app)
{
    (void)lt_app_dialog_push(app, "dyno", "", "Dyno Overlay");
}

void lt_feat_dyno_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "dyno"; dk.draw = dd_draw; dk.close = dd_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Dyno Overlay...", NULL, act_dyno, NULL);
}
