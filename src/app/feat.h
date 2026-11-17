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
void lt_feat_inilib_register(lt_app_ext *reg);    /* Settings > ECU Definitions (INI library) */
void lt_feat_migrate_register(lt_app_ext *reg);   /* Update Project INI, migration report */
void lt_feat_tunediff_register(lt_app_ext *reg);  /* Compare Tune Files, selective merge */
void lt_feat_loggers_register(lt_app_ext *reg);   /* tooth logger, composite logger, ECU console */
/* ask before sending an INI [ControllerCommands] entry (confirmation dialog unless "Always Allow") */
void lt_app_controller_command(lt_app *app, const char *name, const char *label, const char *desc);
void lt_feat_dashboard_register(lt_app_ext *reg); /* dashboard settings, full screen, kiosk */
void lt_feat_history_register(lt_app_ext *reg);   /* Tune History (snapshots, branches), auto-commit */
void lt_feat_restore_register(lt_app_ext *reg);   /* Restore Points dialog */
void lt_feat_annot_register(lt_app_ext *reg);     /* change annotations */
void lt_feat_csv_register(lt_app_ext *reg);       /* tune CSV export / import */
/* history: commit the project (saves the tune first), restore a version, switch branch; tests/scripts */
int  lt_feat_history_commit(lt_app *app, const char *message, const char *note, char id_out[9]);
int  lt_feat_history_restore(lt_app *app, const char *id);
int  lt_feat_history_switch(lt_app *app, const char *branch);
void lt_feat_history_default_message(lt_app *app, char *out, size_t cap);
/* annotations of the open project (keys: constant, table, table:row:col); tag = LT_ANNOT_* or -1 */
size_t lt_feat_annot_count(lt_app *app, const char *table);
int  lt_feat_annot_set(lt_app *app, const char *key, const char *text, int tag);
int  lt_feat_annot_delete(lt_app *app, const char *key);
/* CSV export / import of the open tune */
int  lt_feat_csv_export(lt_app *app, const char *path, size_t *count);
int  lt_feat_csv_import(lt_app *app, const char *path, size_t *imported);
/* switch the open project to another INI (tune re-resolved, project reopened); tests and scripts */
int  lt_feat_migrate_apply(lt_app *app, const char *new_ini);
/* Difference Report "Ignore": project tune is kept, also on disconnect; tests and scripts */
void lt_feat_sync_ignore(lt_app *app);

void lt_feat_logging_register(lt_app_ext *reg);  /* smart recording, sample rate, rec channels, alerts */
void lt_feat_math_register(lt_app_ext *reg);     /* math channels editor */

#endif
