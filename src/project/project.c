/* project.c - project directories, project.json, TunerStudio import.
 * Spec: docs/spec/09-project.md sections 1 and 1.3.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "proj_priv.h"
#include "../plat/plat.h"

/* ---------------------------------------------------------------- */
/* config                                                            */
/* ---------------------------------------------------------------- */
void lt_proj_config_clear(lt_proj_config *c)
{
    lt_free(c->name);
    lt_free(c->created);
    lt_free(c->modified);
    lt_free(c->ecu_definition);
    lt_free(c->signature);
    lt_free(c->dashboard);
    lt_free(c->conn.port);
    memset(c, 0, sizeof *c);
}

static int need_str(const lt_json *o, const char *key, char **dst, const char *path)
{
    const lt_json *v = lt_json_get(o, key);
    if (v == NULL || v->type != LT_JSON_STR) {
        lt_err_set("%s: missing or invalid \"%s\"", path, key);
        return LT_EDATA;
    }
    *dst = lt_strdup(v->str);
    return LT_OK;
}

static int need_num(const lt_json *o, const char *key, double *dst, const char *path)
{
    const lt_json *v = lt_json_get(o, key);
    if (v == NULL || v->type != LT_JSON_NUM || v->num < 0) {
        lt_err_set("%s: missing or invalid \"%s\"", path, key);
        return LT_EDATA;
    }
    *dst = v->num;
    return LT_OK;
}

/* JSON number -> unsigned, saturating (an out-of-range conversion is undefined behaviour) */
static unsigned json_uint(double d)
{
    if (!(d > 0.0)) return 0;
    if (d >= (double)UINT_MAX) return UINT_MAX;
    return (unsigned)d;
}

static int need_bool(const lt_json *o, const char *key, lt_bool *dst, const char *path)
{
    const lt_json *v = lt_json_get(o, key);
    if (v == NULL || v->type != LT_JSON_BOOL) {
        lt_err_set("%s: missing or invalid \"%s\"", path, key);
        return LT_EDATA;
    }
    *dst = v->boolean ? LT_TRUE : LT_FALSE;
    return LT_OK;
}

int lt_proj_config_read(lt_proj_config *c, const char *path)
{
    char err[160];
    lt_json *j;
    const lt_json *conn, *set, *v;
    char *ver = NULL;
    double d = 0;
    int rc;

    memset(c, 0, sizeof *c);
    if (!lt_file_exists(path)) {
        lt_err_set("project.json not found: %s", path);
        return LT_ENOENT;
    }
    j = lt_json_load(path, err, sizeof err);
    if (j == NULL) {
        lt_err_set("%s: invalid JSON (%s)", path, err);
        return LT_EDATA;
    }
    if (j->type != LT_JSON_OBJ) {
        lt_json_free(j);
        lt_err_set("%s: not a JSON object", path);
        return LT_EDATA;
    }
    rc = need_str(j, "version", &ver, path);
    if (rc == LT_OK) {
        lt_strlcpy(c->version, ver, sizeof c->version);
        lt_free(ver);
        rc = need_str(j, "name", &c->name, path);
    }
    if (rc == LT_OK) rc = need_str(j, "created", &c->created, path);
    if (rc == LT_OK) rc = need_str(j, "modified", &c->modified, path);
    if (rc == LT_OK) rc = need_str(j, "ecu_definition", &c->ecu_definition, path);
    if (rc == LT_OK) rc = need_str(j, "signature", &c->signature, path);
    conn = lt_json_get(j, "connection");
    set = lt_json_get(j, "settings");
    if (rc == LT_OK && (conn == NULL || conn->type != LT_JSON_OBJ || set == NULL ||
                        set->type != LT_JSON_OBJ)) {
        lt_err_set("%s: missing \"connection\" or \"settings\"", path);
        rc = LT_EDATA;
    }
    if (rc == LT_OK) {
        v = lt_json_get(conn, "port");
        if (v != NULL && v->type == LT_JSON_STR) c->conn.port = lt_strdup(v->str);
        else if (v != NULL && v->type != LT_JSON_NULL) {
            lt_err_set("%s: invalid \"port\"", path);
            rc = LT_EDATA;
        }
    }
    if (rc == LT_OK) rc = need_num(conn, "baud_rate", &d, path), c->conn.baud_rate = json_uint(d);
    if (rc == LT_OK) rc = need_num(conn, "timeout_ms", &d, path), c->conn.timeout_ms = json_uint(d);
    if (rc == LT_OK) rc = need_bool(set, "auto_load_tune", &c->settings.auto_load_tune, path);
    if (rc == LT_OK) rc = need_bool(set, "auto_save_tune", &c->settings.auto_save_tune, path);
    if (rc == LT_OK) rc = need_bool(set, "auto_connect", &c->settings.auto_connect, path);
    if (rc == LT_OK) {
        c->settings.max_restore_points = json_uint(lt_json_get_num(set, "max_restore_points", 20));
        v = lt_json_get(j, "dashboard");
        if (v != NULL && v->type == LT_JSON_STR) c->dashboard = lt_strdup(v->str);
    }
    lt_json_free(j);
    if (rc != LT_OK) lt_proj_config_clear(c);
    return rc;
}

