/* dialogs_settings.c - Settings dialog (built-in pages + pages registered through app_ext.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"
#include "app_ext.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* Settings                                                            */
static const char *const themes[] = { "dark", "light", "midnight", "carbon", "synthwave",
                                      "solarized", "nord", "dracula", "highcontrast" };
static const char *const backends[] = { "auto", "x11", "win32", "web", "tty" };
static const char *const heatmaps[] = { "tunerstudio", "grayscale", "pastel", "thermal", "viridis", "plasma" };
static const char *const temps[] = { "Celsius", "Fahrenheit" };
static const char *const press[] = { "kPa", "psi", "bar" };
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
    lt_ui_label(ui, "");
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

static void settings_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    lt_app_settings *st = &app->settings;
    int open = d->open, w1 = -1, sel, ch = 0, wf[2], fh;
    double hz;
    int *tab = &app->settings_tab;
    const char *titles[N_SET_MAX];
    int nt, ni;
    if (lt_ui_modal_begin(ui, "Settings", 600, 560, &open)) {
        fh = lt_ui_cell_mode(ui) ? 16 : 30;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_layout_gap(ui, 0, 0);
        for (nt = 0; nt < N_SET_BUILTIN; nt++) { titles[nt] = set_tabs[nt]; }
        for (ni = 0; ni < lt_ext_n_settings_pages(app) && nt < N_SET_MAX; ni++) {
            titles[nt++] = lt_ext_settings_title(app, ni);
        }
        if (*tab >= nt) { *tab = 0; }
        lt_ui_tabs_flat(ui, "settabs", titles, nt, tab);
        lt_ui_layout_row(ui, 1, &w1, -(fh + 8));
        if (lt_ui_scroll_begin(ui, "setbody")) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            switch (*tab) {
            case 0:
                form_label(ui, "Unit system");
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Metric (\xC2\xB0" "C, kPa)", &st->metric)) {
                    st->temp_unit = st->metric ? 0 : 1;
                    st->press_unit = st->metric ? 0 : 1;
                    ch = 1;
                }
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Temperature");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->temp_unit, 0, 1);
                if (lt_ui_combo(ui, "temp", &sel, temps, 2)) { st->temp_unit = sel; ch = 1; }
                form_label(ui, "Pressure");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->press_unit, 0, 2);
                if (lt_ui_combo(ui, "press", &sel, press, 3)) { st->press_unit = sel; ch = 1; }
                form_label(ui, "Air-Fuel Ratio");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->afr_unit, 0, 1);
                if (lt_ui_combo(ui, "afr", &sel, afrs, 2)) { st->afr_unit = sel; ch = 1; }
                break;
            case 1:
                form_label(ui, "Theme");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(themes, 9, st->theme);
                if (lt_ui_combo(ui, "theme", &sel, themes, 9)) { lt_app_apply_theme(app, themes[sel]); ch = 1; }
                wf[0] = 0; wf[1] = -1;
                swatch_row(ui, themes[sel], wf);
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Heat map");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(heatmaps, 6, st->heatmap);
                if (lt_ui_combo(ui, "heat", &sel, heatmaps, 6)) { lt_strlcpy(st->heatmap, heatmaps[sel], sizeof(st->heatmap)); ch = 1; }
                form_label(ui, "UI scale");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = 0;
                { int si; for (si = 0; si < 8; si++) { if (scale_pcts[si] == st->scale_pct) { sel = si; } } }
                if (lt_ui_combo(ui, "scale", &sel, scales, 8)) { lt_app_set_scale_setting(app, scale_pcts[sel]); ch = 1; }
                lt_ui_layout_space(ui, 8);
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Show ECU menus in the menu bar", &st->show_ecu_menus)) { ch = 1; }
                break;
            case 2:
                form_label(ui, "Realtime rate (Hz)");
                lt_ui_layout_row(ui, 1, &w1, 0);
                hz = st->poll_hz;
                if (lt_ui_number(ui, "hz", &hz, 5, 100, 1, 0) & LT_UI_CHANGED) { st->poll_hz = (int)(hz + 0.5); ch = 1; }
                lt_ui_layout_space(ui, 8);
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Connect automatically when a project opens", &st->auto_connect)) { ch = 1; }
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Graphics backend");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(backends, 5, st->backend);
                if (lt_ui_combo(ui, "backend", &sel, backends, 5)) { lt_strlcpy(st->backend, backends[sel], sizeof(st->backend)); ch = 1; }
                form_note(ui, "The backend change applies at the next start.");
                break;
            default:
                lt_ext_settings_draw(app, *tab - N_SET_BUILTIN);
                break;
            }
            lt_ui_scroll_end(ui);
        }
        wf[0] = -1; wf[1] = 96;
        lt_ui_layout_row(ui, 2, wf, fh);
        lt_ui_layout_next(ui);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        if (ch) { lt_app_settings_save(app); }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

void lt_dlg_register_settings(lt_app *app)
{
    lt_app_dialog_kind dk;
    dk.kind = "settings"; dk.draw = settings_draw; dk.close = NULL;
    lt_app_register_dialog_kind(app, &dk);
}
