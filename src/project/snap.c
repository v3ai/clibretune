/* snap.c - snapshot-directory versioning (git replacement).
 * Spec: docs/spec/09-project.md section 5.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "proj_priv.h"
#include "../plat/plat.h"

/* ---------------------------------------------------------------- */
/* file sets                                                         */
/* ---------------------------------------------------------------- */
static lt_bool excluded_name(const char *name)
{
    if (name[0] == '.') return LT_TRUE; /* hidden, .cache */
    if (lt_str_ends(name, ".tmp") || lt_str_ends(name, ".bak")) return LT_TRUE;
    return LT_FALSE;
}

/* append relative paths (with '/') of regular files under base/rel */
static void walk(const char *base, const char *rel, char ***v, size_t *n)
{
    char *dir = rel[0] ? lt_path_join(base, rel) : lt_strdup(base);
    char **names = NULL;
    int cnt = lt_plat_list_dir(dir, &names), i;
    for (i = 0; i < cnt; i++) {
        char *r2, *full;
        if (excluded_name(names[i])) continue;
        r2 = rel[0] ? lt_path_join(rel, names[i]) : lt_strdup(names[i]);
        full = lt_path_join(base, r2);
        if (lt_plat_is_dir(full)) {
            walk(base, r2, v, n);
            lt_free(r2);
        } else {
            *v = (char **)lt_realloc(*v, (*n + 1) * sizeof(char *));
            (*v)[(*n)++] = r2;
        }
        lt_free(full);
    }
    if (cnt >= 0) lt_plat_free_list(names, cnt);
    lt_free(dir);
}

/* included set of a directory laid out like a project (or like a snapshot) */
static void collect(const char *base, char ***v, size_t *n)
{
    static const char *const roots[] = {"CurrentTune.msq", "project.json"};
    static const char *const dirs[] = {"projectCfg", "dashboards"};
    size_t i;
    *v = NULL;
    *n = 0;
    for (i = 0; i < 2; i++) {
        char *f = lt_path_join(base, roots[i]);
        if (lt_proj_is_file(f)) {
            *v = (char **)lt_realloc(*v, (*n + 1) * sizeof(char *));
            (*v)[(*n)++] = lt_strdup(roots[i]);
        }
        lt_free(f);
    }
    for (i = 0; i < 2; i++) {
        char *d = lt_path_join(base, dirs[i]);
        if (lt_plat_is_dir(d)) walk(base, dirs[i], v, n);
        lt_free(d);
    }
    if (*n > 1) qsort(*v, *n, sizeof(char *), lt_proj_cmp_str);
}

/* Byte comparison that ignores the values of writeDate="..." attributes: every save
 * stamps the time, so two saves of identical content a second apart must still count
 * as unchanged (no empty history commits). */
int lt_snap_content_equal(const char *da, size_t la, const char *db, size_t lb)
{
    static const char key[] = "writeDate=\"";
    size_t i = 0, j = 0, kl = sizeof(key) - 1;
    while (i < la && j < lb) {
        if (la - i >= kl && lb - j >= kl && memcmp(da + i, key, kl) == 0 &&
            memcmp(db + j, key, kl) == 0) {
            i += kl; j += kl;
            while (i < la && da[i] != '"') i++;
            while (j < lb && db[j] != '"') j++;
            continue;
        }
        if (da[i] != db[j]) return 0;
        i++; j++;
    }
    return i == la && j == lb;
}

static int files_equal(const char *a, const char *b)
{
    size_t la = 0, lb = 0;
    char *da = lt_file_read(a, &la), *db = lt_file_read(b, &lb);
    int eq = (da != NULL && db != NULL && lt_snap_content_equal(da, la, db, lb));
    lt_free(da);
    lt_free(db);
    return eq;
}

/* ---------------------------------------------------------------- */
/* index                                                             */
/* ---------------------------------------------------------------- */
typedef struct snap_rec {
    char id[9];
    unsigned long secs;
    char *author, *message;
} snap_rec;

static char *snap_root(const char *proj)
{
    return lt_path_join(proj, "snapshots");
}

static void esc_field(lt_buf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') lt_buf_puts(b, "\\\\");
        else if (*s == '\t') lt_buf_puts(b, "\\t");
        else if (*s == '\n') lt_buf_puts(b, "\\n");
        else if (*s == '\r') lt_buf_puts(b, "\\r");
        else lt_buf_putc(b, (unsigned char)*s);
    }
}

static char *unesc_field(const char *s, size_t n)
{
    lt_buf b;
    size_t i;
    lt_buf_init(&b);
    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            if (s[i] == 't') lt_buf_putc(&b, '\t');
            else if (s[i] == 'n') lt_buf_putc(&b, '\n');
            else if (s[i] == 'r') lt_buf_putc(&b, '\r');
            else lt_buf_putc(&b, (unsigned char)s[i]);
        } else {
            lt_buf_putc(&b, (unsigned char)s[i]);
        }
    }
    return b.data ? lt_buf_take(&b) : lt_strdup("");
}

