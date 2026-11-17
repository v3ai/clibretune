/* tables_autotune.c - AutoTune tab: live VE analyze, recommendations grid, settings, replay.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "tables_priv.h"
#include "view_kit.h"
#include "feat_log.h"
#include "../autotune/at_extra.h"

#define AT_FD_REPLAY (LT_APP_FD_USER0 + 20)
#define AT_FD_CSV (LT_APP_FD_USER0 + 21)
#define AT_FD_PROP (LT_APP_FD_USER0 + 22)

/* one tuned table: its collection state, views and recommendations */
typedef struct at_slot {
    lt_at_state *at;
    const lt_ini_table *t;
    lt_tblview ve, tgt;
    int have_tgt, loaded, zdig, tsel;
    lt_at_rec *recs; size_t nrecs, cap;
    unsigned char *sel; size_t sel_cap;
    lt_atx_trace trace;            /* cells visited by the live operating point */
} at_slot;

typedef struct at_tab {
    at_slot s;                     /* the table shown (primary or secondary) */
    at_slot o;                     /* the other session table, when sec_on */
    int sec_on, on_sec;            /* secondary session enabled; s currently holds the secondary */
    int load_src, load_manual;     /* LT_ATX_LOAD_*, chosen by the user */
    int burn_after;                /* update mode: 0 send to RAM, 1 send and burn */
    int pend_kind, pend_val;       /* guarded switch: 1 table change, 2 secondary off */
    int hm_mode, trace_on;         /* heat map view, trace line */
    int show_pred, show_anom;      /* predicted fills / anomaly overlay in the grid */
    lt_at_settings set;
    lt_at_filters f;
    lt_at_authority auth;
    lt_at_channels ch;
    const char *names[24]; size_t ntables;
    char msg[200], info[240], custom[160];
    char *report;                  /* offline analysis report text */
    char delay_note[120], load_hint[96];
    unsigned long last_frames;
    int running, replay, ybottom, hw_sel, cur_r, cur_c;
    double t0_ms;
    int params_dirty;
    lt_health_report hr;           /* table health card */
    int hr_valid, ptab;            /* ptab: 0 health, 1 settings, 2 session, 3 report */
    unsigned long hr_stamp;
    int frame;
    lt_anomaly an[32]; size_t n_an;
    lt_predicted_cell pc[96]; size_t n_pc;
} at_tab;

static at_tab *g_at = NULL;

static void slot_free_views(at_slot *sl)
{
    lt_tblview_free(&sl->ve);
    lt_tblview_free(&sl->tgt);
    lt_free(sl->recs); sl->recs = NULL; sl->nrecs = sl->cap = 0;
    lt_free(sl->sel); sl->sel = NULL; sl->sel_cap = 0;
    sl->have_tgt = 0;
}

static void at_free_views(at_tab *st) { slot_free_views(&st->s); }

/* exchange the shown table with the other session table */
static void at_swap(at_tab *st)
{
    at_slot t = st->s;
    st->s = st->o;
    st->o = t;
    st->on_sec = !st->on_sec;
}

static void at_set_refs(lt_app *app, at_tab *st)
{
    lt_at_reftables ref;
    (void)app;
    lt_at_reftables_from_views(&st->s.ve, st->s.have_tgt ? &st->s.tgt : NULL, &ref);
    lt_at_set_reference_tables(st->s.at, &ref);
}

static void at_load(lt_app *app, at_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const char *tn;
    const lt_ini_table *tt;
    size_t n;
    at_free_views(st);
    lt_tblview_init(&st->s.ve);
    lt_tblview_init(&st->s.tgt);
    st->s.loaded = 0;
    if (st->s.tsel < 0 || (size_t)st->s.tsel >= st->ntables) { lt_snprintf(st->msg, sizeof(st->msg), "No fuel table can be tuned with this definition."); return; }
    tn = st->names[st->s.tsel];
    st->s.t = lt_ini_find_table(app->ini, tn);
    if (st->s.t == NULL || lt_tblview_load_table(&st->s.ve, app->ini, st->s.t, &src) < 0) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load table '%s': %s", tn, lt_err_get());
        return;
    }
    st->s.zdig = lt_tabs_const_digits(app->ini, st->s.t->z_const);
    tt = app->ini->ve_analyze.target_table != NULL ? lt_ini_find_table(app->ini, app->ini->ve_analyze.target_table) : NULL;
    if (tt != NULL && lt_tblview_load_table(&st->s.tgt, app->ini, tt, &src) == 0) { st->s.have_tgt = 1; }
    n = st->s.ve.rows * st->s.ve.cols;
    st->s.cap = n;
    st->s.recs = (lt_at_rec *)lt_calloc(n + 1, sizeof(lt_at_rec));
    st->s.sel = (unsigned char *)lt_calloc(n + 1, 1);
    st->s.sel_cap = n;
    lt_at_set_table_name(st->s.at, tn);
    at_set_refs(app, st);
    st->s.loaded = 1;
    st->s.nrecs = 0;
    lt_atx_trace_clear(&st->s.trace);
    if (!st->load_manual && !st->on_sec && !st->running && st->s.t->y_channel != NULL) {
        int ls = lt_atx_load_detect(st->s.t->y_channel);
        if (ls != LT_ATX_LOAD_MAP && lt_atx_load_apply(&st->ch, ls)) {
            st->load_src = ls;
            lt_snprintf(st->load_hint, sizeof(st->load_hint), "%s load axis detected.", lt_atx_load_name(ls));
        }
    }
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
    {
        double fl = 0.0, an = 0.0;
        if (lt_tabs_delay_model_load(app, &fl, &an) == 0) {
            st->set.lambda_delay_flow_scaled = LT_TRUE;
            st->set.lambda_delay_floor_ms = fl;
            st->set.lambda_delay_ms = an;
            lt_snprintf(st->delay_note, sizeof(st->delay_note), "Delay model loaded: %.0f ms (floor %.0f ms)", an, fl);
        }
    }
}

static void at_stop(at_tab *st)
{
    if (st->running) {
        lt_at_stop(st->s.at);
        if (st->sec_on && st->o.at != NULL) { lt_at_stop(st->o.at); }
    }
    st->running = 0;
}

/* load (or reload) the secondary session table into the hidden slot */
static void at_load_other(lt_app *app, at_tab *st)
{
    if (!st->sec_on) { return; }
    if (st->o.at == NULL) { st->o.at = lt_at_new(); }
    at_swap(st);
    at_load(app, st);
    at_swap(st);
}

static void at_secondary_set(lt_app *app, at_tab *st, int on)
{
    if (on && !st->sec_on) {
        size_t i;
        int pick = -1;
        for (i = 0; i < st->ntables; i++) { if ((int)i != st->s.tsel) { pick = (int)i; break; } }
        if (pick < 0) { lt_app_toast(app, 2, "No second fuel table to tune in this definition"); return; }
        if (st->o.at == NULL) { st->o.at = lt_at_new(); }
        st->o.tsel = pick;
        st->sec_on = 1;
        at_load_other(app, st);
    } else if (!on && st->sec_on) {
        if (st->on_sec) { at_swap(st); }   /* back to the primary, the secondary is dropped */
        st->sec_on = 0;
        slot_free_views(&st->o);
        if (st->o.at != NULL) { lt_at_free(st->o.at); st->o.at = NULL; }
    }
}

