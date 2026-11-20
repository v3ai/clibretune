/* trig_logic.c - action trigger rules: edge detection, hold time, re-arm, storage (trig_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "trig_logic.h"

static const char *const g_keys[LT_TRIG_N] = {
    "message", "beep", "marker", "log_start", "log_stop", "dashboard", "msqpart", "script"
};
static const char *const g_labels[LT_TRIG_N] = {
    "Show a message", "Sound alert (beep)", "Add a data log marker", "Start data logging",
    "Stop data logging", "Switch dashboard", "Load a partial tune (.msqpart)", "Run an action script"
};

const char *lt_trig_act_key(int a)
{
    return a >= 0 && a < LT_TRIG_N ? g_keys[a] : "";
}

const char *lt_trig_act_label(int a)
{
    return a >= 0 && a < LT_TRIG_N ? g_labels[a] : "";
}

int lt_trig_act_find(const char *key)
{
    int i;
    for (i = 0; key != NULL && i < LT_TRIG_N; i++) {
        if (strcmp(key, g_keys[i]) == 0) { return i; }
    }
    return -1;
}

void lt_trig_set_init(lt_trig_set *s)
{
    s->r = NULL;
    s->n = 0;
    s->enabled = 1;
}

static void rule_free(lt_trig_rule *r)
{
    if (r->compiled != NULL) { lt_expr_free(r->compiled); r->compiled = NULL; }
    r->compiled_src[0] = '\0';
}

void lt_trig_set_free(lt_trig_set *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) { rule_free(&s->r[i]); }
    lt_free(s->r);
    s->r = NULL;
    s->n = 0;
}

void lt_trig_reset(lt_trig_rule *r)
{
    r->state = LT_TRIG_WAIT_FALSE;
    r->t_true = 0.0;
    r->t_fired = 0.0;
    r->last = 0;
}

lt_trig_rule *lt_trig_add(lt_trig_set *s)
{
    lt_trig_rule *r;
    s->r = (lt_trig_rule *)lt_realloc(s->r, (s->n + 1) * sizeof(lt_trig_rule));
    r = &s->r[s->n++];
    memset(r, 0, sizeof(*r));
    lt_snprintf(r->name, sizeof(r->name), "Rule %lu", (unsigned long)s->n);
    r->enabled = 1;
    r->action = LT_TRIG_MESSAGE;
    lt_trig_reset(r);
    return r;
}

void lt_trig_remove(lt_trig_set *s, size_t i)
{
    if (i >= s->n) { return; }
    rule_free(&s->r[i]);
    if (i + 1 < s->n) { memmove(&s->r[i], &s->r[i + 1], (s->n - i - 1) * sizeof(lt_trig_rule)); }
    s->n--;
}

int lt_trig_step(lt_trig_rule *r, int cond, double now_ms)
{
    switch (r->state) {
    case LT_TRIG_WAIT_FALSE:
        if (!cond && (r->fires == 0 || now_ms - r->t_fired >= r->rearm_s * 1000.0)) {
            r->state = LT_TRIG_ARMED;
        }
        return 0;
    case LT_TRIG_ARMED:
        if (!cond) { return 0; }
        r->t_true = now_ms;
        if (r->hold_s > 0.0) {
            r->state = LT_TRIG_HOLDING;
            return 0;
        }
        break;
    case LT_TRIG_HOLDING:
        if (!cond) {
            r->state = LT_TRIG_ARMED;
            return 0;
        }
        if (now_ms - r->t_true < r->hold_s * 1000.0) { return 0; }
        break;
    default:   /* LT_TRIG_DONE */
        return 0;
    }
    r->fires++;
    r->t_fired = now_ms;
    r->state = r->once ? LT_TRIG_DONE : LT_TRIG_WAIT_FALSE;
    return 1;
}

int lt_trig_eval(lt_trig_rule *r, const lt_expr_env *env)
{
    lt_bool b = LT_FALSE;
    if (r->compiled == NULL || strcmp(r->compiled_src, r->expr) != 0) {
        rule_free(r);
        lt_strlcpy(r->compiled_src, r->expr, sizeof(r->compiled_src));
        r->err[0] = '\0';
        lt_trig_reset(r);
        if (r->expr[0] == '\0') {
            lt_strlcpy(r->err, "No condition", sizeof(r->err));
        } else {
            r->compiled = lt_expr_compile(r->expr, r->err, sizeof(r->err));
            if (r->compiled == NULL && r->err[0] == '\0') { lt_strlcpy(r->err, "Syntax error", sizeof(r->err)); }
        }
    }
    if (r->compiled == NULL) { r->last = -1; return -1; }
    if (lt_expr_eval_bool(r->compiled, env, &b) != 0) {
        lt_strlcpy(r->err, lt_err_get(), sizeof(r->err));
        r->last = -1;
        return -1;
    }
    r->err[0] = '\0';
    r->last = b ? 1 : 0;
    return r->last;
}

