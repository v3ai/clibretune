/* feat_wue.c - WUE Analyze tab (TunerStudio MS "WUE Analyze"): live and log-replay tuning of
 * the warm-up enrichment curve.  The curve with current and recommended values, per-bin hits,
 * start/stop, send to ECU with undo, bin locks, filters and authority limits.  The engine is
 * src/autotune/at_wue.c; heat colours come from the change/coverage heat-map contexts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "app_ext.h"
#include "at_ui.h"
#include "tables_priv.h"
#include "view_kit.h"
#include "../autotune/at_wue.h"

#define WUE_FD_REPLAY (LT_APP_FD_USER0 + 110)
#define WUE_ROWS 7
#define WUE_MAX_CURVES 16
#define WUE_MAX_FLT 24

typedef struct wue_tab {
    lt_wue_state *s;
    lt_wue_config c;
    lt_wue_channels ch;
    lt_tblview v;                   /* the warm-up curve (display units) */
    const lt_ini_curve *curve;
    const char *names[WUE_MAX_CURVES]; size_t ncurves;
    int csel, loaded, zdig, xdig;
    double offset, unity;
    double xecu[LT_WUE_MAX_BINS];   /* bins in ECU units (the engine's unit) */
    lt_wue_bin b[LT_WUE_MAX_BINS]; size_t nb;
    unsigned char sel[LT_WUE_MAX_BINS];
    unsigned char flt_on[WUE_MAX_FLT];
    const char *warm_opts[LT_WUE_MAX_BINS + 1]; char warm_lbl[LT_WUE_MAX_BINS][24];
    int warm_sel;                   /* 0 auto, else bin index + 1 */
    int running, replay, burn_after, ptab, have_offset, have_target, undo_ok, frame;
    unsigned long last_frames;
    double t0_ms, live_clt;
    int have_live;
    char msg[200], info[240], custom[160], target_name[64];
} wue_tab;

static wue_tab *g_wue = NULL;
static int g_wue_demo = 0;          /* run the simulated warm-up when the tab opens (demo target) */

static int wue_avail(lt_app *app)
{
    return app->ini != NULL && (app->ini->wue_analyze.wue_curve != NULL ||
                                lt_ini_find_const(app->ini, "cltFuelCorr") != NULL ||
                                lt_ini_find_const(app->ini, "wueRates") != NULL);
}

static int has_ci(const char *s, const char *w)
{
    size_t i, n, m = strlen(w);
    if (s == NULL) { return 0; }
    n = strlen(s);
    for (i = 0; i + m <= n; i++) { if (lt_strnicmp(s + i, w, m) == 0) { return 1; } }
    return 0;
}

/* candidate warm-up curves: the [WueAnalyze] curve first, then curves of the warm-up constants */
static void wue_curves(lt_app *app, wue_tab *st)
{
    const lt_ini *ini = app->ini;
    size_t i, j;
    st->ncurves = 0;
    if (ini->wue_analyze.wue_curve != NULL && lt_ini_find_curve(ini, ini->wue_analyze.wue_curve) != NULL) {
        st->names[st->ncurves++] = ini->wue_analyze.wue_curve;
    }
    for (i = 0; i < ini->n_curves && st->ncurves < WUE_MAX_CURVES; i++) {
        const lt_ini_curve *c = ini->curves[i];
        int dup = 0;
        if (c->y_const == NULL || c->x_const == NULL) { continue; }
        if (!(has_ci(c->y_const, "cltFuelCorr") || has_ci(c->y_const, "wueRates") || has_ci(c->y_const, "warmup") ||
              (has_ci(c->name, "warmup") && !has_ci(c->name, "afr")) || has_ci(c->name, "wue"))) {
            continue;
        }
        if (has_ci(c->name, "offset") || has_ci(c->name, "afr") || has_ci(c->name, "ignition")) { continue; }
        for (j = 0; j < st->ncurves; j++) { if (strcmp(st->names[j], c->name) == 0) { dup = 1; } }
        if (!dup) { st->names[st->ncurves++] = c->name; }
    }
}

/* AFR target offset curve and the target table, in ECU units */
static void wue_refs(lt_app *app, wue_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const lt_ini *ini = app->ini;
    const lt_ini_curve *oc = ini->wue_analyze.afr_comp_curve != NULL ? lt_ini_find_curve(ini, ini->wue_analyze.afr_comp_curve) : NULL;
    const char *tn = ini->wue_analyze.target_table != NULL ? ini->wue_analyze.target_table : ini->ve_analyze.target_table;
    const lt_ini_table *tt = tn != NULL ? lt_ini_find_table(ini, tn) : NULL;
    lt_tblview v;
    size_t i;
    st->have_offset = 0;
    st->have_target = 0;
    st->target_name[0] = '\0';
    lt_wue_set_afr_offset(st->s, NULL, NULL, 0);
    lt_wue_set_target_table(st->s, NULL, 0, NULL, 0, NULL);
    lt_tblview_init(&v);
    if (oc != NULL && lt_tblview_load_curve(&v, ini, oc, &src) == 0 && v.cols > 0 && v.cols <= LT_WUE_MAX_BINS) {
        double x[LT_WUE_MAX_BINS];
        for (i = 0; i < v.cols; i++) { x[i] = lt_uconv_from_disp(&v.xu, v.x[i]); }
        lt_wue_set_afr_offset(st->s, x, v.z, v.cols);
        st->have_offset = 1;
    }
    lt_tblview_free(&v);
    lt_tblview_init(&v);
    if (tt != NULL && tt->y_const != NULL && lt_tblview_load_table(&v, ini, tt, &src) == 0 && v.rows > 0 && v.cols > 0) {
        double *x = (double *)lt_malloc((v.cols + 1) * sizeof(double)), *y = (double *)lt_malloc((v.rows + 1) * sizeof(double));
        for (i = 0; i < v.cols; i++) { x[i] = lt_uconv_from_disp(&v.xu, v.x[i]); }
        for (i = 0; i < v.rows; i++) { y[i] = lt_uconv_from_disp(&v.yu, v.y[i]); }
        lt_wue_set_target_table(st->s, x, v.cols, y, v.rows, v.z);
        lt_strlcpy(st->target_name, tt->name, sizeof(st->target_name));
        st->have_target = 1;
        lt_free(x);
        lt_free(y);
    }
    lt_tblview_free(&v);
}

static void wue_warm_opts(wue_tab *st)
{
    size_t i;
    st->warm_opts[0] = "Auto (neutral end)";
    for (i = 0; i < st->v.cols && i < LT_WUE_MAX_BINS; i++) {
        lt_tabs_fmt(st->warm_lbl[i], sizeof(st->warm_lbl[i]), st->v.x[i], st->xdig);
        lt_strlcat(st->warm_lbl[i], " and warmer", sizeof(st->warm_lbl[i]));
        st->warm_opts[i + 1] = st->warm_lbl[i];
    }
    if (st->warm_sel > (int)st->v.cols) { st->warm_sel = 0; }
    st->c.warm_clt = st->warm_sel > 0 ? st->xecu[st->warm_sel - 1] : LT_WUE_WARM_AUTO;
}

