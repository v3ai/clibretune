/* tune_diff.c - tune diff/merge (spec 04 section 3) and INI migration (section 4).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tune_priv.h"

/* ---------------------------------------------------------------------- */
/* diff                                                                    */
/* ---------------------------------------------------------------------- */
static lt_tune_value *dup_value(const lt_tune_value *v)
{
    lt_tune_value *d = (lt_tune_value *)lt_malloc(sizeof *d);
    lt_tune_value_copy(d, v);
    return d;
}

lt_tune_diff *lt_tune_diff_compare(const lt_tune *a, const lt_tune *b)
{
    lt_tune_diff *d = (lt_tune_diff *)lt_calloc(1, sizeof *d);
    size_t ia = 0, ib = 0, na = lt_tune_n_constants(a), nb = lt_tune_n_constants(b);
    size_t cap = na + nb + 1;

    d->diffs = (lt_tune_difference *)lt_calloc(cap, sizeof(lt_tune_difference));
    d->signature_match = strcmp(a->signature ? a->signature : "", b->signature ? b->signature : "") == 0;
    d->sig_a = lt_strdup(a->signature ? a->signature : "");
    d->sig_b = lt_strdup(b->signature ? b->signature : "");
    d->sum_a = na;
    d->sum_b = nb;
    while (ia < na || ib < nb) {
        const char *ka = ia < na ? lt_tune_constant_name(a, ia) : NULL;
        const char *kb = ib < nb ? lt_tune_constant_name(b, ib) : NULL;
        int c;
        const lt_tune_entry *ea = NULL, *eb = NULL;
        const char *name;
        lt_tune_difference *df;
        if (ka && kb) c = strcmp(ka, kb);
        else c = ka ? -1 : 1;
        if (c < 0) {
            name = ka;
            ea = lt_tune_get_constant(a, ka);
            ia++;
        } else if (c > 0) {
            name = kb;
            eb = lt_tune_get_constant(b, kb);
            ib++;
        } else {
            name = ka;
            ea = lt_tune_get_constant(a, ka);
            eb = lt_tune_get_constant(b, kb);
            ia++;
            ib++;
            if (lt_tune_value_equal(&ea->value, &eb->value)) continue;
        }
        df = &d->diffs[d->n++];
        df->name = lt_strdup(name);
        if (ea) df->value_a = dup_value(&ea->value);
        if (eb) df->value_b = dup_value(&eb->value);
        if (ea && eb && ea->value.kind == LT_TV_SCALAR && eb->value.kind == LT_TV_SCALAR) {
            double x = ea->value.scalar, y = eb->value.scalar;
            df->has_numeric = LT_TRUE;
            df->numeric_diff = y - x;
            if (fabs(x) > 1e-9) {
                df->has_percent = LT_TRUE;
                df->percent_diff = (y - x) / x * 100.0;
            }
        }
    }
    return d;
}

void lt_tune_diff_free(lt_tune_diff *d)
{
    size_t i;
    if (!d) return;
    for (i = 0; i < d->n; i++) {
        lt_free(d->diffs[i].name);
        if (d->diffs[i].value_a) {
            lt_tune_value_clear(d->diffs[i].value_a);
            lt_free(d->diffs[i].value_a);
        }
        if (d->diffs[i].value_b) {
            lt_tune_value_clear(d->diffs[i].value_b);
            lt_free(d->diffs[i].value_b);
        }
    }
    lt_free(d->diffs);
    lt_free(d->sig_a);
    lt_free(d->sig_b);
    lt_free(d);
}

lt_bool lt_tune_diff_identical(const lt_tune_diff *d)
{
    return d->n == 0 && d->signature_match;
}

size_t lt_tune_merge_selected(lt_tune *target, const lt_tune *source, const char *const *names,
                              size_t n)
{
    size_t i, count = 0;
    for (i = 0; i < n; i++) {
        const lt_tune_entry *e = lt_tune_get_constant(source, names[i]);
        if (!e) continue;
        lt_tune_set_constant(target, names[i], &e->value, e->page);
        count++;
    }
    if (count > 0) lt_tune_touch(target);
    return count;
}

