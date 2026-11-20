/* feat_trim.c - Trim Table Analyze tab (TunerStudio Ultra): per-cylinder fuel trim auto-tune.
 * The user picks the cylinder the single wideband is fitted to for this run (the data of the
 * other cylinders is kept while the sensor moves), or assigns a lambda channel per cylinder
 * when the ECU has one sensor per cylinder.  Errors are measured relative to the main VE: the
 * pending VE AutoTune correction is divided out, or a collector (reference) sensor is used.
 * The engine is src/autotune/at_trim.c; filters and authority are the VE AutoTune ones.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "app_ext.h"
#include "at_ui.h"
#include "tables_priv.h"
#include "view_kit.h"
#include "../autotune/at_trim.h"

#define TRIM_FD_REPLAY (LT_APP_FD_USER0 + 111)
#define TRIM_MAX_LAM 32

typedef struct trim_tab {
    lt_trim_state *s;
    lt_trim_config c;
    lt_at_channels ch;
    lt_trim_units u;
    lt_trim_table_ref refs[LT_TRIM_MAX_CYL];
    size_t ncyl;
    char cyl_lbl[LT_TRIM_MAX_CYL][48];
    const char *cyl_opts[LT_TRIM_MAX_CYL];
    lt_tblview v[LT_TRIM_MAX_CYL];
    int loaded[LT_TRIM_MAX_CYL], undo_ok[LT_TRIM_MAX_CYL];
    int cyl, zdig, xdig, ydig;
    int mode;                       /* 0 one wideband moved between cylinders, 1 a sensor per cylinder */
    int wb_sel, chan_sel[LT_TRIM_MAX_CYL], ref_sel;
    const char *lam[TRIM_MAX_LAM]; size_t nlam;
    const char *ref_opts[TRIM_MAX_LAM + 1];
    int use_ve, running, replay, burn_after, ptab, ybottom;
    lt_at_rec *recs; size_t nrecs, cap;
    unsigned char *sel; size_t sel_cap;
    unsigned long last_frames;
    double t0_ms;
    char msg[200], info[240], custom[160], target_name[64];
} trim_tab;

static trim_tab *g_trim = NULL;
static int g_trim_demo = 0;         /* run the simulated cylinder runs when the tab opens (demo target) */
static const char *const g_modes[2] = { "One wideband, moved between cylinders", "A sensor per cylinder" };

static int trim_avail(lt_app *app)
{
    lt_trim_table_ref r[2];
    return app->ini != NULL && lt_trim_find_tables(app->ini, r, 2) > 0;
}

static int lam_index(const trim_tab *st, const char *name)
{
    size_t i;
    if (name == NULL) { return -1; }
    for (i = 0; i < st->nlam; i++) { if (strcmp(st->lam[i], name) == 0) { return (int)i; } }
    return -1;
}

static int is_lambda_name(const char *n)
{
    size_t i, len = n != NULL ? strlen(n) : 0;
    for (i = 0; i + 6 <= len; i++) { if (lt_strnicmp(n + i, "lambda", 6) == 0) { return 1; } }
    return 0;
}

/* load every cylinder's trim table into its view and the engine */
static void trim_load(lt_app *app, trim_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    size_t i, k;
    const lt_ini_table *t;
    const lt_ini_const *zc = NULL;
    for (i = 0; i < st->ncyl; i++) {
        lt_tblview_free(&st->v[i]);
        lt_tblview_init(&st->v[i]);
        st->loaded[i] = 0;
        t = lt_ini_find_table(app->ini, st->refs[i].table);
        if (t == NULL || lt_tblview_load_table(&st->v[i], app->ini, t, &src) < 0 || st->v[i].y == NULL) { continue; }
        if (zc == NULL) {
            zc = lt_ini_find_const(app->ini, t->z_const);
            lt_trim_units_detect(zc, &st->u);
            st->zdig = zc != NULL ? zc->digits : 1;
            st->xdig = lt_tabs_const_digits(app->ini, t->x_const);
            st->ydig = lt_tabs_const_digits(app->ini, t->y_const);
        }
        {
            lt_tblview *v = &st->v[i];
            double *x = (double *)lt_malloc((v->cols + 1) * sizeof(double)), *y = (double *)lt_malloc((v->rows + 1) * sizeof(double));
            for (k = 0; k < v->cols; k++) { x[k] = lt_uconv_from_disp(&v->xu, v->x[k]); }
            for (k = 0; k < v->rows; k++) { y[k] = lt_uconv_from_disp(&v->yu, v->y[k]); }
            (void)lt_trim_set_table(st->s, i, x, v->cols, y, v->rows, v->z, &st->u);
            lt_free(x);
            lt_free(y);
        }
        st->loaded[i] = 1;
    }
    if (zc != NULL && zc->max > zc->min) {
        st->c.auth.min_cell_value = lt_trim_to_mult(&st->u, zc->min) * 100.0;
        st->c.auth.max_cell_value = lt_trim_to_mult(&st->u, zc->max) * 100.0;
    }
    /* target table on ECU axes */
    st->target_name[0] = '\0';
    lt_trim_set_target_table(st->s, NULL, 0, NULL, 0, NULL);
    t = app->ini->ve_analyze.target_table != NULL ? lt_ini_find_table(app->ini, app->ini->ve_analyze.target_table) : NULL;
    if (t != NULL) {
        lt_tblview tv;
        lt_tblview_init(&tv);
        if (lt_tblview_load_table(&tv, app->ini, t, &src) == 0 && tv.y != NULL) {
            double *x = (double *)lt_malloc((tv.cols + 1) * sizeof(double)), *y = (double *)lt_malloc((tv.rows + 1) * sizeof(double));
            for (k = 0; k < tv.cols; k++) { x[k] = lt_uconv_from_disp(&tv.xu, tv.x[k]); }
            for (k = 0; k < tv.rows; k++) { y[k] = lt_uconv_from_disp(&tv.yu, tv.y[k]); }
            lt_trim_set_target_table(st->s, x, tv.cols, y, tv.rows, tv.z);
            lt_strlcpy(st->target_name, t->name, sizeof(st->target_name));
            lt_free(x);
            lt_free(y);
        }
        lt_tblview_free(&tv);
    }
    if (st->cyl >= 0 && (size_t)st->cyl < st->ncyl && st->loaded[st->cyl]) {
        size_t n = st->v[st->cyl].rows * st->v[st->cyl].cols;
        if (n + 1 > st->cap) {
            lt_free(st->recs);
            lt_free(st->sel);
            st->cap = n + 1;
            st->recs = (lt_at_rec *)lt_calloc(st->cap, sizeof(lt_at_rec));
            st->sel = (unsigned char *)lt_calloc(st->cap, 1);
            st->sel_cap = st->cap;
        }
    }
}

