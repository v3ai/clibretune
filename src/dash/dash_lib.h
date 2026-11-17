/* dash_lib.h - dashboard library of a project: scan, create, duplicate, rename, delete, export,
 * import with a compatibility report against the INI channels; plus the compat check itself.
 * Logic only (no UI); the dashboard tab drives it.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_LIB_H
#define LT_DASH_LIB_H

#include "dash.h"

LT_BEGIN_DECLS

typedef struct lt_dashlib_entry {
    char *name;                 /* file name without extension */
    char *path;
    char *category;             /* "User", "Reference", "TunerStudio", ... */
    long size;
} lt_dashlib_entry;
typedef struct lt_dashlib { lt_dashlib_entry *e; size_t n, cap; } lt_dashlib;

void lt_dashlib_init(lt_dashlib *l);
void lt_dashlib_free(lt_dashlib *l);
/* append every *.dash of dir (not recursive); returns the number added */
int  lt_dashlib_scan(lt_dashlib *l, const char *dir, const char *category);
/* TunerStudio project folder: scans dir and its Dash / Dashboards sub folders (any letter case) */
int  lt_dashlib_scan_ts(lt_dashlib *l, const char *dir);
/* group order: "User" first, then "Reference", then alphabetical; by name inside a group */
void lt_dashlib_sort(lt_dashlib *l);

/* a usable file name (no separators / reserved characters, not empty, <= 64 chars) */
lt_bool lt_dashlib_name_ok(const char *name);
/* turn arbitrary text into a name accepted by lt_dashlib_name_ok ("dashboard" if nothing is left) */
void    lt_dashlib_safe_name(const char *in, char *out, size_t cap);
char   *lt_dashlib_path(const char *dir, const char *name);          /* dir/name.dash (malloc'd) */
/* name, then "name 2", "name 3" ... until dir/<n>.dash does not exist; written to out */
void    lt_dashlib_unique(const char *dir, const char *name, char *out, size_t cap);

/* each returns 0 or a negative LT_E* with lt_err_set; *path_out (malloc'd) is the new file */
int lt_dashlib_create(const char *dir, const char *name, const lt_dash_file *d, char **path_out);
int lt_dashlib_duplicate(const char *path, const char *new_name, char **path_out);
int lt_dashlib_rename(const char *path, const char *new_name, char **path_out);
int lt_dashlib_delete(const char *path);
int lt_dashlib_export(const char *path, const char *dest);

/* ---- compatibility against an INI ---- */
typedef struct lt_dash_compat {
    int  parsed;                /* the file could be read */
    char err[160];
    size_t gauges, indicators;
    size_t n_channels;          /* distinct channels referenced */
    size_t n_unresolved;
    char **unresolved;          /* names missing from the INI (malloc'd) */
    int  percent;               /* 0..100, resolved share (100 when nothing is referenced) */
    int  duplicate;             /* an entry with the same name already exists (import) */
} lt_dash_compat;
void lt_dash_compat_free(lt_dash_compat *c);
/* ini NULL = nothing can be checked (everything counts as resolved) */
void lt_dash_compat_check(const lt_dash_file *d, const lt_ini *ini, lt_dash_compat *out);
/* read src and report; the file is not copied. dir (may be NULL) is checked for a duplicate name */
int  lt_dashlib_preview(const char *src, const lt_ini *ini, const char *dir, lt_dash_compat *out);
/* copy src into dir under its own name, or under a unique one when overwrite is 0 and it
 * exists. Callers check rep->duplicate first. dest_out (malloc'd) may be NULL. */
int  lt_dashlib_import(const char *src, const char *dir, int overwrite, char **dest_out);

LT_END_DECLS

#endif
