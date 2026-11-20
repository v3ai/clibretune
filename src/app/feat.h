/* feat.h - registration entry points of the feature modules (one per feat_<name>.c).
 * Add one declaration here and one call in app_features.c per new feature.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FEAT_H
#define LT_FEAT_H

#include "app_ext.h"

/* runs every feature registration (app_features.c) */
void lt_app_features_register(lt_app_ext *reg);

/* ---- hotkeys (feat_hotkeys.c) ---- */
/* the shortcut of def after the user's changes (key 0 = unbound) */
void lt_hk_effective(lt_app *app, const lt_hk_def *def, int *key, int *mods);
/* a built-in shell action (hotkeys.h table) was pressed this frame */
int  lt_hk_pressed(lt_app *app, const char *id);
/* "Ctrl+S" of an action for menu hints (valid until a few later calls); "" when unbound */
const char *lt_hk_text(lt_app *app, const char *id);
/* "Save tune (Ctrl+S)": translated English text plus the current shortcut of id */
const char *lt_hk_tip(lt_app *app, const char *english, const char *id);
/* the editor is waiting for a key: global shortcuts are suspended */
int  lt_hk_capturing(lt_app *app);
/* Edit menu / hotkey actions routed to the active tab; id is "edit.undo" ... "edit.select_all" */
void lt_feat_edit_do(lt_app *app, const char *id);

void lt_feat_i18n_register(lt_app_ext *reg);      /* Settings > Language */
void lt_feat_manual_register(lt_app_ext *reg);    /* User Manual viewer, F1 context help */
void lt_feat_manual_open(lt_app *app, const char *page_id);   /* NULL = last page */
void lt_feat_manual_context(lt_app *app);          /* the page of the active tab (F1) */
void lt_feat_hotkeys_register(lt_app_ext *reg);   /* Settings > Keyboard Shortcuts, Edit routing */
/* tab management (feat_tabs.c): close respecting protected tabs, drag/context menu of the tab bar */
void lt_feat_tabs_close_active(lt_app *app);
int  lt_feat_tabs_move(lt_app *app, int from, int to);              /* reorder; active tab follows */
int  lt_feat_tabs_close_many(lt_app *app, int mode, int idx);       /* 0 others, 1 to the right, 2 all */
void lt_feat_tabs_protect(lt_app *app, int idx, int on);
int  lt_feat_tabs_at(const lt_rect *rects, int n, int x);
void lt_feat_tabs_bar(lt_app *app, const lt_rect *rects, int n);   /* drag + context menu */
void lt_feat_hotkeys_grid(lt_app *app);           /* effective shortcut list (Shortcuts dialog) */
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

/* assistant chat tab, Settings > Assistant, MCP server (G-123 local form, G-124) */
struct lt_ag_env;
void lt_feat_assistant_register(lt_app_ext *reg);
int  lt_feat_assistant_env(lt_app *app, struct lt_ag_env *env);   /* tool view of the app; 0 = ini open */
int  lt_feat_mcp_stdio(lt_app *app);                              /* `libretune --mcp`: serve stdin/stdout */
/* Lua console (G-120) and the rusEFI ECU script editor (G-121) */
void lt_feat_lua_register(lt_app_ext *reg);

/* tools batch (G-80, G-81, G-82, G-35) */
void lt_feat_tablecmp_register(lt_app_ext *reg);  /* Table Comparison */
void lt_feat_dyno_register(lt_app_ext *reg);      /* Dyno Overlay */
void lt_feat_actions_register(lt_app_ext *reg);   /* Action Recorder / manager */
void lt_feat_perf_register(lt_app_ext *reg);      /* Performance Calculator */
/* action recording: the editors report their edits (v: cells changed since the last commit are
 * recorded, display units; constants with the old and new value); no-ops unless recording */
int  lt_feat_actions_recording(lt_app *app);
void lt_feat_actions_note_table(lt_app *app, const char *table, const lt_tblview *v);
void lt_feat_actions_note_constant(lt_app *app, const char *name, double old_v, double new_v);
/* scripting: start recording (0 / error), stop and save the set into the actions folder (count = recorded),
 * play a saved .ltaction on the open tune with the pauses shortened (applied = actions applied) */
int  lt_feat_actions_start(lt_app *app, const char *name, const char *desc);
int  lt_feat_actions_stop(lt_app *app, size_t *count);
int  lt_feat_actions_play_file(lt_app *app, const char *path, size_t *applied);

/* Tools > Calculators (feat_calc.c): expression, linear calibration, units, injectors; eval = the
 * Expression tab on the open app (0 ok, -1 error with the message in out) */
void lt_feat_calc_register(lt_app_ext *reg);
int  lt_feat_calc_eval(lt_app *app, const char *expr, char *out, size_t cap);

#endif
