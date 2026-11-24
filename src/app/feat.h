/* feat.h - registration entry points of the feature modules (one per feat_<name>.c).
 * Add one declaration here and one call in app_features.c per new feature.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FEAT_H
#define LT_FEAT_H

#include "app_ext.h"
#include "ports_logic.h"

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
/* "Replace Definition..." for a project that does not open because its INI is missing or cannot be
 * read: the definition picker (bundled, library, file) for the project folder dir; the choice is
 * copied into the project, which is then opened. _apply does that directly (tests, scripts). */
void lt_feat_migrate_replace(lt_app *app, const char *dir);
int  lt_feat_migrate_replace_apply(lt_app *app, const char *dir, const char *new_ini);
/* migration report dialog for rep (a tune file against an INI, lt_migration_compare_tune); the report
 * is taken over (rep is left empty). intro: one paragraph above the report. on_continue (may be NULL)
 * runs from the dialog when the user presses "Continue with Tune" (Cancel runs nothing); it must only
 * queue work (a tick hook does it), not replace the session. */
void lt_feat_migrate_show(lt_app *app, const char *title, const char *intro, lt_migration_report *rep,
                          lt_ext_action_fn on_continue);
/* File > Load Tune: a tune saved by other firmware, or one whose values do not map onto the open
 * definition, shows the migration report first and loads on "Continue with Tune". 0 loaded,
 * 1 the report is shown, <0 error (toasted). */
int  lt_feat_load_tune_checked(lt_app *app, const char *path);
/* Difference Report "Ignore": project tune is kept, also on disconnect; tests and scripts */
void lt_feat_sync_ignore(lt_app *app);

/* What lt_feat_sync_connect_auto did. */
typedef struct lt_feat_auto {
    int  quality;            /* LT_INIDB_EXACT / LT_INIDB_NEAR (project/inidb.h), 0 none */
    int  created;            /* 1 a new project was created, 0 an existing one was opened */
    char signature[128];     /* the ECU's signature */
    char label[200];         /* the definition used ("rusEFI proteus_f4 - release_20260812 (bundled)") */
    char project[512];       /* project directory */
} lt_feat_auto;
/* One call for the welcome / connect screens once a port answers:
 *   1. identify: signature as given (the caller probed the port) or, when NULL/"", probe port at
 *      baud (lt_disc_run); port "demo" = the simulator
 *   2. pick the definition (lt_inidb_resolve): the ECU's USB drive, the INI library, the bundle;
 *      exact, else the nearest build of the same board
 *   3. open the newest project whose definition has exactly that signature, or create one
 *      ("rusEFI proteus_f4", "Speeduino 202501"; a number is appended when the name is taken)
 *   4. connect (demo: the simulator answers with the picked definition)
 * Returns LT_INIDB_EXACT (one toast names the definition) or LT_INIDB_NEAR (a one-line warning
 * with "Find Exact INI..." is shown), or negative: LT_ENOENT when no definition fits (the Connect
 * ECU wizard opens at its definition step: browse for a file / search online), LT_ETIMEOUT when
 * nothing answered on port, other LT_E* when the project or the link failed (lt_err_get). res
 * (may be NULL) is filled in every case. */
int  lt_feat_sync_connect_auto(lt_app *app, const char *port, long baud, const char *signature,
                               lt_feat_auto *res);

void lt_feat_logging_register(lt_app_ext *reg);  /* smart recording, sample rate, rec channels, alerts */
/* sample rate and file format of the Data Logging settings into app->rec (before a recording starts) */
void lt_feat_logging_apply(lt_app *app);
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
int  lt_feat_mcp_stdio(lt_app *app);                              /* `clibretune --mcp`: serve stdin/stdout */
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

/* AutoTune analyzers: WUE Analyze (warm-up curve) and Trim Table Analyze (per-cylinder trims) */
void lt_feat_wue_register(lt_app_ext *reg);
void lt_feat_trim_register(lt_app_ext *reg);
/* Tools > Firmware Update (USB DFU / OpenBLT flashing, miata-ecus wideband) */
void lt_feat_firmware_register(lt_app_ext *reg);
/* 0 idle, 1 flashing an ECU through its bootloader, 2 flashing the on-board wideband through
 * the ECU connection (quit, close and disconnect must wait) */
