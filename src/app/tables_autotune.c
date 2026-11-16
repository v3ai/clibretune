/* tables_autotune.c - AutoTune tab: live VE analyze, recommendations grid, settings, replay.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "tables_priv.h"

#define AT_FD_REPLAY (LT_APP_FD_USER0 + 20)

typedef struct at_tab {
    lt_at_state *at;
    lt_at_settings set;
    lt_at_filters f;
    lt_at_authority auth;
    lt_at_channels ch;
    const char *names[24]; size_t ntables; int tsel;
    const lt_ini_table *t;
    lt_tblview ve, tgt;
    int have_tgt, loaded, zdig;
    char msg[200], info[240], custom[160];
    lt_at_rec *recs; size_t nrecs, cap;
    unsigned char *sel; size_t sel_cap;
    unsigned long last_frames;
    int running, replay, ybottom, hw_sel, cur_r, cur_c;
    double t0_ms;
    int params_dirty;
} at_tab;

static at_tab *g_at = NULL;

static void at_free_views(at_tab *st)
{
    lt_tblview_free(&st->ve);
    lt_tblview_free(&st->tgt);
    lt_free(st->recs); st->recs = NULL; st->nrecs = st->cap = 0;
    lt_free(st->sel); st->sel = NULL; st->sel_cap = 0;
    st->have_tgt = 0;
}

static void at_set_refs(lt_app *app, at_tab *st)
{
    lt_at_reftables ref;
    (void)app;
    lt_at_reftables_from_views(&st->ve, st->have_tgt ? &st->tgt : NULL, &ref);
    lt_at_set_reference_tables(st->at, &ref);
}

static void at_load(lt_app *app, at_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const char *tn;
    const lt_ini_table *tt;
    size_t n;
    at_free_views(st);
    lt_tblview_init(&st->ve);
    lt_tblview_init(&st->tgt);
    st->loaded = 0;
    if (st->tsel < 0 || (size_t)st->tsel >= st->ntables) { lt_snprintf(st->msg, sizeof(st->msg), "No fuel table can be tuned with this definition."); return; }
    tn = st->names[st->tsel];
    st->t = lt_ini_find_table(app->ini, tn);
    if (st->t == NULL || lt_tblview_load_table(&st->ve, app->ini, st->t, &src) < 0) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load table '%s': %s", tn, lt_err_get());
        return;
    }
    st->zdig = lt_tabs_const_digits(app->ini, st->t->z_const);
    tt = app->ini->ve_analyze.target_table != NULL ? lt_ini_find_table(app->ini, app->ini->ve_analyze.target_table) : NULL;
    if (tt != NULL && lt_tblview_load_table(&st->tgt, app->ini, tt, &src) == 0) { st->have_tgt = 1; }
    n = st->ve.rows * st->ve.cols;
    st->cap = n;
    st->recs = (lt_at_rec *)lt_calloc(n + 1, sizeof(lt_at_rec));
    st->sel = (unsigned char *)lt_calloc(n + 1, 1);
    st->sel_cap = n;
    lt_at_set_table_name(st->at, tn);
    at_set_refs(app, st);
    st->loaded = 1;
    st->nrecs = 0;
}

static void at_defaults(lt_app *app, at_tab *st)
{
    lt_at_settings_default(&st->set);
    lt_at_filters_clear(&st->f);
    lt_at_filters_default(&st->f);
    lt_at_filters_adopt_ini(&st->f, app->ini);
    lt_at_authority_default(&st->auth);
    lt_at_channels_from_ini(app->ini, &st->ch);
    st->custom[0] = '\0';
    st->hw_sel = (int)st->set.hit_weighting;
}

static void at_refresh_recs(at_tab *st)
{
    if (st->recs == NULL || st->replay) { return; }
    st->nrecs = lt_at_recommendations(st->at, st->recs, st->cap);
}

static void at_stop(at_tab *st)
{
    if (st->running) { lt_at_stop(st->at); }
    st->running = 0;
}

static void at_start(lt_app *app, at_tab *st)
{
    const char *why = lt_fuel_tune_refusal(app->ini, st->names[st->tsel]);
    lt_finding fnd[16];
    lt_preflight_input in;
    size_t nf, i;
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 2, "Connect to an ECU (or demo mode) to collect data, or replay a log");
        return;
    }
    if (why != NULL) { lt_app_toast(app, 3, "%s", why); return; }
    at_load(app, st);
    if (!st->loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
    memset(&in, 0, sizeof(in));
    in.target_table = app->ini->ve_analyze.target_table;
    if (st->have_tgt) { in.target_values = st->tgt.z; in.t_rows = st->tgt.rows; in.t_cols = st->tgt.cols; }
    in.ve_rows = st->ve.rows; in.ve_cols = st->ve.cols; in.ve_values = st->ve.z;
    in.celsius = LT_TRUE;
    in.will_write_to_ecu = LT_TRUE;
    nf = lt_preflight_check(&in, &st->set, &st->f, &st->auth, fnd, 16);
    if (lt_preflight_has_blocker(fnd, nf)) {
        for (i = 0; i < nf; i++) {
            if (fnd[i].sev == LT_SEV_BLOCKER) { lt_app_toast(app, 3, "AutoTune blocked: %s", fnd[i].title); break; }
        }
        return;
    }
    for (i = 0; i < nf; i++) {
        if (fnd[i].sev == LT_SEV_WARNING) { lt_app_toast(app, 2, "Pre-check: %s", fnd[i].title); break; }
    }
    st->replay = 0;
    lt_at_start(st->at);
    st->running = 1;
    st->nrecs = 0;
    st->last_frames = app->frames_ok;
    st->t0_ms = lt_plat_now_ms();
    st->info[0] = '\0';
}

static void at_feed(lt_app *app, at_tab *st)
{
    lt_ve_point pt;
    if (!st->running || !st->loaded || app->frames_ok == st->last_frames) { return; }
    st->last_frames = app->frames_ok;
    if (app->rt == NULL) { return; }
    if (!lt_at_sample_from_rt(app->rt, &st->ch, (unsigned long)(lt_plat_now_ms() - st->t0_ms), &pt)) { return; }
    lt_at_add_data_point(st->at, &pt, st->ve.x, st->ve.cols, st->ve.y, st->ve.rows, &st->set, &st->f,
                         &st->auth);
    at_refresh_recs(st);
}

void lt_tabs_autotune_service(lt_app *app)
{
    if (g_at != NULL && g_at->running) {
        if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { at_stop(g_at); }
        else { at_feed(app, g_at); }
    }
}

static int rec_index(const at_tab *st, size_t y, size_t x)
{
    size_t i;
    for (i = 0; i < st->nrecs; i++) {
        if (st->recs[i].cell_y == y && st->recs[i].cell_x == x) { return (int)i; }
    }
    return -1;
}

static void at_apply(lt_app *app, at_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    size_t i, n = 0;
    double lo = st->auth.min_cell_value, hi = st->auth.max_cell_value, zl, zh;
    if (!st->loaded || st->nrecs == 0) { lt_app_toast(app, 2, "No recommendations to apply"); return; }
    lt_tblview_z_range(&st->ve, app->ini, &zl, &zh);
    if (zh > zl) { if (zl > lo) { lo = zl; } if (zh < hi) { hi = zh; } }
    lt_tblview_push_undo(&st->ve);
    for (i = 0; i < st->nrecs; i++) {
        const lt_at_rec *r = &st->recs[i];
        double v = r->recommended_value;
        if (lt_at_is_cell_locked(st->at, r->cell_x, r->cell_y)) { continue; }
        if (r->cell_y >= st->ve.rows || r->cell_x >= st->ve.cols) { continue; }
        v = v < lo ? lo : (v > hi ? hi : v);
        lt_tblview_set(&st->ve, r->cell_y, r->cell_x, lt_tabs_round(v, st->zdig));
        n++;
    }
    if (lt_tblview_commit(&st->ve, app->ini, &dst) < 0) {
        lt_app_toast(app, 3, "Write failed: %s", lt_err_get());
        return;
    }
    lt_app_edited(app);
    lt_app_toast(app, 1, "Applied %d cells to %s", (int)n, st->names[st->tsel]);
    {
        int was = st->running, rep = st->replay;
        at_load(app, st);
        if (rep) { st->replay = 0; st->nrecs = 0; }
        if (was) { lt_at_start(st->at); }
    }
}

static void at_replay(lt_app *app, at_tab *st, const char *path)
{
    lt_log log;
    lt_replay_config cfg;
    lt_replay_report rep;
    lt_at_reftables ref;
    size_t i;
    int rc;
    if (!st->loaded) { at_load(app, st); }
    if (!st->loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
    at_stop(st);
    lt_log_init(&log);
    if (lt_log_read(path, &log) < 0) {
        lt_app_toast(app, 3, "Cannot read log: %s", lt_err_get());
        lt_log_free(&log);
        return;
    }
    lt_replay_config_default(&cfg);
    cfg.settings = st->set;
    cfg.authority = st->auth;
    cfg.filters = st->f;     /* shallow copy: strings stay owned by the tab */
    lt_at_reftables_from_views(&st->ve, st->have_tgt ? &st->tgt : NULL, &ref);
    memset(&rep, 0, sizeof(rep));
    rc = lt_replay(&log, &st->ch, st->ve.x, st->ve.cols, st->ve.y, st->ve.rows, &ref, &cfg, &rep);
    if (rc < 0) {
        lt_app_toast(app, 3, "Replay failed: %s", lt_err_get());
    } else {
        st->nrecs = 0;
        for (i = 0; i < rep.ncells && st->nrecs < st->cap; i++) {
            lt_at_rec *r = &st->recs[st->nrecs++];
            memset(r, 0, sizeof(*r));
            r->cell_x = (unsigned)rep.cells[i].x; r->cell_y = (unsigned)rep.cells[i].y;
            r->beginning_value = rep.cells[i].current_ve;
            r->recommended_value = rep.cells[i].proposed_ve;
            r->hit_count = rep.cells[i].hits;
            r->hit_weighting = rep.cells[i].weight;
            r->target_afr = rep.cells[i].target_afr;
        }
        st->replay = 1;
        lt_snprintf(st->info, sizeof(st->info), "Replay %s: %d samples, %d cells proposed%s", lt_path_base(path),
                    (int)rep.total_samples, (int)rep.ncells,
                    rep.has_validation ? " (validated)" : "");
        lt_app_toast(app, 1, "Replay: %d cells proposed", (int)rep.ncells);
    }
    lt_replay_report_free(&rep);
    lt_log_free(&log);
}

