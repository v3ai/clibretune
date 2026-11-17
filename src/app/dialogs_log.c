/* dialogs_log.c - Data logging tab (live chart + recording) and the datalog
 * viewer (MLG/CSV playback with timeline scrubber, channel picker, CSV export).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dialogs.h"
#include "view_kit.h"
#include "feat_log.h"

#define CH 8
#define LT_DLG_FD_LOG_EXPORT_MLG (LT_APP_FD_USER0 + 85)
#define MAX_SEL 8
#define HN 400              /* live history samples */
#define MAXPTS 512

typedef struct log_state {
    int viewer;
    char filter[64];
    int *sel_idx; int n_sel;             /* selected channel indices (<= MAX_SEL) */
    /* live history, one row per selected slot */
    double hist[MAX_SEL][HN];
    int hn;
    double last_sample_ms;
    /* viewer */
    lt_log log; int have_log;
    lt_player pl;
    char path[512];
    int playing, speed_sel, loop, drive, zoom_sel;
    double cursor_t, last_ms;
    char **files; int n_files, file_sel, files_loaded;
    char dir[512];
    char err[160];
    int show_stats;
    int rename_on, del_confirm;
    char rename_buf[128];
    int n_math;                          /* math channels appended to the loaded log */
} log_state;

static const char *const speeds[] = { "0.25x", "0.5x", "1x", "2x", "4x", "8x" };
static const double speed_v[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
static const char *const zooms[] = { "Whole log", "120 s", "60 s", "30 s", "10 s" };
static const double zoom_v[] = { 0.0, 120.0, 60.0, 30.0, 10.0 };

static const lt_color palette[MAX_SEL] = {
    0xFF4FC3F7UL, 0xFFFFB74DUL, 0xFF81C784UL, 0xFFE57373UL,
    0xFFBA68C8UL, 0xFFFFF176UL, 0xFF4DB6ACUL, 0xFFF06292UL
};

/* ---- channel set accessors ---- */
static int n_channels(lt_app *app, log_state *s)
{
    if (s->viewer) { return s->have_log ? s->log.n_ch : 0; }
    return app->ini != NULL ? (int)app->ini->n_channels : 0;
}

static const char *chan_name(lt_app *app, log_state *s, int i)
{
    if (s->viewer) { return s->log.names[i]; }
    return app->ini->channels[i]->name;
}

static const char *chan_units(lt_app *app, log_state *s, int i)
{
    const char *u;
    if (s->viewer) { u = s->log.units != NULL ? s->log.units[i] : ""; }
    else { u = app->ini->channels[i]->units; }
    return u != NULL ? u : "";
}

static int sel_find(log_state *s, int idx)
{
    int i;
    for (i = 0; i < s->n_sel; i++) { if (s->sel_idx[i] == idx) { return i; } }
    return -1;
}

static void sel_toggle(log_state *s, int idx, int on)
{
    int k = sel_find(s, idx), i;
    if (on && k < 0 && s->n_sel < MAX_SEL) {
        s->sel_idx[s->n_sel] = idx;
        memset(s->hist[s->n_sel], 0, sizeof(s->hist[0]));
        s->n_sel++;
    } else if (!on && k >= 0) {
        for (i = k; i < s->n_sel - 1; i++) {
            s->sel_idx[i] = s->sel_idx[i + 1];
            memcpy(s->hist[i], s->hist[i + 1], sizeof(s->hist[0]));
        }
        s->n_sel--;
    }
}

static void sel_defaults(lt_app *app, log_state *s)
{
    static const char *const want[] = { "rpm", "tps", "map", "afr", "clt", "coolant", "ve" };
    int w, i, n = n_channels(app, s);
    s->n_sel = 0;
    for (w = 0; w < 7 && s->n_sel < 4; w++) {
        int best = -1;
        for (i = 0; i < n; i++) {
            const char *nm = chan_name(app, s, i);
            if (lt_dlg_contains_ci(nm, want[w]) && sel_find(s, i) < 0 &&
                (best < 0 || strlen(nm) < strlen(chan_name(app, s, best)))) { best = i; }
        }
        if (best >= 0) { sel_toggle(s, best, 1); }
    }
    if (s->n_sel == 0) { for (i = 0; i < n && i < 3; i++) { sel_toggle(s, i, 1); } }
}

/* ---- chart ---- */
static void chart_draw(lt_ui *ui, lt_rect r, int ns, const char *const *names,
                       const char *const *units, double *const *pts, int npts,
                       const double *cur_vals, double cursor_frac,
                       const lt_log *mk, double mk_t0, double mk_t1)
{
    const lt_theme *t = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int k, i, g, top = cell ? 20 : lt_ui_px(ui, 30), lx = r.x + (cell ? 10 : lt_ui_px(ui, 10));
    lt_rect pr;
    lt_ui_draw_box(ui, r, t->table_bg, t->border_subtle, cell ? 0 : lt_ui_px(ui, 6));
    pr.x = r.x + (cell ? 6 : lt_ui_px(ui, 6)); pr.y = r.y + top; pr.w = r.w - 2 * (cell ? 6 : lt_ui_px(ui, 6)); pr.h = r.h - top - (cell ? 8 : lt_ui_px(ui, 8));
    if (!cell) {
        lt_ui_draw_vgrad(ui, pr, LT_RGBA(255, 255, 255, t->dark ? 6 : 0), LT_RGBA(0, 0, 0, t->dark ? 40 : 6));
    }
    for (g = 1; g < 4; g++) {
        lt_ui_draw_hline(ui, pr.x, pr.x + pr.w, pr.y + pr.h * g / 4, cell ? t->table_grid : LT_RGBA(255, 255, 255, 22));
    }
    for (g = 1; g < 8; g++) {
        lt_ui_draw_vline(ui, pr.x + pr.w * g / 8, pr.y, pr.y + pr.h, cell ? t->table_grid : LT_RGBA(255, 255, 255, 14));
    }
    lt_ui_clip_push(ui, r);
    for (k = 0; k < ns; k++) {
        double lo = 1e300, hi = -1e300, x0 = 0, y0 = 0, x1, y1;
        char lab[96], vb[32];
        lt_color col = palette[k];
        for (i = 0; i < npts; i++) {
            if (pts[k][i] < lo) { lo = pts[k][i]; }
            if (pts[k][i] > hi) { hi = pts[k][i]; }
        }
        if (npts == 0) { lo = 0; hi = 1; }
        if (hi - lo < 1e-9) { lo -= 0.5; hi += 0.5; }
        for (i = 0; i < npts; i++) {
            x1 = pr.x + (npts > 1 ? (double)i * (pr.w - 1) / (npts - 1) : 0);
            y1 = pr.y + pr.h - 1 - (pts[k][i] - lo) / (hi - lo) * (pr.h - 2);
            if (i > 0) {
                if (!cell) { lt_ui_draw_line(ui, x0, y0, x1, y1, lt_ui_px(ui, 4), (col & 0x00FFFFFFUL) | 0x28000000UL); }
                lt_ui_draw_line(ui, x0, y0, x1, y1, 1.5 * (double)lt_ui_scale_pct(ui) / 100.0, col);
            }
            x0 = x1; y0 = y1;
        }
        lt_fmt_double(vb, sizeof(vb), cur_vals != NULL ? cur_vals[k] : 0.0, 2);
        if (cell) {
            lt_snprintf(lab, sizeof(lab), "%s=%s %s", names[k], vb, units[k]);
            lt_ui_draw_text(ui, r.x + 6 + (k % 4) * (r.w / 4), r.y + 2, lab, col);
        } else {
            /* legend chip: dot, name, value */
            int nw = lt_vk_text_w(ui, (int)LT_FONT_SANS_MEDIUM, 11.0, names[k]);
            int vw = lt_vk_text_w(ui, (int)LT_FONT_MONO_MEDIUM, 11.0, vb);
            lt_ui_draw_circle_fill(ui, (double)lx + (double)lt_ui_px(ui, 4), (double)(r.y + lt_ui_px(ui, 15)), 3.5 * (double)lt_ui_scale_pct(ui) / 100.0, col);
            lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 12), r.y + lt_ui_px(ui, 4), nw + lt_ui_px(ui, 4), lt_ui_px(ui, 22)), (int)LT_FONT_SANS_MEDIUM, 11.0, names[k], t->text2, 0);
            lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 14) + nw, r.y + lt_ui_px(ui, 4), vw + lt_ui_px(ui, 4), lt_ui_px(ui, 22)), (int)LT_FONT_MONO_MEDIUM, 11.0, vb, col, 0);
            lt_vk_text(ui, lt_rect_make(lx + lt_ui_px(ui, 18) + nw + vw, r.y + lt_ui_px(ui, 4), lt_ui_px(ui, 60), lt_ui_px(ui, 22)), (int)LT_FONT_SANS, 10.0, units[k], t->muted, 0);
            lx += lt_ui_px(ui, 18 + 6 + 16) + nw + vw + lt_vk_text_w(ui, (int)LT_FONT_SANS, 10.0, units[k]);
        }
    }
    if (mk != NULL && mk_t1 > mk_t0) {
        long m;
        for (m = 0; m < mk->n_markers; m++) {
            double f = (mk->markers[m].t - mk_t0) / (mk_t1 - mk_t0);
            if (f >= 0.0 && f <= 1.0) {
                int mx = pr.x + (int)(f * (pr.w - 1));
                lt_ui_draw_vline(ui, mx, pr.y, pr.y + pr.h, 0xFFFFB74DUL);
                lt_ui_draw_text(ui, mx + 2, pr.y + 1, mk->markers[m].text, 0xFFFFB74DUL);
            }
        }
    }
    if (cursor_frac >= 0.0 && cursor_frac <= 1.0) {
        int cx = pr.x + (int)(cursor_frac * (pr.w - 1));
        lt_ui_draw_vline(ui, cx, pr.y, pr.y + pr.h, cell ? t->text : LT_RGBA(255, 255, 255, 150));
    }
    lt_ui_clip_pop(ui);
}

