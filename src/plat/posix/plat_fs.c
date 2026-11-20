/* plat_fs.c - POSIX filesystem, clock and environment primitives.
 *
 * PLATFORM FILE: includes POSIX headers (unistd.h, dirent.h, sys/stat.h,
 * sys/select.h). Compiled with the same strict flags as universal code; the
 * build adds feature-test macros for this directory only (tools/target.sh).
 * The only #if use is feature probing of OS-defined macros, no compiler or
 * OS identity tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>

double lt_posix_clamp_time(double t)
{
    static double last = 0.0;
    if (t < last) t = last;
    last = t;
    return t;
}

void lt_plat_os_sleep_ms(int ms)
{
    struct timeval tv;
    if (ms <= 0) return;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    select(0, NULL, NULL, NULL, &tv);
}

int lt_plat_mkdir(const char *path)
{
    if (mkdir(path, 0777) == 0) return 0;
    if (errno == EEXIST && lt_plat_is_dir(path)) return 0;
    lt_err_set("mkdir %s: %s", path, strerror(errno));
    return -1;
}

lt_bool lt_plat_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return LT_FALSE;
    return S_ISDIR(st.st_mode) ? LT_TRUE : LT_FALSE;
}

int lt_plat_remove(const char *path)
{
    if (remove(path) == 0) return 0;
    if (rmdir(path) == 0) return 0;
    lt_err_set("remove %s: %s", path, strerror(errno));
    return -1;
}

int lt_plat_chmod_private(const char *path)
{
    if (chmod(path, 0600) == 0) return 0;
    lt_err_set("chmod %s: %s", path, strerror(errno));
    return -1;
}

int lt_plat_rename(const char *from, const char *to)
{
    if (rename(from, to) == 0) return 0;
    lt_err_set("rename %s: %s", from, strerror(errno));
    return -1;
}

static int write_fd_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        size_t want = n > 65536u ? 65536u : n, ok = lt_plat_fault_write_allow(want);
        ssize_t w;
        if (ok == 0) {
            errno = ENOSPC;
            return -1;
        }
        w = write(fd, p, ok);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* fsync the directory holding path, so the rename itself survives a power loss (best effort:
 * some file systems refuse fsync on directories) */
static void sync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    int fd;
    if (slash == NULL) dir = lt_strdup(".");
    else if (slash == path) dir = lt_strdup("/");
    else dir = lt_strndup(path, (size_t)(slash - path));
    fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
    lt_free(dir);
}

int lt_plat_write_atomic(const char *path, const void *data, size_t len)
{
    static unsigned long seq = 0;
    size_t tl = strlen(path) + 48;
    char *tmp = (char *)lt_malloc(tl);
    struct stat st;
    int fd = -1, tries, e;

    for (tries = 0; tries < 100; tries++) {
        lt_snprintf(tmp, tl, "%s.%lu.%lu.tmp", path, (unsigned long)getpid(), ++seq);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        lt_err_set("cannot create a temporary file for '%s': %s", path, strerror(errno));
        lt_free(tmp);
        return -1;
    }
    if (stat(path, &st) == 0) (void)fchmod(fd, st.st_mode & 07777);
    if (write_fd_all(fd, (const unsigned char *)data, len) != 0 || fsync(fd) != 0) {
        e = errno;
        close(fd);
        remove(tmp);
        lt_err_set("cannot write '%s': %s", path, strerror(e));
        lt_free(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        e = errno;
        remove(tmp);
        lt_err_set("cannot write '%s': %s", path, strerror(e));
        lt_free(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        e = errno;
        remove(tmp);
        lt_err_set("cannot replace '%s': %s", path, strerror(e));
        lt_free(tmp);
        return -1;
    }
    sync_parent_dir(path);
    lt_free(tmp);
    return 0;
}

int lt_plat_list_dir(const char *path, char ***names)
{
    DIR *d;
    struct dirent *e;
    char **arr = NULL;
    int n = 0, cap = 0;
    *names = NULL;
    d = opendir(path);
    if (d == NULL) {
        lt_err_set("opendir %s: %s", path, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n = lt_plat_names_add(&arr, n, &cap, e->d_name);
    }
    closedir(d);
    lt_plat_sort_names(arr, n);
    *names = arr;
    return n;
}

int lt_plat_setenv(const char *name, const char *value)
{
    return setenv(name, value, 1) == 0 ? 0 : -1;
}

char *lt_posix_home_dir(void)
{
    char *h = lt_plat_getenv("HOME");
    if (h == NULL) h = lt_strdup(".");
    return h;
}

char *lt_plat_home_dir(void)
{
    return lt_posix_home_dir();
}

char *lt_posix_xdg_config_dir(void)
{
    char *base, *dir;
    base = lt_plat_getenv("XDG_CONFIG_HOME");
    if (base == NULL) {
        char *h = lt_posix_home_dir();
        base = lt_path_join(h, ".config");
        lt_free(h);
    }
    dir = lt_path_join(base, "libretune");
    lt_free(base);
    lt_plat_mkdirs(dir);
    return dir;
}

int lt_posix_serial_scan(const char *const *prefixes, size_t nprefix,
                         const char *const *fixed, size_t nfixed, char ***names)
{
    char **dev = NULL;
    char **out = NULL;
    int nd, n = 0, cap = 0, i;
    size_t j;
    char path[300];

    *names = NULL;
    nd = lt_plat_list_dir("/dev", &dev);
    if (nd < 0) return 0;
    for (i = 0; i < nd; i++) {
        int match = 0;
        for (j = 0; j < nprefix && !match; j++) {
            if (lt_str_starts(dev[i], prefixes[j])) match = 1;
        }
        for (j = 0; j < nfixed && !match; j++) {
            if (strcmp(dev[i], fixed[j]) == 0) match = 1;
        }
        if (match) {
            lt_snprintf(path, sizeof path, "/dev/%s", dev[i]);
            n = lt_plat_names_add(&out, n, &cap, path);
        }
    }
    lt_plat_free_list(dev, nd);
    *names = out;
    return n;
}
