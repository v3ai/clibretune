/* project.h - projects on disk, .properties files, local INI repository,
 *             restore points, snapshot history (git replacement), TunerStudio
 *             project import, sensor calibration curves, pin conflict detection.
 *
 * Spec: docs/spec/09-project.md. Action scripting lives in action.h (included here).
 * Out of scope: online INI repository, git.
 *
 * Conventions: functions return 0 or a negative LT_E* (lt_codes.h) with the
 * message in lt_err_get(); LT_EEXIST for "project already exists", LT_EDATA for
 * a corrupt project.json, LT_EINVAL for rejected file names (path traversal).
 * Time stamps are UTC "YYYY-MM-DDTHH:MM:SSZ" (string comparable).
 *
 * MEMORY: lt_project owns its tune (when loaded) and strings; lt_project_close
 * / lt_project_free release them. List results are malloc'd arrays with
 * matching *_free functions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PROJECT_H
#define LT_PROJECT_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../tune/tune.h"
#include "../json/json.h"
#include "action.h"

/* ====================================================================== */
/* Projects (spec 1)                                                       */
/* ====================================================================== */
typedef struct lt_proj_conn { char *port; unsigned baud_rate, timeout_ms; } lt_proj_conn;   /* port NULL = none */
typedef struct lt_proj_settings {
    lt_bool auto_load_tune, auto_save_tune, auto_connect;
    unsigned max_restore_points;          /* 20; 0 = unlimited */
} lt_proj_settings;
typedef struct lt_proj_config {
    char version[16];                     /* "1.0" */
    char *name, *created, *modified, *ecu_definition, *signature, *dashboard;   /* dashboard may be NULL */
    lt_proj_conn conn;
    lt_proj_settings settings;
} lt_proj_config;

/* Layout: <path>/project.json, CurrentTune.msq, datalogs/, dashboards/,
 * restorePoints/, projectCfg/{definition.ini,pcVariableValues.msq} */
typedef struct lt_project {
    char *path;                           /* project directory */
    lt_proj_config cfg;
    lt_tune *tune;                        /* NULL until loaded */
    lt_bool dirty;                        /* tune changed since last save */
} lt_project;

typedef struct lt_project_info { char *name, *path, *signature, *modified; } lt_project_info;

/* Directory where new projects go: $LIBRETUNE_PROJECTS, else ~/Documents/LibreTuneProjects
 * (if ~/Documents exists) else ~/LibreTuneProjects (via plat.h env helpers). malloc'd. */
char *lt_projects_dir(void);
/* Create <parent>/<safe name>/ with the INI copied to projectCfg/definition.ini.
 * parent NULL = lt_projects_dir(). LT_EEXIST if the directory exists. */
int  lt_project_create(const char *name, const char *ini_src, const char *signature,
                       const char *parent_dir, lt_project *out);
int  lt_project_open(const char *path, lt_project *out);      /* loads the tune if auto_load_tune */
int  lt_project_save_config(lt_project *p);                    /* sets modified=now */
int  lt_project_load_tune(lt_project *p);                      /* success no-op if no CurrentTune.msq */
int  lt_project_save_tune(lt_project *p);                      /* also pcVariableValues.msq */
int  lt_project_close(lt_project *p);                          /* auto-saves per settings; frees */
void lt_project_free(lt_project *p);
char *lt_project_ini_path(const lt_project *p);                /* <path>/projectCfg/definition.ini (malloc'd) */
char *lt_project_file(const lt_project *p, const char *rel);   /* <path>/<rel> (malloc'd) */
/* Load the project's INI (decoded, #includes resolved). NULL + lt_err on failure. */
lt_ini *lt_project_load_ini(const lt_project *p, const lt_ini_options *opts);
/* Newest-modified first; unparseable directories skipped. */
int  lt_project_list(lt_project_info **out, size_t *n, const char *parent_dir);
void lt_project_info_free(lt_project_info *v, size_t n);
/* Import a TunerStudio project directory (spec 1.3). */
int  lt_project_import_ts(const char *ts_dir, const char *target_parent, lt_project *out);

