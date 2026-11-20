/* feat_logging.c - data logging extras (G-61, G-63, G-65): smart recording (start/stop on
 * key-on/key-off by RPM), recorder sample rate and file format (MLG / MSL), recording channel
 * selection, alert rules, live math channels (evaluated on every realtime frame and published as
 * virtual realtime channels), the loaded datalog shared by the viewer and the curve overlay,
 * and Tools > Data Logging.
 * Settings page "Data Logging"; dialog kind "rec-channels"; configuration in logging.conf.
 * Logic (key tracking, alert checks) lives in datalog/dl_ana.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "feat.h"
#include "feat_log.h"
#include "dialogs.h"
#include "../project/project.h"
#include "../plat/plat.h"

static void logx_free(lt_app *app, void *p)
{
    lt_logx *x = (lt_logx *)p;
    if (x->off != NULL) { lt_strv_free(x->off, x->n_off); }
    lt_math_set_free(&x->math);
    lt_math_live_free(&x->live);
    if (x->have_shared) { lt_log_free(&x->shared); }
    if (app->rt != NULL) { lt_ini_rt_set_virtual(app->rt, NULL, NULL); }
    lt_datalog_set_virtual_channels(NULL, NULL, 0);
}

static char *conf_path(lt_app *app)
{
    return app->config_dir != NULL ? lt_path_join(app->config_dir, "logging.conf") : NULL;
}

static void set_rule(lt_alert_rule *r, const char *ch, int above, double limit)
{
    memset(r, 0, sizeof(*r));
    lt_strlcpy(r->channel, ch, sizeof(r->channel));
    r->above = above;
    r->limit = limit;
}

static void set_defaults(lt_logx *x)
{
    x->auto_rec = 0; x->key_rpm = 100; x->key_timeout = 2; x->rate_hz = 10;
    x->alert_on = 1; x->alert_abs = 5.0; x->alert_pct = 10.0;
    set_rule(&x->rules[0], "coolant", 1, 110.0);
    set_rule(&x->rules[1], "afr", 1, 17.0);
    set_rule(&x->rules[2], "rpm", 1, 7000.0);
    set_rule(&x->rules[3], "batteryVoltage", 0, 11.0);
}

int lt_logx_excluded(const lt_logx *x, const char *name)
{
    size_t i;
    for (i = 0; i < x->n_off; i++) { if (strcmp(x->off[i], name) == 0) { return 1; } }
    return 0;
}

void lt_logx_set_excluded(lt_logx *x, const char *name, int excluded)
{
    size_t i;
    if (excluded) {
        if (lt_logx_excluded(x, name)) { return; }
        x->off = (char **)lt_realloc(x->off, (x->n_off + 1) * sizeof(char *));
        x->off[x->n_off++] = lt_strdup(name);
        return;
    }
    for (i = 0; i < x->n_off; i++) {
        if (strcmp(x->off[i], name) == 0) {
            lt_free(x->off[i]);
            memmove(&x->off[i], &x->off[i + 1], (x->n_off - i - 1) * sizeof(char *));
            x->n_off--;
            return;
        }
    }
}

static void conf_load(lt_app *app, lt_logx *x)
{
    lt_props p;
    char *path = conf_path(app), key[24];
    const char *v;
    int i, n = 0;
    set_defaults(x);
    if (path == NULL) { return; }
    lt_props_init(&p);
    if (lt_file_exists(path) && lt_props_load(&p, path) == 0) {
        lt_props_get_i32(&p, "auto_rec", &x->auto_rec);
        lt_props_get_i32(&p, "key_rpm", &x->key_rpm);
        lt_props_get_i32(&p, "key_timeout", &x->key_timeout);
        lt_props_get_i32(&p, "rate_hz", &x->rate_hz);
        lt_props_get_i32(&p, "rec_fmt", &x->rec_fmt);
        lt_props_get_i32(&p, "alert_on", &x->alert_on);
        lt_props_get_f64(&p, "alert_abs", &x->alert_abs);
        lt_props_get_f64(&p, "alert_pct", &x->alert_pct);
        for (i = 0; i < LT_LOGX_RULES; i++) {
            lt_snprintf(key, sizeof(key), "rule%d_ch", i);
            v = lt_props_get(&p, key);
            if (v != NULL) { lt_strlcpy(x->rules[i].channel, v, sizeof(x->rules[i].channel)); }
            lt_snprintf(key, sizeof(key), "rule%d_above", i);
            lt_props_get_i32(&p, key, &x->rules[i].above);
            lt_snprintf(key, sizeof(key), "rule%d_limit", i);
            lt_props_get_f64(&p, key, &x->rules[i].limit);
            lt_snprintf(key, sizeof(key), "rule%d_on", i);
            lt_props_get_i32(&p, key, &x->rules[i].enabled);
        }
        lt_props_get_i32(&p, "off_n", &n);
        for (i = 0; i < n && i < 4096; i++) {
            lt_snprintf(key, sizeof(key), "off%d", i);
            v = lt_props_get(&p, key);
            if (v != NULL) { lt_logx_set_excluded(x, v, 1); }
        }
    }
    x->key_rpm = LT_CLAMP(x->key_rpm, LT_KEY_RPM_MIN, LT_KEY_RPM_MAX);
    x->key_timeout = LT_CLAMP(x->key_timeout, LT_KEY_TIMEOUT_MIN, LT_KEY_TIMEOUT_MAX);
    x->rate_hz = LT_CLAMP(x->rate_hz, 1, 200);
    x->rec_fmt = x->rec_fmt == 1 ? 1 : 0;
    lt_props_free(&p);
    lt_free(path);
}

lt_logx *lt_logx_get(lt_app *app)
{
    lt_logx *x = (lt_logx *)lt_ext_state(app, "logx", sizeof(lt_logx), logx_free);
    if (!x->cfg_loaded) {
        x->cfg_loaded = 1;
        conf_load(app, x);
        lt_keytrack_init(&x->key);
        x->app = app;
    }
    return x;
}

static void put_i(lt_props *p, const char *k, int v)
{
    char b[32];
    lt_snprintf(b, sizeof(b), "%d", v);
    lt_props_set(p, k, b);
}

static void put_f(lt_props *p, const char *k, double v)
{
    char b[48];
    lt_fmt_double(b, sizeof(b), v, 4);
    lt_props_set(p, k, b);
}

void lt_logx_save(lt_app *app)
{
    lt_logx *x = lt_logx_get(app);
    lt_props p;
    char *path = conf_path(app), key[24];
    size_t i;
    if (path == NULL) { return; }
    lt_props_init(&p);
    put_i(&p, "auto_rec", x->auto_rec); put_i(&p, "key_rpm", x->key_rpm);
    put_i(&p, "key_timeout", x->key_timeout); put_i(&p, "rate_hz", x->rate_hz);
    put_i(&p, "rec_fmt", x->rec_fmt);
    put_i(&p, "alert_on", x->alert_on);
    put_f(&p, "alert_abs", x->alert_abs); put_f(&p, "alert_pct", x->alert_pct);
    for (i = 0; i < LT_LOGX_RULES; i++) {
        lt_snprintf(key, sizeof(key), "rule%d_ch", (int)i); lt_props_set(&p, key, x->rules[i].channel);
        lt_snprintf(key, sizeof(key), "rule%d_above", (int)i); put_i(&p, key, x->rules[i].above);
        lt_snprintf(key, sizeof(key), "rule%d_limit", (int)i); put_f(&p, key, x->rules[i].limit);
        lt_snprintf(key, sizeof(key), "rule%d_on", (int)i); put_i(&p, key, x->rules[i].enabled);
    }
    put_i(&p, "off_n", (int)x->n_off);
    for (i = 0; i < x->n_off; i++) {
        lt_snprintf(key, sizeof(key), "off%d", (int)i);
        lt_props_set(&p, key, x->off[i]);
    }
    (void)lt_plat_mkdirs(app->config_dir);
    (void)lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(path);
}

/* ---- project math channels ---- */
static char *math_path(lt_app *app)
{
    if (app->has_project) { return lt_project_file(&app->project, "math_channels.json"); }
    return lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "math_channels.json");
}

