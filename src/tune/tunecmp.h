/* tunecmp.h - presentation logic of the tune compare dialog (no UI): value text, severity,
 * filter / search / sort of an lt_tune_diff. Mirrors the original's TuneFileDiffDialog.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TUNECMP_H
#define LT_TUNECMP_H

#include "tune.h"

LT_BEGIN_DECLS

#define LT_TCMP_ALL     0
#define LT_TCMP_SCALAR  1   /* both sides numeric (has a numeric difference) */
#define LT_TCMP_ADDED   2   /* absent in A */
#define LT_TCMP_REMOVED 3   /* absent in B */

#define LT_TCMP_SORT_NAME 0
#define LT_TCMP_SORT_DIFF 1   /* |numeric difference| */
#define LT_TCMP_SORT_PCT  2   /* |percent difference| */

/* "(missing)", "14.70", "true", "\"text\"", "[1.0, 2.0]" or "[16 values]" */
void lt_tcmp_format(const lt_tune_value *v, char *out, size_t cap);
/* 0 added/removed/non numeric, 1 low (<5%), 2 medium (>=5%), 3 high (>=20%) */
int  lt_tcmp_severity(const lt_tune_difference *d);
/* Indexes into d->diffs that pass the filter and the case-insensitive name search, sorted.
 * idx must have room for d->n entries. Returns the count. */
size_t lt_tcmp_select(const lt_tune_diff *d, int filter, const char *search, int sort, int ascending,
                      size_t *idx);

LT_END_DECLS

#endif