/* ---------------------------------------------------------------------- */
/* migration                                                               */
/* ---------------------------------------------------------------------- */
void lt_migration_init(lt_migration_report *r)
{
    memset(r, 0, sizeof *r);
    r->can_auto_migrate = LT_TRUE;
    r->requires_user_review = LT_FALSE;
    lt_strlcpy(r->severity, "none", sizeof r->severity);
}

static void change_clear(lt_constant_change *c)
{
    lt_free(c->name);
    lt_free(c->old_type);
    lt_free(c->new_type);
    lt_free(c->description);
}

static void td_strs_free(char **a, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) lt_free(a[i]);
    if (a) lt_free(a);
}

void lt_migration_clear(lt_migration_report *r)
{
    size_t i;
    td_strs_free(r->missing_in_tune, r->n_missing_in_tune);
    td_strs_free(r->missing_in_ini, r->n_missing_in_ini);
    for (i = 0; i < r->n_type; i++) change_clear(&r->type_changed[i]);
    for (i = 0; i < r->n_scale; i++) change_clear(&r->scale_changed[i]);
    for (i = 0; i < r->n_loc; i++) change_clear(&r->location_changed[i]);
    if (r->type_changed) lt_free(r->type_changed);
    if (r->scale_changed) lt_free(r->scale_changed);
    if (r->location_changed) lt_free(r->location_changed);
    lt_migration_init(r);
}

static void add_str(char ***a, size_t *n, const char *s)
{
    *a = (char **)lt_realloc(*a, (*n + 1) * sizeof(char *));
    (*a)[(*n)++] = lt_strdup(s);
}

static void add_change(lt_constant_change **a, size_t *n, const lt_manifest_entry *m,
                       const lt_ini_const *c, const char *desc)
{
    lt_constant_change *ch;
    *a = (lt_constant_change *)lt_realloc(*a, (*n + 1) * sizeof(lt_constant_change));
    ch = &(*a)[(*n)++];
    ch->name = lt_strdup(m->name);
    ch->old_type = lt_strdup(m->type ? m->type : "");
    ch->old_page = m->page;
    ch->old_offset = m->offset;
    ch->old_scale = m->scale;
    ch->old_translate = m->translate;
    ch->new_type = lt_strdup(lt_dtype_name(c->type));
    ch->new_page = c->page;
    ch->new_offset = c->offset;
    ch->new_scale = c->scale;
    ch->new_translate = c->translate;
    ch->description = lt_strdup(desc);
}

static int cmp_cp(const void *a, const void *b)
{
    return strcmp((*(const lt_ini_const *const *)a)->name, (*(const lt_ini_const *const *)b)->name);
}

