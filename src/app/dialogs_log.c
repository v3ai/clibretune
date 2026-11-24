/* dialogs_log.c - Data Logging tab (live graph + recording) and the Datalog Viewer (MLG / MSL /
 * CSV playback).  Both show MegaLogViewer-style graph pages of stacked panes (log_graph.c) whose
 * setup is edited in the left column and stored as JSON (graph_setup.json in the project, else
 * the config dir).  The viewer adds the scatter plot and histogram / table generator (log_xy.c).
 * The live tab keeps its own in-memory history of every channel (INI + live math channels).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dialogs.h"
#include "view_kit.h"
#include "feat_log.h"
#include "log_view.h"

#define CH 8
#define LT_DLG_FD_LOG_EXPORT_ANY (LT_APP_FD_USER0 + 85)
#define LIVE_MAX 2400       /* live history rows kept (2 min at 20 Hz) */
#define LIVE_DROP 400       /* rows dropped at once when full */
#define LIVE_DT 50.0        /* live sampling period, ms */

typedef struct log_state {
    int viewer;
    char filter[64];
    /* graph setup */
    lt_graph_setup gs;
    int page, trace_sel, gs_ready;
    lt_lg_view view;
    int follow;                          /* live: the window follows the newest sample */
    double span;                         /* live window length, s */
    /* live history */
    lt_log live;
    int live_ok;
    size_t live_nini, live_nvirt;
    double live_t0, last_sample_ms;
    /* viewer */
    int log_gen;
    lt_player pl;
    int playing, speed_sel, loop, drive;
    double last_ms;
    char **files; int n_files, file_sel, files_loaded;
    unsigned files_gen;                  /* app->log_files_gen at the last scan */
    int last_frame;                      /* ui frame this tab was last drawn in */
    char dir[512];
    char err[160];
    int show_stats;
    int rename_on, del_confirm;
    char rename_buf[128];
    int mode;                            /* viewer: 0 graph, 1 scatter, 2 histogram */
    lt_lxy xy;
} log_state;

