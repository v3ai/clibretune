/* app_features.c - the feature list: ONLY registration calls, one line per feature.
 * New features add their declaration to feat.h and one line here (append at the end).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "feat.h"

void lt_app_features_register(lt_app_ext *reg)
{
    lt_feat_core_register(reg);
    lt_feat_help_register(reg);
    lt_feat_calib_register(reg);
}