static void at_start(lt_app *app, at_tab *st)
{
    const char *why = lt_fuel_tune_refusal(app->ini, st->names[st->s.tsel]);
    lt_finding fnd[16];
    lt_preflight_input in;
    size_t nf, i;
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 2, "Connect to an ECU (or demo mode) to collect data, or replay a log");
        return;
    }
    if (why != NULL) { lt_app_toast(app, 3, "%s", why); return; }
    at_load(app, st);
    if (!st->s.loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
    memset(&in, 0, sizeof(in));
    in.target_table = app->ini->ve_analyze.target_table;
    if (st->s.have_tgt) { in.target_values = st->s.tgt.z; in.t_rows = st->s.tgt.rows; in.t_cols = st->s.tgt.cols; }
    in.ve_rows = st->s.ve.rows; in.ve_cols = st->s.ve.cols; in.ve_values = st->s.ve.z;
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
    (void)lt_atx_load_apply(&st->ch, st->load_src);
    lt_at_start(st->s.at);
    if (st->sec_on) {
        at_load_other(app, st);
        if (st->o.loaded) { lt_at_start(st->o.at); }
    }
    st->running = 1;
    st->s.nrecs = 0;
    st->last_frames = app->frames_ok;
    st->t0_ms = lt_plat_now_ms();
    st->info[0] = '\0';
}

static void slot_feed(at_tab *st, at_slot *sl, const lt_ve_point *pt)
{
    int ix, iy;
    if (!sl->loaded || sl->at == NULL) { return; }
    lt_at_add_data_point(sl->at, pt, sl->ve.x, sl->ve.cols, sl->ve.y, sl->ve.rows, &st->set, &st->f,
                         &st->auth);
    if (sl->recs != NULL && !st->replay) { sl->nrecs = lt_at_recommendations(sl->at, sl->recs, sl->cap); }
    ix = lt_at_find_bin(pt->rpm, sl->ve.x, sl->ve.cols);
    iy = lt_at_find_bin(pt->load, sl->ve.y, sl->ve.rows);
    if (ix >= 0 && iy >= 0) { lt_atx_trace_push(&sl->trace, (unsigned)ix, (unsigned)iy); }
}

static void at_feed(lt_app *app, at_tab *st)
{
    lt_ve_point pt;
    if (!st->running || !st->s.loaded || app->frames_ok == st->last_frames) { return; }
    st->last_frames = app->frames_ok;
    if (app->rt == NULL) { return; }
    if (!lt_at_sample_from_rt(app->rt, &st->ch, (unsigned long)(lt_plat_now_ms() - st->t0_ms), &pt)) { return; }
    slot_feed(st, &st->s, &pt);
    if (st->sec_on) { slot_feed(st, &st->o, &pt); }
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
    for (i = 0; i < st->s.nrecs; i++) {
        if (st->s.recs[i].cell_y == y && st->s.recs[i].cell_x == x) { return (int)i; }
    }
    return -1;
}

/* write the recommendations of the shown table; returns 1 when written */
static int at_apply(lt_app *app, at_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    size_t i, n = 0;
    double lo = st->auth.min_cell_value, hi = st->auth.max_cell_value, zl, zh;
    if (!st->s.loaded || st->s.nrecs == 0) { lt_app_toast(app, 2, "No recommendations to apply"); return 0; }
    lt_tblview_z_range(&st->s.ve, app->ini, &zl, &zh);
    if (zh > zl) { if (zl > lo) { lo = zl; } if (zh < hi) { hi = zh; } }
    lt_tblview_push_undo(&st->s.ve);
    for (i = 0; i < st->s.nrecs; i++) {
        const lt_at_rec *r = &st->s.recs[i];
        double v = r->recommended_value;
        if (lt_at_is_cell_locked(st->s.at, r->cell_x, r->cell_y)) { continue; }
        if (r->cell_y >= st->s.ve.rows || r->cell_x >= st->s.ve.cols) { continue; }
        v = v < lo ? lo : (v > hi ? hi : v);
        lt_tblview_set(&st->s.ve, r->cell_y, r->cell_x, lt_tabs_round(v, st->s.zdig));
        n++;
    }
    (void)lt_tabs_check_change(app, &st->s.ve);
    if (lt_tblview_commit(&st->s.ve, app->ini, &dst) < 0) {
        lt_app_toast(app, 3, "Write failed: %s", lt_err_get());
        return 0;
    }
    lt_app_edited(app);
    lt_app_toast(app, 1, "Applied %d cells to %s", (int)n, st->names[st->s.tsel]);
    {
        int was = st->running, rep = st->replay;
        at_load(app, st);
        if (rep) { st->replay = 0; st->s.nrecs = 0; }
        if (was) { lt_at_start(st->s.at); }
    }
    if (st->burn_after) {
        if (lt_app_can_burn(app)) { (void)lt_app_burn(app); }
        else { lt_app_toast(app, 2, "Sent to RAM; connect to burn"); }
    }
    return 1;
}

static void at_apply_other(lt_app *app, at_tab *st)
{
    if (!st->sec_on) { return; }
    at_swap(st);
    (void)at_apply(app, st);
    at_swap(st);
}