int lt_proj_config_write(const lt_proj_config *c, const char *path)
{
    lt_json *o = lt_json_obj(), *conn = lt_json_obj(), *set = lt_json_obj();
    char *s;
    int rc;

    lt_json_obj_set(o, "version", lt_json_str(c->version));
    lt_json_obj_set(o, "name", lt_json_str(c->name ? c->name : ""));
    lt_json_obj_set(o, "created", lt_json_str(c->created ? c->created : ""));
    lt_json_obj_set(o, "modified", lt_json_str(c->modified ? c->modified : ""));
    lt_json_obj_set(o, "ecu_definition", lt_json_str(c->ecu_definition ? c->ecu_definition : ""));
    lt_json_obj_set(o, "signature", lt_json_str(c->signature ? c->signature : ""));
    lt_json_obj_set(conn, "port", c->conn.port ? lt_json_str(c->conn.port) : lt_json_null());
    lt_json_obj_set(conn, "baud_rate", lt_json_num((double)c->conn.baud_rate));
    lt_json_obj_set(conn, "timeout_ms", lt_json_num((double)c->conn.timeout_ms));
    lt_json_obj_set(o, "connection", conn);
    lt_json_obj_set(set, "auto_load_tune", lt_json_bool(c->settings.auto_load_tune));
    lt_json_obj_set(set, "auto_save_tune", lt_json_bool(c->settings.auto_save_tune));
    lt_json_obj_set(set, "auto_connect", lt_json_bool(c->settings.auto_connect));
    lt_json_obj_set(set, "max_restore_points", lt_json_num((double)c->settings.max_restore_points));
    lt_json_obj_set(o, "settings", set);
    lt_json_obj_set(o, "dashboard", c->dashboard ? lt_json_str(c->dashboard) : lt_json_null());
    s = lt_json_to_string(o, 1);
    lt_json_free(o);
    rc = lt_proj_write_atomic(path, s, strlen(s));
    lt_free(s);
    return rc;
}

/* ---------------------------------------------------------------- */
/* paths                                                             */
/* ---------------------------------------------------------------- */
char *lt_projects_dir(void)
{
    char *env = lt_plat_getenv("LIBRETUNE_PROJECTS");
    char *home, *docs, *r;
    if (env != NULL && env[0] != '\0') return env;
    lt_free(env);
    home = lt_plat_getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        lt_free(home);
        home = lt_plat_getenv("USERPROFILE");
    }
    if (home == NULL || home[0] == '\0') {
        lt_free(home);
        lt_err_set("no home directory");
        return NULL;
    }
    docs = lt_path_join(home, "Documents");
    if (lt_plat_is_dir(docs)) {
        r = lt_path_join(docs, "LibreTuneProjects");
    } else {
        r = lt_path_join(home, "LibreTuneProjects");
    }
    lt_free(docs);
    lt_free(home);
    return r;
}

