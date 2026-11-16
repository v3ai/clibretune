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

static int files_equal(const char *a, const char *b)
{
    size_t la = 0, lb = 0;
    char *da = lt_file_read(a, &la), *db = lt_file_read(b, &lb);
    int eq = (da != NULL && db != NULL && la == lb && (la == 0 || memcmp(da, db, la) == 0));
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
    int rc = index_read(proj_dir, &recs, &nr), r;
    if (rc != LT_OK) return rc;
    if (nr == 0) {
        char **f;
        size_t nf;
        collect(proj_dir, &f, &nf);
        lt_proj_strv_free(f, nf);
        recs_free(recs, nr);
        return nf > 0 ? 1 : 0;
    }
    {
        char *root = snap_root(proj_dir), *d = lt_path_join(root, recs[nr - 1].id);
        r = has_changes_vs(proj_dir, d);
        lt_free(d);
        lt_free(root);
    }
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
    size_t nr, i, cnt;
    lt_snap_info *v;
    char *root;
    int rc = index_read(proj_dir, &recs, &nr);

    *out = NULL;
    *n = 0;
    if (rc != LT_OK) return rc;
    cnt = (max == 0 || max > nr) ? nr : max;
    if (cnt == 0) {
        recs_free(recs, nr);
        return LT_OK;
    }
    v = (lt_snap_info *)lt_calloc(cnt, sizeof *v);
    root = snap_root(proj_dir);
    for (i = 0; i < cnt; i++) {
        const snap_rec *r = &recs[nr - 1 - i];
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
    for (i = 0; i < drop; i++) {
        char *d = lt_path_join(root, recs[i].id);
        lt_proj_rmtree(d);
        lt_free(d);
    }
    lt_free(root);
    for (i = 0; i < drop; i++) {
        lt_free(recs[i].author);
        lt_free(recs[i].message);
    }
    memmove(recs, recs + drop, keep * sizeof *recs);
    rc = index_write(proj_dir, recs, keep);
    for (i = 0; i < keep; i++) {
        lt_free(recs[i].author);
        lt_free(recs[i].message);
    }
    lt_free(recs);
    return rc;
}
