/* os_macos.c - macOS platform pieces (selected by the build system,
 * TARGET=macos). Untested here: no Mac available.
 *
 * PLATFORM FILE: uses Mach/dyld headers (mach/mach_time.h, mach-o/dyld.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdlib.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>

lt_os lt_plat_os(void)
{
    return LT_OS_MACOS;
}

double lt_plat_os_now_ms(void)
{
    static mach_timebase_info_data_t tb;
    double t;
    if (tb.denom == 0) mach_timebase_info(&tb);
    t = (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom / 1000000.0;
    return lt_posix_clamp_time(t);
}

static char *config_dir_os(void)
{
    char *h = lt_posix_home_dir();
    char *base = lt_path_join(h, "Library/Application Support");
    char *dir = lt_path_join(base, "LibreTune");
    lt_free(h);
    lt_free(base);
    lt_plat_mkdirs(dir);
    return dir;
}

char *lt_plat_config_dir(void)
{
    char *o = lt_plat_config_override();
    return o != NULL ? o : config_dir_os();
}

char *lt_plat_exe_path(void)
{
    char buf[1024];
    lt_u32 sz = (lt_u32)sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) return lt_strdup(buf);
    return NULL;
}

int lt_plat_open_url(const char *url)
{
    return lt_posix_spawn_open("open", url);
}

int lt_serial_list(char ***names)
{
    static const char *const prefixes[] = { "cu.usb", "tty.usb" };
    return lt_posix_serial_scan(prefixes, LT_COUNTOF(prefixes), NULL, 0, names);
}

int lt_serial_list_info(lt_serial_info **out)
{
    return lt_plat_serial_info_by_name(out);
}

int lt_serial_access_ok(void)
{
    return 1;
}

int lt_serial_perm_fix(char *cmd, size_t cap)
{
    if (cap > 0) cmd[0] = '\0';
    return 0;
}
