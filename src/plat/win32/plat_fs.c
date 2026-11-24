/* plat_fs.c - Windows filesystem primitives.
 *
 * PLATFORM FILE: uses Win32 headers via win_priv.h (see the exception noted
 * there). Built only for TARGET=windows.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

#include <stdlib.h>
#include <string.h>

lt_bool lt_plat_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return LT_FALSE;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? LT_TRUE : LT_FALSE;
}

int lt_plat_mkdir(const char *path)
{
    if (CreateDirectoryA(path, NULL)) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS && lt_plat_is_dir(path)) return 0;
    lt_err_set("mkdir %s failed (%lu)", path, (unsigned long)GetLastError());
    return -1;
}

int lt_plat_chmod_private(const char *path)
{
    /* the profile directory is already private to the user */
    LT_UNUSED(path);
    return 0;
}

int lt_plat_remove(const char *path)
{
    if (lt_plat_is_dir(path)) {
        if (RemoveDirectoryA(path)) return 0;
    } else if (DeleteFileA(path)) {
        return 0;
    }
    lt_err_set("remove %s failed (%lu)", path, (unsigned long)GetLastError());
    return -1;
}

int lt_plat_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return 0;
    lt_err_set("rename %s failed (%lu)", from, (unsigned long)GetLastError());
    return -1;
}

static void win_err(const char *what, const char *path, DWORD e)
{
    lt_err_set("%s '%s' failed (%lu)", what, path, (unsigned long)e);
}

int lt_plat_write_atomic(const char *path, const void *data, size_t len)
{
    static unsigned long seq = 0;
    size_t tl = strlen(path) + 48;
    char *tmp = (char *)lt_malloc(tl);
    const unsigned char *p = (const unsigned char *)data;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD e = 0, attr;
    int tries;

    for (tries = 0; tries < 100; tries++) {
        lt_snprintf(tmp, tl, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), ++seq);
        h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        win_err("create temporary file for", path, e);
        lt_free(tmp);
        return -1;
    }
    e = 0;
    while (len > 0) {
        size_t want = len > 65536u ? 65536u : len, ok = lt_plat_fault_write_allow(want);
        DWORD w = 0;
        if (ok == 0) { e = ERROR_DISK_FULL; break; }
        if (!WriteFile(h, p, (DWORD)ok, &w, NULL) || w == 0) { e = GetLastError(); break; }
        p += w;
        len -= (size_t)w;
    }
    if (len == 0 && !FlushFileBuffers(h)) e = GetLastError();
    if (!CloseHandle(h) && len == 0 && e == 0) e = GetLastError();
    if (len != 0 || e != 0) {
        DeleteFileA(tmp);
        win_err("write", path, e);
        lt_free(tmp);
        return -1;
    }
    attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        DeleteFileA(tmp);
        win_err("replace read-only", path, ERROR_ACCESS_DENIED);
        lt_free(tmp);
        return -1;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        e = GetLastError();
        DeleteFileA(tmp);
        win_err("replace", path, e);
        lt_free(tmp);
        return -1;
    }
    lt_free(tmp);
    return 0;
}

/* ANSI code page <-> UTF-16 like the *A file functions the rest of this layer uses, so the result
 * opens with them; NULL on failure */
static WCHAR *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    size_t bytes = 0;
    WCHAR *w;
    if (n <= 0 || !lt_mul_size((size_t)n, sizeof(WCHAR), &bytes)) return NULL;
    w = (WCHAR *)lt_malloc(bytes);
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, w, n) != n) {
        lt_free(w);
        return NULL;
    }
    return w;
}

static char *from_wide(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)lt_malloc((size_t)n);
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, s, n, NULL, NULL) != n) {
        lt_free(s);
        return NULL;
    }
    return s;
}

char *lt_plat_abspath(const char *path)
{
    WCHAR *w, *full = NULL;
    DWORD n = 0, got = 0;
    size_t bytes = 0;
    char *a = NULL, *r;
    if (path == NULL || path[0] == '\0') return NULL;
    w = to_wide(path);
    if (w != NULL) n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n > 0 && lt_mul_size((size_t)n, sizeof(WCHAR), &bytes)) {
        full = (WCHAR *)lt_malloc(bytes);
        got = GetFullPathNameW(w, n, full, NULL);
        if (got > 0 && got < n) a = from_wide(full);
    }
    lt_free(full);
    lt_free(w);
    /* GetFullPathName folds "." and ".."; the fold normalises the separators and the end */
    r = lt_plat_path_fold("", a != NULL ? a : path, LT_TRUE);
    lt_free(a);
    return r;
}

int lt_plat_list_dir(const char *path, char ***names)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *pat;
    char **arr = NULL;
    int n = 0, cap = 0;
    *names = NULL;
    pat = lt_path_join(path, "*");
    h = FindFirstFileA(pat, &fd);
    lt_free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        lt_err_set("cannot list %s", path);
        return -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = (char **)lt_realloc(arr, (size_t)cap * sizeof(char *));
        }
        arr[n++] = lt_strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    lt_plat_sort_names(arr, n);
    *names = arr;
    return n;
}