static void at_sel_cells(at_tab *st, int lock, int unlock_all)
{
    size_t y, x, cols = st->ve.cols;
    for (y = 0; y < st->ve.rows; y++) {
        for (x = 0; x < cols; x++) {
            if (unlock_all) { lt_at_unlock_cell(st->at, (unsigned)x, (unsigned)y); }
            else if (st->sel[y * cols + x]) {
                if (lock == 2) {
                    if (lt_at_is_cell_locked(st->at, (unsigned)x, (unsigned)y)) { lt_at_unlock_cell(st->at, (unsigned)x, (unsigned)y); }
                    else { lt_at_lock_cell(st->at, (unsigned)x, (unsigned)y); }
                }
            }
        }
    }
}

static void at_grid(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int rows = (int)st->ve.rows, cols = (int)st->ve.cols, cell = lt_ui_cell_mode(ui);
    int cw = cell ? 64 : 66, ch = cell ? 16 : 24, rhw = cell ? 64 : 72, chh = cell ? 16 : 24;
    char b[48];
    if (!lt_ui_grid_begin(ui, "atgrid", rows, cols, cw, ch, rhw, chh)) { return; }
    {
        int r0, r1, c0, c1, r, c;
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, "Y \\ X");
        for (c = c0; c <= c1; c++) {
            lt_tabs_fmt(b, sizeof(b), st->ve.x[c], lt_tabs_const_digits(app->ini, st->t->x_const));
            lt_ui_grid_header(ui, 1, c, b);
        }
        for (r = r0; r <= r1; r++) {
            int dr = st->ybottom ? rows - 1 - r : r;
            lt_tabs_fmt(b, sizeof(b), st->ve.y[dr], lt_tabs_const_digits(app->ini, st->t->y_const));
            lt_ui_grid_header(ui, 0, r, b);
        }
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) {
                int dr = st->ybottom ? rows - 1 - r : r, ri = rec_index(st, (size_t)dr, (size_t)c);
                double cur = st->ve.z[(size_t)dr * (size_t)cols + (size_t)c];
                lt_color bg = th->table_cell, fg = th->text2;
                int locked = lt_at_is_cell_locked(st->at, (unsigned)c, (unsigned)dr);
                lt_rect cr;
                if (ri >= 0) {
                    const lt_at_rec *rc = &st->recs[ri];
                    double d = rc->beginning_value != 0.0 ? (rc->recommended_value - rc->beginning_value) / rc->beginning_value : 0.0;
                    double a = fabs(d) * 10.0;
                    int t256;
                    if (a > 1.0) { a = 1.0; }
                    t256 = (int)(a * 200.0) + 30;
                    bg = lt_color_mix(th->table_cell, d >= 0.0 ? th->change_richer : th->change_leaner, t256);
                    fg = lt_color_contrast(bg);
                    lt_tabs_fmt(b, sizeof(b), rc->recommended_value, st->zdig);
                } else {
                    lt_tabs_fmt(b, sizeof(b), cur, st->zdig);
                }
                if (locked) { bg = lt_color_mix(bg, th->warning, 90); }
                lt_ui_grid_cell(ui, r, c, b, bg, fg);
                cr = lt_ui_grid_cell_rect(ui, r, c);
                if (ri >= 0 && !cell) {
                    lt_snprintf(b, sizeof(b), "%u", st->recs[ri].hit_count);
                    lt_ui_draw_vtext(ui, cr.x + 3, cr.y + 7, b, 9.0, 0.0, 0, 1, 1.0, fg);
                }
                if (locked && !cell) { lt_ui_draw_vtext(ui, cr.x + cr.w - 3, cr.y + 7, "L", 9.0, 0.0, 2, 1, 1.0, th->warning); }
            }
        }
        memset(st->sel, 0, st->sel_cap);
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                if (lt_ui_grid_selected(ui, r, c)) {
                    st->sel[(size_t)(st->ybottom ? rows - 1 - r : r) * (size_t)cols + (size_t)c] = 1;
                }
            }
        }
        lt_ui_grid_end(ui);
    }
}

