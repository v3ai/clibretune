/* gfx_rec.c - retained rendering: a display list of the frame's lt_gfx calls and a tile cache.
 *
 * While lt_tiles records a context, every pixel-mode draw call stores its parameters (strings,
 * polygon points and blitted pixels copied, offscreen canvases referenced by their content
 * generation), the clip, the translation and a conservative bounding box, plus a hash of all of
 * it. At the end of the frame each screen tile gets the ordered hash of the calls touching it;
 * tiles whose hash equals the previous frame's keep their pixels, the others are rasterized by
 * replaying the calls that touch them with the clip narrowed to the tile run and to each call's
 * box (rasterization does not depend on the clip except where pixels are written, so this is
 * pixel-identical to drawing everything). Replay starts at the last opaque fill covering the
 * whole run, which skips the overdraw of window and panel backgrounds.
 *
 * A canvas referenced by the recorded frame that is about to change (drawn into, touched,
 * freed) is copied first (lt_rec_canvas_changing), so the replay always sees the pixels the
 * call saw.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "gui.h"
#include "gfx_priv.h"

#define TILE_W 64
#define TILE_H 32

typedef struct rec_src {
    const lt_canvas *c;
    lt_canvas copy;      /* pixels saved when the original changed while still referenced */
    int copied;
    lt_u32 gen;
} rec_src;

struct lt_rec {
    lt_dcmd *cmd;
    long ncmd, capcmd;
    char *bytes;
    long nbytes, capbytes;
    double *dbl;
    long ndbl, capdbl;
    lt_u32 *pix;
    long npix, cappix;
    rec_src *src;
    int nsrc, capsrc;
    int cw, ch;
};

typedef struct run_rect { int x, y, w, h, row; } run_rect;

struct lt_tiles {
    struct lt_rec rec;
    int recorded;               /* rec holds a frame (begin was called in pixel mode) */
    int cw, ch, tx, ty;         /* canvas size and tile grid of the stored hashes */
    lt_u32 *h1, *h2, *p1, *p2;  /* tile hashes: this frame, previous frame */
    long *cut;                  /* per tile: last call of this frame's prefix that equals the
                                 * whole previous frame (pixels already there), or -1 */
    int valid;                  /* p1/p2 describe what the canvas shows */
    long *rowstart, *rowidx;    /* per tile row: indices of the calls touching it */
    long caprowidx;
    run_rect *runs;
    int nruns, capruns;
    lt_canvas scratch;          /* full render for lt_tiles_verify */
    long st_cmd;
    int st_dirty;
    double st_pixels;
};

static struct lt_rec *g_live;   /* recorder whose canvas sources are protected */
static lt_u32 g_gen;

lt_u32 lt_gfx_next_gen(void)
{
    g_gen = (g_gen + 1U) & LT_U32_MASK;
    if (g_gen == 0) { g_gen = 1; }
    return g_gen;
}

/* ------------------------------------------------------------------ */
/* growable arrays                                                      */
static void *grow(void *p, long *cap, long need, size_t elem)
{
    long n = *cap;
    size_t bytes;
    if (need <= n) { return p; }
    if (n < 64) { n = 64; }
    while (n < need) { n *= 2; }
    if (!lt_mul_size((size_t)n, elem, &bytes)) { bytes = (size_t)-1; }   /* lt_realloc aborts */
    *cap = n;
    return lt_realloc(p, bytes);
}

static void src_release(struct lt_rec *r)
{
    int i;
    for (i = 0; i < r->nsrc; i++) {
        if (r->src[i].copied && r->src[i].copy.px != NULL) { lt_free(r->src[i].copy.px); }
    }
    r->nsrc = 0;
}

static void rec_reset(struct lt_rec *r)
{
    src_release(r);
    r->ncmd = 0; r->nbytes = 0; r->ndbl = 0; r->npix = 0;
}

static void rec_free(struct lt_rec *r)
{
    src_release(r);
    lt_free(r->cmd); lt_free(r->bytes); lt_free(r->dbl); lt_free(r->pix); lt_free(r->src);
    memset(r, 0, sizeof(*r));
    if (g_live == r) { g_live = NULL; }
}

