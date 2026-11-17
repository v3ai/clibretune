/* dash_lib.c - dashboard library operations (see dash_lib.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "dash_lib.h"
#include "../plat/plat.h"

void lt_dashlib_init(lt_dashlib *l) { memset(l, 0, sizeof *l); }

void lt_dashlib_free(lt_dashlib *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) { lt_free(l->e[i].name); lt_free(l->e[i].path); lt_free(l->e[i].category); }
    lt_free(l->e);
    memset(l, 0, sizeof *l);
}

static lt_bool has_dash_ext(const char *n)
{
    size_t k = strlen(n);
    return k > 5 && lt_stricmp(n + k - 5, ".dash") == 0;
}

int lt_dashlib_scan(lt_dashlib *l, const char *dir, const char *category)
{
    char **names = NULL;
    int n, i, added = 0;
    if (dir == NULL || !lt_plat_is_dir(dir)) { return 0; }
    n = lt_plat_list_dir(dir, &names);
    for (i = 0; i < n; i++) {
        lt_dashlib_entry *e;
        char *p;
        size_t k, len = 0;
        char *data;
        if (!has_dash_ext(names[i])) { continue; }
        p = lt_path_join(dir, names[i]);
        if (lt_plat_is_dir(p)) { lt_free(p); continue; }
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 16;
            l->e = (lt_dashlib_entry *)lt_realloc(l->e, sizeof(lt_dashlib_entry) * l->cap);
        }
        e = &l->e[l->n++];
        k = strlen(names[i]) - 5;
        e->name = (char *)lt_malloc(k + 1);
        memcpy(e->name, names[i], k);
        e->name[k] = '\0';
        e->path = p;
        e->category = lt_strdup(category != NULL ? category : "User");
        e->size = 0;
        data = lt_file_read(p, &len);
        if (data != NULL) { e->size = (long)len; lt_free(data); }
        added++;
    }
    if (names != NULL) { lt_plat_free_list(names, n); }
    return added;
}

int lt_dashlib_scan_ts(lt_dashlib *l, const char *dir)
{
    char **names = NULL;
    int n, i, added = lt_dashlib_scan(l, dir, "TunerStudio");
    if (dir == NULL || !lt_plat_is_dir(dir)) { return added; }
    n = lt_plat_list_dir(dir, &names);
    for (i = 0; i < n; i++) {                /* "Dash" / "Dashboards" in any letter case, once each */
        if (lt_stricmp(names[i], "dash") == 0 || lt_stricmp(names[i], "dashboards") == 0) {
            char *p = lt_path_join(dir, names[i]);
            added += lt_dashlib_scan(l, p, "TunerStudio");
            lt_free(p);
        }
    }
    if (names != NULL) { lt_plat_free_list(names, n); }
    return added;
}

static int cat_rank(const char *c)
{
    if (strcmp(c, "User") == 0) { return 0; }
    if (strcmp(c, "Reference") == 0) { return 1; }
    return 2;
}

void lt_dashlib_sort(lt_dashlib *l)
{
    size_t i, j;
    for (i = 1; i < l->n; i++) {            /* insertion sort: stable, lists are short */
        lt_dashlib_entry key = l->e[i];
        j = i;
        while (j > 0) {
            const lt_dashlib_entry *p = &l->e[j - 1];
            int rp = cat_rank(p->category), rk = cat_rank(key.category);
            int c = rp != rk ? (rp < rk ? -1 : 1) : strcmp(p->category, key.category);
            if (c == 0) { c = lt_stricmp(p->name, key.name); }
            if (c <= 0) { break; }
            l->e[j] = l->e[j - 1];
            j--;
        }
        l->e[j] = key;
    }
}

lt_bool lt_dashlib_name_ok(const char *name)
{
    size_t n, i;
    if (name == NULL) { return LT_FALSE; }
    n = strlen(name);
    if (n == 0 || n > 64 || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.') {
        return LT_FALSE;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || strchr("/\\:*?\"<>|", (int)c) != NULL) { return LT_FALSE; }
    }
    return LT_TRUE;
}