static void numrow(lt_ui *ui, const char *label, const char *id, double *v, double lo, double hi,
                   double step, int digits, int *changed)
{
    int w[2];
    w[0] = 130; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, label);
    if (lt_ui_number(ui, id, v, lo, hi, step, digits) & LT_UI_CHANGED) { *changed = 1; }
}

static void at_panel(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, ch = 0, w2[2], hw;
    char b[160];
    static const char *const hws[4] = { "Uniform", "Cell proximity", "Proximity squared", "Cell centre only" };
    if (!lt_ui_scroll_begin(ui, "atpanel")) { return; }
    if (lt_ui_group_begin(ui, "Statistics", 0)) {
        lt_at_reject rj[8];
        size_t nrj = lt_at_rejections(st->at, rj, 8), i, rejected = 0, locks = 0, y, x;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "State: %s", st->running ? "running" : (st->replay ? "replay result" : "stopped"));
        lt_ui_label_ex(ui, b, st->running ? th->success : th->text2, LT_UI_ALIGN_LEFT);
        for (i = 0; i < nrj; i++) { rejected += rj[i].count; }
        for (y = 0; y < st->ve.rows; y++) { for (x = 0; x < st->ve.cols; x++) { if (lt_at_is_cell_locked(st->at, (unsigned)x, (unsigned)y)) { locks++; } } }
        lt_ui_labelf(ui, "Samples: %lu accepted, %lu rejected", lt_at_total_samples(st->at), (unsigned long)rejected);
        lt_ui_labelf(ui, "Cells with recommendations: %d of %d", (int)st->nrecs, (int)(st->ve.rows * st->ve.cols));
        lt_ui_labelf(ui, "Locked cells: %d", (int)locks);
        for (i = 0; i < nrj && i < 3; i++) { lt_ui_labelf(ui, "  %s: %lu", rj[i].reason, rj[i].count); }
        if (st->info[0] != '\0') { lt_ui_label_wrap(ui, st->info, th->text2); }
        lt_ui_group_end(ui);
    }
    if (lt_ui_group_begin(ui, "Target / weighting", 0)) {
        numrow(ui, "Target AFR", "tafr", &st->set.target_afr, 8.0, 25.0, 0.1, 2, &ch);
        w2[0] = 130; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Hit weighting");
        hw = st->hw_sel;
        if (lt_ui_combo(ui, "hw", &hw, hws, 4)) { st->hw_sel = hw; st->set.hit_weighting = (lt_hit_weighting)hw; }
        numrow(ui, "Base weight", "bw", &st->set.base_weight, 0.0, 500.0, 1.0, 0, &ch);
        numrow(ui, "Min change", "mc", &st->set.min_change, 0.0, 50.0, 0.1, 1, &ch);
        numrow(ui, "Delay ms (0=auto)", "dl", &st->set.lambda_delay_ms, 0.0, 1000.0, 5.0, 0, &ch);
        lt_ui_group_end(ui);
    }
    if (lt_ui_group_begin(ui, "Filters", 0)) {
        int ex = st->f.exclude_accel_enrich ? 1 : 0;
        numrow(ui, "Min RPM", "fmin", &st->f.min_rpm, 0.0, 20000.0, 100.0, 0, &ch);
        numrow(ui, "Max RPM", "fmax", &st->f.max_rpm, 0.0, 20000.0, 100.0, 0, &ch);
        numrow(ui, "Min coolant", "fclt", &st->f.min_clt, -50.0, 300.0, 1.0, 0, &ch);
        numrow(ui, "Max TPS rate", "ftps", &st->f.max_tps_rate, 0.0, 1000.0, 5.0, 0, &ch);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, "Exclude accel enrichment", &ex)) { st->f.exclude_accel_enrich = ex ? LT_TRUE : LT_FALSE; }
        w2[0] = 130; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Custom filter");
        if (lt_ui_text(ui, "cf", st->custom, sizeof(st->custom), 0) & LT_UI_SUBMIT) {
            lt_at_filters_set_custom(&st->f, st->custom[0] != '\0' ? st->custom : NULL);
        }
        lt_ui_group_end(ui);
    }
    if (lt_ui_group_begin(ui, "Authority limits", 0)) {
        numrow(ui, "Max change", "amc", &st->auth.max_cell_value_change, 0.0, 255.0, 1.0, 1, &ch);
        numrow(ui, "Max change %", "amp", &st->auth.max_cell_percentage_change, 0.0, 100.0, 1.0, 1, &ch);
        numrow(ui, "Min cell value", "amn", &st->auth.min_cell_value, 0.0, 255.0, 1.0, 0, &ch);
        numrow(ui, "Max cell value", "amx", &st->auth.max_cell_value, 0.0, 255.0, 1.0, 0, &ch);
        lt_ui_group_end(ui);
    }
    lt_ui_scroll_end(ui);
    (void)ch;
}