/* ====================================================================== */
/* Java .properties files (spec 2)                                         */
/* ====================================================================== */
typedef struct lt_props {
    char **keys, **vals; size_t n;        /* insertion ordered, unique keys */
    char **header; size_t n_header;       /* leading comment lines, verbatim */
} lt_props;
void lt_props_init(lt_props *p);
void lt_props_free(lt_props *p);
int  lt_props_parse(lt_props *p, const char *text);
int  lt_props_load(lt_props *p, const char *path);             /* UTF-8 else Latin-1 */
int  lt_props_save(const lt_props *p, const char *path);
char *lt_props_to_string(const lt_props *p);
const char *lt_props_get(const lt_props *p, const char *key);  /* NULL if absent */
lt_bool lt_props_get_i32(const lt_props *p, const char *key, int *out);
lt_bool lt_props_get_f64(const lt_props *p, const char *key, double *out);
lt_bool lt_props_get_bool(const lt_props *p, const char *key, lt_bool *out);   /* true/yes/1 */
void lt_props_set(lt_props *p, const char *key, const char *val);
lt_bool lt_props_remove(lt_props *p, const char *key);

/* ====================================================================== */
/* Local INI repository (spec 3)                                           */
/* ====================================================================== */
typedef struct lt_ini_entry { char *id, *name, *signature, *path, *imported, *source; } lt_ini_entry;
typedef struct lt_ini_repo { char *dir; lt_ini_entry *e; size_t n; } lt_ini_repo;
/* dir NULL = $LIBRETUNE_DEFINITIONS, else <data dir>/LibreTune/definitions */
int  lt_ini_repo_open(lt_ini_repo *r, const char *dir);
void lt_ini_repo_close(lt_ini_repo *r);
/* Import a file (BOM/cp1252 -> UTF-8); returns existing id if the signature is known. id malloc'd. */
int  lt_ini_repo_import(lt_ini_repo *r, const char *src_path, char **id_out);
/* the same for n bytes in memory; source is recorded in the index (e.g. a URL) */
int  lt_ini_repo_import_data(lt_ini_repo *r, const char *data, size_t n, const char *source,
                             char **id_out);
const lt_ini_entry *lt_ini_repo_get(const lt_ini_repo *r, const char *id);
const lt_ini_entry *lt_ini_repo_find_signature(const lt_ini_repo *r, const char *signature,
                                               lt_bool prefix_match);   /* for "ECU signature differs" prompts */
char *lt_ini_repo_path(const lt_ini_repo *r, const char *id);           /* malloc'd absolute path */
int  lt_ini_repo_remove(lt_ini_repo *r, const char *id);
/* ids of *.ini files directly inside dir (non recursive). */
int  lt_ini_repo_scan_dir(lt_ini_repo *r, const char *dir, char ***ids, size_t *n);
/* signature and nEmu name from [MegaTune]/[TunerStudio] without a full parse (spec 3). */
int  lt_ini_extract_info(const char *ini_text, char **signature, char **name);

/* ====================================================================== */
/* Restore points (spec 4)                                                 */
/* ====================================================================== */
typedef struct lt_restore_info { char *filename, *path; char created[32]; unsigned long size_bytes; } lt_restore_info;
int  lt_restore_create(lt_project *p, char **path_out);         /* LT_ENOENT if no tune loaded */
int  lt_restore_list(const lt_project *p, lt_restore_info **out, size_t *n);   /* newest first */
void lt_restore_info_free(lt_restore_info *v, size_t n);
/* filename validation: plain component only, must be a regular file inside restorePoints/ */
int  lt_restore_path(const lt_project *p, const char *filename, char **path_out);
/* "<Name>_<YYYY-MM-DD_HH.MM.SS>[-N].msq" -> "YYYY-MM-DDTHH:MM:SSZ"; LT_FALSE if not parseable */
lt_bool lt_restore_parse_timestamp(const char *filename, char out[21]);
int  lt_restore_load(lt_project *p, const char *filename);       /* replaces tune; dirty = true */
int  lt_restore_delete(const lt_project *p, const char *filename);
int  lt_restore_prune(const lt_project *p, size_t keep, size_t *deleted);

