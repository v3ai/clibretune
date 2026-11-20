/* dl_view.c - graph setup model + JSON, 2-D histogram / table generator, X-Y point ring.
 * See dl_view.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dl_view.h"
#include "../json/json.h"

/* ====================================================================== */
/* graph setup                                                             */
/* ====================================================================== */

void lt_graph_init(lt_graph_setup *g)
{
    g->pages = NULL;
    g->n_pages = 0;
}

void lt_graph_free(lt_graph_setup *g)
{
    lt_free(g->pages);
    lt_graph_init(g);
}

void lt_graph_copy(lt_graph_setup *dst, const lt_graph_setup *src)
{
    lt_graph_free(dst);
    if (src->n_pages > 0) {
        dst->pages = (lt_graph_page *)lt_malloc((size_t)src->n_pages * sizeof(lt_graph_page));
        memcpy(dst->pages, src->pages, (size_t)src->n_pages * sizeof(lt_graph_page));
        dst->n_pages = src->n_pages;
    }
}

int lt_graph_add_page(lt_graph_setup *g, const char *name)
{
    lt_graph_page *p;
    if (g->n_pages >= LT_GRAPH_MAX_PAGES) {
        return -1;
    }
    g->pages = (lt_graph_page *)lt_realloc(g->pages, (size_t)(g->n_pages + 1) * sizeof(lt_graph_page));
    p = &g->pages[g->n_pages];
    memset(p, 0, sizeof *p);
    if (name != NULL && name[0]) {
        lt_strlcpy(p->name, name, sizeof p->name);
    } else {
        lt_snprintf(p->name, sizeof p->name, "Page %d", g->n_pages + 1);
    }
    return g->n_pages++;
}

void lt_graph_remove_page(lt_graph_setup *g, int page)
{
    if (page < 0 || page >= g->n_pages) {
        return;
    }
    memmove(&g->pages[page], &g->pages[page + 1],
            (size_t)(g->n_pages - page - 1) * sizeof(lt_graph_page));
    g->n_pages--;
}

int lt_graph_add_pane(lt_graph_page *p)
{
    if (p->n_panes >= LT_GRAPH_MAX_PANES) {
        return -1;
    }
    memset(&p->pane[p->n_panes], 0, sizeof(lt_graph_pane));
    return p->n_panes++;
}

void lt_graph_remove_pane(lt_graph_page *p, int pane)
{
    if (pane < 0 || pane >= p->n_panes) {
        return;
    }
    memmove(&p->pane[pane], &p->pane[pane + 1], (size_t)(p->n_panes - pane - 1) * sizeof(lt_graph_pane));
    p->n_panes--;
}

int lt_graph_pane_find(const lt_graph_pane *p, const char *channel)
{
    int i;
    for (i = 0; i < p->n; i++) {
        if (strcmp(p->tr[i].channel, channel) == 0) {
            return i;
        }
    }
    return -1;
}

int lt_graph_pane_add(lt_graph_pane *p, const char *channel)
{
    int k = lt_graph_pane_find(p, channel);
    lt_graph_trace *t;
    if (k >= 0) {
        return k;
    }
    if (p->n >= LT_GRAPH_MAX_TRACES || channel == NULL || channel[0] == '\0') {
        return -1;
    }
    t = &p->tr[p->n];
    memset(t, 0, sizeof *t);
    lt_strlcpy(t->channel, channel, sizeof t->channel);
    t->autoscale = 1;
    t->lo = 0.0;
    t->hi = 100.0;
    return p->n++;
}