static void at_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    at_tab *st = (at_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, wt[10], wm[2], i, combo_sel;
    const char *picked;
    LT_UNUSED(r);
    if (st == NULL) {
        st = (at_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        g_at = st;
        st->at = lt_at_new();
        st->ybottom = 1;
        lt_at_filters_default(&st->f);
        st->ntables = lt_fuel_tunable_tables(app->ini, st->names, 24);
        st->tsel = 0;
        for (i = 0; i < (int)st->ntables; i++) {
            if (app->ini->ve_analyze.ve_table != NULL && strcmp(st->names[i], app->ini->ve_analyze.ve_table) == 0) { st->tsel = i; }
        }
        at_defaults(app, st);
        at_load(app, st);
    }
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "AutoTune");
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    /* keep the page view in sync when not collecting */
    if (!st->running && !st->replay && !lt_tblview_is_dirty(&st->ve)) {
        static int tick = 0;
        if (++tick % 20 == 0) { at_load(app, st); if (!st->loaded) { return; } }
    }
    picked = lt_app_file_result(app, AT_FD_REPLAY);
    if (picked != NULL) { at_replay(app, st, picked); }
    if (lt_ui_key_pressed(ui, ' ', 0)) { if (st->running) { at_stop(st); } else { at_start(app, st); } }
    if (lt_ui_key_pressed(ui, 'l', 0)) { at_sel_cells(st, 2, 0); }
    /* toolbar */
    for (i = 0; i < 10; i++) { wt[i] = 0; }
    wt[0] = 90; wt[1] = 220;
    lt_ui_layout_row(ui, 10, wt, 0);
    lt_ui_label(ui, "VE table");
    combo_sel = st->tsel;
    if (st->ntables > 0 && lt_ui_combo(ui, "tsel", &combo_sel, (const char *const *)st->names, (int)st->ntables)) { }
    if (st->running) {
        if (lt_ui_button_ex(ui, "Stop", LT_UI_BTN_DANGER)) { at_stop(st); }
    } else if (lt_ui_button_ex(ui, "Start", LT_UI_BTN_PRIMARY)) { at_start(app, st); }
    lt_ui_tooltip(ui, "Start/stop collecting (Space)");
    if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_ACCENT | (st->nrecs == 0 ? LT_UI_BTN_DISABLED : 0))) { at_apply(app, st); }
    lt_ui_tooltip(ui, "Write the recommendations (except locked cells) to the table");
    if (lt_ui_button_ex(ui, "Reset", LT_UI_BTN_FLAT)) {
        int was = st->running;
        lt_at_start(st->at); st->running = was; st->nrecs = 0; st->replay = 0; st->info[0] = '\0';
        if (!was) { lt_at_stop(st->at); }
    }
    if (lt_ui_button_ex(ui, "Replay log...", LT_UI_BTN_FLAT)) {
        lt_app_file_dialog(app, AT_FD_REPLAY, LT_UI_FILE_OPEN, NULL, "", "Logs|*.mlg;*.csv;*.msl|All files|*");
    }
    if (lt_ui_button_ex(ui, "Lock", LT_UI_BTN_FLAT)) { at_sel_cells(st, 2, 0); }
    lt_ui_tooltip(ui, "Toggle lock on the selected cells (L); locked cells are not changed");
    if (lt_ui_button_ex(ui, "Unlock all", LT_UI_BTN_FLAT)) { at_sel_cells(st, 0, 1); }
    if (lt_ui_button_ex(ui, "Y-bot", LT_UI_BTN_FLAT | (st->ybottom ? LT_UI_BTN_SELECTED : 0))) { st->ybottom = !st->ybottom; }
    if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT | (lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED))) { lt_app_burn(app); }
    if (combo_sel != st->tsel && st->ntables > 0) {
        st->tsel = combo_sel;
        at_stop(st);
        at_load(app, st);
        return;
    }
    wm[0] = -1; wm[1] = 320;
    lt_ui_layout_row(ui, 2, wm, -1);
    at_grid(app, st);
    at_panel(app, st);
    tab->dirty = st->running ? 1 : 0;
}

static void at_close(lt_app *app, lt_app_tab *tab)
{
    at_tab *st = (at_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    if (g_at == st) { g_at = NULL; }
    at_free_views(st);
    lt_at_filters_clear(&st->f);
    if (st->at != NULL) { lt_at_free(st->at); }
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_tabs_kind_autotune = { "autotune", at_draw, at_close };