/* ====================================================================== */
/* Snapshots: git replacement (spec 5)                                     */
/* ====================================================================== */
typedef struct lt_snap_info {
    char id[9]; char *message, *annotation, *author; char timestamp[20]; lt_bool is_head;
} lt_snap_info;
typedef struct lt_snap_change { char *name, *old_value, *new_value; const char *change_type; } lt_snap_change;
typedef struct lt_snap_diff {
    lt_snap_change *changes; size_t n_changes;     /* tune constant level */
    char **files_changed; size_t n_files;          /* relative paths, "path (added|removed|modified)" */
} lt_snap_diff;
int  lt_snap_commit(const char *proj_dir, const char *message, const char *annotation,
                    const char *author, char id_out[9]);
int  lt_snap_has_changes(const char *proj_dir);
/* content equality that ignores writeDate="..." stamps (1 equal, 0 different) */
int lt_snap_content_equal(const char *a, size_t la, const char *b, size_t lb);
int  lt_snap_history(const char *proj_dir, size_t max, lt_snap_info **out, size_t *n);   /* newest first */
void lt_snap_info_free(lt_snap_info *v, size_t n);
/* to == NULL compares with the working copy */
int  lt_snap_diff_compute(const char *proj_dir, const char *from, const char *to, lt_snap_diff *out);
void lt_snap_diff_free(lt_snap_diff *d);
/* restores files byte exact; LT_EBUSY when !force and the working copy has changes */
int  lt_snap_restore(const char *proj_dir, const char *id, lt_bool force);
int  lt_snap_prune(const char *proj_dir, size_t keep);
char *lt_snap_format_message(const char *msg, const char *note);    /* "msg\n\nLT-Note: note" */

/* Branches: named chains of snapshots (git branch replacement). Commits go onto the current
 * branch (a snapshot records its parent); history/has_changes follow the current branch. A store
 * without branch data has the single branch "main". */
typedef struct lt_snap_branch { char name[64]; char tip[9]; lt_bool is_current; } lt_snap_branch;
lt_bool lt_snap_branch_name_ok(const char *name);        /* [A-Za-z0-9._/-], 1..63, not starting - or . */
int  lt_snap_branches(const char *proj_dir, lt_snap_branch **out, size_t *n);   /* lt_free(*out) */
void lt_snap_branch_current(const char *proj_dir, char out[64]);
/* new branch at the current tip; LT_EEXIST, LT_ENOENT when no snapshot exists yet */
int  lt_snap_branch_create(const char *proj_dir, const char *name);
/* makes `name` current and restores its tip into the working copy; LT_EBUSY when !force and the
 * working copy has changes */
int  lt_snap_branch_switch(const char *proj_dir, const char *name, lt_bool force);
/* version control state of a project: a store exists (snapshots/index.txt) / auto-commit is on
 * (a store exists and was not switched off) */
lt_bool lt_snap_exists(const char *proj_dir);
lt_bool lt_snap_enabled(const char *proj_dir);
int  lt_snap_set_enabled(const char *proj_dir, lt_bool on);
/* commit message from a format with {date} {time} {table}; stamp is "YYYY-MM-DD HH:MM:SS"
 * (lt_plat_timestamp), table may be NULL; malloc'd */
char *lt_snap_expand_format(const char *fmt, const char *table, const char *stamp);

/* ====================================================================== */
/* Sensor calibration curves (spec 6) - used by the calibration dialogs    */
/* ====================================================================== */
#define LT_CALIB_ADC_COUNTS 1024
double lt_calib_adc_to_volts(int adc);                           /* adc*5/1023 */
/* Evaluate a [ReferenceTables] solution expression for adc 0..1023 (variable adcValue).
 * LT_EDATA with a clear message for parse/eval errors; LT_ENOENT "preset needs the lookup
 * file 'x.inc'" if table() cannot be loaded from `cache`. */
int  lt_calib_evaluate_solution(const char *expr, lt_inc_cache *cache, double out[1024]);
int  lt_calib_solution_curve(const lt_ini_ref_table *t, const char *label, lt_inc_cache *cache,
                             double out[1024]);

typedef struct lt_afr_corr { double gain, offset; int npoints; double pts[2][2]; } lt_afr_corr;   /* {measured, expected} */
/* pts = n pairs flattened {measured0, expected0, measured1, expected1} (C89 cannot pass a const
 * double[][2] without casts); n is 1 or 2 */