static void recs_free(snap_rec *r, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        lt_free(r[i].author);
        lt_free(r[i].message);
    }
    lt_free(r);
}

static lt_bool valid_id(const char *id)
{
    size_t i;
    if (id == NULL || strlen(id) != 8) return LT_FALSE;
    for (i = 0; i < 8; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return LT_FALSE;
    return LT_TRUE;
}

static int index_read(const char *proj, snap_rec **out, size_t *n)
{
    char *root = snap_root(proj), *f = lt_path_join(root, "index.txt");
    char *data;
    size_t len = 0;
    const char *cur;
    snap_rec *v = NULL;
    size_t nv = 0;

    lt_free(root);
    *out = NULL;
    *n = 0;
    if (!lt_file_exists(f)) {
        lt_free(f);
        return LT_OK;
    }
    data = lt_file_read(f, &len);
    lt_free(f);
    if (data == NULL) return LT_EIO;
    cur = data;
    while (*cur != '\0') {
        const char *eol = strchr(cur, '\n');
        size_t ll = eol ? (size_t)(eol - cur) : strlen(cur);
        const char *t1, *t2, *t3;
        char idbuf[9];
        snap_rec *r;
        const char *line = cur;
        cur = eol ? eol + 1 : cur + ll;
        t1 = (const char *)memchr(line, '\t', ll);
        if (t1 == NULL) continue;
        t2 = (const char *)memchr(t1 + 1, '\t', ll - (size_t)(t1 + 1 - line));
        if (t2 == NULL) continue;
        t3 = (const char *)memchr(t2 + 1, '\t', ll - (size_t)(t2 + 1 - line));
        if (t3 == NULL) continue;
        if ((size_t)(t1 - line) != 8) continue;
        memcpy(idbuf, line, 8);
        idbuf[8] = '\0';
        if (!valid_id(idbuf)) continue;
        v = (snap_rec *)lt_realloc(v, (nv + 1) * sizeof *v);
        r = &v[nv++];
        memcpy(r->id, idbuf, 9);
        r->secs = strtoul(t1 + 1, NULL, 10);
        r->author = unesc_field(t2 + 1, (size_t)(t3 - t2 - 1));
        r->message = unesc_field(t3 + 1, ll - (size_t)(t3 + 1 - line));
    }
    lt_free(data);
    *out = v;
    *n = nv;
    return LT_OK;
}

static int index_write(const char *proj, const snap_rec *v, size_t n)
{
    lt_buf b;
    size_t i;
    char *root = snap_root(proj), *f;
    int rc;
    lt_buf_init(&b);
    for (i = 0; i < n; i++) {
        lt_buf_printf(&b, "%s\t%lu\t", v[i].id, v[i].secs);
        esc_field(&b, v[i].author);
        lt_buf_putc(&b, '\t');
        esc_field(&b, v[i].message);
        lt_buf_putc(&b, '\n');
    }
    f = lt_path_join(root, "index.txt");
    lt_free(root);
    rc = lt_proj_write_atomic(f, b.data ? b.data : "", b.len);
    lt_free(f);
    lt_buf_free(&b);
    return rc;
}

/* ---------------------------------------------------------------- */
/* message formatting                                                */
/* ---------------------------------------------------------------- */
char *lt_snap_format_message(const char *msg, const char *note)
{
    char *n, *r;
    if (note == NULL) return lt_strdup(msg);
    n = lt_strdup(note);
    lt_str_trim(n);
    if (n[0] == '\0') {
        lt_free(n);
        return lt_strdup(msg);
    }
    r = (char *)lt_malloc(strlen(msg) + strlen(n) + 16);
    strcpy(r, msg);
    strcat(r, "\n\nLT-Note: ");
    strcat(r, n);
    lt_free(n);
    return r;
}

/* ---------------------------------------------------------------- */
/* branches: named chains of snapshots                               */
/* ---------------------------------------------------------------- */
/* snapshots/branches.txt: "current<TAB>name" then one "name<TAB>tip" line per branch. Without
 * the file there is one branch "main" whose tip is the newest snapshot. Every snapshot records its
 * parent in snapshots/<id>/parent.txt ("-" = none); snapshots without it (older stores) take the
 * previous index entry as parent. */
#define BR_NAME_MAX 64
typedef struct br_ent { char name[BR_NAME_MAX]; char tip[9]; } br_ent;
typedef struct br_set { br_ent *v; size_t n; char cur[BR_NAME_MAX]; lt_bool from_file; } br_set;

static char *br_file(const char *proj)
{
    char *root = snap_root(proj), *f = lt_path_join(root, "branches.txt");
    lt_free(root);
    return f;
}

static void br_free(br_set *b)
{
    lt_free(b->v);
    b->v = NULL;
    b->n = 0;
}

static void br_add(br_set *b, const char *name, const char *tip)
{
    br_ent *e;
    b->v = (br_ent *)lt_realloc(b->v, (b->n + 1) * sizeof *b->v);
    e = &b->v[b->n++];
    memset(e, 0, sizeof *e);
    lt_strlcpy(e->name, name, sizeof e->name);
    lt_strlcpy(e->tip, tip, sizeof e->tip);
}

lt_bool lt_snap_branch_name_ok(const char *name)
{
    size_t i, n;
    if (name == NULL) return LT_FALSE;
    n = strlen(name);
    if (n == 0 || n >= BR_NAME_MAX) return LT_FALSE;
    if (name[0] == '-' || name[0] == '.') return LT_FALSE;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_' || c == '.' || c == '/'))
            return LT_FALSE;
    }
    return LT_TRUE;
}

