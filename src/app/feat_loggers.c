/* feat_loggers.c - tooth logger (G-70), composite logger (G-71) and the ECU console with the
 * controller command confirmation (G-07).  The loggers drive lt_proto_logger_* against the INI
 * [LoggerDefinition], decode records by field name (datalog/logcap.h) and plot them with zoom
 * and two cursors; captures are saved as CSV under <project>/datalogs.  Console history is
 * persisted in the config directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "feat.h"
#include "dialogs.h"
#include "../datalog/logcap.h"

#define CH 8
#define READ_INTERVAL_MS 250.0
#define MAX_EMPTY_READS 200

/* ------------------------------------------------------------------ shared helpers */
static int is_online(const lt_app *app)
{
    return (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && app->proto != NULL;
}

static int in_rect(lt_point m, lt_rect r)
{
    return m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ logger views */
typedef struct lg_view {
    int comp;                       /* 0 tooth view, 1 composite view */
    const lt_ini_logger *lg;
    lt_logcap cap;
    int cap_ready;
    int capturing, auto_done, frames;
    double next_read;
    double t_start;
    double secs, teeth, missing;    /* capture length, wheel teeth per revolution, missing teeth */
    double zoom, scroll;
    double cur_a, cur_b;            /* cursors: tooth index (tooth view) or time in us */
    int has_a, has_b;
    int prev_l, prev_r;
    double *teeth_us;                  /* derived tooth intervals */
    size_t nt;
    lt_comp_sample *cs;
    size_t ncs;
    unsigned long key_seen;        /* cache key */
    size_t key_n;
    char err[200];
    char saved[300];
} lg_view;

static const char *logger_missing_msg(int comp)
{
    return comp ? "This ECU definition declares no composite (trigger) logger."
                : "This INI declares no diagnostic loggers.";
}

static lg_view *view_get(lt_app_tab *tab, int comp)
{
    lg_view *v = (lg_view *)tab->data;
    if (v == NULL) {
        v = (lg_view *)lt_calloc(1, sizeof(*v));
        v->comp = comp;
        v->secs = 20.0;
        v->teeth = 36.0;
        v->missing = 1.0;
        v->zoom = 1.0;
        v->scroll = 1.0;
        tab->data = v;
    }
    return v;
}

static void view_free(lt_app *app, lt_app_tab *tab)
{
    lg_view *v = (lg_view *)tab->data;
    LT_UNUSED(app);
    if (v != NULL) {
        if (v->cap_ready) { lt_logcap_free(&v->cap); }
        lt_free(v->teeth_us);
        lt_free(v->cs);
        lt_free(v);
    }
    tab->data = NULL;
}

/* keep the logger pointer in step with the open INI */
static void view_sync_logger(lt_app *app, lg_view *v)
{
    const lt_ini_logger *lg = app->ini != NULL ? lt_logcap_pick(app->ini, v->comp ? "composite" : "tooth", !v->comp)
                                                : NULL;
    if (lg == v->lg && v->cap_ready) { return; }
    if (v->cap_ready) { lt_logcap_free(&v->cap); }
    v->lg = lg;
    lt_logcap_init(&v->cap, lg);
    v->cap_ready = 1;
    v->capturing = 0;
    v->nt = 0;
    v->ncs = 0;
}

static void view_rebuild(lg_view *v)
{
    if (v->key_seen == v->cap.n_seen && v->key_n == v->cap.n && (v->teeth_us != NULL || v->cs != NULL)) {
        return;
    }
    v->key_seen = v->cap.n_seen;
    v->key_n = v->cap.n;
    if (v->comp) {
        if (v->cs == NULL) { v->cs = (lt_comp_sample *)lt_calloc(LT_LOGCAP_MAX, sizeof(*v->cs)); }
        v->ncs = lt_logcap_composite(&v->cap, v->cs, LT_LOGCAP_MAX);
    } else {
        if (v->teeth_us == NULL) { v->teeth_us = (double *)lt_calloc(LT_LOGCAP_MAX, sizeof(double)); }
        v->nt = lt_logcap_tooth_us(&v->cap, v->teeth_us, LT_LOGCAP_MAX);
    }
}

static double current_rpm(lt_app *app)
{
    const lt_ini_channel *c;
    if (app->ini == NULL || app->rt == NULL) { return 0.0; }
    c = lt_ini_find_channel(app->ini, "rpm");
    if (c == NULL) { c = lt_ini_find_channel(app->ini, "RPMValue"); }
    return c != NULL ? lt_ini_rt_value(app->rt, c->index) : 0.0;
}

/* writes the capture as CSV; returns 0 and the path in out */
static int view_save(lt_app *app, lg_view *v, char *out, size_t cap)
{
    lt_buf csv;
    char *dir, *path, stamp[40], fname[80];
    size_t i;
    int rc;
    lt_buf_init(&csv);
    if (v->comp) { lt_comp_csv(v->cs, v->ncs, &csv); }
    else { lt_tooth_csv(v->teeth_us, v->nt, &csv); }
    dir = app->has_project ? lt_project_file(&app->project, "datalogs")
                           : lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "datalogs");
    (void)lt_plat_mkdirs(dir);
    lt_plat_timestamp(stamp, sizeof(stamp));
    for (i = 0; stamp[i] != '\0'; i++) {
        if (stamp[i] == ' ') { stamp[i] = '_'; }
        else if (stamp[i] == ':') { stamp[i] = '.'; }
    }
    lt_snprintf(fname, sizeof(fname), "%s_%s.csv", v->comp ? "composite" : "tooth", stamp);
    path = lt_path_join(dir, fname);
    rc = lt_file_write(path, csv.data, csv.len);
    if (rc == 0) { lt_strlcpy(out, path, cap); }
    lt_free(path);
    lt_free(dir);
    lt_buf_free(&csv);
    return rc;
}

static void capture_stop(lt_app *app, lg_view *v, int save)
{
    if (!v->capturing) { return; }
    v->capturing = 0;
    if (is_online(app) && v->lg != NULL) { (void)lt_proto_logger_stop(app->proto, v->lg); }
    view_rebuild(v);
    if (save && (v->comp ? v->ncs : v->nt) > 0) {
        char path[300];
        if (view_save(app, v, path, sizeof(path)) == 0) {
            lt_snprintf(v->saved, sizeof(v->saved), "Saved %lu records to %s", (unsigned long)v->cap.n_seen, path);
            lt_app_toast(app, 1, "%s", v->saved);
        } else {
            lt_app_toast(app, 3, "Auto-save failed: %s", lt_err_get());
        }
    }
}

static void capture_start(lt_app *app, lg_view *v)
{
    double rpm;
    v->err[0] = '\0';
    v->saved[0] = '\0';
    if (v->lg == NULL) { lt_strlcpy(v->err, logger_missing_msg(v->comp), sizeof(v->err)); return; }
    if (!is_online(app)) { lt_strlcpy(v->err, "Not connected to an ECU.", sizeof(v->err)); return; }
    if (v->lg->data_read_command.n == 0) {
        lt_snprintf(v->err, sizeof(v->err), "'%s' declares no dataReadCommand; there is nothing to read with",
                    v->lg->name != NULL ? v->lg->name : "logger");
        return;
    }
    rpm = current_rpm(app);
    if (!lt_proto_is_modern(app->proto) && rpm > 1500.0) {
        lt_snprintf(v->err, sizeof(v->err),
                    "Refusing to start the logger at %.0f rpm on the legacy protocol: its blocking "
                    "transmit stalls ignition scheduling. Capture while cranking or idling.", rpm);
        return;
    }
    lt_logcap_clear(&v->cap);
    v->key_seen = (unsigned long)-1;
    if (lt_proto_logger_start(app->proto, v->lg) < 0) {
        lt_snprintf(v->err, sizeof(v->err), "Failed to start '%s': %s",
                    v->lg->name != NULL ? v->lg->name : "logger", lt_err_get());
        return;
    }
    v->capturing = 1;
    v->t_start = lt_plat_now_ms();
    v->next_read = 0.0;
    v->has_a = 0;
    v->has_b = 0;
    v->scroll = 1.0;
}

static void capture_tick(lt_app *app, lg_view *v)
{
    lt_buf rep;
    double now;
    int rc;
    if (!v->capturing) { return; }
    if (!is_online(app) || v->lg == NULL) {
        lt_strlcpy(v->err, "Connection lost during capture.", sizeof(v->err));
        v->capturing = 0;
        return;
    }
    now = lt_plat_now_ms();
    if (!v->comp && now - v->t_start >= v->secs * 1000.0) {
        lt_snprintf(v->err, sizeof(v->err), "stopped at the %.0fs limit", v->secs);
        capture_stop(app, v, 1);
        return;
    }
    if (now < v->next_read) { return; }
    v->next_read = now + (app->headless ? 0.0 : READ_INTERVAL_MS);  /* no pacing for screenshots */
    lt_buf_init(&rep);
    rc = lt_proto_logger_read(app->proto, v->lg, &rep);
    if (rc < 0) {
        lt_snprintf(v->err, sizeof(v->err), "read failed after %lu reads: %s", v->cap.reads, lt_err_get());
        lt_buf_free(&rep);
        capture_stop(app, v, 1);
        return;
    }
    (void)lt_logcap_feed(&v->cap, (const lt_u8 *)rep.data, rep.len);
    lt_buf_free(&rep);
    if (v->cap.empty_reads > MAX_EMPTY_READS) {
        lt_strlcpy(v->err, "no data after 200 empty reads - is the engine running?", sizeof(v->err));
        capture_stop(app, v, 1);
        return;
    }
    if (!v->lg->continuous_read) {
        lt_strlcpy(v->err, "logger does not declare continuousRead; one buffer only", sizeof(v->err));
        capture_stop(app, v, 1);
    }
}

/* visible window of a series of n samples at the zoom/scroll of the view */
static void window_of(const lg_view *v, size_t n, size_t *start, size_t *count)
{
    size_t c = (size_t)((double)n / v->zoom);
    if (c < 2) { c = n < 2 ? n : 2; }
    if (c > n) { c = n; }
    *count = c;
    *start = (size_t)(v->scroll * (double)(n - c));
}

static void fmt_num(char *dst, size_t cap, double v, int dec)
{
    lt_fmt_double(dst, cap, v, dec);
}

/* ---- tooth plot ---- */
static void teeth_plot(lt_app *app, lg_view *v, lt_rect r, int click_l, int click_r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int padl = lt_ui_px(ui, 58), padr = lt_ui_px(ui, 10), padt = lt_ui_cell_mode(ui) ? 16 : lt_ui_px(ui, 8), padb = lt_ui_cell_mode(ui) ? 32 : lt_ui_px(ui, 22);
    int rh = lt_ui_row_height(ui), i, bw;
    lt_rect pl;
    size_t s, c, k;
    double lo, hi, avg = 0.0, range;
    lt_point m = lt_ui_mouse(ui);
    char b[64];
    pl.x = r.x + padl; pl.y = r.y + padt; pl.w = r.w - padl - padr; pl.h = r.h - padt - padb;
    lt_ui_draw_box(ui, r, th->table_bg, th->border, 3);
    if (pl.w < 20 || pl.h < 20) { return; }
    if (v->nt < 2) {
        lt_ui_draw_text_in(ui, r, v->capturing ? "Waiting for tooth data..."
                           : "No tooth data captured yet. Press Capture to record tooth timing from the ECU.",
                           th->muted, LT_UI_ALIGN_CENTER);
        return;
    }
    window_of(v, v->nt, &s, &c);
    lo = hi = v->teeth_us[s];
    for (k = s; k < s + c; k++) {
        avg += v->teeth_us[k];
        if (v->teeth_us[k] < lo) { lo = v->teeth_us[k]; }
        if (v->teeth_us[k] > hi) { hi = v->teeth_us[k]; }
    }
    avg /= (double)c;
    lo *= 0.9;
    hi *= 1.1;
    range = hi - lo > 1e-9 ? hi - lo : 1.0;
    for (i = 0; i <= 4; i++) {
        int y = pl.y + pl.h * i / 4;
        lt_rect lr;
        lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, y, th->table_grid);
        fmt_num(b, sizeof(b), hi - range * (double)i / 4.0, 0);
        lr.x = r.x + 2; lr.y = y - rh / 2; lr.w = padl - 6; lr.h = rh;
        lt_ui_draw_text_in(ui, lr, b, th->text2, LT_UI_ALIGN_RIGHT);
    }
    for (i = 0; i <= 10; i++) {
        int x = pl.x + pl.w * i / 10;
        lt_rect lr;
        lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h, th->table_grid);
        lt_snprintf(b, sizeof(b), "%lu", (unsigned long)(s + c * (size_t)i / 10));
        lr.x = x - lt_ui_px(ui, 20); lr.y = pl.y + pl.h + 2; lr.w = lt_ui_px(ui, 40); lr.h = rh;
        lt_ui_draw_text_in(ui, lr, b, th->text2, LT_UI_ALIGN_CENTER);
    }
    {
        int ay = pl.y + pl.h - (int)((avg - lo) / range * (double)pl.h);
        lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, ay, th->muted);
    }
    bw = (int)((double)pl.w / (double)c);
    bw = bw > 2 ? bw - 1 : 1;
    lt_ui_clip_push(ui, pl);
    for (k = 0; k < c; k++) {
        double t = v->teeth_us[s + k];
        int cls = lt_tooth_class(t, avg);
        lt_color col = cls == 0 ? th->success : (cls == 1 ? th->warning : th->error);
        int x = pl.x + (int)((double)pl.w * (double)k / (double)c);
        int bh = (int)((t - lo) / range * (double)pl.h);
        lt_rect br;
        if (bh < 1) { bh = 1; }
        br.x = x; br.y = pl.y + pl.h - bh; br.w = bw; br.h = bh;
        lt_ui_draw_fill(ui, br, col);
    }
    lt_ui_clip_pop(ui);
    lt_ui_draw_vline(ui, pl.x, pl.y, pl.y + pl.h, th->border_strong);
    lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, pl.y + pl.h, th->border_strong);
    /* cursors */
    for (i = 0; i < 2; i++) {
        int has = i == 0 ? v->has_a : v->has_b;
        double idx = i == 0 ? v->cur_a : v->cur_b;
        if (has && idx >= (double)s && idx < (double)(s + c)) {
            int x = pl.x + (int)((idx - (double)s + 0.5) * (double)pl.w / (double)c);
            lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h, i == 0 ? th->accent : th->warning);
        }
    }
    /* hover + clicks */
    if (in_rect(m, pl) && !lt_ui_popup_open(ui)) {
        size_t idx = s + (size_t)((double)(m.x - pl.x) * (double)c / (double)pl.w);
        lt_rect tr;
        if (idx >= s + c) { idx = s + c - 1; }
        if (click_l) { v->cur_a = (double)idx; v->has_a = 1; }
        if (click_r) { v->cur_b = (double)idx; v->has_b = 1; }
        lt_snprintf(b, sizeof(b), "tooth %lu: %.0f us", (unsigned long)idx, v->teeth_us[idx]);
        tr.w = lt_ui_text_width(ui, b) + lt_ui_px(ui, 12);
        tr.h = rh + 2;
        tr.x = m.x + lt_ui_px(ui, 12);
        if (tr.x + tr.w > r.x + r.w) { tr.x = m.x - tr.w - lt_ui_px(ui, 6); }
        tr.y = m.y - tr.h - 2;
        if (tr.y < r.y) { tr.y = m.y + lt_ui_px(ui, 14); }
        lt_ui_draw_box(ui, tr, th->elevated, th->border, 3);
        lt_ui_draw_text_in(ui, tr, b, th->text, LT_UI_ALIGN_CENTER);
    }
}

