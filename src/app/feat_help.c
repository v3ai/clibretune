/* feat_help.c - Help menu (About, Keyboard Shortcuts), F1, and the shortcuts list as a Settings
 * page.  Worked example of dialog kinds, menu items, shortcuts and settings pages.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

static const char *const shortcut_rows[][2] = {
    { "Ctrl+N", "New project" }, { "Ctrl+O", "Load tune" },
    { "Ctrl+S", "Save tune" }, { "Ctrl+Shift+S", "Save tune as" },
    { "Ctrl+B", "Burn" }, { "Ctrl+Q", "Quit" },
    { "Ctrl+Shift+C", "Connect" }, { "Ctrl+Shift+D", "Disconnect" },
    { "Ctrl+D", "Dashboard" }, { "Ctrl+L", "Data log" },
    { "Ctrl+Shift+A", "AutoTune" }, { "Ctrl+,", "Settings" },
    { "Ctrl+W", "Close tab" }, { "Ctrl+Tab", "Next tab" },
    { "Ctrl+1..9", "Go to tab" }, { "Ctrl+K", "Search the sidebar" },
    { "F9", "Toggle sidebar" }, { "Ctrl+Left/Right", "Resize sidebar (terminal)" }
};

/* key / action pairs, two pairs per row */
static void shortcut_grid(lt_ui *ui)
{
    const lt_theme *t = lt_ui_theme(ui);
    int w[4], i, n = (int)LT_COUNTOF(shortcut_rows);
    w[0] = 128; w[1] = -1; w[2] = 128; w[3] = -1;
    for (i = 0; i < n; i += 2) {
        lt_ui_layout_row(ui, 4, w, 0);
        lt_ui_label_ex(ui, shortcut_rows[i][0], t->primary_hover, LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, shortcut_rows[i][1], t->text, LT_UI_ALIGN_LEFT);
        if (i + 1 < n) {
            lt_ui_label_ex(ui, shortcut_rows[i + 1][0], t->primary_hover, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, shortcut_rows[i + 1][1], t->text, LT_UI_ALIGN_LEFT);
        }
    }
}

static void about_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui_msgbox(app->ui, "about", "About LibreTune",
                 "LibreTune " LT_APP_VERSION " - open source ECU tuning software.\n"
                 "In Universal C. GPL-2.0-or-later.\nBuild: " __DATE__, "OK", &d->open);
}

static void keys_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    int w[2];
    if (lt_ui_modal_begin(ui, "Keyboard shortcuts", 640, 270, &d->open)) {
        shortcut_grid(ui);
        lt_ui_layout_space(ui, 12);
        w[0] = -1; w[1] = 96;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_layout_next(ui);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { d->open = 0; }
        lt_ui_modal_end(ui);
    }
}

static void act_about(lt_app *app) { (void)lt_app_dialog_push(app, "about", "", "About"); }
static void act_keys(lt_app *app) { (void)lt_app_dialog_push(app, "shortcuts", "", "Shortcuts"); }

static void keys_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Global shortcuts", lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    shortcut_grid(ui);
}

void lt_feat_help_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "about"; dk.draw = about_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "shortcuts"; dk.draw = keys_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "Keyboard &Shortcuts", "F1", act_keys, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "&About LibreTune", NULL, act_about, NULL);
    lt_ext_add_shortcut(reg, LT_KEY_F1, 0, act_keys, NULL);
    lt_ext_add_settings_page(reg, "Keyboard Shortcuts", keys_page);
}