static void wue_load(lt_app *app, wue_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const lt_ini_const *zc;
    size_t i;
    lt_tblview_free(&st->v);
    lt_tblview_init(&st->v);
    st->loaded = 0;
    st->nb = 0;
    if (st->csel < 0 || (size_t)st->csel >= st->ncurves) {
        lt_snprintf(st->msg, sizeof(st->msg), "This definition has no warm-up enrichment curve.");
        return;
    }
    st->curve = lt_ini_find_curve(app->ini, st->names[st->csel]);
    if (st->curve == NULL || lt_tblview_load_curve(&st->v, app->ini, st->curve, &src) < 0 || st->v.cols < 2) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load curve '%s': %s", st->names[st->csel], lt_err_get());
        return;
    }
    if (st->v.cols > LT_WUE_MAX_BINS) { st->v.cols = LT_WUE_MAX_BINS; }
    zc = lt_ini_find_const(app->ini, st->curve->y_const);
    st->zdig = zc != NULL ? zc->digits : 2;
    st->xdig = lt_tabs_const_digits(app->ini, st->curve->x_const);
    {
        int whole = 1;
        for (i = 0; i < st->v.cols; i++) { if (fabs(st->v.x[i] - floor(st->v.x[i] + 0.5)) > 1e-6) { whole = 0; } }
        if (whole) { st->xdig = 0; }
    }
    lt_wue_units_for(zc, strcmp(st->curve->name, app->ini->wue_analyze.wue_curve != NULL ? app->ini->wue_analyze.wue_curve : "") == 0
                             ? app->ini->wue_analyze.wue_percent_offset : 0.0,
                     &st->offset, &st->unity);
    for (i = 0; i < st->v.cols; i++) { st->xecu[i] = lt_uconv_from_disp(&st->v.xu, st->v.x[i]); }
    lt_wue_set_curve(st->s, st->xecu, st->v.z, st->v.cols, st->offset, st->unity);
    /* authority rails from the constant's limits */
    if (zc != NULL && zc->max > zc->min) {
        st->c.auth.min_cell_value = zc->min;
        st->c.auth.max_cell_value = zc->max;
    }
    wue_refs(app, st);
    wue_warm_opts(st);
    memset(st->sel, 0, sizeof(st->sel));
    st->loaded = 1;
    st->undo_ok = 0;
}

static void wue_defaults(lt_app *app, wue_tab *st)
{
    size_t i;
    lt_wue_config_clear(&st->c);
    lt_wue_config_default(&st->c);
    lt_wue_channels_from_ini(app->ini, &st->ch);
    for (i = 0; i < WUE_MAX_FLT; i++) { st->flt_on[i] = 1; }
    st->custom[0] = '\0';
}

/* ---- live collection ---- */
static const char *wue_declared_reject(lt_app *app, const wue_tab *st)
{
    const lt_ini_analyze *a = &app->ini->wue_analyze;
    size_t i;
    double v;
    for (i = 0; i < a->n_filters && i < WUE_MAX_FLT; i++) {
        const lt_ini_filter *f = &a->filters[i];
        if (!st->flt_on[i] || f->channel == NULL || f->channel[0] == '\0') { continue; }
        if (lt_ini_rt_get(app->rt, f->channel, &v) && lt_at_filter_rejects(f, LT_TRUE, LT_TRUE, v)) {
            return f->display != NULL ? f->display : f->name;
        }
    }
    return NULL;
}

static void wue_tick(lt_app *app)
{
    wue_tab *st = g_wue;
    lt_wue_point p;
    if (st == NULL) { return; }
    st->have_live = 0;
    if (app->rt != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) &&
        lt_ini_rt_get(app->rt, st->ch.at.clt, &st->live_clt)) {
        st->have_live = 1;
    }
    if (!st->running) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { st->running = 0; lt_wue_stop(st->s); return; }
    if (app->frames_ok == st->last_frames || app->rt == NULL) { return; }
    st->last_frames = app->frames_ok;
    if (!lt_wue_sample_from_rt(app->rt, &st->ch, (unsigned long)(lt_plat_now_ms() - st->t0_ms), &p)) { return; }
    p.reject = wue_declared_reject(app, st);
    lt_wue_add(st->s, &p, &st->c);
}

static void wue_start(lt_app *app, wue_tab *st)
{
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 2, "Connect to an ECU (or demo mode) to collect data, or replay a log");
        return;
    }
    wue_load(app, st);
    if (!st->loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
    lt_wue_start(st->s);
    st->running = 1;
    st->replay = 0;
    st->last_frames = app->frames_ok;
    st->t0_ms = lt_plat_now_ms();
    st->info[0] = '\0';
}

static void wue_stop(wue_tab *st)
{
    st->running = 0;
    lt_wue_stop(st->s);
}

static void wue_replay(lt_app *app, wue_tab *st, const char *path)
{
    lt_log log;
    long acc;
    wue_stop(st);
    if (!st->loaded) { wue_load(app, st); }
    if (!st->loaded) { lt_app_toast(app, 3, "%s", st->msg); return; }
    lt_log_init(&log);
    if (lt_log_read(path, &log) < 0) {
        lt_app_toast(app, 3, "Cannot read log: %s", lt_err_get());
        lt_log_free(&log);
        return;
    }
    (void)lt_log_use_channel_names(&log, app->ini);      /* TunerStudio logs: labels as names */
    {
        unsigned char en[WUE_MAX_FLT];
        size_t nf = app->ini->wue_analyze.n_filters < WUE_MAX_FLT ? app->ini->wue_analyze.n_filters : WUE_MAX_FLT;
        memcpy(en, st->flt_on, sizeof(en));
        acc = lt_wue_replay(st->s, &log, &st->ch, &st->c, app->ini->wue_analyze.filters, nf, en);
    }
    if (acc < 0) {
        lt_app_toast(app, 3, "Replay failed: %s", lt_err_get());
    } else {
        st->replay = 1;
        lt_snprintf(st->info, sizeof(st->info), "Replay %s: %ld of %ld samples used", lt_path_base(path), acc, log.n);
        lt_app_toast(app, 1, "WUE replay: %ld samples used", acc);
    }
    lt_log_free(&log);
}

