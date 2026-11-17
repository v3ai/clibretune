/* restore.c - restore points (TunerStudio compatible .msq backups).
 * Spec: docs/spec/09-project.md section 4.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "proj_priv.h"
#include "../plat/plat.h"

void lt_restore_info_free(lt_restore_info *v, size_t n)
{
    size_t i;
    if (v == NULL) return;
    for (i = 0; i < n; i++) {
        lt_free(v[i].filename);
        lt_free(v[i].path);
    }
    lt_free(v);
}

static int digits(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Parse <Name>_<YYYY-MM-DD_HH.MM.SS>[-N].msq -> "YYYY-MM-DDTHH:MM:SSZ". */
lt_bool lt_restore_parse_timestamp(const char *filename, char out[21]);
lt_bool lt_restore_parse_timestamp(const char *filename, char out[21])
{
    size_t n = strlen(filename);
    char *s;
    char *p, *q;
    const char *ts;
    size_t tl;
    lt_bool ok = LT_FALSE;

    if (n < 4 || strcmp(filename + n - 4, ".msq") != 0) return LT_FALSE;
    s = lt_strndup(filename, n - 4);
    p = strrchr(s, '_');
    if (p != NULL) {
        char save = *p;
        *p = '\0';
        q = strrchr(s, '_');
        *p = save;
        if (q != NULL) {
            ts = q + 1;
            tl = strlen(ts);
            if (tl >= 19 && digits(ts, 4) && ts[4] == '-' && digits(ts + 5, 2) && ts[7] == '-' &&
                digits(ts + 8, 2) && ts[10] == '_' && digits(ts + 11, 2) && ts[13] == '.' &&
                digits(ts + 14, 2) && ts[16] == '.' && digits(ts + 17, 2)) {
                /* optional "-N" collision suffix */
                if (tl == 19 || (ts[19] == '-' && tl > 20 && digits(ts + 20, tl - 20))) {
                    lt_snprintf(out, 21, "%.10sT%.2s:%.2s:%.2sZ", ts, ts + 11, ts + 14, ts + 17);
                    ok = LT_TRUE;
                }
            }
        }
    }
    lt_free(s);
    return ok;
}

static char *restore_dir(const lt_project *p)
{
    return lt_path_join(p->path, "restorePoints");
}

int lt_restore_create(lt_project *p, char **path_out)
{
    char *dir, *safe, *fname, *full, ts[21], stamp[24];
    int rc, k;

    if (p->tune == NULL) {
        lt_err_set("no tune loaded");
        return LT_ENOENT;
    }
    dir = restore_dir(p);
    if (lt_plat_mkdirs(dir) != 0) {
        lt_err_set("cannot create '%s'", dir);
        lt_free(dir);
        return LT_EIO;
    }
    {
        /* SafeName: alphanumeric, '-', '_' only (spaces dropped) */
        const char *nm = p->cfg.name ? p->cfg.name : "";
        lt_buf b;
        lt_buf_init(&b);
        for (; *nm; nm++) {
            unsigned char c = (unsigned char)*nm;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '-' || c == '_' || c >= 0x80)
                lt_buf_putc(&b, c);
        }
        safe = b.data ? lt_buf_take(&b) : lt_strdup("");
    }
    lt_proj_utc_now(ts);
    /* YYYY-MM-DD_HH.MM.SS */
    lt_snprintf(stamp, sizeof stamp, "%.10s_%.2s.%.2s.%.2s", ts, ts + 11, ts + 14, ts + 17);
    fname = (char *)lt_malloc(strlen(safe) + 64);
    full = NULL;
    for (k = 0;; k++) {
        if (k == 0) lt_snprintf(fname, strlen(safe) + 64, "%s_%s.msq", safe, stamp);
        else lt_snprintf(fname, strlen(safe) + 64, "%s_%s-%d.msq", safe, stamp, k + 1);
        lt_free(full);
        full = lt_path_join(dir, fname);
        if (!lt_file_exists(full)) break;
    }
    lt_free(fname);
    lt_free(safe);
    lt_free(dir);
    rc = lt_tune_save(p->tune, full);
    if (rc != 0) {
        lt_free(full);
        return LT_EIO;
    }
    if (path_out) *path_out = full;
    else lt_free(full);
    /* automatic cleanup: keep the newest max_restore_points (0 = unlimited) */
    if (p->cfg.settings.max_restore_points > 0) {
        (void)lt_restore_prune(p, (size_t)p->cfg.settings.max_restore_points, NULL);
    }
    return LT_OK;
}