static int br_load(const char *proj, const snap_rec *recs, size_t nr, br_set *b)
{
    char *f = br_file(proj), *data;
    size_t len = 0;
    const char *cur;
    memset(b, 0, sizeof *b);
    lt_strlcpy(b->cur, "main", sizeof b->cur);
    if (!lt_file_exists(f)) {
        lt_free(f);
        br_add(b, "main", nr > 0 ? recs[nr - 1].id : "");
        return LT_OK;
    }
    data = lt_file_read(f, &len);
    lt_free(f);
    if (data == NULL) return LT_EIO;
    b->from_file = LT_TRUE;
    cur = data;
    while (*cur != '\0') {
        const char *eol = strchr(cur, '\n');
        size_t ll = eol ? (size_t)(eol - cur) : strlen(cur);
        const char *tab = (const char *)memchr(cur, '\t', ll);
        char name[BR_NAME_MAX], val[16];
        if (tab != NULL) {
            size_t nl = (size_t)(tab - cur), vl = ll - nl - 1;
            if (nl > 0 && nl < sizeof name && vl < sizeof val) {
                memcpy(name, cur, nl);
                name[nl] = '\0';
                memcpy(val, tab + 1, vl);
                val[vl] = '\0';
                if (strcmp(name, "current") == 0) lt_strlcpy(b->cur, val, sizeof b->cur);
                else if (lt_snap_branch_name_ok(name) && (val[0] == '\0' || valid_id(val)))
                    br_add(b, name, val);
            }
        }
        cur = eol ? eol + 1 : cur + ll;
    }
    lt_free(data);
    if (b->n == 0) br_add(b, "main", nr > 0 ? recs[nr - 1].id : "");
    return LT_OK;
}

static int br_save(const char *proj, const br_set *b)
{
    lt_buf o;
    size_t i;
    char *f;
    int rc;
    lt_buf_init(&o);
    lt_buf_printf(&o, "current\t%s\n", b->cur);
    for (i = 0; i < b->n; i++) lt_buf_printf(&o, "%s\t%s\n", b->v[i].name, b->v[i].tip);
    f = br_file(proj);
    rc = lt_proj_write_atomic(f, o.data ? o.data : "", o.len);
    lt_free(f);
    lt_buf_free(&o);
    return rc;
}

static br_ent *br_find(br_set *b, const char *name)
{
    size_t i;
    for (i = 0; i < b->n; i++)
        if (strcmp(b->v[i].name, name) == 0) return &b->v[i];
    return NULL;
}

static const char *br_tip(br_set *b)
{
    br_ent *e = br_find(b, b->cur);
    if (e == NULL && b->n > 0) e = &b->v[0];
    return e != NULL ? e->tip : "";
}

static long rec_index(const snap_rec *v, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(v[i].id, id) == 0) return (long)i;
    return -1;
}

/* index of the parent record of recs[i], -1 = none */
static long rec_parent(const char *proj, const snap_rec *recs, size_t nr, size_t i)
{
    char *root = snap_root(proj), *d = lt_path_join(root, recs[i].id);
    char *f = lt_path_join(d, "parent.txt");
    long r;
    lt_free(root);
    lt_free(d);
    if (lt_file_exists(f)) {
        size_t len = 0;
        char *t = lt_file_read(f, &len);
        r = -1;
        if (t != NULL) {
            char id[9];
            memset(id, 0, sizeof id);
            memcpy(id, t, len < 8 ? len : 8);
            if (valid_id(id)) r = rec_index(recs, nr, id);
        }
        lt_free(t);
    } else {
        r = i > 0 ? (long)i - 1 : -1;
    }
    lt_free(f);
    return r;
}

/* record indexes from `tip` back to the root, newest first (caller frees) */
static size_t chain_of(const char *proj, const snap_rec *recs, size_t nr, const char *tip,
                       size_t **out)
{
    size_t *v = (size_t *)lt_calloc(nr + 1, sizeof(size_t)), n = 0;
    long cur = tip[0] != '\0' ? rec_index(recs, nr, tip) : -1;
    while (cur >= 0 && n < nr) {
        v[n++] = (size_t)cur;
        cur = rec_parent(proj, recs, nr, (size_t)cur);
    }
    *out = v;
    return n;
}