/* ---- viewer helpers ---- */
static long find_index(const lt_log *l, double t)
{
    long lo = 0, hi = l->n - 1, mid;
    if (l->n <= 0) { return 0; }
    if (t <= l->t[0]) { return 0; }
    if (t >= l->t[l->n - 1]) { return l->n - 1; }
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (l->t[mid] <= t) { lo = mid; } else { hi = mid - 1; }
    }
    return lo;
}

static void files_scan(lt_app *app, log_state *s)
{
    char **names = NULL;
    int n, i, k = 0;
    char *dir;
    dir = app->has_project ? lt_project_file(&app->project, "datalogs")
                           : lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "datalogs");
    lt_strlcpy(s->dir, dir, sizeof(s->dir));
    lt_free(dir);
    if (s->files != NULL) { lt_plat_free_list(s->files, s->n_files); }
    s->files = NULL; s->n_files = 0;
    s->files_loaded = 1;
    n = lt_plat_list_dir(s->dir, &names);
    if (n <= 0) { if (names != NULL) { lt_plat_free_list(names, n > 0 ? n : 0); } return; }
    /* keep only .mlg/.csv, newest (name sorted) first */
    s->files = (char **)lt_calloc((size_t)n, sizeof(char *));
    for (i = n - 1; i >= 0; i--) {
        if (lt_log_format_from_ext(names[i]) != LT_FMT_NONE) { s->files[k++] = lt_strdup(names[i]); }
    }
    s->n_files = k;
    lt_plat_free_list(names, n);
}