static const char *const speeds[] = { "0.25x", "0.5x", "1x", "2x", "4x", "8x" };
static const double speed_v[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
static const char *const modes[] = { "Graph", "Scatter plot", "Histogram / table" };

/* the log the graph shows: the loaded datalog (viewer) or the live history */
static const lt_log *src_log(lt_app *app, log_state *s)
{
    if (s->viewer) { return lt_logx_shared_log(app); }
    return s->live_ok ? &s->live : NULL;
}

/* ---- graph setup persistence ---- */
static void gs_save(lt_app *app, log_state *s)
{
    char *p = lt_logx_graph_path(app);
    (void)lt_plat_mkdirs(app->has_project ? app->project.path : app->config_dir);
    if (lt_graph_save(p, &s->gs) != 0) { lt_app_toast(app, 3, "Graph setup not saved: %s", lt_err_get()); }
    lt_free(p);
}

static void gs_defaults(lt_app *app, log_state *s)
{
    const lt_log *L = src_log(app, s);
    lt_graph_defaults(&s->gs, L != NULL ? (const char *const *)L->names : NULL, L != NULL ? L->n_ch : 0);
    s->page = 0; s->trace_sel = 0; s->view.sel_pane = 0;
}

static int gs_load(lt_app *app, log_state *s)
{
    char *p = lt_logx_graph_path(app);
    int rc = lt_graph_load(p, &s->gs);
    lt_free(p);
    if (rc != 0 || s->gs.n_pages == 0) { return -1; }
    s->page = 0; s->trace_sel = 0; s->view.sel_pane = 0;
    return 0;
}

/* setup available once the channel names are known (defaults need them) */
static void gs_ensure(lt_app *app, log_state *s)
{
    if (s->gs_ready || src_log(app, s) == NULL) { return; }
    if (gs_load(app, s) != 0) { gs_defaults(app, s); }
    s->gs_ready = 1;
}

static lt_graph_page *cur_page(log_state *s)
{
    if (s->gs.n_pages == 0) { (void)lt_graph_add_page(&s->gs, "Main"); }
    if (s->page >= s->gs.n_pages) { s->page = s->gs.n_pages - 1; }
    if (s->page < 0) { s->page = 0; }
    return &s->gs.pages[s->page];
}

static lt_graph_pane *cur_pane(log_state *s)
{
    lt_graph_page *pg = cur_page(s);
    if (pg->n_panes == 0) { (void)lt_graph_add_pane(pg); }
    if (s->view.sel_pane >= pg->n_panes) { s->view.sel_pane = pg->n_panes - 1; }
    if (s->view.sel_pane < 0) { s->view.sel_pane = 0; }
    return &pg->pane[s->view.sel_pane];
}

/* ---- live history ---- */
static void live_build(lt_app *app, log_state *s)
{
    size_t i, nv = lt_datalog_virtual_count(), n = app->ini->n_channels, k = 0;
    const char **nm = (const char **)lt_calloc(n + nv + 1, sizeof(char *));
    const char **un = (const char **)lt_calloc(n + nv + 1, sizeof(char *));
    for (i = 0; i < n; i++) {
        nm[k] = app->ini->channels[i]->name;
        un[k] = app->ini->channels[i]->units != NULL ? app->ini->channels[i]->units : "";
        k++;
    }
    for (i = 0; i < nv; i++) {
        if (lt_ini_find_channel(app->ini, lt_datalog_virtual_name(i)) != NULL) { continue; }
        nm[k] = lt_datalog_virtual_name(i);
        un[k] = lt_datalog_virtual_units(i);
        k++;
    }
    lt_log_set_channels(&s->live, nm, un, (int)k);
    lt_free((void *)nm);
    lt_free((void *)un);
    s->live_ok = 1;
    s->live_nini = n;
    s->live_nvirt = nv;
    s->live_t0 = lt_ui_now(app->ui);
}

static void live_clear(lt_app *app, log_state *s)
{
    if (s->live_ok) { lt_log_free(&s->live); s->live_ok = 0; }
    if (app->ini != NULL) { live_build(app, s); }
    s->view.has_cursor = 0;
    s->follow = 1;
}

static void live_sample(lt_app *app, log_state *s, double now)
{
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    double *row, t;
    int c;
    if (app->ini == NULL) { return; }
    if (!s->live_ok || s->live_nini != app->ini->n_channels || s->live_nvirt != lt_datalog_virtual_count()) {
        if (s->live_ok) { lt_log_free(&s->live); s->live_ok = 0; }
        live_build(app, s);
        s->gs_ready = s->gs_ready && s->live.n_ch > 0;
    }
    if (!online || app->rt == NULL) { return; }
    if (now - s->last_sample_ms < LIVE_DT && s->last_sample_ms <= now) { return; }
    s->last_sample_ms = now;
    if (s->live.n >= LIVE_MAX) {      /* drop the oldest rows */
        long keep = s->live.n - LIVE_DROP;
        memmove(s->live.t, s->live.t + LIVE_DROP, (size_t)keep * sizeof(double));
        memmove(s->live.v, s->live.v + (size_t)LIVE_DROP * (size_t)s->live.n_ch,
                (size_t)keep * (size_t)s->live.n_ch * sizeof(double));
        s->live.n = keep;
    }
    row = (double *)lt_malloc((size_t)(s->live.n_ch > 0 ? s->live.n_ch : 1) * sizeof(double));
    for (c = 0; c < s->live.n_ch; c++) {
        double v = 0.0;
        if ((size_t)c < s->live_nini) {
            if (lt_ini_rt_valid(app->rt, (size_t)c)) { v = lt_ini_rt_value(app->rt, (size_t)c); }
        } else if (!lt_logx_live_math(app, s->live.names[c], &v)) { v = 0.0; }
        row[c] = lt_isfinite(v) ? v : 0.0;
    }
    t = (now - s->live_t0) / 1000.0;
    if (s->live.n > 0 && t < s->live.t[s->live.n - 1]) { t = s->live.t[s->live.n - 1]; }
    lt_log_append(&s->live, t, row);
    lt_free(row);
}

/* ---- viewer helpers ---- */
static void files_scan(lt_app *app, log_state *s)
{
    char **names = NULL;
    int n, i, k = 0;
    char *dir, keep[256];
    keep[0] = '\0';
    if (s->files != NULL && s->file_sel >= 0 && s->file_sel < s->n_files) {
        lt_strlcpy(keep, s->files[s->file_sel], sizeof(keep));
    }
    s->files_gen = app->log_files_gen;
    dir = app->has_project ? lt_project_file(&app->project, "datalogs")
                           : lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "datalogs");
    lt_strlcpy(s->dir, dir, sizeof(s->dir));
    lt_free(dir);
    if (s->files != NULL) { lt_plat_free_list(s->files, s->n_files); }
    s->files = NULL; s->n_files = 0;
    s->files_loaded = 1;
    n = lt_plat_list_dir(s->dir, &names);
    if (n <= 0) { if (names != NULL) { lt_plat_free_list(names, n > 0 ? n : 0); } return; }
    /* keep only logs (.mlg/.msl/.csv), newest (name sorted) first */
    s->files = (char **)lt_calloc((size_t)n, sizeof(char *));
    for (i = n - 1; i >= 0; i--) {
        if (lt_log_format_from_ext(names[i]) != LT_FMT_NONE) { s->files[k++] = lt_strdup(names[i]); }
    }
    s->n_files = k;
    lt_plat_free_list(names, n);
    /* the same file stays selected when others come or go */
    for (i = 0; keep[0] != '\0' && i < k; i++) {
        if (strcmp(s->files[i], keep) == 0) { s->file_sel = i; break; }
    }
    if (s->file_sel >= k) { s->file_sel = k - 1; }
}