/* ---- composite plot ---- */
static size_t comp_lower(const lg_view *v, double t)
{
    size_t lo = 0, hi = v->ncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v->cs[mid].t_us < t) { lo = mid + 1; } else { hi = mid; }
    }
    return lo;
}

static void comp_plot(lt_app *app, lg_view *v, lt_rect r, int click_l, int click_r)
{
    static const lt_u32 cols[LT_COMP_CHANNELS] = {
        0xFF22C55Eu, 0xFF3B82F6u, 0xFFA855F7u, 0xFF14B8A6u, 0xFFEF4444u, 0xFFF59E0Bu
    };
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int padl = lt_ui_cell_mode(ui) ? 104 : lt_ui_px(ui, 78), padr = lt_ui_px(ui, 10), padt = lt_ui_cell_mode(ui) ? 16 : lt_ui_px(ui, 8), padb = lt_ui_cell_mode(ui) ? 32 : lt_ui_px(ui, 22);
    int rh = lt_ui_row_height(ui), i, nch = 0, ch[LT_COMP_CHANNELS], row;
    unsigned mask = lt_logcap_comp_mask(&v->cap);
    lt_rect pl;
    double t_first, span, wspan, t0, t1;
    lt_point m = lt_ui_mouse(ui);
    char b[64];
    pl.x = r.x + padl; pl.y = r.y + padt; pl.w = r.w - padl - padr; pl.h = r.h - padt - padb;
    lt_ui_draw_box(ui, r, th->table_bg, th->border, 3);
    if (pl.w < 20 || pl.h < 20) { return; }
    if (v->ncs < 2) {
        lt_ui_draw_text_in(ui, r, v->capturing ? "Waiting for composite data..."
                           : "No composite data captured yet. Press Capture while the engine is turning.",
                           th->muted, LT_UI_ALIGN_CENTER);
        return;
    }
    for (i = 0; i < LT_COMP_CHANNELS; i++) { if (mask & (1u << i)) { ch[nch++] = i; } }
    if (nch == 0) { return; }
    t_first = v->cs[0].t_us;
    span = v->cs[v->ncs - 1].t_us - t_first;
    if (span <= 0.0) { return; }
    wspan = span / v->zoom;
    t0 = t_first + v->scroll * (span - wspan);
    t1 = t0 + wspan;
    row = pl.h / nch;
    for (i = 0; i < nch; i++) {
        int y0 = pl.y + row * i;
        lt_rect lr;
        lt_ui_draw_hline(ui, pl.x, pl.x + pl.w, y0 + row, th->table_grid);
        lr.x = r.x + 4; lr.y = y0 + row / 2 - rh / 2; lr.w = padl - 10; lr.h = rh;
        lt_ui_draw_text_in(ui, lr, lt_comp_names[ch[i]], (lt_color)cols[ch[i]], LT_UI_ALIGN_RIGHT);
    }
    for (i = 0; i <= 10; i++) {
        int x = pl.x + pl.w * i / 10;
        lt_rect lr;
        lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h, th->table_grid);
        lt_snprintf(b, sizeof(b), "%.1fms", (t0 + wspan * (double)i / 10.0 - t_first) / 1000.0);
        lr.x = x - lt_ui_px(ui, 28); lr.y = pl.y + pl.h + 2; lr.w = lt_ui_px(ui, 56); lr.h = rh;
        lt_ui_draw_text_in(ui, lr, b, th->text2, LT_UI_ALIGN_CENTER);
    }
    lt_ui_clip_push(ui, pl);
    {
        size_t first = comp_lower(v, t0), idx;
        for (i = 0; i < nch; i++) {
            int y0 = pl.y + row * i, hy = y0 + 4, ly = y0 + row - 5;
            int c = ch[i], cur, lastx = pl.x, thick = lt_ui_px(ui, 2) > 1 ? lt_ui_px(ui, 2) : 1;
            lt_rect sr;
            idx = first > 0 ? first - 1 : 0;
            cur = v->cs[idx].lvl[c];
            for (; idx < v->ncs; idx++) {
                int x, lv = v->cs[idx].lvl[c];
                if (v->cs[idx].t_us < t0) { cur = lv; continue; }
                if (v->cs[idx].t_us > t1) { break; }
                x = pl.x + (int)((v->cs[idx].t_us - t0) / wspan * (double)pl.w);
                if (lv != cur) {
                    sr.x = lastx; sr.y = (cur ? hy : ly) - thick / 2; sr.w = x - lastx + 1; sr.h = thick;
                    lt_ui_draw_fill(ui, sr, (lt_color)cols[c]);
                    sr.x = x; sr.y = hy; sr.w = thick; sr.h = ly - hy + 1;
                    lt_ui_draw_fill(ui, sr, (lt_color)cols[c]);
                    lastx = x;
                    cur = lv;
                }
            }
            sr.x = lastx; sr.y = (cur ? hy : ly) - thick / 2; sr.w = pl.x + pl.w - lastx; sr.h = thick;
            lt_ui_draw_fill(ui, sr, (lt_color)cols[c]);
        }
    }
    for (i = 0; i < 2; i++) {
        int has = i == 0 ? v->has_a : v->has_b;
        double t = i == 0 ? v->cur_a : v->cur_b;
        if (has && t >= t0 && t <= t1) {
            int x = pl.x + (int)((t - t0) / wspan * (double)pl.w);
            lt_ui_draw_vline(ui, x, pl.y, pl.y + pl.h, i == 0 ? th->accent : th->warning);
        }
    }
    lt_ui_clip_pop(ui);
    if (in_rect(m, pl) && !lt_ui_popup_open(ui)) {
        double t = t0 + (double)(m.x - pl.x) / (double)pl.w * wspan;
        if (click_l) { v->cur_a = t; v->has_a = 1; }
        if (click_r) { v->cur_b = t; v->has_b = 1; }
        lt_ui_draw_vline(ui, m.x, pl.y, pl.y + pl.h, th->border_strong);
    }
}