static const char *file_get(void *ud, int i) { return ((log_state *)ud)->files[i]; }

static int load_log(lt_app *app, log_state *s, const char *path)
{
    lt_log nl;
    lt_log_init(&nl);
    if (lt_log_read(path, &nl) < 0) {
        lt_snprintf(s->err, sizeof(s->err), "Cannot read %s: %s", lt_path_base(path), lt_err_get());
        lt_log_free(&nl);
        return -1;
    }
    s->n_math = 0;
    {
        lt_math_set *ms = lt_logx_math(app);
        if (ms->n > 0 && lt_log_channel_index(&nl, ms->ch[0].name) < 0) {
            int before = nl.n_ch;
            if (lt_math_apply_to_log(ms, &nl) == 0) { s->n_math = nl.n_ch - before; }
        }
    }
    if (s->have_log) { lt_log_free(&s->log); }
    s->log = nl;
    s->have_log = 1;
    lt_player_init(&s->pl, &s->log);
    lt_strlcpy(s->path, path, sizeof(s->path));
    s->cursor_t = 0.0; s->playing = 0; s->err[0] = '\0';
    sel_defaults(app, s);
    return 0;
}

/* ---- statistics panel (G-62) ---- */
static int stats_rows(const log_state *s) { return (s->n_sel > 0 ? s->n_sel : 1) + 1; }

static void stats_cell(lt_ui *ui, double v, lt_color col)
{
    char b[32];
    lt_fmt_double(b, sizeof(b), v, 2);
    lt_ui_label_ex(ui, b, col, LT_UI_ALIGN_RIGHT);
}

