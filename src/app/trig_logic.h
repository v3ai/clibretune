/* trig_logic.h - user action triggers (TunerStudio "Action Management"): rules of the form
 * "when <expression over live channels> becomes true [and stays true for hold_s] -> action".
 *
 * Edge semantics: a rule fires on a false -> true transition only. It starts waiting for the
 * condition to be false, so a condition that is already true when the rules load (or are
 * enabled, or edited) does not fire until it has been false once. After firing the rule re-arms
 * when the condition is false again AND rearm_s seconds have passed since the firing; a rule
 * marked `once` fires a single time until it is reset. With hold_s > 0 the condition must stay
 * true that long without interruption. An expression that fails to compile or evaluate counts
 * as false (the message is kept in err).
 *
 * The rules are stored as Java properties (trigger.count, trigger.N.name / expr / enabled /
 * hold / rearm / once / action / arg). Pure logic, no UI: feat_triggers.c runs the rules on the
 * realtime frames and performs the actions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TRIG_LOGIC_H
#define LT_TRIG_LOGIC_H

#include "../lt.h"
#include "../ini/expr.h"
#include "../project/project.h"

typedef enum lt_trig_act {
    LT_TRIG_MESSAGE = 0,   /* arg: text of the notification */
    LT_TRIG_BEEP,          /* arg: unused */
    LT_TRIG_MARKER,        /* arg: marker text (data log must be recording) */
    LT_TRIG_LOG_START,
    LT_TRIG_LOG_STOP,
    LT_TRIG_DASHBOARD,     /* arg: dashboard file (.dash) or a name in the dashboards folder */
    LT_TRIG_MSQPART,       /* arg: .msqpart file, applied after confirmation */
    LT_TRIG_SCRIPT,        /* arg: recorded action set (.ltaction) */
    LT_TRIG_N
} lt_trig_act;

enum { LT_TRIG_WAIT_FALSE = 0, LT_TRIG_ARMED, LT_TRIG_HOLDING, LT_TRIG_DONE };

typedef struct lt_trig_rule {
    char name[64];
    char expr[256];
    char arg[256];
    int enabled, action, once;
    double hold_s, rearm_s;
    /* runtime (not stored) */
    int state;
    double t_true, t_fired;            /* ms */
    unsigned long fires;
    int last;                          /* last condition: 1, 0, or -1 for an error */
    lt_expr *compiled;
    char compiled_src[256];
    char err[96];
} lt_trig_rule;

typedef struct lt_trig_set {
    lt_trig_rule *r;
    size_t n;
    int enabled;                       /* master switch */
} lt_trig_set;

/* stable file key ("message", "beep", "marker", "log_start", "log_stop", "dashboard", "msqpart",
 * "script") and English label of an action; find returns -1 for an unknown key */
const char *lt_trig_act_key(int a);
const char *lt_trig_act_label(int a);
int         lt_trig_act_find(const char *key);

void lt_trig_set_init(lt_trig_set *s);
void lt_trig_set_free(lt_trig_set *s);
/* a new enabled rule with defaults (pointer valid until the next add/remove) */
lt_trig_rule *lt_trig_add(lt_trig_set *s);
void lt_trig_remove(lt_trig_set *s, size_t i);
/* forget the runtime state: wait for a false condition, fire count kept */
void lt_trig_reset(lt_trig_rule *r);

/* one step of the state machine with the condition value at now_ms; 1 when the rule fires */
int  lt_trig_step(lt_trig_rule *r, int cond, double now_ms);
/* the condition now: 1 / 0, or -1 when it does not compile or evaluate (message in r->err);
 * recompiles (and resets the rule) when the expression text changed */
int  lt_trig_eval(lt_trig_rule *r, const lt_expr_env *env);
/* evaluate and step every enabled rule (nothing when the set is disabled); the indexes of the
 * rules that fired are stored in fired (up to cap); returns how many fired */
size_t lt_trig_run(lt_trig_set *s, const lt_expr_env *env, double now_ms, size_t *fired, size_t cap);

/* persistence; load replaces the rules (a missing file gives an empty, enabled set: 0) */
void lt_trig_to_props(const lt_trig_set *s, lt_props *p);
void lt_trig_from_props(lt_trig_set *s, const lt_props *p);
int  lt_trig_save(const lt_trig_set *s, const char *path);
int  lt_trig_load(lt_trig_set *s, const char *path);

#endif