char *lt_project_ini_path(const lt_project *p)
{
    return lt_proj_join3(p->path, "projectCfg", "definition.ini");
}

char *lt_project_file(const lt_project *p, const char *rel)
{
    return lt_path_join(p->path, rel);
}

/* ---------------------------------------------------------------- */
/* lifecycle                                                         */
/* ---------------------------------------------------------------- */
void lt_project_free(lt_project *p)
{
    if (p == NULL) return;
    lt_free(p->path);
    lt_proj_config_clear(&p->cfg);
    if (p->tune != NULL) lt_tune_free(p->tune);
    memset(p, 0, sizeof *p);
}

static int save_config_to(const lt_project *p)
{
    char *f = lt_path_join(p->path, "project.json");
    int rc = lt_proj_config_write(&p->cfg, f);
    lt_free(f);
    return rc;
}

int lt_project_create(const char *name, const char *ini_src, const char *signature,
                      const char *parent_dir, lt_project *out)
{
    char *parent, *safe, *path, *sub, *dst, now[21];
    int rc;
    lt_project p;
    size_t i;

    if (name == NULL || name[0] == '\0' || out == NULL) {
        lt_err_set("project name is empty");
        return LT_EINVAL;
    }
    if (ini_src == NULL || !lt_file_exists(ini_src)) {
        lt_err_set("INI file not found: %s", ini_src ? ini_src : "(null)");
        return LT_ENOENT;
    }
    parent = parent_dir ? lt_strdup(parent_dir) : lt_projects_dir();
    if (parent == NULL) return LT_ENOENT;
    safe = lt_proj_safe_name(name, LT_TRUE);
    /* a name made only of dots would escape or hide the directory */
    for (i = 0; safe[i] == '.'; i++) {
    }
    if (safe[i] == '\0' && i > 0) memset(safe, '_', i);
    path = lt_path_join(parent, safe);
    lt_free(safe);
    lt_free(parent);
    if (lt_plat_is_dir(path) || lt_file_exists(path)) {
        lt_err_set("Project '%s' already exists", name);
        lt_free(path);
        return LT_EEXIST;
    }
    if (lt_plat_mkdirs(path) != 0) {
        lt_err_set("cannot create '%s'", path);
        lt_free(path);
        return LT_EIO;
    }
    sub = lt_path_join(path, "projectCfg");
    rc = lt_plat_mkdirs(sub);
    lt_free(sub);
    sub = lt_path_join(path, "datalogs");
    if (rc == 0) rc = lt_plat_mkdirs(sub);
    lt_free(sub);
    sub = lt_path_join(path, "dashboards");
    if (rc == 0) rc = lt_plat_mkdirs(sub);
    lt_free(sub);
    if (rc != 0) {
        lt_err_set("cannot create project subdirectories in '%s'", path);
        lt_free(path);
        return LT_EIO;
    }
    dst = lt_proj_join3(path, "projectCfg", "definition.ini");
    rc = lt_proj_copy_file(ini_src, dst);
    lt_free(dst);
    if (rc != LT_OK) {
        lt_free(path);
        return rc;
    }
    memset(&p, 0, sizeof p);
    p.path = path;
    lt_strlcpy(p.cfg.version, "1.0", sizeof p.cfg.version);
    p.cfg.name = lt_strdup(name);
    lt_proj_utc_now(now);
    p.cfg.created = lt_strdup(now);
    p.cfg.modified = lt_strdup(now);
    p.cfg.ecu_definition = lt_strdup("projectCfg/definition.ini");
    p.cfg.signature = lt_strdup(signature ? signature : "");
    p.cfg.conn.baud_rate = 115200;
    p.cfg.conn.timeout_ms = 1000;
    p.cfg.settings.auto_load_tune = LT_TRUE;
    p.cfg.settings.auto_save_tune = LT_TRUE;
    p.cfg.settings.auto_connect = LT_FALSE;
    p.cfg.settings.max_restore_points = 20;
    rc = save_config_to(&p);
    if (rc != LT_OK) {
        lt_project_free(&p);
        return rc;
    }
    *out = p;
    return LT_OK;
}

