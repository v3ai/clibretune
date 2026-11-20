/* plat_common.c - universal (portable C89) parts of plat.h, shared by every
 * platform. Everything here uses only the C standard library plus the
 * platform primitives declared in plat.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat_priv.h"

/* ---- clock (with the screenshot harness's virtual mode) ---- */
static int g_vclock = 0;
static double g_vclock_ms = 0.0;

double lt_plat_now_ms(void)
{
    if (g_vclock) {
        g_vclock_ms += 0.01;
        return g_vclock_ms;
    }
    return lt_plat_os_now_ms();
}

void lt_plat_sleep_ms(int ms)
{
    if (g_vclock) {
        if (ms > 0) { g_vclock_ms += (double)ms; }
        return;
    }
    lt_plat_os_sleep_ms(ms);
}

void lt_plat_clock_virtual(int on)
{
    if (on && !g_vclock) { g_vclock_ms = lt_plat_os_now_ms(); }
    g_vclock = on ? 1 : 0;
}

lt_bool lt_plat_clock_is_virtual(void)
{
    return g_vclock ? LT_TRUE : LT_FALSE;
}

const char *lt_plat_os_name(void)
{
    switch (lt_plat_os()) {
    case LT_OS_LINUX: return "Linux";
    case LT_OS_MACOS: return "macOS";
    case LT_OS_BSD: return "BSD";
    case LT_OS_WINDOWS: return "Windows";
    default: break;
    }
    return "Unix";
}

void lt_plat_timestamp(char *buf, size_t cap)
{
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    if (cap == 0) return;
    if (tmv == NULL || strftime(buf, cap, "%Y-%m-%d %H:%M:%S", tmv) == 0) {
        lt_strlcpy(buf, "0000-00-00 00:00:00", cap);
    }
}

char *lt_plat_getenv(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') return NULL;
    return lt_strdup(v);
}

int lt_plat_mkdirs(const char *path)
{
    char *p;
    size_t i, n;
    int rc;
    if (path == NULL || path[0] == '\0') return -1;
    p = lt_strdup(path);
    n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) {
        n--;
        p[n] = '\0';
    }
    for (i = 1; i < n; i++) {
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = '\0';
            if (!lt_plat_is_dir(p)) lt_plat_mkdir(p); /* errors surface at the end */
            p[i] = c;
        }
    }
    rc = lt_plat_mkdir(p);
    lt_free(p);
    return rc;
}

int lt_plat_copy_file(const char *from, const char *to)
{
    FILE *in, *out;
    char buf[4096];
    size_t n;
    int rc = 0;
    in = fopen(from, "rb");
    if (in == NULL) {
        lt_err_set("cannot open %s", from);
        return -1;
    }
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        lt_err_set("cannot create %s", to);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    if (fclose(out) != 0) rc = -1;
    fclose(in);
    if (rc != 0) lt_err_set("copy %s -> %s failed", from, to);
    return rc;
}

static int cmp_names(const void *a, const void *b)
{
    int r = strcmp(*(char *const *)a, *(char *const *)b);
    return (r > 0) - (r < 0);
}

void lt_plat_sort_names(char **names, int n)
{
    if (n > 1) qsort(names, (size_t)n, sizeof(char *), cmp_names);
}

int lt_plat_names_add(char ***arr, int n, int *cap, const char *name)
{
    if (n == *cap) {
        size_t bytes;
        int nc = *cap > 0 ? *cap : 8;
        if (nc > 1000000 || !lt_mul_size((size_t)nc * 2U, sizeof(char *), &bytes)) {
            return n;
        }
        nc *= 2;
        *arr = (char **)lt_realloc(*arr, bytes);
        *cap = nc;
    }
    (*arr)[n] = lt_strdup(name);
    return n + 1;
}

void lt_plat_free_list(char **names, int n)
{
    int i;
    for (i = 0; i < n; i++) lt_free(names[i]);
    lt_free(names);
}

char *lt_plat_projects_dir(void)
{
    char *h = lt_plat_home_dir();
    char *dir;
    if (lt_plat_os() == LT_OS_WINDOWS) {
        char *docs = lt_path_join(h, "Documents");
        dir = lt_path_join(docs, "LibreTune Projects");
        lt_free(docs);
    } else {
        dir = lt_path_join(h, "LibreTune Projects");
    }
    lt_free(h);
    return dir;
}

int lt_stream_read_full(lt_stream *s, void *buf, int n, int timeout_ms)
{
    int total = 0;
    double end = lt_plat_now_ms() + (double)timeout_ms;
    while (total < n) {
        double left = end - lt_plat_now_ms();
        int r;
        if (left < 0.0) left = 0.0;
        r = lt_stream_read(s, (char *)buf + total, n - total, (int)left);
        if (r < 0) return -1;
        total += r;
        if (r == 0 && end - lt_plat_now_ms() <= 0.0) break;
    }
    return total;
}

char *lt_plat_config_override(void)
{
    char *d = lt_plat_getenv("LIBRETUNE_CONFIG_DIR");
    if (d != NULL && d[0] == '\0') {
        lt_free(d);
        d = NULL;
    }
    if (d != NULL) lt_plat_mkdirs(d);
    return d;
}