static void stats_draw(lt_app *app, log_state *s, double win_t0, double win_t1)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const hd[] = { "Channel", "Mean", "Median", "Std dev", "Min", "Max", "P25", "P75", "IQR" };
    int w[9], k, c, w1 = -1;
    for (c = 0; c < 9; c++) { w[c] = c == 0 ? CH * 18 : -1; }
    lt_ui_layout_row(ui, 9, w, 0);
    for (c = 0; c < 9; c++) { lt_ui_label_ex(ui, hd[c], t->muted, c == 0 ? LT_UI_ALIGN_LEFT : LT_UI_ALIGN_RIGHT); }
    for (k = 0; k < s->n_sel; k++) {
        lt_stats st;
        int rc;
        if (s->viewer) {
            if (s->have_log) {
                rc = lt_stats_log_channel(&s->log, s->sel_idx[k], find_index(&s->log, win_t0),
                                          find_index(&s->log, win_t1), &st);
            } else { rc = -1; }
        } else {
            rc = lt_stats_compute(s->hist[k], s->hn, &st);
        }
        lt_ui_layout_row(ui, 9, w, 0);
        lt_ui_label_ex(ui, chan_name(app, s, s->sel_idx[k]), palette[k], LT_UI_ALIGN_LEFT);
        if (rc != 0) {
            for (c = 1; c < 9; c++) { lt_ui_label_ex(ui, "-", t->muted, LT_UI_ALIGN_RIGHT); }
            continue;
        }
        stats_cell(ui, st.mean, t->text); stats_cell(ui, st.median, t->text); stats_cell(ui, st.stddev, t->text);
        stats_cell(ui, st.vmin, t->text); stats_cell(ui, st.vmax, t->text); stats_cell(ui, st.p25, t->text);
        stats_cell(ui, st.p75, t->text); stats_cell(ui, st.iqr, t->text);
    }
    if (s->n_sel == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Tick channels to see statistics", t->muted, LT_UI_ALIGN_LEFT);
    }
}

