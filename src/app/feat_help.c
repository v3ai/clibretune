/* feat_help.c - Help menu (Keyboard Shortcuts, Check for Updates, About), F1, and the shortcuts
 * list as a Settings page.  Worked example of dialog kinds, menu items, shortcuts and settings pages.
 * Check for Updates does not download anything: it shows the running version and build and opens
 * the releases page in the browser, where new versions are published.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

static void about_draw(lt_app *app, lt_app_dialog *d)
{
    char msg[320];
    lt_snprintf(msg, sizeof(msg), "LibreTune " LT_APP_VERSION " - open source ECU tuning software.\n"
                "In Universal C. GPL-2.0-or-later.\nBuild: %s (" __DATE__ ")\nUI scale: %d%%",
                lt_app_build_id(), app->scale_pct);
    lt_ui_msgbox(app->ui, "about", "About LibreTune", msg, "OK", &d->open);
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

#define LT_RELEASES_URL "https://github.com/v3ai/clibretune/releases"

typedef struct upd_state { char msg[200]; int err; } upd_state;

static upd_state *upd_get(lt_app *app)
{
    return (upd_state *)lt_ext_state(app, "updates", sizeof(upd_state), NULL);
}

static void updates_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    upd_state *s = upd_get(app);
    int w[3], w1 = -1, cell = lt_ui_cell_mode(ui);
    char b[200];
    if (lt_ui_modal_begin(ui, lt_tr("Check for Updates"), 640, 330, &d->open)) {
        w[0] = 160; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label_ex(ui, lt_tr("Version:"), t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_label(ui, "LibreTune " LT_APP_VERSION);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label_ex(ui, lt_tr("Build:"), t->text2, LT_UI_ALIGN_LEFT);
        lt_snprintf(b, sizeof(b), "%s (" __DATE__ ")", lt_app_build_id());
        lt_ui_label(ui, b);
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label_ex(ui, lt_tr("System:"), t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_label(ui, lt_plat_os_name());
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label(ui, "");
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, lt_tr("LibreTune does not update itself. New versions are published on the releases "
                                   "page: compare the version there with the one above, then download and install "
                                   "the new build the usual way for your system."), t->text);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, LT_RELEASES_URL, t->primary, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, cell ? -20 : -40);
        lt_ui_label_ex(ui, s->msg, s->err ? t->error : t->muted, LT_UI_ALIGN_LEFT);
        w[0] = -1; w[1] = 200; w[2] = 96;
        lt_ui_layout_row(ui, 3, w, 0);
        if (lt_ui_button(ui, lt_tr("Copy link"))) {
            lt_ui_clipboard_set(ui, LT_RELEASES_URL);
            lt_strlcpy(s->msg, lt_tr("Link copied to the clipboard"), sizeof(s->msg));
            s->err = 0;
        }
        if (lt_ui_button_ex(ui, lt_tr("Open releases page"), LT_UI_BTN_PRIMARY)) {
            if (lt_plat_open_url(LT_RELEASES_URL) == 0) {
                lt_strlcpy(s->msg, lt_tr("Opened in the web browser"), sizeof(s->msg));
                s->err = 0;
            } else {
                lt_strlcpy(s->msg, lt_tr("Cannot start a web browser here: copy the link instead"), sizeof(s->msg));
                s->err = 1;
            }
        }
        if (lt_ui_button_ex(ui, lt_tr("Close"), LT_UI_BTN_DEFAULT)) { d->open = 0; }
        lt_ui_modal_end(ui);
    }
    if (!d->open) { s->msg[0] = '\0'; }
}

static void act_updates(lt_app *app) { (void)lt_app_dialog_push(app, "updates", "", "Check for Updates"); }
static void act_about(lt_app *app) { (void)lt_app_dialog_push(app, "about", "", "About"); }
static void act_keys(lt_app *app) { (void)lt_app_dialog_push(app, "shortcuts", "", "Shortcuts"); }

void lt_feat_help_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "about"; dk.draw = about_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "shortcuts"; dk.draw = keys_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "updates"; dk.draw = updates_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "Keyboard &Shortcuts", NULL, act_keys, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "Check for &Updates...", NULL, act_updates, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "&About LibreTune", NULL, act_about, NULL);
}