void lt_migration_compare(lt_migration_report *out, const lt_manifest_entry *saved, size_t n,
                          const lt_ini *ini)
{
    lt_map known;
    const lt_ini_const **sorted;
    size_t i, ns = 0;

    lt_migration_clear(out);
    lt_map_init(&known, LT_FALSE);
    for (i = 0; i < n; i++) lt_map_put(&known, saved[i].name, (void *)&saved[i]);

    sorted = (const lt_ini_const **)lt_malloc((ini->n_constants + 1) * sizeof(*sorted));
    for (i = 0; i < ini->n_constants; i++) sorted[ns++] = ini->constants[i];
    qsort(sorted, ns, sizeof(*sorted), cmp_cp);
    for (i = 0; i < ns; i++) {
        if (sorted[i]->is_pc_var) continue;
        if (!lt_map_has(&known, sorted[i]->name)) add_str(&out->missing_in_tune, &out->n_missing_in_tune, sorted[i]->name);
    }
    lt_free((void *)sorted);

    for (i = 0; i < n; i++) {
        const lt_manifest_entry *m = &saved[i];
        const lt_ini_const *c = lt_ini_find_const(ini, m->name);
        char desc[256], a[64], b[64], c1[64], d[64];
        if (!c) {
            add_str(&out->missing_in_ini, &out->n_missing_in_ini, m->name);
            continue;
        }
        if (strcmp(m->type ? m->type : "", lt_dtype_name(c->type)) != 0) {
            lt_snprintf(desc, sizeof desc, "Type changed from %s to %s", m->type ? m->type : "",
                        lt_dtype_name(c->type));
            add_change(&out->type_changed, &out->n_type, m, c, desc);
        } else if (fabs(m->scale - c->scale) > 1e-9 || fabs(m->translate - c->translate) > 1e-9) {
            lt_tune_fmt_num(a, sizeof a, m->scale, 1);
            lt_tune_fmt_num(b, sizeof b, m->translate, 1);
            lt_tune_fmt_num(c1, sizeof c1, c->scale, 1);
            lt_tune_fmt_num(d, sizeof d, c->translate, 1);
            lt_snprintf(desc, sizeof desc, "Scale changed from %s*x+%s to %s*x+%s", a, b, c1, d);
            add_change(&out->scale_changed, &out->n_scale, m, c, desc);
        } else if (m->page != c->page || m->offset != c->offset) {
            lt_snprintf(desc, sizeof desc, "Moved from page %u offset %lu to page %u offset %lu",
                        (unsigned)m->page, (unsigned long)m->offset, (unsigned)c->page,
                        (unsigned long)c->offset);
            add_change(&out->location_changed, &out->n_loc, m, c, desc);
        }
    }
    lt_map_free(&known);

    if (out->n_type || out->n_missing_in_ini) {
        out->requires_user_review = LT_TRUE;
        out->can_auto_migrate = LT_FALSE;
        lt_strlcpy(out->severity, "high", sizeof out->severity);
    } else if (out->n_scale) {
        out->requires_user_review = LT_TRUE;
        out->can_auto_migrate = LT_TRUE;
        lt_strlcpy(out->severity, "medium", sizeof out->severity);
    } else if (out->n_missing_in_tune || out->n_loc) {
        out->requires_user_review = LT_FALSE;
        out->can_auto_migrate = LT_TRUE;
        lt_strlcpy(out->severity, "low", sizeof out->severity);
    }
}

/* ---- tunes without a constantManifest (TunerStudio files, base tunes): compare by name ---- */
static void shape_text(const lt_ini_const *c, char *out, size_t cap)
{
    size_t cnt = lt_shape_count(&c->shape);
    if (c->type == LT_DT_BITS || c->type == LT_DT_STRING || cnt <= 1) {
        lt_strlcpy(out, lt_dtype_name(c->type), cap);
    } else {
        lt_snprintf(out, cap, "%s[%lu]", lt_dtype_name(c->type), (unsigned long)cnt);
    }
}

static void value_text(const lt_tune_value *v, char *out, size_t cap)
{
    switch (v->kind) {
    case LT_TV_ARRAY: lt_snprintf(out, cap, "%lu values", (unsigned long)v->n); break;
    case LT_TV_STRING: lt_snprintf(out, cap, "\"%s\"", v->str != NULL ? v->str : ""); break;
    case LT_TV_BOOL: lt_strlcpy(out, v->boolean ? "true" : "false", cap); break;
    default: lt_strlcpy(out, "1 value", cap); break;
    }
}

static void add_mismatch(lt_migration_report *out, const char *name, const lt_tune_value *v,
                         const lt_ini_const *c, const char *desc)
{
    lt_constant_change *ch;
    char a[96], b[64];
    value_text(v, a, sizeof a);
    shape_text(c, b, sizeof b);
    out->type_changed = (lt_constant_change *)lt_realloc(out->type_changed,
                                                         (out->n_type + 1) * sizeof(lt_constant_change));
    ch = &out->type_changed[out->n_type++];
    memset(ch, 0, sizeof *ch);
    ch->name = lt_strdup(name);
    ch->old_type = lt_strdup(a);
    ch->new_type = lt_strdup(b);
    ch->new_page = c->page;
    ch->new_offset = c->offset;
    ch->new_scale = c->scale;
    ch->new_translate = c->translate;
    ch->description = lt_strdup(desc);
}

