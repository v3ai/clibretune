/* dash_edit.c - dashboard editing logic (see dash_edit.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "dash_edit.h"
#include "dash_priv.h"

#define HIDE_KEY "lt_prev_enabled_condition"

/* ---- geometry ---- */
void lt_dash_comp_get_rect(const lt_dash_comp *c, double v[4])
{
    if (c->gauge != NULL) {
        v[0] = c->gauge->relative_x; v[1] = c->gauge->relative_y;
        v[2] = c->gauge->relative_width; v[3] = c->gauge->relative_height;
    } else {
        v[0] = c->indicator->relative_x; v[1] = c->indicator->relative_y;
        v[2] = c->indicator->relative_width; v[3] = c->indicator->relative_height;
    }
}

void lt_dash_comp_set_rect(lt_dash_comp *c, const double v[4])
{
    if (c->gauge != NULL) {
        c->gauge->relative_x = v[0]; c->gauge->relative_y = v[1];
        c->gauge->relative_width = v[2]; c->gauge->relative_height = v[3];
    } else {
        c->indicator->relative_x = v[0]; c->indicator->relative_y = v[1];
        c->indicator->relative_width = v[2]; c->indicator->relative_height = v[3];
    }
}

void lt_dash_comp_label(const lt_dash_comp *c, char *buf, size_t cap)
{
    const char *t = NULL, *ch = NULL;
    if (c->gauge != NULL) { t = c->gauge->title; ch = c->gauge->output_channel; }
    else if (c->indicator != NULL) {
        t = c->indicator->on_text; ch = c->indicator->output_channel;
    }
    if (t != NULL && t[0] != '\0' && ch != NULL && ch[0] != '\0' && strcmp(t, ch) != 0) {
        lt_snprintf(buf, cap, "%s (%s)", t, ch);
    } else if (t != NULL && t[0] != '\0') {
        lt_snprintf(buf, cap, "%s", t);
    } else if (ch != NULL && ch[0] != '\0') {
        lt_snprintf(buf, cap, "%s", ch);
    } else {
        lt_snprintf(buf, cap, "%s", c->gauge != NULL ? "Gauge" : "Indicator");
    }
}

/* ---- history ---- */
void lt_dash_hist_init(lt_dash_hist *h) { memset(h, 0, sizeof *h); h->pos = -1; }

void lt_dash_hist_free(lt_dash_hist *h)
{
    int i;
    for (i = 0; i < h->n; i++) { lt_free(h->s[i]); }
    lt_free(h->s);
    lt_dash_hist_init(h);
}

static char *snapshot(const lt_dash_file *d)
{
    lt_buf b;
    lt_buf_init(&b);
    lt_dash_write(d, &b);
    return lt_buf_take(&b);
}

int lt_dash_hist_push(lt_dash_hist *h, const lt_dash_file *d)
{
    char *s = snapshot(d);
    int i;
    if (h->pos >= 0 && strcmp(h->s[h->pos], s) == 0) { lt_free(s); return 0; }
    for (i = h->pos + 1; i < h->n; i++) { lt_free(h->s[i]); }     /* drop the redo tail */
    h->n = h->pos + 1;
    if (h->n == LT_DASH_HIST_MAX) {                                 /* drop the oldest */
        lt_free(h->s[0]);
        memmove(h->s, h->s + 1, sizeof(char *) * (size_t)(h->n - 1));
        h->n--;
    }
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->s = (char **)lt_realloc(h->s, sizeof(char *) * (size_t)h->cap);
    }
    h->s[h->n++] = s;
    h->pos = h->n - 1;
    return 1;
}

int lt_dash_hist_can_undo(const lt_dash_hist *h) { return h->pos > 0; }
int lt_dash_hist_can_redo(const lt_dash_hist *h) { return h->pos >= 0 && h->pos < h->n - 1; }