static void view_draw(lt_app *app, lt_app_tab *tab, lt_rect r, int comp)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lg_view *v = view_get(tab, comp);
    int w1 = -1, wc[10], wz[4], nrec, cl, cr, i;
    int busy;
    char b[240], n1[40], n2[40], n3[40];
    lt_rect pr;
    LT_UNUSED(r);
    view_sync_logger(app, v);
    if (!v->auto_done && app->headless) {   /* LT_SHOT_CAPTURE=N: start a capture after N frames */
        char *e = lt_plat_getenv("LT_SHOT_CAPTURE");
        if (e == NULL) { v->auto_done = 1; }
        else if (++v->frames > atoi(e)) {
            char *z = lt_plat_getenv("LT_SHOT_ZOOM");      /* optional zoom for the preview */
            v->auto_done = 1;
            capture_start(app, v);
            if (z != NULL) { v->zoom = clampd(atof(z), 1.0, 50.0); v->scroll = 0.5; lt_free(z); }
        }
        lt_free(e);
    }
    capture_tick(app, v);
    view_rebuild(v);
    nrec = (int)(comp ? v->ncs : v->nt);
    busy = v->capturing;
    if (busy) { lt_ui_request_redraw(ui); }

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(b, sizeof(b), "%s%s%s", comp ? "Composite Logger" : "Tooth Logger",
                v->lg != NULL && v->lg->label != NULL ? " - " : "",
                v->lg != NULL && v->lg->label != NULL ? v->lg->label : "");
    lt_ui_heading(ui, b);

    /* controls */
    i = 0;
    if (!comp) {
        wc[i++] = CH * 3; wc[i++] = CH * 7; wc[i++] = CH * 2;
        wc[i++] = CH * 9; wc[i++] = CH * 7;
        wc[i++] = CH * 8; wc[i++] = CH * 5;
    }
    wc[i++] = CH * 10; wc[i++] = CH * 12; wc[i++] = CH * 8;
    lt_ui_layout_row(ui, i, wc, 0);
    lt_ui_disabled_begin(ui, busy);
    if (!comp) {
        lt_ui_label(ui, "for");
        (void)lt_ui_number(ui, "secs", &v->secs, 1, 600, 1, 0);
        lt_ui_label(ui, "s");
        lt_ui_label(ui, "teeth/rev");
        (void)lt_ui_number(ui, "teeth", &v->teeth, 1, 360, 1, 0);
        lt_ui_label(ui, "missing");
        (void)lt_ui_number(ui, "missing", &v->missing, 0, 10, 1, 0);
    }
    lt_ui_disabled_end(ui);
    if (lt_ui_button_ex(ui, busy ? "Stop" : "Capture", busy ? LT_UI_BTN_DANGER : LT_UI_BTN_PRIMARY)) {
        if (busy) { capture_stop(app, v, 1); } else { capture_start(app, v); }
    }
    if (lt_ui_button_ex(ui, "Export CSV", nrec > 0 ? 0 : LT_UI_BTN_DISABLED) && nrec > 0) {
        char path[300];
        if (view_save(app, v, path, sizeof(path)) == 0) {
            lt_snprintf(v->saved, sizeof(v->saved), "Exported %d records to %s", nrec, path);
            lt_app_toast(app, 1, "%s", v->saved);
        } else {
            lt_app_toast(app, 3, "Export failed: %s", lt_err_get());
        }
    }
    if (lt_ui_button_ex(ui, "Clear", (nrec > 0 && !busy) ? 0 : LT_UI_BTN_DISABLED) && nrec > 0 && !busy) {
        lt_logcap_clear(&v->cap);
        v->key_seen = (unsigned long)-1;
        v->has_a = 0;
        v->has_b = 0;
        v->saved[0] = '\0';
        view_rebuild(v);
    }

    /* zoom / scroll */
    wz[0] = CH * 5; wz[1] = -1; wz[2] = CH * 7; wz[3] = -2;
    lt_ui_layout_row(ui, 4, wz, 0);
    lt_ui_label(ui, "Zoom");
    (void)lt_ui_slider(ui, "zoom", &v->zoom, 1.0, 50.0, 1);
    lt_ui_label(ui, "Scroll");
    (void)lt_ui_slider(ui, "scroll", &v->scroll, 0.0, 1.0, 2);

    if (v->err[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, v->err, th->error, LT_UI_ALIGN_LEFT);
    } else if (v->saved[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, v->saved, th->success, LT_UI_ALIGN_LEFT);
    }

    /* statistics */
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (!comp) {
        lt_tooth_stats st;
        int ntp = (int)v->teeth - (int)v->missing;
        double rpm = lt_tooth_rpm(v->teeth_us, v->nt, ntp);
        if (lt_tooth_stats_calc(v->teeth_us, v->nt, &st) == 0) {
            fmt_num(n1, sizeof(n1), st.avg, 0);
            fmt_num(n2, sizeof(n2), st.variability, 1);
            lt_snprintf(b, sizeof(b), "Detected RPM: %.0f   Avg: %s us   Variability: %s%%   Min/Max: %.0f / %.0f us",
                        rpm, n1, n2, st.min, st.max);
            lt_ui_label_ex(ui, b, st.variability > 5.0 ? th->warning : th->text, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (st.missing_idx >= 0) {
                lt_snprintf(b, sizeof(b), "Missing tooth: gap at #%ld (%lu gaps)   Extra/noise teeth: %lu",
                            st.missing_idx, (unsigned long)st.missing_n, (unsigned long)st.extra_n);
            } else {
                lt_snprintf(b, sizeof(b), "Missing tooth: none detected   Extra/noise teeth: %lu",
                            (unsigned long)st.extra_n);
            }
            lt_ui_label_ex(ui, b, (st.missing_idx >= 0 || st.extra_n > 0) ? th->accent : th->muted,
                           LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "Teeth: 0", th->muted, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label(ui, "");
        }
    } else {
        lt_comp_stats st;
        if (lt_comp_stats_calc(v->cs, v->ncs, &st) == 0) {
            fmt_num(n1, sizeof(n1), st.rate_hz / 1000.0, 1);
            fmt_num(n2, sizeof(n2), st.duration_ms, 1);
            lt_snprintf(b, sizeof(b), "Samples: %lu   Sample rate: %s kHz   Duration: %s ms",
                        (unsigned long)v->ncs, n1, n2);
            lt_ui_label(ui, b);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (st.sync_lost > 0) { lt_snprintf(n3, sizeof(n3), " (lost %lux)", st.sync_lost); }
            else { n3[0] = '\0'; }
            lt_snprintf(b, sizeof(b), "Primary pulses: %lu   Secondary pulses: %lu   Sync: %s%s",
                        st.pri_pulses, st.sec_pulses, st.sync_acquired ? "Acquired" : "Not acquired", n3);
            lt_ui_label_ex(ui, b, st.sync_acquired ? th->success : th->warning, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "Samples: 0", th->muted, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label(ui, "");
        }
    }
    /* cursors */
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (v->has_a || v->has_b) {
        if (comp) {
            double base = v->ncs > 0 ? v->cs[0].t_us : 0.0;
            fmt_num(n1, sizeof(n1), (v->cur_a - base) / 1000.0, 3);
            fmt_num(n2, sizeof(n2), (v->cur_b - base) / 1000.0, 3);
            lt_snprintf(b, sizeof(b), "A: %s   B: %s", v->has_a ? n1 : "-", v->has_b ? n2 : "-");
            if (v->has_a && v->has_b && v->cur_a != v->cur_b) {
                double d = v->cur_b - v->cur_a;
                fmt_num(n3, sizeof(n3), d / 1000.0, 3);
                lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), " ms   Delta: %s ms (%.1f Hz)", n3,
                            1000000.0 / (d < 0 ? -d : d));
            } else { lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), " ms"); }
        } else {
            size_t ia = (size_t)v->cur_a, ib = (size_t)v->cur_b;
            b[0] = '\0';
            if (ia >= v->nt) { ia = v->nt > 0 ? v->nt - 1 : 0; }
            if (ib >= v->nt) { ib = v->nt > 0 ? v->nt - 1 : 0; }
            if (v->has_a) { lt_snprintf(b, sizeof(b), "A: tooth %lu (%.0f us)", (unsigned long)ia, v->teeth_us[ia]); }
            if (v->has_b) {
                lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), "%sB: tooth %lu (%.0f us)", v->has_a ? "   " : "",
                            (unsigned long)ib, v->teeth_us[ib]);
            }
            if (v->has_a && v->has_b && v->nt > 0) {
                size_t a = ia, e = ib, k;
                double sum = 0.0;
                if (a > e) { size_t t = a; a = e; e = t; }
                for (k = a + 1; k <= e && k < v->nt; k++) { sum += v->teeth_us[k]; }
                lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), "   Delta: %lu teeth, %.0f us",
                            (unsigned long)(e - a), sum);
            }
        }
        lt_ui_label_ex(ui, b, th->accent, LT_UI_ALIGN_LEFT);
    } else {
        lt_ui_label_ex(ui, "Left click sets cursor A, right click sets cursor B.", th->muted, LT_UI_ALIGN_LEFT);
    }

    lt_ui_layout_row(ui, 1, &w1, -1);
    pr = lt_ui_layout_next(ui);
    {
        lt_point m = lt_ui_mouse(ui);
        int dl = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0, dr = lt_ui_mouse_down(ui, LT_MB_RIGHT) ? 1 : 0;
        cl = dl && !v->prev_l && in_rect(m, pr);
        cr = dr && !v->prev_r && in_rect(m, pr);
        v->prev_l = dl;
        v->prev_r = dr;
    }
    if (comp) { comp_plot(app, v, pr, cl, cr); } else { teeth_plot(app, v, pr, cl, cr); }
}

