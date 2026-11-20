/* inidb.c - bundled ECU definitions: catalog, unpacking, signature parsing, resolve.
 * The embedded data is inidb_data.c (generated); the USB drive lookup is inidb_drive.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "inidb.h"
#include "inilib.h"
#include "../lt_lzb.h"

#define MAX_DEPTH 64

size_t lt_inidb_count(void)
{
    return lt_inidb_tab_n;
}

const lt_inidb_def *lt_inidb_at(size_t i)
{
    return i < lt_inidb_tab_n ? &lt_inidb_tab[i] : NULL;
}

long lt_inidb_index(const lt_inidb_def *d)
{
    size_t i;
    for (i = 0; d != NULL && i < lt_inidb_tab_n; i++) {
        if (&lt_inidb_tab[i] == d) { return (long)i; }
    }
    return -1L;
}

const char *lt_inidb_family_title(const char *family)
{
    if (family == NULL) { return ""; }
    if (strcmp(family, "speeduino") == 0) { return "Speeduino"; }
    if (strcmp(family, "rusefi") == 0) { return "rusEFI"; }
    if (strcmp(family, "fome") == 0) { return "FOME"; }
    return family;
}

void lt_inidb_label(const lt_inidb_def *d, char *out, size_t cap)
{
    if (cap == 0) { return; }
    if (d == NULL) { out[0] = '\0'; return; }
    if (d->board[0] != '\0') {
        lt_snprintf(out, cap, "%s %s - %s", lt_inidb_family_title(d->family), d->board, d->version);
    } else {
        lt_snprintf(out, cap, "%s %s", lt_inidb_family_title(d->family), d->version);
    }
}

/* ---- search ---- */
static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static lt_bool ci_find(const char *hay, const char *needle, size_t n)
{
    size_t i, j, hl;
    if (n == 0) { return LT_TRUE; }
    if (hay == NULL) { return LT_FALSE; }
    hl = strlen(hay);
    for (i = 0; i + n <= hl; i++) {
        for (j = 0; j < n; j++) {
            if (lower_ascii((unsigned char)hay[i + j]) != lower_ascii((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == n) { return LT_TRUE; }
    }
    return LT_FALSE;
}

lt_bool lt_inidb_match(const lt_inidb_def *d, const char *query)
{
    const char *q = query;
    if (d == NULL) { return LT_FALSE; }
    if (q == NULL) { return LT_TRUE; }
    for (;;) {
        size_t n;
        while (*q == ' ' || *q == '\t') { q++; }
        if (*q == '\0') { return LT_TRUE; }
        n = 0;
        while (q[n] != '\0' && q[n] != ' ' && q[n] != '\t') { n++; }
        if (!ci_find(lt_inidb_family_title(d->family), q, n) && !ci_find(d->family, q, n) &&
            !ci_find(d->board, q, n) && !ci_find(d->version, q, n) &&
            !ci_find(d->signature, q, n) && !ci_find(d->file, q, n)) {
            return LT_FALSE;
        }
        q += n;
    }
}

size_t lt_inidb_filter(const char *query, size_t *out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; i < lt_inidb_tab_n; i++) {
        if (lt_inidb_match(&lt_inidb_tab[i], query)) {
            if (out != NULL && n < cap) { out[n] = i; }
            n++;
        }
    }
    return n;
}

static lt_bool sig_equal(const char *a, const char *b)
{
    char x[256], y[256];
    if (a == NULL || b == NULL) { return LT_FALSE; }
    if (lt_sig_normalize(a, x, sizeof(x)) == 0) { return LT_FALSE; }
    (void)lt_sig_normalize(b, y, sizeof(y));
    return strcmp(x, y) == 0;
}

size_t lt_inidb_find(const char *sig, size_t *out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; i < lt_inidb_tab_n; i++) {
        if (sig_equal(sig, lt_inidb_tab[i].signature)) {
            if (out != NULL && n < cap) { out[n] = i; }
            n++;
        }
    }
    return n;
}

/* ---- unpacking ---- */
/* the packed bytes of d, gathered from the blocks (malloc'd) */
static unsigned char *gather(const lt_inidb_def *d)
{
    unsigned char *z;
    size_t at = 0, i, want = (size_t)d->zlen, pos = (size_t)d->zoff, got = 0;
    if ((unsigned long)pos != d->zoff || (unsigned long)want != d->zlen) { return NULL; }
    if (want == 0 || pos > lt_inidb_ztotal || want > lt_inidb_ztotal - pos) { return NULL; }
    z = (unsigned char *)lt_malloc(want);
    for (i = 0; i < lt_inidb_zn && got < want; i++) {
        size_t bl = lt_inidb_zlen[i];
        if (pos < at + bl) {
            size_t from = pos > at ? pos - at : 0, k = bl - from;
            if (k > want - got) { k = want - got; }
            memcpy(z + got, lt_inidb_z[i] + from, k);
            got += k;
            pos += k;
        }
        at += bl;
    }
    if (got != want) {
        lt_free(z);
        return NULL;
    }
    return z;
}

/* unpack entry i (and the entries it is packed against); malloc'd, size + 1 bytes */
static unsigned char *unpack(size_t i, int depth)
{
    const lt_inidb_def *d = &lt_inidb_tab[i];
    unsigned char *h0 = NULL, *h1 = NULL, *buf, *z;
    size_t s0 = 0, s1 = 0, hist, total, size = (size_t)d->size;
    int rc;
    if (depth > MAX_DEPTH || (unsigned long)size != d->size) { return NULL; }
    /* history entries always come earlier in the catalog: no cycles */
    if (d->hist0 >= 0) {
        if ((size_t)d->hist0 >= i) { return NULL; }
        h0 = unpack((size_t)d->hist0, depth + 1);
        if (h0 == NULL) { return NULL; }
        s0 = (size_t)lt_inidb_tab[d->hist0].size;
    }
    if (d->hist1 >= 0) {
        if ((size_t)d->hist1 >= i) { lt_free(h0); return NULL; }
        h1 = unpack((size_t)d->hist1, depth + 1);
        if (h1 == NULL) { lt_free(h0); return NULL; }
        s1 = (size_t)lt_inidb_tab[d->hist1].size;
    }
    if (!lt_add_size(s0, s1, &hist) || !lt_add_size(hist, size, &total) ||
        !lt_add_size(total, 1, &total)) {
        lt_free(h0);
        lt_free(h1);
        return NULL;
    }
    buf = (unsigned char *)lt_malloc(total);
    if (s0 > 0) { memcpy(buf, h0, s0); }
    if (s1 > 0) { memcpy(buf + s0, h1, s1); }
    lt_free(h0);
    lt_free(h1);
    z = gather(d);
    rc = z != NULL ? lt_lzb_decode_hist(z, (size_t)d->zlen, buf, hist, size) : -1;
    lt_free(z);
    if (rc != 0) {
        lt_free(buf);
        return NULL;
    }
    if (hist > 0) { memmove(buf, buf + hist, size); }
    buf[size] = 0;
    return buf;
}

char *lt_inidb_text(const lt_inidb_def *d, size_t *len)
{
    long idx = lt_inidb_index(d);
    unsigned char *b;
    if (len != NULL) { *len = 0; }
    if (idx < 0) {
        lt_err_set("No such bundled definition");
        return NULL;
    }
    b = unpack((size_t)idx, 0);
    if (b == NULL || lt_crc32(0, b, (size_t)d->size) != d->crc32) {
        lt_free(b);
        lt_err_set("The bundled definition %s is damaged", d->file);
        return NULL;
    }
    if (len != NULL) { *len = (size_t)d->size; }
    return (char *)b;
}

int lt_inidb_materialise(lt_ini_repo *r, const lt_inidb_def *d, char **id_out)
{
    char *txt, src[600];
    size_t len = 0;
    int rc;
    if (id_out != NULL) { *id_out = NULL; }
    if (r == NULL || d == NULL) {
        lt_err_set("No INI repository or definition given");
        return LT_EINVAL;
    }
    txt = lt_inidb_text(d, &len);
    if (txt == NULL) { return LT_EDATA; }
    lt_snprintf(src, sizeof(src), "bundled: %s %s %s (%s)", lt_inidb_family_title(d->family),
                d->board, d->version, d->source);
    rc = lt_ini_repo_import_data(r, txt, len, src, id_out);
    lt_free(txt);
    return rc;
}

/* ---- signatures ---- */
static lt_bool all_digits(const char *s, size_t n, size_t lo, size_t hi)
{
    size_t i;
    if (n < lo || n > hi) { return LT_FALSE; }
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') { return LT_FALSE; }
    }
    return LT_TRUE;
}