size_t lt_trig_run(lt_trig_set *s, const lt_expr_env *env, double now_ms, size_t *fired, size_t cap)
{
    size_t i, n = 0;
    if (!s->enabled) { return 0; }
    for (i = 0; i < s->n; i++) {
        lt_trig_rule *r = &s->r[i];
        int c;
        if (!r->enabled) { continue; }
        c = lt_trig_eval(r, env);
        if (lt_trig_step(r, c > 0, now_ms)) {
            if (fired != NULL && n < cap) { fired[n] = i; }
            n++;
        }
    }
    return n;
}

/* ---- storage ---- */
static void put_num(lt_props *p, const char *key, double v)
{
    char b[48];
    lt_fmt_double(b, sizeof(b), v, 3);
    lt_props_set(p, key, b);
}

void lt_trig_to_props(const lt_trig_set *s, lt_props *p)
{
    char k[64], b[24];
    size_t i;
    lt_snprintf(b, sizeof(b), "%lu", (unsigned long)s->n);
    lt_props_set(p, "trigger.count", b);
    lt_props_set(p, "trigger.enabled", s->enabled ? "1" : "0");
    for (i = 0; i < s->n; i++) {
        const lt_trig_rule *r = &s->r[i];
        lt_snprintf(k, sizeof(k), "trigger.%lu.name", (unsigned long)i); lt_props_set(p, k, r->name);
        lt_snprintf(k, sizeof(k), "trigger.%lu.expr", (unsigned long)i); lt_props_set(p, k, r->expr);
        lt_snprintf(k, sizeof(k), "trigger.%lu.enabled", (unsigned long)i); lt_props_set(p, k, r->enabled ? "1" : "0");
        lt_snprintf(k, sizeof(k), "trigger.%lu.hold", (unsigned long)i); put_num(p, k, r->hold_s);
        lt_snprintf(k, sizeof(k), "trigger.%lu.rearm", (unsigned long)i); put_num(p, k, r->rearm_s);
        lt_snprintf(k, sizeof(k), "trigger.%lu.once", (unsigned long)i); lt_props_set(p, k, r->once ? "1" : "0");
        lt_snprintf(k, sizeof(k), "trigger.%lu.action", (unsigned long)i); lt_props_set(p, k, lt_trig_act_key(r->action));
        lt_snprintf(k, sizeof(k), "trigger.%lu.arg", (unsigned long)i); lt_props_set(p, k, r->arg);
    }
}

static void get_str(const lt_props *p, const char *key, char *dst, size_t cap)
{
    const char *v = lt_props_get(p, key);
    if (v != NULL) { lt_strlcpy(dst, v, cap); }
}

static double get_num(const lt_props *p, const char *key, double lo, double hi)
{
    double d = 0.0;
    if (!lt_props_get_f64(p, key, &d) || !lt_isfinite(d)) { return 0.0; }
    return d < lo ? lo : (d > hi ? hi : d);
}

void lt_trig_from_props(lt_trig_set *s, const lt_props *p)
{
    int n = 0, i, v;
    char k[64];
    lt_trig_set_free(s);
    s->enabled = 1;
    if (lt_props_get_i32(p, "trigger.enabled", &v)) { s->enabled = v ? 1 : 0; }
    if (!lt_props_get_i32(p, "trigger.count", &n) || n < 0) { n = 0; }
    if (n > 500) { n = 500; }
    for (i = 0; i < n; i++) {
        lt_trig_rule *r = lt_trig_add(s);
        int a;
        lt_snprintf(k, sizeof(k), "trigger.%d.name", i); get_str(p, k, r->name, sizeof(r->name));
        lt_snprintf(k, sizeof(k), "trigger.%d.expr", i); get_str(p, k, r->expr, sizeof(r->expr));
        lt_snprintf(k, sizeof(k), "trigger.%d.enabled", i);
        if (lt_props_get_i32(p, k, &v)) { r->enabled = v ? 1 : 0; }
        lt_snprintf(k, sizeof(k), "trigger.%d.hold", i); r->hold_s = get_num(p, k, 0.0, 86400.0);
        lt_snprintf(k, sizeof(k), "trigger.%d.rearm", i); r->rearm_s = get_num(p, k, 0.0, 86400.0);
        lt_snprintf(k, sizeof(k), "trigger.%d.once", i);
        if (lt_props_get_i32(p, k, &v)) { r->once = v ? 1 : 0; }
        lt_snprintf(k, sizeof(k), "trigger.%d.action", i);
        a = lt_trig_act_find(lt_props_get(p, k));
        r->action = a >= 0 ? a : LT_TRIG_MESSAGE;
        lt_snprintf(k, sizeof(k), "trigger.%d.arg", i); get_str(p, k, r->arg, sizeof(r->arg));
    }
}

int lt_trig_save(const lt_trig_set *s, const char *path)
{
    lt_props p;
    int rc;
    lt_props_init(&p);
    lt_trig_to_props(s, &p);
    rc = lt_props_save(&p, path);
    lt_props_free(&p);
    return rc;
}

int lt_trig_load(lt_trig_set *s, const char *path)
{
    lt_props p;
    int rc = 0;
    lt_props_init(&p);
    if (path != NULL && lt_file_exists(path)) { rc = lt_props_load(&p, path); }
    if (rc == 0) {
        lt_trig_from_props(s, &p);
    } else {
        lt_trig_set_free(s);
        s->enabled = 1;
    }
    lt_props_free(&p);
    return rc;
}
