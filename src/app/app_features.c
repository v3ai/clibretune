/* app_features.c - the feature list: ONLY registration calls, one line per feature.
 * New features add their declaration to feat.h and one line here (append at the end).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

void lt_app_features_register(lt_app_ext *reg)
{
    lt_feat_core_register(reg);
    lt_feat_manual_register(reg);     /* before Help: User Manual comes first in the menu */
    lt_feat_help_register(reg);
    lt_feat_calib_register(reg);
    lt_feat_sync_register(reg);
    lt_feat_reconnect_register(reg);
    lt_feat_inilib_register(reg);
    lt_feat_migrate_register(reg);
    lt_feat_tunediff_register(reg);
    lt_feat_logging_register(reg);
    lt_feat_math_register(reg);
    lt_feat_loggers_register(reg);
    lt_feat_dashboard_register(reg);
    lt_feat_history_register(reg);
    lt_feat_restore_register(reg);
    lt_feat_annot_register(reg);
    lt_feat_csv_register(reg);
    lt_feat_error_register(reg);
    lt_feat_online_register(reg);
    lt_feat_import_register(reg);
    lt_feat_tunes_register(reg);
    lt_feat_connset_register(reg);
    lt_feat_prefs_register(reg);
    lt_feat_afrdelay_register(reg);
    lt_feat_assistant_register(reg);
    lt_feat_hotkeys_register(reg);
    lt_feat_i18n_register(reg);
    lt_feat_lua_register(reg);
    lt_feat_tablecmp_register(reg);
    lt_feat_dyno_register(reg);
    lt_feat_actions_register(reg);
    lt_feat_perf_register(reg);
    lt_feat_calc_register(reg);
    lt_feat_msqpart_register(reg);
    lt_feat_triggers_register(reg);
}
