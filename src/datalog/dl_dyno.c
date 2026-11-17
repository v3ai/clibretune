/* dl_dyno.c - dyno runs: CSV import, peaks, table mapping, comparison. Spec 06 section 7.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "datalog.h"

/* Split on ',' or '\t' outside quotes; '"' toggles and is dropped. Fields malloc'd. */
static size_t parse_csv_line(const char *line, char ***out)
{
    size_t n = 0, cap = 8;
    char **v = (char **)lt_malloc(cap * sizeof(char *));
    lt_buf cur;
    int inq = 0;
    const char *p;
    lt_buf_init(&cur);
    for (p = line;; p++) {
        if (*p == '"') {
            inq = !inq;
        } else if (*p == '\0' || ((*p == ',' || *p == '\t') && !inq)) {
            if (n == cap) {
                cap *= 2;
                v = (char **)lt_realloc(v, cap * sizeof(char *));
            }
            v[n++] = lt_strdup(cur.data ? cur.data : "");
            lt_buf_clear(&cur);
            if (*p == '\0') {
                break;
            }
        } else {
            lt_buf_putc(&cur, *p);
        }
    }
    lt_buf_free(&cur);
    *out = v;
    return n;
}

static void dl_strs_free(char **v, size_t n)
{
    lt_strv_free(v, n);
}

void lt_dyno_run_free(lt_dyno_run *r)
{
    lt_free(r->name);
    lt_free(r->source_file);
    lt_free(r->data);
    memset(r, 0, sizeof *r);
}

/* Next line of text (no terminator, trailing \r stripped). NULL at the end. */
static char *next_line(const char **pp, const char *end)
{
    const char *p = *pp, *nl;
    size_t ll;
    char *s;
    if (p >= end) {
        return NULL;
    }
    nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
    *pp = nl ? nl + 1 : end;
    if (ll > 0 && p[ll - 1] == '\r') {
        ll--;
    }
    s = lt_strndup(p, ll);
    return s;
}

int lt_dyno_csv_headers(const char *path, char ***headers, size_t *n)
{
    size_t len = 0;
    char *buf = lt_file_read(path, &len);
    const char *p;
    char *line;
    *headers = NULL;
    *n = 0;
    if (!buf) {
        return lt_file_exists(path) ? LT_EIO : LT_ENOENT;
    }
    p = buf;
    line = next_line(&p, buf + len);
    if (!line) {
        lt_free(buf);
        lt_err_set("Empty file");
        return LT_EDATA;
    }
    *n = parse_csv_line(line, headers);
    lt_free(line);
    lt_free(buf);
    return 0;
}

static int has_any(const char *h, const char *const *pats)
{
    for (; *pats; pats++) {
        if (strstr(h, *pats)) {
            return 1;
        }
    }
    return 0;
}

static int cmp_rpm(const void *a, const void *b)
{
    const lt_dyno_point *x = (const lt_dyno_point *)a, *y = (const lt_dyno_point *)b;
    if (x->rpm < y->rpm) {
        return -1;
    }
    return x->rpm > y->rpm ? 1 : 0;
}

/* stable bottom-up merge sort by rpm (qsort is not stable; insertion sort is O(n^2)) */
static void stable_sort_points(lt_dyno_point *d, size_t n)
{
    lt_dyno_point *a = d, *b, *t;
    size_t w, i, lo, mid, hi, l, r, k;
    if (n < 2) {
        return;
    }
    b = (lt_dyno_point *)lt_malloc(n * sizeof *b);
    for (w = 1; w < n; w *= 2) {
        for (lo = 0; lo < n; lo += 2 * w) {
            mid = lo + w < n ? lo + w : n;
            hi = lo + 2 * w < n ? lo + 2 * w : n;
            l = lo; r = mid; k = lo;
            while (l < mid && r < hi) {
                b[k++] = cmp_rpm(&a[r], &a[l]) < 0 ? a[r++] : a[l++];
            }
            while (l < mid) { b[k++] = a[l++]; }
            while (r < hi) { b[k++] = a[r++]; }
        }
        t = a; a = b; b = t;
    }
    if (a != d) {
        for (i = 0; i < n; i++) {
            d[i] = a[i];
        }
        lt_free(a);
    } else {
        lt_free(b);
    }
}