/* ---- send / undo ---- */
static int wue_send(lt_app *app, wue_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    size_t i, n = 0;
    if (!st->loaded) { return 0; }
    lt_wue_bins(st->s, &st->c, st->b, LT_WUE_MAX_BINS);
    lt_tblview_push_undo(&st->v);
    for (i = 0; i < st->v.cols; i++) {
        double r = lt_tabs_round(st->b[i].recommended, st->zdig);
        if (lt_wue_is_locked(st->s, i) || (st->b[i].status != LT_WUE_REC && st->b[i].status != LT_WUE_WARM)) { continue; }
        if (fabs(r - st->v.z[i]) < 1e-9) { continue; }
        lt_tblview_set(&st->v, 0, i, r);
        n++;
    }
    if (n == 0) { lt_app_toast(app, 2, "No changes to send"); return 0; }
    (void)lt_tabs_check_change(app, &st->v);
    if (lt_tblview_commit(&st->v, app->ini, &dst) < 0) {
        lt_app_toast(app, 3, "Write failed: %s", lt_err_get());
        return 0;
    }
    lt_app_edited(app);
    st->undo_ok = 1;
    lt_app_toast(app, 1, "Sent %d warm-up bins to %s", (int)n, st->curve->title != NULL ? st->curve->title : st->curve->name);
    /* the collected factors were relative to the old values: start over on the new curve */
    lt_wue_update_values(st->s, st->v.z, st->v.cols);
    if (st->running) { lt_wue_start(st->s); } else { lt_wue_start(st->s); lt_wue_stop(st->s); }
    st->replay = 0;
    if (st->burn_after) {
        if (lt_app_can_burn(app)) { (void)lt_app_burn(app); }
        else { lt_app_toast(app, 2, "Sent to RAM; connect to burn"); }
    }
    return 1;
}

static void wue_undo(lt_app *app, wue_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    if (!st->undo_ok || !lt_tblview_undo(&st->v)) { lt_app_toast(app, 2, "Nothing to undo"); return; }
    if (lt_tblview_commit(&st->v, app->ini, &dst) < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); return; }
    lt_app_edited(app);
    lt_wue_update_values(st->s, st->v.z, st->v.cols);
    st->undo_ok = 0;
    lt_app_toast(app, 1, "Warm-up curve restored");
}

static void wue_lock_sel(wue_tab *st, int all_off)
{
    size_t i;
    for (i = 0; i < st->v.cols; i++) {
        if (all_off) { lt_wue_lock(st->s, i, LT_FALSE); }
        else if (st->sel[i]) { lt_wue_lock(st->s, i, !lt_wue_is_locked(st->s, i)); }
    }
}

/* ---- axis check, demo session ---- */
static int wue_axis_ok(const wue_tab *st)
{
    size_t i;
    for (i = 1; i < st->v.cols; i++) { if (!(st->xecu[i] > st->xecu[i - 1])) { return 0; } }
    return 1;
}

/* spread an unusable coolant axis: -40 C (or F) upwards in 10 C (18 F) steps */
static void wue_fill_axis(lt_app *app, wue_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    const lt_ini_const *xc = lt_ini_find_const(app->ini, st->curve->x_const);
    double step = (xc != NULL && xc->units != NULL && strchr(xc->units, 'F') != NULL) ? 18.0 : 10.0;
    size_t i;
    lt_tblview_push_undo(&st->v);
    for (i = 0; i < st->v.cols; i++) { lt_tblview_set_x(&st->v, i, lt_uconv_to_disp(&st->v.xu, -40.0 + step * (double)i)); }
    if (lt_tblview_commit(&st->v, app->ini, &dst) < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); return; }
    lt_app_edited(app);
    wue_load(app, st);
    lt_app_toast(app, 1, "Coolant axis set");
}

/* demo mode: a simulated cold start (lean when cold, a 2% VE error when warm, throttle blips)
 * fed through the log replay path */
static void wue_demo(lt_app *app, wue_tab *st)
{
    const char *names[6];
    lt_log log;
    long i, n = 19000, acc;
    names[0] = st->ch.at.rpm[0] ? st->ch.at.rpm : "RPMValue";
    names[1] = st->ch.at.clt;
    names[2] = st->ch.at.afr;
    names[3] = st->ch.at.tps[0] ? st->ch.at.tps : "TPSValue";
    names[4] = st->ch.at.load[0] ? st->ch.at.load : "MAPValue";
    names[5] = st->ch.at.tps_rate[0] ? st->ch.at.tps_rate : "TPSdot";
    if (!wue_axis_ok(st)) { wue_fill_axis(app, st); }
    if (!st->loaded || !wue_axis_ok(st) || st->ch.at.clt[0] == '\0' || st->ch.at.afr[0] == '\0') { return; }
    wue_stop(st);
    lt_log_init(&log);
    lt_log_set_channels(&log, names, NULL, 6);
    for (i = 0; i < n; i++) {
        double v[6], sec = (double)i / 10.0, clt = -10.0 + sec * 0.05, lean, tgt;
        if (clt > 88.0) { clt = 88.0; }
        lean = (clt < 40.0 ? 1.0 + 0.09 * (40.0 - clt) / 50.0 : 1.0) * 1.02;
        v[0] = sec < 2.0 ? 250.0 : 1100.0 + 40.0 * sin(sec * 0.7);
        v[1] = clt;
        v[3] = (i % 600) < 15 ? 55.0 : 3.0;            /* a blip every minute */
        v[5] = (i % 600) < 3 ? 400.0 : 0.0;
        v[4] = 35.0;
        tgt = lt_wue_target_at(st->s, v[0], v[4], st->c.target_afr) + lt_wue_afr_offset_at(st->s, clt);
        v[2] = tgt * lean * (1.0 + 0.004 * sin(sec * 3.1));
        if ((i % 600) < 15) { v[2] = 19.0; }
        if (st->ch.at.afr_is_lambda) { v[2] /= LT_AT_STOICH_AFR; }
        lt_log_append(&log, sec, v);
    }
    {
        lt_wue_channels ch = st->ch;
        lt_strlcpy(ch.at.rpm, names[0], sizeof(ch.at.rpm));
        lt_strlcpy(ch.at.tps, names[3], sizeof(ch.at.tps));
        lt_strlcpy(ch.at.load, names[4], sizeof(ch.at.load));
        lt_strlcpy(ch.at.tps_rate, names[5], sizeof(ch.at.tps_rate));
        ch.ego[0] = ch.cranking[0] = ch.engine[0] = ch.run_s[0] = '\0';
        acc = lt_wue_replay(st->s, &log, &ch, &st->c, NULL, 0, NULL);
    }
    lt_log_free(&log);
    if (acc >= 0) {
        st->replay = 1;
        lt_snprintf(st->info, sizeof(st->info), "Simulated warm-up from -10 to 88: %ld of %ld samples used", acc, n);
    }
}

/* ---- drawing ---- */
static double mult_of(const wue_tab *st, double v) { return st->unity > 0.0 ? (v + st->offset) / st->unity : 1.0; }

