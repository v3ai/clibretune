/* files_logic.c - see files_logic.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "files_logic.h"
#include "../plat/plat.h"
#include "../project/project.h"
#include "../datalog/datalog.h"

static char **grow_ptrs(char **p, size_t n)
{
    size_t bytes;
    if (!lt_mul_size(n, sizeof(char *), &bytes)) { bytes = (size_t)-1; }     /* OOM abort */
    return (char **)lt_realloc(p, bytes);
}

static int ext_is(const char *name, const char *ext)
{
    const char *e = lt_path_ext(name);
    return e != NULL && lt_stricmp(e, ext) == 0;
}

/* number of regular entries of dir with the extension (".msq") */
static int count_ext(const char *dir, const char *ext)
{
    char **names = NULL;
    int n, i, c = 0;
    if (!lt_plat_is_dir(dir)) { return 0; }
    n = lt_plat_list_dir(dir, &names);
    for (i = 0; i < n; i++) {
        if (ext_is(names[i], ext)) { c++; }
    }
    if (n >= 0) { lt_plat_free_list(names, n); }
    return c;
}

static char *join3(const char *a, const char *b, const char *c)
{
    char *t = lt_path_join(a, b);
    char *r = lt_path_join(t, c);
    lt_free(t);
    return r;
}

int lt_ts_preview_read(const char *ts_dir, lt_ts_preview *out)
{
    char *props_path = join3(ts_dir, "projectCfg", "project.properties");
    lt_props pr;
    const char *v;
    char *p;
    int iv = 0;
    memset(out, 0, sizeof(*out));
    if (!lt_file_exists(props_path)) {
        lt_err_set("Not a valid TS project: project.properties not found");
        lt_free(props_path);
        return LT_ENOENT;
    }
    lt_props_init(&pr);
    if (lt_props_load(&pr, props_path) != 0) {
        lt_free(props_path);
        lt_props_free(&pr);
        return LT_EIO;
    }
    lt_free(props_path);
    v = lt_props_get(&pr, "projectName");
    lt_strlcpy(out->name, v != NULL && v[0] != '\0' ? v : lt_path_base(ts_dir), sizeof(out->name));
    v = lt_props_get(&pr, "ecuConfigFile");
    if (v != NULL) { v = lt_path_base(v); }   /* only a file inside projectCfg/ (as the import) */
    if (v != NULL && (strcmp(v, ".") == 0 || strcmp(v, "..") == 0)) { v = NULL; }
    lt_strlcpy(out->ini_file, v != NULL ? v : "", sizeof(out->ini_file));
    v = lt_props_get(&pr, "commPort");
    lt_strlcpy(out->port, v != NULL ? v : "", sizeof(out->port));
    if (lt_props_get_i32(&pr, "baudRate", &iv) && iv > 0) { out->baud = iv; }
    lt_props_free(&pr);

    p = join3(ts_dir, "projectCfg", out->ini_file[0] != '\0' ? out->ini_file : "mainController.ini");
    if (p != NULL && lt_file_exists(p)) {
        size_t len = 0;
        char *txt = lt_file_read(p, &len);
        out->ini_found = 1;
        if (txt != NULL) {
            char *sig = NULL, *nm = NULL;
            (void)lt_ini_extract_info(txt, &sig, &nm);
            if (sig != NULL) { lt_strlcpy(out->signature, sig, sizeof(out->signature)); }
            lt_free(sig); lt_free(nm);
        }
        lt_free(txt);
    }
    lt_free(p);
    p = lt_path_join(ts_dir, "CurrentTune.msq");
    out->has_tune = lt_file_exists(p);
    lt_free(p);
    p = join3(ts_dir, "projectCfg", "pcVariableValues.msq");
    out->has_pcv = lt_file_exists(p);
    lt_free(p);
    p = lt_path_join(ts_dir, "restorePoints");
    out->n_restore = count_ext(p, ".msq");
    lt_free(p);
    p = lt_path_join(ts_dir, "Dash");
    out->n_dash = count_ext(p, ".dash");
    lt_free(p);
    p = lt_path_join(ts_dir, "dashboards");
    out->n_dash += count_ext(p, ".dash");
    lt_free(p);
    out->n_dash += count_ext(ts_dir, ".dash");
    return 0;
}