static long digits_val(const char *s, size_t n)
{
    long v = 0;
    size_t i;
    for (i = 0; i < n; i++) { v = v * 10L + (long)(s[i] - '0'); }
    return v;
}

static void copy_n(char *dst, size_t cap, const char *s, size_t n)
{
    if (cap == 0) { return; }
    if (n >= cap) { n = cap - 1; }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* "branch.yyyy.mm.dd.board.hash" after the rusEFI prefix */
static int parse_rusefi_rest(const char *s, lt_sig_info *out)
{
    const char *tok[24];
    size_t len[24], nt = 0, i;
    const char *p = s;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t n = dot != NULL ? (size_t)(dot - p) : strlen(p);
        if (nt >= 24) { return -1; }
        tok[nt] = p;
        len[nt] = n;
        nt++;
        if (dot == NULL) { break; }
        p = dot + 1;
    }
    while (nt > 0 && len[nt - 1] > 0 && (tok[nt - 1][len[nt - 1] - 1] == ' ')) {
        len[nt - 1]--;                  /* trailing blanks after the hash */
    }
    for (i = 0; i + 4 < nt; i++) {
        if (all_digits(tok[i], len[i], 4, 4) && all_digits(tok[i + 1], len[i + 1], 1, 2) &&
            all_digits(tok[i + 2], len[i + 2], 1, 2)) {
            long y = digits_val(tok[i], len[i]), m = digits_val(tok[i + 1], len[i + 1]);
            long d = digits_val(tok[i + 2], len[i + 2]);
            const char *b0 = tok[i + 3];
            const char *b1 = tok[nt - 2] + len[nt - 2];   /* board: up to the hash */
            if (i > 0) {
                copy_n(out->branch, sizeof(out->branch), tok[0], (size_t)(tok[i] - tok[0] - 1));
            }
            copy_n(out->board, sizeof(out->board), b0, (size_t)(b1 - b0));
            out->when = y * 372L + m * 31L + d;
            return out->board[0] != '\0' ? 0 : -1;
        }
    }
    return -1;
}

