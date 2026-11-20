/* repo.c - local INI repository: <dir>/index.json + <id>.ini files.
 * Spec: docs/spec/09-project.md section 3.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "proj_priv.h"
#include "../plat/plat.h"

static void entry_clear(lt_ini_entry *e)
{
    lt_free(e->id);
    lt_free(e->name);
    lt_free(e->signature);
    lt_free(e->path);
    lt_free(e->imported);
    lt_free(e->source);
    memset(e, 0, sizeof *e);
}

void lt_ini_repo_close(lt_ini_repo *r)
{
    size_t i;
    if (r == NULL) return;
    for (i = 0; i < r->n; i++) entry_clear(&r->e[i]);
    lt_free(r->e);
    lt_free(r->dir);
    memset(r, 0, sizeof *r);
}

static char *default_repo_dir(void)
{
    char *base = NULL, *r, *home, *t;
    lt_os os = lt_plat_os();

    base = lt_plat_getenv("LIBRETUNE_DEFINITIONS");     /* explicit location (tests, portable use) */
    if (base != NULL && base[0] != '\0') { return base; }
    lt_free(base);
    base = NULL;
    if (os == LT_OS_WINDOWS) {
        base = lt_plat_getenv("LOCALAPPDATA");
    } else if (os == LT_OS_MACOS) {
        home = lt_plat_getenv("HOME");
        if (home) {
            base = lt_proj_join3(home, "Library", "Application Support");
            lt_free(home);
        }
    } else {
        base = lt_plat_getenv("XDG_DATA_HOME");
        if (base == NULL || base[0] == '\0') {
            lt_free(base);
            base = NULL;
            home = lt_plat_getenv("HOME");
            if (home) {
                base = lt_proj_join3(home, ".local", "share");
                lt_free(home);
            }
        }
    }
    if (base == NULL || base[0] == '\0') {
        lt_free(base);
        base = lt_plat_getenv("HOME");
        if (base == NULL || base[0] == '\0') {
            lt_free(base);
            base = lt_plat_getenv("USERPROFILE");
        }
    }
    if (base == NULL || base[0] == '\0') {
        lt_free(base);
        lt_err_set("no data directory available");
        return NULL;
    }
    t = lt_path_join(base, "LibreTune");
    r = lt_path_join(t, "definitions");
    lt_free(t);
    lt_free(base);
    return r;
}

static int index_save(const lt_ini_repo *r)
{
    lt_json *arr = lt_json_arr();
    size_t i;
    char *s, *f;
    int rc;
    for (i = 0; i < r->n; i++) {
        const lt_ini_entry *e = &r->e[i];
        lt_json *o = lt_json_obj();
        lt_json_obj_set(o, "id", lt_json_str(e->id));
        lt_json_obj_set(o, "name", lt_json_str(e->name));
        lt_json_obj_set(o, "signature", lt_json_str(e->signature));
        lt_json_obj_set(o, "path", lt_json_str(e->path));
        lt_json_obj_set(o, "imported", lt_json_str(e->imported ? e->imported : ""));
        lt_json_obj_set(o, "source", e->source ? lt_json_str(e->source) : lt_json_null());
        lt_json_arr_push(arr, o);
    }
    s = lt_json_to_string(arr, 1);
    lt_json_free(arr);
    f = lt_path_join(r->dir, "index.json");
    rc = lt_proj_write_atomic(f, s, strlen(s));
    lt_free(f);
    lt_free(s);
    return rc;
}

static lt_ini_entry *repo_push(lt_ini_repo *r)
{
    r->e = (lt_ini_entry *)lt_realloc(r->e, (r->n + 1) * sizeof *r->e);
    memset(&r->e[r->n], 0, sizeof r->e[0]);
    return &r->e[r->n++];
}