/* a cheap rescan of the datalogs folder when a recording started or ended, and when the viewer
 * comes back into view (another tab was shown, files may have been added or removed meanwhile) */
static void files_refresh(lt_app *app, log_state *s)
{
    int fr = lt_ui_frame_no(app->ui);
    if (s->files_gen != app->log_files_gen || (s->last_frame != 0 && fr - s->last_frame > 1)) {
        files_scan(app, s);
    }
    s->last_frame = fr;
}

static const char *file_get(void *ud, int i) { return ((log_state *)ud)->files[i]; }

/* the shared log was (re)loaded: reset the view */
static void viewer_sync(lt_app *app, log_state *s)
{
    lt_logx *x = lt_logx_get(app);
    lt_log *L = lt_logx_shared_log(app);
    if (L == NULL || s->log_gen == x->shared_gen) { return; }
    s->log_gen = x->shared_gen;
    lt_player_init(&s->pl, L);
    s->playing = 0;
    s->view.t0 = 0.0;
    s->view.t1 = lt_log_duration(L) > 0.0 ? lt_log_duration(L) : 1.0;
    s->view.cursor_t = 0.0;
    s->view.has_cursor = 1;
    s->err[0] = '\0';
    gs_ensure(app, s);
}

static int load_log(lt_app *app, log_state *s, const char *path)
{
    if (lt_logx_load_log(app, path, s->err, sizeof(s->err)) != 0) { return -1; }
    viewer_sync(app, s);
    return 0;
}

/* ---- statistics of the selected pane's channels (G-62) ---- */
static int stats_rows(log_state *s) { return (s->gs.n_pages > 0 ? cur_pane(s)->n : 0) + 2; }

static void stats_cell(lt_ui *ui, double v, lt_color col)
{
    char b[32];
    lt_fmt_double(b, sizeof(b), v, 2);
    lt_ui_label_ex(ui, b, col, LT_UI_ALIGN_RIGHT);
}

static void stats_draw(lt_app *app, log_state *s, const lt_log *L, double t0, double t1)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const hd[] = { "Channel", "Mean", "Median", "Std dev", "Min", "Max", "P25", "P75", "IQR" };
    lt_graph_pane *pn = cur_pane(s);
    int w[9], k, c, w1 = -1;
    for (c = 0; c < 9; c++) { w[c] = c == 0 ? CH * 18 : -1; }
    lt_ui_layout_row(ui, 9, w, 0);
    for (c = 0; c < 9; c++) { lt_ui_label_ex(ui, hd[c], t->muted, c == 0 ? LT_UI_ALIGN_LEFT : LT_UI_ALIGN_RIGHT); }
    for (k = 0; k < pn->n; k++) {
        lt_stats st;
        int rc = -1, ci = L != NULL ? lt_log_channel_index(L, pn->tr[k].channel) : -1;
        memset(&st, 0, sizeof(st));
        if (ci >= 0 && L->n > 0) { rc = lt_stats_log_channel(L, ci, lt_lg_find(L, t0), lt_lg_find(L, t1), &st); }
        lt_ui_layout_row(ui, 9, w, 0);
        lt_ui_label_ex(ui, pn->tr[k].channel, lt_lg_palette[k % LT_LG_COLORS], LT_UI_ALIGN_LEFT);
        if (rc != 0) {
            for (c = 1; c < 9; c++) { lt_ui_label_ex(ui, "-", t->muted, LT_UI_ALIGN_RIGHT); }
            continue;
        }
        {
            lt_uconv uc;
            double sc;
            lt_uconv_make(&uc, L->units != NULL ? L->units[ci] : "");
            sc = lt_uconv_to_disp(&uc, 1.0) - lt_uconv_to_disp(&uc, 0.0);   /* spreads scale only */
            if (sc < 0.0) { sc = -sc; }
            st.mean = lt_uconv_to_disp(&uc, st.mean); st.median = lt_uconv_to_disp(&uc, st.median);
            st.vmin = lt_uconv_to_disp(&uc, st.vmin); st.vmax = lt_uconv_to_disp(&uc, st.vmax);
            st.p25 = lt_uconv_to_disp(&uc, st.p25); st.p75 = lt_uconv_to_disp(&uc, st.p75);
            st.stddev *= sc; st.iqr *= sc;
        }
        stats_cell(ui, st.mean, t->text); stats_cell(ui, st.median, t->text); stats_cell(ui, st.stddev, t->text);
        stats_cell(ui, st.vmin, t->text); stats_cell(ui, st.vmax, t->text); stats_cell(ui, st.p25, t->text);
        stats_cell(ui, st.p75, t->text); stats_cell(ui, st.iqr, t->text);
    }
    if (pn->n == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "The selected pane has no channels", t->muted, LT_UI_ALIGN_LEFT);
    }
}

