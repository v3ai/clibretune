/* feat_core.c - builtin std targets, the Tools "Generate Base Map" item and the AutoTune key,
 * registered through the same registry as every other feature (worked example).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

static int open_dash(lt_app *app, const char *t) { LT_UNUSED(t); return lt_app_dashboard_tab_open(app); }
static int open_autotune(lt_app *app, const char *t) { LT_UNUSED(t); return lt_app_autotune_open(app); }
static int open_log(lt_app *app, const char *t) { LT_UNUSED(t); return lt_app_datalog_open(app, 0); }
static int open_viewer(lt_app *app, const char *t) { LT_UNUSED(t); return lt_app_datalog_open(app, 1); }
static int open_special(lt_app *app, const char *t) { return lt_app_special_open(app, t); }

static void act_basemap(lt_app *app) { (void)lt_app_basemap_wizard(app); }
static void act_autotune(lt_app *app) { (void)lt_app_autotune_open(app); }

void lt_feat_core_register(lt_app_ext *reg)
{
    lt_ext_add_target(reg, "std_realtime", open_dash);
    lt_ext_add_target(reg, "autotune", open_autotune);
    lt_ext_add_target(reg, "datalog", open_log);
    lt_ext_add_target(reg, "datalog-viewer", open_viewer);
    lt_ext_add_target(reg, "och-status", open_special);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Generate &Base Map...", NULL, act_basemap,
                         lt_ext_pred_ini);
    lt_ext_add_shortcut(reg, 'a', LT_MOD_CTRL | LT_MOD_SHIFT, act_autotune, lt_ext_pred_ini);
}