static int cmp_restore(const void *a, const void *b)
{
    const lt_restore_info *x = (const lt_restore_info *)a, *y = (const lt_restore_info *)b;
    int c = strcmp(y->created, x->created);
    size_t lx, ly;
    if (c != 0) return c;
    lx = strlen(x->filename);
    ly = strlen(y->filename);
    if (lx != ly) return lx < ly ? 1 : -1;    /* collision suffix = newer */
    return strcmp(y->filename, x->filename);
}

int lt_restore_list(const lt_project *p, lt_restore_info **out, size_t *n)
{
    char *dir = restore_dir(p);
    char **names = NULL;
    int cnt, i;
    lt_restore_info *v = NULL;
    size_t nv = 0;

    *out = NULL;
    *n = 0;
    cnt = lt_plat_list_dir(dir, &names);
    if (cnt < 0) {
        lt_free(dir);
        return LT_OK;
    }
    for (i = 0; i < cnt; i++) {
        char *full;
        long sz;
        if (strcmp(lt_path_ext(names[i]), ".msq") != 0) continue;
        full = lt_path_join(dir, names[i]);
        if (!lt_proj_is_file(full)) {
            lt_free(full);
            continue;
        }
        v = (lt_restore_info *)lt_realloc(v, (nv + 1) * sizeof *v);
        memset(&v[nv], 0, sizeof v[0]);
        v[nv].filename = lt_strdup(names[i]);
        v[nv].path = full;
        {
            char t[21];
            if (lt_restore_parse_timestamp(names[i], t)) lt_strlcpy(v[nv].created, t, 32);
            else lt_strlcpy(v[nv].created, "1970-01-01T00:00:00Z", 32);
        }
        sz = lt_proj_file_size(v[nv].path);
        v[nv].size_bytes = sz < 0 ? 0UL : (unsigned long)sz;
        nv++;
    }
    lt_plat_free_list(names, cnt);
    lt_free(dir);
    if (nv > 1) qsort(v, nv, sizeof *v, cmp_restore);
    *out = v;
    *n = nv;
    return LT_OK;
}

int lt_restore_path(const lt_project *p, const char *filename, char **path_out)
{
    char *dir, *full;
    if (!lt_proj_plain_component(filename)) {
        lt_err_set("invalid restore point name");
        return LT_EINVAL;
    }
    dir = restore_dir(p);
    full = lt_path_join(dir, filename);
    lt_free(dir);
    if (!lt_proj_is_file(full)) {
        lt_err_set("restore point not found: %s", filename);
        lt_free(full);
        return LT_ENOENT;
    }
    *path_out = full;
    return LT_OK;
}

int lt_restore_load(lt_project *p, const char *filename)
{
    char *full = NULL;
    lt_tune *t;
    int rc = lt_restore_path(p, filename, &full);
    if (rc != LT_OK) return rc;
    t = lt_tune_load(full);
    lt_free(full);
    if (t == NULL) return LT_EDATA;
    if (p->tune != NULL) lt_tune_free(p->tune);
    p->tune = t;
    p->dirty = LT_TRUE;
    return LT_OK;
}

int lt_restore_delete(const lt_project *p, const char *filename)
{
    char *full = NULL;
    int rc = lt_restore_path(p, filename, &full);
    if (rc != LT_OK) return rc;
    rc = lt_plat_remove(full) == 0 ? LT_OK : LT_EIO;
    lt_free(full);
    return rc;
}

int lt_restore_prune(const lt_project *p, size_t keep, size_t *deleted)
{
    lt_restore_info *v;
    size_t n, i, d = 0;
    int rc = lt_restore_list(p, &v, &n);
    if (rc != LT_OK) return rc;
    for (i = keep; i < n; i++)
        if (lt_plat_remove(v[i].path) == 0) d++;
    lt_restore_info_free(v, n);
    if (deleted) *deleted = d;
    return LT_OK;
}
