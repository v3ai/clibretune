/* dlg_afrdelay.c - AFR transport delay test (Tools > AFR Delay Test...).
 *
 * Steps fuelling up by a few percent through the last slot of the warmup curve (a single byte,
 * RAM only, never burned), holds it, restores it and times how long the wideband takes to see
 * it.  Enrichment only and bounded; the original value is written back after every step, on
 * abort, when the dialog closes and when the link drops.  The per-flow delay map and the flow
 * model fit come from the autotune library; "Save model" writes afr_delay_model.json for
 * AutoTune.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "tables_priv.h"
#include "../json/json.h"
#include "../autotune/at_extra.h"

#define AFRD_BUF 1024
#define AFRD_TRACES 6
#define AFRD_TPTS 128
#define AFRD_PRE_MS 2000ul
#define AFRD_LEVER "wueRates"

enum { PH_IDLE = 0, PH_BASE, PH_HOLD, PH_SETTLE, PH_DONE, PH_ABORTED };

typedef struct afrd_s {
    double step_pct, hold_ms, settle_ms, repeats;
    int phase, step, nsteps, enriched;
    double base_val;                       /* restore value of the lever slot */
    unsigned long t_open, phase_t0, anchor;
    unsigned long last_frames;
    lt_at_channels ch;
    struct { unsigned long t; double afr, rpm, load; int bad; } *buf;
    size_t nbuf;
    int unusable, rejected;
    lt_atx_delays dl;
    int tn[AFRD_TRACES], ntr;              /* overlay traces, newest last */
    double tt[AFRD_TRACES][AFRD_TPTS], ta[AFRD_TRACES][AFRD_TPTS], tdelay[AFRD_TRACES];
    char msg[200], model_msg[160];
} afrd_s;

static unsigned long now_rel(const afrd_s *s) { return (unsigned long)lt_plat_now_ms() - s->t_open; }

/* ---- the lever: last slot of the warmup curve ---- */
/* warmup curve of the definition: the speeduino/MS style wueRates, else the rusEFI warmup multiplier,
 * else the Y constant of the WueAnalyze curve */
static const lt_ini_const *lever_const(const lt_ini *ini)
{
    static const char *const names[2] = { AFRD_LEVER, "cltFuelCorr" };
    const lt_ini_const *c = NULL;
    size_t i;
    for (i = 0; i < 2 && c == NULL; i++) { c = lt_ini_find_const(ini, names[i]); }
    if (c == NULL && ini->wue_analyze.wue_curve != NULL) {
        const lt_ini_curve *cv = lt_ini_find_curve(ini, ini->wue_analyze.wue_curve);
        if (cv != NULL && cv->y_const != NULL) { c = lt_ini_find_const(ini, cv->y_const); }
    }
    return c;
}

static int lever_rw(lt_app *app, double *val, int write)
{
    const lt_ini_const *c = lever_const(app->ini);
    double v[32];
    int n;
    lt_pagesrc src = lt_app_pages(app);
    if (c == NULL) { return -1; }
    n = lt_ini_const_get_array(app->ini, c, &src, v, 32);
    if (n < 2) { return -1; }
    if (!write) { *val = v[n - 1]; return 0; }
    v[n - 1] = *val;
    if (lt_ini_const_set_array(app->ini, c, &src, v, (size_t)n) < 0) { return -1; }
    lt_app_edited(app);
    return 0;
}

static void restore(lt_app *app, afrd_s *s)
{
    double v = s->base_val;
    int tries;
    if (!s->enriched) { return; }
    for (tries = 0; tries < 3; tries++) {
        if (lever_rw(app, &v, 1) == 0) { s->enriched = 0; return; }
    }
    lt_app_toast(app, 3, "Could not restore the warmup slot to %.0f%%; cycle the ECU power to clear it", v);
}

static void finish(lt_app *app, afrd_s *s, int phase, const char *msg)
{
    restore(app, s);
    s->phase = phase;
    lt_strlcpy(s->msg, msg, sizeof(s->msg));
}