/* ------------------------------------------------------------------ */
/* hashing (two independent 32-bit lanes)                               */
static lt_u32 rotl(lt_u32 v, int n)
{
    return ((v << n) | (v >> (32 - n))) & LT_U32_MASK;
}

typedef struct hstate { lt_u32 a, b; } hstate;

static void hword(hstate *h, lt_u32 v)
{
    lt_u32 k = (v * 0xCC9E2D51U) & LT_U32_MASK;
    k = (rotl(k, 15) * 0x1B873593U) & LT_U32_MASK;
    h->a ^= k;
    h->a = (rotl(h->a, 13) * 5U + 0xE6546B64U) & LT_U32_MASK;
    h->b = ((h->b ^ v) * 0x01000193U) & LT_U32_MASK;
    h->b ^= h->b >> 15;
}

static void hint(hstate *h, long v)
{
    hword(h, (lt_u32)((unsigned long)v & 0xFFFFFFFFUL));
}

static void hbytes(hstate *h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        hword(h, (lt_u32)p[i] | ((lt_u32)p[i + 1] << 8) | ((lt_u32)p[i + 2] << 16) |
                 ((lt_u32)p[i + 3] << 24));
    }
    if (i < n) {
        lt_u32 v = 0;
        int s = 0;
        for (; i < n; i++, s += 8) { v |= (lt_u32)p[i] << s; }
        hword(h, v);
    }
    hint(h, (long)n);
}

static void hrect(hstate *h, lt_rect r)
{
    hint(h, r.x); hint(h, r.y); hint(h, r.w); hint(h, r.h);
}

/* ------------------------------------------------------------------ */
/* recording                                                            */
lt_dcmd *lt_rec_cmd(lt_gfx *g, int op, lt_rect bb)
{
    struct lt_rec *r = g->rec;
    lt_dcmd *k;
    bb = lt_rect_intersect(bb, g->clip);
    bb = lt_rect_intersect(bb, lt_rect_make(0, 0, r->cw, r->ch));
    if (bb.w <= 0 || bb.h <= 0) { return NULL; }
    r->cmd = (lt_dcmd *)grow(r->cmd, &r->capcmd, r->ncmd + 1, sizeof(lt_dcmd));
    k = &r->cmd[r->ncmd++];
    memset(k, 0, sizeof(*k));
    k->op = op;
    k->clip = g->clip;
    k->bb = bb;
    k->ox = g->ox; k->oy = g->oy;
    k->bitmap_text = g->bitmap_text ? 1 : 0;
    k->off = -1; k->off2 = -1; k->src = -1;
    return k;
}

long lt_rec_bytes(lt_gfx *g, const void *data, long n)
{
    struct lt_rec *r = g->rec;
    long off = r->nbytes;
    r->bytes = (char *)grow(r->bytes, &r->capbytes, r->nbytes + n, 1);
    if (n > 0) { memcpy(r->bytes + off, data, (size_t)n); }
    r->nbytes += n;
    return off;
}

long lt_rec_doubles(lt_gfx *g, const double *v, long n)
{
    struct lt_rec *r = g->rec;
    long off = r->ndbl;
    r->dbl = (double *)grow(r->dbl, &r->capdbl, r->ndbl + n, sizeof(double));
    if (n > 0) { memcpy(r->dbl + off, v, (size_t)n * sizeof(double)); }
    r->ndbl += n;
    return off;
}

long lt_rec_pixels(lt_gfx *g, const lt_u32 *px, int w, int h, int pitch)
{
    struct lt_rec *r = g->rec;
    long off = r->npix, n = (long)w * (long)h;
    int y;
    r->pix = (lt_u32 *)grow(r->pix, &r->cappix, r->npix + n, sizeof(lt_u32));
    for (y = 0; y < h; y++) {
        memcpy(r->pix + off + (long)y * w, px + (size_t)y * (size_t)pitch, (size_t)w * sizeof(lt_u32));
    }
    r->npix += n;
    return off;
}

int lt_rec_canvas(lt_gfx *g, const lt_canvas *c)
{
    struct lt_rec *r = g->rec;
    int i;
    long cap = r->capsrc;
    for (i = r->nsrc - 1; i >= 0; i--) {
        if (r->src[i].c == c && !r->src[i].copied && r->src[i].gen == c->gen) { return i; }
    }
    r->src = (rec_src *)grow(r->src, &cap, r->nsrc + 1, sizeof(rec_src));
    r->capsrc = (int)cap;
    i = r->nsrc++;
    memset(&r->src[i], 0, sizeof(rec_src));
    r->src[i].c = c;
    r->src[i].gen = c->gen;
    return i;
}