static void at_replay(lt_app *app, at_tab *st, const char *path)
{
    lt_log log;
    lt_replay_config cfg;
    lt_replay_report rep;
    lt_at_reftables ref;
    size_t i;
    int rc;
    if (!st->s.loaded) { at_load(app, st); }
    if (!st->s.loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
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
    lt_at_reftables_from_views(&st->s.ve, st->s.have_tgt ? &st->s.tgt : NULL, &ref);
    memset(&rep, 0, sizeof(rep));
    rc = lt_replay(&log, &st->ch, st->s.ve.x, st->s.ve.cols, st->s.ve.y, st->s.ve.rows, &ref, &cfg, &rep);
    if (rc < 0) {
        lt_app_toast(app, 3, "Replay failed: %s", lt_err_get());
    } else {
        lt_buf rb;
        st->s.nrecs = 0;
        for (i = 0; i < rep.ncells && st->s.nrecs < st->s.cap; i++) {
            lt_at_rec *r = &st->s.recs[st->s.nrecs++];
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
                    (int)rep.samples_all, (int)rep.ncells,
                    rep.has_validation ? " (validated)" : "");
        lt_buf_init(&rb);
        lt_buf_printf(&rb, "Log: %s\n", lt_path_base(path));
        lt_atx_replay_report(&rb, &rep, app->ini->ve_analyze.n_filters);
        lt_buf_puts(&rb, "\nAcceleration enrichment\n");
        if (lt_atx_ae_report(&rb, &log, &st->ch, st->set.target_afr) < 0) {
            lt_buf_puts(&rb, "  no throttle rate / AFR channel in this log\n");
        }
        lt_free(st->report);
        st->report = lt_buf_take(&rb);
        st->ptab = 3;
        lt_app_toast(app, 1, "Replay: %d cells proposed", (int)rep.ncells);
    }
    lt_replay_report_free(&rep);
    lt_log_free(&log);
}

static void at_sel_cells(at_tab *st, int lock, int unlock_all)
{
    size_t y, x, cols = st->s.ve.cols;
    for (y = 0; y < st->s.ve.rows; y++) {
        for (x = 0; x < cols; x++) {
            if (unlock_all) { lt_at_unlock_cell(st->s.at, (unsigned)x, (unsigned)y); }
            else if (st->s.sel[y * cols + x]) {
                if (lock == 2) {
                    if (lt_at_is_cell_locked(st->s.at, (unsigned)x, (unsigned)y)) { lt_at_unlock_cell(st->s.at, (unsigned)x, (unsigned)y); }
                    else { lt_at_lock_cell(st->s.at, (unsigned)x, (unsigned)y); }
                }
            }
        }
    }
}

static void at_grid(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int rows = (int)st->s.ve.rows, cols = (int)st->s.ve.cols, cell = lt_ui_cell_mode(ui);
    int cw = cell ? 64 : 66, ch = cell ? 16 : 24, rhw = cell ? 64 : 72, chh = cell ? 16 : 24;
    char b[48];
    double zlo = 1e300, zhi = -1e300, mh = 0.0, mw = 0.0, mc = 0.0;
    size_t k;
    lt_atx_map_maxima(st->s.recs, st->s.nrecs, &mh, &mw, &mc);
    for (k = 0; k < st->s.ve.rows * st->s.ve.cols; k++) {
        if (st->s.ve.z[k] < zlo) { zlo = st->s.ve.z[k]; }
        if (st->s.ve.z[k] > zhi) { zhi = st->s.ve.z[k]; }
    }
    if (!cell && cols > 0) {
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui) - th->scrollbar_w - th->spacing_x) - 360 - rhw - 2, fit = avail / cols;
        if (fit > cw) { cw = fit > 80 ? 80 : fit; }
    }
    if (!lt_ui_grid_begin(ui, "atgrid", rows, cols, cw, ch, rhw, chh)) { return; }
    {
        int r0, r1, c0, c1, r, c;
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, "Y \\ X");
        for (c = c0; c <= c1; c++) {
            lt_tabs_fmt(b, sizeof(b), st->s.ve.x[c], lt_tabs_const_digits(app->ini, st->s.t->x_const));
            lt_ui_grid_header(ui, 1, c, b);
        }
        for (r = r0; r <= r1; r++) {
            int dr = st->ybottom ? rows - 1 - r : r;
            lt_tabs_fmt(b, sizeof(b), st->s.ve.y[dr], lt_tabs_const_digits(app->ini, st->s.t->y_const));
            lt_ui_grid_header(ui, 0, r, b);
        }
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) {
                int dr = st->ybottom ? rows - 1 - r : r, ri = rec_index(st, (size_t)dr, (size_t)c);
                double cur = st->s.ve.z[(size_t)dr * (size_t)cols + (size_t)c];
                lt_color bg = lt_tabs_heat(app, th, zlo, zhi, cur), fg = lt_tabs_heat_text(bg);
                int locked = lt_at_is_cell_locked(st->s.at, (unsigned)c, (unsigned)dr);
                lt_rect cr;
                int pi = -1, an = 0;
                if (st->show_pred && ri < 0) {
                    for (k = 0; k < st->n_pc; k++) { if (st->pc[k].row == (size_t)dr && st->pc[k].col == (size_t)c) { pi = (int)k; break; } }
                }
                if (st->show_anom) {
                    for (k = 0; k < st->n_an; k++) { if (st->an[k].row == (size_t)dr && st->an[k].col == (size_t)c) { an = 1; break; } }
                }
                if (ri >= 0) {
                    const lt_at_rec *rc = &st->s.recs[ri];
                    double d = rc->beginning_value != 0.0 ? (rc->recommended_value - rc->beginning_value) / rc->beginning_value : 0.0;
                    double a = fabs(d) * 10.0;
                    int t256;
                    if (a > 1.0) { a = 1.0; }
                    t256 = (int)(a * 170.0) + 70;
                    if (st->hm_mode == LT_ATX_MAP_VALUE) {
                        bg = lt_color_mix(bg, d >= 0.0 ? th->change_richer : th->change_leaner, t256);
                    } else if (st->hm_mode == LT_ATX_MAP_CHANGE) {
                        bg = lt_color_mix(th->surface, d >= 0.0 ? th->change_richer : th->change_leaner,
                                          (int)(lt_atx_map_fraction(st->hm_mode, rc, mh, mw, mc) * 230.0) + 25);
                    } else {
                        bg = lt_tabs_heat(app, th, 0.0, 1.0, lt_atx_map_fraction(st->hm_mode, rc, mh, mw, mc));
                    }
                    fg = lt_tabs_heat_text(bg);
                    lt_tabs_fmt(b, sizeof(b), rc->recommended_value, st->s.zdig);
                } else if (pi >= 0) {
                    bg = lt_color_mix(bg, th->info, 110);
                    fg = lt_tabs_heat_text(bg);
                    lt_tabs_fmt(b, sizeof(b), st->pc[pi].predicted, st->s.zdig);
                } else {
                    if (st->hm_mode != LT_ATX_MAP_VALUE) {
                        bg = st->hm_mode == LT_ATX_MAP_CHANGE ? th->surface : lt_tabs_heat(app, th, 0.0, 1.0, 0.0);
                        fg = lt_tabs_heat_text(bg);
                    }
                    lt_tabs_fmt(b, sizeof(b), cur, st->s.zdig);
                }
                if (locked) { bg = lt_color_mix(bg, th->warning, 90); }
                lt_ui_grid_cell(ui, r, c, b, bg, fg);
                cr = lt_ui_grid_cell_rect(ui, r, c);
                if (ri >= 0 && !cell) {
                    lt_snprintf(b, sizeof(b), "%u", st->s.recs[ri].hit_count);
                    lt_ui_draw_vtext(ui, cr.x + 3, cr.y + 7, b, 9.0, 0.0, 0, 1, 1.0, fg);
                }
                if (locked && !cell) { lt_ui_draw_vtext(ui, cr.x + cr.w - 3, cr.y + 7, "L", 9.0, 0.0, 2, 1, 1.0, th->warning); }
                if (an) { lt_ui_draw_rect(ui, cr, th->error); }
            }
        }
        if (st->trace_on && !cell) {
            size_t ti, tn = lt_atx_trace_count(&st->s.trace);
            double px = 0.0, py = 0.0;
            for (ti = 0; ti < tn; ti++) {
                unsigned tx = 0, ty = 0;
                int vr;
                lt_rect tr;
                if (!lt_atx_trace_at(&st->s.trace, ti, &tx, &ty)) { continue; }
                vr = st->ybottom ? rows - 1 - (int)ty : (int)ty;
                if (vr < r0 || vr > r1 || (int)tx < c0 || (int)tx > c1) { px = 0.0; continue; }
                tr = lt_ui_grid_cell_rect(ui, vr, (int)tx);
                if (ti > 0 && px > 0.0) {
                    lt_ui_draw_line(ui, px, py, (double)(tr.x + tr.w / 2), (double)(tr.y + tr.h / 2), 2.0, th->accent);
                }
                px = (double)(tr.x + tr.w / 2); py = (double)(tr.y + tr.h / 2);
                if (ti + 1 == tn) { lt_ui_draw_circle_fill(ui, px, py, 4.0, th->accent); }
            }
        }
        memset(st->s.sel, 0, st->s.sel_cap);
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                if (lt_ui_grid_selected(ui, r, c)) {
                    st->s.sel[(size_t)(st->ybottom ? rows - 1 - r : r) * (size_t)cols + (size_t)c] = 1;
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

static void at_health_card(lt_app *app, at_tab *st);
static void at_health_extras(lt_app *app, at_tab *st);
static void at_session_group(lt_app *app, at_tab *st);
static void at_report_group(lt_app *app, at_tab *st);
static void ati(lt_vk_item *it, int kind, int icon, const char *label, const char *tip);

static void at_panel(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, ch = 0, w2[2], hw;
    char b[160];
    static const char *const hws[4] = { "Uniform", "Cell proximity", "Proximity squared", "Cell centre only" };
    if (!lt_ui_scroll_begin(ui, "atpanel")) { return; }
    if (!lt_ui_cell_mode(ui)) {
        lt_vk_item pt[5];
        lt_rect tr;
        int h2;
        lt_ui_layout_row(ui, 1, &w1, 32);
        tr = lt_ui_layout_next(ui);
        ati(&pt[0], LT_VK_BTN, LT_ICON_ACTIVITY, "Health", "Tune health, anomalies and predicted fills"); pt[0].on = st->ptab == 0;
        ati(&pt[1], LT_VK_BTN, LT_ICON_SLIDERS_HORIZONTAL, "Settings", "Targets, filters and authority limits"); pt[1].on = st->ptab == 1;
        ati(&pt[2], LT_VK_BTN, LT_ICON_LAYERS, "Session", "Tables, load source, update mode"); pt[2].on = st->ptab == 2;
        ati(&pt[3], LT_VK_BTN, LT_ICON_FILE_TEXT, "Report", "Analysis report and export"); pt[3].on = st->ptab == 3;
        ati(&pt[4], LT_VK_END, 0, NULL, NULL);
        h2 = lt_vk_toolbar_in(ui, pt, lt_rect_make(tr.x, tr.y + lt_ui_px(ui, 2), tr.w, lt_ui_px(ui, 28)));
        if (h2 >= 0 && h2 <= 3) { st->ptab = h2; }
        if (st->ptab == 0) { at_health_card(app, st); at_health_extras(app, st); }
    }
    if ((lt_ui_cell_mode(ui) || st->ptab == 0) && lt_ui_group_begin(ui, "Statistics", 0)) {
        lt_at_reject rj[8];
        size_t nrj = lt_at_rejections(st->s.at, rj, 8), i, rejected = 0, locks = 0, y, x;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "State: %s", st->running ? "running" : (st->replay ? "replay result" : "stopped"));
        lt_ui_label_ex(ui, b, st->running ? th->success : th->text2, LT_UI_ALIGN_LEFT);
        for (i = 0; i < nrj; i++) { rejected += rj[i].count; }
        for (y = 0; y < st->s.ve.rows; y++) { for (x = 0; x < st->s.ve.cols; x++) { if (lt_at_is_cell_locked(st->s.at, (unsigned)x, (unsigned)y)) { locks++; } } }
        lt_ui_labelf(ui, "Samples: %lu accepted, %lu rejected", lt_at_total_samples(st->s.at), (unsigned long)rejected);
        lt_ui_labelf(ui, "Cells with recommendations: %d of %d", (int)st->s.nrecs, (int)(st->s.ve.rows * st->s.ve.cols));
        lt_ui_labelf(ui, "Locked cells: %d", (int)locks);
        for (i = 0; i < nrj && i < 3; i++) { lt_ui_labelf(ui, "  %s: %lu", rj[i].reason, rj[i].count); }
        if (st->info[0] != '\0') { lt_ui_label_wrap(ui, st->info, th->text2); }
        lt_ui_group_end(ui);
    }
    if ((lt_ui_cell_mode(ui) || st->ptab == 1) && lt_ui_group_begin(ui, "Target / weighting", 0)) {
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
    if ((lt_ui_cell_mode(ui) || st->ptab == 1) && lt_ui_group_begin(ui, "Filters", 0)) {
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
    if ((lt_ui_cell_mode(ui) || st->ptab == 1) && lt_ui_group_begin(ui, "Authority limits", 0)) {
        numrow(ui, "Max change", "amc", &st->auth.max_cell_value_change, 0.0, 255.0, 1.0, 1, &ch);
        numrow(ui, "Max change %", "amp", &st->auth.max_cell_percentage_change, 0.0, 100.0, 1.0, 1, &ch);
        numrow(ui, "Min cell value", "amn", &st->auth.min_cell_value, 0.0, 255.0, 1.0, 0, &ch);
        numrow(ui, "Max cell value", "amx", &st->auth.max_cell_value, 0.0, 255.0, 1.0, 0, &ch);
        lt_ui_group_end(ui);
    }
    if (lt_ui_cell_mode(ui)) { at_health_extras(app, st); }
    if (lt_ui_cell_mode(ui) || st->ptab == 2) { at_session_group(app, st); }
    if (lt_ui_cell_mode(ui) || st->ptab == 3) { at_report_group(app, st); }
    lt_ui_scroll_end(ui);
    (void)ch;
}

/* ---- health card ---- */
static void at_health(at_tab *st)
{
    size_t n = st->s.ve.rows * st->s.ve.cols, i;
    unsigned *hits;
    if (!st->s.loaded || n == 0 || st->s.ve.y == NULL) { st->hr_valid = 0; return; }
    hits = (unsigned *)lt_calloc(n + 1, sizeof(unsigned));
    for (i = 0; i < st->s.nrecs; i++) {
        size_t k = st->s.recs[i].cell_y * st->s.ve.cols + st->s.recs[i].cell_x;
        if (k < n) { hits[k] = st->s.recs[i].hit_count; }
    }
    memset(&st->hr, 0, sizeof(st->hr));
    lt_health_score(st->s.ve.z, hits, st->s.ve.rows, st->s.ve.cols, st->s.ve.x, st->s.ve.y, &st->hr);
    st->hr_valid = 1;
    {
        lt_anomaly_config ac;
        lt_predict_config pcfg;
        lt_anomaly *an = (lt_anomaly *)lt_calloc(n + 1, sizeof(lt_anomaly));
        lt_anomaly_config_default(&ac);
        st->n_an = lt_anomaly_detect(st->s.ve.z, st->s.ve.rows, st->s.ve.cols, st->s.ve.x, st->s.ve.y, &ac, an, n);
        if (st->n_an > LT_COUNTOF(st->an)) { st->n_an = LT_COUNTOF(st->an); }
        memcpy(st->an, an, st->n_an * sizeof(lt_anomaly));
        lt_free(an);
        lt_predict_config_default(&pcfg);
        st->n_pc = lt_predict_cells(st->s.ve.z, hits, st->s.ve.rows, st->s.ve.cols, st->s.ve.x, st->s.ve.y, &pcfg,
                                    st->pc, LT_COUNTOF(st->pc));
    }
    lt_free(hits);
}

static lt_color grade_color(char g)
{
    switch (g) {
    case 'A': return LT_RGB(0x4C, 0xAF, 0x50);
    case 'B': return LT_RGB(0x8B, 0xC3, 0x4A);
    case 'C': return LT_RGB(0xFF, 0xC1, 0x07);
    case 'D': return LT_RGB(0xFF, 0x98, 0x00);
    default: return LT_RGB(0xF4, 0x43, 0x36);
    }
}

static void score_row(lt_ui *ui, lt_rect r, const char *label, unsigned score)
{
    const lt_theme *th = lt_ui_theme(ui);
    char b[16];
    int bw = r.w - lt_ui_px(ui, 168);
    lt_vk_text(ui, lt_rect_make(r.x, r.y, lt_ui_px(ui, 96), r.h), (int)LT_FONT_SANS, 11.0, label, th->text2, 0);
    lt_vk_bar(ui, lt_rect_make(r.x + lt_ui_px(ui, 100), r.y + r.h / 2 - lt_ui_px(ui, 2), bw, lt_ui_px(ui, 4)), (double)score / 100.0,
              lt_vk_score_color((int)score), th->dark ? LT_RGBA(255, 255, 255, 26) : LT_RGBA(0, 0, 0, 24));
    lt_snprintf(b, sizeof(b), "%u", score);
    lt_vk_text(ui, lt_rect_make(r.x + r.w - lt_ui_px(ui, 56), r.y, lt_ui_px(ui, 56), r.h), (int)LT_FONT_MONO_MEDIUM, 11.0, b, th->text, 2);
}

static void at_health_card(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, i;
    lt_rect r;
    char b[96];
    if (!st->hr_valid) { return; }
    lt_ui_layout_row(ui, 1, &w1, 96);
    r = lt_ui_layout_next(ui);
    lt_vk_card(ui, r, th->surface, th->border_subtle, lt_ui_px(ui, 10));
    {
        double fs_ = (double)lt_ui_scale_pct(ui) / 100.0, cx = (double)r.x + 46.0 * fs_, cy = (double)r.y + 48.0 * fs_;
        lt_color gc = grade_color(st->hr.grade);
        char gl[2];
        lt_ui_draw_circle_fill(ui, cx, cy, 30.0 * fs_, LT_RGBA(0, 0, 0, 70));
        lt_vk_ring(ui, cx, cy, 29.0 * fs_, 3.0 * fs_, (double)st->hr.overall_score / 100.0, gc, th->dark ? LT_RGBA(255, 255, 255, 30) : LT_RGBA(0, 0, 0, 30));
        gl[0] = st->hr.grade != '\0' ? st->hr.grade : '?'; gl[1] = '\0';
        lt_vk_text(ui, lt_rect_make((int)cx - lt_ui_px(ui, 24), (int)cy - lt_ui_px(ui, 20), lt_ui_px(ui, 48), lt_ui_px(ui, 26)), (int)LT_FONT_SANS_SEMIBOLD, 24.0, gl, gc, 1);
        lt_snprintf(b, sizeof(b), "%u/100", st->hr.overall_score);
        lt_vk_text(ui, lt_rect_make((int)cx - lt_ui_px(ui, 24), (int)cy + lt_ui_px(ui, 8), lt_ui_px(ui, 48), lt_ui_px(ui, 14)), (int)LT_FONT_MONO, 10.0, b, th->text2, 1);
    }
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 92), r.y + lt_ui_px(ui, 12), r.w - lt_ui_px(ui, 104), lt_ui_px(ui, 20)), (int)LT_FONT_SANS_SEMIBOLD, 15.0, "Tune Health", th->text, 0);
    lt_snprintf(b, sizeof(b), "%u/%u cells covered (%.0f%%)", (unsigned)st->hr.coverage_cells,
                (unsigned)st->hr.cells_all, st->hr.coverage_percent);
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 92), r.y + lt_ui_px(ui, 34), r.w - lt_ui_px(ui, 104), lt_ui_px(ui, 16)), (int)LT_FONT_SANS, 12.0, b, th->text2, 0);
    {
        lt_rect p1 = lt_rect_make(r.x + lt_ui_px(ui, 92), r.y + lt_ui_px(ui, 58), lt_ui_px(ui, 92), lt_ui_px(ui, 22)), p2 = lt_rect_make(r.x + lt_ui_px(ui, 190), r.y + lt_ui_px(ui, 58), lt_ui_px(ui, 100), lt_ui_px(ui, 22));
        lt_vk_pill(ui, p1, LT_RGBA(0xF4, 0x43, 0x36, 50), LT_RGBA(0xF4, 0x43, 0x36, 90));
        lt_snprintf(b, sizeof(b), "%u recs", (unsigned)st->hr.n_recommendations);
        lt_vk_text(ui, p1, (int)LT_FONT_SANS, 11.0, b, th->error, 1);
        lt_vk_pill(ui, p2, LT_RGBA(0x21, 0x96, 0xF3, 50), LT_RGBA(0x21, 0x96, 0xF3, 90));
        lt_snprintf(b, sizeof(b), "%d changes", (int)st->s.nrecs);
        lt_vk_text(ui, p2, (int)LT_FONT_SANS, 11.0, b, th->info, 1);
    }
    for (i = 0; i < (int)st->hr.n_regions; i++) {
        const lt_region_health *rg = &st->hr.regions[i];
        lt_rect cr, row;
        lt_ui_layout_row(ui, 1, &w1, 98);
        cr = lt_ui_layout_next(ui);
        lt_vk_card(ui, cr, th->surface, th->border_subtle, lt_ui_px(ui, 8));
        lt_vk_text(ui, lt_rect_make(cr.x + lt_ui_px(ui, 12), cr.y + lt_ui_px(ui, 8), cr.w - lt_ui_px(ui, 80), lt_ui_px(ui, 20)), (int)LT_FONT_SANS_SEMIBOLD, 13.0, rg->name, th->text, 0);
        lt_snprintf(b, sizeof(b), "%u", rg->score);
        lt_vk_text(ui, lt_rect_make(cr.x + cr.w - lt_ui_px(ui, 62), cr.y + lt_ui_px(ui, 8), lt_ui_px(ui, 50), lt_ui_px(ui, 20)), (int)LT_FONT_SANS_SEMIBOLD, 15.0, b,
                   lt_vk_score_color((int)rg->score), 2);
        row = lt_rect_make(cr.x + lt_ui_px(ui, 12), cr.y + lt_ui_px(ui, 30), cr.w - lt_ui_px(ui, 24), lt_ui_px(ui, 18));
        score_row(ui, row, "Coverage", rg->coverage_score);
        row.y += lt_ui_px(ui, 17); score_row(ui, row, "Smoothness", rg->smoothness_score);
        row.y += lt_ui_px(ui, 17); score_row(ui, row, "Monotonicity", rg->monotonicity_score);
        lt_snprintf(b, sizeof(b), "%u cells", (unsigned)rg->cell_count);
        lt_vk_text(ui, lt_rect_make(cr.x + lt_ui_px(ui, 12), cr.y + lt_ui_px(ui, 80), cr.w - lt_ui_px(ui, 24), lt_ui_px(ui, 14)), (int)LT_FONT_SANS, 10.0, b, th->muted, 0);
    }
    for (i = 0; i < (int)st->hr.n_recommendations && i < 4; i++) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, st->hr.recommendations[i], th->text2);
    }
}