static void add_trace(afrd_s *s, const lt_afr_sample *pre, size_t npre, const lt_afr_sample *post, size_t npost,
                      unsigned long anchor, double delay)
{
    int k, i;
    size_t j, np = 0, total = npre + npost;
    size_t stride = total > AFRD_TPTS ? (total + AFRD_TPTS - 1) / AFRD_TPTS : 1;
    if (s->ntr == AFRD_TRACES) {
        for (k = 1; k < AFRD_TRACES; k++) {
            memcpy(s->tt[k - 1], s->tt[k], sizeof(s->tt[0]));
            memcpy(s->ta[k - 1], s->ta[k], sizeof(s->ta[0]));
            s->tn[k - 1] = s->tn[k]; s->tdelay[k - 1] = s->tdelay[k];
        }
        s->ntr--;
    }
    k = s->ntr++;
    i = 0;
    for (j = 0; j < total && i < AFRD_TPTS; j += stride) {
        const lt_afr_sample *p = j < npre ? &pre[j] : &post[j - npre];
        s->tt[k][i] = (double)p->t_ms - (double)anchor;
        s->ta[k][i] = p->afr;
        i++; np++;
    }
    s->tn[k] = (int)np;
    s->tdelay[k] = delay;
}

static void measure(lt_app *app, afrd_s *s)
{
    lt_afr_sample *pre = (lt_afr_sample *)lt_calloc(AFRD_BUF + 1, sizeof(lt_afr_sample));
    lt_afr_sample *post = (lt_afr_sample *)lt_calloc(AFRD_BUF + 1, sizeof(lt_afr_sample));
    size_t i, npre = 0, npost = 0;
    int bad = 0;
    double rpm = 0.0, load = 0.0;
    size_t nr = 0;
    LT_UNUSED(app);
    for (i = 0; i < s->nbuf; i++) {
        if (s->buf[i].t + AFRD_PRE_MS < s->anchor) { continue; }
        if (s->buf[i].bad) { bad = 1; }
        if (s->buf[i].t < s->anchor) {
            pre[npre].t_ms = s->buf[i].t; pre[npre].afr = s->buf[i].afr; npre++;
        } else {
            post[npost].t_ms = s->buf[i].t; post[npost].afr = s->buf[i].afr; npost++;
        }
        rpm += s->buf[i].rpm; load += s->buf[i].load; nr++;
    }
    if (nr > 0) { rpm /= (double)nr; load /= (double)nr; }
    if (bad) {
        s->unusable++;
        add_trace(s, pre, npre, post, npost, s->anchor, -1.0);
        lt_snprintf(s->msg, sizeof(s->msg), "Step %d unusable: throttle moving or fuel cut, trace excluded", s->step + 1);
    } else {
        lt_delay_measurement m;
        lt_delay_rejection why = LT_DR_NO_RESPONSE;
        memset(&m, 0, sizeof(m));
        if (lt_detect_delay(s->anchor, pre, npre, post, npost, &m, &why) == 0) {
            lt_atx_delays_add(&s->dl, rpm, load, m.delay_ms);
            add_trace(s, pre, npre, post, npost, s->anchor, m.delay_ms);
            lt_snprintf(s->msg, sizeof(s->msg), "Step %d: delay %.0f ms at %.0f rpm / %.0f", s->step + 1, m.delay_ms, rpm, load);
        } else {
            s->rejected++;
            add_trace(s, pre, npre, post, npost, s->anchor, -1.0);
            lt_snprintf(s->msg, sizeof(s->msg), "Step %d rejected: %s", s->step + 1, lt_delay_rejection_text(why));
        }
    }
    lt_free(pre);
    lt_free(post);
}

