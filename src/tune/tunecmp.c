/* tunecmp.c - tune compare presentation logic.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "tunecmp.h"

void lt_tcmp_format(const lt_tune_value *v, char *out, size_t cap)
{
    if (cap == 0) { return; }
    if (v == NULL) { lt_strlcpy(out, "(missing)", cap); return; }
    switch (v->kind) {
    case LT_TV_SCALAR: lt_snprintf(out, cap, "%.2f", v->scalar); break;
    case LT_TV_BOOL: lt_strlcpy(out, v->boolean ? "true" : "false", cap); break;
    case LT_TV_STRING: lt_snprintf(out, cap, "\"%s\"", v->str != NULL ? v->str : ""); break;
    case LT_TV_ARRAY:
        if (v->n <= 8) {
            size_t i, len = 0;
            lt_strlcpy(out, "[", cap);
            for (i = 0; i < v->n; i++) {
                char one[48];
                len = strlen(out);
                lt_snprintf(one, sizeof(one), i > 0 ? ", %.1f" : "%.1f", v->arr[i]);
                if (len + strlen(one) + 2 < cap) { lt_strlcpy(out + len, one, cap - len); }
            }
            len = strlen(out);
            if (len + 2 <= cap) { lt_strlcpy(out + len, "]", cap - len); }
        } else {
            lt_snprintf(out, cap, "[%lu values]", (unsigned long)v->n);
        }
        break;
    default: lt_strlcpy(out, "?", cap); break;
    }
}

int lt_tcmp_severity(const lt_tune_difference *d)
{
    double pct;
    if (!d->has_percent || d->percent_diff == 0.0) { return 0; }
    pct = fabs(d->percent_diff);
    if (pct >= 20.0) { return 3; }
    if (pct >= 5.0) { return 2; }
    return 1;
}

static int has_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), hl = strlen(hay), i, k;
    if (nl == 0) { return 1; }
    for (i = 0; i + nl <= hl; i++) {
        for (k = 0; k < nl; k++) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
            if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
            if (a != b) { break; }
        }
        if (k == nl) { return 1; }
    }
    return 0;
}

static double sort_key(const lt_tune_difference *d, int sort)
{
    if (sort == LT_TCMP_SORT_DIFF) { return d->has_numeric ? fabs(d->numeric_diff) : 0.0; }
    return d->has_percent ? fabs(d->percent_diff) : 0.0;
}

/* a before b (strictly) for the requested order */
static int less(const lt_tune_diff *d, int sort, int asc, size_t a, size_t b)
{
    int c;
    if (sort == LT_TCMP_SORT_NAME) {
        c = lt_stricmp(d->diffs[a].name, d->diffs[b].name);
    } else {
        double x = sort_key(&d->diffs[a], sort), y = sort_key(&d->diffs[b], sort);
        c = x < y ? -1 : (x > y ? 1 : 0);
    }
    return asc ? c < 0 : c > 0;
}

size_t lt_tcmp_select(const lt_tune_diff *d, int filter, const char *search, int sort, int ascending,
                      size_t *idx)
{
    size_t i, n = 0, j;
    for (i = 0; i < d->n; i++) {
        const lt_tune_difference *df = &d->diffs[i];
        if (filter == LT_TCMP_SCALAR && !df->has_numeric) { continue; }
        if (filter == LT_TCMP_ADDED && df->value_a != NULL) { continue; }
        if (filter == LT_TCMP_REMOVED && df->value_b != NULL) { continue; }
        if (search != NULL && !has_ci(df->name, search)) { continue; }
        /* stable insertion sort (the list is capped by what a tune holds) */
        for (j = n; j > 0 && less(d, sort, ascending, i, idx[j - 1]); j--) { idx[j] = idx[j - 1]; }
        idx[j] = i;
        n++;
    }
    return n;
}