void lt_rec_canvas_changing(const lt_canvas *c)
{
    struct lt_rec *r = g_live;
    int i;
    if (r == NULL || r->nsrc == 0 || c == NULL) { return; }
    for (i = 0; i < r->nsrc; i++) {
        rec_src *s = &r->src[i];
        if (s->c == c && !s->copied) {
            s->copy = *c;
            s->copy.pitch = c->w;
            s->copy.px = NULL;
            if (c->px != NULL && c->w > 0 && c->h > 0) {
                int y;
                s->copy.px = (lt_u32 *)lt_malloc((size_t)c->w * (size_t)c->h * sizeof(lt_u32));
                for (y = 0; y < c->h; y++) {
                    memcpy(s->copy.px + (size_t)y * (size_t)c->w, c->px + (size_t)y * (size_t)c->pitch,
                           (size_t)c->w * sizeof(lt_u32));
                }
            }
            s->copied = 1;
        }
    }
}

const char *lt_rec_bytes_at(const struct lt_rec *r, long off) { return r->bytes + off; }
const double *lt_rec_doubles_at(const struct lt_rec *r, long off) { return r->dbl + off; }
const lt_u32 *lt_rec_pixels_at(const struct lt_rec *r, long off) { return r->pix + off; }

const lt_canvas *lt_rec_canvas_at(const struct lt_rec *r, int src)
{
    const rec_src *s;
    if (src < 0 || src >= r->nsrc) { return NULL; }
    s = &r->src[src];
    return s->copied ? &s->copy : s->c;
}

void lt_rec_done(lt_gfx *g, lt_dcmd *k)
{
    struct lt_rec *r = g->rec;
    hstate h;
    int i;
    h.a = 0x9747B28CU; h.b = 0x811C9DC5U;
    hint(&h, k->op);
    hrect(&h, k->clip);
    hrect(&h, k->bb);
    hint(&h, k->ox); hint(&h, k->oy); hint(&h, k->bitmap_text);
    hword(&h, k->c); hword(&h, k->c2);
    for (i = 0; i < 4; i++) { hint(&h, k->i[i]); }
    hbytes(&h, k->d, sizeof(k->d));
    hword(&h, (lt_u32)(((size_t)k->f >> 3) & 0xFFFFFFFFUL));
    hint(&h, k->len);
    if (k->off >= 0) {
        switch (k->op) {
        case LT_OP_POLY: hbytes(&h, r->dbl + k->off, (size_t)k->len * sizeof(double)); break;
        case LT_OP_BLIT: hbytes(&h, r->pix + k->off, (size_t)k->len * sizeof(lt_u32)); break;
        default: hbytes(&h, r->bytes + k->off, (size_t)k->len); break;
        }
    }
    if (k->off2 >= 0) { hbytes(&h, r->bytes + k->off2, 256); }
    if (k->src >= 0) {
        const lt_canvas *c = r->src[k->src].c;
        hword(&h, r->src[k->src].gen);
        hint(&h, c->w); hint(&h, c->h);
    }
    k->h1 = h.a; k->h2 = h.b;
}

