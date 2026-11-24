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
/* the OS clock and sleep; lt_plat_now_ms / lt_plat_sleep_ms (plat_common.c) wrap them
 * with the virtual clock used by the screenshot harness */
/* lt_plat_fault_write_limit: how many of `want` bytes the next write may store (0 = "disk
 * full"); consumes them from the budget. Returns want when no limit is set. */
size_t lt_plat_fault_write_allow(size_t want);
double lt_plat_os_now_ms(void);
/* lt_serial_list_info from the port names alone (OSes without a cheap USB lookup) */
int lt_plat_serial_info_by_name(lt_serial_info **out);
void   lt_plat_os_sleep_ms(int ms);
/* The text half of lt_plat_abspath: `path` made absolute against `cwd` when it is relative, "."
 * and ".." folded, empty components dropped, separators normalised, no trailing separator except
 * at the root. win selects the Windows rules ('\\', "C:\\" drive roots, "C:name" relative to a
 * drive, "\\name" relative to the current drive's root, "\\\\server\\share" UNC roots). A relative
 * result (empty cwd) keeps leading ".." and is "." when nothing is left. malloc'd. */
char  *lt_plat_path_fold(const char *cwd, const char *path, lt_bool win);

LT_END_DECLS

#endif /* LT_PLAT_PRIV_H */