/* ---- multi-table session: guarded switches ---- */
static at_slot *at_sec_slot(at_tab *st) { return st->on_sec ? &st->s : &st->o; }

static void at_apply_sec(lt_app *app, at_tab *st)
{
    if (st->on_sec) { (void)at_apply(app, st); } else { at_apply_other(app, st); }
}

static void at_pending_exec(lt_app *app, at_tab *st, int kind, int val, int send)
{
    if (kind == 1) {
        if (send) { (void)at_apply(app, st); }
        at_stop(st);
        st->s.tsel = val;
        at_load(app, st);
    } else if (kind == 2) {
        if (send) { at_apply_sec(app, st); }
        at_secondary_set(app, st, 0);
    } else if (kind == 3) {
        if (send) { at_apply_sec(app, st); }
        at_stop(st);
        at_sec_slot(st)->tsel = val;
        if (st->on_sec) { at_load(app, st); } else { at_load_other(app, st); }
    }
}

/* kind: 1 change the shown table, 2 drop the secondary, 3 change the secondary table */
static void at_request(lt_app *app, at_tab *st, int kind, int val)
{
    at_slot *drop = kind == 1 ? &st->s : at_sec_slot(st);
    if (kind == 1 && st->sec_on && val == st->o.tsel) {
        lt_app_toast(app, 2, "That table is already the other session table");
        return;
    }
    if (kind == 3 && val == (st->on_sec ? st->o.tsel : st->s.tsel)) {
        lt_app_toast(app, 2, "Pick a different table than the primary");
        return;
    }
    if (drop->nrecs > 0) { st->pend_kind = kind; st->pend_val = val; return; }
    at_pending_exec(app, st, kind, val, 0);
}