static void teeth_draw(lt_app *app, lt_app_tab *tab, lt_rect r) { view_draw(app, tab, r, 0); }
static void comp_draw(lt_app *app, lt_app_tab *tab, lt_rect r) { view_draw(app, tab, r, 1); }

/* ------------------------------------------------------------------ console */
#define CON_MAX 500
#define CON_LINE 200

typedef struct con_shared {
    char (*lines)[CON_LINE];         /* first char: '>' command, '<' reply, '!' error, '#' note */
    int n;
    int loaded;
    int warn_off;                    /* "Always Allow": no controller command confirmation */
    int warn_loaded;
    /* pending controller command */
    int pend_open;
    char pend_name[96], pend_label[96], pend_desc[320];
} con_shared;

typedef struct con_tab {
    char input[CON_LINE];
    int hist_pos;                    /* -1 = editing a new line */
    int last_n;
} con_tab;

static void con_shared_free(lt_app *app, void *p)
{
    con_shared *s = (con_shared *)p;
    LT_UNUSED(app);
    lt_free(s->lines);
}

static con_shared *con_get(lt_app *app)
{
    return (con_shared *)lt_ext_state(app, "console", sizeof(con_shared), con_shared_free);
}

static char *hist_path(lt_app *app, const char *name)
{
    return lt_path_join(app->config_dir != NULL ? app->config_dir : ".", name);
}