static void push_sample(afrd_s *s, const lt_ve_point *p)
{
    if (s->nbuf >= AFRD_BUF) {
        memmove(s->buf, s->buf + AFRD_BUF / 2, (AFRD_BUF / 2) * sizeof(s->buf[0]));
        s->nbuf = AFRD_BUF / 2;
    }
    s->buf[s->nbuf].t = now_rel(s);
    s->buf[s->nbuf].afr = p->afr;
    s->buf[s->nbuf].rpm = p->rpm;
    s->buf[s->nbuf].load = p->load;
    s->buf[s->nbuf].bad = (p->fuel_cut_active == 1 || fabs(p->tps_rate) > LT_ATX_TPS_DOT_LIMIT) ? 1 : 0;
    s->nbuf++;
}

static void start(lt_app *app, afrd_s *s)
{
    double v = 0.0;
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_snprintf(s->msg, sizeof(s->msg), "Connect to the ECU first (or use demo mode).");
        return;
    }
    if (lever_rw(app, &v, 0) < 0) {
        lt_snprintf(s->msg, sizeof(s->msg), "This definition has no warmup curve to step.");
        return;
    }
    s->step_pct = lt_atx_clamp_step(s->step_pct);
    s->hold_ms = (double)lt_atx_clamp_hold(s->hold_ms);
    s->settle_ms = s->settle_ms < 1500.0 ? 1500.0 : (s->settle_ms > 15000.0 ? 15000.0 : s->settle_ms);
    s->nsteps = lt_atx_clamp_repeats(s->repeats);
    s->repeats = (double)s->nsteps;
    s->base_val = v;
    s->step = 0; s->unusable = 0; s->rejected = 0; s->ntr = 0;
    lt_atx_delays_init(&s->dl);
    s->nbuf = 0;
    s->last_frames = app->frames_ok;
    s->phase = PH_BASE;
    s->phase_t0 = now_rel(s);
    lt_snprintf(s->msg, sizeof(s->msg), "Hold the engine at a steady operating point. Baseline slot value %.0f%%.", v);
}

/* advance the run; called every frame while the dialog is open */
static void tick(lt_app *app, afrd_s *s)
{
    lt_ve_point pt;
    unsigned long t = now_rel(s);
    int fresh = 0;
    if (s->phase == PH_IDLE || s->phase >= PH_DONE) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { finish(app, s, PH_ABORTED, "Link lost: slot restored, test aborted."); return; }
    if (app->frames_ok != s->last_frames && app->rt != NULL) {
        s->last_frames = app->frames_ok;
        if (lt_at_sample_from_rt(app->rt, &s->ch, t, &pt)) { push_sample(s, &pt); fresh = 1; }
    }
    if (!fresh && s->nbuf == 0) { return; }
    if (s->phase == PH_BASE) {
        if (s->nbuf > 0 && s->buf[s->nbuf - 1].rpm < 400.0) {
            s->phase_t0 = t;
            lt_snprintf(s->msg, sizeof(s->msg), "Waiting for the engine to run (rpm > 400)...");
        } else if (t - s->phase_t0 >= AFRD_PRE_MS) {
            double v = lt_atx_step_value(s->base_val, s->step_pct);
            if (lever_rw(app, &v, 1) < 0) { finish(app, s, PH_ABORTED, "Could not write the warmup slot; test aborted."); return; }
            s->enriched = 1;
            s->anchor = t;
            s->phase = PH_HOLD;
            lt_snprintf(s->msg, sizeof(s->msg), "Step %d of %d: enriched to %.0f%%", s->step + 1, s->nsteps, v);
        }
    } else if (s->phase == PH_HOLD) {
        if (t - s->anchor >= (unsigned long)s->hold_ms) { restore(app, s); s->phase = PH_SETTLE; }
    } else if (s->phase == PH_SETTLE) {
        if (t - s->anchor >= (unsigned long)(s->hold_ms + s->settle_ms)) {
            measure(app, s);
            s->step++;
            if (s->step >= s->nsteps) { s->phase = PH_DONE; }
            else { s->phase = PH_BASE; s->phase_t0 = t; }
        }
    }
}

/* ---- model file ---- */
static char *model_path(lt_app *app)
{
    return lt_path_join(app->has_project ? app->project.path : app->config_dir, "afr_delay_model.json");
}

