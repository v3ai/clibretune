/* feat_prefs.c - the preference pages and what they drive (G-92, G-93, G-94, G-96, G-104):
 *   Settings -> Table Display   Y axis, cursor and trail colours, heat map schemes with custom
 *                               colour stops for the value / change / coverage contexts
 *   Settings -> Behavior        auto-burn on close, large-change alert thresholds, help icons,
 *                               restore session, demo mode, onboarding
 *   Settings -> Status Bar      up to 8 live channels shown in the status bar
 *   Settings -> Project         the open project's own settings
 *   session restore             open tabs (tables, curves, dialogs) come back with the project
 * Values live in lt_prefs (prefs.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../datalog/datalog.h"

typedef struct pf_state {
    char seen[512];                 /* project whose session was handled */
    int demo_req;                   /* 1 start demo, 2 stop (queued from the settings page) */
    char filter[48];
    char **chan; size_t n_chan;     /* output channels of the open INI (cache) */
    const lt_ini *chan_ini;
    int sb_sel;
} pf_state;

static void pf_free(lt_app *app, void *p)
{
    pf_state *s = (pf_state *)p;
    LT_UNUSED(app);
    if (s->chan != NULL) { lt_strv_free(s->chan, s->n_chan); }
    s->chan = NULL;
}

static pf_state *pf(lt_app *app)
{
    return (pf_state *)lt_ext_state(app, "prefs", sizeof(pf_state), pf_free);
}

/* ------------------------------------------------------------------ */
/* helpers used by the shell                                           */
/* table cursor / trail colours: 0 = theme default */
lt_bool lt_feat_prefs_color(const lt_app *app, int trail, lt_color *out)
{
    lt_u32 v = 0;
    const char *s = trail ? app->settings.prefs.trail_color : app->settings.prefs.cursor_color;
    if (!lt_prefs_parse_color(s, &v)) { return LT_FALSE; }
    *out = LT_RGB((v >> 16) & 255U, (v >> 8) & 255U, v & 255U);
    return LT_TRUE;
}

/* heat-map colour of a context when it uses custom stops */
lt_bool lt_feat_prefs_heat(const lt_app *app, int ctx, double t, lt_color *out)
{
    lt_u32 v = 0;
    int scheme = 0;
    if (!lt_prefs_heat(&app->settings.prefs, ctx, t, &v, &scheme)) { return LT_FALSE; }
    *out = LT_RGB((v >> 16) & 255U, (v >> 8) & 255U, v & 255U);
    return LT_TRUE;
}

/* text for the status bar: the chosen channels with live values; 0 when none are chosen or the
 * app is offline (the caller keeps its default text) */