int lt_project_load_tune(lt_project *p)
{
    char *f = lt_path_join(p->path, "CurrentTune.msq");
    char *pc;
    lt_tune *t;

    if (!lt_file_exists(f)) {
        lt_free(f);
        return LT_OK;
    }
    t = lt_tune_load(f);               /* refuses a cut-off file */
    lt_free(f);
    if (t == NULL) {
        if (lt_err_get()[0] == '\0') lt_err_set("cannot load CurrentTune.msq");
        return LT_EDATA;
    }
    pc = lt_proj_join3(p->path, "projectCfg", "pcVariableValues.msq");
    if (lt_file_exists(pc)) (void)lt_tune_load_pc_variables(t, pc);
    lt_free(pc);
    if (p->tune != NULL) lt_tune_free(p->tune);
    p->tune = t;
    p->dirty = LT_FALSE;
    return LT_OK;
}

int lt_project_open(const char *path, lt_project *out)
{
    lt_project p;
    char *f;
    int rc;

    memset(&p, 0, sizeof p);
    f = lt_path_join(path, "project.json");
    rc = lt_proj_config_read(&p.cfg, f);
    lt_free(f);
    if (rc != LT_OK) return rc;
    p.path = lt_strdup(path);
    if (p.cfg.settings.auto_load_tune) (void)lt_project_load_tune(&p);
    *out = p;
    return LT_OK;
}

int lt_project_save_config(lt_project *p)
{
    char now[21];
    lt_proj_utc_now(now);
    lt_proj_set_str(&p->cfg.modified, now);
    return save_config_to(p);
}

int lt_project_save_tune(lt_project *p)
{
    char *f;
    int rc;
    if (p->tune == NULL) return LT_OK;
    f = lt_path_join(p->path, "CurrentTune.msq");
    rc = lt_tune_save(p->tune, f);     /* atomic: the previous file survives a failure */
    lt_free(f);
    if (rc != 0) return LT_EIO;
    if (p->tune->pc_variables.len >= 1) {
        char *pc = lt_proj_join3(p->path, "projectCfg", "pcVariableValues.msq");
        rc = lt_tune_save_pc_variables(p->tune, pc, p->cfg.signature);
        lt_free(pc);
        if (rc != 0) return LT_EIO;      /* still dirty: the next save retries */
    }
    p->dirty = LT_FALSE;
    return LT_OK;
}

int lt_project_close(lt_project *p)
{
    int rc = LT_OK;
    if (p->cfg.settings.auto_save_tune && p->tune != NULL) rc = lt_project_save_tune(p);
    lt_project_free(p);
    return rc;
}

lt_ini *lt_project_load_ini(const lt_project *p, const lt_ini_options *opts)
{
    char *f = lt_project_ini_path(p);
    lt_ini *ini;
    if (!lt_file_exists(f)) {
        lt_err_set("project INI not found: %s", f);
        lt_free(f);
        return NULL;
    }
    ini = lt_ini_load_file(f, opts);
    lt_free(f);
    return ini;
}

/* ---------------------------------------------------------------- */
/* listing                                                           */
/* ---------------------------------------------------------------- */
void lt_project_info_free(lt_project_info *v, size_t n)
{
    size_t i;
    if (v == NULL) return;
    for (i = 0; i < n; i++) {
        lt_free(v[i].name);
        lt_free(v[i].path);
        lt_free(v[i].signature);
        lt_free(v[i].modified);
    }
    lt_free(v);
}

static int cmp_info(const void *a, const void *b)
{
    const lt_project_info *x = (const lt_project_info *)a, *y = (const lt_project_info *)b;
    int c = strcmp(y->modified, x->modified);
    if (c != 0) return c;
    return strcmp(x->name, y->name);
}

