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

static long fault_write_budget = -1L;

void lt_plat_fault_write_limit(long bytes)
{
    fault_write_budget = bytes < 0 ? -1L : bytes;
}

size_t lt_plat_fault_write_allow(size_t want)
{
    if (fault_write_budget < 0) return want;
    if ((unsigned long)fault_write_budget < (unsigned long)want) want = (size_t)fault_write_budget;
    fault_write_budget -= (long)want;
    return want;
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

char *lt_plat_app_env(const char *suffix)
{
    char name[64];
    char *v;
    lt_snprintf(name, sizeof(name), "CLIBRETUNE_%s", suffix);
    v = lt_plat_getenv(name);
    if (v == NULL) {
        lt_snprintf(name, sizeof(name), "LIBRETUNE_%s", suffix);
        v = lt_plat_getenv(name);
    }
    return v;
}

char *lt_plat_app_subdir(const char *base, const char *name, const char *old_name)
{
    char *dir = lt_path_join(base, name);
    if (!lt_plat_is_dir(dir)) {
        char *old = lt_path_join(base, old_name);
        if (lt_plat_is_dir(old)) {
            lt_free(dir);
            return old;
        }
        lt_free(old);
    }
    return dir;
}

char *lt_plat_projects_dir(void)
{
    char *h = lt_plat_home_dir();
    char *dir;
    if (lt_plat_os() == LT_OS_WINDOWS) {
        char *docs = lt_path_join(h, "Documents");
        dir = lt_plat_app_subdir(docs, "clibretune Projects", "LibreTune Projects");
        lt_free(docs);
    } else {
        dir = lt_plat_app_subdir(h, "clibretune Projects", "LibreTune Projects");
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
    char *d = lt_plat_app_env("CONFIG_DIR");
    if (d != NULL && d[0] == '\0') {
        lt_free(d);
        d = NULL;
    }
    if (d != NULL) lt_plat_mkdirs(d);
    return d;
}

/* lt_serial_list_info where the OS has no cheap USB identity (macOS, BSD): the names of
 * lt_serial_list, marked USB when the name says so (cu.usbmodem*, cu.usbserial*, cuaU*). */
int lt_plat_serial_info_by_name(lt_serial_info **out)
{
    char **names = NULL;
    lt_serial_info *a;
    int n = lt_serial_list(&names), i;
    *out = NULL;
    if (n <= 0) return n < 0 ? n : 0;
    a = (lt_serial_info *)lt_calloc((size_t)n, sizeof(lt_serial_info));
    for (i = 0; i < n; i++) {
        const char *base = lt_path_base(names[i]);
        lt_strlcpy(a[i].name, names[i], sizeof a[i].name);
        a[i].usb = (strstr(base, "usb") != NULL || lt_str_starts(base, "cuaU") ||
                    lt_str_starts(base, "ttyU")) ? 1 : 0;
        if (strstr(base, "usbmodem") != NULL) lt_strlcpy(a[i].driver, "cdc", sizeof a[i].driver);
    }
    lt_plat_free_list(names, n);
    *out = a;
    return n;
}

/* ---- path folding (the universal half of lt_plat_abspath) ---- */
static lt_bool drive_at(const char *s)
{
    return ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':';
}

/* length of the root of a path with normalised separators: "/" (1), "C:\\" (3),
 * "\\\\server\\share" (without the separator after it); 0 = relative */
static size_t root_len(const char *s, lt_bool win)
{
    size_t i;
    if (!win) return s[0] == '/' ? 1u : 0u;
    if (drive_at(s) && s[2] == '\\') return 3;
    if (s[0] == '\\' && s[1] == '\\') {
        i = 2;
        while (s[i] != '\0' && s[i] != '\\') i++;          /* server */
        if (s[i] == '\\') i++;
        while (s[i] != '\0' && s[i] != '\\') i++;          /* share */
        return i;
    }
    return 0;
}

static char *norm_seps(const char *s, lt_bool win)
{
    char *r = lt_strdup(s), *q;
    if (win) {
        for (q = r; *q != '\0'; q++) {
            if (*q == '/') *q = '\\';
        }
    }
    return r;
}

static int up_char(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

char *lt_plat_path_fold(const char *cwd, const char *path, lt_bool win)
{
    char sep = win ? '\\' : '/';
    char *p = norm_seps(path != NULL ? path : "", win), *c = norm_seps(cwd != NULL ? cwd : "", win);
    char *full, *out;
    size_t rl = 0, n = 0, k = 0, i = 0, start = 0, len = 0, base = 0, rootk = 0, cap = 0;
    lt_buf b;

    lt_buf_init(&b);
    if (root_len(p, win) > 0) {
        lt_buf_puts(&b, p);
    } else if (win && p[0] == '\\') {                       /* root of the current drive */
        lt_buf_append(&b, c, root_len(c, win));
        lt_buf_putc(&b, sep);
        lt_buf_puts(&b, p + 1);
    } else if (win && drive_at(p)) {                        /* "X:name": relative to drive X */
        if (drive_at(c) && up_char((unsigned char)c[0]) == up_char((unsigned char)p[0])) {
            lt_buf_puts(&b, c);
        } else {
            lt_buf_append(&b, p, 2);
        }
        lt_buf_putc(&b, sep);
        lt_buf_puts(&b, p + 2);
    } else {
        lt_buf_puts(&b, c);
        if (c[0] != '\0') lt_buf_putc(&b, sep);
        lt_buf_puts(&b, p);
    }
    lt_free(p);
    lt_free(c);
    full = lt_buf_take(&b);
    n = strlen(full);
    rl = root_len(full, win);
    if (!lt_add_size(n, 4, &cap)) cap = n;                   /* n bytes are in memory already */
    out = (char *)lt_malloc(cap);
    memcpy(out, full, rl);
    k = rl;
    if (k > 0 && out[k - 1] != sep) out[k++] = sep;          /* "\\\\server\\share" + '\\' */
    rootk = base = k;
    i = rl;
    while (i < n) {
        start = i;
        while (i < n && full[i] != sep) i++;
        len = i - start;
        if (i < n) i++;
        if (len == 0 || (len == 1 && full[start] == '.')) continue;
        if (len == 2 && full[start] == '.' && full[start + 1] == '.') {
            if (k > base) {                                  /* drop the last component */
                k--;
                while (k > base && out[k - 1] != sep) k--;
            } else if (rootk == 0) {                         /* relative, nothing to drop */
                out[k++] = '.';
                out[k++] = '.';
                out[k++] = sep;
                base = k;
            }                                                /* else ".." at the root stays */
            continue;
        }
        memcpy(out + k, full + start, len);
        k += len;
        out[k++] = sep;
    }
    if (k > rootk) {
        k--;                                                 /* no trailing separator */
    } else if (k > 0 && full[rl - 1] != sep) {
        k--;                                                 /* a bare UNC root */
    } else if (k == 0) {
        out[k++] = '.';
    }
    out[k] = '\0';
    lt_free(full);
    return out;
}

lt_bool lt_plat_path_same(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return LT_FALSE;
    if (lt_plat_os() == LT_OS_WINDOWS) return lt_stricmp(a, b) == 0 ? LT_TRUE : LT_FALSE;
    return strcmp(a, b) == 0 ? LT_TRUE : LT_FALSE;
}