int lt_feat_statusbar_channels(lt_app *app, char *out, size_t cap)
{
    const lt_prefs *p = &app->settings.prefs;
    int i, n = 0;
    char piece[64];
    if (p->sb_n <= 0 || !(app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) { return 0; }
    out[0] = '\0';
    for (i = 0; i < p->sb_n; i++) {
        double v = 0.0;
        const char *ul = "";
        if (!lt_app_channel_disp(app, p->sb_chan[i], &v, &ul)) { continue; }
        lt_snprintf(piece, sizeof(piece), "%s%s %.*f%s%s", n > 0 ? "   " : "", p->sb_chan[i],
                    (v < 100.0 && v > -100.0) ? 1 : 0, v, ul[0] != '\0' ? " " : "", ul);
        lt_strlcat(out, piece, cap);
        n++;
    }
    return n;
}

/* quit: burn pending changes first when "auto-burn on close" is on; 1 = burned */
int lt_feat_prefs_auto_burn(lt_app *app)
{
    if (!app->settings.prefs.auto_burn_close || !lt_app_can_burn(app)) { return 0; }
    return lt_app_burn(app) == 0;
}

/* ------------------------------------------------------------------ */
/* session                                                             */
static int restorable(const char *kind)
{
    return strcmp(kind, "table") == 0 || strcmp(kind, "curve") == 0 || strcmp(kind, "dialog") == 0;
}

static void session_restore(lt_app *app)
{
    lt_prefs *p = &app->settings.prefs;
    char kind[24], target[128];
    int i, k = 0;
    lt_app_tab *want = NULL;
    if (!p->restore_session || strcmp(p->session_project, app->project.path) != 0) { return; }
    for (i = 0; lt_prefs_session_get(p, i, kind, sizeof(kind), target, sizeof(target)); i++) {
        if (!restorable(kind)) { continue; }
        if (lt_app_open_target(app, target, NULL) == 0) {
            lt_app_tab *t = lt_app_tab_find(app, kind, target);
            if (i == p->last_active) { want = t; }
            k++;
        }
    }
    if (want != NULL) {
        for (i = 0; i < app->n_tabs; i++) {
            if (app->tabs[i] == want) { app->active_tab = i; }
        }
    }
    LT_UNUSED(k);
}

/* tabs -> prefs; returns 1 when the stored session changed */
int lt_feat_prefs_session_record(lt_app *app)
{
    lt_prefs *p = &app->settings.prefs;
    lt_prefs now;
    int i, n = 0, active = -1, changed;
    if (!app->has_project) { return 0; }
    now = *p;
    lt_prefs_session_clear(&now);
    for (i = 0; i < app->n_tabs; i++) {
        const lt_app_tab *t = app->tabs[i];
        if (!restorable(t->kind)) { continue; }
        if (lt_prefs_session_add(&now, t->kind, t->target) == 0) {
            if (i == app->active_tab) { active = n; }
            n++;
        }
    }
    changed = strcmp(now.open_tabs, p->open_tabs) != 0 || active != p->last_active ||
              strcmp(p->session_project, app->project.path) != 0;
    if (changed) {
        lt_prefs_session_clear(p);
        lt_strlcpy(p->open_tabs, now.open_tabs, sizeof(p->open_tabs));
        p->last_active = active;
        lt_strlcpy(p->session_project, app->project.path, sizeof(p->session_project));
    }
    return changed;
}

/* a project was (re)opened: its saved session is restored on the next tick */
void lt_feat_prefs_project_opened(lt_app *app)
{
    pf(app)->seen[0] = '\0';
}

static void prefs_tick(lt_app *app)
{
    pf_state *s = pf(app);
    if (s->demo_req == 1) { s->demo_req = 0; (void)lt_app_start_demo(app, NULL); return; }
    if (s->demo_req == 2) { s->demo_req = 0; lt_app_disconnect(app); return; }
    if (!app->has_project) { s->seen[0] = '\0'; return; }
    if (strcmp(s->seen, app->project.path) != 0) {
        lt_strlcpy(s->seen, app->project.path, sizeof(s->seen));
        session_restore(app);
        return;
    }
    if (app->headless) { return; }
    if (lt_feat_prefs_session_record(app)) { lt_app_settings_save(app); }
}

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
static void caption(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

static void note(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, text, lt_ui_theme(ui)->muted);
}

static lt_color rgb_to_color(lt_u32 v)
{
    return LT_RGB((v >> 16) & 255U, (v >> 8) & 255U, v & 255U);
}

/* custom colour field: checkbox + swatch.  hex is "#rrggbb" or "" */
static int color_field(lt_ui *ui, const char *label, const char *id, char hex[8], lt_u32 def)
{
    int w2[2], on = hex[0] != '\0', ch = 0;
    lt_color c;
    lt_u32 v = def;
    w2[0] = 190; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_push_id(ui, id);
    if (lt_ui_checkbox(ui, label, &on)) {
        if (on) { lt_prefs_format_color(def, hex); } else { hex[0] = '\0'; }
        ch = 1;
    }
    if (hex[0] != '\0') {
        (void)lt_prefs_parse_color(hex, &v);
        c = rgb_to_color(v);
        if (lt_ui_color_edit(ui, "col", &c)) {
            lt_prefs_format_color(((lt_u32)LT_COLOR_R(c) << 16) | ((lt_u32)LT_COLOR_G(c) << 8) |
                                  (lt_u32)LT_COLOR_B(c), hex);
            ch = 1;
        }
    } else {
        lt_ui_label_ex(ui, "Theme colour", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    }
    lt_ui_pop_id(ui);
    return ch;
}

/* a strip previewing the colour ramp of a context */
static void ramp(lt_app *app, int ctx)
{
    lt_ui *ui = app->ui;
    int w1 = -1, i, n = 32;
    lt_rect r;
    lt_ui_layout_row(ui, 1, &w1, 16);
    r = lt_ui_layout_next(ui);
    for (i = 0; i < n; i++) {
        lt_rect q = r;
        double f = (double)i / (double)(n - 1);
        lt_color c;
        q.x = r.x + r.w * i / n;
        q.w = r.w * (i + 1) / n - r.w * i / n;
        if (!lt_feat_prefs_heat(app, ctx, f, &c)) {
            int sc = ctx == 0 ? lt_prefs_scheme_index(app->settings.heatmap)
                              : lt_prefs_scheme_index(app->settings.prefs.heat_scheme[ctx]);
            c = lt_heat_color(sc, f);
        }
        lt_ui_draw_fill(ui, q, c);
    }
}

static const char *const heat_names[] = { "tunerstudio", "grayscale", "pastel", "thermal", "viridis",
                                          "plasma", "custom" };
static const char *const heat_labels[] = { "TunerStudio", "Grayscale", "Pastel", "Thermal", "Viridis",
                                           "Plasma", "Custom colours" };
static const char *const ctx_titles[] = { "Value (VE, timing tables)", "Change (AFR correction size)",
                                          "Coverage (hit weighting)" };

static int heat_context(lt_app *app, int ctx)
{
    lt_ui *ui = app->ui;
    lt_prefs *p = &app->settings.prefs;
    int w1 = -1, sel, ch = 0, i, custom;
    const char *cur = ctx == 0 && strcmp(p->heat_scheme[0], "custom") != 0 ? app->settings.heatmap
                                                                          : p->heat_scheme[ctx];
    char id[16];
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, ctx_titles[ctx], lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
    lt_snprintf(id, sizeof(id), "heat%d", ctx);
    sel = 0;
    for (i = 0; i < 7; i++) { if (strcmp(heat_names[i], cur) == 0) { sel = i; } }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_combo(ui, id, &sel, heat_labels, 7)) {
        lt_strlcpy(p->heat_scheme[ctx], heat_names[sel], sizeof(p->heat_scheme[ctx]));
        if (ctx == 0 && sel < 6) { lt_strlcpy(app->settings.heatmap, heat_names[sel], sizeof(app->settings.heatmap)); }
        if (sel == 6 && p->heat_n[ctx] < 2) {         /* start from a blue-green-red ramp */
            p->heat_stops[ctx][0] = 0x2040C0UL; p->heat_stops[ctx][1] = 0x20C040UL;
            p->heat_stops[ctx][2] = 0xE0D020UL; p->heat_stops[ctx][3] = 0xE02020UL;
            p->heat_n[ctx] = 4;
        }
        ch = 1;
    }
    custom = strcmp(p->heat_scheme[ctx], "custom") == 0;
    if (custom) {
        int rm = -1;
        for (i = 0; i < p->heat_n[ctx]; i++) {
            int w3[3];
            lt_color c = rgb_to_color(p->heat_stops[ctx][i]);
            char lab[24];
            w3[0] = 90; w3[1] = -1; w3[2] = 80;
            lt_ui_layout_row(ui, 3, w3, 0);
            lt_ui_push_id_int(ui, ctx * 10 + i);
            lt_snprintf(lab, sizeof(lab), "Stop %d", i + 1);
            lt_ui_label_ex(ui, lab, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
            if (lt_ui_color_edit(ui, "stop", &c)) {
                p->heat_stops[ctx][i] = ((lt_u32)LT_COLOR_R(c) << 16) | ((lt_u32)LT_COLOR_G(c) << 8) |
                                        (lt_u32)LT_COLOR_B(c);
                ch = 1;
            }
            if (p->heat_n[ctx] > 2 && lt_ui_button(ui, "Remove")) { rm = i; }
            lt_ui_pop_id(ui);
        }
        if (rm >= 0) {
            for (i = rm; i < p->heat_n[ctx] - 1; i++) { p->heat_stops[ctx][i] = p->heat_stops[ctx][i + 1]; }
            p->heat_n[ctx]--;
            ch = 1;
        }
        if (p->heat_n[ctx] < LT_PREFS_HEAT_STOPS) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_push_id_int(ui, ctx);
            if (lt_ui_button(ui, "Add colour stop")) {
                p->heat_stops[ctx][p->heat_n[ctx]] = p->heat_stops[ctx][p->heat_n[ctx] - 1];
                p->heat_n[ctx]++;
                ch = 1;
            }
            lt_ui_pop_id(ui);
        }
    }
    ramp(app, ctx);
    lt_ui_layout_space(ui, 6);
    return ch;
}

static void table_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    lt_prefs *p = &app->settings.prefs;
    double fade;
    int w1 = -1, ch = 0, c;
    caption(ui, "Table display");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Y axis starts at the bottom (new table tabs)", &p->y_bottom)) { ch = 1; }
    if (color_field(ui, "Custom cursor colour", "curcol", p->cursor_color, 0xFF8000UL)) { ch = 1; }
    if (color_field(ui, "Custom trail colour", "trlcol", p->trail_color, 0x3080FFUL)) { ch = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Trail fade (seconds, 0 = never)", lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    fade = p->trail_fade_sec;
    if (lt_ui_number(ui, "fade", &fade, 0, 600, 1, 0) & LT_UI_CHANGED) { p->trail_fade_sec = (int)(fade + 0.5); ch = 1; }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Heat maps");
    for (c = 0; c < LT_PREFS_HEAT_CTX; c++) {
        if (heat_context(app, c)) { ch = 1; }
    }
    note(ui, "Heat map changes apply to table tabs opened afterwards.");
    if (ch) { lt_app_settings_save(app); }
}

static void behavior_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    lt_prefs *p = &app->settings.prefs;
    pf_state *s = pf(app);
    int w1 = -1, ch = 0, demo = app->conn == LT_APP_DEMO, dchg;
    double v;
    caption(ui, "On exit");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Burn pending changes to the ECU when closing", &p->auto_burn_close)) { ch = 1; }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Large change alert");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Warn when an edit changes a cell by a large amount", &p->change_alert)) { ch = 1; }
    if (p->change_alert) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Absolute change greater than", lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        v = p->change_abs;
        if (lt_ui_number(ui, "chgabs", &v, 0, 100000, 1, 2) & LT_UI_CHANGED) { p->change_abs = v; ch = 1; }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Percent change greater than", lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        v = p->change_pct;
        if (lt_ui_number(ui, "chgpct", &v, 0, 10000, 1, 1) & LT_UI_CHANGED) { p->change_pct = v; ch = 1; }
    }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Interface");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Show help icons next to every field", &p->help_icons)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Restore open tabs when a project opens", &p->restore_session)) { ch = 1; }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Demo mode");
    lt_ui_layout_row(ui, 1, &w1, 0);
    dchg = lt_ui_checkbox(ui, "Demo mode (simulated ECU)", &demo);
    if (dchg) { s->demo_req = demo ? 1 : 2; }
    note(ui, "Demo mode replaces the open project with the bundled demo ECU definition.");
    lt_ui_layout_space(ui, 8);
    caption(ui, "Welcome");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Onboarding completed", &p->onboarded)) { ch = 1; }
    if (ch) { lt_app_settings_save(app); }
}