lt_bool lt_snap_enabled(const char *proj_dir)
{
    char *root = snap_root(proj_dir), *f = lt_path_join(root, "index.txt"), *m;
    lt_bool on = lt_file_exists(f);
    lt_free(f);
    m = lt_path_join(root, "disabled");
    if (lt_file_exists(m)) on = LT_FALSE;
    lt_free(m);
    lt_free(root);
    return on;
}

lt_bool lt_snap_exists(const char *proj_dir)
{
    char *root = snap_root(proj_dir), *f = lt_path_join(root, "index.txt");
    lt_bool on = lt_file_exists(f);
    lt_free(f);
    lt_free(root);
    return on;
}

int lt_snap_set_enabled(const char *proj_dir, lt_bool on)
{
    char *root = snap_root(proj_dir), *m = lt_path_join(root, "disabled");
    int rc = LT_OK;
    if (on) {
        if (lt_file_exists(m) && lt_plat_remove(m) != 0) rc = LT_EIO;
    } else if (lt_plat_mkdirs(root) != 0 || lt_file_write(m, "off\n", 4) != 0) {
        rc = LT_EIO;
    }
    lt_free(m);
    lt_free(root);
    return rc;
}

int lt_snap_branches(const char *proj_dir, lt_snap_branch **out, size_t *n)
{
    snap_rec *recs;
    size_t nr, i;
    br_set b;
    lt_snap_branch *v;
    int rc = index_read(proj_dir, &recs, &nr);
    *out = NULL;
    *n = 0;
    if (rc != LT_OK) return rc;
    rc = br_load(proj_dir, recs, nr, &b);
    recs_free(recs, nr);
    if (rc != LT_OK) return rc;
    v = (lt_snap_branch *)lt_calloc(b.n + 1, sizeof *v);
    for (i = 0; i < b.n; i++) {
        lt_strlcpy(v[i].name, b.v[i].name, sizeof v[i].name);
        memcpy(v[i].tip, b.v[i].tip, 9);
        v[i].is_current = strcmp(b.v[i].name, b.cur) == 0;
    }
    *n = b.n;
    *out = v;
    br_free(&b);
    return LT_OK;
}

void lt_snap_branch_current(const char *proj_dir, char out[64])
{
    snap_rec *recs = NULL;
    size_t nr = 0;
    br_set b;
    lt_strlcpy(out, "main", 64);
    if (index_read(proj_dir, &recs, &nr) != LT_OK) return;
    if (br_load(proj_dir, recs, nr, &b) == LT_OK) {
        lt_strlcpy(out, b.cur, 64);
        br_free(&b);
    }
    recs_free(recs, nr);
}

int lt_snap_branch_create(const char *proj_dir, const char *name)
{
    snap_rec *recs;
    size_t nr;
    br_set b;
    const char *tip;
    int rc;
    if (!lt_snap_branch_name_ok(name)) {
        lt_err_set("invalid branch name");
        return LT_EINVAL;
    }
    rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) return rc;
    rc = br_load(proj_dir, recs, nr, &b);
    if (rc == LT_OK) {
        tip = br_tip(&b);
        if (br_find(&b, name) != NULL) {
            lt_err_set("branch '%s' already exists", name);
            rc = LT_EEXIST;
        } else if (tip[0] == '\0') {
            lt_err_set("save a version first, then create a branch");
            rc = LT_ENOENT;
        } else {
            char tipcopy[9];
            memcpy(tipcopy, tip, 9);   /* br_add may move the array tip points into */
            br_add(&b, name, tipcopy);
            rc = br_save(proj_dir, &b);
        }
        br_free(&b);
    }
    recs_free(recs, nr);
    return rc;
}

int lt_snap_branch_switch(const char *proj_dir, const char *name, lt_bool force)
{
    snap_rec *recs;
    size_t nr;
    br_set b;
    int rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) return rc;
    rc = br_load(proj_dir, recs, nr, &b);
    recs_free(recs, nr);
    if (rc != LT_OK) return rc;
    {
        br_ent *e = br_find(&b, name != NULL ? name : "");
        if (e == NULL) {
            lt_err_set("no such branch");
            rc = LT_ENOENT;
        } else if (strcmp(e->name, b.cur) == 0) {
            rc = LT_OK;
        } else {
            char tip[9];
            memcpy(tip, e->tip, 9);
            if (!force) {
                int ch = lt_snap_has_changes(proj_dir);
                if (ch > 0) {
                    lt_err_set("working copy has uncommitted changes");
                    rc = LT_EBUSY;
                } else if (ch < 0) {
                    rc = ch;
                }
            }
            if (rc == LT_OK && tip[0] != '\0') rc = lt_snap_restore(proj_dir, tip, LT_TRUE);
            if (rc == LT_OK) {
                lt_strlcpy(b.cur, e->name, sizeof b.cur);
                rc = br_save(proj_dir, &b);
            }
        }
    }
    br_free(&b);
    return rc;
}