static void save_model(lt_app *app, afrd_s *s)
{
    lt_flow_fit fit;
    lt_buf b;
    char *p;
    if (lt_atx_delay_model(&s->dl, &fit) < 0) {
        lt_snprintf(s->model_msg, sizeof(s->model_msg), "Not enough measurements for a model (need 3 or more).");
        return;
    }
    lt_buf_init(&b);
    lt_atx_delay_model_json(&b, &fit);
    (void)lt_plat_mkdirs(app->has_project ? app->project.path : app->config_dir);
    p = model_path(app);
    if (lt_file_write(p, b.data, b.len) < 0) {
        lt_snprintf(s->model_msg, sizeof(s->model_msg), "Cannot save: %s", lt_err_get());
    } else {
        lt_snprintf(s->model_msg, sizeof(s->model_msg), "Saved %s (floor %.0f ms, anchor %.0f ms). AutoTune uses it next time.",
                    lt_path_base(p), fit.floor_ms, fit.anchor_ms);
        lt_app_toast(app, 1, "Delay model saved");
    }
    lt_free(p);
    lt_buf_free(&b);
}

int lt_tabs_delay_model_load(lt_app *app, double *floor_ms, double *anchor_ms)
{
    char *p = model_path(app);
    lt_json *j;
    char err[96];
    double fl, an;
    err[0] = '\0';
    j = lt_json_load(p, err, sizeof(err));
    lt_free(p);
    if (j == NULL) { return -1; }
    fl = lt_json_get_num(j, "floorMs", -1.0);
    an = lt_json_get_num(j, "anchorMs", -1.0);
    lt_json_free(j);
    if (fl < 0.0 || an <= 0.0) { return -1; }
    *floor_ms = fl; *anchor_ms = an;
    return 0;
}

/* ---- dialog ---- */
static void plot(lt_app *app, afrd_s *s, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int k, i;
    double tmin = -(double)AFRD_PRE_MS, tmax = s->hold_ms + s->settle_ms, amin = 1e9, amax = -1e9, pad;
    int px = lt_ui_px(ui, 34), py = lt_ui_px(ui, 6);
    lt_rect pr = lt_rect_make(r.x + px, r.y + py, r.w - px - lt_ui_px(ui, 8), r.h - 2 * py - lt_ui_px(ui, 14));
    char b[32];
    lt_ui_draw_fill(ui, r, th->table_bg);
    lt_ui_draw_rect(ui, r, th->border_subtle);
    for (k = 0; k < s->ntr; k++) {
        for (i = 0; i < s->tn[k]; i++) {
            if (s->ta[k][i] < amin) { amin = s->ta[k][i]; }
            if (s->ta[k][i] > amax) { amax = s->ta[k][i]; }
        }
    }
    if (amax <= amin) { amin = 12.0; amax = 16.0; }
    pad = (amax - amin) * 0.1 + 0.05; amin -= pad; amax += pad;
    {
        double ax = (double)pr.x - (double)pr.w * tmin / (tmax - tmin);
        lt_ui_draw_vline(ui, (int)ax, pr.y, pr.y + pr.h, th->border);
        lt_ui_draw_text(ui, (int)ax + 3, pr.y, "step", th->muted);
        lt_snprintf(b, sizeof(b), "%.1f", amax);
        lt_ui_draw_text(ui, r.x + 3, pr.y - 2, b, th->muted);
        lt_snprintf(b, sizeof(b), "%.1f", amin);
        lt_ui_draw_text(ui, r.x + 3, pr.y + pr.h - lt_ui_px(ui, 14), b, th->muted);
        for (k = 0; k < s->ntr; k++) {
            int newest = k == s->ntr - 1;
            lt_color c = newest ? th->accent : lt_color_mix(th->table_bg, th->text2, 90);
            double lx = 0.0, ly = 0.0;
            for (i = 0; i < s->tn[k]; i++) {
                double x = (double)pr.x + (double)pr.w * (s->tt[k][i] - tmin) / (tmax - tmin);
                double y = (double)(pr.y + pr.h) - (double)pr.h * (s->ta[k][i] - amin) / (amax - amin);
                if (i > 0) { lt_ui_draw_line(ui, lx, ly, x, y, newest ? 2.0 : 1.0, c); }
                lx = x; ly = y;
            }
            if (newest && s->tdelay[k] > 0.0) {
                double mx = (double)pr.x + (double)pr.w * (s->tdelay[k] - tmin) / (tmax - tmin);
                lt_ui_draw_vline(ui, (int)mx, pr.y, pr.y + pr.h, th->success);
                lt_snprintf(b, sizeof(b), "%.0f ms", s->tdelay[k]);
                lt_ui_draw_text(ui, (int)mx + 3, pr.y + pr.h - lt_ui_px(ui, 28), b, th->success);
            }
        }
    }
    lt_ui_draw_text(ui, r.x + r.w - lt_ui_px(ui, 150), r.y + r.h - lt_ui_px(ui, 16), "AFR trace, ms from the step", th->muted);
}