static void con_save(lt_app *app, con_shared *s)
{
    lt_buf b;
    char *path;
    int i;
    lt_buf_init(&b);
    for (i = 0; i < s->n; i++) {
        lt_buf_puts(&b, s->lines[i]);
        lt_buf_puts(&b, "\n");
    }
    path = hist_path(app, "console_history.txt");
    (void)lt_file_write(path, b.data, b.len);
    lt_free(path);
    lt_buf_free(&b);
}

static void con_add(con_shared *s, char tag, const char *text)
{
    if (s->lines == NULL) { s->lines = (char (*)[CON_LINE])lt_calloc(CON_MAX, CON_LINE); }
    if (s->n >= CON_MAX) {
        memmove(s->lines[0], s->lines[1], (size_t)(CON_MAX - 1) * CON_LINE);
        s->n = CON_MAX - 1;
    }
    s->lines[s->n][0] = tag;
    lt_strlcpy(s->lines[s->n] + 1, text, CON_LINE - 1);
    s->n++;
}

/* add possibly multi-line text */
static void con_add_text(con_shared *s, char tag, const char *text)
{
    const char *p = text;
    while (*p != '\0') {
        char tmp[CON_LINE];
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '\n' && p[len] != '\r' && len < sizeof(tmp) - 1) { len++; }
        memcpy(tmp, p, len);
        tmp[len] = '\0';
        p += len;
        while (*p == '\n' || *p == '\r') { p++; }
        if (tmp[0] != '\0') { con_add(s, tag, tmp); }
    }
}