lt_bool lt_gfx_recording(const lt_gfx *g)
{
    return g != NULL && g->rec != NULL ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* tiles                                                                */
lt_tiles *lt_tiles_new(void)
{
    lt_tiles *t = (lt_tiles *)lt_calloc(1, sizeof(lt_tiles));
    return t;
}

static void tiles_free_grid(lt_tiles *t)
{
    lt_free(t->h1); lt_free(t->h2); lt_free(t->p1); lt_free(t->p2); lt_free(t->rowstart);
    lt_free(t->cut);
    t->h1 = t->h2 = t->p1 = t->p2 = NULL;
    t->cut = NULL;
    t->rowstart = NULL;
    t->valid = 0;
    t->cw = t->ch = t->tx = t->ty = 0;
}

void lt_tiles_free(lt_tiles *t)
{
    if (t == NULL) { return; }
    rec_free(&t->rec);
    tiles_free_grid(t);
    lt_free(t->rowidx);
    lt_free(t->runs);
    lt_canvas_free(&t->scratch);
    lt_free(t);
}

void lt_tiles_invalidate(lt_tiles *t)
{
    if (t != NULL) { t->valid = 0; }
}

void lt_tiles_begin(lt_tiles *t, lt_gfx *g)
{
    rec_reset(&t->rec);
    t->recorded = 0;
    if (g->canvas == NULL || g->cells != NULL) { return; }
    g_live = &t->rec;
    t->rec.cw = g->canvas->w;
    t->rec.ch = g->canvas->h;
    t->recorded = 1;
    g->rec = &t->rec;
}

void lt_tiles_cancel(lt_tiles *t, lt_gfx *g)
{
    g->rec = NULL;
    rec_reset(&t->rec);
    t->recorded = 0;
    t->st_cmd = 0;
    t->st_dirty = 0;
    t->st_pixels = 0.0;
    t->nruns = 0;
}

static void tiles_grid(lt_tiles *t, int cw, int ch)
{
    size_t n;
    if (t->h1 != NULL && t->cw == cw && t->ch == ch) { return; }
    tiles_free_grid(t);
    t->cw = cw; t->ch = ch;
    t->tx = (cw + TILE_W - 1) / TILE_W;
    t->ty = (ch + TILE_H - 1) / TILE_H;
    n = (size_t)t->tx * (size_t)t->ty;
    t->h1 = (lt_u32 *)lt_malloc(n * sizeof(lt_u32));
    t->h2 = (lt_u32 *)lt_malloc(n * sizeof(lt_u32));
    t->p1 = (lt_u32 *)lt_calloc(n, sizeof(lt_u32));
    t->p2 = (lt_u32 *)lt_calloc(n, sizeof(lt_u32));
    t->cut = (long *)lt_malloc(n * sizeof(long));
    t->rowstart = (long *)lt_malloc(((size_t)t->ty + 1) * sizeof(long));
    t->valid = 0;
}

/* is k an opaque fill covering all of r? */
static int covers(const lt_dcmd *k, lt_rect r)
{
    if (k->op == LT_OP_FILL) {
        if (LT_COLOR_A(k->c) != 255) { return 0; }
    } else if (k->op != LT_OP_CLEAR) {
        return 0;
    }
    return k->bb.x <= r.x && k->bb.y <= r.y && k->bb.x + k->bb.w >= r.x + r.w &&
           k->bb.y + k->bb.h >= r.y + r.h;
}

/* rasterize the calls idx[0..n) clipped to r. cut >= 0: r already shows calls up to index cut
 * (the previous frame was a prefix of this one), only the later ones are drawn on top */
static void render_run(lt_tiles *t, lt_gfx *g, lt_rect r, const long *idx, long n, long cut)
{
    const lt_dcmd *cmd = t->rec.cmd;
    long i, start = -1, first = 0;
    if (cut >= 0) {
        while (first < n && idx[first] <= cut) { first++; }
    }
    for (i = n - 1; i >= first; i--) {
        if (covers(&cmd[idx[i]], r)) { start = i; break; }
    }
    if (start < 0 && cut >= 0) {
        start = first;
    } else if (start < 0) {
        int y;
        for (y = r.y; y < r.y + r.h; y++) {
            memset(g->canvas->px + (size_t)y * (size_t)g->canvas->pitch + (size_t)r.x, 0,
                   (size_t)r.w * sizeof(lt_u32));
        }
        start = 0;
    }
    for (i = start; i < n; i++) {
        const lt_dcmd *k = &cmd[idx[i]];
        lt_rect lim = lt_rect_intersect(k->bb, r);
        if (lim.w > 0 && lim.h > 0) { lt_gfx_replay(g, &t->rec, k, lim); }
    }
}

static void add_out(lt_rect *out, int *n, int max_out, lt_rect r)
{
    int i, best = -1;
    double best_cost = 0.0;
    /* extend a rectangle of the previous tile row with the same horizontal extent */
    for (i = 0; i < *n; i++) {
        if (out[i].x == r.x && out[i].w == r.w && out[i].y + out[i].h == r.y) {
            out[i].h += r.h;
            return;
        }
    }
    if (*n < max_out) { out[(*n)++] = r; return; }
    /* too many: merge into the rectangle whose bounding box grows least */
    for (i = 0; i < *n; i++) {
        int x0 = LT_MIN(out[i].x, r.x), y0 = LT_MIN(out[i].y, r.y);
        int x1 = LT_MAX(out[i].x + out[i].w, r.x + r.w), y1 = LT_MAX(out[i].y + out[i].h, r.y + r.h);
        double cost = (double)(x1 - x0) * (double)(y1 - y0) - (double)out[i].w * (double)out[i].h;
        if (best < 0 || cost < best_cost) { best = i; best_cost = cost; }
    }
    {
        lt_rect *o = &out[best];
        int x0 = LT_MIN(o->x, r.x), y0 = LT_MIN(o->y, r.y);
        int x1 = LT_MAX(o->x + o->w, r.x + r.w), y1 = LT_MAX(o->y + o->h, r.y + r.h);
        o->x = x0; o->y = y0; o->w = x1 - x0; o->h = y1 - y0;
    }
}

int lt_tiles_end(lt_tiles *t, lt_gfx *g, lt_rect *out, int max_out)
{
    struct lt_rec *r = &t->rec;
    long i, total = 0;
    int tx, ty, n_out = 0, all, ndirty = 0;
    double pixels = 0.0;
    lt_rect prev_clip;
    g->rec = NULL;
    t->st_cmd = r->ncmd;
    t->st_dirty = 0;
    t->st_pixels = 0.0;
    t->nruns = 0;
    if (!t->recorded || g->canvas == NULL || g->canvas->w != r->cw || g->canvas->h != r->ch) {
        return 0;
    }
    tiles_grid(t, r->cw, r->ch);
    all = !t->valid;
    for (i = 0; i < (long)t->tx * (long)t->ty; i++) {
        t->h1[i] = 0x2545F491U; t->h2[i] = 0x9E3779B9U; t->cut[i] = -1;
    }
    for (ty = 0; ty <= t->ty; ty++) { t->rowstart[ty] = 0; }
    /* hash the calls into the tiles they touch; count the calls per tile row */
    for (i = 0; i < r->ncmd; i++) {
        const lt_dcmd *k = &r->cmd[i];
        int x0 = k->bb.x / TILE_W, x1 = (k->bb.x + k->bb.w - 1) / TILE_W;
        int y0 = k->bb.y / TILE_H, y1 = (k->bb.y + k->bb.h - 1) / TILE_H;
        for (ty = y0; ty <= y1; ty++) {
            size_t row = (size_t)ty * (size_t)t->tx;
            lt_u32 *a = t->h1 + row, *b = t->h2 + row;
            const lt_u32 *pa = t->p1 + row, *pb = t->p2 + row;
            for (tx = x0; tx <= x1; tx++) {
                lt_u32 v = a[tx] ^ k->h1;
                a[tx] = (rotl(v, 7) * 0x2C1B3C6DU + 0x297A2D39U) & LT_U32_MASK;
                b[tx] = ((b[tx] + k->h2) * 0x85EBCA6BU) & LT_U32_MASK;
                b[tx] ^= b[tx] >> 13;
                /* the calls so far are exactly what the tile shows (a menu, tooltip or dialog
                 * opening on top of an unchanged screen only adds calls) */
                if (a[tx] == pa[tx] && b[tx] == pb[tx]) { t->cut[row + (size_t)tx] = i; }
            }
            t->rowstart[ty + 1]++;
            total++;
        }
    }
    for (ty = 0; ty < t->ty; ty++) { t->rowstart[ty + 1] += t->rowstart[ty]; }
    t->rowidx = (long *)grow(t->rowidx, &t->caprowidx, total + 1, sizeof(long));
    {
        long *fill = (long *)lt_malloc(((size_t)t->ty + 1) * sizeof(long));
        for (ty = 0; ty < t->ty; ty++) { fill[ty] = t->rowstart[ty]; }
        for (i = 0; i < r->ncmd; i++) {
            const lt_dcmd *k = &r->cmd[i];
            int y0 = k->bb.y / TILE_H, y1 = (k->bb.y + k->bb.h - 1) / TILE_H;
            for (ty = y0; ty <= y1; ty++) { t->rowidx[fill[ty]++] = i; }
        }
        lt_free(fill);
    }
    /* changed tiles -> horizontal runs per tile row -> rasterize */
    prev_clip = g->clip;
    for (ty = 0; ty < t->ty; ty++) {
        int run0 = -1;
        long run_cut = -1;
        for (tx = 0; tx <= t->tx; tx++) {
            int changed = 0;
            long c = -1;
            if (tx < t->tx) {
                size_t k = (size_t)ty * (size_t)t->tx + (size_t)tx;
                changed = all || t->h1[k] != t->p1[k] || t->h2[k] != t->p2[k];
                c = all ? -1 : t->cut[k];
                if (changed) { ndirty++; }
            }
            /* a run ends at an unchanged tile or where the reusable prefix differs */
            if (run0 >= 0 && (!changed || c != run_cut)) {
                lt_rect rr;
                rr.x = run0 * TILE_W; rr.y = ty * TILE_H;
                rr.w = LT_MIN(tx * TILE_W, r->cw) - rr.x;
                rr.h = LT_MIN((ty + 1) * TILE_H, r->ch) - rr.y;
                render_run(t, g, rr, t->rowidx + t->rowstart[ty], t->rowstart[ty + 1] - t->rowstart[ty],
                           run_cut);
                pixels += (double)rr.w * (double)rr.h;
                if (max_out > 0) { add_out(out, &n_out, max_out, rr); }
                run0 = -1;
            }
            if (changed && run0 < 0) { run0 = tx; run_cut = c; }
        }
    }
    g->clip = prev_clip;
    /* this frame's hashes describe the canvas now */
    {
        lt_u32 *s1 = t->p1, *s2 = t->p2;
        t->p1 = t->h1; t->p2 = t->h2; t->h1 = s1; t->h2 = s2;
    }
    t->valid = 1;
    t->st_dirty = ndirty;
    t->st_pixels = pixels;
    g->dirty = lt_rect_make(0, 0, 0, 0);
    for (i = 0; i < n_out; i++) { lt_rect_union_into(&g->dirty, out[i]); }
    return n_out;
}

long lt_tiles_verify(lt_tiles *t, const lt_gfx *g, int *x, int *y)
{
    struct lt_rec *r = &t->rec;
    lt_gfx s;
    long i, bad = 0;
    int yy, xx;
    *x = *y = -1;
    if (!t->recorded || g->canvas == NULL || g->canvas->w != r->cw || g->canvas->h != r->ch) {
        return 0;
    }
    lt_canvas_resize(&t->scratch, r->cw, r->ch);
    for (yy = 0; yy < r->ch; yy++) {
        memset(t->scratch.px + (size_t)yy * (size_t)t->scratch.pitch, 0, (size_t)r->cw * sizeof(lt_u32));
    }
    lt_gfx_init_canvas(&s, &t->scratch);
    s.ui_pct = g->ui_pct;
    for (i = 0; i < r->ncmd; i++) {
        lt_gfx_replay(&s, r, &r->cmd[i], lt_rect_make(0, 0, r->cw, r->ch));
    }
    for (yy = 0; yy < r->ch; yy++) {
        const lt_u32 *a = t->scratch.px + (size_t)yy * (size_t)t->scratch.pitch;
        const lt_u32 *b = g->canvas->px + (size_t)yy * (size_t)g->canvas->pitch;
        if (memcmp(a, b, (size_t)r->cw * sizeof(lt_u32)) == 0) { continue; }
        for (xx = 0; xx < r->cw; xx++) {
            if (a[xx] != b[xx]) {
                if (bad == 0) { *x = xx; *y = yy; }
                bad++;
            }
        }
    }
    return bad;
}

void lt_tiles_stats(const lt_tiles *t, long *ncmd, int *ntiles, int *ndirty, double *pixels)
{
    if (ncmd != NULL) { *ncmd = t->st_cmd; }
    if (ntiles != NULL) { *ntiles = t->tx * t->ty; }
    if (ndirty != NULL) { *ndirty = t->st_dirty; }
    if (pixels != NULL) { *pixels = t->st_pixels; }
}