lt_math_set *lt_logx_math(lt_app *app)
{
    lt_logx *x = lt_logx_get(app);
    char *p = math_path(app);
    if (!x->math_loaded || strcmp(p, x->math_path) != 0) {
        lt_math_hooks h = LT_MATH_HOOKS_INI;
        lt_math_set_hooks(&h);
        lt_math_set_free(&x->math);
        if (lt_math_load(p, &x->math) < 0) { lt_math_set_free(&x->math); }
        lt_strlcpy(x->math_path, p, sizeof(x->math_path));
        x->math_loaded = 1;
        x->math_gen++;
        x->live.valid = 0;
    }
    lt_free(p);
    return &x->math;
}

int lt_logx_math_save(lt_app *app)
{
    lt_logx *x = lt_logx_get(app);
    char *p = math_path(app);
    int rc;
    (void)lt_plat_mkdirs(app->has_project ? app->project.path : app->config_dir);
    rc = lt_math_save(p, &x->math);
    lt_strlcpy(x->math_path, p, sizeof(x->math_path));
    x->math_loaded = 1;
    x->math_gen++;
    x->live.valid = 0;
    lt_free(p);
    return rc;
}

int lt_logx_check_change(lt_app *app, double old_v, double new_v)
{
    lt_logx *x = lt_logx_get(app);
    double d;
    if (!x->alert_on) { return 0; }
    if (!lt_alert_large_change(old_v, new_v, x->alert_abs, x->alert_pct, &d)) { return 0; }
    lt_app_toast(app, 2, "Large table change: %s%.2f (%.2f to %.2f)", d >= 0.0 ? "+" : "", d, old_v, new_v);
    return 1;
}

