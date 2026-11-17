/* plat_os.c - Windows OS identity, clocks, directories and shell helpers.
 *
 * PLATFORM FILE: uses Win32 headers via win_priv.h (see the exception noted
 * there). Built only for TARGET=windows. Untested on real Windows hardware;
 * exercised under wine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

#include <stdlib.h>
#include <string.h>

lt_os lt_plat_os(void)
{
    return LT_OS_WINDOWS;
}

double lt_plat_now_ms(void)
{
    static double freq = 0.0;
    LARGE_INTEGER c;
    if (freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq;
}

void lt_plat_sleep_ms(int ms)
{
    if (ms > 0) Sleep((DWORD)ms);
}

int lt_plat_setenv(const char *name, const char *value)
{
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

char *lt_plat_home_dir(void)
{
    char *h = lt_plat_getenv("USERPROFILE");
    if (h == NULL) h = lt_strdup(".");
    return h;
}

static char *config_dir_os(void)
{
    char *base = lt_plat_getenv("APPDATA");
    char *dir;
    if (base == NULL) base = lt_strdup(".");
    dir = lt_path_join(base, "LibreTune");
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
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return NULL;
    return lt_strdup(buf);
}

int lt_plat_open_url(const char *url)
{
    HINSTANCE r = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32) ? 0 : -1;
}
