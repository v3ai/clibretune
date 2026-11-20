/* tune_part.c - partial tunes (.msqpart): extract a subset of a tune, capture the current values
 * of chosen constants, find the constants that differ from a reference, collect the constants
 * behind an INI table / curve / dialog, and apply a partial tune with an undo copy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "tune_priv.h"

static char *dup_or_null(const char *s)
{
    return s != NULL ? lt_strdup(s) : NULL;
}

static void copy_attrs(lt_tune_entry *dst, const lt_tune_entry *src)
{
    if (!src->has_attrs || dst->has_attrs) return;
    dst->attrs.digits = dup_or_null(src->attrs.digits);
    dst->attrs.units = dup_or_null(src->attrs.units);
    dst->attrs.rows = dup_or_null(src->attrs.rows);
    dst->attrs.cols = dup_or_null(src->attrs.cols);
    dst->has_attrs = LT_TRUE;
}

static lt_tune *part_new(const char *signature)
{
    lt_tune *t = lt_tune_new(signature != NULL ? signature : "");
    t->author = lt_strdup("LibreTune");
    return t;
}

static void part_describe(lt_tune *t)
{
    char b[96];
    size_t n = t->constants.len + t->pc_variables.len;
    lt_snprintf(b, sizeof b, "Partial tune: %lu value%s", (unsigned long)n, n == 1 ? "" : "s");
    if (t->description) lt_free(t->description);
    t->description = lt_strdup(b);
}

lt_tune *lt_tune_part_extract(const lt_tune *src, const char *const *names, size_t n)
{
    lt_tune *t = part_new(src->signature);
    size_t i;
    for (i = 0; i < n; i++) {
        const lt_tune_entry *e = lt_tune_get_constant(src, names[i]);
        if (e != NULL) {
            lt_tune_set_constant(t, names[i], &e->value, e->page);
            copy_attrs((lt_tune_entry *)lt_map_get(&t->constants, names[i]), e);
            continue;
        }
        e = lt_tune_get_pc_variable(src, names[i]);
        if (e != NULL) lt_tune_set_pc_variable(t, names[i], &e->value);
    }
    part_describe(t);
    return t;
}

lt_tune *lt_tune_part_capture(const lt_ini *ini, const lt_pagesrc *ps, const lt_tune *pcvars,
                              const char *const *names, size_t n)
{
    lt_tune *t = part_new(ini->signature);
    size_t i;
    for (i = 0; i < n; i++) {
        const lt_ini_const *c = lt_ini_find_const(ini, names[i]);
        lt_tune_value v;
        if (c == NULL) continue;
        if (c->is_pc_var) {
            const lt_tune_entry *e = pcvars != NULL ? lt_tune_get_pc_variable(pcvars, names[i]) : NULL;
            if (e != NULL) lt_tune_set_pc_variable(t, names[i], &e->value);
            continue;
        }
        if (!lt_tune_read_const(ini, c, ps, &v)) {
            lt_tune_value_clear(&v);
            continue;
        }
        lt_tune_set_constant(t, names[i], &v, c->page);
        lt_tune_value_clear(&v);
        lt_tune_fill_attrs((lt_tune_entry *)lt_map_get(&t->constants, names[i]), c);
    }
    part_describe(t);
    return t;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t lt_tune_part_diff_names(const lt_tune *cur, const lt_tune *ref, char ***names_out)
{
    lt_tune_diff *d = lt_tune_diff_compare(ref, cur);
    char **v = (char **)lt_malloc((d->n + 1) * sizeof(char *));
    size_t i, n = 0;
    for (i = 0; i < d->n; i++) {
        if (d->diffs[i].value_b == NULL) continue;      /* only in the reference */
        v[n++] = lt_strdup(d->diffs[i].name);
    }
    lt_tune_diff_free(d);
    *names_out = v;
    return n;
}

static size_t add_const(const lt_ini *ini, const char *name, lt_map *set)
{
    if (name == NULL || name[0] == '\0' || lt_ini_find_const(ini, name) == NULL) return 0;
    if (lt_map_has(set, name)) return 0;
    lt_map_put(set, name, (void *)set);
    return 1;
}