int  lt_feat_firmware_busy(lt_app *app);
/* Tools > Calculators (feat_calc.c): expression, linear calibration, units, injectors; eval = the
 * Expression tab on the open app (0 ok, -1 error with the message in out) */
void lt_feat_calc_register(lt_app_ext *reg);
int  lt_feat_calc_eval(lt_app *app, const char *expr, char *out, size_t cap);

/* File > Save / Load Partial Tune (.msqpart, feat_msqpart.c). save: the named constants of the
 * open tune; apply_file: only the values in the file (the previous ones become the Undo copy);
 * undo: restore them; open_load: the Load dialog with the preview of path (confirmation) */
void lt_feat_msqpart_register(lt_app_ext *reg);
int  lt_feat_msqpart_save(lt_app *app, const char *path, const char *const *names, size_t n);
int  lt_feat_msqpart_apply_file(lt_app *app, const char *path, size_t *applied, size_t *skipped);
int  lt_feat_msqpart_undo(lt_app *app);
int  lt_feat_msqpart_can_undo(lt_app *app);
void lt_feat_msqpart_open_load(lt_app *app, const char *path);

/* Tools > Action Triggers (feat_triggers.c): rules over the live channels that run an action.
 * rules: the loaded set (project or configuration folder); eval: check every rule at now_ms and
 * run the actions of those that fire (returns how many); fire: run the action of rule i now
 * (0 / -1); last: the latest firing log line; save: write the rules (0 ok) */
struct lt_trig_set;
void   lt_feat_triggers_register(lt_app_ext *reg);
void   lt_feat_triggers_open(lt_app *app);
struct lt_trig_set *lt_feat_triggers_rules(lt_app *app);
size_t lt_feat_triggers_eval(lt_app *app, double now_ms);
int    lt_feat_triggers_fire(lt_app *app, size_t i);
const char *lt_feat_triggers_last(lt_app *app);
int    lt_feat_triggers_save(lt_app *app);
/* INI extras: help topics, pin conflict report + burn gate, power cycle notice, project INI
 * settings, port editor (feat_iniext.c; API in iniext.h) */
void lt_feat_iniext_register(lt_app_ext *reg);

/* Base Tune library browser (feat_basetune.c, tunes in tune/basetune.h). open: mode "new" (new
 * project), "load" (into the open definition), "" (load when a Speeduino INI is open, else new).
 * load: base tune b into the open definition/project, values converted to it (0 / -1);
 * new_project: create project `name` (NULL/"" = the tune's name) for ini_path, load b, save it */
struct lt_basetune;
void lt_feat_basetune_register(lt_app_ext *reg);
void lt_feat_basetune_open(lt_app *app, const char *mode);
int  lt_feat_basetune_load(lt_app *app, const struct lt_basetune *b);
int  lt_feat_basetune_new_project(lt_app *app, const struct lt_basetune *b, const char *ini_path,
                                  const char *name);

/* ECUs this computer has talked to and the serial ports right now (feat_devices.c, logic in
 * ports_logic.h). mem: devices.conf (port, baud, signature, project per USB serial number).
 * ports: ranked USB ports, cached for a second (never opens a port); generation changes when
 * the list does. info: identity of a port name now (0 = not listed). detected: the likeliest
 * ECU plugged in (1) and the existing project it was last used with (or ""). connect: the
 * start screen's "Connect to my ECU": open that project and connect, else the Connect wizard. */
void lt_feat_devices_register(lt_app_ext *reg);
const lt_ecuport_mem *lt_feat_devices_mem(lt_app *app);
int  lt_feat_devices_ports(lt_app *app, const lt_ecuport **out, int *n_hidden);
unsigned long lt_feat_devices_generation(lt_app *app);
int  lt_feat_devices_info(lt_app *app, const char *port, lt_serial_info *out);
int  lt_feat_devices_detected(lt_app *app, lt_ecuport *port, char *project, size_t pcap);
void lt_feat_devices_connect(lt_app *app);