static void wue_chart(lt_app *app, wue_tab *st, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    size_t i, n = st->v.cols, widx = lt_wue_warm_index(st->s, &st->c);
    double lo = 1e300, hi = -1e300, x0 = st->v.x[0], x1 = st->v.x[n - 1], hmax = 1.0;
    int pl = lt_ui_px(ui, 52), pr = lt_ui_px(ui, 16), pt = lt_ui_px(ui, 26), pb = lt_ui_px(ui, 40);
    lt_rect pa;
    char b[48];
    lt_vk_card(ui, r, th->surface, th->border_subtle, lt_ui_px(ui, 8));
    pa = lt_rect_make(r.x + pl, r.y + pt, r.w - pl - pr, r.h - pt - pb);
    if (pa.w < 20 || pa.h < 20 || n < 2) { return; }
    for (i = 0; i < n; i++) {
        double a = st->v.z[i], c = st->b[i].recommended;
        if (a < lo) { lo = a; } if (a > hi) { hi = a; }
        if (c < lo) { lo = c; } if (c > hi) { hi = c; }
        if ((double)st->b[i].hits > hmax) { hmax = (double)st->b[i].hits; }
    }
    {
        double u = st->unity - st->offset, pad;
        if (u < lo) { lo = u; }
        pad = (hi - lo) * 0.08 + 1e-6;
        lo -= pad; hi += pad;
    }
    if (x1 <= x0) { x1 = x0 + 1.0; }
#define WX(v) ((double)pa.x + ((v) - x0) / (x1 - x0) * (double)pa.w)
#define WY(v) ((double)(pa.y + pa.h) - ((v) - lo) / (hi - lo) * (double)pa.h)
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 12), r.y + lt_ui_px(ui, 4), lt_ui_px(ui, 300), lt_ui_px(ui, 18)),
               (int)LT_FONT_SANS_MEDIUM, 12.0, st->curve->title != NULL ? st->curve->title : st->curve->name, th->text, 0);
    /* warm region */
    if (widx < n) {
        double wx = widx > 0 ? (WX(st->v.x[widx - 1]) + WX(st->v.x[widx])) / 2.0 : (double)pa.x;
        lt_ui_draw_fill(ui, lt_rect_make((int)wx, pa.y, pa.x + pa.w - (int)wx, pa.h), th->dark ? LT_RGBA(255, 160, 0, 22) : LT_RGBA(255, 160, 0, 30));
        lt_vk_text(ui, lt_rect_make((int)wx + lt_ui_px(ui, 4), pa.y + lt_ui_px(ui, 2), lt_ui_px(ui, 140), lt_ui_px(ui, 14)),
                   (int)LT_FONT_SANS, 10.0, "warm: held at no enrichment", th->muted, 0);
    }
    /* grid lines and y labels */
    for (i = 0; i <= 4; i++) {
        double v = lo + (hi - lo) * (double)i / 4.0;
        int y = (int)WY(v);
        lt_ui_draw_hline(ui, pa.x, pa.x + pa.w, y, th->border_subtle);
        lt_tabs_fmt(b, sizeof(b), v, st->zdig);
        lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 4), y - lt_ui_px(ui, 7), pl - lt_ui_px(ui, 8), lt_ui_px(ui, 14)), (int)LT_FONT_MONO, 10.0, b, th->muted, 2);
    }
    /* hit bars along the bottom (coverage context) */
    for (i = 0; i < n; i++) {
        double f = (double)st->b[i].hits / hmax;
        int bh = (int)(f * (double)lt_ui_px(ui, 26)), bx = (int)WX(st->v.x[i]);
        if (st->b[i].hits > 0) {
            lt_ui_draw_fill(ui, lt_rect_make(bx - lt_ui_px(ui, 5), pa.y + pa.h - bh, lt_ui_px(ui, 10), bh),
                            lt_at_ui_heat(app, LT_HEAT_COVERAGE, 0.25 + 0.75 * f));
        }
        lt_tabs_fmt(b, sizeof(b), st->v.x[i], st->xdig);
        lt_vk_text(ui, lt_rect_make(bx - lt_ui_px(ui, 24), pa.y + pa.h + lt_ui_px(ui, 4), lt_ui_px(ui, 48), lt_ui_px(ui, 14)), (int)LT_FONT_MONO, 10.0, b, th->muted, 1);
    }
    /* unity line */
    {
        int y = (int)WY(st->unity - st->offset);
        lt_ui_draw_line(ui, (double)pa.x, (double)y, (double)(pa.x + pa.w), (double)y, 1.0, th->muted);
    }
    /* current and recommended lines */
    for (i = 0; i + 1 < n; i++) {
        lt_ui_draw_line(ui, WX(st->v.x[i]), WY(st->v.z[i]), WX(st->v.x[i + 1]), WY(st->v.z[i + 1]), 2.0, th->text2);
        lt_ui_draw_line(ui, WX(st->v.x[i]), WY(st->b[i].recommended), WX(st->v.x[i + 1]), WY(st->b[i + 1].recommended), 2.0, th->accent);
    }
    for (i = 0; i < n; i++) {
        lt_ui_draw_circle_fill(ui, WX(st->v.x[i]), WY(st->v.z[i]), 3.0, th->text2);
        if (st->b[i].status == LT_WUE_REC || st->b[i].status == LT_WUE_WARM) {
            lt_ui_draw_circle_fill(ui, WX(st->v.x[i]), WY(st->b[i].recommended), 4.0,
                                   lt_at_ui_change(app, mult_of(st, st->b[i].recommended) / mult_of(st, st->v.z[i]) - 1.0, 0.1));
        }
        if (lt_wue_is_locked(st->s, i)) {
            lt_vk_text(ui, lt_rect_make((int)WX(st->v.x[i]) - lt_ui_px(ui, 6), (int)WY(st->v.z[i]) - lt_ui_px(ui, 20), lt_ui_px(ui, 12), lt_ui_px(ui, 12)),
                       (int)LT_FONT_SANS_SEMIBOLD, 10.0, "L", th->warning, 1);
        }
    }
    /* live coolant */
    if (st->have_live) {
        double lx = lt_uconv_to_disp(&st->v.xu, st->live_clt);
        if (lx >= x0 && lx <= x1) {
            lt_ui_draw_line(ui, WX(lx), (double)pa.y, WX(lx), (double)(pa.y + pa.h), 1.5, th->cursor_live);
        }
    }
    /* legend */
    {
        int lx = r.x + r.w - lt_ui_px(ui, 250), ly = r.y + lt_ui_px(ui, 13);
        lt_ui_draw_line(ui, (double)lx, (double)ly, (double)(lx + lt_ui_px(ui, 18)), (double)ly, 2.0, th->text2);
        lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 22), ly - lt_ui_px(ui, 7), lt_ui_px(ui, 80), lt_ui_px(ui, 14)), (int)LT_FONT_SANS, 10.0, "Current", th->text2, 0);
        lx += lt_ui_px(ui, 80);
        lt_ui_draw_line(ui, (double)lx, (double)ly, (double)(lx + lt_ui_px(ui, 18)), (double)ly, 2.0, th->accent);
        lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 22), ly - lt_ui_px(ui, 7), lt_ui_px(ui, 90), lt_ui_px(ui, 14)), (int)LT_FONT_SANS, 10.0, "Recommended", th->text2, 0);
        lx += lt_ui_px(ui, 110);
        lt_ui_draw_fill(ui, lt_rect_make(lx, ly - lt_ui_px(ui, 4), lt_ui_px(ui, 8), lt_ui_px(ui, 8)), lt_at_ui_heat(app, LT_HEAT_COVERAGE, 0.8));
        lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 12), ly - lt_ui_px(ui, 7), lt_ui_px(ui, 40), lt_ui_px(ui, 14)), (int)LT_FONT_SANS, 10.0, "Hits", th->text2, 0);
    }