static void trim_init(lt_app *app, trim_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    double cc = 0.0;
    size_t i;
    int li;
    st->ncyl = lt_trim_find_tables(app->ini, st->refs, LT_TRIM_MAX_CYL);
    if (lt_ini_get(app->ini, &src, "cylindersCount", &cc) == 0 && cc >= 1.0 && cc < (double)st->ncyl) {
        st->ncyl = (size_t)cc;
    }
    for (i = 0; i < st->ncyl; i++) {
        lt_snprintf(st->cyl_lbl[i], sizeof(st->cyl_lbl[i]), "Cylinder %d (%s)", st->refs[i].cylinder, st->refs[i].table);
        st->cyl_opts[i] = st->cyl_lbl[i];
        lt_tblview_init(&st->v[i]);
    }
    st->s = lt_trim_new(st->ncyl);
    lt_trim_config_default(&st->c);
    lt_at_filters_adopt_ini(&st->c.f, app->ini);
    lt_at_channels_from_ini(app->ini, &st->ch);
    st->nlam = lt_trim_lambda_channels(app->ini, st->lam, TRIM_MAX_LAM);
    st->ref_opts[0] = "None";
    for (i = 0; i < st->nlam; i++) { st->ref_opts[i + 1] = st->lam[i]; }
    li = lam_index(st, st->ch.afr);
    st->wb_sel = li >= 0 ? li : 0;
    for (i = 0; i < LT_TRIM_MAX_CYL; i++) { st->chan_sel[i] = st->wb_sel; }
    st->use_ve = 1;
    st->ybottom = 1;
    if (st->ncyl == 0) { lt_snprintf(st->msg, sizeof(st->msg), "This definition has no per-cylinder fuel trim tables."); return; }
    trim_load(app, st);
}

/* ---- live collection ---- */
static double rt_afr(lt_app *app, const char *name, int *ok)
{
    double v = 0.0;
    *ok = name != NULL && lt_ini_rt_get(app->rt, name, &v) ? 1 : 0;
    return is_lambda_name(name) ? lt_at_normalise_to_afr(v) : v;
}

static void trim_feed_cyl(lt_app *app, trim_tab *st, size_t cyl, const char *chan, unsigned long now)
{
    lt_at_channels ch = st->ch;
    lt_ve_point p;
    double vf = 1.0, ref = 0.0;
    int ok = 0;
    if (chan == NULL || !st->loaded[cyl]) { return; }
    lt_strlcpy(ch.afr, chan, sizeof(ch.afr));
    ch.afr_is_lambda = is_lambda_name(chan) ? LT_TRUE : LT_FALSE;
    if (!lt_at_sample_from_rt(app->rt, &ch, now, &p)) { return; }
    if (st->use_ve) { (void)lt_at_ui_ve_factor(app, p.rpm, p.load, &vf); }
    if (st->ref_sel > 0 && (size_t)st->ref_sel <= st->nlam) {
        ref = rt_afr(app, st->lam[st->ref_sel - 1], &ok);
        if (!ok) { ref = 0.0; }
    }
    lt_trim_add(st->s, cyl, &p, 0.0, vf, ref, &st->c);
}

static void trim_tick(lt_app *app)
{
    trim_tab *st = g_trim;
    unsigned long now;
    size_t i;
    if (st == NULL || !st->running) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { st->running = 0; lt_trim_stop(st->s); return; }
    if (app->frames_ok == st->last_frames || app->rt == NULL) { return; }
    st->last_frames = app->frames_ok;
    now = (unsigned long)(lt_plat_now_ms() - st->t0_ms);
    if (st->mode == 0) {
        if (st->cyl >= 0 && (size_t)st->cyl < st->ncyl && (size_t)st->wb_sel < st->nlam) {
            trim_feed_cyl(app, st, (size_t)st->cyl, st->lam[st->wb_sel], now);
        }
    } else {
        for (i = 0; i < st->ncyl; i++) {
            if ((size_t)st->chan_sel[i] < st->nlam) { trim_feed_cyl(app, st, i, st->lam[st->chan_sel[i]], now); }
        }
    }
}

static void trim_start(lt_app *app, trim_tab *st)
{
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 2, "Connect to an ECU (or demo mode) to collect data, or replay a log");
        return;
    }
    if (st->nlam == 0) { lt_app_toast(app, 3, "No lambda/AFR channel in this definition"); return; }
    trim_load(app, st);
    lt_trim_start(st->s);
    st->running = 1;
    st->replay = 0;
    st->last_frames = app->frames_ok;
    st->t0_ms = lt_plat_now_ms();
    st->info[0] = '\0';
}

static void trim_stop(trim_tab *st)
{
    st->running = 0;
    lt_trim_stop(st->s);
}