int lt_project_list(lt_project_info **out, size_t *n, const char *parent_dir)
{
    char *parent = parent_dir ? lt_strdup(parent_dir) : lt_projects_dir();
    char **names = NULL;
    int cnt, i;
    lt_project_info *v = NULL;
    size_t nv = 0;

    *out = NULL;
    *n = 0;
    if (parent == NULL) return LT_ENOENT;
    cnt = lt_plat_list_dir(parent, &names);
    if (cnt < 0) {
        lt_free(parent);
        return LT_OK; /* missing dir: empty */
    }
    for (i = 0; i < cnt; i++) {
        char *dir = lt_path_join(parent, names[i]);
        char *f = lt_path_join(dir, "project.json");
        lt_proj_config c;
        if (lt_plat_is_dir(dir) && lt_file_exists(f) && lt_proj_config_read(&c, f) == LT_OK) {
            v = (lt_project_info *)lt_realloc(v, (nv + 1) * sizeof *v);
            v[nv].name = lt_strdup(c.name);
            v[nv].path = dir;
            dir = NULL;
            v[nv].signature = lt_strdup(c.signature);
            v[nv].modified = lt_strdup(c.modified);
            nv++;
            lt_proj_config_clear(&c);
        }
        lt_free(dir);
        lt_free(f);
    }
    lt_plat_free_list(names, cnt);
    lt_free(parent);
    if (nv > 1) qsort(v, nv, sizeof *v, cmp_info);
    *out = v;
    *n = nv;
    return LT_OK;
}

/* ---------------------------------------------------------------- */
/* TunerStudio import                                                */
/* ---------------------------------------------------------------- */
static char *ts_extract_signature(const char *text)
{
    const char *cur = text;
    while (*cur != '\0') {
        const char *eol = strchr(cur, '\n');
        size_t len = eol ? (size_t)(eol - cur) : strlen(cur);
        char *line = lt_strndup(cur, len);
        char *t = lt_str_trim(line);
        cur = eol ? eol + 1 : cur + len;
        if (lt_str_istarts(t, "signature")) {
            char *eq = strchr(t, '=');
            if (eq != NULL) {
                char *key, *val, *res = NULL;
                size_t kl = (size_t)(eq - t);
                key = lt_strndup(t, kl);
                lt_str_trim(key);
                val = lt_strdup(eq + 1);
                lt_str_trim(val);
                if (lt_stricmp(key, "signature") == 0) {
                    char *q1 = strchr(val, '"');
                    char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                    if (q1 != NULL && q2 != NULL) {
                        res = lt_strndup(q1 + 1, (size_t)(q2 - q1 - 1));
                    } else {
                        char *semi = strchr(val, ';');
                        size_t l;
                        if (semi) *semi = '\0';
                        lt_str_trim(val);
                        l = strlen(val);
                        while (l > 0 && (val[l - 1] == '"' || val[l - 1] == '\'')) val[--l] = '\0';
                        {
                            char *st = val;
                            while (*st == '"' || *st == '\'') st++;
                            res = lt_strdup(st);
                        }
                    }
                    lt_str_trim(res);
                }
                lt_free(key);
                lt_free(val);
                if (res != NULL && res[0] != '\0') {
                    lt_free(line);
                    return res;
                }
                lt_free(res);
            }
        }
        lt_free(line);
    }
    return lt_strdup("");
}

static char *dir_basename(const char *path)
{
    size_t n = strlen(path);
    const char *b;
    char *tmp, *r;
    tmp = lt_strdup(path);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';
    b = tmp;
    {
        const char *p;
        for (p = tmp; *p; p++)
            if (*p == '/' || *p == '\\') b = p + 1;
    }
    r = lt_strdup(b);
    lt_free(tmp);
    return r;
}