/* ---- recorder maintenance ---- */
static void rec_drop(lt_app *app)
{
    if (app->rec != NULL) { lt_rec_free(app->rec); app->rec = NULL; }
    if (app->rec_names != NULL) { lt_strv_free(app->rec_names, app->rec_n); app->rec_names = NULL; }
    lt_free(app->rec_vals);
    app->rec_vals = NULL;
    app->rec_n = 0;
}

/* (re)create the recorder with the selected channels (all datalog channels minus the excluded) */
static void rec_build(lt_app *app, lt_logx *x)
{
    char **names = NULL, **units = NULL, **kn, **ku;
    size_t n, i, k = 0;
    rec_drop(app);
    n = lt_datalog_channels_from_ini(app->ini, &names, &units);
    if (n == 0) { return; }
    kn = (char **)lt_calloc(n + 1, sizeof(char *));
    ku = (char **)lt_calloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++) {
        if (lt_logx_excluded(x, names[i])) { continue; }
        kn[k] = lt_strdup(names[i]); ku[k] = lt_strdup(units[i]); k++;
    }
    if (k == 0) {   /* never record nothing: keep the first channel */
        kn[0] = lt_strdup(names[0]); ku[0] = lt_strdup(units[0]); k = 1;
    }
    app->rec_names = kn;
    app->rec_n = k;
    app->rec = lt_rec_new((const char *const *)kn, (const char *const *)ku, (int)k);
    app->rec_vals = (double *)lt_calloc(k, sizeof(double));
    lt_strv_free(names, n);
    lt_strv_free(units, n);
    lt_strv_free(ku, k);
}

/* append the math channels to a finished recording (rewrites the file) */
static void rec_postprocess(lt_app *app, const char *path)
{
    lt_log lg;
    lt_math_set *ms = lt_logx_math(app);
    if (ms->n == 0 || path == NULL || path[0] == '\0') { return; }
    lt_log_init(&lg);
    if (lt_log_read(path, &lg) == 0 && lg.n > 0 && lt_log_channel_index(&lg, ms->ch[0].name) < 0 &&
        lt_math_apply_to_log(ms, &lg) == 0) {
        if (lt_log_write(path, &lg) != 0) { lt_app_toast(app, 2, "Math channels not stored: %s", lt_err_get()); }
    }
    lt_log_free(&lg);
}

static int excl_sig(const lt_logx *x)
{
    size_t i;
    unsigned long h = 5381UL;
    const char *s;
    for (i = 0; i < x->n_off; i++) {
        for (s = x->off[i]; *s != '\0'; s++) { h = (h * 33UL + (unsigned long)(unsigned char)*s) & 0x7FFFFFFUL; }
        h = (h * 31UL + 7UL) & 0x7FFFFFFUL;
    }
    return (int)h;
}

/* ---- live math channels ---- */
static int math_index(const lt_math_set *ms, const char *name)
{
    size_t k;
    for (k = 0; k < ms->n; k++) {
        if (ms->ch[k].name != NULL && strcmp(ms->ch[k].name, name) == 0) { return (int)k; }
    }
    return -1;
}