int lt_ini_repo_open(lt_ini_repo *r, const char *dir)
{
    char *f, err[160];
    lt_json *j;
    size_t i;

    memset(r, 0, sizeof *r);
    r->dir = dir ? lt_strdup(dir) : default_repo_dir();
    if (r->dir == NULL) return LT_ENOENT;
    if (lt_plat_mkdirs(r->dir) != 0) {
        lt_err_set("cannot create '%s'", r->dir);
        lt_free(r->dir);
        r->dir = NULL;
        return LT_EIO;
    }
    f = lt_path_join(r->dir, "index.json");
    if (!lt_file_exists(f)) {
        lt_free(f);
        return LT_OK;
    }
    j = lt_json_load(f, err, sizeof err);
    if (j == NULL || j->type != LT_JSON_ARR) {
        lt_err_set("%s: invalid index (%s)", f, j ? "not an array" : err);
        lt_json_free(j);
        lt_free(f);
        lt_ini_repo_close(r);
        return LT_EDATA;
    }
    lt_free(f);
    for (i = 0; i < j->n; i++) {
        const lt_json *o = j->items[i];
        const char *id = lt_json_get_str(o, "id", NULL);
        const char *name = lt_json_get_str(o, "name", NULL);
        const char *sig = lt_json_get_str(o, "signature", NULL);
        const char *path = lt_json_get_str(o, "path", NULL);
        lt_ini_entry *e;
        if (o->type != LT_JSON_OBJ || !id || !name || !sig || !path) {
            lt_err_set("index.json entry %lu is incomplete", (unsigned long)i);
            lt_json_free(j);
            lt_ini_repo_close(r);
            return LT_EDATA;
        }
        e = repo_push(r);
        e->id = lt_strdup(id);
        e->name = lt_strdup(name);
        e->signature = lt_strdup(sig);
        e->path = lt_strdup(path);
        e->imported = lt_strdup(lt_json_get_str(o, "imported", ""));
        e->source = lt_proj_dup_or_null(lt_json_get_str(o, "source", NULL));
    }
    lt_json_free(j);
    return LT_OK;
}

const lt_ini_entry *lt_ini_repo_get(const lt_ini_repo *r, const char *id)
{
    size_t i;
    if (id == NULL) return NULL;
    for (i = 0; i < r->n; i++)
        if (strcmp(r->e[i].id, id) == 0) return &r->e[i];
    return NULL;
}

const lt_ini_entry *lt_ini_repo_find_signature(const lt_ini_repo *r, const char *signature,
                                               lt_bool prefix_match)
{
    size_t i;
    if (signature == NULL) return NULL;
    for (i = 0; i < r->n; i++)
        if (strcmp(r->e[i].signature, signature) == 0) return &r->e[i];
    if (prefix_match) {
        for (i = 0; i < r->n; i++) {
            const char *s = r->e[i].signature;
            if (s[0] == '\0') continue;
            if (lt_str_istarts(s, signature) || lt_str_istarts(signature, s)) return &r->e[i];
        }
    }
    return NULL;
}

char *lt_ini_repo_path(const lt_ini_repo *r, const char *id)
{
    const lt_ini_entry *e = lt_ini_repo_get(r, id);
    if (e == NULL) return NULL;
    return lt_path_join(r->dir, e->path);
}

int lt_ini_repo_remove(lt_ini_repo *r, const char *id)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].id, id) == 0) {
            char *f;
            if (lt_proj_plain_component(r->e[i].path)) {
                f = lt_path_join(r->dir, r->e[i].path);
                if (lt_file_exists(f)) lt_plat_remove(f);
                lt_free(f);
            }
            entry_clear(&r->e[i]);
            for (; i + 1 < r->n; i++) r->e[i] = r->e[i + 1];
            r->n--;
            break;
        }
    }
    return index_save(r);
}

/* ---- INI info ---- */
static char *extract_quoted(const char *v)
{
    char *s = lt_strdup(v), *q1, *q2, *r;
    size_t l;
    lt_str_trim(s);
    q1 = strchr(s, '"');
    if (q1 != NULL) {
        q2 = strchr(q1 + 1, '"');
        if (q2 != NULL) {
            r = lt_strndup(q1 + 1, (size_t)(q2 - q1 - 1));
            lt_free(s);
            return r;
        }
    }
    q1 = strchr(s, ';');
    if (q1) *q1 = '\0';
    lt_str_trim(s);
    l = strlen(s);
    if (l > 0 && s[l - 1] == '"') s[--l] = '\0';
    r = lt_strdup(s[0] == '"' ? s + 1 : s);
    lt_free(s);
    return r;
}