static void trim_replay(lt_app *app, trim_tab *st, const char *path)
{
    lt_log log;
    lt_at_channels ch = st->ch;
    long acc;
    double *x = NULL, *y = NULL, *z = NULL;
    size_t nx = 0, ny = 0;
    const char *chan = (size_t)st->wb_sel < st->nlam ? st->lam[st->wb_sel] : NULL;
    trim_stop(st);
    if (chan == NULL || st->cyl < 0 || (size_t)st->cyl >= st->ncyl) { return; }
    lt_log_init(&log);
    if (lt_log_read(path, &log) < 0) {
        lt_app_toast(app, 3, "Cannot read log: %s", lt_err_get());
        lt_log_free(&log);
        return;
    }
    if (st->mode == 1 && (size_t)st->chan_sel[st->cyl] < st->nlam) { chan = st->lam[st->chan_sel[st->cyl]]; }
    lt_strlcpy(ch.afr, chan, sizeof(ch.afr));
    ch.afr_is_lambda = is_lambda_name(chan) ? LT_TRUE : LT_FALSE;
    if (st->use_ve && lt_at_ui_ve_factor_table(app, &x, &nx, &y, &ny, &z)) {
        lt_trim_set_ve_factor_table(st->s, x, nx, y, ny, z);
    } else {
        lt_trim_set_ve_factor_table(st->s, NULL, 0, NULL, 0, NULL);
    }
    lt_free(x); lt_free(y); lt_free(z);
    acc = lt_trim_replay(st->s, (size_t)st->cyl, &log, &ch,
                         st->ref_sel > 0 && (size_t)st->ref_sel <= st->nlam ? st->lam[st->ref_sel - 1] : NULL, &st->c);
    if (acc < 0) {
        lt_app_toast(app, 3, "Replay failed: %s", lt_err_get());
    } else {
        st->replay = 1;
        lt_snprintf(st->info, sizeof(st->info), "Replay %s on cylinder %d: %ld of %ld samples used",
                    lt_path_base(path), st->refs[st->cyl].cylinder, acc, log.n);
        lt_app_toast(app, 1, "Trim replay: %ld samples used", acc);
    }
    lt_log_free(&log);
}

/* ---- send / undo ---- */
static int trim_send_cyl(lt_app *app, trim_tab *st, size_t cyl)
{
    lt_pagesrc dst = lt_app_pages(app);
    lt_at_rec *r;
    size_t i, n, m = 0, cap;
    lt_tblview *v = &st->v[cyl];
    double lo = -1e300, hi = 1e300;
    if (!st->loaded[cyl]) { return 0; }
    cap = v->rows * v->cols + 1;
    r = (lt_at_rec *)lt_calloc(cap, sizeof(lt_at_rec));
    n = lt_trim_recommendations(st->s, cyl, &st->c, r, cap);
    lt_tblview_z_range(v, app->ini, &lo, &hi);
    lt_tblview_push_undo(v);
    for (i = 0; i < n; i++) {
        double val = r[i].recommended_value;
        if (lt_at_is_cell_locked(lt_trim_engine(st->s, cyl), r[i].cell_x, r[i].cell_y)) { continue; }
        if (r[i].cell_y >= v->rows || r[i].cell_x >= v->cols) { continue; }
        if (hi > lo) { val = val < lo ? lo : (val > hi ? hi : val); }
        val = lt_tabs_round(val, st->zdig);
        if (fabs(val - v->z[(size_t)r[i].cell_y * v->cols + r[i].cell_x]) < 1e-9) { continue; }
        lt_tblview_set(v, r[i].cell_y, r[i].cell_x, val);
        m++;
    }
    lt_free(r);
    if (m == 0) { return 0; }
    (void)lt_tabs_check_change(app, v);
    if (lt_tblview_commit(v, app->ini, &dst) < 0) {
        lt_app_toast(app, 3, "Write failed: %s", lt_err_get());
        return -1;
    }
    lt_app_edited(app);
    st->undo_ok[cyl] = 1;
    return (int)m;
}

static void trim_send(lt_app *app, trim_tab *st, int all)
{
    size_t i;
    int total = 0, cyls = 0, rc;
    for (i = 0; i < st->ncyl; i++) {
        if (!all && (int)i != st->cyl) { continue; }
        rc = trim_send_cyl(app, st, i);
        if (rc < 0) { return; }
        if (rc > 0) { total += rc; cyls++; lt_trim_reset_cyl(st->s, i); }
    }
    if (total == 0) { lt_app_toast(app, 2, "No trim changes to send"); return; }
    trim_load(app, st);   /* the engines measure against the new trims from now on */
    st->replay = 0;
    lt_app_toast(app, 1, "Sent %d trim cells on %d cylinder%s", total, cyls, cyls == 1 ? "" : "s");
    if (st->burn_after) {
        if (lt_app_can_burn(app)) { (void)lt_app_burn(app); }
        else { lt_app_toast(app, 2, "Sent to RAM; connect to burn"); }
    }
}

static void trim_undo(lt_app *app, trim_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    lt_tblview *v = &st->v[st->cyl];
    if (!st->undo_ok[st->cyl] || !lt_tblview_undo(v)) { lt_app_toast(app, 2, "Nothing to undo"); return; }
    if (lt_tblview_commit(v, app->ini, &dst) < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); return; }
    lt_app_edited(app);
    st->undo_ok[st->cyl] = 0;
    trim_load(app, st);
    lt_app_toast(app, 1, "Trim table of cylinder %d restored", st->refs[st->cyl].cylinder);
}

static void trim_locks(trim_tab *st, int all_off)
{
    lt_tblview *v = &st->v[st->cyl];
    lt_at_state *at = lt_trim_engine(st->s, (size_t)st->cyl);
    size_t y, x;
    if (at == NULL) { return; }
    for (y = 0; y < v->rows; y++) {
        for (x = 0; x < v->cols; x++) {
            if (all_off) { lt_at_unlock_cell(at, (unsigned)x, (unsigned)y); }
            else if (st->sel != NULL && y * v->cols + x < st->sel_cap && st->sel[y * v->cols + x]) {
                if (lt_at_is_cell_locked(at, (unsigned)x, (unsigned)y)) { lt_at_unlock_cell(at, (unsigned)x, (unsigned)y); }
                else { lt_at_lock_cell(at, (unsigned)x, (unsigned)y); }
            }
        }
    }
}

/* ---- axis check, demo session ---- */
static int trim_axes_ok(const trim_tab *st)
{
    const lt_tblview *v = &st->v[st->cyl];
    size_t i;
    for (i = 1; i < v->cols; i++) { if (!(v->x[i] > v->x[i - 1])) { return 0; } }
    for (i = 1; i < v->rows; i++) { if (!(v->y[i] > v->y[i - 1])) { return 0; } }
    return 1;
}