/* re-evaluate when a new realtime frame arrived (or 100 ms passed: replayed values) */
static void live_refresh(lt_logx *x)
{
    lt_app *app = x->app;
    double now;
    if (app == NULL || app->rt == NULL || x->live.busy || x->math.n == 0) { return; }
    now = lt_plat_now_ms();
    if (x->live.valid && x->live_stamp == app->frames_ok && now - x->live_ms < 100.0 && now >= x->live_ms) {
        return;
    }
    x->live_stamp = app->frames_ok;
    x->live_ms = now;
    (void)lt_math_live_eval(&x->live, &x->math, &app->varenv.env);
}

/* lt_ini_rt virtual channel hook: names that are not INI channels */
static lt_bool virt_get(void *ud, const char *name, double *out)
{
    lt_logx *x = (lt_logx *)ud;
    if (x->math.n == 0 || math_index(&x->math, name) < 0) { return LT_FALSE; }
    live_refresh(x);
    return lt_math_live_get(&x->live, &x->math, name, out);
}

lt_bool lt_logx_live_math(lt_app *app, const char *name, double *out)
{
    lt_logx *x = lt_logx_get(app);
    (void)lt_logx_math(app);
    return virt_get(x, name, out);
}

/* publish the math channel names as virtual channels and hook them into the realtime values */
static void live_math_sync(lt_app *app, lt_logx *x)
{
    lt_math_set *ms = lt_logx_math(app);
    if (x->virt_gen != x->math_gen) {
        const char **nm = (const char **)lt_calloc(ms->n + 1, sizeof(char *));
        const char **un = (const char **)lt_calloc(ms->n + 1, sizeof(char *));
        size_t k;
        for (k = 0; k < ms->n; k++) { nm[k] = ms->ch[k].name; un[k] = ms->ch[k].units; }
        lt_datalog_set_virtual_channels(nm, un, ms->n);
        lt_free((void *)nm);
        lt_free((void *)un);
        x->virt_gen = x->math_gen;
    }
    if (app->rt != NULL) { lt_ini_rt_set_virtual(app->rt, virt_get, x); }
}

/* ---- the loaded datalog ---- */
lt_log *lt_logx_shared_log(lt_app *app)
{
    lt_logx *x = lt_logx_get(app);
    return x->have_shared ? &x->shared : NULL;
}

int lt_logx_load_log(lt_app *app, const char *path, char *err, size_t errlen)
{
    lt_logx *x = lt_logx_get(app);
    lt_log nl;
    lt_math_set *ms;
    lt_log_init(&nl);
    if (lt_log_read(path, &nl) < 0) {
        lt_snprintf(err, errlen, "Cannot read %s: %s", lt_path_base(path), lt_err_get());
        lt_log_free(&nl);
        return -1;
    }
    x->shared_math = 0;
    ms = lt_logx_math(app);
    if (ms->n > 0 && lt_log_channel_index(&nl, ms->ch[0].name) < 0) {
        int before = nl.n_ch;
        if (lt_math_apply_to_log(ms, &nl) == 0) { x->shared_math = nl.n_ch - before; }
    }
    if (x->have_shared) { lt_log_free(&x->shared); }
    x->shared = nl;
    x->have_shared = 1;
    x->shared_gen++;
    lt_strlcpy(x->shared_path, path, sizeof(x->shared_path));
    return 0;
}

char *lt_logx_graph_path(lt_app *app)
{
    if (app->has_project) { return lt_project_file(&app->project, "graph_setup.json"); }
    return lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "graph_setup.json");
}

/* the recorder already holds every virtual channel */
static int rec_has_virtuals(const lt_app *app)
{
    size_t k, i;
    for (k = 0; k < lt_datalog_virtual_count(); k++) {
        int found = 0;
        for (i = 0; i < app->rec_n && !found; i++) {
            found = strcmp(app->rec_names[i], lt_datalog_virtual_name(k)) == 0;
        }
        if (!found) { return 0; }
    }
    return 1;
}