static void delay_map(lt_app *app, afrd_s *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    static const char *const rl[6] = { "<1200", "-2000", "-3000", "-4500", "-6500", "6500+" };
    static const char *const ll[5] = { "<40", "-60", "-80", "-100", "100+" };
    int w[7], r, c;
    char b[40];
    w[0] = 56;
    for (c = 1; c < 7; c++) { w[c] = -1; }
    lt_ui_layout_row(ui, 7, w, 0);
    lt_ui_label(ui, "load\\rpm");
    for (c = 0; c < 6; c++) { lt_ui_label_ex(ui, rl[c], th->muted, LT_UI_ALIGN_LEFT); }
    for (r = 0; r < 5; r++) {
        lt_ui_layout_row(ui, 7, w, 0);
        lt_ui_label_ex(ui, ll[r], th->muted, LT_UI_ALIGN_LEFT);
        for (c = 0; c < 6; c++) {
            const lt_delay_cell *dc = &s->dl.table.cells[r][c];
            if (dc->n == 0) { lt_ui_label_ex(ui, "-", th->disabled, LT_UI_ALIGN_LEFT); }
            else {
                lt_snprintf(b, sizeof(b), "%.0f (%u)", dc->mean_ms, dc->n);
                lt_ui_label_ex(ui, b, th->text, LT_UI_ALIGN_LEFT);
            }
        }
    }
}

static afrd_s *state(lt_app_dialog *d)
{
    return (afrd_s *)d->data;
}

