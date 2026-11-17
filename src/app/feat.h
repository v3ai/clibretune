/* feat.h - registration entry points of the feature modules (one per feat_<name>.c).
 * Add one declaration here and one call in app_features.c per new feature.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FEAT_H
#define LT_FEAT_H

#include "app_ext.h"

/* runs every feature registration (app_features.c) */
void lt_app_features_register(lt_app_ext *reg);

void lt_feat_core_register(lt_app_ext *reg);     /* builtin std targets, base map, autotune key */
void lt_feat_help_register(lt_app_ext *reg);     /* About, Keyboard Shortcuts, F1 */
void lt_feat_calib_register(lt_app_ext *reg);    /* sensor calibration: std_ms2gentherm, std_ms2geno2 */
void lt_feat_sync_register(lt_app_ext *reg);     /* signature mismatch, tune sync, connect wizard */
void lt_feat_reconnect_register(lt_app_ext *reg); /* automatic reconnect, auto-connect */

#endif
