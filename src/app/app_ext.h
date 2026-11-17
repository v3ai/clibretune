/* app_ext.h - shell extension registry (see docs/ARCHITECTURE.md, "Adding a feature").
 *
 * A feature module owns one src/app/feat_<name>.c with ONE registration function
 *     void lt_feat_<name>_register(lt_app_ext *reg);
 * declared in feat.h and called from app_features.c (one line per feature).  Inside it
 * the feature registers any of: std/menu targets, menu items, global shortcuts, settings
 * pages, per-frame tick hooks, overlays (modals drawn every frame), dialog kinds and tab
 * kinds.  Per-feature state lives in lt_ext_state(), not in lt_app.
 * All tables are plain static-style arrays; callbacks are ordinary function pointers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_APP_EXT_H
#define LT_APP_EXT_H

#include "app.h"

/* returned by lt_ext_dispatch_target when no handler is registered for the name */
#define LT_EXT_NO_TARGET 1

typedef enum lt_ext_menu {
    LT_EXT_MENU_FILE = 0, LT_EXT_MENU_VIEW, LT_EXT_MENU_TOOLS, LT_EXT_MENU_HELP, LT_EXT_MENU_COUNT
} lt_ext_menu;

/* open handler of a menu/std target; returns 0 ok, negative error */
typedef int  (*lt_ext_target_fn)(lt_app *app, const char *target);
typedef void (*lt_ext_action_fn)(lt_app *app);
/* capability predicate: nonzero = available (item shown / shortcut active) */
typedef int  (*lt_ext_pred_fn)(lt_app *app);
typedef void (*lt_ext_draw_fn)(lt_app *app);
/* releases a feature state block (the block itself is freed by the registry) */
typedef void (*lt_ext_free_fn)(lt_app *app, void *state);

/* stock predicates */
int lt_ext_pred_ini(lt_app *app);        /* an ECU definition is open */
int lt_ext_pred_project(lt_app *app);    /* a project is open */
int lt_ext_pred_online(lt_app *app);     /* connected to an ECU or the demo */

/* ---- registration (call from lt_feat_<name>_register only) ---- */
/* a later registration of the same name replaces the earlier one */
void lt_ext_add_target(lt_app_ext *reg, const char *name, lt_ext_target_fn open_fn);
/* menu item; shortcut is display text only ("Ctrl+K" or NULL); pred NULL = always */
void lt_ext_add_menu_item(lt_app_ext *reg, int menu, const char *label, const char *shortcut,
                          lt_ext_action_fn fn, lt_ext_pred_fn pred);
/* global shortcut (key as lt_ui_key_pressed, mods LT_MOD_*); pred NULL = always */
void lt_ext_add_shortcut(lt_app_ext *reg, int key, int mods, lt_ext_action_fn fn,
                         lt_ext_pred_fn pred);
/* Settings dialog page: draw runs inside the scrolled body with a 1-column full-width
 * layout row already set; save settings yourself when something changed */
void lt_ext_add_settings_page(lt_app_ext *reg, const char *title, lt_ext_draw_fn draw);
/* called at the start of every lt_app_service (every frame, also when offline) */
void lt_ext_add_tick(lt_app_ext *reg, lt_ext_draw_fn fn);
/* called every frame while dialogs are drawn: draw your own modal (lt_ui_msgbox,
 * lt_ui_modal_begin) guarded by a flag kept in your state block */
void lt_ext_add_overlay(lt_app_ext *reg, lt_ext_draw_fn fn);
/* called once after a successful connect (serial, TCP or demo), when the ECU pages are read and
 * the app is online; use it for the signature check, the tune sync choice, auto actions */
void lt_ext_add_on_connect(lt_app_ext *reg, lt_ext_draw_fn fn);
/* called when the link drops unexpectedly, just BEFORE the app disconnects (app->conn is still
 * online, app->settings.port holds the port) */
void lt_ext_add_on_link_lost(lt_app_ext *reg, lt_ext_draw_fn fn);
/* convenience wrappers over lt_app_register_dialog_kind / _tab_kind */
void lt_ext_add_dialog_kind(lt_app_ext *reg, const lt_app_dialog_kind *k);
void lt_ext_add_tab_kind(lt_app_ext *reg, const lt_app_tab_kind *k);

/* ---- per-feature state ---- */
/* Zero-initialised block of `size` bytes owned by the registry, created on the first call
 * for `key` and returned again afterwards (size must stay the same).  cleanup (may be NULL)
 * runs from lt_app_free.  Usable at any time, e.g. state = lt_ext_state(app, "calib", ...). */
void *lt_ext_state(lt_app *app, const char *key, size_t size, lt_ext_free_fn cleanup);

/* ---- used by the shell and by tests ---- */
lt_app_ext *lt_ext_new(lt_app *app);     /* also runs lt_app_features_register */
void        lt_ext_free(lt_app *app);
/* handler lookup; LT_EXT_NO_TARGET when unknown, else the handler result */
int  lt_ext_dispatch_target(lt_app *app, const char *target);
int  lt_ext_has_target(const lt_app *app, const char *target);
int  lt_ext_n_menu_items(const lt_app *app, int menu);
const char *lt_ext_menu_item_label(const lt_app *app, int menu, int idx);
/* draw the registered items of a menu (those whose predicate holds); a separator first when
 * lead_sep and at least one item is shown; returns the number of items shown */
int  lt_ext_menu_draw(lt_app *app, int menu, int lead_sep);
void lt_ext_shortcuts(lt_app *app);
void lt_ext_tick(lt_app *app);
void lt_ext_on_connect(lt_app *app);
void lt_ext_on_link_lost(lt_app *app);
void lt_ext_overlays(lt_app *app);
int  lt_ext_n_settings_pages(const lt_app *app);
const char *lt_ext_settings_title(const lt_app *app, int idx);
void lt_ext_settings_draw(lt_app *app, int idx);

#endif