/* ---- status bar channels ---- */
static void load_channels(lt_app *app, pf_state *s)
{
    char **units = NULL;
    if (s->chan_ini == app->ini && s->chan != NULL) { return; }
    if (s->chan != NULL) { lt_strv_free(s->chan, s->n_chan); }
    s->chan = NULL; s->n_chan = 0;
    s->chan_ini = app->ini;
    if (app->ini != NULL) {
        s->n_chan = lt_datalog_channels_from_ini(app->ini, &s->chan, &units);
        if (units != NULL) { lt_strv_free(units, s->n_chan); }
    }
}

/* the four defaults of the original: the first of RPM, AFR, MAP, TPS, CLT ... present */
void lt_feat_statusbar_defaults(lt_prefs *p, char *const *names, size_t n)
{
    static const char *const pref[] = { "RPM", "rpm", "AFR", "afr", "lambda", "MAP", "map", "TPS", "tps",
                                        "coolant", "CLT", "IAT" };
    size_t i, k;
    p->sb_n = 0;
    for (i = 0; i < LT_COUNTOF(pref) && p->sb_n < 4; i++) {
        for (k = 0; k < n; k++) {
            if (strcmp(names[k], pref[i]) == 0) { (void)lt_prefs_sb_toggle(p, pref[i]); break; }
        }
    }
    for (k = 0; k < n && p->sb_n == 0 && k < 4; k++) { (void)lt_prefs_sb_toggle(p, names[k]); }
}