#undef WX
#undef WY
}

static void wue_grid(lt_app *app, wue_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    static const char *const rows[WUE_ROWS] = { "Current", "Recommended", "Change %", "Hits", "Mean AFR", "Target AFR", "Status" };
    int cols = (int)st->v.cols, cell = lt_ui_cell_mode(ui), cw = cell ? 9 * 8 : 70, ch = cell ? 16 : 24, rhw = cell ? 112 : 104;
    double hmax = 1.0;
    char b[48];
    int r0, r1, c0, c1, r, c;
    size_t i;
    for (i = 0; i < st->v.cols; i++) { if ((double)st->b[i].hits > hmax) { hmax = (double)st->b[i].hits; } }
    if (!cell && cols > 0) {
        int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui) - th->scrollbar_w) - rhw - 4, fit = avail / cols;
        if (fit > cw) { cw = fit > 110 ? 110 : fit; }
        if (fit < cw) { cw = fit < 52 ? 52 : fit; }
    }
    if (!lt_ui_grid_begin(ui, "wuegrid", WUE_ROWS, cols, cw, ch, rhw, ch)) { return; }
    lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
    lt_ui_grid_corner(ui, "Coolant");
    for (c = c0; c <= c1; c++) {
        lt_tabs_fmt(b, sizeof(b), st->v.x[c], st->xdig);
        lt_ui_grid_header(ui, 1, c, b);
    }
    for (r = r0; r <= r1; r++) { lt_ui_grid_header(ui, 0, r, rows[r]); }
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            const lt_wue_bin *bn = &st->b[c];
            lt_color bg = th->table_cell, fg = th->text;
            double d = mult_of(st, bn->recommended) / mult_of(st, bn->current) - 1.0;
            int has = bn->status == LT_WUE_REC || bn->status == LT_WUE_WARM;
            b[0] = '\0';
            switch (r) {
            case 0: lt_tabs_fmt(b, sizeof(b), bn->current, st->zdig); break;
            case 1:
                lt_tabs_fmt(b, sizeof(b), bn->recommended, st->zdig);
                if (has && fabs(d) > 1e-9) { bg = lt_at_ui_change(app, d, 0.1); fg = lt_tabs_heat_text(bg); }
                break;
            case 2:
                if (has) { lt_snprintf(b, sizeof(b), "%+.1f", d * 100.0); }
                else { lt_strlcpy(b, "-", sizeof(b)); }
                break;
            case 3:
                lt_snprintf(b, sizeof(b), "%u", bn->hits);
                if (bn->hits > 0) { bg = lt_at_ui_heat(app, LT_HEAT_COVERAGE, (double)bn->hits / hmax); fg = lt_tabs_heat_text(bg); }
                break;
            case 4:
            case 5:
                if (bn->weight > 0.0) { lt_snprintf(b, sizeof(b), "%.2f", r == 4 ? bn->afr_mean : bn->target_mean); }
                else { lt_strlcpy(b, "-", sizeof(b)); }
                fg = th->text2;
                break;
            default:
                lt_strlcpy(b, bn->status == LT_WUE_REC ? "tuned" : bn->status == LT_WUE_WARM ? "warm" :
                              bn->status == LT_WUE_LOCKED ? "locked" : bn->status == LT_WUE_FEW ? "few" : "-", sizeof(b));
                fg = bn->status == LT_WUE_REC ? th->success : bn->status == LT_WUE_WARM ? th->info : th->muted;
                break;
            }
            if (lt_wue_is_locked(st->s, (size_t)c)) { bg = lt_color_mix(bg, th->warning, 90); fg = lt_tabs_heat_text(bg); }
            lt_ui_grid_cell(ui, r, c, b, bg, fg);
        }
    }
    memset(st->sel, 0, sizeof(st->sel));
    for (r = 0; r < WUE_ROWS; r++) {
        for (c = 0; c < cols; c++) { if (lt_ui_grid_selected(ui, r, c)) { st->sel[c] = 1; } }
    }
    lt_ui_grid_end(ui);
}

static void nrow(lt_ui *ui, const char *label, const char *id, double *v, double lo, double hi, double step, int dg)
{
    int w[2];
    w[0] = 150; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, label);
    (void)lt_ui_number(ui, id, v, lo, hi, step, dg);
}

static void crow(lt_ui *ui, const char *label, lt_bool *v)
{
    int w1 = -1, on = *v ? 1 : 0;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, label, &on)) { *v = on ? LT_TRUE : LT_FALSE; }
}

