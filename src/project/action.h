/* action.h - action scripting: record, serialise (JSON, adjacently tagged),
 * validate and replay table/constant edits. Spec: docs/spec/09-project.md sec. 8.
 *
 * The recorder is called lt_arec here because datalog.h already owns lt_recorder.
 * Lua actions are stored and validated for non-emptiness but never executed.
 * Errors: 0 or negative LT_E*, message via lt_err_get().
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_ACTION_H
#define LT_ACTION_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../tune/tune.h"

typedef enum lt_act_type {
    LT_ACT_TABLE_EDIT = 0, LT_ACT_CONSTANT_CHANGE, LT_ACT_BULK, LT_ACT_LUA, LT_ACT_PAUSE,
    LT_ACT_SEND_CMD
} lt_act_type;

typedef struct lt_act_cell { unsigned short x, y; } lt_act_cell;   /* x = column, y = row */

typedef struct lt_action {
    lt_act_type type;
    char *table_name, *constant_name, *operation, *script, *description, *command;
    unsigned short x_index, y_index;
    double new_value, old_value; int has_old;
    unsigned long duration_ms;
    lt_act_cell *cells; size_t ncells;
    char **param_keys; double *param_vals; size_t nparams;
    double *old_values; size_t nold; int has_old_values;
} lt_action;

typedef struct lt_action_meta {
    char *created_by, *created_at, *modified_at;
    char **tags; size_t ntags;
    char **compatible_ecus; size_t necus;
} lt_action_meta;

typedef struct lt_action_set {
    char *id, *name, *description, *version;
    lt_action *actions; size_t n;
    lt_action_meta meta;
} lt_action_set;

/* ---- actions ---- */
void lt_action_init(lt_action *a, lt_act_type type);          /* zeroed, strings NULL */
void lt_action_clear(lt_action *a);                           /* frees owned memory */
void lt_action_copy(lt_action *dst, const lt_action *src);    /* deep copy (dst overwritten) */
/* convenience builders (copy their string arguments) */
void lt_action_table_edit(lt_action *a, const char *table, unsigned x, unsigned y,
                          double new_value, int has_old, double old_value);
void lt_action_constant_change(lt_action *a, const char *name, double new_value, int has_old,
                               double old_value);
void lt_action_bulk(lt_action *a, const char *operation, const char *table);
void lt_action_bulk_add_cell(lt_action *a, unsigned x, unsigned y);
void lt_action_bulk_set_param(lt_action *a, const char *key, double value);   /* replaces */
void lt_action_pause(lt_action *a, unsigned long ms);
void lt_action_send_command(lt_action *a, const char *cmd);
void lt_action_lua(lt_action *a, const char *script, const char *description);
const char *lt_action_type_name(lt_act_type t);               /* "TableEdit", ... */

/* ---- sets ---- */
void lt_action_set_init(lt_action_set *s);
void lt_action_set_free(lt_action_set *s);                    /* members only, not s itself */
/* Serialise as pretty JSON (2-space). NULL + err text ("Failed to serialize action set: ...")
 * on non-finite numbers. */
char *lt_action_set_serialize(const lt_action_set *s, char *err, size_t errlen);
/* LT_EDATA with err "Failed to deserialize action set: <msg>" on bad input. */
int   lt_action_set_deserialize(const char *json, lt_action_set *out, char *err, size_t errlen);
int   lt_action_set_save(const lt_action_set *s, const char *path);
int   lt_action_set_load(const char *path, lt_action_set *out);

/* ---- recorder ---- */
typedef struct lt_arec { lt_action_set *cur; } lt_arec;       /* cur NULL = idle */
void  lt_arec_init(lt_arec *r);
void  lt_arec_free(lt_arec *r);
char *lt_arec_start(lt_arec *r, const char *name, const char *desc, const char *created_by);
lt_action_set *lt_arec_stop(lt_arec *r);                      /* caller frees: set_free + lt_free */
int   lt_arec_record(lt_arec *r, const lt_action *a);         /* copies; "No recording in progress" */
const lt_action_set *lt_arec_current(const lt_arec *r);
lt_bool lt_arec_is_recording(const lt_arec *r);
int   lt_arec_add_tag(lt_arec *r, const char *tag);
int   lt_arec_set_compatible_ecus(lt_arec *r, char **ecus, size_t n);

/* ---- validation (spec 8.2) ---- */
/* ini may be NULL (structural checks only). Returns 0 when there are no errors (warnings may
 * still be set) else LT_EINVAL; the error and warning arrays are malloc'd string arrays (free with
 * lt_act_strv_free), either pointer may be NULL to ignore. */
int  lt_action_validate(const lt_action_set *s, const lt_ini *ini, char ***warnings, size_t *nw,
                        char ***errors, size_t *ne);
void lt_act_strv_free(char **v, size_t n);
char *lt_action_summarize(const lt_action_set *s);            /* malloc'd */

/* ---- replay executor (spec 8.4) ---- */
typedef struct lt_replay_env {
    const lt_ini *ini;       /* required for table/constant actions */
    lt_tune *tune;           /* edited in place (pages and named values) */
    void *ud;
    /* SendCommand; NULL = skipped with a warning. Return 0 or a negative LT_E* */
    int  (*send_command)(void *ud, const char *name);
    /* Pause; NULL = lt_plat_sleep_ms */
    void (*sleep_ms)(void *ud, unsigned long ms);
} lt_replay_env;
typedef struct lt_replay_result {
    size_t applied, skipped;
    char **warnings; size_t n_warnings;
} lt_replay_result;
/* Validates first (against env->ini) and refuses on errors (LT_EINVAL, first error in
 * lt_err_get). On an execution error stops and returns it; earlier actions stay applied.
 * Ends by re-capturing named constants from the pages. res may be NULL. */
int  lt_action_replay(const lt_action_set *s, lt_replay_env *env, lt_replay_result *res);
void lt_replay_result_free(lt_replay_result *r);

#endif /* LT_ACTION_H */