/* ---- main draw ---- */
static void log_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    log_state *s = (log_state *)tab->data;
    int w1 = -1, wc[2], w3[4], i, n, k;
    const char *picked;
    double now;
    const char *names[MAX_SEL], *units[MAX_SEL];
    double *pts[MAX_SEL];
    double curv[MAX_SEL];
    double cursor_frac = -1.0, win_t0 = 0.0, win_t1 = 1.0;
    int npts = 0;
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    LT_UNUSED(r);
    if (s == NULL) {
        s = (log_state *)lt_calloc(1, sizeof(*s));
        tab->data = s;
        s->viewer = strcmp(tab->kind, "datalog-viewer") == 0;
        s->sel_idx = (int *)lt_calloc(MAX_SEL, sizeof(int));
        s->speed_sel = 2;
        if (!s->viewer) { sel_defaults(app, s); }
        else { files_scan(app, s); }
    }
    now = lt_ui_now(ui);

    /* file dialog results */
    picked = lt_app_file_result(app, LT_DLG_FD_LOG_OPEN);
    if (picked != NULL && s->viewer) { load_log(app, s, picked); }
    picked = lt_app_file_result(app, LT_DLG_FD_LOG_EXPORT);
    if (picked != NULL && s->viewer && s->have_log) {
        if (lt_csv_write(picked, &s->log) == 0) { lt_app_toast(app, 1, "Exported %s", lt_path_base(picked)); }
        else { lt_app_toast(app, 3, "Export failed: %s", lt_err_get()); }
    }

    picked = lt_app_file_result(app, LT_DLG_FD_LOG_EXPORT_MLG);
    if (picked != NULL && s->viewer && s->have_log) {
        if (lt_mlg_write(picked, &s->log, NULL) == 0) { lt_app_toast(app, 1, "Exported %s", lt_path_base(picked)); }
        else { lt_app_toast(app, 3, "Export failed: %s", lt_err_get()); }
    }

    /* live sampling / playback clock */
    if (!s->viewer) {
        if (now - s->last_sample_ms >= 50.0 || s->last_sample_ms > now) {
            s->last_sample_ms = now;
            if (s->hn < HN) { s->hn++; }
            else { for (k = 0; k < MAX_SEL; k++) { memmove(&s->hist[k][0], &s->hist[k][1], (HN - 1) * sizeof(double)); } }
            for (k = 0; k < s->n_sel; k++) {
                double v = 0.0;
                if (online && app->ini != NULL) { lt_app_channel(app, app->ini->channels[s->sel_idx[k]]->name, &v); }
                s->hist[k][s->hn - 1] = v;
            }
        }
        lt_ui_request_redraw(ui);
    } else if (s->have_log) {
        double dt = s->last_ms > 0 && now >= s->last_ms ? (now - s->last_ms) / 1000.0 : 0.0;
        s->last_ms = now;
        if (s->playing) {
            lt_player_set_rate(&s->pl, speed_v[s->speed_sel]);
            lt_player_set_loop(&s->pl, s->loop ? LT_TRUE : LT_FALSE);
            if (!lt_player_advance(&s->pl, dt)) { s->playing = 0; }
            s->cursor_t = lt_player_time(&s->pl);
            lt_ui_request_redraw(ui);
        }
    }

    wc[0] = CH * 34; wc[1] = -1;
    lt_ui_layout_row(ui, 2, wc, -1);
    /* ---------------- left column: controls + picker ---------------- */
    lt_ui_layout_column_begin(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, s->viewer ? "Datalog Viewer" : "Data Logging");
    if (!s->viewer) {
        int recording = app->rec != NULL && lt_rec_is_recording(app->rec);
        char b[200];
        w3[0] = -1; w3[1] = -1;
        lt_ui_layout_row(ui, 2, w3, 0);
        if (lt_ui_button_ex(ui, recording ? "Stop recording" : "Start recording",
                            recording ? LT_UI_BTN_DANGER : (online ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED))) {
            lt_app_log_toggle(app);
        }
        if (lt_ui_button_ex(ui, "Add marker", recording ? 0 : LT_UI_BTN_DISABLED) && recording) {
            lt_rec_marker(app->rec, "marker");
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (recording) {
            lt_snprintf(b, sizeof(b), "REC  %ld rows  %.1f s", lt_rec_count(app->rec), lt_rec_duration(app->rec));
            lt_ui_label_ex(ui, b, t->error, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, lt_path_base(lt_rec_path(app->rec)), t->muted, LT_UI_ALIGN_LEFT);
        } else if (!online) {
            lt_ui_label_wrap(ui, "Connect to an ECU (or use the demo) to record and see live data.", t->muted);
        } else if (app->rec != NULL && lt_rec_path(app->rec)[0] != '\0') {
            lt_snprintf(b, sizeof(b), "Last: %s", lt_path_base(lt_rec_path(app->rec)));
            lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
        }
        {
            lt_logx *lx = lt_logx_get(app);
            double hz = (double)lx->rate_hz;
            int wr[4], ch;
            wr[0] = 110; wr[1] = 90; wr[2] = -1;
            lt_ui_layout_row(ui, 3, wr, 0);
            lt_ui_label(ui, "Sample rate");
            if (lt_ui_number(ui, "rate", &hz, 1.0, 200.0, 1.0, 0)) { lx->rate_hz = (int)hz; lt_logx_save(app); }
            lt_ui_label_ex(ui, "Hz", t->muted, LT_UI_ALIGN_LEFT);
            wr[0] = -1; wr[1] = -1;
            lt_ui_layout_row(ui, 2, wr, 0);
            ch = lx->auto_rec;
            if (lt_ui_checkbox(ui, "Auto-record", &ch)) { lx->auto_rec = ch; lt_logx_save(app); }
            if (lx->auto_rec) {
                char kb[48];
                lt_snprintf(kb, sizeof(kb), "key [%s]  >= %d rpm, %d s", lx->key.on ? "on" : "off",
                            lx->key_rpm, lx->key_timeout);
                lt_ui_label_ex(ui, kb, lx->key.on ? t->success : t->muted, LT_UI_ALIGN_LEFT);
            } else { lt_ui_label(ui, ""); }
            wr[0] = -1; wr[1] = -1;
            lt_ui_layout_row(ui, 2, wr, 0);
            if (lt_ui_button_ex(ui, "Channels...", recording ? LT_UI_BTN_DISABLED : 0) && !recording) {
                (void)lt_app_dialog_push(app, "rec-channels", "", "Recording channels");
            }
            lt_ui_checkbox(ui, "Stats", &s->show_stats);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lx->n_off > 0) {
                char kb[64];
                lt_snprintf(kb, sizeof(kb), "%lu channel%s excluded from recording", (unsigned long)lx->n_off,
                            lx->n_off == 1 ? "" : "s");
                lt_ui_label_ex(ui, kb, t->muted, LT_UI_ALIGN_LEFT);
            }
        }
        lt_ui_label_wrap(ui, "Recording stores the selected datalog channels; the chart "
                         "shows the channels ticked below.", t->muted);
    } else {
        w3[0] = -1; w3[1] = -1;
        lt_ui_layout_row(ui, 2, w3, 0);
        if (lt_ui_button(ui, "Open file...")) {
            lt_app_file_dialog(app, LT_DLG_FD_LOG_OPEN, LT_UI_FILE_OPEN, s->dir[0] ? s->dir : NULL, "",
                               "Datalogs|*.mlg;*.csv|All files|*");
        }
        if (lt_ui_button_ex(ui, "Export CSV...", s->have_log ? 0 : LT_UI_BTN_DISABLED) && s->have_log) {
            char nm[300];
            lt_snprintf(nm, sizeof(nm), "%s", s->path[0] ? lt_path_base(s->path) : "log.csv");
            if (lt_str_ends(nm, ".mlg")) { nm[strlen(nm) - 4] = '\0'; lt_strlcpy(nm + strlen(nm), ".csv", 5); }
            lt_app_file_dialog(app, LT_DLG_FD_LOG_EXPORT, LT_UI_FILE_SAVE, s->dir[0] ? s->dir : NULL, nm,
                               "CSV|*.csv");
        }
        lt_ui_layout_row(ui, 2, w3, 0);
        if (lt_ui_button_ex(ui, "Export MLG...", s->have_log ? 0 : LT_UI_BTN_DISABLED) && s->have_log) {
            char nm[300];
            lt_snprintf(nm, sizeof(nm), "%s", s->path[0] ? lt_path_base(s->path) : "log.mlg");
            if (lt_str_ends(nm, ".csv")) { nm[strlen(nm) - 4] = '\0'; lt_strlcpy(nm + strlen(nm), ".mlg", 5); }
            lt_app_file_dialog(app, LT_DLG_FD_LOG_EXPORT_MLG, LT_UI_FILE_SAVE, s->dir[0] ? s->dir : NULL, nm,
                               "MegaLogViewer|*.mlg");
        }
        lt_ui_checkbox(ui, "Stats", &s->show_stats);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Logs in the project datalogs folder", t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 16 * 5);
        if (s->n_files > 0) {
            int res = lt_ui_listbox_cb(ui, "files", &s->file_sel, s->n_files, file_get, s);
            if ((res & LT_UI_ACTIVATED) && s->file_sel >= 0 && s->file_sel < s->n_files) {
                char *p = lt_path_join(s->dir, s->files[s->file_sel]);
                load_log(app, s, p);
                lt_free(p);
            }
        } else {
            lt_ui_label_ex(ui, "(none found)", t->muted, LT_UI_ALIGN_LEFT);
        }
        if (s->n_files > 0 && s->file_sel >= 0 && s->file_sel < s->n_files) {
            if (s->rename_on) {
                int wr[2];
                wr[0] = -1; wr[1] = CH * 10;
                lt_ui_layout_row(ui, 2, wr, 0);
                lt_ui_text(ui, "rn", s->rename_buf, sizeof(s->rename_buf), 0);
                if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_PRIMARY)) {
                    char *from = lt_path_join(s->dir, s->files[s->file_sel]);
                    char nm[160], *to;
                    lt_strlcpy(nm, s->rename_buf, sizeof(nm));
                    if (lt_log_format_from_ext(nm) == LT_FMT_NONE) {
                        const char *ext = lt_path_ext(s->files[s->file_sel]);
                        lt_strlcpy(nm + strlen(nm), ext != NULL ? ext : ".mlg", sizeof(nm) - strlen(nm));
                    }
                    to = lt_path_join(s->dir, nm);
                    if (nm[0] == '\0' || lt_file_exists(to)) { lt_app_toast(app, 2, "Cannot rename to %s", nm); }
                    else if (lt_plat_rename(from, to) == 0) {
                        if (strcmp(s->path, from) == 0) { lt_strlcpy(s->path, to, sizeof(s->path)); }
                        lt_app_toast(app, 1, "Renamed to %s", nm);
                        s->rename_on = 0;
                        files_scan(app, s);
                    } else { lt_app_toast(app, 3, "Rename failed: %s", lt_err_get()); }
                    lt_free(from); lt_free(to);
                }
            } else {
                int wr[2];
                wr[0] = -1; wr[1] = -1;
                lt_ui_layout_row(ui, 2, wr, 0);
                if (lt_ui_button(ui, "Rename")) {
                    lt_strlcpy(s->rename_buf, s->files[s->file_sel], sizeof(s->rename_buf));
                    s->rename_on = 1; s->del_confirm = 0;
                }
                if (lt_ui_button_ex(ui, s->del_confirm == s->file_sel + 1 ? "Click again to delete" : "Delete", LT_UI_BTN_DANGER)) {
                    if (s->del_confirm != s->file_sel + 1) { s->del_confirm = s->file_sel + 1; }
                    else {
                        char *p = lt_path_join(s->dir, s->files[s->file_sel]);
                        if (lt_plat_remove(p) == 0) { lt_app_toast(app, 0, "Deleted %s", s->files[s->file_sel]); }
                        else { lt_app_toast(app, 3, "Delete failed: %s", lt_err_get()); }
                        lt_free(p);
                        s->del_confirm = 0;
                        files_scan(app, s);
                        if (s->file_sel >= s->n_files) { s->file_sel = s->n_files - 1; }
                    }
                }
            }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->err[0] != '\0') { lt_ui_label_wrap(ui, s->err, t->error); }
        else if (s->have_log) {
            char b[200];
            lt_snprintf(b, sizeof(b), "%s: %ld rows, %.1f s, %d channels", lt_path_base(s->path),
                        s->log.n, lt_log_duration(&s->log), s->log.n_ch);
            lt_ui_label_wrap(ui, b, t->text2);
        }
    }
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text(ui, "filter", s->filter, sizeof(s->filter), 0);
    {
        char b[64];
        lt_snprintf(b, sizeof(b), "Channels (%d of %d shown, max %d)", s->n_sel, n_channels(app, s), MAX_SEL);
        lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
    }
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "picker")) {
        n = n_channels(app, s);
        lt_ui_layout_row(ui, 1, &w1, 0);
        for (i = 0; i < n; i++) {
            const char *nm = chan_name(app, s, i);
            int on, k2;
            if (!lt_dlg_contains_ci(nm, s->filter)) { continue; }
            k2 = sel_find(s, i);
            on = k2 >= 0;
            lt_ui_push_id_int(ui, i);
            if (lt_ui_checkbox(ui, nm, &on)) { sel_toggle(s, i, on); }
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
    lt_ui_layout_column_end(ui);

    /* ---------------- right column: chart + transport ---------------- */
    lt_ui_layout_column_begin(ui);
    if (s->viewer) {
        int w6[7];
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->have_log) {
            double dur = lt_log_duration(&s->log), tt = s->cursor_t;
            char b[64];
            w6[0] = CH * 8; w6[1] = CH * 8; w6[2] = CH * 8; w6[3] = CH * 12; w6[4] = CH * 11; w6[5] = -1;
            lt_ui_layout_row(ui, 6, w6, 0);
            if (lt_ui_button_ex(ui, s->playing ? "Pause" : "Play", LT_UI_BTN_PRIMARY)) {
                if (!s->playing && s->cursor_t >= dur - 1e-6) { lt_player_seek(&s->pl, 0); s->cursor_t = 0.0; }
                s->playing = !s->playing;
                s->last_ms = now;
            }
            if (lt_ui_button(ui, "Start")) { lt_player_seek(&s->pl, 0); s->cursor_t = 0.0; s->playing = 0; }
            lt_ui_combo(ui, "speed", &s->speed_sel, speeds, 6);
            lt_ui_combo(ui, "zoom", &s->zoom_sel, zooms, 5);
            lt_ui_checkbox(ui, "Loop", &s->loop);
            if (app->conn == LT_APP_OFFLINE && app->rt != NULL) { lt_ui_checkbox(ui, "Drive gauges", &s->drive); }
            else { lt_ui_label(ui, ""); }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_slider(ui, "timeline", &tt, 0.0, dur > 0.0 ? dur : 1.0, 2)) {
                s->cursor_t = tt;
                lt_player_seek_time(&s->pl, tt);
            }
            lt_snprintf(b, sizeof(b), "%.2f / %.2f s", s->cursor_t, dur);
            lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_RIGHT);
            if (s->drive && app->rt != NULL && app->ini != NULL) { lt_player_apply_to_rt(&s->pl, app->ini, app->rt); }
        } else {
            lt_ui_label_ex(ui, "Open a .mlg or .csv log to review it.", t->muted, LT_UI_ALIGN_LEFT);
        }
    } else {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Live view: about the last 20 seconds, each trace scaled to its own range.",
                       t->muted, LT_UI_ALIGN_LEFT);
    }
    lt_ui_layout_row(ui, 1, &w1, s->show_stats ? -(stats_rows(s) * (lt_ui_cell_mode(ui) ? 16 : 30) + 12) : -1);
    {
        lt_rect cr = lt_ui_layout_next(ui);
        double *store = (double *)lt_calloc((size_t)(MAX_SEL * MAXPTS), sizeof(double));
        int ok = 1;
        for (k = 0; k < s->n_sel && k < MAX_SEL; k++) {
            pts[k] = store + (size_t)k * MAXPTS;
            names[k] = chan_name(app, s, s->sel_idx[k]);
            units[k] = chan_units(app, s, s->sel_idx[k]);
            curv[k] = 0.0;
        }
        if (!s->viewer) {
            npts = s->hn;
            for (k = 0; k < s->n_sel; k++) {
                for (i = 0; i < npts; i++) { pts[k][i] = s->hist[k][i]; }
                curv[k] = npts > 0 ? s->hist[k][npts - 1] : 0.0;
            }
        } else if (s->have_log && s->log.n > 0) {
            double dur = lt_log_duration(&s->log), win = zoom_v[s->zoom_sel];
            if (win <= 0.0 || win >= dur) { win_t0 = 0.0; win_t1 = dur > 0 ? dur : 1.0; }
            else {
                win_t0 = s->cursor_t - win / 2.0;
                if (win_t0 < 0.0) { win_t0 = 0.0; }
                if (win_t0 + win > dur) { win_t0 = dur - win; }
                win_t1 = win_t0 + win;
            }
            npts = LT_CLAMP(cr.w - 8, 2, MAXPTS);
            for (i = 0; i < npts; i++) {
                long idx = find_index(&s->log, win_t0 + (win_t1 - win_t0) * i / (npts - 1));
                for (k = 0; k < s->n_sel; k++) { pts[k][i] = LT_LOG_AT(&s->log, idx, s->sel_idx[k]); }
            }
            {
                long ci = find_index(&s->log, s->cursor_t);
                for (k = 0; k < s->n_sel; k++) { curv[k] = LT_LOG_AT(&s->log, ci, s->sel_idx[k]); }
            }
            cursor_frac = (s->cursor_t - win_t0) / (win_t1 - win_t0);
            /* click / drag on the chart scrubs */
            if (lt_ui_mouse_down(ui, LT_MB_LEFT) && !lt_ui_popup_open(ui)) {
                lt_point m = lt_ui_mouse(ui);
                if (m.x >= cr.x && m.x < cr.x + cr.w && m.y >= cr.y && m.y < cr.y + cr.h) {
                    double f = (double)(m.x - cr.x - 4) / (double)(cr.w - 8);
                    f = LT_CLAMP(f, 0.0, 1.0);
                    s->cursor_t = win_t0 + f * (win_t1 - win_t0);
                    lt_player_seek_time(&s->pl, s->cursor_t);
                }
            }
        } else { ok = 0; }
        if (ok) {
            chart_draw(ui, cr, s->n_sel, names, units, pts, npts, curv, cursor_frac,
                       s->viewer && s->have_log ? &s->log : NULL, win_t0, win_t1);
        } else { chart_draw(ui, cr, 0, names, units, pts, 0, curv, -1.0, NULL, 0.0, 1.0); }
        lt_free(store);
    }
    if (s->show_stats) { stats_draw(app, s, win_t0, win_t1); }
    lt_ui_layout_column_end(ui);
}

static void log_close(lt_app *app, lt_app_tab *tab)
{
    log_state *s = (log_state *)tab->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->have_log) { lt_log_free(&s->log); }
        if (s->files != NULL) { lt_plat_free_list(s->files, s->n_files); }
        lt_free(s->sel_idx);
        lt_free(s);
    }
    tab->data = NULL;
}

void lt_dlg_register_log(lt_app *app)
{
    lt_app_tab_kind tk;
    lt_datalog_set_rt_setter(lt_ini_rt_set);
    tk.draw = log_draw; tk.close = log_close;
    tk.kind = "datalog"; lt_app_register_tab_kind(app, &tk);
    tk.kind = "datalog-viewer"; lt_app_register_tab_kind(app, &tk);
}
