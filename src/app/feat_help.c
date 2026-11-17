/* feat_help.c - Help menu (About, Keyboard Shortcuts), F1, and the shortcuts list as a Settings
 * page.  Worked example of dialog kinds, menu items, shortcuts and settings pages.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

static void about_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui_msgbox(app->ui, "about", "About LibreTune",
                 "LibreTune " LT_APP_VERSION " - open source ECU tuning software.\n"
                 "In Universal C. GPL-2.0-or-later.\nBuild: " __DATE__, "OK", &d->open);
}

static void keys_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    int w[2], w1 = -1;
    if (lt_ui_modal_begin(ui, lt_tr("Keyboard shortcuts"), 720, 430, &d->open)) {
        lt_ui_layout_row(ui, 1, &w1, -36);
        if (lt_ui_scroll_begin(ui, "keysbody")) {
            lt_feat_hotkeys_grid(app);
            lt_ui_scroll_end(ui);
        }
        w[0] = -1; w[1] = 96;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_layout_next(ui);
        if (lt_ui_button_ex(ui, lt_tr("Close"), LT_UI_BTN_DEFAULT)) { d->open = 0; }
        lt_ui_modal_end(ui);
    }
}

static void act_about(lt_app *app) { (void)lt_app_dialog_push(app, "about", "", "About"); }
static void act_keys(lt_app *app) { (void)lt_app_dialog_push(app, "shortcuts", "", "Shortcuts"); }

void lt_feat_help_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "about"; dk.draw = about_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "shortcuts"; dk.draw = keys_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "Keyboard &Shortcuts", NULL, act_keys, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "&About LibreTune", NULL, act_about, NULL);
}