void lt_dyno_compute_peaks(lt_dyno_run *r)
{
    size_t i;
    r->has_peak_hp = r->has_peak_tq = LT_FALSE;
    r->peak_hp = r->peak_hp_rpm = r->peak_tq = r->peak_tq_rpm = 0.0;
    for (i = 0; i < r->n; i++) {
        const lt_dyno_point *p = &r->data[i];
        if (p->has_hp && (!r->has_peak_hp || p->hp >= r->peak_hp)) {
            r->has_peak_hp = LT_TRUE;
            r->peak_hp = p->hp;
            r->peak_hp_rpm = p->rpm;
        }
        if (p->has_tq && (!r->has_peak_tq || p->tq_val >= r->peak_tq)) {
            r->has_peak_tq = LT_TRUE;
            r->peak_tq = p->tq_val;
            r->peak_tq_rpm = p->rpm;
        }
    }
}

static lt_bool get_cell(char **f, size_t nf, long col, double *out)
{
    char tmp[128];
    if (col < 0 || (size_t)col >= nf) {
        return LT_FALSE;
    }
    lt_strlcpy(tmp, f[col], sizeof tmp);
    return lt_parse_double(lt_str_trim(tmp), out);
}

int lt_dyno_from_csv(const char *path, const char *name, lt_dyno_run *out)
{
    static const char *const p_rpm[] = { "rpm", NULL };
    static const char *const p_hp[] = { "hp", "horsepower", "power", "whp", "bhp", NULL };
    static const char *const p_tq[] = { "torque", "tq", "ft-lb", "ftlb", "nm", NULL };
    static const char *const p_afr[] = { "afr", "lambda", "air fuel", "a/f", NULL };
    static const char *const p_boost[] = { "boost", "manifold", "map", NULL };
    static const char *const p_time[] = { "time", "elapsed", "sec", NULL };
    size_t len = 0, nh, i, cap = 64;
    char *buf = lt_file_read(path, &len);
    const char *p;
    char *line, **hf;
    long c_rpm = -1, c_hp = -1, c_tq = -1, c_afr = -1, c_boost = -1, c_time = -1;

    memset(out, 0, sizeof *out);
    if (!buf) {
        return lt_file_exists(path) ? LT_EIO : LT_ENOENT;
    }
    p = buf;
    line = next_line(&p, buf + len);
    if (!line) {
        lt_free(buf);
        lt_err_set("Empty CSV file");
        return LT_EDATA;
    }
    nh = parse_csv_line(line, &hf);
    lt_free(line);
    for (i = 0; i < nh; i++) {
        char h[256];
        size_t k;
        lt_strlcpy(h, hf[i], sizeof h);
        lt_str_trim(h);
        for (k = 0; h[k]; k++) {
            h[k] = (char)tolower((unsigned char)h[k]);
        }
        if (c_rpm < 0 && (has_any(h, p_rpm) || strcmp(h, "speed") == 0 ||
                          strcmp(h, "engine speed") == 0)) {
            c_rpm = (long)i;
        } else if (c_hp < 0 && has_any(h, p_hp)) {
            c_hp = (long)i;
        } else if (c_tq < 0 && has_any(h, p_tq)) {
            c_tq = (long)i;
        } else if (c_afr < 0 && has_any(h, p_afr)) {
            c_afr = (long)i;
        } else if (c_boost < 0 && has_any(h, p_boost)) {
            c_boost = (long)i;
        } else if (c_time < 0 && has_any(h, p_time)) {
            c_time = (long)i;
        }
    }
    dl_strs_free(hf, nh);
    if (c_rpm < 0) {
        lt_free(buf);
        lt_err_set("Could not find RPM column in CSV headers");
        return LT_EDATA;
    }
    out->data = (lt_dyno_point *)lt_malloc(cap * sizeof(lt_dyno_point));
    while ((line = next_line(&p, buf + len)) != NULL) {
        char **f, *tl = lt_str_trim(line);
        size_t nf;
        double rpm, v;
        lt_dyno_point *pt;
        if (*tl == '\0' || *tl == '#') {
            lt_free(line);
            continue;
        }
        nf = parse_csv_line(tl, &f);
        if (nf <= (size_t)c_rpm || !get_cell(f, nf, c_rpm, &rpm) ||
            !(rpm >= 100.0 && rpm <= 20000.0)) {
            dl_strs_free(f, nf);
            lt_free(line);
            continue;
        }
        if (out->n == cap) {
            cap *= 2;
            out->data = (lt_dyno_point *)lt_realloc(out->data, cap * sizeof(lt_dyno_point));
        }
        pt = &out->data[out->n++];
        memset(pt, 0, sizeof *pt);
        pt->rpm = rpm;
        pt->has_hp = get_cell(f, nf, c_hp, &v); pt->hp = pt->has_hp ? v : 0.0;
        pt->has_tq = get_cell(f, nf, c_tq, &v); pt->tq_val = pt->has_tq ? v : 0.0;
        pt->has_afr = get_cell(f, nf, c_afr, &v); pt->afr = pt->has_afr ? v : 0.0;
        pt->has_boost = get_cell(f, nf, c_boost, &v); pt->boost = pt->has_boost ? v : 0.0;
        pt->has_time = get_cell(f, nf, c_time, &v); pt->time = pt->has_time ? v : 0.0;
        dl_strs_free(f, nf);
        lt_free(line);
    }
    lt_free(buf);
    stable_sort_points(out->data, out->n);
    out->name = lt_strdup(name ? name : "");
    out->source_file = lt_strdup(path);
    lt_strlcpy(out->color, "#4fc3f7", sizeof out->color);
    lt_dyno_compute_peaks(out);
    return 0;
}

