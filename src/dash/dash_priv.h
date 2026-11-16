/* dash_priv.h - internal helpers shared by the dash module .c files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_PRIV_H
#define LT_DASH_PRIV_H

#include "dash.h"

/* replace *p by a copy of s (s may be NULL -> NULL) */
void dash_set_str(char **p, const char *s);
/* shortest round-trip decimal text of v, never in exponent form (Rust f64 Display) */
void dash_fmt_f64(char *dst, size_t cap, double v);
/* current local time "%Y-%m-%d %H:%M:%S" (full) or "%Y-%m-%d" */
void dash_now_str(char *dst, size_t cap, lt_bool with_time);
void dash_image_free(lt_dash_image *im);
void dash_comp_free(lt_dash_comp *c);
/* strict decimal int parse (optional sign), whole trimmed string, range of lt_s32 */
lt_bool dash_parse_i32(const char *s, long *out);

void dash_cluster_reset_attrs(lt_dash_cluster *c);   /* keeps images + components */

#endif