static void status_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    lt_prefs *p = &app->settings.prefs;
    pf_state *s = pf(app);
    int w1 = -1, ch = 0, i, shown = 0, w3[3];
    char lab[80];
    caption(ui, "Status bar channels");
    lt_snprintf(lab, sizeof(lab), "Selected (%d of %d)", p->sb_n, LT_PREFS_SB_MAX);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, lab, lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
    if (p->sb_n == 0) { note(ui, "None: the status bar shows the RPM."); }
    for (i = 0; i < p->sb_n; i++) {
        int up = 0, down = 0, rm = 0;
        w3[0] = -1; w3[1] = 60; w3[2] = 70;
        lt_ui_layout_row(ui, 3, w3, 0);
        lt_ui_push_id_int(ui, i);
        lt_ui_label(ui, p->sb_chan[i]);
        up = i > 0 && lt_ui_button(ui, "Up");
        if (i == 0) { lt_ui_label(ui, ""); }
        rm = lt_ui_button(ui, "Remove");
        (void)down;
        lt_ui_pop_id(ui);
        if (up) {
            char tmp[32];
            lt_strlcpy(tmp, p->sb_chan[i], sizeof(tmp));
            lt_strlcpy(p->sb_chan[i], p->sb_chan[i - 1], sizeof(p->sb_chan[0]));
            lt_strlcpy(p->sb_chan[i - 1], tmp, sizeof(p->sb_chan[0]));
            ch = 1;
        }
        if (rm) { (void)lt_prefs_sb_toggle(p, p->sb_chan[i]); ch = 1; break; }
    }
    w3[0] = -1; w3[1] = 130; w3[2] = 1;
    lt_ui_layout_row(ui, 2, w3, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button(ui, "Use defaults")) {
        load_channels(app, s);
        lt_feat_statusbar_defaults(p, s->chan, s->n_chan);
        ch = 1;
    }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Add a channel");
    lt_ui_layout_row(ui, 1, &w1, 0);
    (void)lt_ui_text(ui, "sbfilter", s->filter, sizeof(s->filter), 0);
    load_channels(app, s);
    if (app->ini == NULL) { note(ui, "Open a project to choose from its output channels."); }
    for (i = 0; i < (int)s->n_chan && shown < 40; i++) {
        int on, full = p->sb_n >= LT_PREFS_SB_MAX;
        if (s->filter[0] != '\0') {
            char a[64], b[64];
            size_t k;
            lt_strlcpy(a, s->chan[i], sizeof(a));
            lt_strlcpy(b, s->filter, sizeof(b));
            for (k = 0; a[k] != '\0'; k++) { if (a[k] >= 'A' && a[k] <= 'Z') { a[k] = (char)(a[k] - 'A' + 'a'); } }
            for (k = 0; b[k] != '\0'; k++) { if (b[k] >= 'A' && b[k] <= 'Z') { b[k] = (char)(b[k] - 'A' + 'a'); } }
            if (strstr(a, b) == NULL) { continue; }
        }
        on = lt_prefs_sb_has(p, s->chan[i]);
        if (on) { continue; }
        shown++;
        w3[0] = -1; w3[1] = 70;
        lt_ui_layout_row(ui, 2, w3, 0);
        lt_ui_push_id_int(ui, 1000 + i);
        lt_ui_label_ex(ui, s->chan[i], lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
        if (lt_ui_button_ex(ui, "Add", full ? LT_UI_BTN_DISABLED : 0) && !full) {
            (void)lt_prefs_sb_toggle(p, s->chan[i]);
            ch = 1;
        }
        lt_ui_pop_id(ui);
    }
    if (ch) { lt_app_settings_save(app); }
}

