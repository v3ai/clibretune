/* feat_help.c - Help menu (About, Keyboard Shortcuts), F1, and the shortcuts list as a Settings
 * page.  Worked example of dialog kinds, menu items, shortcuts and settings pages.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

static const char shortcut_text[] =
    "Ctrl+N new project    Ctrl+O load tune    Ctrl+S save    Ctrl+Shift+S save as\n"
    "Ctrl+B burn    Ctrl+Q quit    Ctrl+Shift+C connect    Ctrl+Shift+D disconnect\n"
    "Ctrl+D dashboard    Ctrl+L log    Ctrl+Shift+A autotune    Ctrl+, settings\n"
    "Ctrl+W close tab    Ctrl+Tab next tab    Ctrl+1..9 tab    Ctrl+K search    F9 sidebar";

static void about_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui_msgbox(app->ui, "about", "About LibreTune",
                 "LibreTune " LT_APP_VERSION " - open source ECU tuning software.\n"
                 "In Universal C. GPL-2.0-or-later.\nBuild: " __DATE__, "OK", &d->open);
}

static void keys_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui_msgbox(app->ui, "keys", "Keyboard shortcuts", shortcut_text, "Close", &d->open);
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
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, shortcut_text, lt_ui_theme(ui)->text2);
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