/* spread unusable trim axes over the range of the VE table's axes (the trim tables share them) */
static void trim_fill_axes(lt_app *app, trim_tab *st)
{
    lt_pagesrc src = lt_app_pages(app), dst = lt_app_pages(app);
    const lt_ini_table *vt = app->ini->ve_analyze.ve_table != NULL ? lt_ini_find_table(app->ini, app->ini->ve_analyze.ve_table) : NULL;
    lt_tblview *v = &st->v[st->cyl], ve;
    double x0 = 1000.0, x1 = 6000.0, y0 = 30.0, y1 = 100.0;
    size_t i;
    lt_tblview_init(&ve);
    if (vt != NULL && lt_tblview_load_table(&ve, app->ini, vt, &src) == 0 && ve.y != NULL && ve.cols > 1 && ve.rows > 1 &&
        ve.x[ve.cols - 1] > ve.x[0] && ve.y[ve.rows - 1] > ve.y[0]) {
        x0 = ve.x[0]; x1 = ve.x[ve.cols - 1]; y0 = ve.y[0]; y1 = ve.y[ve.rows - 1];
    }
    lt_tblview_free(&ve);
    lt_tblview_push_undo(v);
    for (i = 0; i < v->cols; i++) { lt_tblview_set_x(v, i, x0 + (x1 - x0) * (double)i / (double)(v->cols > 1 ? v->cols - 1 : 1)); }
    for (i = 0; i < v->rows; i++) { lt_tblview_set_y(v, i, y0 + (y1 - y0) * (double)i / (double)(v->rows > 1 ? v->rows - 1 : 1)); }
    if (lt_tblview_commit(v, app->ini, &dst) < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); return; }
    lt_app_edited(app);
    trim_load(app, st);
    lt_app_toast(app, 1, "Trim table axes set");
}

/* demo mode: one simulated run per cylinder with the wideband moved between them; each
 * cylinder has its own (known) fuelling error, replayed through the log path */
static void trim_demo(lt_app *app, trim_tab *st)
{
    const char *names[4];
    const char *chan = (size_t)st->wb_sel < st->nlam ? st->lam[st->wb_sel] : NULL;
    lt_at_channels ch = st->ch;
    size_t cyl, x, y, k, keep = (size_t)st->cyl;
    lt_tblview *v;
    long used = 0;
    if (chan == NULL) { return; }
    if (!trim_axes_ok(st)) { trim_fill_axes(app, st); }
    if (!trim_axes_ok(st)) { return; }
    trim_stop(st);
    v = &st->v[st->cyl];
    names[0] = st->ch.rpm[0] ? st->ch.rpm : "RPMValue";
    names[1] = st->ch.load[0] ? st->ch.load : "MAPValue";
    names[2] = chan;
    names[3] = st->ch.clt[0] ? st->ch.clt : "coolant";
    lt_strlcpy(ch.rpm, names[0], sizeof(ch.rpm));
    lt_strlcpy(ch.load, names[1], sizeof(ch.load));
    lt_strlcpy(ch.afr, chan, sizeof(ch.afr));
    lt_strlcpy(ch.clt, names[3], sizeof(ch.clt));
    ch.afr_is_lambda = is_lambda_name(chan) ? LT_TRUE : LT_FALSE;
    ch.ve[0] = ch.tps[0] = ch.tps_rate[0] = ch.map[0] = ch.maf[0] = ch.fuel_cut[0] = ch.accel_enrich[0] = '\0';
    lt_trim_set_ve_factor_table(st->s, NULL, 0, NULL, 0, NULL);
    for (cyl = 0; cyl < st->ncyl; cyl++) {
        lt_log log;
        double sec = 0.0, err = 1.0 + 0.035 * sin((double)cyl * 1.9 + 0.4);
        long acc;
        lt_log_init(&log);
        lt_log_set_channels(&log, names, NULL, 4);
        for (y = 0; y < v->rows; y++) {
            for (x = 0; x < v->cols; x++) {
                double rpm = lt_uconv_from_disp(&v->xu, v->x[x]), load = lt_uconv_from_disp(&v->yu, v->y[y]);
                double e = err * (1.0 + 0.01 * (double)x / (double)(v->cols > 1 ? v->cols - 1 : 1));
                if (rpm < st->c.f.min_rpm + 50.0) { rpm = st->c.f.min_rpm + 50.0; }
                for (k = 0; k < 50; k++) {
                    double row[4], tgt = lt_trim_target_at(st->s, rpm, load, st->c.set.target_afr);
                    row[0] = rpm + 20.0 * sin(sec);
                    row[1] = load;
                    row[2] = tgt * e * (1.0 + 0.003 * sin(sec * 2.3));
                    if (ch.afr_is_lambda) { row[2] /= LT_AT_STOICH_AFR; }
                    row[3] = 88.0;
                    lt_log_append(&log, sec, row);
                    sec += 0.1;
                }
            }
        }
        acc = lt_trim_replay(st->s, cyl, &log, &ch, NULL, &st->c);
        if (acc > 0) { used += acc; }
        lt_log_free(&log);
    }
    st->cyl = (int)keep;
    st->replay = 1;
    lt_snprintf(st->info, sizeof(st->info), "Simulated runs on %d cylinders (wideband moved after each): %ld samples used",
                (int)st->ncyl, used);
}

/* ---- drawing ---- */
static int rec_at(const trim_tab *st, size_t y, size_t x)
{
    size_t i;
    for (i = 0; i < st->nrecs; i++) { if (st->recs[i].cell_y == y && st->recs[i].cell_x == x) { return (int)i; } }
    return -1;
}