int lt_ini_extract_info(const char *ini_text, char **signature, char **name)
{
    const char *cur = ini_text;
    char *sig = lt_strdup(""), *nm = lt_strdup("");
    int in_section = 0;

    while (*cur != '\0') {
        const char *eol = strchr(cur, '\n');
        size_t len = eol ? (size_t)(eol - cur) : strlen(cur);
        char *line = lt_strndup(cur, len), *t;
        cur = eol ? eol + 1 : cur + len;
        t = lt_str_trim(line);
        if (t[0] == '[') {
            char *semi = strchr(t, ';'), *h = t;
            size_t l;
            if (semi) *semi = '\0';
            lt_str_trim(t);
            while (*h == '[' || *h == ']') h++;
            l = strlen(h);
            while (l > 0 && (h[l - 1] == ']' || h[l - 1] == '[')) h[--l] = '\0';
            lt_str_trim(h);
            h = lt_str_trim(h);
            in_section = (lt_stricmp(h, "MegaTune") == 0 || lt_stricmp(h, "TunerStudio") == 0);
        } else if (in_section) {
            char *eq = strchr(t, '=');
            if (eq != NULL) {
                char *key = lt_strndup(t, (size_t)(eq - t));
                lt_str_trim(key);
                if (sig[0] == '\0' && lt_stricmp(key, "signature") == 0) {
                    lt_free(sig);
                    sig = extract_quoted(eq + 1);
                } else if (nm[0] == '\0' && lt_stricmp(key, "nEmu") == 0) {
                    lt_free(nm);
                    nm = extract_quoted(eq + 1);
                }
                lt_free(key);
            }
        }
        lt_free(line);
    }
    if (sig[0] == '\0') {
        lt_free(sig);
        lt_free(nm);
        lt_err_set("Could not find signature in [MegaTune] or [TunerStudio] section of INI file");
        return LT_EDATA;
    }
    if (nm[0] == '\0') {
        lt_free(nm);
        nm = lt_strdup(sig);
    }
    if (signature) *signature = sig;
    else lt_free(sig);
    if (name) *name = nm;
    else lt_free(nm);
    return LT_OK;
}

static char *make_id(const char *sig)
{
    char *id = lt_strdup(sig);
    char *p;
    for (p = id; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_' || c == '.' || c >= 0x80))
            *p = '_';
    }
    return id;
}

int lt_ini_repo_import(lt_ini_repo *r, const char *src_path, char **id_out)
{
    size_t n = 0;
    char *raw = lt_file_read(src_path, &n);
    int rc;
    if (raw == NULL) return LT_ENOENT;
    rc = lt_ini_repo_import_data(r, raw, n, src_path, id_out);
    lt_free(raw);
    return rc;
}

int lt_ini_repo_import_data(lt_ini_repo *r, const char *data, size_t n, const char *source,
                            char **id_out)
{
    size_t tl = 0;
    char *text, *sig = NULL, *name = NULL, *id, *f, *file;
    lt_ini_entry *e;
    char now[21];
    int rc;

    if (data == NULL) return LT_EINVAL;
    text = lt_ini_decode((const lt_u8 *)data, n, &tl);
    if (text == NULL) return LT_EDATA;
    rc = lt_ini_extract_info(text, &sig, &name);
    if (rc != LT_OK) {
        lt_free(text);
        return rc;
    }
    id = make_id(sig);
    if (lt_ini_repo_get(r, id) != NULL) {
        lt_free(text);
        lt_free(sig);
        lt_free(name);
        if (id_out) *id_out = id;
        else lt_free(id);
        return LT_OK;
    }
    file = (char *)lt_malloc(strlen(id) + 5);
    strcpy(file, id);
    strcat(file, ".ini");
    f = lt_path_join(r->dir, file);
    rc = lt_plat_write_atomic(f, text, tl) == 0 ? LT_OK : LT_EIO;
    lt_free(f);
    lt_free(text);
    if (rc != LT_OK) {
        lt_free(file);
        lt_free(sig);
        lt_free(name);
        lt_free(id);
        return rc;
    }
    e = repo_push(r);
    e->id = lt_strdup(id);
    e->name = name;
    e->signature = sig;
    e->path = file;
    lt_proj_utc_now(now);
    e->imported = lt_strdup(now);
    e->source = lt_strdup(source != NULL ? source : "");
    rc = index_save(r);
    if (rc != LT_OK) {
        lt_free(id);
        return rc;
    }
    if (id_out) *id_out = id;
    else lt_free(id);
    return LT_OK;
}

int lt_ini_repo_scan_dir(lt_ini_repo *r, const char *dir, char ***ids, size_t *n)
{
    char **names = NULL;
    int cnt = lt_plat_list_dir(dir, &names), i;
    char **out = NULL;
    size_t no = 0;

    *ids = NULL;
    *n = 0;
    if (cnt < 0) return LT_OK;
    for (i = 0; i < cnt; i++) {
        char *p, *id = NULL;
        if (lt_stricmp(lt_path_ext(names[i]), ".ini") != 0) continue;
        p = lt_path_join(dir, names[i]);
        if (lt_proj_is_file(p) && lt_ini_repo_import(r, p, &id) == LT_OK) {
            out = (char **)lt_realloc(out, (no + 1) * sizeof(char *));
            out[no++] = id;
        }
        lt_free(p);
    }
    lt_plat_free_list(names, cnt);
    *ids = out;
    *n = no;
    return LT_OK;
}