static void afrd_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    afrd_s *s = state(d);
    int w1 = -1, w4[4], wb[4], running, open_ok;
    char b[200];
    lt_rect pr;
    if (s == NULL) {
        s = (afrd_s *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->buf = lt_calloc(AFRD_BUF, sizeof(s->buf[0]));
        s->step_pct = 8.0; s->hold_ms = 1500.0; s->settle_ms = 4000.0; s->repeats = 10.0;
        s->t_open = (unsigned long)lt_plat_now_ms();
        lt_at_channels_from_ini(app->ini, &s->ch);
        lt_atx_delays_init(&s->dl);
        lt_snprintf(s->msg, sizeof(s->msg), "Idle. Hold the engine at a steady operating point, then start.");
    }
    tick(app, s);
    running = s->phase >= PH_BASE && s->phase < PH_DONE;
    open_ok = d->open;
    if (!lt_ui_modal_begin(ui, "AFR delay test", 680, 640, &open_ok)) { d->open = open_ok; if (!open_ok) { restore(app, s); } return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "Steps the fuelling up (enrichment only, 3-20%) through the last warmup-curve slot in ECU RAM, "
                         "holds it, restores it and times how long the wideband takes to react. Never burned: cycling "
                         "the ECU power also restores the tune. Keep the engine steady with a closed throttle.",
                     th->text2);
    w4[0] = 120; w4[1] = 100; w4[2] = 120; w4[3] = -1;
    lt_ui_layout_row(ui, 4, w4, 0);
    lt_ui_label(ui, "Step %");
    lt_ui_label(ui, "Hold ms");
    lt_ui_label(ui, "Settle ms");
    lt_ui_label(ui, "Repeats");
    lt_ui_layout_row(ui, 4, w4, 0);
    lt_ui_disabled_begin(ui, running ? LT_TRUE : LT_FALSE);
    lt_ui_number(ui, "afd_step", &s->step_pct, LT_ATX_STEP_MIN_PCT, LT_ATX_STEP_MAX_PCT, 1.0, 0);
    lt_ui_number(ui, "afd_hold", &s->hold_ms, (double)LT_ATX_HOLD_MIN_MS, (double)LT_ATX_HOLD_MAX_MS, 100.0, 0);
    lt_ui_number(ui, "afd_settle", &s->settle_ms, 1500.0, 15000.0, 500.0, 0);
    lt_ui_number(ui, "afd_rep", &s->repeats, 1.0, (double)LT_ATX_MAX_REPEATS, 1.0, 0);
    lt_ui_disabled_end(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, s->msg, s->phase == PH_ABORTED ? th->error : th->text);
    if (s->nsteps > 0) {
        lt_ui_progress(ui, (double)s->step / (double)s->nsteps,
                       s->phase == PH_BASE ? "baseline" : (s->phase == PH_HOLD ? "enriched" : (s->phase == PH_SETTLE ? "settling" : "")));
    }
    lt_ui_layout_row(ui, 1, &w1, 130);
    pr = lt_ui_layout_next(ui);
    plot(app, s, pr);
    delay_map(app, s);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->dl.n > 0) {
        lt_snprintf(b, sizeof(b), "%d measured (%d rejected, %d unusable): median %.0f ms, range %.0f-%.0f ms",
                    (int)s->dl.n, s->rejected, s->unusable, lt_atx_delays_median(&s->dl), s->dl.mn, s->dl.mx);
        lt_ui_label_ex(ui, b, th->text, LT_UI_ALIGN_LEFT);
        {
            lt_flow_fit fit;
            if (lt_atx_delay_model(&s->dl, &fit) == 0) {
                lt_snprintf(b, sizeof(b), "Flow model: floor %.0f ms, anchor %.0f ms, rms %.0f ms over %d samples",
                            fit.floor_ms, fit.anchor_ms, fit.rms_ms, (int)fit.samples);
                lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
            }
        }
    }
    if (s->model_msg[0] != '\0') { lt_ui_label_wrap(ui, s->model_msg, th->text2); }
    wb[0] = -1; wb[1] = 120; wb[2] = 150; wb[3] = 90;
    lt_ui_layout_row(ui, 4, wb, 0);
    lt_ui_label(ui, "");
    if (!running) {
        if (lt_ui_button_ex(ui, "Start", LT_UI_BTN_PRIMARY)) { start(app, s); }
    } else if (lt_ui_button_ex(ui, "Abort", LT_UI_BTN_DANGER)) {
        finish(app, s, PH_ABORTED, "Aborted: warmup slot restored.");
    }
    if (lt_ui_button_ex(ui, "Save model", s->dl.n < 3 ? LT_UI_BTN_DISABLED : 0)) { save_model(app, s); }
    if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_FLAT)) { open_ok = 0; }
    lt_ui_modal_end(ui);
    if (!open_ok) { restore(app, s); }
    d->open = open_ok;
}

static void afrd_close(lt_app *app, lt_app_dialog *d)
{
    afrd_s *s = state(d);
    if (s == NULL) { return; }
    restore(app, s);
    lt_free(s->buf);
    lt_free(s);
    d->data = NULL;
}

static void act_afrdelay(lt_app *app) { (void)lt_app_dialog_push(app, "afrdelay", "", "AFR delay test"); }

void lt_feat_afrdelay_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "afrdelay"; dk.draw = afrd_draw; dk.close = afrd_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "AFR &Delay Test...", NULL, act_afrdelay, lt_ext_pred_ini);
}