static void at_pending_draw(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int open = 1, w1 = -1, wb[4], pick = 0;
    char b[200];
    at_slot *drop;
    if (st->pend_kind == 0) { return; }
    drop = st->pend_kind == 1 ? &st->s : at_sec_slot(st);
    if (lt_ui_modal_begin(ui, "Unsent recommendations", 520, 170, &open)) {
        lt_snprintf(b, sizeof(b), "%s has %d accumulated recommendations that have not been sent to the ECU. "
                    "Send them first, discard them, or cancel the switch.",
                    st->names[drop->tsel], (int)drop->nrecs);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, b, th->text2);
        wb[0] = -1; wb[1] = 90; wb[2] = 140; wb[3] = 130;
        lt_ui_layout_row(ui, 4, wb, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Cancel", LT_UI_BTN_FLAT)) { pick = 1; }
        if (lt_ui_button_ex(ui, "Discard & Switch", LT_UI_BTN_FLAT)) { pick = 2; }
        if (lt_ui_button_ex(ui, "Send & Switch", LT_UI_BTN_DEFAULT)) { pick = 3; }
        lt_ui_modal_end(ui);
    }
    if (!open) { pick = 1; }
    if (pick != 0) {
        int kind = st->pend_kind, val = st->pend_val;
        st->pend_kind = 0;
        if (pick >= 2) { at_pending_exec(app, st, kind, val, pick == 3); }
    }
}