/* ---- left column: graph setup editor ---- */
static const char *page_get(void *ud, int i) { return ((log_state *)ud)->gs.pages[i].name; }

static void setup_editor(lt_app *app, log_state *s, const lt_log *L)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_graph_page *pg;
    lt_graph_pane *pn;
    int w1 = -1, w[6], k, changed = 0;
    char b[96];
    if (!s->gs_ready) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->viewer ? "Open a log to set up the graph." : "Connect (or start the demo) to graph live data.", t->muted);
        return;
    }
    pg = cur_page(s);
    pn = cur_pane(s);
    /* pages */
    w[0] = 44; w[1] = -1; w[2] = 30; w[3] = 30;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label_ex(ui, "Page", t->text2, LT_UI_ALIGN_LEFT);
    k = s->page;
    if (lt_ui_combo_cb(ui, "gpage", &k, s->gs.n_pages, page_get, s)) { s->page = k; s->view.sel_pane = 0; s->trace_sel = 0; }
    if (lt_ui_button_ex(ui, "+", s->gs.n_pages < LT_GRAPH_MAX_PAGES ? 0 : LT_UI_BTN_DISABLED)) {
        k = lt_graph_add_page(&s->gs, NULL);
        if (k >= 0) { s->page = k; (void)lt_graph_add_pane(&s->gs.pages[k]); s->view.sel_pane = 0; changed = 1; }
    }
    if (lt_ui_button_ex(ui, "-", s->gs.n_pages > 1 ? 0 : LT_UI_BTN_DISABLED) && s->gs.n_pages > 1) {
        lt_graph_remove_page(&s->gs, s->page);
        s->page = s->page > 0 ? s->page - 1 : 0;
        changed = 1;
    }
    pg = cur_page(s);
    w[0] = 44; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label_ex(ui, "Name", t->text2, LT_UI_ALIGN_LEFT);
    if (lt_ui_text(ui, "gpname", pg->name, sizeof(pg->name), 0) & LT_UI_CHANGED) { changed = 1; }
    /* panes */
    w[0] = -1; w[1] = 30; w[2] = 30; w[3] = 30; w[4] = 30;
    lt_ui_layout_row(ui, 5, w, 0);
    lt_snprintf(b, sizeof(b), "Pane %d of %d", s->view.sel_pane + 1, pg->n_panes);
    lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
    if (lt_ui_button(ui, "<") && s->view.sel_pane > 0) { s->view.sel_pane--; s->trace_sel = 0; }
    if (lt_ui_button(ui, ">") && s->view.sel_pane + 1 < pg->n_panes) { s->view.sel_pane++; s->trace_sel = 0; }
    if (lt_ui_button_ex(ui, "+", pg->n_panes < LT_GRAPH_MAX_PANES ? 0 : LT_UI_BTN_DISABLED)) {
        k = lt_graph_add_pane(pg);
        if (k >= 0) { s->view.sel_pane = k; s->trace_sel = 0; changed = 1; }
    }
    if (lt_ui_button_ex(ui, "-", pg->n_panes > 1 ? 0 : LT_UI_BTN_DISABLED) && pg->n_panes > 1) {
        lt_graph_remove_pane(pg, s->view.sel_pane);
        changed = 1;
    }
    pn = cur_pane(s);
    /* traces of the pane */
    if (s->trace_sel >= pn->n) { s->trace_sel = pn->n - 1; }
    if (s->trace_sel < 0) { s->trace_sel = 0; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    for (k = 0; k < pn->n; k++) {
        lt_ui_push_id_int(ui, k);
        lt_snprintf(b, sizeof(b), "%s %s%s", k == s->trace_sel ? ">" : " ", pn->tr[k].channel, pn->tr[k].right ? "  (right axis)" : "");
        if (lt_ui_button_ex(ui, b, LT_UI_BTN_FLAT | (k == s->trace_sel ? LT_UI_BTN_SELECTED : 0))) { s->trace_sel = k; }
        lt_ui_pop_id(ui);
    }
    if (pn->n > 0) {
        lt_graph_trace *tr = &pn->tr[s->trace_sel];
        static const char *const sides[] = { "Left", "Right" };
        w[0] = 56; w[1] = -1; w[2] = -1; w[3] = 64;
        lt_ui_layout_row(ui, 4, w, 0);
        if (lt_ui_checkbox(ui, "Auto", &tr->autoscale)) { changed = 1; }
        lt_ui_disabled_begin(ui, tr->autoscale ? LT_TRUE : LT_FALSE);
        if (lt_ui_number(ui, "gmin", &tr->lo, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { changed = 1; }
        if (lt_ui_number(ui, "gmax", &tr->hi, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_disabled_end(ui);
        k = tr->right;
        if (lt_ui_combo(ui, "gside", &k, sides, 2)) { tr->right = k; changed = 1; }
        w[0] = -1; w[1] = 80;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label_ex(ui, tr->autoscale ? "Scale: automatic" : "Scale: fixed min / max", t->muted, LT_UI_ALIGN_LEFT);
        if (lt_ui_button(ui, "Remove")) { lt_graph_pane_remove(pn, s->trace_sel); changed = 1; }
    }
    /* channel picker: ticked = plotted in the selected pane */
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text(ui, "filter", s->filter, sizeof(s->filter), 0);
    lt_snprintf(b, sizeof(b), "Channels in pane %d (%d of max %d)", s->view.sel_pane + 1, pn->n, LT_GRAPH_MAX_TRACES);
    lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, -(lt_ui_cell_mode(ui) ? 16 : 34));
    if (lt_ui_scroll_begin(ui, "picker")) {
        int n = L != NULL ? L->n_ch : 0, i;
        lt_ui_layout_row(ui, 1, &w1, 0);
        for (i = 0; i < n; i++) {
            const char *nm = L->names[i];
            int on, k2;
            if (!lt_dlg_contains_ci(nm, s->filter)) { continue; }
            k2 = lt_graph_pane_find(pn, nm);
            on = k2 >= 0;
            lt_ui_push_id_int(ui, i);
            if (lt_ui_checkbox(ui, nm, &on)) {
                if (on) { k2 = lt_graph_pane_add(pn, nm); if (k2 >= 0) { s->trace_sel = k2; } }
                else { lt_graph_pane_remove(pn, k2); }
                changed = 1;
            }
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
    w[0] = -1; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    if (lt_ui_button(ui, "Load setup")) {
        if (gs_load(app, s) == 0) { lt_app_toast(app, 1, "Graph setup loaded"); }
        else { lt_app_toast(app, 2, "No saved graph setup"); }
    }
    if (lt_ui_button(ui, "Defaults")) { gs_defaults(app, s); changed = 1; }
    if (changed) { gs_save(app, s); }
}

/* ---- left column: recording controls (live) ---- */
static void live_controls(lt_app *app, log_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int recording = app->rec != NULL && lt_rec_is_recording(app->rec);
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    int w1 = -1, w2[2], wr[3], ch;
    char b[200];
    lt_logx *lx = lt_logx_get(app);
    double hz = (double)lx->rate_hz;
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button_ex(ui, recording ? "Stop recording" : "Start recording",
                        recording ? LT_UI_BTN_DANGER : (online ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED))) {
        lt_app_log_toggle(app);
    }
    if (lt_ui_button_ex(ui, "Add marker", recording ? 0 : LT_UI_BTN_DISABLED) && recording) {
        lt_rec_marker(app->rec, "marker");
    }
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button(ui, "Clear log")) {
        if (app->rec != NULL) { lt_rec_clear(app->rec); }
        live_clear(app, s);
        lt_app_toast(app, 0, recording ? "Log cleared: recording continues in a new file" : "Live history cleared");
    }
    if (lt_ui_button_ex(ui, "Channels...", recording ? LT_UI_BTN_DISABLED : 0) && !recording) {
        (void)lt_app_dialog_push(app, "rec-channels", "", "Recording channels");
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (recording) {
        lt_snprintf(b, sizeof(b), "REC  %ld rows  %.1f s  %s", lt_rec_count(app->rec), lt_rec_duration(app->rec),
                    lt_path_base(lt_rec_path(app->rec)));
        lt_ui_label_ex(ui, b, t->error, LT_UI_ALIGN_LEFT);
    } else if (!online) {
        lt_ui_label_wrap(ui, "Connect to an ECU (or use the demo) to record and see live data.", t->muted);
    } else if (app->rec != NULL && lt_rec_path(app->rec)[0] != '\0') {
        lt_snprintf(b, sizeof(b), "Last: %s", lt_path_base(lt_rec_path(app->rec)));
        lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
    } else {
        lt_ui_label_ex(ui, "Not recording", t->muted, LT_UI_ALIGN_LEFT);
    }
    {
        static const char *const fmts[] = { "MLG", "MSL" };
        wr[0] = 84; wr[1] = -1; wr[2] = 70;
        lt_ui_layout_row(ui, 3, wr, 0);
        lt_ui_label_ex(ui, "Rate (Hz)", t->text2, LT_UI_ALIGN_LEFT);
        if (lt_ui_number(ui, "rate", &hz, 1.0, 200.0, 1.0, 0)) { lx->rate_hz = (int)hz; lt_logx_save(app); }
        ch = lx->rec_fmt;
        lt_ui_disabled_begin(ui, recording ? LT_TRUE : LT_FALSE);
        if (lt_ui_combo(ui, "fmt", &ch, fmts, 2) && !recording) { lx->rec_fmt = ch; lt_logx_save(app); }
        lt_ui_disabled_end(ui);
    }
    lt_ui_layout_row(ui, 2, w2, 0);
    ch = lx->auto_rec;
    if (lt_ui_checkbox(ui, "Auto-record", &ch)) { lx->auto_rec = ch; lt_logx_save(app); }
    lt_ui_checkbox(ui, "Stats", &s->show_stats);
}

/* ---- left column: file browser (viewer) ---- */
static void viewer_controls(lt_app *app, log_state *s, const lt_log *L)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w3[3];
    w3[0] = -1; w3[1] = -1; w3[2] = 60;
    lt_ui_layout_row(ui, 3, w3, 0);
    if (lt_ui_button(ui, "Open...")) {
        lt_app_file_dialog(app, LT_DLG_FD_LOG_OPEN, LT_UI_FILE_OPEN, s->dir[0] ? s->dir : NULL, "", lt_log_filter_all());
    }
    if (lt_ui_button_ex(ui, "Export...", L != NULL ? 0 : LT_UI_BTN_DISABLED) && L != NULL) {
        char nm[300];
        lt_logx *x = lt_logx_get(app);
        const char *e;
        lt_snprintf(nm, sizeof(nm), "%s", x->shared_path[0] ? lt_path_base(x->shared_path) : "log.msl");
        e = lt_path_ext(nm);
        if (e != NULL && e[0] != '\0') { nm[strlen(nm) - strlen(e)] = '\0'; }
        lt_strlcpy(nm + strlen(nm), lt_stricmp(e != NULL ? e : "", ".msl") == 0 ? ".csv" : ".msl", 5);
        lt_app_file_dialog(app, LT_DLG_FD_LOG_EXPORT_ANY, LT_UI_FILE_SAVE, s->dir[0] ? s->dir : NULL, nm,
                           "MegaLogViewer text|*.msl|MegaLogViewer binary|*.mlg|CSV|*.csv");
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
        int wr[2];
        if (s->rename_on) {
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
                    lt_logx *x = lt_logx_get(app);
                    if (strcmp(x->shared_path, from) == 0) { lt_strlcpy(x->shared_path, to, sizeof(x->shared_path)); }
                    lt_app_toast(app, 1, "Renamed to %s", nm);
                    s->rename_on = 0;
                    files_scan(app, s);
                } else { lt_app_toast(app, 3, "Rename failed: %s", lt_err_get()); }
                lt_free(from); lt_free(to);
            }
        } else {
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
    else if (L != NULL) {
        lt_logx *x = lt_logx_get(app);
        char b[200];
        lt_snprintf(b, sizeof(b), "%s: %ld rows, %.1f s, %d channels%s", lt_path_base(x->shared_path),
                    L->n, lt_log_duration(L), L->n_ch, x->shared_math > 0 ? " (incl. math)" : "");
        lt_ui_label_wrap(ui, b, t->text2);
    }
}

/* ---- right column: transport / live bar ---- */
static void viewer_transport(lt_app *app, log_state *s, const lt_log *L, double now)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w6[7], w1 = -1;
    double dur = lt_log_duration(L), tt = s->view.cursor_t;
    char b[64];
    w6[0] = CH * 8; w6[1] = CH * 8; w6[2] = CH * 8; w6[3] = CH * 8; w6[4] = CH * 8; w6[5] = -1;
    lt_ui_layout_row(ui, 6, w6, 0);
    if (lt_ui_button_ex(ui, s->playing ? "Pause" : "Play", LT_UI_BTN_PRIMARY)) {
        if (!s->playing && s->view.cursor_t >= dur - 1e-6) { lt_player_seek(&s->pl, 0); s->view.cursor_t = 0.0; }
        if (!s->playing) { lt_player_seek_time(&s->pl, s->view.cursor_t); }
        s->playing = !s->playing;
        s->last_ms = now;
    }
    if (lt_ui_button(ui, "Start")) { lt_player_seek(&s->pl, 0); s->view.cursor_t = 0.0; s->playing = 0; }
    lt_ui_combo(ui, "speed", &s->speed_sel, speeds, 6);
    if (lt_ui_button(ui, "Fit")) { s->view.t0 = 0.0; s->view.t1 = dur > 0.0 ? dur : 1.0; }
    lt_ui_checkbox(ui, "Loop", &s->loop);
    if (app->conn == LT_APP_OFFLINE && app->rt != NULL) { lt_ui_checkbox(ui, "Drive gauges", &s->drive); }
    else { lt_ui_label_ex(ui, "Wheel: zoom   right-drag: pan   click: cursor", t->muted, LT_UI_ALIGN_RIGHT); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_slider(ui, "timeline", &tt, 0.0, dur > 0.0 ? dur : 1.0, 2)) {
        s->view.cursor_t = tt;
        s->view.has_cursor = 1;
        lt_player_seek_time(&s->pl, tt);
    }
    lt_snprintf(b, sizeof(b), "%.2f / %.2f s   window %.1f s", s->view.cursor_t, dur, s->view.t1 - s->view.t0);
    lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_RIGHT);
}

static void live_bar(lt_app *app, log_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const spans[] = { "10 s", "20 s", "30 s", "60 s", "120 s" };
    static const double span_v[] = { 10.0, 20.0, 30.0, 60.0, 120.0 };
    int w[4], k, sel = 1;
    for (k = 0; k < 5; k++) { if (fabs(s->span - span_v[k]) < 0.01) { sel = k; } }
    w[0] = CH * 10; w[1] = CH * 10; w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    if (lt_ui_combo(ui, "span", &sel, spans, 5)) { s->span = span_v[sel]; s->follow = 1; }
    if (lt_ui_button_ex(ui, s->follow ? "Following" : "Follow live", s->follow ? LT_UI_BTN_SELECTED : LT_UI_BTN_PRIMARY)) {
        s->follow = 1;
        s->view.has_cursor = 0;
    }
    lt_ui_label_ex(ui, s->follow ? "Click: cursor   wheel: zoom   right-drag: pan (pauses following)"
                                 : "Paused view: data keeps recording; Follow live returns to the newest data",
                   t->muted, LT_UI_ALIGN_RIGHT);
}

/* ---- main draw ---- */
static void log_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    log_state *s = (log_state *)tab->data;
    int w1 = -1, wc[2];
    const char *picked;
    const lt_log *L;
    double now, tmin = 0.0, tmax = 1.0;
    LT_UNUSED(r);
    if (s == NULL) {
        s = (log_state *)lt_calloc(1, sizeof(*s));
        tab->data = s;
        s->viewer = strcmp(tab->kind, "datalog-viewer") == 0;
        s->speed_sel = 2;
        s->span = 20.0;
        s->follow = 1;
        lt_graph_init(&s->gs);
        lt_log_init(&s->live);
        if (s->viewer) {
            char *e = lt_plat_getenv("LT_SHOT_LOGVIEW");   /* screenshot harness: initial view */
            files_scan(app, s);
            if (e != NULL) { s->mode = strcmp(e, "scatter") == 0 ? 1 : (strcmp(e, "hist") == 0 ? 2 : 0); lt_free(e); }
        }
    }
    now = lt_ui_now(ui);

    /* file dialog results */
    picked = lt_app_file_result(app, LT_DLG_FD_LOG_OPEN);
    if (picked != NULL && s->viewer) { load_log(app, s, picked); }
    picked = lt_app_file_result(app, LT_DLG_FD_LOG_EXPORT_ANY);
    if (picked != NULL && s->viewer && lt_logx_shared_log(app) != NULL) {
        char path[600];
        lt_strlcpy(path, picked, sizeof(path));
        if (lt_log_format_from_ext(path) == LT_FMT_NONE) { lt_strlcpy(path + strlen(path), ".msl", sizeof(path) - strlen(path)); }
        if (lt_log_write(path, lt_logx_shared_log(app)) == 0) { lt_app_toast(app, 1, "Exported %s", lt_path_base(path)); }
        else { lt_app_toast(app, 3, "Export failed: %s", lt_err_get()); }
    }

    /* sampling / playback clock */
    if (!s->viewer) {
        live_sample(app, s, now);
        gs_ensure(app, s);
        lt_ui_request_redraw(ui);
    } else {
        files_refresh(app, s);
        viewer_sync(app, s);
        if (lt_logx_shared_log(app) != NULL) {
            double dt = s->last_ms > 0 && now >= s->last_ms ? (now - s->last_ms) / 1000.0 : 0.0;
            s->last_ms = now;
            if (s->playing) {
                lt_player_set_rate(&s->pl, speed_v[s->speed_sel]);
                lt_player_set_loop(&s->pl, s->loop ? LT_TRUE : LT_FALSE);
                if (!lt_player_advance(&s->pl, dt)) { s->playing = 0; }
                s->view.cursor_t = lt_player_time(&s->pl);
                s->view.has_cursor = 1;
                if (s->view.cursor_t > s->view.t1 || s->view.cursor_t < s->view.t0) {   /* page along */
                    double sp = s->view.t1 - s->view.t0;
                    s->view.t0 = s->view.cursor_t - sp * 0.1;
                    if (s->view.t0 < 0.0) { s->view.t0 = 0.0; }
                    s->view.t1 = s->view.t0 + sp;
                }
                lt_ui_request_redraw(ui);
            }
        }
    }
    L = src_log(app, s);

    /* live window */
    if (L != NULL && L->n > 0) {
        tmin = L->t[0];
        tmax = L->t[L->n - 1];
        if (!s->viewer && s->follow) {
            s->view.t1 = tmax > s->span ? tmax : s->span;
            s->view.t0 = s->view.t1 - s->span;
            if (s->view.t0 < tmin && tmax - tmin < s->span) { s->view.t0 = tmin; s->view.t1 = tmin + s->span; }
        }
    } else if (!s->viewer) {
        s->view.t0 = 0.0; s->view.t1 = s->span;
    }
    if (tmax <= tmin) { tmax = tmin + 1.0; }

    wc[0] = CH * 36; wc[1] = -1;
    lt_ui_layout_row(ui, 2, wc, -1);
    /* ---------------- left column ---------------- */
    lt_ui_layout_column_begin(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, s->viewer ? "Datalog Viewer" : "Data Logging");
    if (s->viewer) { viewer_controls(app, s, L); } else { live_controls(app, s); }
    lt_ui_separator(ui);
    if (!s->viewer || s->mode == 0) { setup_editor(app, s, L); }
    else {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->mode == 1
                         ? "Scatter plot: every sample of the log as a point, X against Y, coloured by a third channel. "
                           "The filter is an expression over the log channels (for example rpm > 2000 && tps > 50)."
                         : "Histogram / table generator: the average of Z in every X/Y cell (or the hit count). "
                           "Use the axes of a tune table to build a correction table, then Copy values and paste "
                           "it into the table editor.", lt_ui_theme(ui)->muted);
    }
    lt_ui_layout_column_end(ui);

    /* ---------------- right column ---------------- */
    lt_ui_layout_column_begin(ui);
    if (s->viewer) {
        if (lt_ui_cell_mode(ui)) {          /* the flat tab row needs two text rows: buttons here */
            int wm[3], k;
            wm[0] = CH * 8; wm[1] = CH * 15; wm[2] = CH * 20;
            lt_ui_layout_row(ui, 3, wm, 0);
            for (k = 0; k < 3; k++) {
                if (lt_ui_button_ex(ui, modes[k], k == s->mode ? LT_UI_BTN_SELECTED : LT_UI_BTN_FLAT)) { s->mode = k; }
            }
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            (void)lt_ui_tabs_flat(ui, "lmode", modes, 3, &s->mode);
        }
    }
    if (s->viewer && s->mode != 0) {
        lt_lxy_draw(app, &s->xy, L, s->log_gen, s->mode, s->view.t0, s->view.t1);
    } else {
        if (s->viewer) {
            if (L != NULL) { viewer_transport(app, s, L, now); }
            else {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "Open a .mlg, .msl or .csv log to review it.", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
            }
        } else {
            live_bar(app, s);
        }
        lt_ui_layout_row(ui, 1, &w1, s->show_stats ? -(stats_rows(s) * (lt_ui_cell_mode(ui) ? 16 : 30) + 12) : -1);
        {
            lt_rect cr = lt_ui_layout_next(ui);
            double old_span = s->view.t1 - s->view.t0;
            int was_drag = s->view.drag;
            s->view.user_moved = 0;
            if (lt_lg_draw(app, cr, L, s->gs_ready ? cur_page(s) : NULL, &s->view, tmin, tmax)) {
                if (s->viewer) {
                    if (s->view.drag == 1 || !s->playing) { lt_player_seek_time(&s->pl, s->view.cursor_t); }
                } else if (s->view.user_moved) {
                    double ns = s->view.t1 - s->view.t0;
                    if (was_drag == 2 || s->view.drag == 2) { s->follow = 0; }
                    else if (fabs(ns - old_span) > 1e-9) { s->span = ns < 1.0 ? 1.0 : (ns > 120.0 ? 120.0 : ns); }
                }
            }
            if (s->drive && s->viewer && app->rt != NULL && app->ini != NULL && L != NULL) {
                lt_player_apply_to_rt(&s->pl, app->ini, app->rt);
            }
        }
        if (s->show_stats && s->gs_ready) { stats_draw(app, s, L, s->view.t0, s->view.t1); }
    }
    lt_ui_layout_column_end(ui);
}

static void log_close(lt_app *app, lt_app_tab *tab)
{
    log_state *s = (log_state *)tab->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->live_ok) { lt_log_free(&s->live); }
        if (s->files != NULL) { lt_plat_free_list(s->files, s->n_files); }
        lt_graph_free(&s->gs);
        lt_lxy_free(&s->xy);
        lt_free(s);
    }
    tab->data = NULL;
}

int lt_dlg_log_files(const lt_app_tab *tab, const char **sel_name)
{
    const log_state *s = tab != NULL ? (const log_state *)tab->data : NULL;
    if (sel_name != NULL) { *sel_name = NULL; }
    if (s == NULL || !s->viewer) { return -1; }
    if (sel_name != NULL && s->file_sel >= 0 && s->file_sel < s->n_files) { *sel_name = s->files[s->file_sel]; }
    return s->n_files;
}

void lt_dlg_register_log(lt_app *app)
{
    lt_app_tab_kind tk;
    lt_datalog_set_rt_setter(lt_ini_rt_set);
    tk.draw = log_draw; tk.close = log_close;
    tk.kind = "datalog"; lt_app_register_tab_kind(app, &tk);
    tk.kind = "datalog-viewer"; lt_app_register_tab_kind(app, &tk);
}