static void con_load(lt_app *app, con_shared *s)
{
    char *path, *txt;
    size_t len;
    if (s->loaded) { return; }
    s->loaded = 1;
    path = hist_path(app, "console_history.txt");
    txt = lt_file_read(path, &len);
    lt_free(path);
    if (txt != NULL) {
        char *p = txt;
        while (*p != '\0') {
            char *e = strchr(p, '\n');
            if (e != NULL) { *e = '\0'; }
            if (p[0] == '>' || p[0] == '<' || p[0] == '!' || p[0] == '#') { con_add(s, p[0], p + 1); }
            if (e == NULL) { break; }
            p = e + 1;
        }
        lt_free(txt);
    }
    if (!s->warn_loaded) {
        char *wp = hist_path(app, "command_warnings_off");
        s->warn_off = lt_file_exists(wp) ? 1 : 0;
        s->warn_loaded = 1;
        lt_free(wp);
    }
}

/* the n-th previous command (0 = latest), or NULL */
static const char *con_hist_cmd(const con_shared *s, int back)
{
    int i;
    for (i = s->n - 1; i >= 0; i--) {
        if (s->lines[i][0] == '>') {
            if (back == 0) { return s->lines[i] + 1; }
            back--;
        }
    }
    return NULL;
}

static void con_send(lt_app *app, con_shared *s, con_tab *t, const char *cmd)
{
    char *out = NULL;
    int online = is_online(app);
    con_add(s, '>', cmd);
    if (!online) {
        con_add(s, '!', "Not connected.");
    } else if (lt_proto_console(app->proto, cmd, 1500, &out) < 0) {
        char e[CON_LINE];
        lt_snprintf(e, sizeof(e), "%s", lt_err_get());
        con_add_text(s, '!', e);
    } else if (out != NULL) {
        con_add_text(s, '<', out);
    }
    lt_free(out);
    t->hist_pos = -1;
    con_save(app, s);
}