void lt_graph_pane_remove(lt_graph_pane *p, int trace)
{
    if (trace < 0 || trace >= p->n) {
        return;
    }
    memmove(&p->tr[trace], &p->tr[trace + 1], (size_t)(p->n - trace - 1) * sizeof(lt_graph_trace));
    p->n--;
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (lt_strnicmp(hay, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* how well a channel name matches a word: 0 equal, 1 starts with it, 2 contains it, 3 contains it
 * but is a raw sensor value ("rawMap"), -1 no match (all case-insensitive) */
int lt_graph_name_rank(const char *name, const char *word)
{
    size_t n = strlen(word);
    if (lt_stricmp(name, word) == 0 || (lt_strnicmp(name, word, n) == 0 && lt_stricmp(name + n, "value") == 0)) {
        return 0;
    }
    if (lt_strnicmp(name, word, n) == 0) {
        return 1;
    }
    if (!ci_contains(name, word)) {
        return -1;
    }
    return lt_strnicmp(name, "raw", 3) == 0 ? 3 : 2;
}

/* best channel for the first word that matches any (rank, then the shorter name), -1 if none */
static int pick(const char *const *names, int n, const char *const *words, int nw, const int *used)
{
    int w, i, best = -1, best_rank = 99;
    for (w = 0; w < nw && best < 0; w++) {
        for (i = 0; i < n; i++) {
            int rk;
            if (used[i]) {
                continue;
            }
            rk = lt_graph_name_rank(names[i], words[w]);
            if (rk < 0) {
                continue;
            }
            if (best < 0 || rk < best_rank || (rk == best_rank && strlen(names[i]) < strlen(names[best]))) {
                best = i;
                best_rank = rk;
            }
        }
    }
    return best;
}

static int add_pick(lt_graph_pane *pane, const char *const *names, int n, const char *const *words,
                    int nw, int *used)
{
    int k = pick(names, n, words, nw, used);
    if (k < 0) {
        return 0;
    }
    used[k] = 1;
    (void)lt_graph_pane_add(pane, names[k]);
    return 1;
}

void lt_graph_defaults(lt_graph_setup *g, const char *const *names, int n)
{
    static const char *const w_rpm[] = { "rpm" };
    static const char *const w_load[] = { "map", "tps", "load" };
    static const char *const w_load2[] = { "tps", "throttle" };
    static const char *const w_afr[] = { "afr", "lambda", "ego" };
    static const char *const w_temp[] = { "coolant", "clt" };
    static const char *const w_temp2[] = { "intake", "iat", "mat" };
    int *used, pg, k, added = 0;
    lt_graph_page *p;
    lt_graph_free(g);
    pg = lt_graph_add_page(g, "Main");
    p = &g->pages[pg];
    used = (int *)lt_calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    for (k = 0; k < 4; k++) {
        (void)lt_graph_add_pane(p);
    }
    added += add_pick(&p->pane[0], names, n, w_rpm, 1, used);
    added += add_pick(&p->pane[1], names, n, w_load, 3, used);
    added += add_pick(&p->pane[1], names, n, w_load2, 2, used);
    added += add_pick(&p->pane[2], names, n, w_afr, 3, used);
    added += add_pick(&p->pane[3], names, n, w_temp, 2, used);
    added += add_pick(&p->pane[3], names, n, w_temp2, 3, used);
    if (p->pane[1].n > 1) {
        p->pane[1].tr[1].right = 1;
    }
    if (p->pane[3].n > 1) {
        p->pane[3].tr[1].right = 1;
    }
    /* drop empty panes, keep at least one */
    for (k = p->n_panes - 1; k >= 0; k--) {
        if (p->pane[k].n == 0 && p->n_panes > 1) {
            lt_graph_remove_pane(p, k);
        }
    }
    if (added == 0) {
        for (k = 0; k < n && k < 3; k++) {
            (void)lt_graph_pane_add(&p->pane[0], names[k]);
        }
    }
    lt_free(used);
}

void lt_graph_trace_range(const lt_graph_trace *t, const double *v, long n, double *lo, double *hi)
{
    long i;
    double a = 1e300, b = -1e300, pad;
    if (!t->autoscale && t->hi > t->lo) {
        *lo = t->lo;
        *hi = t->hi;
        return;
    }
    for (i = 0; i < n; i++) {
        if (!lt_isfinite(v[i])) {
            continue;
        }
        if (v[i] < a) { a = v[i]; }
        if (v[i] > b) { b = v[i]; }
    }
    if (a > b) {
        *lo = 0.0;
        *hi = 1.0;
        return;
    }
    if (b - a < 1e-9) {
        *lo = a - 0.5;
        *hi = b + 0.5;
        return;
    }
    pad = (b - a) * 0.05;
    *lo = a - pad;
    *hi = b + pad;
}

double lt_graph_nice_step(double span, int want)
{
    double raw, mag, f;
    if (!(span > 0.0) || want < 1) {
        return 1.0;
    }
    raw = span / (double)want;
    mag = pow(10.0, floor(log10(raw)));
    f = raw / mag;
    if (f <= 1.0) { f = 1.0; }
    else if (f <= 2.0) { f = 2.0; }
    else if (f <= 2.5) { f = 2.5; }
    else if (f <= 5.0) { f = 5.0; }
    else { f = 10.0; }
    return f * mag;
}

char *lt_graph_to_json(const lt_graph_setup *g)
{
    lt_json *root = lt_json_obj(), *pages = lt_json_arr();
    char *out;
    int p, q, k;
    lt_json_obj_set(root, "version", lt_json_num(1.0));
    for (p = 0; p < g->n_pages; p++) {
        const lt_graph_page *pg = &g->pages[p];
        lt_json *jp = lt_json_obj(), *panes = lt_json_arr();
        lt_json_obj_set(jp, "name", lt_json_str(pg->name));
        for (q = 0; q < pg->n_panes; q++) {
            lt_json *jq = lt_json_obj(), *trs = lt_json_arr();
            for (k = 0; k < pg->pane[q].n; k++) {
                const lt_graph_trace *t = &pg->pane[q].tr[k];
                lt_json *jt = lt_json_obj();
                lt_json_obj_set(jt, "channel", lt_json_str(t->channel));
                lt_json_obj_set(jt, "auto", lt_json_bool(t->autoscale));
                lt_json_obj_set(jt, "min", lt_json_num(t->lo));
                lt_json_obj_set(jt, "max", lt_json_num(t->hi));
                lt_json_obj_set(jt, "axis", lt_json_str(t->right ? "right" : "left"));
                lt_json_arr_push(trs, jt);
            }
            lt_json_obj_set(jq, "traces", trs);
            lt_json_arr_push(panes, jq);
        }
        lt_json_obj_set(jp, "panes", panes);
        lt_json_arr_push(pages, jp);
    }
    lt_json_obj_set(root, "pages", pages);
    out = lt_json_to_string(root, 1);
    lt_json_free(root);
    return out;
}

int lt_graph_from_json(const char *text, size_t len, lt_graph_setup *g)
{
    char err[128];
    lt_json *root = lt_json_parse(text, len, err, sizeof err);
    const lt_json *pages;
    lt_graph_setup tmp;
    size_t p, q, k;
    if (root == NULL || root->type != LT_JSON_OBJ) {
        lt_json_free(root);
        lt_err_set("graph setup is not valid JSON");
        return LT_EDATA;
    }
    pages = lt_json_get(root, "pages");
    if (pages == NULL || pages->type != LT_JSON_ARR) {
        lt_json_free(root);
        lt_err_set("graph setup has no pages");
        return LT_EDATA;
    }
    lt_graph_init(&tmp);
    for (p = 0; p < pages->n; p++) {
        const lt_json *jp = lt_json_at(pages, p), *panes;
        lt_graph_page *pg;
        int pi;
        if (jp == NULL || jp->type != LT_JSON_OBJ) {
            continue;
        }
        pi = lt_graph_add_page(&tmp, lt_json_get_str(jp, "name", ""));
        if (pi < 0) {
            break;
        }
        pg = &tmp.pages[pi];
        panes = lt_json_get(jp, "panes");
        for (q = 0; panes != NULL && panes->type == LT_JSON_ARR && q < panes->n; q++) {
            const lt_json *jq = lt_json_at(panes, q), *trs;
            int qi = lt_graph_add_pane(pg);
            if (qi < 0) {
                break;
            }
            trs = jq != NULL ? lt_json_get(jq, "traces") : NULL;
            for (k = 0; trs != NULL && trs->type == LT_JSON_ARR && k < trs->n; k++) {
                const lt_json *jt = lt_json_at(trs, k);
                int ti;
                if (jt == NULL || jt->type != LT_JSON_OBJ) {
                    continue;
                }
                ti = lt_graph_pane_add(&pg->pane[qi], lt_json_get_str(jt, "channel", ""));
                if (ti < 0) {
                    continue;
                }
                pg->pane[qi].tr[ti].autoscale = lt_json_get_bool(jt, "auto", 1);
                pg->pane[qi].tr[ti].lo = lt_json_get_num(jt, "min", 0.0);
                pg->pane[qi].tr[ti].hi = lt_json_get_num(jt, "max", 100.0);
                pg->pane[qi].tr[ti].right = strcmp(lt_json_get_str(jt, "axis", "left"), "right") == 0;
            }
        }
    }
    lt_json_free(root);
    lt_graph_free(g);
    *g = tmp;
    return 0;
}

int lt_graph_save(const char *path, const lt_graph_setup *g)
{
    char *s = lt_graph_to_json(g);
    int rc = lt_file_write(path, s, strlen(s));
    lt_free(s);
    if (rc != 0) {
        lt_err_set("cannot write '%s'", path);
        return LT_EIO;
    }
    return 0;
}

int lt_graph_load(const char *path, lt_graph_setup *g)
{
    size_t len = 0;
    char *buf;
    int rc;
    if (!lt_file_exists(path)) {
        return LT_ENOENT;
    }
    buf = lt_file_read(path, &len);
    if (buf == NULL) {
        return LT_EIO;
    }
    rc = lt_graph_from_json(buf, len, g);
    lt_free(buf);
    return rc;
}

/* ====================================================================== */
/* histogram                                                               */
/* ====================================================================== */

int lt_hist_bin(const double *c, int n, double v)
{
    int lo = 0, hi, mid;
    if (n <= 0 || lt_isnan(v)) {
        return -1;
    }
    hi = n - 1;
    if (v <= c[0]) {
        return 0;
    }
    if (v >= c[n - 1]) {
        return n - 1;
    }
    while (hi - lo > 1) {      /* c[lo] < v < c[hi] */
        mid = (lo + hi) / 2;
        if (c[mid] <= v) { lo = mid; } else { hi = mid; }
    }
    if (c[lo] == v) {
        return lo;
    }
    return (v - c[lo] <= c[hi] - v) ? lo : hi;
}

void lt_hist_auto_bins(double lo, double hi, int n, double *out)
{
    int i;
    if (n <= 0) {
        return;
    }
    if (n == 1) {
        out[0] = (lo + hi) / 2.0;
        return;
    }
    for (i = 0; i < n; i++) {
        out[i] = lo + (hi - lo) * (double)i / (double)(n - 1);
    }
}

void lt_hist2d_init(lt_hist2d *h, const double *xb, int nx, const double *yb, int ny)
{
    size_t cells;
    memset(h, 0, sizeof *h);
    h->nx = nx > 0 ? nx : 1;
    h->ny = ny > 0 ? ny : 1;
    cells = (size_t)h->nx * (size_t)h->ny;
    h->xb = (double *)lt_calloc((size_t)h->nx, sizeof(double));
    h->yb = (double *)lt_calloc((size_t)h->ny, sizeof(double));
    if (xb != NULL && nx > 0) { memcpy(h->xb, xb, (size_t)nx * sizeof(double)); }
    if (yb != NULL && ny > 0) { memcpy(h->yb, yb, (size_t)ny * sizeof(double)); }
    h->sum = (double *)lt_calloc(cells, sizeof(double));
    h->avg = (double *)lt_calloc(cells, sizeof(double));
    h->hits = (long *)lt_calloc(cells, sizeof(long));
    h->zmin = 0.0;
    h->zmax = 1.0;
}

void lt_hist2d_free(lt_hist2d *h)
{
    lt_free(h->xb); lt_free(h->yb); lt_free(h->sum); lt_free(h->avg); lt_free(h->hits);
    memset(h, 0, sizeof *h);
}

void lt_hist2d_add(lt_hist2d *h, double x, double y, double z)
{
    int ix = lt_hist_bin(h->xb, h->nx, x), iy = h->ny > 1 ? lt_hist_bin(h->yb, h->ny, y) : 0;
    size_t k;
    if (ix < 0 || iy < 0 || !lt_isfinite(z)) {
        return;
    }
    k = (size_t)iy * (size_t)h->nx + (size_t)ix;
    h->sum[k] += z;
    h->hits[k]++;
    h->n_binned++;
}

static double nan_v(void)
{
    volatile double z = 0.0;
    return z / z;
}

void lt_hist2d_finish(lt_hist2d *h)
{
    size_t k, cells = (size_t)h->nx * (size_t)h->ny;
    int any = 0;
    h->max_hits = 0;
    for (k = 0; k < cells; k++) {
        if (h->hits[k] > 0) {
            double a = h->sum[k] / (double)h->hits[k];
            h->avg[k] = a;
            if (!any || a < h->zmin) { h->zmin = a; }
            if (!any || a > h->zmax) { h->zmax = a; }
            any = 1;
            if (h->hits[k] > h->max_hits) { h->max_hits = h->hits[k]; }
        } else {
            h->avg[k] = nan_v();
        }
    }
    if (!any) {
        h->zmin = 0.0;
        h->zmax = 1.0;
    }
}

void lt_hist2d_from_log(lt_hist2d *h, const lt_log *log, int cx, int cy, int cz,
                        const unsigned char *mask, long i0, long i1)
{
    long i;
    if (i0 < 0) { i0 = 0; }
    if (i1 >= log->n) { i1 = log->n - 1; }
    if (cx >= 0 && cx < log->n_ch) {
        for (i = i0; i <= i1; i++) {
            double x, y, z;
            if (mask != NULL && !mask[i]) {
                continue;
            }
            x = LT_LOG_AT(log, i, cx);
            y = cy >= 0 && cy < log->n_ch ? LT_LOG_AT(log, i, cy) : 0.0;
            z = cz >= 0 && cz < log->n_ch ? LT_LOG_AT(log, i, cz) : 1.0;
            lt_hist2d_add(h, x, y, z);
        }
    }
    lt_hist2d_finish(h);
    if (cz < 0) {   /* counts only: the "average" is the hit count */
        size_t k, cells = (size_t)h->nx * (size_t)h->ny;
        h->zmin = 0.0;
        h->zmax = h->max_hits > 0 ? (double)h->max_hits : 1.0;
        for (k = 0; k < cells; k++) {
            if (h->hits[k] > 0) { h->avg[k] = (double)h->hits[k]; }
        }
    }
}

/* ====================================================================== */
/* X-Y ring                                                                */
/* ====================================================================== */

void lt_xyring_init(lt_xyring *r, int cap)
{
    memset(r, 0, sizeof *r);
    r->cap = cap > 0 ? cap : 1;
    r->x = (double *)lt_calloc((size_t)r->cap, sizeof(double));
    r->y = (double *)lt_calloc((size_t)r->cap, sizeof(double));
    r->t = (double *)lt_calloc((size_t)r->cap, sizeof(double));
}

void lt_xyring_free(lt_xyring *r)
{
    lt_free(r->x); lt_free(r->y); lt_free(r->t);
    memset(r, 0, sizeof *r);
}

void lt_xyring_clear(lt_xyring *r)
{
    r->head = 0;
    r->n = 0;
}

int lt_xyring_push(lt_xyring *r, double x, double y, double t)
{
    int slot;
    if (r->cap <= 0 || !lt_isfinite(x) || !lt_isfinite(y)) {
        return 0;
    }
    if (r->n < r->cap) {
        slot = (r->head + r->n) % r->cap;
        r->n++;
    } else {
        slot = r->head;
        r->head = (r->head + 1) % r->cap;
    }
    r->x[slot] = x;
    r->y[slot] = y;
    r->t[slot] = t;
    return 1;
}

int lt_xyring_count(const lt_xyring *r)
{
    return r->n;
}

void lt_xyring_get(const lt_xyring *r, int i, double *x, double *y, double *t)
{
    int slot = (r->head + i) % (r->cap > 0 ? r->cap : 1);
    if (x != NULL) { *x = r->x[slot]; }
    if (y != NULL) { *y = r->y[slot]; }
    if (t != NULL) { *t = r->t[slot]; }
}

void lt_xyring_expire(lt_xyring *r, double now, double max_age)
{
    while (r->n > 0 && now - r->t[r->head] > max_age) {
        r->head = (r->head + 1) % r->cap;
        r->n--;
    }
}

double lt_xy_age_alpha(double age, double fade_s)
{
    if (fade_s <= 0.0) {
        return 1.0;
    }
    if (age < 0.0) {
        age = 0.0;
    }
    if (age > fade_s) {
        return 0.0;
    }
    return 1.0 - 0.85 * (age / fade_s);
}
