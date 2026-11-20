/* posix_priv.h - internal helpers shared by the POSIX platform files.
 * Feature-test macros (_POSIX_C_SOURCE etc.) come from the build system
 * (tools/target.sh), so this header must be the first include of every file.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_POSIX_PRIV_H
#define LT_POSIX_PRIV_H

#include "../plat_priv.h"

LT_BEGIN_DECLS

/* wait for fd readable (rw == 0) or writable (rw == 1);
 * returns >0 ready, 0 timeout/interrupted, -1 error */
int lt_posix_wait_fd(int fd, int rw, int timeout_ms);

/* never lets a monotonic clock reading go backwards */
double lt_posix_clamp_time(double t);

/* user's home directory; malloc'd, never NULL */
char *lt_posix_home_dir(void);

/* $XDG_CONFIG_HOME/libretune or ~/.config/libretune (created); malloc'd */
char *lt_posix_xdg_config_dir(void);

/* run "cmd url" detached with stdio redirected to /dev/null; 0 ok */
int lt_posix_spawn_open(const char *cmd, const char *url);

/* list /dev entries whose name starts with one of the prefixes or equals
 * one of the fixed names; returns count, *names malloc'd */
int lt_posix_serial_scan(const char *const *prefixes, size_t nprefix,
                         const char *const *fixed, size_t nfixed, char ***names);

/* the subdirectories of each parent directory (mounted volumes); returns count, *out malloc'd */
int lt_posix_volume_scan(const char *const *parents, size_t nparent, char ***out);

LT_END_DECLS

#endif /* LT_POSIX_PRIV_H */