/* ---- Health tab extras: anomalies and predicted fills ---- */
static void at_apply_predicted(lt_app *app, at_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    size_t i, n = 0;
    if (st->n_pc == 0) { lt_app_toast(app, 2, "No predicted fills"); return; }
    lt_tblview_push_undo(&st->s.ve);
    for (i = 0; i < st->n_pc; i++) {
        if (st->pc[i].row >= st->s.ve.rows || st->pc[i].col >= st->s.ve.cols) { continue; }
        lt_tblview_set(&st->s.ve, st->pc[i].row, st->pc[i].col, lt_tabs_round(st->pc[i].predicted, st->s.zdig));
        n++;
    }
    (void)lt_tabs_check_change(app, &st->s.ve);
    if (lt_tblview_commit(&st->s.ve, app->ini, &dst) < 0) {
        lt_app_toast(app, 3, "Write failed: %s", lt_err_get());
        return;
    }
    lt_app_edited(app);
    lt_app_toast(app, 1, "Filled %d empty cells with predicted values", (int)n);
    st->hr_valid = 0;
}

static void at_health_extras(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, on, i;
    char b[200];
    if (!st->s.loaded || !lt_ui_group_begin(ui, "Anomalies and predicted fills", 0)) { return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    on = st->show_anom;
    if (lt_ui_checkbox(ui, "Outline anomalies on the grid", &on)) { st->show_anom = on; }
    lt_snprintf(b, sizeof(b), "%d anomalies found", (int)st->n_an);
    lt_ui_label_ex(ui, b, st->n_an > 0 ? th->warning : th->success, LT_UI_ALIGN_LEFT);
    for (i = 0; i < (int)st->n_an && i < 5; i++) {
        lt_snprintf(b, sizeof(b), "[%d,%d] %s", (int)st->an[i].row, (int)st->an[i].col, st->an[i].description);
        lt_ui_label_wrap(ui, b, th->text2);
    }
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    on = st->show_pred;
    if (lt_ui_checkbox(ui, "Show predicted fills for empty cells", &on)) { st->show_pred = on; }
    lt_snprintf(b, sizeof(b), "%d empty cells can be predicted", (int)st->n_pc);
    lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
    if (lt_ui_button_ex(ui, "Apply predicted fills", st->n_pc == 0 ? LT_UI_BTN_DISABLED : 0)) { at_apply_predicted(app, st); }
    lt_ui_group_end(ui);
}

/* ---- Session tab: second table, load source, update mode, delay test ---- */
static void at_session_group(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], on, sel;
    static const char *const loads[3] = { "MAP", "MAF", "TPS (Alpha-N)" };
    static const char *const modes[2] = { "Send to ECU RAM", "Send and burn" };
    static const char *const views[2] = { "Primary", "Secondary" };
    char b[160];
    if (!lt_ui_group_begin(ui, "Session", 0)) { return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    on = st->sec_on;
    if (lt_ui_checkbox(ui, "Tune a second table in this session", &on)) {
        if (on) { at_secondary_set(app, st, 1); } else { at_request(app, st, 2, 0); }
    }
    if (st->sec_on) {
        at_slot *sc = at_sec_slot(st), *pr = st->on_sec ? &st->o : &st->s;
        w2[0] = 130; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Secondary");
        sel = sc->tsel;
        if (lt_ui_combo(ui, "sectbl", &sel, (const char *const *)st->names, (int)st->ntables) && sel != sc->tsel) {
            at_request(app, st, 3, sel);
        }
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Showing");
        sel = st->on_sec;
        if (lt_ui_combo(ui, "secview", &sel, views, 2) && sel != st->on_sec) { at_swap(st); }
        lt_snprintf(b, sizeof(b), "Primary %s: %d recs   Secondary %s: %d recs", st->names[pr->tsel], (int)pr->nrecs,
                    st->names[sc->tsel], (int)sc->nrecs);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, b, th->text2);
    }
    w2[0] = 130; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Load source");
    sel = st->load_src;
    if (lt_ui_combo(ui, "loadsrc", &sel, loads, 3) && sel != st->load_src && !st->running) {
        if (lt_atx_load_apply(&st->ch, sel)) { st->load_src = sel; st->load_manual = 1; st->load_hint[0] = '\0'; }
        else { lt_app_toast(app, 2, "This definition has no %s channel", lt_atx_load_name(sel)); }
    }
    if (st->load_hint[0] != '\0') { lt_ui_layout_row(ui, 1, &w1, 0); lt_ui_label_wrap(ui, st->load_hint, th->text2); }
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Update mode");
    sel = st->burn_after;
    if (lt_ui_combo(ui, "updmode", &sel, modes, 2)) { st->burn_after = sel; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_button_ex(ui, "AFR delay test...", LT_UI_BTN_FLAT)) { (void)lt_app_dialog_push(app, "afrdelay", "", "AFR delay test"); }
    if (st->delay_note[0] != '\0') { lt_ui_label_wrap(ui, st->delay_note, th->text2); }
    lt_ui_group_end(ui);
}

/* ---- Report tab: heat map, export, analysis text ---- */
static void at_export_csv(lt_app *app, at_tab *st, const char *path)
{
    lt_buf b;
    int rc;
    lt_buf_init(&b);
    lt_atx_csv(&b, st->s.recs, st->s.nrecs, st->s.ve.x, st->s.ve.cols, st->s.ve.y, st->s.ve.rows);
    rc = lt_file_write(path, b.data, b.len);
    lt_buf_free(&b);
    if (rc < 0) { lt_app_toast(app, 3, "Cannot write %s: %s", path, lt_err_get()); }
    else { lt_app_toast(app, 1, "Exported %d recommendations to %s", (int)st->s.nrecs, lt_path_base(path)); }
}

static void at_save_proposal(lt_app *app, at_tab *st, const char *path)
{
    lt_buf b;
    size_t changed;
    int rc;
    lt_buf_init(&b);
    changed = lt_atx_proposal_json(&b, st->names[st->s.tsel], app->ini->signature != NULL ? app->ini->signature : "",
                                   st->s.ve.z, st->s.ve.rows, st->s.ve.cols, st->s.recs, st->s.nrecs);
    rc = lt_file_write(path, b.data, b.len);
    lt_buf_free(&b);
    if (rc < 0) { lt_app_toast(app, 3, "Cannot write %s: %s", path, lt_err_get()); }
    else { lt_app_toast(app, 1, "Proposal saved: %d cells would change", (int)changed); }
}