int lt_sig_parse(const char *sig, lt_sig_info *out)
{
    const char *p = sig;
    memset(out, 0, sizeof(*out));
    if (sig == NULL) { return -1; }
    while (*p == ' ' || *p == '\t') { p++; }
    if (lt_str_istarts(p, "speeduino")) {
        p += 9;
        while (*p == ' ') { p++; }
        if (all_digits(p, 6, 6, 6)) {
            long y = digits_val(p, 4), m = digits_val(p + 4, 2);
            if (m >= 1 && m <= 12) {
                out->family = LT_SIGFAM_SPEEDUINO;
                out->when = y * 12L + m - 1L;
                return 0;
            }
        }
        return -1;
    }
    if (lt_str_istarts(p, "rusefi")) {
        p += 6;
        while (*p == ' ') { p++; }
        if (*p == '(') {
            const char *e = strchr(p, ')');
            if (e == NULL) { return -1; }
            copy_n(out->variant, sizeof(out->variant), p + 1, (size_t)(e - p - 1));
            p = e + 1;
            while (*p == ' ') { p++; }
        }
        if (parse_rusefi_rest(p, out) == 0) {
            out->family = LT_SIGFAM_RUSEFI;
            return 0;
        }
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

long lt_inidb_distance(const char *ecu_sig, const char *ini_sig)
{
    lt_sig_info a, b;
    long d;
    if (ecu_sig == NULL || ini_sig == NULL) { return -1L; }
    if (sig_equal(ecu_sig, ini_sig)) { return 0L; }
    if (lt_sig_parse(ecu_sig, &a) != 0 || lt_sig_parse(ini_sig, &b) != 0) { return -1L; }
    if (a.family != b.family) { return -1L; }
    if (a.family == LT_SIGFAM_RUSEFI &&
        (lt_stricmp(a.variant, b.variant) != 0 || lt_stricmp(a.board, b.board) != 0)) {
        return -1L;
    }
    d = a.when > b.when ? a.when - b.when : b.when - a.when;
    return d > 0 ? d : 1L;
}

/* ---- resolve ---- */
static void pick_reset(lt_inidb_pick *p)
{
    memset(p, 0, sizeof(*p));
    p->bundle = -1L;
    p->distance = -1L;
}

static void pick_bundle(lt_inidb_pick *p, size_t i, int quality, long dist)
{
    const lt_inidb_def *d = &lt_inidb_tab[i];
    char lab[160];
    p->quality = quality;
    p->src = LT_INIDB_SRC_BUNDLE;
    p->bundle = (long)i;
    p->distance = dist;
    lt_strlcpy(p->signature, d->signature, sizeof(p->signature));
    lt_inidb_label(d, lab, sizeof(lab));
    lt_snprintf(p->label, sizeof(p->label), "%s (bundled)", lab);
}

static void pick_repo(lt_inidb_pick *p, const lt_ini_entry *e, int quality, long dist)
{
    p->quality = quality;
    p->src = LT_INIDB_SRC_REPO;
    p->bundle = -1L;
    p->distance = dist;
    lt_strlcpy(p->repo_id, e->id, sizeof(p->repo_id));
    lt_strlcpy(p->signature, e->signature, sizeof(p->signature));
    lt_snprintf(p->label, sizeof(p->label), "%s (INI library)",
                e->name != NULL && e->name[0] != '\0' ? e->name : e->signature);
}

/* is (dist, when) a better near candidate than the current best? ties: newer, then first seen */
static lt_bool closer(long dist, long when, long best_dist, long best_when)
{
    if (best_dist < 0) { return LT_TRUE; }
    if (dist != best_dist) { return dist < best_dist; }
    return when > best_when;
}

static long sig_when(const char *sig)
{
    lt_sig_info si;
    return lt_sig_parse(sig, &si) == 0 ? si.when : 0L;
}

int lt_inidb_resolve(const char *signature, const lt_ini_repo *repo, int flags,
                     lt_inidb_pick *out)
{
    size_t i;
    long best_dist = -1L, best_when = 0L;
    pick_reset(out);
    if (signature == NULL || signature[0] == '\0') { return LT_INIDB_NONE; }
    if ((flags & LT_INIDB_USE_DRIVE) != 0) {
        int comp = 0;
        if (lt_inidb_drive_find(signature, out->path, sizeof(out->path), &comp) == 1) {
            out->quality = LT_INIDB_EXACT;
            out->src = LT_INIDB_SRC_DRIVE;
            out->distance = 0L;
            lt_strlcpy(out->signature, signature, sizeof(out->signature));
            lt_snprintf(out->label, sizeof(out->label), "%s (from the ECU's USB drive)",
                        lt_path_base(out->path));
            return LT_INIDB_EXACT;
        }
        out->path[0] = '\0';
        out->drive_compressed = comp;
    }
    if (repo != NULL) {
        for (i = 0; i < repo->n; i++) {
            if (sig_equal(signature, repo->e[i].signature)) {
                int comp = out->drive_compressed;
                pick_repo(out, &repo->e[i], LT_INIDB_EXACT, 0L);
                out->drive_compressed = comp;
                return LT_INIDB_EXACT;
            }
        }
    }
    for (i = 0; i < lt_inidb_tab_n; i++) {
        if (sig_equal(signature, lt_inidb_tab[i].signature)) {
            int comp = out->drive_compressed;
            pick_bundle(out, i, LT_INIDB_EXACT, 0L);
            out->drive_compressed = comp;
            return LT_INIDB_EXACT;
        }
    }
    /* near: the closest build of the same family/board; the repository wins ties */
    {
        lt_inidb_pick best;
        int comp = out->drive_compressed;
        pick_reset(&best);
        if (repo != NULL) {
            for (i = 0; i < repo->n; i++) {
                long d = lt_inidb_distance(signature, repo->e[i].signature);
                long w = sig_when(repo->e[i].signature);
                if (d > 0 && closer(d, w, best_dist, best_when)) {
                    pick_repo(&best, &repo->e[i], LT_INIDB_NEAR, d);
                    best_dist = d;
                    best_when = w;
                }
            }
        }
        for (i = 0; i < lt_inidb_tab_n; i++) {
            long d = lt_inidb_distance(signature, lt_inidb_tab[i].signature);
            long w = sig_when(lt_inidb_tab[i].signature);
            if (d > 0 && closer(d, w, best_dist, best_when)) {
                pick_bundle(&best, i, LT_INIDB_NEAR, d);
                best_dist = d;
                best_when = w;
            }
        }
        *out = best;
        out->drive_compressed = comp;
    }
    return out->quality;
}

int lt_inidb_pick_path(lt_ini_repo *repo, const lt_inidb_pick *p, char **path_out)
{
    char *id = NULL;
    int rc;
    *path_out = NULL;
    if (p == NULL || p->quality == LT_INIDB_NONE) {
        lt_err_set("No definition picked");
        return LT_ENOENT;
    }
    if (p->src == LT_INIDB_SRC_DRIVE) {
        if (repo == NULL) {
            *path_out = lt_strdup(p->path);
            return LT_OK;
        }
        rc = lt_ini_repo_import(repo, p->path, &id);
    } else if (p->src == LT_INIDB_SRC_BUNDLE) {
        if (repo == NULL) {
            lt_err_set("No INI repository to unpack the definition into");
            return LT_EINVAL;
        }
        rc = lt_inidb_materialise(repo, lt_inidb_at((size_t)p->bundle), &id);
    } else {
        if (repo == NULL) {
            lt_err_set("No INI repository given");
            return LT_EINVAL;
        }
        id = lt_strdup(p->repo_id);
        rc = LT_OK;
    }
    if (rc == LT_OK) {
        *path_out = lt_ini_repo_path(repo, id);
        if (*path_out == NULL) {
            lt_err_set("The INI repository has no entry %s", id);
            rc = LT_ENOENT;
        }
    }
    lt_free(id);
    return rc;
}