/* why the named value v cannot be written to c as it is (NULL = it fits) */
static const char *value_misfit(const lt_ini_const *c, const lt_tune_value *v)
{
    size_t cnt = lt_shape_count(&c->shape);
    double d;
    if (c->type == LT_DT_STRING) {
        return v->kind == LT_TV_STRING ? NULL : "the definition expects text";
    }
    if (c->type == LT_DT_BITS) {
        if (v->kind == LT_TV_STRING) {
            const char *s = v->str != NULL ? v->str : "";
            if (lt_ini_bit_index(c, s) >= 0 || lt_tune_parse_num(s, strlen(s), &d)) { return NULL; }
            return "option not offered by the definition";
        }
        return NULL;
    }
    if (v->kind == LT_TV_STRING) { return "text value for a numeric constant"; }
    if (v->kind == LT_TV_ARRAY && v->n != cnt) { return "different number of values"; }
    if (v->kind != LT_TV_ARRAY && cnt > 1) { return "single value for an array"; }
    return NULL;
}

void lt_migration_compare_tune(lt_migration_report *out, const lt_tune *t, const lt_ini *ini)
{
    const lt_ini_const **sorted;
    size_t i, ns = 0, n;

    if (t->has_manifest && t->n_manifest > 0) {
        lt_migration_compare(out, t->manifest, t->n_manifest, ini);
        return;
    }
    lt_migration_clear(out);
    sorted = (const lt_ini_const **)lt_malloc((ini->n_constants + 1) * sizeof(*sorted));
    for (i = 0; i < ini->n_constants; i++) sorted[ns++] = ini->constants[i];
    qsort(sorted, ns, sizeof(*sorted), cmp_cp);
    for (i = 0; i < ns; i++) {
        if (sorted[i]->is_pc_var) continue;
        if (!lt_tune_get_constant(t, sorted[i]->name))
            add_str(&out->missing_in_tune, &out->n_missing_in_tune, sorted[i]->name);
    }
    lt_free((void *)sorted);

    n = lt_tune_n_constants(t);
    for (i = 0; i < n; i++) {
        const char *name = lt_tune_constant_name(t, i);
        const lt_tune_entry *e = name != NULL ? lt_tune_get_constant(t, name) : NULL;
        const lt_ini_const *c;
        const char *why;
        if (e == NULL) continue;
        c = lt_ini_find_const(ini, name);
        if (c == NULL) {
            add_str(&out->missing_in_ini, &out->n_missing_in_ini, name);
            continue;
        }
        if (c->is_pc_var) continue;
        why = value_misfit(c, &e->value);
        if (why != NULL) add_mismatch(out, name, &e->value, c, why);
    }

    if (out->n_type || out->n_missing_in_ini) {
        out->requires_user_review = LT_TRUE;
        out->can_auto_migrate = LT_FALSE;
        lt_strlcpy(out->severity, "high", sizeof out->severity);
    } else if (out->n_missing_in_tune) {
        out->requires_user_review = LT_FALSE;
        out->can_auto_migrate = LT_TRUE;
        lt_strlcpy(out->severity, "low", sizeof out->severity);
    }
}

lt_bool lt_migration_has_changes(const lt_migration_report *r)
{
    return (r->n_missing_in_tune || r->n_missing_in_ini || r->n_type || r->n_scale || r->n_loc)
               ? LT_TRUE : LT_FALSE;
}

char *lt_migration_summary(const lt_migration_report *r)
{
    lt_buf b;
    int first = 1;
    lt_buf_init(&b);
#define PART(cnt, word) \
    if (cnt) { \
        if (!first) lt_buf_puts(&b, ", "); \
        lt_buf_printf(&b, "%lu " word, (unsigned long)(cnt)); \
        first = 0; \
    }
    PART(r->n_missing_in_tune, "new constants")
    PART(r->n_missing_in_ini, "removed constants")
    PART(r->n_type, "type changes")
    PART(r->n_scale, "scale changes")
    PART(r->n_loc, "location changes")
#undef PART
    if (first) lt_buf_puts(&b, "No changes detected");
    return lt_buf_take(&b);
}