size_t lt_dyno_nearest_bin(const double *bins, size_t n, double v)
{
    size_t i, best = 0;
    double bd = 0.0;
    for (i = 0; i < n; i++) {
        double d = fabs(bins[i] - v);
        if (i == 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

size_t lt_dyno_map_to_table(const lt_dyno_run *r, const double *x_bins, size_t nx,
                            const double *y_bins, size_t ny, double boost_to_load,
                            lt_dyno_cell *out, size_t cap)
{
    size_t cells = nx * ny, i, row, col, n_out = 0;
    double *shp, *stq, *safr;
    size_t *nhp, *ntq, *nafr;

    if (cells == 0 || !out) {
        return 0;
    }
    shp = (double *)lt_calloc(cells, sizeof(double));
    stq = (double *)lt_calloc(cells, sizeof(double));
    safr = (double *)lt_calloc(cells, sizeof(double));
    nhp = (size_t *)lt_calloc(cells, sizeof(size_t));
    ntq = (size_t *)lt_calloc(cells, sizeof(size_t));
    nafr = (size_t *)lt_calloc(cells, sizeof(size_t));
    for (i = 0; i < r->n; i++) {
        const lt_dyno_point *pt = &r->data[i];
        size_t k;
        col = lt_dyno_nearest_bin(x_bins, nx, pt->rpm);
        if (pt->has_boost && boost_to_load != 0.0) {
            row = lt_dyno_nearest_bin(y_bins, ny, pt->boost * boost_to_load);
        } else {
            row = ny - 1;
        }
        k = row * nx + col;
        if (pt->has_hp) { shp[k] += pt->hp; nhp[k]++; }
        if (pt->has_tq) { stq[k] += pt->tq_val; ntq[k]++; }
        if (pt->has_afr) { safr[k] += pt->afr; nafr[k]++; }
    }
    for (row = 0; row < ny; row++) {
        for (col = 0; col < nx; col++) {
            size_t k = row * nx + col, cnt = nhp[k];
            lt_dyno_cell *c;
            if (ntq[k] > cnt) { cnt = ntq[k]; }
            if (nafr[k] > cnt) { cnt = nafr[k]; }
            if (cnt == 0 || n_out >= cap) {
                continue;
            }
            c = &out[n_out++];
            memset(c, 0, sizeof *c);
            c->row = row;
            c->col = col;
            c->samples = cnt;
            c->has_hp = nhp[k] > 0;
            c->has_tq = ntq[k] > 0;
            c->has_afr = nafr[k] > 0;
            if (c->has_hp) { c->hp = shp[k] / (double)nhp[k]; }
            if (c->has_tq) { c->tq_val = stq[k] / (double)ntq[k]; }
            if (c->has_afr) { c->afr = safr[k] / (double)nafr[k]; }
        }
    }
    lt_free(shp); lt_free(stq); lt_free(safr);
    lt_free(nhp); lt_free(ntq); lt_free(nafr);
    return n_out;
}

lt_bool lt_dyno_interpolate(const lt_dyno_run *r, double rpm, int which, double *out)
{
    size_t i;
    int have_lo = 0, have_hi = 0;
    double r1 = 0, v1 = 0, r2 = 0, v2 = 0;
    for (i = 0; i < r->n; i++) {
        const lt_dyno_point *p = &r->data[i];
        lt_bool has = which == 0 ? p->has_hp : p->has_tq;
        double v = which == 0 ? p->hp : p->tq_val;
        if (!has) {
            continue;
        }
        if (p->rpm <= rpm) {
            r1 = p->rpm; v1 = v; have_lo = 1;
        }
        if (!have_hi && p->rpm >= rpm) {
            r2 = p->rpm; v2 = v; have_hi = 1;
        }
    }
    if (!have_lo || !have_hi) {
        return LT_FALSE;
    }
    if (fabs(r2 - r1) < 1e-9) {
        *out = v1;
    } else {
        *out = v1 + (rpm - r1) / (r2 - r1) * (v2 - v1);
    }
    return LT_TRUE;
}

int lt_dyno_compare(const lt_dyno_run *a, const lt_dyno_run *b, lt_dyno_cmp *out)
{
    size_t i;
    double sh = 0.0, st = 0.0;
    memset(out, 0, sizeof *out);
    out->hp_diff = (double *)lt_malloc((a->n * 2 + 2) * sizeof(double));
    out->tq_diff = (double *)lt_malloc((a->n * 2 + 2) * sizeof(double));
    for (i = 0; i < a->n; i++) {
        const lt_dyno_point *p = &a->data[i];
        double v;
        if (p->has_hp && lt_dyno_interpolate(b, p->rpm, 0, &v)) {
            out->hp_diff[2 * out->n_hp] = p->rpm;
            out->hp_diff[2 * out->n_hp + 1] = v - p->hp;
            sh += v - p->hp;
            out->n_hp++;
        }
        if (p->has_tq && lt_dyno_interpolate(b, p->rpm, 1, &v)) {
            out->tq_diff[2 * out->n_tq] = p->rpm;
            out->tq_diff[2 * out->n_tq + 1] = v - p->tq_val;
            st += v - p->tq_val;
            out->n_tq++;
        }
    }
    if (out->n_hp > 0) {
        out->has_total_hp = LT_TRUE;
        out->hp_change_sum = sh / (double)out->n_hp;
    }
    if (out->n_tq > 0) {
        out->has_total_tq = LT_TRUE;
        out->tq_change_sum = st / (double)out->n_tq;
    }
    return 0;
}

void lt_dyno_cmp_free(lt_dyno_cmp *c)
{
    lt_free(c->hp_diff);
    lt_free(c->tq_diff);
    memset(c, 0, sizeof *c);
}