void lt_dashlib_safe_name(const char *in, char *out, size_t cap)
{
    size_t o = 0, i;
    char tmp[80];
    if (cap == 0) { return; }
    for (i = 0; in != NULL && in[i] != '\0' && o < 64; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 32 || strchr("/\\:*?\"<>|", (int)c) != NULL) { c = '_'; }
        tmp[o++] = (char)c;
    }
    tmp[o] = '\0';
    while (o > 0 && (tmp[o - 1] == ' ' || tmp[o - 1] == '.')) { tmp[--o] = '\0'; }
    for (i = 0; tmp[i] == ' ' || tmp[i] == '.'; i++) { }
    if (tmp[i] == '\0') { lt_strlcpy(out, "dashboard", cap); }
    else { lt_strlcpy(out, tmp + i, cap); }
}

char *lt_dashlib_path(const char *dir, const char *name)
{
    char fn[96];
    char *p;
    lt_snprintf(fn, sizeof fn, "%s.dash", name);
    p = lt_path_join(dir, fn);
    return p;
}

void lt_dashlib_unique(const char *dir, const char *name, char *out, size_t cap)
{
    int k;
    char cand[96];
    lt_strlcpy(cand, name, sizeof cand);
    for (k = 2; k < 1000; k++) {
        char *p = lt_dashlib_path(dir, cand);
        lt_bool ex = lt_file_exists(p);
        lt_free(p);
        if (!ex) { break; }
        lt_snprintf(cand, sizeof cand, "%.60s %d", name, k);
    }
    lt_strlcpy(out, cand, cap);
}

static int check_name(const char *name)
{
    if (!lt_dashlib_name_ok(name)) { lt_err_set("invalid dashboard name '%s'", name ? name : ""); return LT_EINVAL; }
    return 0;
}

/* directory part of path (malloc'd) */
static char *dir_of(const char *path)
{
    const char *b = lt_path_base(path);
    size_t n = (size_t)(b - path);
    char *d;
    if (n == 0) { return lt_strdup("."); }
    d = (char *)lt_malloc(n + 1);
    memcpy(d, path, n);
    d[n - 1] = '\0';                      /* drop the separator */
    return d;
}

int lt_dashlib_create(const char *dir, const char *name, const lt_dash_file *d, char **path_out)
{
    char *p;
    int rc = check_name(name);
    if (rc != 0) { return rc; }
    if (lt_plat_mkdirs(dir) != 0) { lt_err_set("cannot create %s", dir); return LT_EIO; }
    p = lt_dashlib_path(dir, name);
    if (lt_file_exists(p)) { lt_err_set("a dashboard named '%s' already exists", name); lt_free(p); return LT_EEXIST; }
    if (lt_dash_save(d, p) != 0) { lt_free(p); return LT_EIO; }
    if (path_out != NULL) { *path_out = p; } else { lt_free(p); }
    return 0;
}

int lt_dashlib_duplicate(const char *path, const char *new_name, char **path_out)
{
    char *dir, *p;
    int rc = check_name(new_name);
    if (rc != 0) { return rc; }
    dir = dir_of(path);
    p = lt_dashlib_path(dir, new_name);
    lt_free(dir);
    if (lt_file_exists(p)) { lt_err_set("a dashboard named '%s' already exists", new_name); lt_free(p); return LT_EEXIST; }
    if (lt_plat_copy_file(path, p) != 0) { lt_err_set("cannot copy %s", path); lt_free(p); return LT_EIO; }
    if (path_out != NULL) { *path_out = p; } else { lt_free(p); }
    return 0;
}

int lt_dashlib_rename(const char *path, const char *new_name, char **path_out)
{
    char *dir, *p;
    int rc = check_name(new_name);
    if (rc != 0) { return rc; }
    dir = dir_of(path);
    p = lt_dashlib_path(dir, new_name);
    lt_free(dir);
    if (strcmp(p, path) != 0) {
        if (lt_file_exists(p)) {
            lt_err_set("a dashboard named '%s' already exists", new_name); lt_free(p); return LT_EEXIST;
        }
        if (lt_plat_rename(path, p) != 0) { lt_err_set("cannot rename %s", path); lt_free(p); return LT_EIO; }
    }
    if (path_out != NULL) { *path_out = p; } else { lt_free(p); }
    return 0;
}

int lt_dashlib_delete(const char *path)
{
    if (!lt_file_exists(path)) { lt_err_set("%s does not exist", path); return LT_ENOENT; }
    if (lt_plat_remove(path) != 0) { lt_err_set("cannot delete %s", path); return LT_EIO; }
    return 0;
}

