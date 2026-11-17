/* plat_priv.h - helpers shared between plat_common.c and the per-platform
 * implementations. Not part of the public plat.h interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PLAT_PRIV_H
#define LT_PLAT_PRIV_H

#include "plat.h"

LT_BEGIN_DECLS

/* sort a name array (as returned by lt_plat_list_dir) ascending by byte value */
void lt_plat_sort_names(char **names, int n);
/* append a copy of name to a growing array; returns the new count */
int lt_plat_names_add(char ***arr, int n, int *cap, const char *name);

LT_END_DECLS

#endif /* LT_PLAT_PRIV_H */
