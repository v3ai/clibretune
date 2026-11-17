/* dialogs_settings.c - Settings dialog (built-in pages + pages registered through app_ext.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"
#include "app_ext.h"
#include "feat.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* Settings                                                            */
static const char *const themes[] = { "dark", "light", "midnight", "carbon", "synthwave",
                                      "solarized", "nord", "dracula", "highcontrast" };
static const char *const backends[] = { "auto", "x11", "win32", "web", "tty" };
static const char *const heatmaps[] = { "tunerstudio", "grayscale", "pastel", "thermal", "viridis", "plasma" };
static const char *const temps[] = { "Celsius", "Fahrenheit", "Kelvin" };
static const char *const press[] = { "kPa", "psi", "bar", "inHg" };
static const char *const afrs[] = { "AFR", "Lambda" };
static const char *const scales[] = { "Auto", "100%", "125%", "150%", "175%", "200%", "250%", "300%" };
static const int scale_pcts[] = { 0, 100, 125, 150, 175, 200, 250, 300 };

static int find_str(const char *const *a, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++) { if (strcmp(a[i], s) == 0) { return i; } }
    return 0;
}

static void swatch_row(lt_ui *ui, const char *name, const int *w2)
{
    lt_theme t;
    lt_rect r;
    lt_color cs[5];
    int i;
    if (lt_theme_preset(&t, name) != 0) { return; }
    lt_ui_layout_row(ui, 2, w2, 20);
    lt_ui_label(ui, lt_tr(""));
    r = lt_ui_layout_next(ui);
    cs[0] = t.bg_app; cs[1] = t.surface; cs[2] = t.primary; cs[3] = t.accent; cs[4] = t.text;
    for (i = 0; i < 5; i++) {
        lt_rect q = r;
        q.x = r.x + i * 48; q.w = 44;
        lt_ui_draw_box(ui, q, cs[i], t.border, lt_ui_px(ui, 2));
    }
}