static void at_report_group(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], sel, on, i;
    const char *mnames[LT_ATX_MAP_COUNT];
    if (!lt_ui_group_begin(ui, "Heat map and export", 0)) { return; }
    for (i = 0; i < LT_ATX_MAP_COUNT; i++) { mnames[i] = lt_atx_map_name(i); }
    w2[0] = 130; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Heat map");
    sel = st->hm_mode;
    if (lt_ui_combo(ui, "hmmode", &sel, mnames, LT_ATX_MAP_COUNT)) { st->hm_mode = sel; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    on = st->trace_on;
    if (lt_ui_checkbox(ui, "Trace line (visited cells)", &on)) { st->trace_on = on; }
    if (lt_ui_button_ex(ui, "Export recommendations (CSV)...", st->s.nrecs == 0 ? LT_UI_BTN_DISABLED : 0)) {
        lt_app_file_dialog(app, AT_FD_CSV, LT_UI_FILE_SAVE, NULL, "autotune_recommendations.csv", "CSV|*.csv|All files|*");
    }
    if (lt_ui_button_ex(ui, "Save proposal file...", st->s.nrecs == 0 ? LT_UI_BTN_DISABLED : 0)) {
        lt_app_file_dialog(app, AT_FD_PROP, LT_UI_FILE_SAVE, NULL, "autotune_proposal.json", "JSON|*.json|All files|*");
    }
    lt_ui_group_end(ui);
    if (lt_ui_group_begin(ui, "Analysis report", 0)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (st->report != NULL) { lt_ui_label_wrap(ui, st->report, th->text2); }
        else { lt_ui_label_wrap(ui, "Replay a data log to see which samples counted, why others were dropped and how "
                                    "the proposal validates against unseen samples.", th->text2); }
        lt_ui_group_end(ui);
    }
}

/* ---- header / heat-map strip (pixel mode) ---- */
enum { AT_REPLAY, AT_RESET, AT_START, AT_APPLY, AT_BURN, AT_LOCK, AT_UNLOCK, AT_FLIP, AT_TAB_H, AT_TAB_S, AT_TRACE, AT_HMODE };

static void ati(lt_vk_item *it, int kind, int icon, const char *label, const char *tip)
{
    memset(it, 0, sizeof(*it));
    it->kind = kind; it->icon = icon; it->label = label; it->tip = tip;
}

static int at_header(lt_app *app, at_tab *st, int *combo_sel)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, hit, n = 0, ids[10], res = -1;
    lt_vk_item it[10];
    lt_rect hr, rr;
    /* band behind the header */
    lt_ui_layout_row(ui, 1, &w1, 54);
    hr = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, hr, th->elevated);
    lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, th->border_subtle);
    lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 14), hr.y, lt_ui_px(ui, 90), hr.h), (int)LT_FONT_SANS_SEMIBOLD, 16.0, "AutoTune", th->text, 0);
    if (st->running) {
        lt_rect bdg = lt_rect_make(hr.x + lt_ui_px(ui, 100), hr.y + lt_ui_px(ui, 18), lt_ui_px(ui, 62), lt_ui_px(ui, 18));
        lt_vk_pill(ui, bdg, LT_RGBA(0x4C, 0xAF, 0x50, 60), th->success);
        lt_vk_text(ui, bdg, (int)LT_FONT_SANS_SEMIBOLD, 10.0, "RUNNING", th->success, 1);
    }
    /* table selector laid out over the band */
    lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 176), hr.y + lt_ui_px(ui, 11), lt_ui_px(ui, 52), lt_ui_px(ui, 32)), (int)LT_FONT_SANS, 12.0, "Table", th->muted, 0);
    *combo_sel = st->s.tsel;
    lt_ui_layout_set_rect(ui, lt_rect_make(hr.x + lt_ui_px(ui, 224), hr.y + lt_ui_px(ui, 11), lt_ui_px(ui, 216), lt_ui_px(ui, 32)));
    if (st->ntables > 0) { lt_ui_combo(ui, "tsel", combo_sel, (const char *const *)st->names, (int)st->ntables); }
    ati(&it[n], LT_VK_SPACER, 0, NULL, NULL); ids[n++] = -1;
    ati(&it[n], LT_VK_BTN, LT_ICON_FOLDER_OPEN, "Replay log", "Replay a data log offline and propose changes"); ids[n++] = AT_REPLAY;
    ati(&it[n], LT_VK_BTN, LT_ICON_ROTATE_CCW, "Reset", "Discard collected data"); ids[n++] = AT_RESET;
    ati(&it[n], LT_VK_BTN, st->running ? LT_ICON_CIRCLE_STOP : LT_ICON_PLAY, st->running ? "Stop" : "Start",
        "Start/stop collecting (Space)");
    it[n].fill = st->running ? LT_RGB(0xD3, 0x2F, 0x2F) : LT_RGB(0x2E, 0xA0, 0x43);
    ids[n++] = AT_START;
    ati(&it[n], LT_VK_BTN, LT_ICON_UPLOAD, "Send to ECU", "Write the recommendations (except locked cells) to the table");
    it[n].disabled = st->s.nrecs == 0 ? 1 : 0; ids[n++] = AT_APPLY;
    ati(&it[n], LT_VK_BTN, LT_ICON_FLAME, "Burn", "Write the changes to ECU flash");
    it[n].accent = 1; it[n].disabled = lt_app_can_burn(app) ? 0 : 1; ids[n++] = AT_BURN;
    ati(&it[n], LT_VK_END, 0, NULL, NULL);
    rr = lt_rect_make(hr.x + lt_ui_px(ui, 450), hr.y + lt_ui_px(ui, 8), hr.w - lt_ui_px(ui, 450) - lt_ui_px(ui, 12), lt_ui_px(ui, 38));
    hit = lt_vk_toolbar_in(ui, it, rr);
    if (hit >= 0) { res = ids[hit]; }
    return res;
}

static int at_heat_strip(lt_app *app, at_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lt_vk_item it[10];
    int n = 0, ids[10], hit, w1 = -1;
    lt_rect r;
    lt_ui_layout_row(ui, 1, &w1, 40);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->bg_toolbar);
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 14), r.y, lt_ui_px(ui, 200), r.h), (int)LT_FONT_SANS_MEDIUM, 12.0,
               st->replay ? "Replay recommendations" : "Recommendations", th->text2, 0);
    ati(&it[n], LT_VK_SPACER, 0, NULL, NULL); ids[n++] = -1;
    ati(&it[n], LT_VK_BTN, LT_ICON_LAYERS, lt_atx_map_name(st->hm_mode), "Heat map view: value, weighting, change, coverage"); ids[n++] = AT_HMODE;
    ati(&it[n], LT_VK_BTN, LT_ICON_ROUTE, NULL, "Trace line: the path of the operating point through the table"); it[n].on = st->trace_on; ids[n++] = AT_TRACE;
    ati(&it[n], LT_VK_BTN, LT_ICON_ARROW_UP_DOWN, NULL, "Y axis: zero at the bottom"); it[n].on = st->ybottom; ids[n++] = AT_FLIP;
    ati(&it[n], LT_VK_BTN, LT_ICON_LOCK, "Lock", "Toggle lock on the selected cells (L); locked cells are not changed"); ids[n++] = AT_LOCK;
    ati(&it[n], LT_VK_BTN, LT_ICON_LOCK_OPEN, "Unlock", "Unlock every cell"); ids[n++] = AT_UNLOCK;
    ati(&it[n], LT_VK_END, 0, NULL, NULL);
    hit = lt_vk_toolbar_in(ui, it, lt_rect_make(r.x + lt_ui_px(ui, 200), r.y + lt_ui_px(ui, 5), r.w - lt_ui_px(ui, 212), lt_ui_px(ui, 30)));
    return hit >= 0 ? ids[hit] : -1;
}

