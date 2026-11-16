/* pin.c - pin conflict detection. Spec: docs/spec/09-project.md section 7.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "proj_priv.h"

static char *lower_dup(const char *s)
{
    char *r = lt_strdup(s), *p;
    for (p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

lt_bool lt_pin_is_unassigned_label(const char *label)
{
    static const char *const names[] = {"none", "invalid", "off", "disabled", "not used",
                                        "unused", "n/a", "na", "default", "board default"};
    char *t = lt_strdup(label ? label : ""), *l;
    size_t i;
    lt_bool r = LT_FALSE;
    lt_str_trim(t);
    if (t[0] == '\0') {
        lt_free(t);
        return LT_TRUE;
    }
    l = lower_dup(t);
    for (i = 0; i < LT_COUNTOF(names); i++)
        if (strcmp(l, names[i]) == 0) r = LT_TRUE;
    lt_free(l);
    lt_free(t);
    return r;
}

static lt_bool looks_like_pin_label(const char *label)
{
    static const char *const words[] = {"output", "injector", "ignition", "digital input",
                                        "low side", "high side"};
    char *t = lt_strdup(label), *l;
    size_t n, i;
    lt_bool r = LT_FALSE;
    lt_str_trim(t);
    n = strlen(t);
    if (n < 2) {
        lt_free(t);
        return LT_FALSE;
    }
    if (isalpha((unsigned char)t[0]) && isalpha((unsigned char)t[1]) && n > 2) {
        r = LT_TRUE;
        for (i = 2; i < n; i++)
            if (!isdigit((unsigned char)t[i])) r = LT_FALSE;
    }
    if (!r) {
        l = lower_dup(t);
        for (i = 0; i < LT_COUNTOF(words); i++)
            if (strstr(l, words[i]) != NULL) r = LT_TRUE;
        lt_free(l);
    }
    lt_free(t);
    return r;
}

lt_bool lt_pin_is_assignment(const lt_ini_const *c)
{
    char *l;
    size_t i, n;
    lt_bool name_pin, has_none = LT_FALSE, pinish = LT_FALSE, r;
    if (c->type != LT_DT_BITS || c->n_bit_options < 3) return LT_FALSE;
    l = lower_dup(c->name);
    n = strlen(l);
    if (strstr(l, "pinmode") != NULL || (n >= 4 && strcmp(l + n - 4, "mode") == 0) ||
        strstr(l, "invert") != NULL) {
        lt_free(l);
        return LT_FALSE;
    }
    name_pin = strstr(l, "pin") != NULL;
    lt_free(l);
    for (i = 0; i < c->n_bit_options; i++) {
        const char *o = c->bit_options[i];
        if (o == NULL) continue;
        if (lt_pin_is_unassigned_label(o)) has_none = LT_TRUE;
        if (looks_like_pin_label(o)) pinish = LT_TRUE;
    }
    r = (name_pin && has_none) || (has_none && pinish && c->n_bit_options >= 8);
    return r;
}

static const char *label_of(const lt_ini_const *c, size_t idx)
{
    if (idx >= c->n_bit_options) return NULL;
    return c->bit_options[idx];
}

typedef struct pin_item {
    const char *label;
    const char *name;
} pin_item;

static int cmp_item(const void *a, const void *b)
{
    const pin_item *x = (const pin_item *)a, *y = (const pin_item *)b;
    int c = strcmp(x->label, y->label);
    if (c != 0) return c;
    return strcmp(x->name, y->name);
}

int lt_pin_detect(const lt_ini *ini, lt_pin_value_fn fn, void *ud, lt_pin_report *out)
{
    pin_item *items = NULL;
    size_t ni = 0, i, j;

    out->c = NULL;
    out->n = 0;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        size_t idx = 0;
        const char *lab;
        if (!lt_pin_is_assignment(c)) continue;
        if (!fn(ud, c, &idx)) continue;
        lab = label_of(c, idx);
        if (lab == NULL || lt_pin_is_unassigned_label(lab)) continue;
        items = (pin_item *)lt_realloc(items, (ni + 1) * sizeof *items);
        items[ni].label = lab;
        items[ni].name = c->name;
        ni++;
    }
    if (ni > 1) qsort(items, ni, sizeof *items, cmp_item);
    for (i = 0; i < ni;) {
        for (j = i; j < ni && strcmp(items[j].label, items[i].label) == 0; j++) {
        }
        if (j - i > 1) {
            lt_pin_conflict *pc;
            size_t k;
            out->c = (lt_pin_conflict *)lt_realloc(out->c, (out->n + 1) * sizeof *out->c);
            pc = &out->c[out->n++];
            pc->pin_label = lt_strdup(items[i].label);
            pc->n = j - i;
            pc->constants = (char **)lt_malloc(pc->n * sizeof(char *));
            for (k = 0; k < pc->n; k++) pc->constants[k] = lt_strdup(items[i + k].name);
        }
        i = j;
    }
    lt_free(items);
    return LT_OK;
}

int lt_pin_conflict_if_assigning(const lt_ini *ini, const char *name, size_t new_index,
                                 lt_pin_value_fn fn, void *ud, lt_pin_conflict *out)
{
    const lt_ini_const *c = lt_ini_find_const(ini, name);
    const char *lab;
    char **others = NULL;
    size_t no = 0, i;

    memset(out, 0, sizeof *out);
    if (c == NULL || !lt_pin_is_assignment(c)) return 0;
    lab = label_of(c, new_index);
    if (lab == NULL || lt_pin_is_unassigned_label(lab)) return 0;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *o = ini->constants[i];
        size_t idx = 0;
        const char *ol;
        if (o == c || !lt_pin_is_assignment(o)) continue;
        if (!fn(ud, o, &idx)) continue;
        ol = label_of(o, idx);
        if (ol != NULL && strcmp(ol, lab) == 0) {
            others = (char **)lt_realloc(others, (no + 1) * sizeof(char *));
            others[no++] = lt_strdup(o->name);
        }
    }
    if (no == 0) return 0;
    if (no > 1) qsort(others, no, sizeof(char *), lt_proj_cmp_str);
    out->pin_label = lt_strdup(lab);
    out->n = no + 1;
    out->constants = (char **)lt_malloc(out->n * sizeof(char *));
    out->constants[0] = lt_strdup(name);
    for (i = 0; i < no; i++) out->constants[i + 1] = others[i];
    lt_free(others);
    return 1;
}

void lt_pin_conflict_free(lt_pin_conflict *c)
{
    lt_free(c->pin_label);
    lt_proj_strv_free(c->constants, c->n);
    memset(c, 0, sizeof *c);
}

void lt_pin_report_free(lt_pin_report *r)
{
    size_t i;
    for (i = 0; i < r->n; i++) lt_pin_conflict_free(&r->c[i]);
    lt_free(r->c);
    r->c = NULL;
    r->n = 0;
}

char *lt_pin_summary(const lt_pin_report *r)
{
    lt_buf b;
    size_t i, k;
    if (r->n == 0) return lt_strdup("");
    lt_buf_init(&b);
    lt_buf_puts(&b, "Pin assignment conflict(s) detected "
                    "\xE2\x80\x94"
                    " the same pin is used by multiple outputs:\n");
    for (i = 0; i < r->n; i++) {
        lt_buf_puts(&b, "\xE2\x80\xA2"
                        " Pin '");
        lt_buf_puts(&b, r->c[i].pin_label);
        lt_buf_puts(&b, "' used by: ");
        for (k = 0; k < r->c[i].n; k++) {
            if (k) lt_buf_puts(&b, ", ");
            lt_buf_puts(&b, r->c[i].constants[k]);
        }
        lt_buf_putc(&b, '\n');
    }
    lt_buf_puts(&b, "Clear or reassign one of the functions before burning (or confirm to burn "
                    "anyway).");
    return lt_buf_take(&b);
}

lt_bool lt_pin_value_from_pages(void *ud, const lt_ini_const *c, size_t *index)
{
    const struct lt_pin_pages *pp = (const struct lt_pin_pages *)ud;
    double raw = 0;
    if (pp == NULL || pp->ini == NULL || pp->src == NULL) return LT_FALSE;
    if (lt_ini_const_get_raw(pp->ini, c, pp->src, 0, &raw) != 0) return LT_FALSE;
    if (raw < 0) return LT_FALSE;
    *index = (size_t)raw;
    return LT_TRUE;
}