/* ---------------------------------------------------------------- */
/* commit / status                                                   */
/* ---------------------------------------------------------------- */
static lt_bool snapshot_exists(const snap_rec *v, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(v[i].id, id) == 0) return LT_TRUE;
    return LT_FALSE;
}

int lt_snap_commit(const char *proj_dir, const char *message, const char *annotation,
                   const char *author, char id_out[9])
{
    char **files, *root, *dir = NULL, id[9];
    size_t nf, i, nr = 0;
    snap_rec *recs = NULL;
    lt_u32 crc = 0;
    unsigned long secs = lt_proj_secs_now();
    unsigned counter = 0;
    int rc;
    lt_bool parent_set = LT_FALSE;
    char cur_branch[BR_NAME_MAX];

    cur_branch[0] = '\0';
    if (message == NULL) message = "";
    if (author == NULL) author = "";
    root = snap_root(proj_dir);
    if (lt_plat_mkdirs(root) != 0) {
        lt_err_set("cannot create '%s'", root);
        lt_free(root);
        return LT_EIO;
    }
    collect(proj_dir, &files, &nf);
    for (i = 0; i < nf; i++) {
        char *f = lt_path_join(proj_dir, files[i]);
        size_t len = 0;
        char *d = lt_file_read(f, &len);
        crc = lt_crc32(crc, files[i], strlen(files[i]) + 1);
        if (d != NULL) crc = lt_crc32(crc, d, len);
        lt_free(d);
        lt_free(f);
    }
    rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) {
        lt_proj_strv_free(files, nf);
        lt_free(root);
        return rc;
    }
    for (;;) {
        unsigned char tb[8];
        lt_u32 h;
        lt_wr_le(tb, 4, (lt_u32)(secs & LT_U32_MASK));
        lt_wr_le(tb + 4, 4, (lt_u32)counter);
        h = lt_crc32(crc, tb, 8);
        h = lt_crc32(h, message, strlen(message));
        lt_snprintf(id, sizeof id, "%08lx", (unsigned long)(h & LT_U32_MASK));
        lt_free(dir);
        dir = lt_path_join(root, id);
        if (!snapshot_exists(recs, nr, id) && !lt_plat_is_dir(dir)) break;
        counter++;
    }
    lt_free(root);
    if (lt_plat_mkdirs(dir) != 0) {
        lt_err_set("cannot create '%s'", dir);
        rc = LT_EIO;
    }
    for (i = 0; rc == LT_OK && i < nf; i++) {
        char *src = lt_path_join(proj_dir, files[i]);
        char *dst = lt_path_join(dir, files[i]);
        char *slash = strrchr(dst, '/');
        if (slash != NULL) {
            *slash = '\0';
            lt_plat_mkdirs(dst);
            *slash = '/';
        }
        rc = lt_proj_copy_file(src, dst);
        lt_free(src);
        lt_free(dst);
    }
    if (rc == LT_OK) {
        br_set bs;
        rc = br_load(proj_dir, recs, nr, &bs);
        if (rc == LT_OK) {
            const char *tip = br_tip(&bs);
            char *pf = lt_path_join(dir, "parent.txt");
            if (lt_file_write(pf, tip[0] != '\0' ? tip : "-", tip[0] != '\0' ? 8 : 1) != 0) rc = LT_EIO;
            lt_free(pf);
            parent_set = bs.from_file;
            if (parent_set) lt_strlcpy(cur_branch, bs.cur, sizeof cur_branch);
            br_free(&bs);
        }
    }
    if (rc == LT_OK && annotation != NULL) {
        char *note = lt_strdup(annotation);
        lt_str_trim(note);
        if (note[0] != '\0') {
            char *nf2 = lt_path_join(dir, "note.txt");
            if (lt_file_write(nf2, note, strlen(note)) != 0) rc = LT_EIO;
            lt_free(nf2);
        }
        lt_free(note);
    }
    if (rc == LT_OK) {
        snap_rec *r;
        recs = (snap_rec *)lt_realloc(recs, (nr + 1) * sizeof *recs);
        r = &recs[nr++];
        memcpy(r->id, id, 9);
        r->secs = secs;
        r->author = lt_strdup(author);
        r->message = lt_strdup(message);
        rc = index_write(proj_dir, recs, nr);
    }
    if (rc == LT_OK && parent_set) {
        br_set bs;
        if (br_load(proj_dir, recs, nr, &bs) == LT_OK) {
            br_ent *e = br_find(&bs, cur_branch);
            if (e != NULL) {
                memcpy(e->tip, id, 9);
                rc = br_save(proj_dir, &bs);
            }
            br_free(&bs);
        }
    }
    if (rc != LT_OK) lt_proj_rmtree(dir);
    lt_free(dir);
    recs_free(recs, nr);
    lt_proj_strv_free(files, nf);
    if (rc == LT_OK && id_out) memcpy(id_out, id, 9);
    return rc;
}