static void logx_tick(lt_app *app)
{
    lt_logx *x = lt_logx_get(app);
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    int recording, i, ev;
    double v, now;
    if (!x->shot_checked) {
        char *e = lt_plat_getenv("LT_SHOT_LOG");      /* screenshot harness: preload a log */
        x->shot_checked = 1;
        if (e != NULL) {
            char eb[200];
            if (lt_logx_load_log(app, e, eb, sizeof(eb)) != 0) { lt_app_toast(app, 3, "%s", eb); }
            lt_free(e);
        }
    }
    if (app->ini == NULL) { return; }
    live_math_sync(app, x);
    recording = app->rec != NULL && lt_rec_is_recording(app->rec);
    /* a recording just ended: add the math channels (when they were not recorded live) */
    if (x->was_rec && !recording && app->rec != NULL) { rec_postprocess(app, lt_rec_path(app->rec)); }
    x->was_rec = recording;
    if (app->rec != NULL && x->virt_rec_gen != x->virt_gen && rec_has_virtuals(app)) {
        x->virt_rec_gen = x->virt_gen;
    }
    /* channel selection (the math channels are recorded too) */
    if (!recording && (x->sig_applied != excl_sig(x) || (app->rec == NULL && x->n_off > 0) ||
                       (app->rec != NULL && x->virt_rec_gen != x->virt_gen))) {
        x->sig_applied = excl_sig(x);
        x->virt_rec_gen = x->virt_gen;
        if (x->n_off > 0 || lt_datalog_virtual_count() > 0) { rec_build(app, x); } else { rec_drop(app); }
    }
    if (app->rec != NULL) {
        lt_rec_set_sample_rate(app->rec, (double)x->rate_hz);
        lt_rec_set_format(app->rec, x->rec_fmt == 1 ? LT_FMT_MSL : LT_FMT_MLG);
    }
    if (!online) { x->key.on = 0; x->key.pending = 0; x->auto_started = 0; return; }
    now = lt_plat_now_ms();
    /* smart recording */
    if (x->auto_rec && lt_app_channel(app, "rpm", &v)) {
        ev = lt_keytrack_update(&x->key, v, now, (double)x->key_rpm, (double)x->key_timeout);
        if (ev > 0 && !recording) { if (lt_app_log_toggle(app) == 0) { x->auto_started = 1; } }
        else if (ev < 0 && recording && x->auto_started) { (void)lt_app_log_toggle(app); x->auto_started = 0; }
    } else if (!x->auto_rec) { lt_keytrack_init(&x->key); }
    if (!recording) { x->auto_started = 0; }
    /* alert rules */
    for (i = 0; i < LT_LOGX_RULES; i++) {
        lt_alert_rule *r = &x->rules[i];
        if (!r->enabled || r->channel[0] == '\0' || !lt_app_channel(app, r->channel, &v)) { r->active = 0; continue; }
        if (lt_alert_rule_check(r, v)) {
            char msg[100];
            lt_snprintf(msg, sizeof(msg), "ALERT %s %s %.4g (%.4g)", r->channel, r->above ? ">" : "<", r->limit, v);
            lt_app_toast(app, 2, "%s", msg);
            if (recording) { lt_rec_marker(app->rec, msg); }
        }
    }
}

/* ---- settings page ---- */
static void num_row(lt_ui *ui, const char *label, const char *id, double *v, double lo, double hi,
                    double step, int digits, int *changed)
{
    int w[2];
    w[0] = 220; w[1] = 120;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, label);
    if (lt_ui_number(ui, id, v, lo, hi, step, digits)) { *changed = 1; }
}

