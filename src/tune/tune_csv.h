/* tune_csv.h - tune export / import as CSV, in the layout of the original:
 *   Name,Page,Offset,Shape,Value,Units,Min,Max,Scale,Translate,DataType,IsPcVariable
 * Values are display values (scale and translate applied); arrays are one quoted "[a,b,c]" field,
 * strings one quoted field. The importer also reads the older 11 column layout without Shape.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TUNE_CSV_H
#define LT_TUNE_CSV_H

#include "tune.h"

LT_BEGIN_DECLS

/* CSV text (malloc'd, no trailing newline) of every constant of the INI read through `src`;
 * *count (may be NULL) receives the number of rows. */
char *lt_tune_csv_export(const lt_ini *ini, const lt_pagesrc *src, size_t *count);
/* write the text to a file; returns 0 or a negative error (lt_err set); *count as above */
int   lt_tune_csv_export_file(const lt_ini *ini, const lt_pagesrc *src, const char *path,
                              size_t *count);

typedef struct lt_csv_import {
    size_t imported;            /* constants written */
    size_t clamped;             /* scalar values outside the INI range that were clamped */
    size_t unknown;             /* rows naming constants this INI does not have (skipped) */
    char **messages;            /* warnings/errors ("Line 7: invalid value 'x'"), first 50 */
    size_t n_messages;
} lt_csv_import;
void lt_csv_import_free(lt_csv_import *r);

/* Apply CSV text to the pages behind `dst`. Unknown names are skipped silently (they may come
 * from another INI); bad values are reported in r->messages and skipped. Returns 0, or a negative
 * error when the arguments are unusable. `r` may be NULL. */
int lt_tune_csv_import(const lt_ini *ini, lt_pagesrc *dst, const char *text, size_t len,
                       lt_csv_import *r);
int lt_tune_csv_import_file(const lt_ini *ini, lt_pagesrc *dst, const char *path,
                            lt_csv_import *r);

/* Split one CSV line (quotes may hold commas, "" is a quote). Returns the field count (<= cap);
 * each field is malloc'd, free them with lt_free. */
size_t lt_csv_split(const char *line, size_t len, char **fields, size_t cap);

LT_END_DECLS

#endif