static lt_dash_file *restore(lt_dash_hist *h, int pos)
{
    char err[160];
    lt_dash_file *f = lt_dash_parse(h->s[pos], strlen(h->s[pos]), err, sizeof err);
    if (f != NULL) { h->pos = pos; }
    return f;
}

lt_dash_file *lt_dash_hist_undo(lt_dash_hist *h)
{
    return lt_dash_hist_can_undo(h) ? restore(h, h->pos - 1) : NULL;
}

lt_dash_file *lt_dash_hist_redo(lt_dash_hist *h)
{
    return lt_dash_hist_can_redo(h) ? restore(h, h->pos + 1) : NULL;
}

/* ---- clone / copy / paste ---- */
static lt_bool id_taken(const lt_dash_file *d, const char *id)
{
    size_t i;
    for (i = 0; i < d->cluster.n_comps; i++) {
        const char *o = d->cluster.comps[i].gauge ? d->cluster.comps[i].gauge->id
                                                  : d->cluster.comps[i].indicator->id;
        if (o != NULL && strcmp(o, id) == 0) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static void fresh_id(lt_dash_file *d, char **slot, const char *prefix)
{
    char id[48];
    int k;
    for (k = 1; k < 100000; k++) {
        lt_snprintf(id, sizeof id, "%s_%d", prefix, k);
        if (!id_taken(d, id)) { break; }
    }
    dash_set_str(slot, id);
}

static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* one-component .dash text of src.comps[idx] (components are borrowed, not copied) */
static char *comp_xml(const lt_dash_file *src, size_t idx, size_t *len)
{
    lt_dash_file *tmp = lt_dash_new();
    lt_buf b;
    char *s;
    tmp->cluster.comps = (lt_dash_comp *)lt_calloc(1, sizeof(lt_dash_comp));
    tmp->cluster.comps[0] = src->cluster.comps[idx];
    tmp->cluster.n_comps = 1;
    lt_buf_init(&b);
    lt_dash_write(tmp, &b);
    lt_free(tmp->cluster.comps);
    tmp->cluster.comps = NULL;
    tmp->cluster.n_comps = 0;
    lt_dash_free(tmp);
    *len = b.len;
    s = lt_buf_take(&b);
    return s;
}

/* move the first component of parsed into dst; returns the new index */
static int adopt(lt_dash_file *dst, lt_dash_file *parsed, double dx, double dy)
{
    lt_dash_comp *pc;
    double v[4];
    int idx;
    if (parsed->cluster.n_comps == 0) { return -1; }
    pc = &parsed->cluster.comps[0];
    if (pc->gauge != NULL) {
        lt_dash_gauge *g = lt_dash_add_gauge(dst);
        lt_dash_gauge_free(g);
        *g = *pc->gauge;
        memset(pc->gauge, 0, sizeof *pc->gauge);
        fresh_id(dst, &g->id, "gauge");
    } else {
        lt_dash_indicator *in = lt_dash_add_indicator(dst);
        lt_dash_indicator_free(in);
        *in = *pc->indicator;
        memset(pc->indicator, 0, sizeof *pc->indicator);
        fresh_id(dst, &in->id, "indicator");
    }
    idx = (int)dst->cluster.n_comps - 1;
    lt_dash_comp_get_rect(&dst->cluster.comps[idx], v);
    v[0] = clampd(v[0] + dx, 0.0, 1.0 - v[2]);
    v[1] = clampd(v[1] + dy, 0.0, 1.0 - v[3]);
    lt_dash_comp_set_rect(&dst->cluster.comps[idx], v);
    return idx;
}

int lt_dash_comp_copy_to(lt_dash_file *dst, const lt_dash_file *src, size_t idx, double dx, double dy)
{
    size_t len;
    char *xml, err[160];
    lt_dash_file *p;
    int r;
    if (idx >= src->cluster.n_comps) { return -1; }
    xml = comp_xml(src, idx, &len);
    p = lt_dash_parse(xml, len, err, sizeof err);
    lt_free(xml);
    if (p == NULL) { return -1; }
    r = adopt(dst, p, dx, dy);
    lt_dash_free(p);
    return r;
}

int lt_dash_comp_clone(lt_dash_file *d, size_t idx)
{
    return lt_dash_comp_copy_to(d, d, idx, 0.02, 0.02);
}

char *lt_dash_clip_copy(const lt_dash_file *d, const size_t *idx, size_t n)
{
    lt_dash_file *tmp = lt_dash_new();
    lt_buf b;
    size_t i;
    for (i = 0; i < n; i++) { (void)lt_dash_comp_copy_to(tmp, d, idx[i], 0.0, 0.0); }
    lt_buf_init(&b);
    lt_dash_write(tmp, &b);
    lt_dash_free(tmp);
    return lt_buf_take(&b);
}

size_t lt_dash_clip_paste(lt_dash_file *d, const char *text, double dx, double dy, size_t *first)
{
    char err[160];
    lt_dash_file *src;
    size_t i, added = 0;
    if (first != NULL) { *first = d->cluster.n_comps; }
    if (text == NULL || text[0] == '\0') { return 0; }
    src = lt_dash_parse(text, strlen(text), err, sizeof err);
    if (src == NULL) { return 0; }
    for (i = 0; i < src->cluster.n_comps; i++) {
        if (lt_dash_comp_copy_to(d, src, i, dx, dy) >= 0) { added++; }
    }
    lt_dash_free(src);
    return added;
}

/* ---- z-order, hide ---- */
size_t lt_dash_comp_move_z(lt_dash_file *d, size_t idx, size_t to)
{
    lt_dash_cluster *c = &d->cluster;
    lt_dash_comp tmp;
    if (idx >= c->n_comps) { return idx; }
    if (to >= c->n_comps) { to = c->n_comps - 1; }
    if (to == idx) { return idx; }
    tmp = c->comps[idx];
    if (to > idx) { memmove(&c->comps[idx], &c->comps[idx + 1], (to - idx) * sizeof tmp); }
    else { memmove(&c->comps[to + 1], &c->comps[to], (idx - to) * sizeof tmp); }
    c->comps[to] = tmp;
    return to;
}

static char **cond_slot(lt_dash_comp *c)
{
    return c->gauge != NULL ? &c->gauge->enabled_condition : &c->indicator->enabled_condition;
}

static lt_dash_extras *extras_of(lt_dash_comp *c)
{
    return c->gauge != NULL ? &c->gauge->extras : &c->indicator->extras;
}

lt_bool lt_dash_comp_hidden(const lt_dash_comp *c)
{
    const char *s = c->gauge != NULL ? c->gauge->enabled_condition : c->indicator->enabled_condition;
    return s != NULL && lt_stricmp(s, "false") == 0;
}

void lt_dash_comp_set_hidden(lt_dash_comp *c, lt_bool hidden)
{
    char **slot = cond_slot(c);
    lt_dash_extras *ex = extras_of(c);
    if (hidden == lt_dash_comp_hidden(c)) { return; }
    if (hidden) {
        lt_dash_extras_set(ex, HIDE_KEY, *slot != NULL ? *slot : "");
        dash_set_str(slot, "false");
    } else {
        const char *prev = lt_dash_extras_get(ex, HIDE_KEY);
        dash_set_str(slot, (prev != NULL && prev[0] != '\0') ? prev : NULL);
        lt_dash_extras_set(ex, HIDE_KEY, "");
    }
}

/* ---- alignment ---- */
double lt_dash_snap(double x, int cells)
{
    if (cells <= 0) { return x; }
    return floor(x * (double)cells + 0.5) / (double)cells;
}

void lt_dash_align(lt_dash_file *d, const size_t *idx, size_t n, lt_align_op op)
{
    double (*r)[4];
    double minx = 2.0, miny = 2.0, maxx = -1.0, maxy = -1.0;
    size_t i, j;
    if (n < 2) { return; }
    r = (double (*)[4])lt_calloc(n, sizeof(double[4]));
    for (i = 0; i < n; i++) {
        lt_dash_comp_get_rect(&d->cluster.comps[idx[i]], r[i]);
        if (r[i][0] < minx) { minx = r[i][0]; }
        if (r[i][1] < miny) { miny = r[i][1]; }
        if (r[i][0] + r[i][2] > maxx) { maxx = r[i][0] + r[i][2]; }
        if (r[i][1] + r[i][3] > maxy) { maxy = r[i][1] + r[i][3]; }
    }
    if (op == LT_ALIGN_DIST_H || op == LT_ALIGN_DIST_V) {
        size_t *ord = (size_t *)lt_calloc(n, sizeof(size_t));
        int h = op == LT_ALIGN_DIST_H;
        double total = 0.0, gap, pos;
        size_t k = h ? 0 : 1;
        for (i = 0; i < n; i++) { ord[i] = i; total += r[i][k + 2]; }
        for (i = 1; i < n; i++) {                       /* sort by position */
            size_t key = ord[i];
            j = i;
            while (j > 0 && r[ord[j - 1]][k] > r[key][k]) { ord[j] = ord[j - 1]; j--; }
            ord[j] = key;
        }
        if (n >= 3) {
            gap = ((h ? maxx - minx : maxy - miny) - total) / (double)(n - 1);
            pos = h ? minx : miny;
            for (i = 0; i < n; i++) {
                r[ord[i]][k] = pos;
                pos += r[ord[i]][k + 2] + gap;
            }
        }
        lt_free(ord);
    } else {
        for (i = 0; i < n; i++) {
            switch (op) {
            case LT_ALIGN_LEFT: r[i][0] = minx; break;
            case LT_ALIGN_RIGHT: r[i][0] = maxx - r[i][2]; break;
            case LT_ALIGN_HCENTER: r[i][0] = (minx + maxx) / 2.0 - r[i][2] / 2.0; break;
            case LT_ALIGN_TOP: r[i][1] = miny; break;
            case LT_ALIGN_BOTTOM: r[i][1] = maxy - r[i][3]; break;
            case LT_ALIGN_VCENTER: r[i][1] = (miny + maxy) / 2.0 - r[i][3] / 2.0; break;
            case LT_ALIGN_SAME_W: r[i][2] = r[0][2]; break;
            case LT_ALIGN_SAME_H: r[i][3] = r[0][3]; break;
            default: break;
            }
        }
    }
    for (i = 0; i < n; i++) {
        r[i][0] = clampd(r[i][0], 0.0, 1.0 - r[i][2]);
        r[i][1] = clampd(r[i][1], 0.0, 1.0 - r[i][3]);
        lt_dash_comp_set_rect(&d->cluster.comps[idx[i]], r[i]);
    }
    lt_free(r);
}

/* ---- assets ---- */
size_t lt_dash_b64_encode(const void *data, size_t n, char *out, size_t cap)
{
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const lt_u8 *p = (const lt_u8 *)data;
    size_t i, o = 0;
    if (cap < 4 * ((n + 2) / 3) + 1) { if (cap > 0) { out[0] = '\0'; } return 0; }
    for (i = 0; i < n; i += 3) {
        lt_u32 v = (lt_u32)p[i] << 16;
        if (i + 1 < n) { v |= (lt_u32)p[i + 1] << 8; }
        if (i + 2 < n) { v |= (lt_u32)p[i + 2]; }
        out[o++] = tb[(v >> 18) & 63];
        out[o++] = tb[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tb[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tb[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

int lt_dash_asset_add(lt_dash_file *d, const char *name, const void *data, size_t len)
{
    lt_dash_cluster *c = &d->cluster;
    const char *base = lt_path_base(name);
    const char *ext = lt_path_ext(base);
    lt_dash_image *im = NULL;
    size_t i, cap;
    char *b64;
    for (i = 0; i < c->n_images; i++) {
        if (c->images[i].image_id != NULL && strcmp(c->images[i].image_id, base) == 0) { im = &c->images[i]; break; }
    }
    if (im == NULL) {
        c->images = (lt_dash_image *)lt_realloc(c->images, sizeof(lt_dash_image) * (c->n_images + 1));
        im = &c->images[c->n_images++];
        memset(im, 0, sizeof *im);
    } else {
        lt_free(im->data_base64); im->data_base64 = NULL;
    }
    dash_set_str(&im->file_name, base);
    dash_set_str(&im->image_id, base);
    im->type = lt_stricmp(ext, ".gif") == 0 ? LT_RES_GIF : (lt_stricmp(ext, ".ttf") == 0 ? LT_RES_TTF : LT_RES_PNG);
    cap = 4 * ((len + 2) / 3) + 1;
    b64 = (char *)lt_malloc(cap);
    (void)lt_dash_b64_encode(data, len, b64, cap);
    im->data_base64 = b64;
    return (int)(im - c->images);
}

lt_bool lt_dash_asset_used(const lt_dash_file *d, size_t idx)
{
    const lt_dash_cluster *c = &d->cluster;
    const char *id;
    size_t i;
    if (idx >= c->n_images) { return LT_FALSE; }
    id = c->images[idx].image_id;
    if (id == NULL) { return LT_FALSE; }
    if (c->background_image_file_name != NULL && strcmp(c->background_image_file_name, id) == 0) { return LT_TRUE; }
    for (i = 0; i < c->n_comps; i++) {
        const lt_dash_gauge *g = c->comps[i].gauge;
        const lt_dash_indicator *in = c->comps[i].indicator;
        if (g != NULL) {
            if (g->background_image_file_name != NULL && strcmp(g->background_image_file_name, id) == 0) { return LT_TRUE; }
            if (g->needle_image_file_name != NULL && strcmp(g->needle_image_file_name, id) == 0) { return LT_TRUE; }
        } else if (in != NULL) {
            if (in->on_image_file_name != NULL && strcmp(in->on_image_file_name, id) == 0) { return LT_TRUE; }
            if (in->off_image_file_name != NULL && strcmp(in->off_image_file_name, id) == 0) { return LT_TRUE; }
        }
    }
    return LT_FALSE;
}

void lt_dash_asset_remove(lt_dash_file *d, size_t idx)
{
    lt_dash_cluster *c = &d->cluster;
    if (idx >= c->n_images) { return; }
    dash_image_free(&c->images[idx]);
    memmove(&c->images[idx], &c->images[idx + 1], (c->n_images - idx - 1) * sizeof(lt_dash_image));
    c->n_images--;
}

/* ---- INI sync ---- */
static const lt_ini_gauge *find_ini_gauge(const lt_ini *ini, const lt_dash_gauge *g)
{
    size_t i;
    for (i = 0; i < ini->n_gauges; i++) {
        if (g->output_channel != NULL && ini->gauges[i]->channel != NULL &&
            lt_stricmp(ini->gauges[i]->channel, g->output_channel) == 0) { return ini->gauges[i]; }
    }
    for (i = 0; i < ini->n_gauges; i++) {
        if (g->title != NULL && ini->gauges[i]->name != NULL && lt_stricmp(ini->gauges[i]->name, g->title) == 0) {
            return ini->gauges[i];
        }
    }
    return NULL;
}

size_t lt_dash_sync_ranges(lt_dash_file *d, const lt_ini *ini, const lt_expr_env *env)
{
    size_t i, changed = 0;
    lt_dash_gauge tmp;
    if (ini == NULL) { return 0; }
    memset(&tmp, 0, sizeof tmp);
    for (i = 0; i < d->cluster.n_comps; i++) {
        lt_dash_gauge *g = d->cluster.comps[i].gauge;
        const lt_ini_gauge *src;
        int diff;
        if (g == NULL) { continue; }
        src = find_ini_gauge(ini, g);
        if (src == NULL) { continue; }
        lt_dash_gauge_from_ini(src, &tmp, env);
        if (!(tmp.max > tmp.min)) { continue; }     /* unusable range: keep the dashboard's own */
        diff = g->min != tmp.min || g->max != tmp.max || g->value_digits != tmp.value_digits ||
               g->has_low_warning != tmp.has_low_warning || g->has_high_warning != tmp.has_high_warning ||
               g->has_low_critical != tmp.has_low_critical || g->has_high_critical != tmp.has_high_critical ||
               (tmp.has_low_warning && g->low_warning != tmp.low_warning) ||
               (tmp.has_high_warning && g->high_warning != tmp.high_warning) ||
               (tmp.has_low_critical && g->low_critical != tmp.low_critical) ||
               (tmp.has_high_critical && g->high_critical != tmp.high_critical) ||
               strcmp(g->units ? g->units : "", tmp.units ? tmp.units : "") != 0;
        if (!diff) { continue; }
        g->min = tmp.min; g->max = tmp.max; g->value_digits = tmp.value_digits;
        g->has_low_warning = tmp.has_low_warning; g->low_warning = tmp.low_warning;
        g->has_high_warning = tmp.has_high_warning; g->high_warning = tmp.high_warning;
        g->has_low_critical = tmp.has_low_critical; g->low_critical = tmp.low_critical;
        g->has_high_critical = tmp.has_high_critical; g->high_critical = tmp.high_critical;
        dash_set_str(&g->units, tmp.units);
        changed++;
    }
    lt_dash_gauge_free(&tmp);
    return changed;
}

/* ---- indicator grid ---- */
void lt_dash_layout_indicators(lt_dash_file *d, int columns, lt_bool fill_empty)
{
    lt_dash_cluster *c = &d->cluster;
    size_t i, n = 0, *ids;
    double x0 = 2.0, y0 = 2.0, x1 = -1.0, y1 = -1.0, cw, ch;
    size_t cols, rows, k;
    ids = (size_t *)lt_calloc(c->n_comps + 1, sizeof(size_t));
    for (i = 0; i < c->n_comps; i++) {
        double v[4];
        if (c->comps[i].indicator == NULL) { continue; }
        lt_dash_comp_get_rect(&c->comps[i], v);
        if (v[0] < x0) { x0 = v[0]; }
        if (v[1] < y0) { y0 = v[1]; }
        if (v[0] + v[2] > x1) { x1 = v[0] + v[2]; }
        if (v[1] + v[3] > y1) { y1 = v[1] + v[3]; }
        ids[n++] = i;
    }
    if (n == 0) { lt_free(ids); return; }
    if (columns <= 0) {
        /* widest existing row: indicators sharing (about) the same top edge */
        size_t best = 1;
        for (i = 0; i < n; i++) {
            double a[4], b[4];
            size_t same = 0;
            lt_dash_comp_get_rect(&c->comps[ids[i]], a);
            for (k = 0; k < n; k++) {
                lt_dash_comp_get_rect(&c->comps[ids[k]], b);
                if (fabs(a[1] - b[1]) < 0.002) { same++; }
            }
            if (same > best) { best = same; }
        }
        cols = best;
    } else {
        cols = (size_t)columns;
    }
    if (cols > n) { cols = n; }
    if (cols < 1) { cols = 1; }
    rows = (n + cols - 1) / cols;
    cw = (x1 - x0) / (double)cols;
    ch = (y1 - y0) / (double)rows;
    for (i = 0; i < n; i++) {
        double v[4];
        size_t col = i % cols, row = i / cols, in_row = cols;
        double w = cw;
        double xs = x0 + cw * (double)col;
        if (fill_empty && row == rows - 1) {
            in_row = n - row * cols;
            w = (x1 - x0) / (double)in_row;
            xs = x0 + w * (double)col;
        }
        v[0] = xs + 0.003; v[1] = y0 + ch * (double)row + 0.003;
        v[2] = w - 0.006; v[3] = ch - 0.006;
        lt_dash_comp_set_rect(&c->comps[ids[i]], v);
    }
    lt_free(ids);
}