static void trim_grid(lt_app *app, trim_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lt_tblview *v = &st->v[st->cyl];
    lt_at_state *at = lt_trim_engine(st->s, (size_t)st->cyl);
    int rows = (int)v->rows, cols = (int)v->cols, cell = lt_ui_cell_mode(ui);
    int cw = cell ? 64 : 84, ch = cell ? 16 : 40, rhw = cell ? 64 : 72, chh = cell ? 16 : 24;
    int r0, r1, c0, c1, r, c;
    char b[48];
    double hmax = 1.0;
    size_t i;
    for (i = 0; i < st->nrecs; i++) { if ((double)st->recs[i].hit_count > hmax) { hmax = (double)st->recs[i].hit_count; } }
    if (!cell && cols > 0) {
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui) - th->scrollbar_w) - rhw - 4, fit = avail / cols;
        int availh = lt_ui_unpx(ui, lt_ui_layout_body(ui).h) - chh - 4, fh = rows > 0 ? availh / rows : ch;
        if (fit > cw) { cw = fit > 200 ? 200 : fit; }
        if (fh > ch) { ch = fh > 72 ? 72 : fh; }
    }
    if (!lt_ui_grid_begin(ui, "trimgrid", rows, cols, cw, ch, rhw, chh)) { return; }
    lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
    lt_ui_grid_corner(ui, "Load \\ RPM");
    for (c = c0; c <= c1; c++) { lt_tabs_fmt(b, sizeof(b), v->x[c], st->xdig); lt_ui_grid_header(ui, 1, c, b); }
    for (r = r0; r <= r1; r++) {
        int dr = st->ybottom ? rows - 1 - r : r;
        lt_tabs_fmt(b, sizeof(b), v->y[dr], st->ydig);
        lt_ui_grid_header(ui, 0, r, b);
    }
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            int dr = st->ybottom ? rows - 1 - r : r, ri = rec_at(st, (size_t)dr, (size_t)c);
            double cur = v->z[(size_t)dr * (size_t)cols + (size_t)c];
            lt_color bg = th->table_cell, fg = th->text;
            int locked = at != NULL && lt_at_is_cell_locked(at, (unsigned)c, (unsigned)dr);
            lt_rect cr;
            if (ri >= 0) {
                const lt_at_rec *rc = &st->recs[ri];
                double d = lt_trim_to_mult(&st->u, rc->recommended_value) / lt_trim_to_mult(&st->u, cur) - 1.0;
                bg = fabs(d) > 1e-9 ? lt_at_ui_change(app, d, 0.05) : lt_at_ui_heat(app, LT_HEAT_COVERAGE, 0.15 + 0.5 * (double)rc->hit_count / hmax);
                fg = lt_tabs_heat_text(bg);
                lt_tabs_fmt(b, sizeof(b), rc->recommended_value, st->zdig);
            } else {
                lt_tabs_fmt(b, sizeof(b), cur, st->zdig);
                fg = th->text2;
            }
            if (locked) { bg = lt_color_mix(bg, th->warning, 90); fg = lt_tabs_heat_text(bg); }
            lt_ui_grid_cell(ui, r, c, b, bg, fg);
            cr = lt_ui_grid_cell_rect(ui, r, c);
            if (!cell && ri >= 0) {
                lt_snprintf(b, sizeof(b), "%u", st->recs[ri].hit_count);
                lt_ui_draw_vtext(ui, cr.x + 3, cr.y + 7, b, 9.0, 0.0, 0, 1, 1.0, fg);
                lt_tabs_fmt(b, sizeof(b), cur, st->zdig);
                lt_ui_draw_vtext(ui, cr.x + cr.w - 3, cr.y + cr.h - 7, b, 9.0, 0.0, 2, 1, 1.0, fg);
            }
            if (locked && !cell) { lt_ui_draw_vtext(ui, cr.x + cr.w - 3, cr.y + 7, "L", 9.0, 0.0, 2, 1, 1.0, th->warning); }
        }
    }
    if (st->sel != NULL) {
        memset(st->sel, 0, st->sel_cap);
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                size_t k = (size_t)(st->ybottom ? rows - 1 - r : r) * (size_t)cols + (size_t)c;
                if (k < st->sel_cap && lt_ui_grid_selected(ui, r, c)) { st->sel[k] = 1; }
            }
        }
    }
    lt_ui_grid_end(ui);
}

static void tnum(lt_ui *ui, const char *label, const char *id, double *v, double lo, double hi, double step, int dg)
{
    int w[2];
    w[0] = 140; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, label);
    (void)lt_ui_number(ui, id, v, lo, hi, step, dg);
}

static void trim_cyl_summary(lt_app *app, trim_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w[4];
    size_t i, k, n, cap = 0;
    lt_at_rec *r;
    char b[64];
    for (i = 0; i < st->ncyl; i++) { if (st->v[i].rows * st->v[i].cols + 1 > cap) { cap = st->v[i].rows * st->v[i].cols + 1; } }
    r = (lt_at_rec *)lt_calloc(cap + 1, sizeof(lt_at_rec));
    w[0] = 64; w[1] = 80; w[2] = 64; w[3] = -1;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label_ex(ui, "Cylinder", th->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Samples", th->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, "Cells", th->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, "Mean change", th->muted, LT_UI_ALIGN_RIGHT);
    for (i = 0; i < st->ncyl; i++) {
        double sum = 0.0;
        int changed = 0;
        n = lt_trim_recommendations(st->s, i, &st->c, r, cap);
        for (k = 0; k < n; k++) {
            double d = r[k].recommended_value - r[k].beginning_value;
            if (fabs(d) > 1e-9) { changed++; sum += d; }
        }
        lt_ui_layout_row(ui, 4, w, 0);
        lt_snprintf(b, sizeof(b), "%d", st->refs[i].cylinder);
        lt_ui_push_id_int(ui, (int)i);
        if (lt_ui_button_ex(ui, b, LT_UI_BTN_FLAT | ((int)i == st->cyl ? LT_UI_BTN_SELECTED : 0))) { st->cyl = (int)i; }
        lt_ui_pop_id(ui);
        lt_snprintf(b, sizeof(b), "%lu", lt_trim_samples(st->s, i));
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_RIGHT);
        lt_snprintf(b, sizeof(b), "%d", changed);
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_RIGHT);
        if (changed > 0) { lt_snprintf(b, sizeof(b), "%+.2f", sum / (double)changed); }
        else { lt_strlcpy(b, "-", sizeof(b)); }
        lt_ui_label_ex(ui, b, changed > 0 ? lt_at_ui_change(app, sum / (double)changed / 100.0, 0.05) : th->muted, LT_UI_ALIGN_RIGHT);
    }
    lt_free(r);
}