static size_t target_consts(const lt_ini *ini, const char *target, lt_map *set, int depth)
{
    const lt_ini_table *tb;
    const lt_ini_curve *cv;
    const lt_ini_dialog *dl;
    size_t n = 0, i, k;
    if (target == NULL || target[0] == '\0' || depth > 8) return 0;
    tb = lt_ini_find_table(ini, target);
    if (tb != NULL) {
        n += add_const(ini, tb->z_const, set);
        n += add_const(ini, tb->x_const, set);
        n += add_const(ini, tb->y_const, set);
        n += add_const(ini, tb->rows_size_const, set);
        n += add_const(ini, tb->cols_size_const, set);
        return n;
    }
    cv = lt_ini_find_curve(ini, target);
    if (cv != NULL) {
        n += add_const(ini, cv->x_const, set);
        n += add_const(ini, cv->y_const, set);
        n += add_const(ini, cv->y_const2, set);
        return n;
    }
    dl = lt_ini_find_dialog(ini, target);
    if (dl == NULL) return 0;
    for (i = 0; i < dl->n_comps; i++) {
        const lt_ini_comp *cp = &dl->comps[i];
        switch (cp->kind) {
        case LT_COMP_FIELD: case LT_COMP_CHECKBOX: case LT_COMP_SLIDER:
            n += add_const(ini, cp->name, set);
            break;
        case LT_COMP_SETTINGSEL:
            for (k = 0; k < cp->n_options; k++) {
                size_t a;
                for (a = 0; a < cp->options[k].n_assigns; a++) n += add_const(ini, cp->options[k].assigns[a].name, set);
            }
            break;
        case LT_COMP_PANEL: case LT_COMP_TABLE:
            n += target_consts(ini, cp->name, set, depth + 1);
            break;
        default:
            break;
        }
    }
    return n;
}

size_t lt_tune_part_target_consts(const lt_ini *ini, const char *target, lt_map *set)
{
    if (ini == NULL || set == NULL) return 0;
    return target_consts(ini, target, set, 0);
}

static void push_keys(const lt_map *m, char **v, size_t *n)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        if (m->slots[i].used == 1) v[(*n)++] = lt_strdup(m->slots[i].key);
}

size_t lt_tune_part_names(const lt_tune *part, char ***names_out)
{
    char **v = (char **)lt_malloc((part->constants.len + part->pc_variables.len + 1) * sizeof(char *));
    size_t n = 0;
    push_keys(&part->constants, v, &n);
    push_keys(&part->pc_variables, v, &n);
    qsort(v, n, sizeof(char *), cmp_name);
    *names_out = v;
    return n;
}

static void apply_pc(lt_tune *pcvars, lt_tune *un, const char *name, const lt_tune_value *v, size_t *ok)
{
    const lt_tune_entry *old = lt_tune_get_pc_variable(pcvars, name);
    if (un != NULL && old != NULL) lt_tune_set_pc_variable(un, name, &old->value);
    lt_tune_set_pc_variable(pcvars, name, v);
    (*ok)++;
}

int lt_tune_part_apply(const lt_ini *ini, lt_pagesrc *ps, lt_tune *pcvars, const lt_tune *part,
                       lt_tune **undo, size_t *applied, size_t *skipped)
{
    lt_tune *un = NULL;
    char **names = NULL;
    size_t n, i, ok = 0, bad = 0;
    if (applied) *applied = 0;
    if (skipped) *skipped = 0;
    if (undo) *undo = NULL;
    if (ini == NULL || ps == NULL || part == NULL) {
        lt_err_set("Nothing to apply");
        return LT_EINVAL;
    }
    if (undo) un = part_new(part->signature);
    n = lt_tune_part_names(part, &names);
    for (i = 0; i < n; i++) {
        const lt_ini_const *c = lt_ini_find_const(ini, names[i]);
        const lt_tune_entry *e = lt_tune_get_constant(part, names[i]);
        lt_tune_value old;
        if (e == NULL) e = lt_tune_get_pc_variable(part, names[i]);
        if (c == NULL || e == NULL) { bad++; continue; }
        if (c->is_pc_var) {
            if (pcvars == NULL) { bad++; continue; }
            apply_pc(pcvars, un, names[i], &e->value, &ok);
            continue;
        }
        if (!lt_tune_read_const(ini, c, ps, &old)) {
            lt_tune_value_clear(&old);
            bad++;
            continue;
        }
        if (lt_tune_write_const(ini, c, ps, &e->value) != 0) {
            lt_tune_value_clear(&old);
            bad++;
            continue;
        }
        if (un != NULL) {
            lt_tune_set_constant(un, names[i], &old, c->page);
            lt_tune_fill_attrs((lt_tune_entry *)lt_map_get(&un->constants, names[i]), c);
        }
        lt_tune_value_clear(&old);
        ok++;
    }
    for (i = 0; i < n; i++) lt_free(names[i]);
    lt_free(names);
    if (un != NULL) {
        part_describe(un);
        *undo = un;
    }
    if (applied) *applied = ok;
    if (skipped) *skipped = bad;
    return 0;
}