/* the start screen shown while no project is open (feat_start.c) */
void lt_feat_start_register(lt_app_ext *reg);
void lt_feat_start_draw(lt_app *app, lt_rect area);
/* take a folder off Recent Projects (the folder is not touched); 1 when it was listed */
int  lt_feat_start_forget(lt_app *app, const char *dir);

/* A project that cannot be opened, or opens with a definition that looks incomplete
 * (feat_projerr.c): a dialog that says what is wrong in plain words and where, with a way out.
 *   LT_PROJERR_PROJECT     project.json missing, empty or invalid; or the folder is gone:
 *                          Show Folder, Remove from Recent Projects, Close
 *   LT_PROJERR_DEFINITION  the project's INI is missing or cannot be read: Replace Definition...
 *                          (lt_feat_migrate_replace), Show Folder, Remove from Recent Projects, Close
 *   LT_PROJERR_INCOMPLETE  the INI parsed but looks incomplete (lt_ini_looks_incomplete); the project
 *                          is open: Replace Definition... (Update Project INI), Continue anyway
 * dir = project folder, file = the file at fault (may equal dir), msg = what is wrong. */
enum { LT_PROJERR_PROJECT = 0, LT_PROJERR_DEFINITION, LT_PROJERR_INCOMPLETE };
enum { LT_PROJERR_ACT_SHOW_FOLDER = 1, LT_PROJERR_ACT_FORGET, LT_PROJERR_ACT_REPLACE, LT_PROJERR_ACT_CLOSE };
void lt_feat_projerr_register(lt_app_ext *reg);
lt_app_dialog *lt_feat_projerr_show(lt_app *app, int kind, const char *dir, const char *file,
                                    const char *msg);
/* what a projerr dialog shows: kind (or -1 when d is not one), message and file; NULL-safe outs */
int  lt_feat_projerr_info(const lt_app_dialog *d, const char **msg, const char **file);
/* run a button of the dialog (what a click does); 0 done, -1 not available in this dialog */
int  lt_feat_projerr_act(lt_app *app, lt_app_dialog *d, int action);

/* Never lose work (feat_guard.c): unburned / unsaved state in the status bar, the questions
 * before quit / close project / disconnect, autosave of the working tune and the offer to
 * restore it after a crash. unburned: named settings in ECU RAM that differ from what was last
 * burned (or read at connect); -1 when not connected. quit/close/disconnect: do it now, or ask
 * first when changes would be lost. autosave_now: write the recovery file (0 ok, 1 nothing to
 * do); recovery_path: where it is for the open project (malloc'd, NULL without a project). */
void lt_feat_guard_register(lt_app_ext *reg);
int  lt_feat_guard_unburned(lt_app *app);
void lt_feat_guard_quit(lt_app *app);
void lt_feat_guard_close_project(lt_app *app);
void lt_feat_guard_disconnect(lt_app *app);
/* something is about to replace the open project (open another, new project, import, demo):
 * ask like close does when changes would be lost, then call fn(app, arg) - at once when nothing
 * would be lost, after Burn / Save / "Close without saving", never after Cancel. The project is
 * not closed here: fn opening another one closes it, fn opening a dialog leaves it open. */
typedef void (*lt_feat_guard_fn)(lt_app *app, const char *arg);
void lt_feat_guard_switch(lt_app *app, lt_feat_guard_fn fn, const char *arg);
void lt_feat_guard_statusbar(lt_app *app, lt_rect r);
int  lt_feat_guard_statusbar_width(lt_app *app);
int  lt_feat_autosave_now(lt_app *app);
char *lt_feat_autosave_path(lt_app *app);
void lt_feat_autosave_discard(lt_app *app);

/* Engine Setup checklist tab (feat_setup.c, logic in setup_logic.h) */
void lt_feat_setup_register(lt_app_ext *reg);

#endif