static int has_changes_vs(const char *proj_dir, const char *snap_dir)
{
    char **a, **b;
    size_t na, nb, i;
    int changed = 0;
    collect(proj_dir, &a, &na);
    collect(snap_dir, &b, &nb);
    if (na != nb) changed = 1;
    for (i = 0; !changed && i < na; i++) {
        char *pa, *pb;
        if (strcmp(a[i], b[i]) != 0) {
            changed = 1;
            break;
        }
        pa = lt_path_join(proj_dir, a[i]);
        pb = lt_path_join(snap_dir, b[i]);
        if (!files_equal(pa, pb)) changed = 1;
        lt_free(pa);
        lt_free(pb);
    }
    lt_proj_strv_free(a, na);
    lt_proj_strv_free(b, nb);
    return changed;
}

int lt_snap_has_changes(const char *proj_dir)
{
    snap_rec *recs;
    size_t nr;
    br_set b;
    const char *tip;
    int rc = index_read(proj_dir, &recs, &nr), r;
    if (rc != LT_OK) return rc;
    rc = br_load(proj_dir, recs, nr, &b);
    if (rc != LT_OK) {
        recs_free(recs, nr);
        return rc;
    }
    tip = br_tip(&b);
    if (tip[0] == '\0' || rec_index(recs, nr, tip) < 0) {
        char **f;
        size_t nf;
        collect(proj_dir, &f, &nf);
        lt_proj_strv_free(f, nf);
        br_free(&b);
        recs_free(recs, nr);
        return nf > 0 ? 1 : 0;
    }
    {
        char *root = snap_root(proj_dir), *d = lt_path_join(root, tip);
        r = has_changes_vs(proj_dir, d);
        lt_free(d);
        lt_free(root);
    }
    br_free(&b);
    recs_free(recs, nr);
    return r;
}

/* ---------------------------------------------------------------- */
/* history                                                           */
/* ---------------------------------------------------------------- */
void lt_snap_info_free(lt_snap_info *v, size_t n)
{
    size_t i;
    if (v == NULL) return;
    for (i = 0; i < n; i++) {
        lt_free(v[i].message);
        lt_free(v[i].annotation);
        lt_free(v[i].author);
    }
    lt_free(v);
}

int lt_snap_history(const char *proj_dir, size_t max, lt_snap_info **out, size_t *n)
{
    snap_rec *recs;
    size_t nr, i, cnt, *chain = NULL, nch;
    lt_snap_info *v;
    char *root;
    br_set b;
    int rc = index_read(proj_dir, &recs, &nr);

    *out = NULL;
    *n = 0;
    if (rc != LT_OK) return rc;
    rc = br_load(proj_dir, recs, nr, &b);
    if (rc != LT_OK) {
        recs_free(recs, nr);
        return rc;
    }
    nch = chain_of(proj_dir, recs, nr, br_tip(&b), &chain);
    br_free(&b);
    cnt = (max == 0 || max > nch) ? nch : max;
    if (cnt == 0) {
        lt_free(chain);
        recs_free(recs, nr);
        return LT_OK;
    }
    v = (lt_snap_info *)lt_calloc(cnt, sizeof *v);
    root = snap_root(proj_dir);
    for (i = 0; i < cnt; i++) {
        const snap_rec *r = &recs[chain[i]];
        char *d = lt_path_join(root, r->id), *nf = lt_path_join(d, "note.txt");
        size_t len = 0;
        char *note = lt_file_exists(nf) ? lt_file_read(nf, &len) : NULL;
        lt_snap_info *o = &v[i];
        char ts[21];
        memcpy(o->id, r->id, 9);
        o->message = lt_strdup(r->message);
        o->annotation = note;
        o->author = lt_strdup(r->author);
        lt_proj_utc_from_secs(r->secs, ts);
        lt_snprintf(o->timestamp, sizeof o->timestamp, "%.10s %.8s", ts, ts + 11);
        o->is_head = (i == 0);
        lt_free(d);
        lt_free(nf);
    }
    lt_free(root);
    lt_free(chain);
    recs_free(recs, nr);
    *out = v;
    *n = cnt;
    return LT_OK;
}

/* ---------------------------------------------------------------- */
/* diff                                                              */
/* ---------------------------------------------------------------- */
void lt_snap_diff_free(lt_snap_diff *d)
{
    size_t i;
    if (d == NULL) return;
    for (i = 0; i < d->n_changes; i++) {
        lt_free(d->changes[i].name);
        lt_free(d->changes[i].old_value);
        lt_free(d->changes[i].new_value);
    }
    lt_free(d->changes);
    lt_proj_strv_free(d->files_changed, d->n_files);
    memset(d, 0, sizeof *d);
}