static void trim_panel(lt_app *app, trim_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], cell = lt_ui_cell_mode(ui), sel;
    char b[200];
    size_t i;
    if (!lt_ui_scroll_begin(ui, "trimpanel")) { return; }
    if (!cell) {
        lt_vk_item pt[3];
        lt_rect tr;
        int h2;
        lt_ui_layout_row(ui, 1, &w1, 32);
        tr = lt_ui_layout_next(ui);
        memset(pt, 0, sizeof(pt));
        pt[0].kind = LT_VK_BTN; pt[0].icon = LT_ICON_ACTIVITY; pt[0].label = "Session"; pt[0].on = st->ptab == 0;
        pt[1].kind = LT_VK_BTN; pt[1].icon = LT_ICON_SLIDERS_HORIZONTAL; pt[1].label = "Settings"; pt[1].on = st->ptab == 1;
        pt[2].kind = LT_VK_END;
        h2 = lt_vk_toolbar_in(ui, pt, lt_rect_make(tr.x, tr.y + lt_ui_px(ui, 2), tr.w, lt_ui_px(ui, 28)));
        if (h2 >= 0 && h2 <= 1) { st->ptab = h2; }
    }
    if ((cell || st->ptab == 0) && lt_ui_group_begin(ui, "Wideband", 0)) {
        w2[0] = 110; w2[1] = -1;
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = st->mode;
        if (lt_ui_combo(ui, "tmode", &sel, g_modes, 2)) { st->mode = sel; }
        if (st->mode == 0) {
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, "Sensor channel");
            sel = st->wb_sel;
            if (st->nlam > 0 && lt_ui_combo(ui, "twb", &sel, st->lam, (int)st->nlam)) { st->wb_sel = sel; }
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "Fit the wideband to cylinder %d's runner, then Start. The other cylinders keep their data while the sensor moves.",
                        st->ncyl > 0 ? st->refs[st->cyl].cylinder : 0);
            lt_ui_label_wrap(ui, b, th->text2);
        } else {
            for (i = 0; i < st->ncyl; i++) {
                lt_ui_layout_row(ui, 2, w2, 0);
                lt_snprintf(b, sizeof(b), "Cylinder %d", st->refs[i].cylinder);
                lt_ui_label(ui, b);
                sel = st->chan_sel[i];
                lt_ui_push_id_int(ui, (int)i);
                if (st->nlam > 0 && lt_ui_combo(ui, "tch", &sel, st->lam, (int)st->nlam)) { st->chan_sel[i] = sel; }
                lt_ui_pop_id(ui);
            }
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 0) && lt_ui_group_begin(ui, "Cylinders", 0)) {
        trim_cyl_summary(app, st);
        if (st->info[0] != '\0') { lt_ui_layout_row(ui, 1, &w1, 0); lt_ui_label_wrap(ui, st->info, th->text2); }
        if (app->conn == LT_APP_DEMO) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_button_ex(ui, "Simulate a run on every cylinder (demo)", LT_UI_BTN_FLAT)) { trim_demo(app, st); }
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 0) && lt_ui_group_begin(ui, "Relative to the main VE", 0)) {
        double f = 1.0;
        int have = lt_at_ui_ve_factor(app, 0.0, 0.0, &f);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, "Divide out the pending VE AutoTune correction", &st->use_ve)) { }
        lt_ui_label_wrap(ui, have ? "The AutoTune tab has VE recommendations: trims measure what is left after them."
                                  : "No VE AutoTune recommendations: trims take the whole error (tune the VE table first).",
                         have ? th->text2 : th->warning);
        w2[0] = 110; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Reference");
        sel = st->ref_sel;
        if (lt_ui_combo(ui, "tref", &sel, st->ref_opts, (int)st->nlam + 1)) { st->ref_sel = sel; }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "A collector sensor as reference measures each cylinder against the engine average.", th->muted);
        {
            int on = st->c.balance ? 1 : 0;
            if (lt_ui_checkbox(ui, "Balance: keep the cylinders' average fuelling", &on)) { st->c.balance = on ? LT_TRUE : LT_FALSE; }
        }
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Update mode");
        {
            static const char *const modes[2] = { "Send to ECU RAM", "Send and burn" };
            sel = st->burn_after;
            if (lt_ui_combo(ui, "tupd", &sel, modes, 2)) { st->burn_after = sel; }
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 1) && lt_ui_group_begin(ui, "Filters", 0)) {
        int ex = st->c.f.exclude_accel_enrich ? 1 : 0;
        tnum(ui, "Min RPM", "tminr", &st->c.f.min_rpm, 0.0, 20000.0, 100.0, 0);
        tnum(ui, "Max RPM", "tmaxr", &st->c.f.max_rpm, 0.0, 20000.0, 100.0, 0);
        tnum(ui, "Min coolant", "tclt", &st->c.f.min_clt, -50.0, 300.0, 1.0, 0);
        tnum(ui, "Max TPS rate", "ttps", &st->c.f.max_tps_rate, 0.0, 1000.0, 5.0, 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, "Exclude accel enrichment", &ex)) { st->c.f.exclude_accel_enrich = ex ? LT_TRUE : LT_FALSE; }
        w2[0] = 140; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Custom filter");
        if (lt_ui_text(ui, "tcf", st->custom, sizeof(st->custom), 0) & LT_UI_SUBMIT) {
            lt_at_filters_set_custom(&st->c.f, st->custom[0] != '\0' ? st->custom : NULL);
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 1) && lt_ui_group_begin(ui, "Weighting and authority", 0)) {
        tnum(ui, "Base weight", "tbw", &st->c.set.base_weight, 0.0, 500.0, 1.0, 0);
        tnum(ui, "Min change %", "tmc", &st->c.set.min_change, 0.0, 10.0, 0.1, 1);
        tnum(ui, "Delay ms (0=auto)", "tdl", &st->c.set.lambda_delay_ms, 0.0, 1000.0, 5.0, 0);
        tnum(ui, "Max change (pts)", "tamc", &st->c.auth.max_cell_value_change, 0.0, 100.0, 0.5, 1);
        tnum(ui, "Max change %", "tamp", &st->c.auth.max_cell_percentage_change, 0.0, 100.0, 1.0, 1);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "Limits work on the fuel multiplier in percent (100 = no trim); the table allows %.0f to %.0f.",
                    st->c.auth.min_cell_value, st->c.auth.max_cell_value);
        lt_ui_label_wrap(ui, b, th->muted);
        lt_ui_group_end(ui);
    }
    lt_ui_scroll_end(ui);
}

