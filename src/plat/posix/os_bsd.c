/* os_bsd.c - FreeBSD/NetBSD/OpenBSD/DragonFly platform pieces (selected by
 * the build system, TARGET=bsd). Untested here: no BSD machine available.
 *
 * PLATFORM FILE: uses POSIX headers (time.h clock_gettime, unistd.h readlink).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>

lt_os lt_plat_os(void)
{
    return LT_OS_BSD;
}

double lt_plat_os_now_ms(void)
{
    struct timespec ts;
    double t = 0.0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        t = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    }
    return lt_posix_clamp_time(t);
}

static char *config_dir_os(void)
{
    return lt_posix_xdg_config_dir();
}

char *lt_plat_config_dir(void)
{
    char *o = lt_plat_config_override();
    return o != NULL ? o : config_dir_os();
}

char *lt_plat_exe_path(void)
{
    static const char *const links[] = { "/proc/curproc/file", "/proc/self/path/a.out" };
    char buf[1024];
    size_t i;
    for (i = 0; i < LT_COUNTOF(links); i++) {
        ssize_t r = readlink(links[i], buf, sizeof buf - 1);
        if (r > 0) {
            buf[r] = '\0';
            return lt_strdup(buf);
        }
    }
    return NULL;
}

int lt_plat_open_url(const char *url)
{
    return lt_posix_spawn_open("xdg-open", url);
}

int lt_serial_list(char ***names)
{
    static const char *const prefixes[] = { "cuaU", "ttyU", "cuau", "ttyu" };
    return lt_posix_serial_scan(prefixes, LT_COUNTOF(prefixes), NULL, 0, names);
}

int lt_serial_list_info(lt_serial_info **out)
{
    return lt_plat_serial_info_by_name(out);
}


/* the process is root or a member of the named group (primary or supplementary) */
static int in_group(const char *name)
{
    struct group gr, *g = NULL;
    char buf[4096];
    gid_t list[256];
    int n, i;
    if (geteuid() == 0) return 1;
    if (getgrnam_r(name, &gr, buf, sizeof buf, &g) != 0 || g == NULL) return 0;
    if (getegid() == g->gr_gid) return 1;
    n = getgroups((int)(sizeof list / sizeof list[0]), list);
    for (i = 0; i < n; i++) {
        if (list[i] == g->gr_gid) return 1;
    }
    return 0;
}

int lt_serial_access_ok(void)
{
    return in_group("dialer");
}

int lt_serial_perm_fix(char *cmd, size_t cap)
{
    /* the BSDs give the callout devices (cuaU*) to the dialer group */
    lt_snprintf(cmd, cap, "sudo pw groupmod dialer -m $USER");
    return 1;
}