static char *value_text(const lt_tune_value *v)
{
    char tmp[64];
    lt_buf b;
    size_t i;
    switch (v->kind) {
    case LT_TV_SCALAR:
        lt_fmt_f64_shortest(tmp, sizeof tmp, v->scalar);
        return lt_strdup(tmp);
    case LT_TV_BOOL: return lt_strdup(v->boolean ? "true" : "false");
    case LT_TV_STRING: return lt_strdup(v->str ? v->str : "");
    case LT_TV_ARRAY:
        lt_buf_init(&b);
        for (i = 0; i < v->n; i++) {
            if (i) lt_buf_putc(&b, ' ');
            lt_fmt_f64_shortest(tmp, sizeof tmp, v->arr[i]);
            lt_buf_puts(&b, tmp);
        }
        return b.data ? lt_buf_take(&b) : lt_strdup("");
    }
    return lt_strdup("");
}

static void add_file_change(lt_snap_diff *d, const char *path, const char *kind)
{
    size_t l = strlen(path) + strlen(kind) + 4;
    char *s = (char *)lt_malloc(l);
    lt_snprintf(s, l, "%s (%s)", path, kind);
    d->files_changed = (char **)lt_realloc(d->files_changed, (d->n_files + 1) * sizeof(char *));
    d->files_changed[d->n_files++] = s;
}

int lt_snap_diff_compute(const char *proj_dir, const char *from, const char *to,
                         lt_snap_diff *out)
{
    char *root, *da = NULL, *db = NULL;
    char **fa, **fb;
    size_t na, nb, i = 0, j = 0;
    snap_rec *recs;
    size_t nr;
    int rc;

    memset(out, 0, sizeof *out);
    if (!valid_id(from) || (to != NULL && !valid_id(to))) {
        lt_err_set("invalid snapshot id");
        return LT_EINVAL;
    }
    rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) return rc;
    if (!snapshot_exists(recs, nr, from) || (to != NULL && !snapshot_exists(recs, nr, to))) {
        recs_free(recs, nr);
        lt_err_set("snapshot not found");
        return LT_ENOENT;
    }
    recs_free(recs, nr);
    root = snap_root(proj_dir);
    da = lt_path_join(root, from);
    db = to ? lt_path_join(root, to) : lt_strdup(proj_dir);
    lt_free(root);
    collect(da, &fa, &na);
    collect(db, &fb, &nb);
    while (i < na || j < nb) {
        int c;
        if (i >= na) c = 1;
        else if (j >= nb) c = -1;
        else c = strcmp(fa[i], fb[j]);
        if (c < 0) {
            add_file_change(out, fa[i], "removed");
            i++;
        } else if (c > 0) {
            add_file_change(out, fb[j], "added");
            j++;
        } else {
            char *pa = lt_path_join(da, fa[i]), *pb = lt_path_join(db, fb[j]);
            if (!files_equal(pa, pb)) add_file_change(out, fa[i], "modified");
            lt_free(pa);
            lt_free(pb);
            i++;
            j++;
        }
    }
    lt_proj_strv_free(fa, na);
    lt_proj_strv_free(fb, nb);
    /* constant level diff of the tune */
    {
        char *pa = lt_path_join(da, "CurrentTune.msq"), *pb = lt_path_join(db, "CurrentTune.msq");
        if (lt_file_exists(pa) && lt_file_exists(pb)) {
            lt_tune *ta = lt_tune_load(pa), *tb = lt_tune_load(pb);
            if (ta != NULL && tb != NULL) {
                lt_tune_diff *td = lt_tune_diff_compare(ta, tb);
                size_t k;
                if (td != NULL) {
                    out->changes = (lt_snap_change *)lt_calloc(td->n ? td->n : 1,
                                                               sizeof(lt_snap_change));
                    for (k = 0; k < td->n; k++) {
                        const lt_tune_difference *df = &td->diffs[k];
                        lt_snap_change *c = &out->changes[out->n_changes++];
                        c->name = lt_strdup(df->name);
                        c->old_value = df->value_a ? value_text(df->value_a) : NULL;
                        c->new_value = df->value_b ? value_text(df->value_b) : NULL;
                        c->change_type = !df->value_a ? "added"
                                         : !df->value_b ? "removed" : "modified";
                    }
                    lt_tune_diff_free(td);
                }
            }
            if (ta) lt_tune_free(ta);
            if (tb) lt_tune_free(tb);
        }
        lt_free(pa);
        lt_free(pb);
    }
    lt_free(da);
    lt_free(db);
    return LT_OK;
}

