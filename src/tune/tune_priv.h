/* tune_priv.h - internals shared by the tune module .c files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TUNE_PRIV_H
#define LT_TUNE_PRIV_H

#include "tune.h"

/* rust_mode 0: format_f64 of spec 04 1.4 (0 for zero/non-finite);
 * rust_mode 1: plain Rust "{}" (NaN, inf, -0 spelled out). Never uses an exponent. */
void    lt_tune_fmt_num(char *dst, size_t cap, double v, int rust_mode);
/* whole token must be a Rust-parsable f64 (no hex floats) */
lt_bool lt_tune_parse_num(const char *s, size_t n, double *out);
lt_bool lt_tune_parse_u8(const char *s, int *out);        /* strict decimal 0..255 */
lt_tune_entry *lt_tune_entry_new(void);
void           lt_tune_entry_free(lt_tune_entry *e);
void           lt_tune_invalidate(lt_tune *t);
char          *lt_tune_now_rfc3339(void);

#endif