int  lt_afr_corr_from_points(lt_afr_corr *c, const double *pts, int n);
void lt_afr_corr_apply(const lt_afr_corr *c, double *curve, size_t n);
void lt_afr_corr_describe(const lt_afr_corr *c, char *buf, size_t cap);

typedef struct lt_linear_wb { double v1, afr1, v2, afr2, slope, intercept; } lt_linear_wb;
int    lt_linear_wb_new(lt_linear_wb *w, double v1, double afr1, double v2, double afr2);
double lt_linear_wb_afr(const lt_linear_wb *w, double volts);
void   lt_linear_wb_curve(const lt_linear_wb *w, double out[1024]);
void   lt_linear_wb_describe(const lt_linear_wb *w, char *buf, size_t cap);

/* Steinhart-Hart thermistor fit through three (tempC, ohm) points */
typedef struct lt_therm_fit { double a, b, c, bias_resistor; } lt_therm_fit;
int    lt_therm_fit_points(lt_therm_fit *f, double bias_resistor, const double temp_c[3],
                           const double ohms[3]);
int    lt_therm_fit_option(lt_therm_fit *f, const lt_ini_therm_opt *o);
double lt_therm_temp_at_resistance(const lt_therm_fit *f, double ohms);
double lt_therm_temp_at_adc(const lt_therm_fit *f, int adc);
/* 32 temperature samples at the given ADC bins (for the Speeduino thermistor table upload). */
void   lt_therm_sample_at_bins(const lt_therm_fit *f, const double adc_bins[32], double out[32]);

/* Calibration dialog builders (calib_build.c) */
double lt_calib_c_to_f(double c);
double lt_calib_f_to_c(double f);
/* 3-point Steinhart-Hart fit -> 32 degC samples at the ADC bins of the protocol (modern = x*33,
 * legacy = x*32). LT_EDATA with a message on a bad bias/point set. */
int  lt_calib_therm_curve(double bias, const double temp_c[3], const double ohms[3],
                          lt_bool modern, double temps[32]);
/* AFR curve of an INI preset (preset != NULL) or the two-point line lin = {v1, afr1, v2, afr2}
 * (preset == NULL), then an optional reference correction of ncorr (0..2) {measured, expected}
 * pairs in corr. desc gets a one-line transfer-function description. */
int  lt_calib_afr_build(const lt_ini_ref_table *t, const char *preset, lt_inc_cache *cache,
                        const double lin[4], int ncorr, const double corr[4], double afr[1024],
                        char *desc, size_t cap);
/* true when the curve leaves the 0.0-25.5 AFR the one-byte wire format carries */
lt_bool lt_calib_afr_clips(const double afr[1024]);

/* ====================================================================== */
/* Pin conflict detection (spec 7)                                         */
/* ====================================================================== */
typedef struct lt_pin_conflict { char *pin_label; char **constants; size_t n; } lt_pin_conflict;
typedef struct lt_pin_report { lt_pin_conflict *c; size_t n; } lt_pin_report;
lt_bool lt_pin_is_assignment(const lt_ini_const *c);
lt_bool lt_pin_is_unassigned_label(const char *label);
/* value lookup callback: current option index of constant c, or 0 if unknown */
typedef lt_bool (*lt_pin_value_fn)(void *ud, const lt_ini_const *c, size_t *index);
int  lt_pin_detect(const lt_ini *ini, lt_pin_value_fn fn, void *ud, lt_pin_report *out);
/* would assigning option new_index to constant `name` collide? 1 conflict (out filled), 0 none */
int  lt_pin_conflict_if_assigning(const lt_ini *ini, const char *name, size_t new_index,
                                  lt_pin_value_fn fn, void *ud, lt_pin_conflict *out);
void lt_pin_report_free(lt_pin_report *r);
void lt_pin_conflict_free(lt_pin_conflict *c);
char *lt_pin_summary(const lt_pin_report *r);                     /* "" if none; malloc'd */
/* value callback reading a page source (decodes with the INI) */
lt_bool lt_pin_value_from_pages(void *ud /* const struct lt_pin_pages* */, const lt_ini_const *c,
                                size_t *index);
struct lt_pin_pages { const lt_ini *ini; const lt_pagesrc *src; };

#endif /* LT_PROJECT_H */