enum { TC_REPLAY, TC_RESET, TC_START, TC_SEND, TC_SENDALL, TC_UNDO, TC_BURN, TC_LOCK, TC_UNLOCK, TC_FLIP };

static void trim_cmd(lt_app *app, trim_tab *st, int cmd)
{
    switch (cmd) {
    case TC_REPLAY: lt_app_file_dialog(app, TRIM_FD_REPLAY, LT_UI_FILE_OPEN, NULL, "", "Logs|*.mlg;*.csv;*.msl|All files|*"); break;
    case TC_RESET: lt_trim_reset_cyl(st->s, (size_t)st->cyl); st->replay = 0; st->info[0] = '\0'; break;
    case TC_START: if (st->running) { trim_stop(st); } else { trim_start(app, st); } break;
    case TC_SEND: trim_send(app, st, 0); break;
    case TC_SENDALL: trim_send(app, st, 1); break;
    case TC_UNDO: trim_undo(app, st); break;
    case TC_BURN: (void)lt_app_burn(app); break;
    case TC_LOCK: trim_locks(st, 0); break;
    case TC_UNLOCK: trim_locks(st, 1); break;
    case TC_FLIP: st->ybottom = !st->ybottom; break;
    default: break;
    }
}

static int trim_header(lt_app *app, trim_tab *st, int *csel)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, n = 0, ids[12], hit;
    lt_vk_item it[12];
    lt_rect hr;
    lt_ui_layout_row(ui, 1, &w1, 54);
    hr = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, hr, th->elevated);
    lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, th->border_subtle);
    lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 14), hr.y, lt_ui_px(ui, 170), hr.h), (int)LT_FONT_SANS_SEMIBOLD, 16.0, "Trim Table Analyze", th->text, 0);
    if (st->running) {
        lt_rect bdg = lt_rect_make(hr.x + lt_ui_px(ui, 172), hr.y + lt_ui_px(ui, 18), lt_ui_px(ui, 62), lt_ui_px(ui, 18));
        lt_vk_pill(ui, bdg, LT_RGBA(0x4C, 0xAF, 0x50, 60), th->success);
        lt_vk_text(ui, bdg, (int)LT_FONT_SANS_SEMIBOLD, 10.0, "RUNNING", th->success, 1);
    }
    *csel = st->cyl;
    lt_ui_layout_set_rect(ui, lt_rect_make(hr.x + lt_ui_px(ui, 244), hr.y + lt_ui_px(ui, 11), lt_ui_px(ui, 250), lt_ui_px(ui, 32)));
    (void)lt_ui_combo(ui, "tcyl", csel, st->cyl_opts, (int)st->ncyl);
    memset(it, 0, sizeof(it));
    it[n].kind = LT_VK_SPACER; ids[n++] = -1;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_FOLDER_OPEN; it[n].label = "Replay log"; it[n].tip = "Analyze a log taken with the wideband on this cylinder"; ids[n++] = TC_REPLAY;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_ROTATE_CCW; it[n].label = "Reset"; it[n].tip = "Discard this cylinder's data"; ids[n++] = TC_RESET;
    it[n].kind = LT_VK_BTN; it[n].icon = st->running ? LT_ICON_CIRCLE_STOP : LT_ICON_PLAY; it[n].label = st->running ? "Stop" : "Start";
    it[n].fill = st->running ? LT_RGB(0xD3, 0x2F, 0x2F) : LT_RGB(0x2E, 0xA0, 0x43); ids[n++] = TC_START;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_UPLOAD; it[n].label = "Send"; it[n].tip = "Write this cylinder's recommendations (except locked cells)";
    it[n].disabled = st->nrecs == 0; ids[n++] = TC_SEND;
    it[n].kind = LT_VK_BTN; it[n].label = "Send all"; it[n].tip = "Write the recommendations of every cylinder"; ids[n++] = TC_SENDALL;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_UNDO_2; it[n].tip = "Undo the last send of this cylinder"; it[n].disabled = !st->undo_ok[st->cyl]; ids[n++] = TC_UNDO;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_FLAME; it[n].label = "Burn"; it[n].accent = 1; it[n].disabled = !lt_app_can_burn(app); ids[n++] = TC_BURN;
    it[n].kind = LT_VK_END;
    hit = lt_vk_toolbar_in(ui, it, lt_rect_make(hr.x + lt_ui_px(ui, 504), hr.y + lt_ui_px(ui, 8), hr.w - lt_ui_px(ui, 516), lt_ui_px(ui, 38)));
    return hit >= 0 ? ids[hit] : -1;
}

static int trim_strip(lt_app *app, trim_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lt_vk_item it[6];
    int n = 0, ids[6], hit, w1 = -1;
    lt_rect r;
    char b[160];
    lt_ui_layout_row(ui, 1, &w1, 40);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->bg_toolbar);
    lt_snprintf(b, sizeof(b), "%s - cylinder %d: %d cells, %lu samples", st->replay ? "Replay" : "Recommendations",
                st->refs[st->cyl].cylinder, (int)st->nrecs, lt_trim_samples(st->s, (size_t)st->cyl));
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 14), r.y, lt_ui_px(ui, 420), r.h), (int)LT_FONT_SANS_MEDIUM, 12.0, b, th->text2, 0);
    memset(it, 0, sizeof(it));
    it[n].kind = LT_VK_SPACER; ids[n++] = -1;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_ARROW_UP_DOWN; it[n].tip = "Y axis: zero at the bottom"; it[n].on = st->ybottom; ids[n++] = TC_FLIP;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_LOCK; it[n].label = "Lock"; it[n].tip = "Toggle lock on the selected cells (L)"; ids[n++] = TC_LOCK;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_LOCK_OPEN; it[n].label = "Unlock"; ids[n++] = TC_UNLOCK;
    it[n].kind = LT_VK_END;
    hit = lt_vk_toolbar_in(ui, it, lt_rect_make(r.x + lt_ui_px(ui, 430), r.y + lt_ui_px(ui, 5), r.w - lt_ui_px(ui, 442), lt_ui_px(ui, 30)));
    return hit >= 0 ? ids[hit] : -1;
}

