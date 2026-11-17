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