static const lt_ini_cmd *find_dfu_command(const lt_app *app)
{
    size_t i;
    if (app->ini == NULL) { return NULL; }
    for (i = 0; i < app->ini->n_commands; i++) {
        const lt_ini_cmd *c = app->ini->commands[i];
        if (c->name != NULL && lt_dlg_contains_ci(c->name, "dfu")) { return c; }
    }
    return NULL;
}

void lt_app_controller_command(lt_app *app, const char *name, const char *label, const char *desc)
{
    con_shared *s = con_get(app);
    con_load(app, s);
    lt_strlcpy(s->pend_name, name, sizeof(s->pend_name));
    lt_strlcpy(s->pend_label, label != NULL ? label : name, sizeof(s->pend_label));
    lt_strlcpy(s->pend_desc, desc != NULL ? desc : "", sizeof(s->pend_desc));
    if (s->warn_off) {
        const lt_ini_cmd *cmd = app->ini != NULL ? lt_ini_find_command(app->ini, name) : NULL;
        if (cmd == NULL) { lt_app_toast(app, 3, "Unknown command %s", name); }
        else if (!is_online(app)) { lt_app_toast(app, 2, "Connect to an ECU to run %s", s->pend_label); }
        else if (lt_proto_run_command(app->proto, cmd) < 0) {
            lt_app_toast(app, 3, "Command failed: %s", lt_err_get());
        } else {
            lt_app_toast(app, 1, "%s sent to ECU", s->pend_label);
        }
        return;
    }
    s->pend_open = 1;
}

static void cmd_overlay(lt_app *app)
{
    con_shared *s = con_get(app);
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w3[3], open, is_dfu;
    if (!s->pend_open) { return; }
    open = s->pend_open;
    is_dfu = lt_dlg_contains_ci(s->pend_name, "dfu");
    if (lt_ui_modal_begin(ui, s->pend_label, 440, 190, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->pend_desc[0] != '\0' ? s->pend_desc
                         : (is_dfu ? "This will reset the ECU into DFU (firmware update) mode. The serial "
                                     "connection will drop and you will need an external tool to flash firmware."
                                   : "This sends a raw command directly to the ECU, bypassing normal memory "
                                     "synchronization."), th->text);
        if (!is_dfu) {
            lt_ui_label_wrap(ui, "Only proceed if you understand what this command does.", th->warning);
        }
        w3[0] = -1; w3[1] = CH * 14; w3[2] = CH * 14;
        lt_ui_layout_row(ui, 3, w3, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        {
            int once = lt_ui_button_ex(ui, "Execute Once", LT_UI_BTN_DEFAULT);
            int always = 0;
            if (!is_dfu) {
                lt_ui_layout_row(ui, 3, w3, 0);
                lt_ui_label(ui, "");
                lt_ui_label(ui, "");
                always = lt_ui_button_ex(ui, "Always Allow", LT_UI_BTN_DANGER);
            }
            if (once || always) {
                const lt_ini_cmd *cmd = lt_ini_find_command(app->ini, s->pend_name);
                if (always) {
                    char *wp = hist_path(app, "command_warnings_off");
                    (void)lt_file_write(wp, "1\n", 2);
                    lt_free(wp);
                    s->warn_off = 1;
                }
                if (cmd == NULL) { lt_app_toast(app, 3, "Unknown command %s", s->pend_name); }
                else if (!is_online(app)) { lt_app_toast(app, 2, "Connect to an ECU first"); }
                else if (lt_proto_run_command(app->proto, cmd) < 0) {
                    lt_app_toast(app, 3, "Command failed: %s", lt_err_get());
                } else {
                    lt_app_toast(app, 1, is_dfu ? "%s sent - the ECU should reboot into DFU mode"
                                                : "%s sent to ECU", s->pend_label);
                }
                open = 0;
            }
        }
        lt_ui_modal_end(ui);
    }
    s->pend_open = open;
}

static void console_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    con_shared *s = con_get(app);
    con_tab *t = (con_tab *)tab->data;
    int w1 = -1, w2[3], wq[6], i, fl, online = is_online(app);
    static const char *const quick[] = { "help", "status", "hello", "triggerinfo", "tsinfo", "tempinfo",
                                         "tpsinfo", "mapinfo", "analoginfo", "caninfo", "sdinfo" };
    static const char *const qtip[] = { "List all supported commands", "Show ECU status",
        "Print firmware version", "Trigger state and diagnostics", "TunerStudio comms summary",
        "CLT and IAT sensor info", "TPS sensor info", "MAP sensor info", "ADC input values",
        "CAN subsystem stats", "SD card usage" };
    const lt_ini_cmd *dfu;
    char hdr[160];
    const char *pending = NULL;
    LT_UNUSED(r);
    if (t == NULL) {
        t = (con_tab *)lt_calloc(1, sizeof(*t));
        t->hist_pos = -1;
        tab->data = t;
    }
    con_load(app, s);

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(hdr, sizeof(hdr), "ECU Console%s%s", app->ini != NULL && app->ini->signature != NULL ? " - " : "",
                app->ini != NULL && app->ini->signature != NULL ? app->ini->signature : "");
    lt_ui_heading(ui, hdr);
    dfu = find_dfu_command(app);
    w2[0] = -1; w2[1] = CH * 12; w2[2] = CH * 14;
    lt_ui_layout_row(ui, 3, w2, 0);
    if (!online) { lt_ui_label_ex(ui, "DISCONNECTED: connect to an ECU to use the console", th->warning, LT_UI_ALIGN_LEFT); }
    else { lt_ui_label_ex(ui, "Up/Down or Ctrl+Up/Down: command history", th->muted, LT_UI_ALIGN_LEFT); }
    if (dfu != NULL) {
        if (lt_ui_button_ex(ui, "Enter DFU", online ? 0 : LT_UI_BTN_DISABLED) && online) {
            lt_app_controller_command(app, dfu->name, "Enter DFU Mode",
                "Reset the ECU into DFU (firmware update) mode. The connection will drop - use your "
                "board's flash tool to update firmware.");
        }
    } else { lt_ui_label(ui, ""); }
    if (lt_ui_button_ex(ui, "Clear History", s->n > 0 ? 0 : LT_UI_BTN_DISABLED) && s->n > 0) {
        s->n = 0;
        con_save(app, s);
    }

    lt_ui_layout_row(ui, 1, &w1, -92);
    if (lt_ui_scroll_begin(ui, "con")) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->n == 0) {
            lt_ui_label_ex(ui, "Welcome to the ECU console. Type a command and press Enter.", th->muted, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, "Examples: help, status, hello, set someVar 100", th->muted, LT_UI_ALIGN_LEFT);
        }
        for (i = 0; i < s->n; i++) {
            const char *ln = s->lines[i];
            char buf[CON_LINE + 4];
            lt_color col = th->text;
            if (ln[0] == '>') { lt_snprintf(buf, sizeof(buf), "> %s", ln + 1); col = th->accent; }
            else if (ln[0] == '!') { lt_snprintf(buf, sizeof(buf), "x %s", ln + 1); col = th->error; }
            else if (ln[0] == '#') { lt_snprintf(buf, sizeof(buf), "  %s", ln + 1); col = th->muted; }
            else { lt_snprintf(buf, sizeof(buf), "  %s", ln + 1); }
            lt_ui_label_ex(ui, buf, col, LT_UI_ALIGN_LEFT);
        }
        if (s->n != t->last_n) { lt_ui_scroll_set(ui, 0, 1 << 20); t->last_n = s->n; }
        lt_ui_scroll_end(ui);
    }

    /* quick commands */
    for (i = 0; i < (int)LT_COUNTOF(quick); i += 6) {
        int k, cnt = (int)LT_COUNTOF(quick) - i < 6 ? (int)LT_COUNTOF(quick) - i : 6;
        for (k = 0; k < 6; k++) { wq[k] = 0; }
        lt_ui_layout_row(ui, cnt, wq, 0);
        for (k = 0; k < cnt; k++) {
            if (lt_ui_button_ex(ui, quick[i + k], online ? LT_UI_BTN_FLAT : LT_UI_BTN_DISABLED) && online) {
                pending = quick[i + k];
            }
            lt_ui_tooltip(ui, qtip[i + k]);
        }
    }

    w2[0] = CH * 2; w2[1] = -1; w2[2] = CH * 10;
    lt_ui_layout_row(ui, 3, w2, 0);
    lt_ui_label_ex(ui, ">", th->accent, LT_UI_ALIGN_CENTER);
    fl = lt_ui_text(ui, "cmd", t->input, sizeof(t->input), 0);
    {
        lt_u32 input_id = lt_ui_last_id(ui);
        int up = 0, down = 0;
        up = lt_ui_key_pressed(ui, LT_KEY_UP, LT_MOD_CTRL) || lt_ui_key_pressed(ui, LT_KEY_UP, 0);
        down = lt_ui_key_pressed(ui, LT_KEY_DOWN, LT_MOD_CTRL) || lt_ui_key_pressed(ui, LT_KEY_DOWN, 0);
        if (up || down) {
            int np = t->hist_pos + (up ? 1 : -1);
            const char *h = np >= 0 ? con_hist_cmd(s, np) : NULL;
            if (np < 0) { t->hist_pos = -1; t->input[0] = '\0'; }
            else if (h != NULL) { t->hist_pos = np; lt_strlcpy(t->input, h, sizeof(t->input)); }
            lt_ui_set_focus(ui, input_id);
        }
        if (fl & LT_UI_SUBMIT) { pending = t->input[0] != '\0' ? t->input : NULL; }
        if (lt_ui_button_ex(ui, "Send", (online && t->input[0] != '\0') ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) &&
            t->input[0] != '\0') {
            pending = t->input;
        }
        if (pending != NULL) {
            char cmd[CON_LINE];
            lt_strlcpy(cmd, pending, sizeof(cmd));
            con_send(app, s, t, cmd);
            t->input[0] = '\0';
            lt_ui_set_focus(ui, input_id);
        }
    }
}