int lt_project_import_ts(const char *ts_dir, const char *target_parent, lt_project *out)
{
    char *props_path = lt_proj_join3(ts_dir, "projectCfg", "project.properties");
    char *name = NULL, *ini_path, *ini_raw, *ini_text, *sig, *f, *g;
    const char *v;
    lt_props pr;
    lt_project p;
    size_t n = 0;
    int rc, iv;

    if (!lt_file_exists(props_path)) {
        lt_err_set("Not a valid TS project: project.properties not found");
        lt_free(props_path);
        return LT_ENOENT;
    }
    lt_props_init(&pr);
    rc = lt_props_load(&pr, props_path);
    lt_free(props_path);
    if (rc != LT_OK) return rc;
    v = lt_props_get(&pr, "projectName");
    if (v != NULL && v[0] != '\0') name = lt_strdup(v);
    else {
        name = dir_basename(ts_dir);
        if (name[0] == '\0') {
            lt_free(name);
            name = lt_strdup("Imported Project");
        }
    }
    v = lt_props_get(&pr, "ecuConfigFile");
    /* a plain file name inside projectCfg/: a path in an imported project must not make us
     * read (and copy into the new project) a file from elsewhere */
    if (v != NULL) v = lt_path_base(v);
    if (v != NULL && (strcmp(v, ".") == 0 || strcmp(v, "..") == 0)) v = NULL;
    ini_path = lt_proj_join3(ts_dir, "projectCfg", v && v[0] ? v : "mainController.ini");
    ini_raw = lt_file_read(ini_path, &n);
    if (ini_raw == NULL) {
        lt_err_set("ECU definition not found: %s", ini_path);
        lt_free(ini_path);
        lt_free(name);
        lt_props_free(&pr);
        return LT_ENOENT;
    }
    ini_text = lt_proj_decode_latin1((const unsigned char *)ini_raw, n);
    lt_free(ini_raw);
    sig = ts_extract_signature(ini_text);
    lt_free(ini_text);

    rc = lt_project_create(name, ini_path, sig, target_parent, &p);
    lt_free(ini_path);
    lt_free(name);
    lt_free(sig);
    if (rc != LT_OK) {
        lt_props_free(&pr);
        return rc;
    }
    v = lt_props_get(&pr, "commPort");
    if (v != NULL) lt_proj_set_str(&p.cfg.conn.port, v);
    if (lt_props_get_i32(&pr, "baudRate", &iv) && iv >= 0) p.cfg.conn.baud_rate = (unsigned)iv;
    lt_props_free(&pr);

    f = lt_path_join(ts_dir, "CurrentTune.msq");
    if (lt_file_exists(f)) {
        g = lt_path_join(p.path, "CurrentTune.msq");
        rc = lt_proj_copy_file(f, g);
        lt_free(g);
        if (rc == LT_OK) rc = lt_project_load_tune(&p);
        if (rc != LT_OK) {
            lt_free(f);
            lt_project_free(&p);
            return rc;
        }
    }
    lt_free(f);
    f = lt_proj_join3(ts_dir, "projectCfg", "pcVariableValues.msq");
    if (lt_file_exists(f)) {
        g = lt_proj_join3(p.path, "projectCfg", "pcVariableValues.msq");
        (void)lt_proj_copy_file(f, g);
        lt_free(g);
        if (p.tune != NULL) {
            g = lt_proj_join3(p.path, "projectCfg", "pcVariableValues.msq");
            (void)lt_tune_load_pc_variables(p.tune, g);
            lt_free(g);
        }
    }
    lt_free(f);
    f = lt_path_join(ts_dir, "restorePoints");
    if (lt_plat_is_dir(f)) {
        char **names = NULL;
        int cnt = lt_plat_list_dir(f, &names), i;
        g = lt_path_join(p.path, "restorePoints");
        lt_plat_mkdirs(g);
        for (i = 0; i < cnt; i++) {
            char *src, *dst;
            if (strcmp(lt_path_ext(names[i]), ".msq") != 0) continue;
            src = lt_path_join(f, names[i]);
            if (lt_proj_is_file(src)) {
                dst = lt_path_join(g, names[i]);
                (void)lt_proj_copy_file(src, dst);
                lt_free(dst);
            }
            lt_free(src);
        }
        if (cnt >= 0) lt_plat_free_list(names, cnt);
        lt_free(g);
    }
    lt_free(f);
    rc = lt_project_save_config(&p);
    if (rc != LT_OK) {
        lt_project_free(&p);
        return rc;
    }
    *out = p;
    return LT_OK;
}