/* ---- project settings ---- */
static void project_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_proj_config *c;
    int w1 = -1, w2[2], ch = 0, b;
    double v;
    if (!app->has_project) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "Open a project to edit its settings.", t->muted);
        return;
    }
    c = &app->project.cfg;
    caption(ui, "Project");
    w2[0] = 140; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "Name", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, c->name != NULL ? c->name : "", t->text, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "Folder", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, app->project.path, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, "ECU signature", t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, c->signature != NULL ? c->signature : "", t->text, LT_UI_ALIGN_LEFT);
    lt_ui_layout_space(ui, 8);
    caption(ui, "Behaviour");
    lt_ui_layout_row(ui, 1, &w1, 0);
    b = c->settings.auto_load_tune ? 1 : 0;
    if (lt_ui_checkbox(ui, "Load the tune when the project opens", &b)) { c->settings.auto_load_tune = b ? LT_TRUE : LT_FALSE; ch = 1; }
    b = c->settings.auto_save_tune ? 1 : 0;
    if (lt_ui_checkbox(ui, "Save the tune when the project closes", &b)) { c->settings.auto_save_tune = b ? LT_TRUE : LT_FALSE; ch = 1; }
    b = c->settings.auto_connect ? 1 : 0;
    if (lt_ui_checkbox(ui, "Connect to the ECU when the project opens", &b)) {
        c->settings.auto_connect = b ? LT_TRUE : LT_FALSE;
        app->settings.auto_connect = b;
        ch = 1;
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Restore points to keep (0 = all)", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    v = (double)c->settings.max_restore_points;
    if (lt_ui_number(ui, "maxrp", &v, 0, 1000, 1, 0) & LT_UI_CHANGED) {
        c->settings.max_restore_points = (unsigned)(v + 0.5);
        ch = 1;
    }
    if (ch) { (void)lt_project_save_config(&app->project); }
}

static void act_settings(lt_app *app) { (void)lt_app_settings_dialog(app); }

void lt_feat_prefs_register(lt_app_ext *reg)
{
    /* File -> Settings also on the welcome screen, where Tools is not available (G-105) */
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "&Settings...", "Ctrl+,", act_settings, NULL);
    lt_ext_add_tick(reg, prefs_tick);
    lt_ext_add_settings_page(reg, "Table Display", table_page);
    lt_ext_add_settings_page(reg, "Behavior", behavior_page);
    lt_ext_add_settings_page(reg, "Status Bar", status_page);
    lt_ext_add_settings_page(reg, "Project", project_page);
}