static void at_legend(lt_ui *ui, const lt_theme *th)
{
    int w1 = -1, i;
    lt_rect r;
    lt_ui_layout_row(ui, 1, &w1, 26);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->bg_toolbar);
    lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y, th->border_subtle);
    {
        int bw = 140, bx = r.x + (r.w - bw) / 2;
        lt_vk_text(ui, lt_rect_make(bx - lt_ui_px(ui, 60), r.y, lt_ui_px(ui, 54), r.h), (int)LT_FONT_SANS, 10.0, "Richer", th->muted, 2);
        for (i = 0; i < bw; i++) {
            double t = (double)i / (double)(bw - 1);
            lt_color c = t < 0.5 ? lt_color_mix(th->change_richer, th->surface, (int)(t * 512.0))
                                 : lt_color_mix(th->surface, th->change_leaner, (int)((t - 0.5) * 512.0));
            lt_ui_draw_fill(ui, lt_rect_make(bx + i, r.y + lt_ui_px(ui, 9), 1, lt_ui_px(ui, 8)), c);
        }
        lt_vk_text(ui, lt_rect_make(bx + bw + lt_ui_px(ui, 6), r.y, lt_ui_px(ui, 60), r.h), (int)LT_FONT_SANS, 10.0, "Leaner", th->muted, 0);
    }
}

static void at_cmd(lt_app *app, at_tab *st, int cmd)
{
    switch (cmd) {
    case AT_REPLAY: lt_app_file_dialog(app, AT_FD_REPLAY, LT_UI_FILE_OPEN, NULL, "", "Logs|*.mlg;*.csv;*.msl|All files|*"); break;
    case AT_RESET: {
        int was = st->running;
        lt_at_start(st->s.at); st->running = was; st->s.nrecs = 0; st->replay = 0; st->info[0] = '\0';
        if (!was) { lt_at_stop(st->s.at); }
        lt_atx_trace_clear(&st->s.trace);
        if (st->sec_on && st->o.at != NULL) {
            lt_at_start(st->o.at);
            if (!was) { lt_at_stop(st->o.at); }
            st->o.nrecs = 0;
            lt_atx_trace_clear(&st->o.trace);
        }
        break; }
    case AT_START: if (st->running) { at_stop(st); } else { at_start(app, st); } break;
    case AT_APPLY: (void)at_apply(app, st); if (st->sec_on && st->o.nrecs > 0) { at_apply_other(app, st); } break;
    case AT_BURN: lt_app_burn(app); break;
    case AT_LOCK: at_sel_cells(st, 2, 0); break;
    case AT_UNLOCK: at_sel_cells(st, 0, 1); break;
    case AT_FLIP: st->ybottom = !st->ybottom; break;
    case AT_TRACE: st->trace_on = !st->trace_on; break;
    case AT_HMODE: st->hm_mode = (st->hm_mode + 1) % LT_ATX_MAP_COUNT; break;
    default: break;
    }
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
        st->s.at = lt_at_new();
        st->ybottom = 1;
        lt_at_filters_default(&st->f);
        st->ntables = lt_fuel_tunable_tables(app->ini, st->names, 24);
        st->s.tsel = 0;
        for (i = 0; i < (int)st->ntables; i++) {
            if (app->ini->ve_analyze.ve_table != NULL && strcmp(st->names[i], app->ini->ve_analyze.ve_table) == 0) { st->s.tsel = i; }
        }
        at_defaults(app, st);
        at_load(app, st);
    }
    if (!st->s.loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "AutoTune");
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    /* keep the page view in sync when not collecting */
    if (!st->running && !st->replay && !lt_tblview_is_dirty(&st->s.ve)) {
        static int tick = 0;
        if (++tick % 20 == 0 && st->s.nrecs == 0) { at_load(app, st); if (!st->s.loaded) { return; } }
    }
    picked = lt_app_file_result(app, AT_FD_REPLAY);
    if (picked != NULL) { at_replay(app, st, picked); }
    picked = lt_app_file_result(app, AT_FD_CSV);
    if (picked != NULL) { at_export_csv(app, st, picked); }
    picked = lt_app_file_result(app, AT_FD_PROP);
    if (picked != NULL) { at_save_proposal(app, st, picked); }
    if (lt_ui_key_pressed(ui, ' ', 0)) { if (st->running) { at_stop(st); } else { at_start(app, st); } }
    if (lt_ui_key_pressed(ui, 'l', 0)) { at_sel_cells(st, 2, 0); }
    st->frame++;
    if (!st->hr_valid || st->frame % 20 == 0 || st->hr_stamp != lt_at_total_samples(st->s.at)) {
        st->hr_stamp = lt_at_total_samples(st->s.at);
        at_health(st);
    }
    if (lt_ui_cell_mode(ui)) {
        /* toolbar */
        for (i = 0; i < 10; i++) { wt[i] = 0; }
        wt[0] = 90; wt[1] = 220;
        lt_ui_layout_row(ui, 10, wt, 0);
        lt_ui_label(ui, "VE table");
        combo_sel = st->s.tsel;
        if (st->ntables > 0 && lt_ui_combo(ui, "tsel", &combo_sel, (const char *const *)st->names, (int)st->ntables)) { }
        if (st->running) {
            if (lt_ui_button_ex(ui, "Stop", LT_UI_BTN_DANGER)) { at_stop(st); }
        } else if (lt_ui_button_ex(ui, "Start", LT_UI_BTN_PRIMARY)) { at_start(app, st); }
        if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_ACCENT | (st->s.nrecs == 0 ? LT_UI_BTN_DISABLED : 0))) { at_apply(app, st); }
        if (lt_ui_button_ex(ui, "Reset", LT_UI_BTN_FLAT)) { at_cmd(app, st, AT_RESET); }
        if (lt_ui_button_ex(ui, "Replay log...", LT_UI_BTN_FLAT)) { at_cmd(app, st, AT_REPLAY); }
        if (lt_ui_button_ex(ui, "Lock", LT_UI_BTN_FLAT)) { at_cmd(app, st, AT_LOCK); }
        if (lt_ui_button_ex(ui, "Unlock all", LT_UI_BTN_FLAT)) { at_cmd(app, st, AT_UNLOCK); }
        if (lt_ui_button_ex(ui, "Y-bot", LT_UI_BTN_FLAT | (st->ybottom ? LT_UI_BTN_SELECTED : 0))) { at_cmd(app, st, AT_FLIP); }
        if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT | (lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED))) { at_cmd(app, st, AT_BURN); }
    } else {
        int cmd = at_header(app, st, &combo_sel);
        if (cmd < 0) { cmd = at_heat_strip(app, st); } else { (void)at_heat_strip(app, st); }
        if (cmd >= 0) { at_cmd(app, st, cmd); }
    }
    if (combo_sel != st->s.tsel && st->ntables > 0) {
        at_request(app, st, 1, combo_sel);
        at_pending_draw(app, st);
        return;
    }
    wm[0] = -1; wm[1] = 360;
    lt_ui_layout_row(ui, 2, wm, lt_ui_cell_mode(ui) ? -1 : -27);
    at_grid(app, st);
    at_panel(app, st);
    if (!lt_ui_cell_mode(ui)) { at_legend(ui, th); }
    at_pending_draw(app, st);
    tab->dirty = st->running ? 1 : 0;
}

static void at_close(lt_app *app, lt_app_tab *tab)
{
    at_tab *st = (at_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    if (g_at == st) { g_at = NULL; }
    at_free_views(st);
    slot_free_views(&st->o);
    if (st->o.at != NULL) { lt_at_free(st->o.at); }
    lt_free(st->report);
    lt_at_filters_clear(&st->f);
    if (st->s.at != NULL) { lt_at_free(st->s.at); }
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_tabs_kind_autotune = { "autotune", at_draw, at_close };
