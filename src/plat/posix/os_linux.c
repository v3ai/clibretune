/* os_linux.c - Linux-specific platform pieces (selected by the build system).
 *
 * PLATFORM FILE: uses POSIX/Linux headers (time.h clock_gettime, unistd.h
 * readlink). Built only when TARGET=linux.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

lt_os lt_plat_os(void)
{
    return LT_OS_LINUX;
}

double lt_plat_now_ms(void)
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
    char buf[1024];
    ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (r <= 0) return NULL;
    buf[r] = '\0';
    return lt_strdup(buf);
}

int lt_plat_open_url(const char *url)
{
    return lt_posix_spawn_open("xdg-open", url);
}

int lt_serial_list(char ***names)
{
    static const char *const prefixes[] = { "ttyUSB", "ttyACM" };
    static const char *const fixed[] = { "ttyS0", "ttyS1", "ttyS2", "ttyS3" };
    return lt_posix_serial_scan(prefixes, LT_COUNTOF(prefixes), fixed, LT_COUNTOF(fixed), names);
}