int lt_dashlib_export(const char *path, const char *dest)
{
    if (lt_plat_copy_file(path, dest) != 0) { lt_err_set("cannot write %s", dest); return LT_EIO; }
    return 0;
}

/* ---- compatibility ---- */
void lt_dash_compat_free(lt_dash_compat *c)
{
    size_t i;
    for (i = 0; i < c->n_unresolved; i++) { lt_free(c->unresolved[i]); }
    lt_free(c->unresolved);
    memset(c, 0, sizeof *c);
}

static lt_bool ini_has(const lt_ini *ini, const char *name)
{
    size_t i;
    for (i = 0; i < ini->n_channels; i++) {
        if (strcmp(ini->channels[i]->name, name) == 0) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static lt_bool seen(char **v, size_t n, const char *s)
{
    size_t i;
    for (i = 0; i < n; i++) { if (strcmp(v[i], s) == 0) { return LT_TRUE; } }
    return LT_FALSE;
}

void lt_dash_compat_check(const lt_dash_file *d, const lt_ini *ini, lt_dash_compat *out)
{
    const lt_dash_cluster *c = &d->cluster;
    char **all = (char **)lt_calloc(c->n_comps + 1, sizeof(char *));
    size_t i, na = 0, ok = 0;
    memset(out, 0, sizeof *out);
    out->parsed = 1;
    out->unresolved = (char **)lt_calloc(c->n_comps + 1, sizeof(char *));
    for (i = 0; i < c->n_comps; i++) {
        const char *ch = NULL;
        if (c->comps[i].gauge != NULL) { out->gauges++; ch = c->comps[i].gauge->output_channel; }
        else if (c->comps[i].indicator != NULL) {
            out->indicators++;
            if (lt_dash_extras_get(&c->comps[i].indicator->extras, "lt_expression") == NULL) {
                ch = c->comps[i].indicator->output_channel;
            }
        }
        if (ch == NULL || ch[0] == '\0' || seen(all, na, ch)) { continue; }
        all[na++] = (char *)ch;
        if (ini == NULL || ini_has(ini, ch)) { ok++; }
        else { out->unresolved[out->n_unresolved++] = lt_strdup(ch); }
    }
    out->n_channels = na;
    out->percent = na == 0 ? 100 : (int)((ok * 100 + na / 2) / na);
    lt_free(all);
}

/* base name of src without a ".dash" extension, made filename-safe */
static void import_name(const char *src, char *out, size_t cap)
{
    char nm[96];
    const char *b = lt_path_base(src);
    size_t k = strlen(b);
    if (k > 5 && has_dash_ext(b)) { k -= 5; }
    if (k > 80) { k = 80; }
    memcpy(nm, b, k);
    nm[k] = '\0';
    lt_dashlib_safe_name(nm, out, cap);
}

int lt_dashlib_preview(const char *src, const lt_ini *ini, const char *dir, lt_dash_compat *out)
{
    lt_dash_file *d = lt_dash_load(src);
    memset(out, 0, sizeof *out);
    if (d == NULL) {
        lt_snprintf(out->err, sizeof out->err, "%s", lt_err_get());
        return LT_EDATA;
    }
    lt_dash_compat_check(d, ini, out);
    lt_dash_free(d);
    if (dir != NULL) {
        char nm[96], *p;
        import_name(src, nm, sizeof nm);
        p = lt_dashlib_path(dir, nm);
        out->duplicate = lt_file_exists(p) ? 1 : 0;
        lt_free(p);
    }
    return 0;
}

int lt_dashlib_import(const char *src, const char *dir, int overwrite, char **dest_out)
{
    char nm[96], uniq[96], *p;
    import_name(src, nm, sizeof nm);
    if (lt_plat_mkdirs(dir) != 0) { lt_err_set("cannot create %s", dir); return LT_EIO; }
    if (overwrite) { lt_strlcpy(uniq, nm, sizeof uniq); }
    else { lt_dashlib_unique(dir, nm, uniq, sizeof uniq); }
    p = lt_dashlib_path(dir, uniq);
    if (lt_plat_copy_file(src, p) != 0) { lt_err_set("cannot copy %s", src); lt_free(p); return LT_EIO; }
    if (dest_out != NULL) { *dest_out = p; } else { lt_free(p); }
    return 0;
}