static void wue_panel(lt_app *app, wue_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], cell = lt_ui_cell_mode(ui), sel;
    char b[200];
    size_t i;
    if (!lt_ui_scroll_begin(ui, "wuepanel")) { return; }
    if (!cell) {
        lt_vk_item pt[4];
        lt_rect tr;
        int h2;
        lt_ui_layout_row(ui, 1, &w1, 32);
        tr = lt_ui_layout_next(ui);
        memset(pt, 0, sizeof(pt));
        pt[0].kind = LT_VK_BTN; pt[0].icon = LT_ICON_ACTIVITY; pt[0].label = "Session"; pt[0].on = st->ptab == 0;
        pt[1].kind = LT_VK_BTN; pt[1].icon = LT_ICON_SLIDERS_HORIZONTAL; pt[1].label = "Filters"; pt[1].on = st->ptab == 1;
        pt[2].kind = LT_VK_BTN; pt[2].icon = LT_ICON_THERMOMETER; pt[2].label = "Convergence"; pt[2].on = st->ptab == 2;
        pt[3].kind = LT_VK_END;
        h2 = lt_vk_toolbar_in(ui, pt, lt_rect_make(tr.x, tr.y + lt_ui_px(ui, 2), tr.w, lt_ui_px(ui, 28)));
        if (h2 >= 0 && h2 <= 2) { st->ptab = h2; }
    }
    if ((cell || st->ptab == 0) && lt_ui_group_begin(ui, "Session", 0)) {
        lt_at_reject rj[8];
        size_t nrj = lt_wue_rejections(st->s, rj, 8);
        unsigned long rejected = 0, wn = 0;
        double wf = lt_wue_warm_factor(st->s, &st->c, &wn);
        int tuned = 0;
        for (i = 0; i < nrj; i++) { rejected += rj[i].count; }
        for (i = 0; i < st->v.cols; i++) { if (st->b[i].status == LT_WUE_REC) { tuned++; } }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "State: %s", st->running ? "collecting" : (st->replay ? "replay result" : "stopped"));
        lt_ui_label_ex(ui, b, st->running ? th->success : th->text2, LT_UI_ALIGN_LEFT);
        lt_ui_labelf(ui, "Samples: %lu used, %lu filtered", lt_wue_accepted(st->s), rejected);
        lt_ui_labelf(ui, "Bins with recommendations: %d of %d", tuned, (int)st->v.cols);
        if (wn >= st->c.min_samples) {
            lt_snprintf(b, sizeof(b), "Warm engine runs %.1f%% %s: a VE table matter, divided out of the cold bins.",
                        fabs(wf - 1.0) * 100.0, wf >= 1.0 ? "lean" : "rich");
        } else {
            lt_snprintf(b, sizeof(b), "Warm-engine reference: %lu of %u samples.", wn, st->c.min_samples);
        }
        lt_ui_label_wrap(ui, b, th->text2);
        for (i = 0; i < nrj && i < 4; i++) { lt_ui_labelf(ui, "  %s: %lu", rj[i].reason, rj[i].count); }
        if (st->have_live) {
            lt_snprintf(b, sizeof(b), "Coolant now: %.1f", lt_uconv_to_disp(&st->v.xu, st->live_clt));
            lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
        }
        if (st->info[0] != '\0') { lt_ui_label_wrap(ui, st->info, th->text2); }
        if (app->conn == LT_APP_DEMO && lt_ui_button_ex(ui, "Simulate a cold start (demo)", LT_UI_BTN_FLAT)) { wue_demo(app, st); }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 0) && lt_ui_group_begin(ui, "Inputs", 0)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(b, sizeof(b), "Coolant %s, %s %s, EGO %s", st->ch.at.clt[0] ? st->ch.at.clt : "-",
                    st->ch.at.afr_is_lambda ? "lambda" : "AFR", st->ch.at.afr[0] ? st->ch.at.afr : "-",
                    st->ch.ego[0] ? st->ch.ego : "none");
        lt_ui_label_wrap(ui, b, th->text2);
        lt_snprintf(b, sizeof(b), "Target: %s%s", st->have_target ? st->target_name : "flat target",
                    st->have_offset ? " + AFR offset vs coolant" : "");
        lt_ui_label_wrap(ui, b, th->text2);
        w2[0] = 150; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Update mode");
        {
            static const char *const modes[2] = { "Send to ECU RAM", "Send and burn" };
            sel = st->burn_after;
            if (lt_ui_combo(ui, "wuemode", &sel, modes, 2)) { st->burn_after = sel; }
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 1) && lt_ui_group_begin(ui, "Filters", 0)) {
        int ex = st->c.f.exclude_accel_enrich ? 1 : 0;
        nrow(ui, "Min RPM", "wminr", &st->c.f.min_rpm, 0.0, 20000.0, 100.0, 0);
        nrow(ui, "Max RPM", "wmaxr", &st->c.f.max_rpm, 0.0, 20000.0, 100.0, 0);
        nrow(ui, "Cranking below RPM", "wcrank", &st->c.cranking_rpm, 0.0, 2000.0, 50.0, 0);
        nrow(ui, "After start (s)", "wase", &st->c.after_start_s, 0.0, 600.0, 5.0, 0);
        nrow(ui, "Max throttle %", "wtps", &st->c.max_tps, 0.0, 100.0, 1.0, 0);
        nrow(ui, "Max TPS rate %/s", "wtpsr", &st->c.f.max_tps_rate, 0.0, 1000.0, 5.0, 0);
        nrow(ui, "Max coolant rate /s", "wcltmax", &st->c.max_clt_rate, 0.0, 50.0, 0.1, 1);
        nrow(ui, "Min coolant rate /s", "wcltmin", &st->c.min_clt_rate, 0.0, 5.0, 0.01, 2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, "Exclude accel enrichment", &ex)) { st->c.f.exclude_accel_enrich = ex ? LT_TRUE : LT_FALSE; }
        w2[0] = 150; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Custom filter");
        if (lt_ui_text(ui, "wcf", st->custom, sizeof(st->custom), 0) & LT_UI_SUBMIT) {
            lt_at_filters_set_custom(&st->c.f, st->custom[0] != '\0' ? st->custom : NULL);
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 1) && app->ini->wue_analyze.n_filters > 0 && lt_ui_group_begin(ui, "INI filters ([WueAnalyze])", 0)) {
        const lt_ini_analyze *a = &app->ini->wue_analyze;
        for (i = 0; i < a->n_filters && i < WUE_MAX_FLT; i++) {
            const lt_ini_filter *f = &a->filters[i];
            int on = st->flt_on[i];
            if (f->channel == NULL || f->channel[0] == '\0') { continue; }
            lt_snprintf(b, sizeof(b), "%s (%s)", f->display != NULL ? f->display : f->name, f->channel);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_push_id_int(ui, (int)i);
            if (lt_ui_checkbox(ui, b, &on)) { st->flt_on[i] = (unsigned char)(on ? 1 : 0); }
            lt_ui_pop_id(ui);
        }
        lt_ui_group_end(ui);
    }
    if ((cell || st->ptab == 2) && lt_ui_group_begin(ui, "Convergence and authority", 0)) {
        double ms = (double)st->c.min_samples;
        nrow(ui, "Min samples per bin", "wmins", &ms, 1.0, 10000.0, 5.0, 0);
        st->c.min_samples = (unsigned)(ms + 0.5);
        nrow(ui, "Base weight", "wbw", &st->c.base_weight, 0.0, 10000.0, 5.0, 0);
        nrow(ui, "Max change per pass %", "wmcp", &st->c.auth.max_cell_percentage_change, 0.0, 100.0, 1.0, 1);
        nrow(ui, "Min change", "wminc", &st->c.min_change, 0.0, 100.0, 0.01, st->zdig);
        nrow(ui, "Min value", "wminv", &st->c.auth.min_cell_value, -1000.0, 10000.0, 1.0, st->zdig);
        nrow(ui, "Max value", "wmaxv", &st->c.auth.max_cell_value, -1000.0, 10000.0, 1.0, st->zdig);
        nrow(ui, "Delay ms (0=auto)", "wdl", &st->c.lambda_delay_ms, 0.0, 2000.0, 10.0, 0);
        nrow(ui, "Fallback target AFR", "wtafr", &st->c.target_afr, 8.0, 25.0, 0.1, 1);
        w2[0] = 150; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Warm end");
        sel = st->warm_sel;
        if (lt_ui_combo(ui, "wwarm", &sel, st->warm_opts, (int)st->v.cols + 1)) {
            st->warm_sel = sel;
            st->c.warm_clt = sel > 0 ? st->xecu[sel - 1] : LT_WUE_WARM_AUTO;
        }
        crow(ui, "Divide out the warm-engine error", &st->c.relative_to_warm);
        crow(ui, "Colder bins never get less enrichment", &st->c.monotonic);
        crow(ui, "Never below no enrichment", &st->c.never_below_unity);
        crow(ui, "Share samples between neighbor bins", &st->c.split_hits);
        lt_ui_group_end(ui);
    }
    lt_ui_scroll_end(ui);
}