static void console_free(lt_app *app, lt_app_tab *tab)
{
    LT_UNUSED(app);
    lt_free(tab->data);
    tab->data = NULL;
}

/* ------------------------------------------------------------------ registration */
static int open_tooth(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    return lt_app_tab_open(app, "toothlog", "tooth-logger", "Tooth Logger", 1) != NULL ? 0 : -1;
}

static int open_comp(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    return lt_app_tab_open(app, "complog", "composite-logger", "Composite Logger", 1) != NULL ? 0 : -1;
}

static int open_console(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    return lt_app_tab_open(app, "console", "console", "Console", 1) != NULL ? 0 : -1;
}

static void act_console(lt_app *app) { (void)open_console(app, "console"); }

static void act_dfu(lt_app *app)
{
    const lt_ini_cmd *c = find_dfu_command(app);
    if (c != NULL) {
        lt_app_controller_command(app, c->name, "Enter DFU Mode",
            "Reset the ECU into DFU (firmware update) mode. The connection will drop - use your "
            "board's flash tool to update firmware.");
    }
}

static int pred_console(lt_app *app)
{
    return app->ini != NULL && lt_ini_supports_console(app->ini);
}

static int pred_dfu(lt_app *app)
{
    return app->ini != NULL && find_dfu_command(app) != NULL && is_online(app);
}

void lt_feat_loggers_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    lt_ext_add_target(reg, "tooth-logger", open_tooth);
    lt_ext_add_target(reg, "composite-logger", open_comp);
    lt_ext_add_target(reg, "console", open_console);
    tk.kind = "toothlog"; tk.draw = teeth_draw; tk.close = view_free;
    lt_ext_add_tab_kind(reg, &tk);
    tk.kind = "complog"; tk.draw = comp_draw; tk.close = view_free;
    lt_ext_add_tab_kind(reg, &tk);
    tk.kind = "console"; tk.draw = console_draw; tk.close = console_free;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "ECU Conso&le", NULL, act_console, pred_console);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Enter D&FU Mode...", NULL, act_dfu, pred_dfu);
    lt_ext_add_overlay(reg, cmd_overlay);
}
