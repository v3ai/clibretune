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
void lt_feat_afrdelay_register(lt_app_ext *reg);  /* AFR delay test dialog */

/* connection/projects/files/prefs batch (G-05, G-06, G-17, G-18, G-19, G-92..G-96, G-102, G-104) */
void lt_feat_error_register(lt_app_ext *reg);     /* error details dialog */
void lt_feat_online_register(lt_app_ext *reg);    /* Search for INI Online (assisted) */
void lt_feat_import_register(lt_app_ext *reg);    /* TunerStudio import wizard */
void lt_feat_tunes_register(lt_app_ext *reg);     /* Open Tune / New Tune */
void lt_feat_connset_register(lt_app_ext *reg);   /* project connection, protocol tuning, metrics */
void lt_feat_prefs_register(lt_app_ext *reg);     /* table display, behaviour, status bar, project pages */
/* error dialog: title/message/details, or from the protocol layer (code, CRC, page/offset) */
void lt_feat_error_show(lt_app *app, const char *title, const char *message, const char *details);
void lt_feat_error_proto(lt_app *app, const char *title);
void lt_feat_error_describe(const lt_proto_err *e, char *out, size_t cap);
void lt_feat_error_report(const char *title, const char *msg, const char *det, char *out, size_t cap);
const char *lt_feat_error_code_name(int code);
/* INI online: open the dialog (signature may be ""); import counter lets other dialogs refresh */
void lt_feat_online_open(lt_app *app, const char *signature);
int  lt_feat_online_generation(lt_app *app);
/* TunerStudio import: the wizard; the import itself (ini_path "" = the project's own INI) */
void lt_feat_import_ts_wizard(lt_app *app, const char *picked_path);
int  lt_feat_import_ts_with_ini(lt_app *app, const char *root, const char *ini_path);
void lt_feat_import_ts_root(const char *picked, char *root, size_t cap);
/* New Tune: default-valued tune saved as a new file of the project and made current */
int  lt_feat_tune_new(lt_app *app, const char *name, char *out, size_t cap);
/* connection: project settings -> app settings (called when a project opens); settings dialog block;
 * metrics dialog */
void lt_feat_connset_apply(lt_app *app);
void lt_feat_connset_draw(lt_app *app);
void lt_feat_connset_metrics(lt_app *app);
/* prefs: table colours (trail 0/1) and custom heat-map colour of a context (0 value, 1 change,
 * 2 coverage); status bar channel text; burn before quit; session recording (tests) */
lt_bool lt_feat_prefs_color(const lt_app *app, int trail, lt_color *out);
lt_bool lt_feat_prefs_heat(const lt_app *app, int ctx, double t, lt_color *out);
int  lt_feat_statusbar_channels(lt_app *app, char *out, size_t cap);
int  lt_feat_prefs_auto_burn(lt_app *app);
int  lt_feat_prefs_session_record(lt_app *app);
void lt_feat_prefs_project_opened(lt_app *app);   /* a project was (re)opened: restore its tabs */
void lt_feat_statusbar_defaults(lt_prefs *p, char *const *names, size_t n);

/* Lua console (G-120) and the rusEFI ECU script editor (G-121) */
void lt_feat_lua_register(lt_app_ext *reg);

#endif