static void trim_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    trim_tab *st = (trim_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, wm[2], csel, cell = lt_ui_cell_mode(ui), cmd = -1;
    const char *picked;
    LT_UNUSED(r);
    if (st == NULL) {
        st = (trim_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        g_trim = st;
        trim_init(app, st);
    }
    if (st->ncyl == 0 || !st->loaded[st->cyl]) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "Trim Table Analyze");
        lt_ui_label_wrap(ui, st->msg[0] != '\0' ? st->msg : "The trim table of this cylinder cannot be read.", th->error);
        return;
    }
    picked = lt_app_file_result(app, TRIM_FD_REPLAY);
    if (picked != NULL) { trim_replay(app, st, picked); }
    if (lt_ui_key_pressed(ui, 'l', 0)) { trim_locks(st, 0); }
    if (g_trim_demo) {
        g_trim_demo = 0;
        if (app->conn == LT_APP_DEMO) { trim_demo(app, st); }
    }
    st->nrecs = lt_trim_recommendations(st->s, (size_t)st->cyl, &st->c, st->recs, st->cap);
    if (cell) {
        int wt[10], i;
        for (i = 0; i < 10; i++) { wt[i] = 0; }
        wt[0] = 80; wt[1] = 260;
        lt_ui_layout_row(ui, 10, wt, 0);
        lt_ui_label(ui, "Trim table");
        csel = st->cyl;
        (void)lt_ui_combo(ui, "tcyl", &csel, st->cyl_opts, (int)st->ncyl);
        if (st->running) { if (lt_ui_button_ex(ui, "Stop", LT_UI_BTN_DANGER)) { cmd = TC_START; } }
        else if (lt_ui_button_ex(ui, "Start", LT_UI_BTN_PRIMARY)) { cmd = TC_START; }
        if (lt_ui_button_ex(ui, "Send", LT_UI_BTN_ACCENT | (st->nrecs == 0 ? LT_UI_BTN_DISABLED : 0))) { cmd = TC_SEND; }
        if (lt_ui_button_ex(ui, "Send all", LT_UI_BTN_FLAT)) { cmd = TC_SENDALL; }
        if (lt_ui_button_ex(ui, "Undo", LT_UI_BTN_FLAT | (st->undo_ok[st->cyl] ? 0 : LT_UI_BTN_DISABLED))) { cmd = TC_UNDO; }
        if (lt_ui_button_ex(ui, "Reset", LT_UI_BTN_FLAT)) { cmd = TC_RESET; }
        if (lt_ui_button_ex(ui, "Replay log...", LT_UI_BTN_FLAT)) { cmd = TC_REPLAY; }
        if (lt_ui_button_ex(ui, "Lock", LT_UI_BTN_FLAT)) { cmd = TC_LOCK; }
    } else {
        int c2;
        cmd = trim_header(app, st, &csel);
        c2 = trim_strip(app, st);
        if (cmd < 0) { cmd = c2; }
    }
    if (cmd >= 0) { trim_cmd(app, st, cmd); }
    if (csel != st->cyl && csel >= 0 && (size_t)csel < st->ncyl) {
        st->cyl = csel;
        st->replay = 0;
        trim_load(app, st);
        st->nrecs = lt_trim_recommendations(st->s, (size_t)st->cyl, &st->c, st->recs, st->cap);
    }
    if (!trim_axes_ok(st)) {
        int wb[2];
        wb[0] = -1; wb[1] = 170;
        lt_ui_layout_row(ui, 2, wb, 0);
        lt_ui_label_ex(ui, "The RPM/load axes of the trim tables are not ascending: set them first.", th->warning, LT_UI_ALIGN_LEFT);
        if (lt_ui_button_ex(ui, "Fill from VE axes", LT_UI_BTN_FLAT)) { trim_fill_axes(app, st); return; }
    }
    wm[0] = -1; wm[1] = 360;
    lt_ui_layout_row(ui, 2, wm, cell ? -1 : -27);
    trim_grid(app, st);
    trim_panel(app, st);
    if (!cell) {
        lt_rect lr;
        lt_ui_layout_row(ui, 1, &w1, 26);
        lr = lt_ui_layout_next(ui);
        lt_ui_draw_fill(ui, lr, th->bg_toolbar);
        lt_ui_draw_hline(ui, lr.x, lr.x + lr.w - 1, lr.y, th->border_subtle);
        lt_at_ui_change_legend(app, lr);
    }
    tab->dirty = st->running ? 1 : 0;
}

static void trim_close(lt_app *app, lt_app_tab *tab)
{
    trim_tab *st = (trim_tab *)tab->data;
    size_t i;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    if (g_trim == st) { g_trim = NULL; }
    for (i = 0; i < st->ncyl; i++) { lt_tblview_free(&st->v[i]); }
    lt_trim_config_clear(&st->c);
    lt_trim_free(st->s);
    lt_free(st->recs);
    lt_free(st->sel);
    lt_free(st);
    tab->data = NULL;
}

static int open_trim(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    if (app->ini == NULL) { return -1; }
    return lt_app_tab_open(app, "trimanalyze", "trimanalyze", "Trim Table Analyze", 1) != NULL ? 0 : -1;
}

static void act_trim(lt_app *app) { (void)open_trim(app, NULL); }

/* the tab with simulated per-cylinder runs already analysed (demo mode; screenshots) */
static int open_trim_demo(lt_app *app, const char *t)
{
    g_trim_demo = 1;
    return open_trim(app, t);
}

void lt_feat_trim_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    tk.kind = "trimanalyze"; tk.draw = trim_draw; tk.close = trim_close;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_target(reg, "trim-analyze", open_trim);
    lt_ext_add_target(reg, "trim-analyze-demo", open_trim_demo);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "T&rim Table Analyze", NULL, act_trim, trim_avail);
    lt_ext_add_tick(reg, trim_tick);
}