static int copy_dashes(const char *src_dir, const char *dst_dir)
{
    char **names = NULL;
    int n, i, c = 0;
    if (!lt_plat_is_dir(src_dir)) { return 0; }
    n = lt_plat_list_dir(src_dir, &names);
    for (i = 0; i < n; i++) {
        if (ext_is(names[i], ".dash") || ext_is(names[i], ".gauge")) {
            char *s = lt_path_join(src_dir, names[i]);
            char *d = lt_path_join(dst_dir, names[i]);
            if (!lt_plat_is_dir(s) && lt_plat_copy_file(s, d) == 0) { c++; }
            lt_free(s);
            lt_free(d);
        }
    }
    if (n >= 0) { lt_plat_free_list(names, n); }
    return c;
}

int lt_ts_import_dashboards(const char *ts_dir, const char *project_dir)
{
    char *dst = lt_path_join(project_dir, "dashboards");
    char *a, *b;
    int c = 0;
    (void)lt_plat_mkdirs(dst);
    a = lt_path_join(ts_dir, "Dash");
    b = lt_path_join(ts_dir, "dashboards");
    c += copy_dashes(a, dst);
    c += copy_dashes(b, dst);
    c += copy_dashes(ts_dir, dst);
    lt_free(a); lt_free(b); lt_free(dst);
    return c;
}

int lt_tuneinfo_read(const char *path, lt_tuneinfo *out)
{
    lt_tune *t = lt_tune_load(path);
    size_t len = 0, i;
    char *raw;
    memset(out, 0, sizeof(*out));
    if (t == NULL) { return LT_EDATA; }
    lt_strlcpy(out->file, lt_path_base(path), sizeof(out->file));
    lt_strlcpy(out->signature, t->signature != NULL ? t->signature : "", sizeof(out->signature));
    lt_strlcpy(out->version, t->version != NULL ? t->version : "", sizeof(out->version));
    lt_strlcpy(out->saved, t->modified != NULL ? t->modified : "", sizeof(out->saved));
    lt_strlcpy(out->author, t->author != NULL ? t->author : "", sizeof(out->author));
    lt_strlcpy(out->desc, t->description != NULL ? t->description : "", sizeof(out->desc));
    out->constants = lt_tune_n_constants(t);
    for (i = 0; i < 256; i++) { if (t->pages[i] != NULL) { out->pages++; } }
    lt_tune_free(t);
    raw = lt_file_read(path, &len);
    out->size = (unsigned long)len;
    lt_free(raw);
    return 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int lt_tunes_list(const char *dir, char ***names, size_t *n)
{
    char **all = NULL, **out = NULL;
    int cnt, i;
    size_t got = 0;
    *names = NULL;
    *n = 0;
    if (dir == NULL || !lt_plat_is_dir(dir)) { return -1; }
    cnt = lt_plat_list_dir(dir, &all);
    if (cnt < 0) { return -1; }
    for (i = 0; i < cnt; i++) {
        if (ext_is(all[i], ".msq") || ext_is(all[i], ".xml")) {
            char *full = lt_path_join(dir, all[i]);
            if (!lt_plat_is_dir(full)) {
                out = (char **)grow_ptrs(out, got + 1);
                out[got++] = lt_strdup(all[i]);
            }
            lt_free(full);
        }
    }
    lt_plat_free_list(all, cnt);
    if (got > 1) { qsort(out, got, sizeof(char *), cmp_names); }
    *names = out;
    *n = got;
    return 0;
}

void lt_tunes_new_name(const char *dir, const char *name, char *out, size_t cap)
{
    char base[96], cand[128];
    size_t k = 0, i;
    int n;
    for (i = 0; name[i] != '\0' && k + 1 < sizeof(base); i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
            || c == '>' || c == '|' || (unsigned char)c < 32) { c = '_'; }
        base[k++] = c;
    }
    base[k] = '\0';
    while (k > 0 && (base[k - 1] == ' ' || base[k - 1] == '.')) { base[--k] = '\0'; }
    if (k == 0 || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        lt_strlcpy(base, "NewTune", sizeof(base));
    }
    lt_snprintf(cand, sizeof(cand), "%s.msq", base);
    for (n = 2; n < 1000; n++) {
        char *full = lt_path_join(dir, cand);
        int exists = lt_file_exists(full);
        lt_free(full);
        if (!exists) { break; }
        lt_snprintf(cand, sizeof(cand), "%s %d.msq", base, n);
    }
    lt_strlcpy(out, cand, cap);
}