enum { WC_REPLAY, WC_RESET, WC_START, WC_SEND, WC_UNDO, WC_BURN, WC_LOCK, WC_UNLOCK };

static void wue_cmd(lt_app *app, wue_tab *st, int cmd)
{
    switch (cmd) {
    case WC_REPLAY: lt_app_file_dialog(app, WUE_FD_REPLAY, LT_UI_FILE_OPEN, NULL, "", "Logs|*.mlg;*.csv;*.msl|All files|*"); break;
    case WC_RESET:
        lt_wue_start(st->s);
        if (!st->running) { lt_wue_stop(st->s); }
        st->replay = 0;
        st->info[0] = '\0';
        break;
    case WC_START: if (st->running) { wue_stop(st); } else { wue_start(app, st); } break;
    case WC_SEND: (void)wue_send(app, st); break;
    case WC_UNDO: wue_undo(app, st); break;
    case WC_BURN: (void)lt_app_burn(app); break;
    case WC_LOCK: wue_lock_sel(st, 0); break;
    case WC_UNLOCK: wue_lock_sel(st, 1); break;
    default: break;
    }
}

static int wue_header(lt_app *app, wue_tab *st, int *csel)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, n = 0, ids[12], hit;
    lt_vk_item it[12];
    lt_rect hr;
    int nrec = 0;
    size_t i;
    for (i = 0; i < st->v.cols; i++) {
        if ((st->b[i].status == LT_WUE_REC || st->b[i].status == LT_WUE_WARM) && fabs(st->b[i].recommended - st->b[i].current) > 1e-9) { nrec++; }
    }
    lt_ui_layout_row(ui, 1, &w1, 54);
    hr = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, hr, th->elevated);
    lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, th->border_subtle);
    lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 14), hr.y, lt_ui_px(ui, 130), hr.h), (int)LT_FONT_SANS_SEMIBOLD, 16.0, "WUE Analyze", th->text, 0);
    if (st->running) {
        lt_rect bdg = lt_rect_make(hr.x + lt_ui_px(ui, 128), hr.y + lt_ui_px(ui, 18), lt_ui_px(ui, 62), lt_ui_px(ui, 18));
        lt_vk_pill(ui, bdg, LT_RGBA(0x4C, 0xAF, 0x50, 60), th->success);
        lt_vk_text(ui, bdg, (int)LT_FONT_SANS_SEMIBOLD, 10.0, "RUNNING", th->success, 1);
    }
    lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 200), hr.y + lt_ui_px(ui, 11), lt_ui_px(ui, 52), lt_ui_px(ui, 32)), (int)LT_FONT_SANS, 12.0, "Curve", th->muted, 0);
    *csel = st->csel;
    lt_ui_layout_set_rect(ui, lt_rect_make(hr.x + lt_ui_px(ui, 244), hr.y + lt_ui_px(ui, 11), lt_ui_px(ui, 230), lt_ui_px(ui, 32)));
    if (st->ncurves > 0) { (void)lt_ui_combo(ui, "wcsel", csel, st->names, (int)st->ncurves); }
    memset(it, 0, sizeof(it));
    it[n].kind = LT_VK_SPACER; ids[n++] = -1;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_FOLDER_OPEN; it[n].label = "Replay log"; it[n].tip = "Analyze a data log of a cold start"; ids[n++] = WC_REPLAY;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_ROTATE_CCW; it[n].label = "Reset"; it[n].tip = "Discard collected data"; ids[n++] = WC_RESET;
    it[n].kind = LT_VK_BTN; it[n].icon = st->running ? LT_ICON_CIRCLE_STOP : LT_ICON_PLAY; it[n].label = st->running ? "Stop" : "Start";
    it[n].tip = "Start/stop collecting during a warm-up"; it[n].fill = st->running ? LT_RGB(0xD3, 0x2F, 0x2F) : LT_RGB(0x2E, 0xA0, 0x43); ids[n++] = WC_START;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_UPLOAD; it[n].label = "Send to ECU"; it[n].tip = "Write the recommended bins (except locked ones)";
    it[n].disabled = nrec == 0; ids[n++] = WC_SEND;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_UNDO_2; it[n].label = "Undo"; it[n].tip = "Restore the curve as it was before the last send";
    it[n].disabled = !st->undo_ok; ids[n++] = WC_UNDO;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_FLAME; it[n].label = "Burn"; it[n].accent = 1; it[n].disabled = !lt_app_can_burn(app); ids[n++] = WC_BURN;
    it[n].kind = LT_VK_END;
    hit = lt_vk_toolbar_in(ui, it, lt_rect_make(hr.x + lt_ui_px(ui, 486), hr.y + lt_ui_px(ui, 8), hr.w - lt_ui_px(ui, 498), lt_ui_px(ui, 38)));
    return hit >= 0 ? ids[hit] : -1;
}

static int wue_strip(lt_app *app, wue_tab *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lt_vk_item it[5];
    int n = 0, ids[5], hit, w1 = -1;
    lt_rect r;
    lt_ui_layout_row(ui, 1, &w1, 40);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, r, th->bg_toolbar);
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 14), r.y, lt_ui_px(ui, 420), r.h), (int)LT_FONT_SANS_MEDIUM, 12.0,
               st->replay ? "Replay recommendations per coolant bin" : "Recommendations per coolant bin", th->text2, 0);
    memset(it, 0, sizeof(it));
    it[n].kind = LT_VK_SPACER; ids[n++] = -1;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_LOCK; it[n].label = "Lock"; it[n].tip = "Toggle lock on the selected bins (L)"; ids[n++] = WC_LOCK;
    it[n].kind = LT_VK_BTN; it[n].icon = LT_ICON_LOCK_OPEN; it[n].label = "Unlock"; it[n].tip = "Unlock every bin"; ids[n++] = WC_UNLOCK;
    it[n].kind = LT_VK_END;
    hit = lt_vk_toolbar_in(ui, it, lt_rect_make(r.x + lt_ui_px(ui, 420), r.y + lt_ui_px(ui, 5), r.w - lt_ui_px(ui, 432), lt_ui_px(ui, 30)));
    return hit >= 0 ? ids[hit] : -1;
}