/* bold caption above a full-width field (stock settings form) */
static void form_label(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

static void form_note(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS, LT_UI_FS_SM);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

static const char *const set_tabs[] = { "General", "Appearance", "Connection" };
#define N_SET_BUILTIN 3
#define N_SET_MAX 24

/* words the search matches besides the section title (G-92) */
typedef struct set_kw { const char *title, *words; } set_kw;
static const set_kw keywords[] = {
    { "General", "units metric imperial temperature pressure air fuel ratio afr lambda celsius fahrenheit kpa psi bar" },
    { "Appearance", "theme dark light colour color heat map scale zoom ecu menus menu bar" },
    { "Connection", "port baud serial auto connect packet mode burst och adaptive timing fome metrics frames "
                    "latency project realtime rate hz backend" },
    { "Keyboard Shortcuts", "hotkeys keys bindings" },
    { "Dashboard", "gauges refresh kiosk full screen" },
    { "ECU Definitions", "ini library definitions import signature" },
    { "Version Control", "history commit snapshot branch auto message" },
    { "Reconnect", "reconnect firmware controller command retry" },
    { "Data Logging", "record log alert rules channels sample rate auto record" },
    { "Table Display", "y axis cursor trail fade heat map colours stops gradient value change coverage" },
    { "Behavior", "burn close exit alert large change threshold help icons session restore tabs demo "
                  "simulated onboarding welcome" },
    { "Status Bar", "channels live values rpm status" },
    { "Project", "project name folder restore points auto save load connect" }
};

static const char *words_of(const char *title)
{
    size_t i;
    for (i = 0; i < LT_COUNTOF(keywords); i++) {
        if (strcmp(keywords[i].title, title) == 0) { return keywords[i].words; }
    }
    return "";
}

/* does section `title` match the search text? (empty matches all) */
lt_bool lt_dlg_settings_match(const char *title, const char *query)
{
    return lt_dlg_contains_ci(title, query) || lt_dlg_contains_ci(words_of(title), query);
}

typedef struct set_dlg { char q[48]; const char *titles[N_SET_MAX]; int vis[N_SET_MAX]; } set_dlg;

static const char *set_item(void *user, int i)
{
    const set_dlg *sd = (const set_dlg *)user;
    return lt_tr(sd->titles[sd->vis[i]]);
}

/* returns 1 when a setting changed */
static int page_builtin(lt_app *app, int tab)
{
    lt_ui *ui = app->ui;
    lt_app_settings *st = &app->settings;
    int w1 = -1, sel, ch = 0, wf[2];
    double hz;
    switch (tab) {
    case 0:
        form_label(ui, lt_tr("Unit system"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, lt_tr("Metric (\xC2\xB0" "C, kPa)"), &st->metric)) {
            st->temp_unit = st->metric ? 0 : 1;
            st->press_unit = st->metric ? 0 : 1;
            ch = 1;
        }
        lt_ui_layout_space(ui, 8);
        form_label(ui, lt_tr("Temperature"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = LT_CLAMP(st->temp_unit, 0, 2);
        if (lt_ui_combo(ui, "temp", &sel, temps, 3)) { st->temp_unit = sel; ch = 1; }
        form_label(ui, lt_tr("Pressure"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = LT_CLAMP(st->press_unit, 0, 3);
        if (lt_ui_combo(ui, "press", &sel, press, 4)) { st->press_unit = sel; ch = 1; }
        form_label(ui, lt_tr("Air-Fuel Ratio"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = LT_CLAMP(st->afr_unit, 0, 1);
        if (lt_ui_combo(ui, "afr", &sel, afrs, 2)) { st->afr_unit = sel; ch = 1; }
        break;
    case 1:
        form_label(ui, lt_tr("Theme"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = find_str(themes, 9, st->theme);
        if (lt_ui_combo(ui, "theme", &sel, themes, 9)) { lt_app_apply_theme(app, themes[sel]); ch = 1; }
        wf[0] = 0; wf[1] = -1;
        swatch_row(ui, themes[sel], wf);
        lt_ui_layout_space(ui, 8);
        form_label(ui, lt_tr("Heat map"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = find_str(heatmaps, 6, st->heatmap);
        if (lt_ui_combo(ui, "heat", &sel, heatmaps, 6)) {
            lt_strlcpy(st->heatmap, heatmaps[sel], sizeof(st->heatmap));
            lt_strlcpy(st->prefs.heat_scheme[0], heatmaps[sel], sizeof(st->prefs.heat_scheme[0]));
            ch = 1;
        }
        form_label(ui, lt_tr("UI scale"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = 0;
        { int si; for (si = 0; si < 8; si++) { if (scale_pcts[si] == st->scale_pct) { sel = si; } } }
        if (lt_ui_combo(ui, "scale", &sel, scales, 8)) { lt_app_set_scale_setting(app, scale_pcts[sel]); ch = 1; }
        lt_ui_layout_space(ui, 8);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, lt_tr("Show ECU menus in the menu bar"), &st->show_ecu_menus)) { ch = 1; }
        break;
    default:
        form_label(ui, lt_tr("Realtime rate (Hz)"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        hz = st->poll_hz;
        if (lt_ui_number(ui, "hz", &hz, 5, 100, 1, 0) & LT_UI_CHANGED) { st->poll_hz = (int)(hz + 0.5); ch = 1; }
        lt_ui_layout_space(ui, 8);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_checkbox(ui, lt_tr("Connect automatically when a project opens"), &st->auto_connect)) { ch = 1; }
        lt_ui_layout_space(ui, 8);
        form_label(ui, lt_tr("Graphics backend"));
        lt_ui_layout_row(ui, 1, &w1, 0);
        sel = find_str(backends, 5, st->backend);
        if (lt_ui_combo(ui, "backend", &sel, backends, 5)) { lt_strlcpy(st->backend, backends[sel], sizeof(st->backend)); ch = 1; }
        form_note(ui, lt_tr("The backend change applies at the next start."));
        lt_feat_connset_draw(app);
        break;
    }
    return ch;
}

static void settings_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    int open = d->open, w1 = -1, ch = 0, fh, wc[2], wq[2];
    int *tab = &app->settings_tab;
    set_dlg *sd = (set_dlg *)d->data;
    int nt, ni, nv, k, cur = 0;
    if (sd == NULL) {
        sd = (set_dlg *)lt_calloc(1, sizeof(*sd));
        d->data = sd;
    }
    if (lt_ui_modal_begin(ui, lt_tr("Settings"), 780, 580, &open)) {
        fh = lt_ui_cell_mode(ui) ? 16 : 30;
        for (nt = 0; nt < N_SET_BUILTIN; nt++) { sd->titles[nt] = set_tabs[nt]; }
        for (ni = 0; ni < lt_ext_n_settings_pages(app) && nt < N_SET_MAX; ni++) {
            sd->titles[nt++] = lt_ext_settings_title(app, ni);
        }
        if (*tab >= nt || *tab < 0) { *tab = 0; }
        wq[0] = 70; wq[1] = -1;
        lt_ui_layout_row(ui, 2, wq, 0);
        lt_ui_label(ui, lt_tr("Search"));
        (void)lt_ui_text(ui, "setsearch", sd->q, sizeof(sd->q), 0);
        nv = 0;
        for (k = 0; k < nt; k++) {
            if (lt_dlg_settings_match(sd->titles[k], sd->q)) {
                if (k == *tab) { cur = nv; }
                sd->vis[nv++] = k;
            }
        }
        if (nv > 0 && sd->vis[cur] != *tab) { *tab = sd->vis[0]; cur = 0; }
        wc[0] = 190; wc[1] = -1;
        /* terminal: one row per section (the body scrolls) so the modal fits its
         * content instead of filling the screen; pixel mode fills the modal */
        lt_ui_layout_row(ui, 2, wc, lt_ui_cell_mode(ui) ? (nt + 2) * lt_ui_row_height(ui) : -(fh + 8));
        lt_ui_layout_column_begin(ui);
        lt_ui_layout_row(ui, 1, &w1, -1);
        if (nv > 0) {
            if (lt_ui_listbox_cb(ui, "setnav", &cur, nv, set_item, sd) & LT_UI_CHANGED) { *tab = sd->vis[cur]; }
        } else {
            lt_ui_label_ex(ui, lt_tr("No matching section"), lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_layout_column_end(ui);
        lt_ui_layout_column_begin(ui);
        lt_ui_layout_row(ui, 1, &w1, -1);
        if (lt_ui_scroll_begin(ui, "setbody")) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (*tab < N_SET_BUILTIN) { ch = page_builtin(app, *tab); }
            else { lt_ext_settings_draw(app, *tab - N_SET_BUILTIN); }
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_column_end(ui);
        wc[0] = -1; wc[1] = 96;
        lt_ui_layout_row(ui, 2, wc, fh);
        lt_ui_layout_next(ui);
        if (lt_ui_button_ex(ui, lt_tr("Close"), LT_UI_BTN_DEFAULT)) { open = 0; }
        if (ch) { lt_app_settings_save(app); }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void settings_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

void lt_dlg_register_settings(lt_app *app)
{
    lt_app_dialog_kind dk;
    dk.kind = "settings"; dk.draw = settings_draw; dk.close = settings_close;
    lt_app_register_dialog_kind(app, &dk);
}
