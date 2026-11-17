/* actions_logic.c - see actions_logic.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "actions_logic.h"

size_t lt_actx_record_table(lt_arec *r, const char *table, const double *z, const double *z0,
                            size_t rows, size_t cols)
{
    size_t i, n = 0, total;
    if (r == NULL || !lt_arec_is_recording(r) || table == NULL || z == NULL || z0 == NULL) { return 0; }
    if (!lt_mul_size(rows, cols, &total)) { return 0; }
    for (i = 0; i < total; i++) {
        if (fabs(z[i] - z0[i]) > 1e-9 && cols <= 65535 && rows <= 65535) {
            lt_action a;
            lt_action_init(&a, LT_ACT_TABLE_EDIT);
            lt_action_table_edit(&a, table, (unsigned)(i % cols), (unsigned)(i / cols), z[i], 1, z0[i]);
            if (lt_arec_record(r, &a) == 0) { n++; }
            lt_action_clear(&a);
        }
    }
    return n;
}

int lt_actx_record_constant(lt_arec *r, const char *name, double old_v, double new_v)
{
    lt_action a;
    int rc;
    if (r == NULL || !lt_arec_is_recording(r) || name == NULL || fabs(new_v - old_v) <= 1e-12) { return 0; }
    lt_action_init(&a, LT_ACT_CONSTANT_CHANGE);
    lt_action_constant_change(&a, name, new_v, 1, old_v);
    rc = lt_arec_record(r, &a);
    lt_action_clear(&a);
    return rc == 0 ? 1 : 0;
}

void lt_actx_describe(const lt_action *a, char *out, size_t cap)
{
    const char *tn = a->table_name != NULL ? a->table_name : "";
    switch (a->type) {
    case LT_ACT_TABLE_EDIT:
        if (a->has_old) {
            lt_snprintf(out, cap, "Table edit %s [%u, %u]: %g -> %g", tn, (unsigned)a->x_index, (unsigned)a->y_index,
                        a->old_value, a->new_value);
        } else {
            lt_snprintf(out, cap, "Table edit %s [%u, %u]: -> %g", tn, (unsigned)a->x_index, (unsigned)a->y_index,
                        a->new_value);
        }
        break;
    case LT_ACT_CONSTANT_CHANGE:
        if (a->has_old) {
            lt_snprintf(out, cap, "Constant %s: %g -> %g", a->constant_name != NULL ? a->constant_name : "",
                        a->old_value, a->new_value);
        } else {
            lt_snprintf(out, cap, "Constant %s: -> %g", a->constant_name != NULL ? a->constant_name : "", a->new_value);
        }
        break;
    case LT_ACT_BULK: {
        char extra[64];
        extra[0] = '\0';
        if (a->nparams > 0) { lt_snprintf(extra, sizeof(extra), " %s %g", a->param_keys[0], a->param_vals[0]); }
        lt_snprintf(out, cap, "%s %s (%lu cells)%s", a->operation != NULL ? a->operation : "Bulk", tn,
                    (unsigned long)a->ncells, extra);
        break; }
    case LT_ACT_LUA:
        lt_snprintf(out, cap, "Lua: %s", a->description != NULL && a->description[0] != '\0' ? a->description : "script");
        break;
    case LT_ACT_PAUSE:
        lt_snprintf(out, cap, "Pause %lu ms", a->duration_ms);
        break;
    case LT_ACT_SEND_CMD:
        lt_snprintf(out, cap, "Send command %s", a->command != NULL ? a->command : "");
        break;
    default:
        lt_strlcpy(out, "Action", cap);
        break;
    }
}

void lt_actx_new_file(const char *dir, const char *name, char *out, size_t cap)
{
    char base[96], cand[128];
    size_t k = 0, i;
    int n;
    for (i = 0; name[i] != '\0' && k + 1 < sizeof(base); i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || (unsigned char)c < 32) {
            c = '_';
        }
        base[k++] = c;
    }
    base[k] = '\0';
    while (k > 0 && (base[k - 1] == ' ' || base[k - 1] == '.')) { base[--k] = '\0'; }
    if (k == 0) { lt_strlcpy(base, "actions", sizeof(base)); }
    lt_snprintf(cand, sizeof(cand), "%s.ltaction", base);
    for (n = 2; n < 1000; n++) {
        char *full = lt_path_join(dir, cand);
        int exists = lt_file_exists(full);
        lt_free(full);
        if (!exists) { break; }
        lt_snprintf(cand, sizeof(cand), "%s %d.ltaction", base, n);
    }
    lt_strlcpy(out, cand, cap);
}

static void lower_into(const char *s, char *out, size_t cap)
{
    size_t i;
    for (i = 0; s[i] != '\0' && i + 1 < cap; i++) { out[i] = (char)tolower((unsigned char)s[i]); }
    out[i] = '\0';
}

lt_bool lt_actx_compatible(const lt_action_set *s, const char *signature)
{
    size_t i;
    char sig[200];
    if (s == NULL || s->meta.necus == 0) { return LT_TRUE; }
    if (signature == NULL || signature[0] == '\0') { return LT_FALSE; }
    lower_into(signature, sig, sizeof(sig));
    for (i = 0; i < s->meta.necus; i++) {
        char e[200];
        lower_into(s->meta.compatible_ecus[i], e, sizeof(e));
        if (e[0] != '\0' && (strstr(sig, e) != NULL || strstr(e, sig) != NULL)) { return LT_TRUE; }
    }
    return LT_FALSE;
}

size_t lt_actx_split_list(const char *text, char ***out)
{
    char **v = NULL;
    size_t n = 0;
    const char *p = text != NULL ? text : "";
    while (*p != '\0') {
        const char *e = strchr(p, ',');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        char *item = (char *)lt_malloc(len + 1);
        memcpy(item, p, len);
        item[len] = '\0';
        lt_str_trim(item);
        if (item[0] != '\0') {
            v = (char **)lt_realloc(v, (n + 1) * sizeof(char *));
            v[n++] = item;
        } else {
            lt_free(item);
        }
        p = e != NULL ? e + 1 : p + len;
    }
    *out = v;
    return n;
}

void lt_actx_join_list(char *const *v, size_t n, char *out, size_t cap)
{
    size_t i;
    if (cap == 0) { return; }
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        if (i > 0) { lt_strlcat(out, ", ", cap); }
        lt_strlcat(out, v[i], cap);
    }
}

static void free_list(char **v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { lt_free(v[i]); }
    lt_free(v);
}

void lt_actx_set_tags(lt_action_set *s, const char *text)
{
    free_list(s->meta.tags, s->meta.ntags);
    s->meta.ntags = lt_actx_split_list(text, &s->meta.tags);
}

void lt_actx_set_ecus(lt_action_set *s, const char *text)
{
    free_list(s->meta.compatible_ecus, s->meta.necus);
    s->meta.necus = lt_actx_split_list(text, &s->meta.compatible_ecus);
}

int lt_actx_remove(lt_action_set *s, size_t i)
{
    if (s == NULL || i >= s->n) { return LT_ERANGE; }
    lt_action_clear(&s->actions[i]);
    memmove(&s->actions[i], &s->actions[i + 1], (s->n - i - 1) * sizeof(lt_action));
    s->n--;
    memset(&s->actions[s->n], 0, sizeof(lt_action));
    return 0;
}

void lt_actx_append(lt_action_set *s, const lt_action *a)
{
    s->actions = (lt_action *)lt_realloc(s->actions, (s->n + 1) * sizeof(lt_action));
    lt_action_copy(&s->actions[s->n], a);
    s->n++;
}

void lt_actx_touch(lt_action_set *s)
{
    lt_tune *t = lt_tune_new("");
    lt_free(s->meta.modified_at);
    s->meta.modified_at = lt_strdup(t != NULL && t->modified != NULL ? t->modified : "");
    lt_tune_free(t);
}