static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    lt_logx *x = lt_logx_get(app);
    int w1 = -1, ch = 0, i;
    double d;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Smart recording");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Enable auto-record (start on key-on, stop on key-off)", &x->auto_rec)) { ch = 1; }
    d = (double)x->key_rpm;
    num_row(ui, "Key-on threshold (RPM, 50-500)", "lg_rpm", &d, LT_KEY_RPM_MIN, LT_KEY_RPM_MAX, 10.0, 0, &ch);
    x->key_rpm = (int)d;
    d = (double)x->key_timeout;
    num_row(ui, "Key-off timeout (s, 1-10)", "lg_to", &d, LT_KEY_TIMEOUT_MIN, LT_KEY_TIMEOUT_MAX, 1.0, 0, &ch);
    x->key_timeout = (int)d;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Recording");
    d = (double)x->rate_hz;
    num_row(ui, "Sample rate (Hz, 1-200)", "lg_hz", &d, 1.0, 200.0, 1.0, 0, &ch);
    x->rate_hz = (int)d;
    {
        static const char *const fmts[] = { "MLG (MegaLogViewer binary)", "MSL (MegaLogViewer text)" };
        int w[2];
        w[0] = 220; w[1] = 220;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "File format");
        if (lt_ui_combo(ui, "lg_fmt", &x->rec_fmt, fmts, 2)) { ch = 1; }
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "Choose the recorded channels with the Channels button in the Data Logging tab.",
                     lt_ui_theme(ui)->muted);
    lt_ui_heading(ui, "Alert rules");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Warn on large table changes", &x->alert_on)) { ch = 1; }
    num_row(ui, "Absolute change threshold", "lg_abs", &x->alert_abs, 0.0, 100000.0, 0.5, 2, &ch);
    num_row(ui, "Percent change threshold", "lg_pct", &x->alert_pct, 0.0, 10000.0, 1.0, 1, &ch);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label(ui, "Channel limits (toast and log marker when crossed):");
    for (i = 0; i < LT_LOGX_RULES; i++) {
        int w[4], sel;
        static const char *const ops[] = { "below", "above" };
        lt_alert_rule *r = &x->rules[i];
        w[0] = 60; w[1] = 150; w[2] = 90; w[3] = 120;
        lt_ui_layout_row(ui, 4, w, 0);
        lt_ui_push_id_int(ui, i);
        if (lt_ui_checkbox(ui, "on", &r->enabled)) { ch = 1; }
        if (lt_ui_text(ui, "chan", r->channel, sizeof(r->channel), 0) & LT_UI_CHANGED) { ch = 1; }
        sel = r->above;
        if (lt_ui_combo(ui, "op", &sel, ops, 2)) { r->above = sel; ch = 1; }
        if (lt_ui_number(ui, "lim", &r->limit, -1e6, 1e6, 1.0, 2)) { ch = 1; }
        lt_ui_pop_id(ui);
    }
    if (ch) { lt_logx_save(app); }
}

/* ---- recording channels dialog ---- */
typedef struct rc_dlg { char filter[64]; char **names; size_t n; } rc_dlg;

static void rc_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    lt_logx *x = lt_logx_get(app);
    rc_dlg *s = (rc_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], i, changed = 0;
    if (s == NULL) {
        char **u = NULL;
        s = (rc_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        if (app->ini != NULL) { s->n = lt_datalog_channels_from_ini(app->ini, &s->names, &u); }
        if (u != NULL) { lt_strv_free(u, s->n); }
    }
    if (lt_ui_modal_begin(ui, "Recording channels", 520, 560, &open)) {
        char b[96];
        if (app->rec != NULL && lt_rec_is_recording(app->rec)) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Stop recording to change the channel set.", lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        (void)lt_ui_text(ui, "rcf", s->filter, sizeof(s->filter), 0);
        w2[0] = 110; w2[1] = 110;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Select all")) {
            while (x->n_off > 0) { lt_logx_set_excluded(x, x->off[0], 0); }
            changed = 1;
        }
        if (lt_ui_button(ui, "Select none")) {
            for (i = 0; i < (int)s->n; i++) { lt_logx_set_excluded(x, s->names[i], 1); }
            changed = 1;
        }
        lt_snprintf(b, sizeof(b), "%lu of %lu channels recorded",
                    (unsigned long)(s->n - x->n_off), (unsigned long)s->n);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, b, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, -36);
        if (lt_ui_scroll_begin(ui, "rcl")) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            for (i = 0; i < (int)s->n; i++) {
                int on;
                if (!lt_dlg_contains_ci(s->names[i], s->filter)) { continue; }
                on = !lt_logx_excluded(x, s->names[i]);
                lt_ui_push_id_int(ui, i);
                if (lt_ui_checkbox(ui, s->names[i], &on)) { lt_logx_set_excluded(x, s->names[i], !on); changed = 1; }
                lt_ui_pop_id(ui);
            }
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_button(ui, "Close")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    if (changed) { lt_logx_save(app); }
    d->open = open;
}

static void rc_close(lt_app *app, lt_app_dialog *d)
{
    rc_dlg *s = (rc_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) { if (s->names != NULL) { lt_strv_free(s->names, s->n); } lt_free(s); }
    d->data = NULL;
}

static void act_datalog(lt_app *app) { (void)lt_app_datalog_open(app, 0); }

void lt_feat_logging_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "rec-channels"; dk.draw = rc_draw; dk.close = rc_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_tick(reg, logx_tick);
    lt_ext_add_settings_page(reg, "Data Logging", settings_page);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Data &Logging", NULL, act_datalog, lt_ext_pred_ini);
}