/* ---------------------------------------------------------------- */
/* restore / prune                                                   */
/* ---------------------------------------------------------------- */
int lt_snap_restore(const char *proj_dir, const char *id, lt_bool force)
{
    snap_rec *recs;
    size_t nr, i, nf, nw;
    char *root, *sd;
    char **fs, **fw;
    int rc = LT_OK;

    if (!valid_id(id)) {
        lt_err_set("invalid snapshot id");
        return LT_EINVAL;
    }
    rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) return rc;
    if (!snapshot_exists(recs, nr, id)) {
        recs_free(recs, nr);
        lt_err_set("snapshot %s not found", id);
        return LT_ENOENT;
    }
    recs_free(recs, nr);
    if (!force) {
        int ch = lt_snap_has_changes(proj_dir);
        if (ch < 0) return ch;
        if (ch > 0) {
            lt_err_set("working copy has uncommitted changes");
            return LT_EBUSY;
        }
    }
    root = snap_root(proj_dir);
    sd = lt_path_join(root, id);
    lt_free(root);
    collect(sd, &fs, &nf);
    collect(proj_dir, &fw, &nw);
    for (i = 0; rc == LT_OK && i < nf; i++) {
        char *src = lt_path_join(sd, fs[i]), *dst = lt_path_join(proj_dir, fs[i]);
        char *slash = strrchr(dst, '/');
        if (slash != NULL) {
            *slash = '\0';
            lt_plat_mkdirs(dst);
            *slash = '/';
        }
        rc = lt_proj_copy_file(src, dst);
        lt_free(src);
        lt_free(dst);
    }
    for (i = 0; rc == LT_OK && i < nw; i++) {
        size_t k;
        lt_bool found = LT_FALSE;
        for (k = 0; k < nf && !found; k++)
            if (strcmp(fw[i], fs[k]) == 0) found = LT_TRUE;
        if (!found) {
            char *f = lt_path_join(proj_dir, fw[i]);
            lt_plat_remove(f);
            lt_free(f);
        }
    }
    lt_proj_strv_free(fs, nf);
    lt_proj_strv_free(fw, nw);
    lt_free(sd);
    return rc;
}

int lt_snap_prune(const char *proj_dir, size_t keep)
{
    snap_rec *recs;
    size_t nr, drop, i;
    char *root;
    int rc;

    if (keep == 0) {
        lt_err_set("prune must keep at least one snapshot");
        return LT_EINVAL;
    }
    rc = index_read(proj_dir, &recs, &nr);
    if (rc != LT_OK) return rc;
    if (nr <= keep) {
        recs_free(recs, nr);
        return LT_OK;
    }
    drop = nr - keep;
    root = snap_root(proj_dir);
    {
        /* the tip of every branch survives, whatever its age */
        br_set b;
        size_t w = 0;
        if (br_load(proj_dir, recs, nr, &b) != LT_OK) memset(&b, 0, sizeof b);
        for (i = 0; i < nr; i++) {
            lt_bool kill = LT_FALSE;
            size_t k;
            if (i < drop) {
                kill = LT_TRUE;
                for (k = 0; k < b.n; k++)
                    if (strcmp(b.v[k].tip, recs[i].id) == 0) kill = LT_FALSE;
            }
            if (kill) {
                char *d = lt_path_join(root, recs[i].id);
                lt_proj_rmtree(d);
                lt_free(d);
                lt_free(recs[i].author);
                lt_free(recs[i].message);
            } else {
                if (w != i) recs[w] = recs[i];
                w++;
            }
        }
        br_free(&b);
        lt_free(root);
        rc = index_write(proj_dir, recs, w);
        for (i = 0; i < w; i++) {
            lt_free(recs[i].author);
            lt_free(recs[i].message);
        }
    }
    lt_free(recs);
    return rc;
}

/* ---------------------------------------------------------------- */
/* commit message format                                             */
/* ---------------------------------------------------------------- */
char *lt_snap_expand_format(const char *fmt, const char *table, const char *stamp)
{
    lt_buf b;
    char date[16], tm[16];
    size_t i, n;
    lt_buf_init(&b);
    memset(date, 0, sizeof date);
    memset(tm, 0, sizeof tm);
    if (stamp != NULL && strlen(stamp) >= 19) {
        memcpy(date, stamp, 10);
        memcpy(tm, stamp + 11, 8);
    }
    n = strlen(fmt);
    for (i = 0; i < n; i++) {
        if (fmt[i] == '{' && strncmp(fmt + i, "{date}", 6) == 0) {
            lt_buf_puts(&b, date);
            i += 5;
        } else if (fmt[i] == '{' && strncmp(fmt + i, "{time}", 6) == 0) {
            lt_buf_puts(&b, tm);
            i += 5;
        } else if (fmt[i] == '{' && strncmp(fmt + i, "{table}", 7) == 0) {
            lt_buf_puts(&b, table != NULL ? table : "");
            i += 6;
        } else {
            lt_buf_putc(&b, (unsigned char)fmt[i]);
        }
    }
    return b.data != NULL ? lt_buf_take(&b) : lt_strdup("");
}