static void wue_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    wue_tab *st = (wue_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, wm[2], csel, cell = lt_ui_cell_mode(ui), cmd = -1;
    const char *picked;
    LT_UNUSED(r);
    if (st == NULL) {
        st = (wue_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        g_wue = st;
        st->s = lt_wue_new();
        lt_tblview_init(&st->v);
        wue_curves(app, st);
        wue_defaults(app, st);
        wue_load(app, st);
    }
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "WUE Analyze");
        lt_ui_label_wrap(ui, st->msg, th->error);
        return;
    }
    /* follow edits made elsewhere while idle */
    if (!st->running && !st->replay && lt_wue_accepted(st->s) == 0 && !lt_tblview_is_dirty(&st->v) && ++st->frame % 20 == 0) {
        lt_pagesrc src = lt_app_pages(app);
        lt_tblview t;
        lt_tblview_init(&t);
        if (lt_tblview_load_curve(&t, app->ini, st->curve, &src) == 0 && t.cols >= st->v.cols) {
            memcpy(st->v.z, t.z, st->v.cols * sizeof(double));
            lt_wue_update_values(st->s, st->v.z, st->v.cols);
        }
        lt_tblview_free(&t);
    }
    picked = lt_app_file_result(app, WUE_FD_REPLAY);
    if (picked != NULL) { wue_replay(app, st, picked); }
    if (lt_ui_key_pressed(ui, 'l', 0)) { wue_lock_sel(st, 0); }
    if (g_wue_demo) {
        g_wue_demo = 0;
        if (app->conn == LT_APP_DEMO) { wue_demo(app, st); }
    }
    st->nb = lt_wue_bins(st->s, &st->c, st->b, LT_WUE_MAX_BINS);
    if (cell) {
        int wt[9], i;
        for (i = 0; i < 9; i++) { wt[i] = 0; }
        wt[0] = 72; wt[1] = 220;
        lt_ui_layout_row(ui, 9, wt, 0);
        lt_ui_label(ui, "WUE curve");
        csel = st->csel;
        if (st->ncurves > 0) { (void)lt_ui_combo(ui, "wcsel", &csel, st->names, (int)st->ncurves); }
        if (st->running) { if (lt_ui_button_ex(ui, "Stop", LT_UI_BTN_DANGER)) { cmd = WC_START; } }
        else if (lt_ui_button_ex(ui, "Start", LT_UI_BTN_PRIMARY)) { cmd = WC_START; }
        if (lt_ui_button_ex(ui, "Send", LT_UI_BTN_ACCENT)) { cmd = WC_SEND; }
        if (lt_ui_button_ex(ui, "Undo", LT_UI_BTN_FLAT | (st->undo_ok ? 0 : LT_UI_BTN_DISABLED))) { cmd = WC_UNDO; }
        if (lt_ui_button_ex(ui, "Reset", LT_UI_BTN_FLAT)) { cmd = WC_RESET; }
        if (lt_ui_button_ex(ui, "Replay log...", LT_UI_BTN_FLAT)) { cmd = WC_REPLAY; }
        if (lt_ui_button_ex(ui, "Lock", LT_UI_BTN_FLAT)) { cmd = WC_LOCK; }
        if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT | (lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED))) { cmd = WC_BURN; }
    } else {
        cmd = wue_header(app, st, &csel);
        {
            int c2 = wue_strip(app, st);
            if (cmd < 0) { cmd = c2; }
        }
    }
    if (cmd >= 0) { wue_cmd(app, st, cmd); }
    if (csel != st->csel && st->ncurves > 0) {
        wue_stop(st);
        st->csel = csel;
        wue_load(app, st);
        return;
    }
    if (!wue_axis_ok(st)) {
        int wb[2];
        wb[0] = -1; wb[1] = 170;
        lt_ui_layout_row(ui, 2, wb, 0);
        lt_ui_label_ex(ui, "The coolant axis of this curve is not ascending: WUE Analyze needs real coolant bins.",
                       th->warning, LT_UI_ALIGN_LEFT);
        if (lt_ui_button_ex(ui, "Fill coolant axis", LT_UI_BTN_FLAT)) { wue_fill_axis(app, st); return; }
    }
    wm[0] = -1; wm[1] = 360;
    lt_ui_layout_row(ui, 2, wm, -1);
    lt_ui_layout_column_begin(ui);
    if (!cell) {
        lt_rect cr;
        int ch = lt_ui_unpx(ui, lt_ui_layout_body(ui).h) - (WUE_ROWS + 1) * 24 - 28;
        if (ch < 160) { ch = 160; }
        lt_ui_layout_row(ui, 1, &w1, ch);
        cr = lt_ui_layout_next(ui);
        wue_chart(app, st, cr);
        lt_ui_layout_row(ui, 1, &w1, (WUE_ROWS + 1) * 24 + 16);
    } else {
        lt_ui_layout_row(ui, 1, &w1, -1);
    }
    wue_grid(app, st);
    lt_ui_layout_column_end(ui);
    wue_panel(app, st);
    tab->dirty = st->running ? 1 : 0;
}

static void wue_close(lt_app *app, lt_app_tab *tab)
{
    wue_tab *st = (wue_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    if (g_wue == st) { g_wue = NULL; }
    lt_tblview_free(&st->v);
    lt_wue_config_clear(&st->c);
    lt_wue_free(st->s);
    lt_free(st);
    tab->data = NULL;
}

static int open_wue(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    if (app->ini == NULL) { return -1; }
    return lt_app_tab_open(app, "wueanalyze", "wueanalyze", "WUE Analyze", 1) != NULL ? 0 : -1;
}

static void act_wue(lt_app *app) { (void)open_wue(app, NULL); }

/* the tab with a simulated cold start already analysed (demo mode; screenshots) */
static int open_wue_demo(lt_app *app, const char *t)
{
    g_wue_demo = 1;
    return open_wue(app, t);
}

void lt_feat_wue_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    tk.kind = "wueanalyze"; tk.draw = wue_draw; tk.close = wue_close;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_target(reg, "wue-analyze", open_wue);
    lt_ext_add_target(reg, "wue-analyze-demo", open_wue_demo);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&WUE Analyze", NULL, act_wue, wue_avail);
    lt_ext_add_tick(reg, wue_tick);
}
