/* ttf.c - TrueType loader + exact-coverage anti-aliased rasterizer.
 * See ttf.h. Universal C: bounds-checked big-endian reads by byte shifts,
 * no struct overlays, no pointer casts on font data.
 *
 * Rasterizer: the outline is flattened to line segments in pixel space and
 * each segment deposits signed area/cover into an accumulation buffer (the
 * "font-rs" scheme); a running sum along each row then yields the exact
 * fractional pixel coverage, clamped to [0, 1].
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "ttf.h"

#define MAX_PTS 20000
#define MAX_CONTOURS 4000
#define MAX_DEPTH 6
#define MAX_COMPONENTS 64
#define MAX_BMP_PIXELS 1048576L
#define MAX_PX 1000.0
#define KERN_LOOKUPS 16
#define CACHE_SLOTS 768
#define CACHE_BUCKETS 512
#define CACHE_DEFAULT_BYTES 1048576UL
#define KASC_N 96               /* kerning table of the printable ASCII pairs (32..127) */
#define KASC_UNSET (-32768)

static int glyph_index_cmap(const lt_ttf *f, lt_u32 cp);

typedef struct cache_slot {
    lt_u32 ka, kb;      /* key: gid | subpos << 16, size in 1/64 px */
    lt_u8 *data;
    lt_ttf_bitmap bm;
    unsigned long stamp;
    int next;           /* next slot in bucket chain, -1 = end */
    int used;
} cache_slot;

struct lt_ttf {
    const lt_u8 *d;
    size_t n;
    lt_u8 *own;
    /* tables (absolute offsets; 0 = absent) */
    size_t glyf, glyf_len, loca, hmtx, cmap_sub, gpos, kern;
    int cmap_fmt;
    int upem, nglyphs, nhm, loca_long;
    int ascent, descent, line_gap, xh, cap;
    /* Latin-1 code point -> glyph (text measuring and drawing look up every character) */
    int gid_lat1[256];
    /* kerning */
    lt_u8 *kidx;                /* per glyph: 1 + its slot in kasc (an ASCII character), or 0 */
    short *kasc;                /* KASC_N x KASC_N kerning of ASCII pairs, KASC_UNSET = not yet */
    int nkl;
    size_t kl[KERN_LOOKUPS];
    size_t kpairs;
    int nkpairs;
    /* outline scratch (design units -> pixels) */
    double *ox, *oy;
    lt_u8 *on;
    int *oend;
    int np, nc, pcap, ccap;
    /* raster scratch */
    double *acc;
    size_t acc_cap;
    lt_u8 *bmp;
    size_t bmp_cap;
    /* glyph cache */
    cache_slot *slots;
    int buckets[CACHE_BUCKETS];
    int nused;
    size_t cbytes, climit;
    unsigned long clock;
    long hits, misses;
};

/* ------------------------------------------------------------------ */
/* bounds-checked big-endian reads (return 0 out of range)             */
static lt_u32 rd8(const lt_ttf *f, size_t off)
{
    if (off >= f->n) { return 0; }
    return (lt_u32)f->d[off];
}

static lt_u32 rd16(const lt_ttf *f, size_t off)
{
    if (off > f->n || f->n - off < 2) { return 0; }
    return ((lt_u32)f->d[off] << 8) | (lt_u32)f->d[off + 1];
}

static int rds16(const lt_ttf *f, size_t off)
{
    lt_u32 v = rd16(f, off);
    return v >= 0x8000UL ? (int)v - 65536 : (int)v;
}

static lt_u32 rd32(const lt_ttf *f, size_t off)
{
    if (off > f->n || f->n - off < 4) { return 0; }
    return ((lt_u32)f->d[off] << 24) | ((lt_u32)f->d[off + 1] << 16) |
           ((lt_u32)f->d[off + 2] << 8) | (lt_u32)f->d[off + 3];
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
static lt_bool find_table(const lt_ttf *f, const char *tag, size_t *off, size_t *len)
{
    int nt = (int)rd16(f, 4), i;
    for (i = 0; i < nt; i++) {
        size_t rec = 12 + (size_t)i * 16;
        size_t o, l;
        if (rec + 16 > f->n) { return LT_FALSE; }
        if (f->d[rec] == (lt_u8)tag[0] && f->d[rec + 1] == (lt_u8)tag[1] &&
            f->d[rec + 2] == (lt_u8)tag[2] && f->d[rec + 3] == (lt_u8)tag[3]) {
            o = (size_t)rd32(f, rec + 8);
            l = (size_t)rd32(f, rec + 12);
            if (o > f->n || l > f->n - o) { return LT_FALSE; }
            *off = o; *len = l;
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

static void choose_cmap(lt_ttf *f, size_t cm)
{
    int nt = (int)rd16(f, cm + 2), i;
    int best = 0;
    for (i = 0; i < nt && i < 64; i++) {
        size_t rec = cm + 4 + (size_t)i * 8;
        unsigned pid = (unsigned)rd16(f, rec), eid = (unsigned)rd16(f, rec + 2);
        size_t sub = cm + (size_t)rd32(f, rec + 4);
        int fmt, score = 0;
        if (sub >= f->n) { continue; }
        fmt = (int)rd16(f, sub);
        if (fmt != 4 && fmt != 12) { continue; }
        if (pid == 3 && eid == 10) { score = 5; }
        else if (pid == 0 && fmt == 12) { score = 4; }
        else if (pid == 3 && eid == 1) { score = 3; }
        else if (pid == 0) { score = 2; }
        else { score = 1; }
        if (score > best) {
            best = score;
            f->cmap_sub = sub;
            f->cmap_fmt = fmt;
        }
    }
}

static size_t glyph_off(const lt_ttf *f, int gid)
{
    if (f->loca_long) { return (size_t)rd32(f, f->loca + 4 * (size_t)gid); }
    return 2 * (size_t)rd16(f, f->loca + 2 * (size_t)gid);
}

/* range [s, e) of glyph gid inside the file; LT_FALSE when empty/invalid */
static lt_bool glyph_range(const lt_ttf *f, int gid, size_t *s, size_t *e)
{
    size_t a, b;
    if (gid < 0 || gid >= f->nglyphs) { return LT_FALSE; }
    a = glyph_off(f, gid);
    b = glyph_off(f, gid + 1);
    if (b <= a || b > f->glyf_len || b - a < 10) { return LT_FALSE; }
    *s = f->glyf + a;
    *e = f->glyf + b;
    return LT_TRUE;
}

static int measure_top(const lt_ttf *f, lt_u32 cp, int fallback)
{
    int x0, y0, x1, y1;
    if (lt_ttf_glyph_box(f, lt_ttf_glyph_index(f, cp), &x0, &y0, &x1, &y1) && y1 > 0) {
        return y1;
    }
    return fallback;
}

static void find_kern(lt_ttf *f)
{
    size_t kt = 0, kl = 0, fl, ll;
    int nf, i, j;
    f->nkl = 0;
    if (f->gpos != 0) {
        fl = f->gpos + (size_t)rd16(f, f->gpos + 6);
        ll = f->gpos + (size_t)rd16(f, f->gpos + 8);
        nf = (int)rd16(f, fl);
        for (i = 0; i < nf && i < 256; i++) {
            size_t rec = fl + 2 + (size_t)i * 6, ft;
            int nl;
            if (rd32(f, rec) != 0x6B65726EUL) { continue; } /* 'kern' */
            ft = fl + (size_t)rd16(f, rec + 4);
            nl = (int)rd16(f, ft + 2);
            for (j = 0; j < nl && j < 64; j++) {
                int li = (int)rd16(f, ft + 4 + (size_t)j * 2), k, dup = 0;
                size_t lo;
                if (li >= (int)rd16(f, ll)) { continue; }
                lo = ll + (size_t)rd16(f, ll + 2 + (size_t)li * 2);
                for (k = 0; k < f->nkl; k++) {
                    if (f->kl[k] == lo) { dup = 1; }
                }
                if (!dup && f->nkl < KERN_LOOKUPS) { f->kl[f->nkl++] = lo; }
            }
        }
    }
    /* legacy kern table, format 0 first subtable */
    f->kpairs = 0;
    f->nkpairs = 0;
    if (find_table(f, "kern", &kt, &kl) && kl >= 12 && rd16(f, kt) == 0) {
        size_t st = kt + 4;
        if ((rd16(f, st + 4) >> 8) == 0) {
            int np = (int)rd16(f, st + 6);
            if (st + 14 + (size_t)np * 6 <= f->n) {
                f->kpairs = st + 14;
                f->nkpairs = np;
            }
        }
    }
}

lt_ttf *lt_ttf_open(const lt_u8 *data, size_t len, lt_bool copy)
{
    lt_ttf *f;
    size_t o, l, head = 0, hhea = 0, maxp = 0, cm = 0, hmtx_len = 0, loca_len = 0;
    lt_u32 ver;
    if (data == NULL || len < 12 || len > 0x7FFFFFFFUL) {
        lt_err_set("ttf: no data");
        return NULL;
    }
    f = (lt_ttf *)lt_calloc(1, sizeof(*f));
    if (copy) {
        f->own = (lt_u8 *)lt_malloc(len);
        memcpy(f->own, data, len);
        f->d = f->own;
    } else {
        f->d = data;
    }
    f->n = len;
    ver = rd32(f, 0);
    if (ver != 0x00010000UL && ver != 0x74727565UL) { /* 0x00010000 or 'true' */
        lt_err_set("ttf: not a TrueType-outline sfnt");
        goto fail;
    }
    if (!find_table(f, "head", &head, &l) || l < 54 ||
        !find_table(f, "hhea", &hhea, &l) || l < 36 ||
        !find_table(f, "maxp", &maxp, &l) || l < 6 ||
        !find_table(f, "hmtx", &f->hmtx, &hmtx_len) ||
        !find_table(f, "loca", &f->loca, &loca_len) ||
        !find_table(f, "glyf", &f->glyf, &f->glyf_len) ||
        !find_table(f, "cmap", &cm, &l)) {
        lt_err_set("ttf: missing required table");
        goto fail;
    }
    f->upem = (int)rd16(f, head + 18);
    f->loca_long = rds16(f, head + 50) != 0;
    f->nglyphs = (int)rd16(f, maxp + 4);
    f->nhm = (int)rd16(f, hhea + 34);
    f->ascent = rds16(f, hhea + 4);
    f->descent = rds16(f, hhea + 6);
    f->line_gap = rds16(f, hhea + 8);
    if (f->upem < 16 || f->upem > 16384 || f->nglyphs < 1 || f->nhm < 1 ||
        (size_t)f->nhm * 4 > hmtx_len) {
        lt_err_set("ttf: bad metrics");
        goto fail;
    }
    /* loca must hold nglyphs+1 entries */
    o = (size_t)(f->nglyphs + 1) * (f->loca_long ? 4U : 2U);
    if (o > loca_len) {
        lt_err_set("ttf: truncated loca");
        goto fail;
    }
    choose_cmap(f, cm);
    if (f->cmap_sub == 0) {
        lt_err_set("ttf: no usable cmap");
        goto fail;
    }
    if (!find_table(f, "GPOS", &f->gpos, &l)) { f->gpos = 0; }
    find_kern(f);
    {
        lt_u32 cp;
        int i;
        for (cp = 0; cp < 256U; cp++) { f->gid_lat1[cp] = glyph_index_cmap(f, cp); }
        if (f->nglyphs > 0) {
            f->kidx = (lt_u8 *)lt_calloc((size_t)f->nglyphs, 1);
            f->kasc = (short *)lt_malloc((size_t)KASC_N * KASC_N * sizeof(short));
            for (i = 0; i < KASC_N * KASC_N; i++) { f->kasc[i] = KASC_UNSET; }
            for (cp = 32; cp < 32U + KASC_N; cp++) {
                int gid = f->gid_lat1[cp];
                if (gid > 0 && gid < f->nglyphs && f->kidx[gid] == 0) {
                    f->kidx[gid] = (lt_u8)(cp - 31U);
                }
            }
        }
    }
    f->xh = measure_top(f, 'x', f->upem / 2);
    f->cap = measure_top(f, 'H', (f->upem * 7) / 10);
    if (f->ascent <= 0 || f->ascent > f->upem * 4) { f->ascent = (f->upem * 8) / 10; }
    if (f->descent >= 0 || f->descent < -f->upem * 4) { f->descent = -(f->upem * 2) / 10; }
    f->climit = CACHE_DEFAULT_BYTES;
    {
        int i;
        for (i = 0; i < CACHE_BUCKETS; i++) { f->buckets[i] = -1; }
    }
    return f;
fail:
    lt_ttf_close(f);
    return NULL;
}

lt_ttf *lt_ttf_open_chunks(const lt_ttf_chunk *chunks, int count)
{
    size_t total = 0, pos = 0;
    int i;
    lt_u8 *buf;
    lt_ttf *f;
    if (chunks == NULL || count < 1) { return NULL; }
    for (i = 0; i < count; i++) {
        if (!lt_add_size(total, chunks[i].len, &total)) { return NULL; }
    }
    if (total == 0) { return NULL; }
    buf = (lt_u8 *)lt_malloc(total);
    for (i = 0; i < count; i++) {
        if (chunks[i].len > 0) {
            memcpy(buf + pos, chunks[i].data, chunks[i].len);
            pos += chunks[i].len;
        }
    }
    f = lt_ttf_open(buf, total, LT_FALSE);
    if (f == NULL) {
        lt_free(buf);
        return NULL;
    }
    f->own = buf;
    return f;
}

void lt_ttf_cache_clear(lt_ttf *f)
{
    int i;
    if (f->slots != NULL) {
        for (i = 0; i < CACHE_SLOTS; i++) {
            if (f->slots[i].data != NULL) { lt_free(f->slots[i].data); }
            f->slots[i].data = NULL;
            f->slots[i].used = 0;
        }
    }
    for (i = 0; i < CACHE_BUCKETS; i++) { f->buckets[i] = -1; }
    f->nused = 0;
    f->cbytes = 0;
}

void lt_ttf_close(lt_ttf *f)
{
    if (f == NULL) { return; }
    lt_ttf_cache_clear(f);
    lt_free(f->slots);
    lt_free(f->kidx);
    lt_free(f->kasc);
    lt_free(f->ox);
    lt_free(f->oy);
    lt_free(f->on);
    lt_free(f->oend);
    lt_free(f->acc);
    lt_free(f->bmp);
    lt_free(f->own);
    lt_free(f);
}

/* ------------------------------------------------------------------ */
/* metrics                                                             */
int lt_ttf_units_per_em(const lt_ttf *f) { return f->upem; }
int lt_ttf_num_glyphs(const lt_ttf *f) { return f->nglyphs; }
int lt_ttf_ascent(const lt_ttf *f) { return f->ascent; }
int lt_ttf_descent(const lt_ttf *f) { return f->descent; }
int lt_ttf_line_gap(const lt_ttf *f) { return f->line_gap; }
int lt_ttf_x_height(const lt_ttf *f) { return f->xh; }
int lt_ttf_cap_height(const lt_ttf *f) { return f->cap; }

int lt_ttf_advance(const lt_ttf *f, int gid)
{
    if (gid < 0 || gid >= f->nglyphs) { gid = 0; }
    if (gid >= f->nhm) { gid = f->nhm - 1; }
    return (int)rd16(f, f->hmtx + 4 * (size_t)gid);
}

lt_bool lt_ttf_glyph_box(const lt_ttf *f, int gid, int *x0, int *y0, int *x1, int *y1)
{
    size_t s, e;
    if (!glyph_range(f, gid, &s, &e)) { return LT_FALSE; }
    *x0 = rds16(f, s + 2); *y0 = rds16(f, s + 4);
    *x1 = rds16(f, s + 6); *y1 = rds16(f, s + 8);
    return LT_TRUE;
}

lt_bool lt_ttf_glyph_composite(const lt_ttf *f, int gid)
{
    size_t s, e;
    if (!glyph_range(f, gid, &s, &e)) { return LT_FALSE; }
    return rds16(f, s) < 0;
}

/* ------------------------------------------------------------------ */
/* cmap                                                                */
static int glyph_index_cmap(const lt_ttf *f, lt_u32 cp)
{
    size_t t = f->cmap_sub;
    int gid = 0;
    if (t == 0) { return 0; }
    if (f->cmap_fmt == 4) {
        int segs = (int)(rd16(f, t + 6) / 2), lo = 0, hi, seg;
        size_t ends, starts, deltas, ranges;
        lt_u32 start, end, delta, ro;
        if (cp > 0xFFFFUL || segs < 1) { return 0; }
        ends = t + 14;
        starts = ends + (size_t)segs * 2 + 2;
        deltas = starts + (size_t)segs * 2;
        ranges = deltas + (size_t)segs * 2;
        hi = segs - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (rd16(f, ends + (size_t)mid * 2) < cp) { lo = mid + 1; } else { hi = mid; }
        }
        seg = lo;
        end = rd16(f, ends + (size_t)seg * 2);
        start = rd16(f, starts + (size_t)seg * 2);
        if (cp < start || cp > end) { return 0; }
        delta = rd16(f, deltas + (size_t)seg * 2);
        ro = rd16(f, ranges + (size_t)seg * 2);
        if (ro == 0) {
            gid = (int)((cp + delta) & 0xFFFFUL);
        } else {
            lt_u32 g = rd16(f, ranges + (size_t)seg * 2 + (size_t)ro + (size_t)(cp - start) * 2);
            if (g == 0) { return 0; }
            gid = (int)((g + delta) & 0xFFFFUL);
        }
    } else if (f->cmap_fmt == 12) {
        lt_u32 ng = rd32(f, t + 12);
        lt_u32 lo = 0, hi;
        if (ng == 0 || ng > 0x100000UL) { return 0; }
        hi = ng - 1;
        while (lo < hi) {
            lt_u32 mid = lo + (hi - lo) / 2;
            size_t g = t + 16 + (size_t)mid * 12;
            if (rd32(f, g + 4) < cp) { lo = mid + 1; } else { hi = mid; }
        }
        {
            size_t g = t + 16 + (size_t)lo * 12;
            lt_u32 s = rd32(f, g), e = rd32(f, g + 4), sg = rd32(f, g + 8);
            if (cp < s || cp > e) { return 0; }
            if (sg > 0xFFFFUL || cp - s > 0xFFFFUL) { return 0; }
            gid = (int)(sg + (cp - s));
        }
    }
    if (gid < 0 || gid >= f->nglyphs) { return 0; }
    return gid;
}

/* ------------------------------------------------------------------ */
/* kerning                                                             */
static int popcount16(lt_u32 v)
{
    int n = 0;
    while (v != 0) { n += (int)(v & 1U); v >>= 1; }
    return n;
}

/* coverage index of gid or -1 */
static int coverage_index(const lt_ttf *f, size_t cov, int gid)
{
    lt_u32 fmt = rd16(f, cov);
    if (fmt == 1) {
        int n = (int)rd16(f, cov + 2), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            int g = (int)rd16(f, cov + 4 + (size_t)mid * 2);
            if (g == gid) { return mid; }
            if (g < gid) { lo = mid + 1; } else { hi = mid - 1; }
        }
    } else if (fmt == 2) {
        int n = (int)rd16(f, cov + 2), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            size_t r = cov + 4 + (size_t)mid * 6;
            int s = (int)rd16(f, r), e = (int)rd16(f, r + 2);
            if (gid < s) { hi = mid - 1; }
            else if (gid > e) { lo = mid + 1; }
            else { return (int)rd16(f, r + 4) + (gid - s); }
        }
    }
    return -1;
}

static int class_of(const lt_ttf *f, size_t cd, int gid)
{
    lt_u32 fmt = rd16(f, cd);
    if (fmt == 1) {
        int start = (int)rd16(f, cd + 2), n = (int)rd16(f, cd + 4);
        if (gid >= start && gid - start < n) {
            return (int)rd16(f, cd + 6 + (size_t)(gid - start) * 2);
        }
    } else if (fmt == 2) {
        int n = (int)rd16(f, cd + 2), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            size_t r = cd + 4 + (size_t)mid * 6;
            int s = (int)rd16(f, r), e = (int)rd16(f, r + 2);
            if (gid < s) { hi = mid - 1; }
            else if (gid > e) { lo = mid + 1; }
            else { return (int)rd16(f, r + 4); }
        }
    }
    return 0;
}

/* xAdvance of ValueRecord at off with format vf */
static int value_xadv(const lt_ttf *f, size_t off, lt_u32 vf)
{
    if ((vf & 0x0004UL) == 0) { return 0; }
    return rds16(f, off + 2 * (size_t)popcount16(vf & 0x0003UL));
}

/* returns LT_TRUE and sets *adv if this PairPos subtable decides the pair */
static lt_bool pairpos_lookup(const lt_ttf *f, size_t st, int l, int r, int *adv)
{
    lt_u32 fmt = rd16(f, st), vf1 = rd16(f, st + 4), vf2 = rd16(f, st + 6);
    size_t cov = st + (size_t)rd16(f, st + 2);
    int ci = coverage_index(f, cov, l);
    size_t sz1 = 2 * (size_t)popcount16(vf1), sz2 = 2 * (size_t)popcount16(vf2);
    if (ci < 0) { return LT_FALSE; }
    if (fmt == 1) {
        int npairs, lo, hi;
        size_t ps, rec = 2 + sz1 + sz2;
        if (ci >= (int)rd16(f, st + 8)) { return LT_FALSE; }
        ps = st + (size_t)rd16(f, st + 10 + (size_t)ci * 2);
        npairs = (int)rd16(f, ps);
        lo = 0; hi = npairs - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            size_t pr = ps + 2 + (size_t)mid * rec;
            int g = (int)rd16(f, pr);
            if (g == r) { *adv = value_xadv(f, pr + 2, vf1); return LT_TRUE; }
            if (g < r) { lo = mid + 1; } else { hi = mid - 1; }
        }
        return LT_FALSE;
    }
    if (fmt == 2) {
        size_t cd1 = st + (size_t)rd16(f, st + 8), cd2 = st + (size_t)rd16(f, st + 10);
        int n1 = (int)rd16(f, st + 12), n2 = (int)rd16(f, st + 14);
        int c1 = class_of(f, cd1, l), c2 = class_of(f, cd2, r);
        size_t rec;
        if (c1 >= n1 || c2 >= n2) { return LT_FALSE; }
        rec = sz1 + sz2;
        *adv = value_xadv(f, st + 16 + ((size_t)c1 * (size_t)n2 + (size_t)c2) * rec, vf1);
        return LT_TRUE;
    }
    return LT_FALSE;
}

int lt_ttf_glyph_index(const lt_ttf *f, lt_u32 cp)
{
    if (cp < 256U) { return f->gid_lat1[cp]; }
    return glyph_index_cmap(f, cp);
}

static int kern_lookup(const lt_ttf *f, int left, int right);

int lt_ttf_kern(const lt_ttf *f, int left, int right)
{
    if (left > 0 && right > 0 && left < f->nglyphs && right < f->nglyphs && f->kasc != NULL) {
        int ia = f->kidx[left], ib = f->kidx[right];
        if (ia > 0 && ib > 0) {
            short *v = &f->kasc[(ia - 1) * KASC_N + (ib - 1)];
            if (*v == KASC_UNSET) {
                int k = kern_lookup(f, left, right);
                if (k <= KASC_UNSET || k > 32767) { return k; }
                *v = (short)k;
            }
            return *v;
        }
    }
    return kern_lookup(f, left, right);
}

static int kern_lookup(const lt_ttf *f, int left, int right)
{
    int i, sum = 0;
    if (left <= 0 || right <= 0) { return 0; }
    /* each 'kern' lookup is applied in turn; within a lookup the first
     * subtable that decides the pair wins; adjustments add up */
    for (i = 0; i < f->nkl; i++) {
        size_t lo = f->kl[i];
        int ns = (int)rd16(f, lo + 4), j;
        for (j = 0; j < ns && j < 64; j++) {
            size_t st = lo + (size_t)rd16(f, lo + 6 + (size_t)j * 2);
            int type = (int)rd16(f, lo), adv = 0;
            if (type == 9) {
                if (rd16(f, st) != 1 || rd16(f, st + 2) != 2) { continue; }
                st += (size_t)rd32(f, st + 4);
            } else if (type != 2) {
                continue;
            }
            if (pairpos_lookup(f, st, left, right, &adv)) {
                sum += adv;
                break;
            }
        }
    }
    if (f->nkl > 0) { return sum; }
    if (f->nkpairs > 0) {
        lt_u32 key = ((lt_u32)left << 16) | (lt_u32)right;
        int lo = 0, hi = f->nkpairs - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            size_t p = f->kpairs + (size_t)mid * 6;
            lt_u32 k = rd32(f, p);
            if (k == key) { return rds16(f, p + 4); }
            if (k < key) { lo = mid + 1; } else { hi = mid - 1; }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* outline decoding                                                    */
static lt_bool ol_reserve(lt_ttf *f, int addp, int addc)
{
    int need;
    if (addp < 0 || addc < 0 || f->np + addp > MAX_PTS || f->nc + addc > MAX_CONTOURS) {
        return LT_FALSE;
    }
    need = f->np + addp;
    if (need > f->pcap) {
        int cap = f->pcap < 256 ? 256 : f->pcap;
        double *nx, *ny;
        lt_u8 *no;
        while (cap < need) { cap *= 2; }
        nx = (double *)lt_realloc(f->ox, (size_t)cap * sizeof(double));
        f->ox = nx;
        ny = (double *)lt_realloc(f->oy, (size_t)cap * sizeof(double));
        f->oy = ny;
        no = (lt_u8 *)lt_realloc(f->on, (size_t)cap);
        f->on = no;
        f->pcap = cap;
    }
    need = f->nc + addc;
    if (need > f->ccap) {
        int cap = f->ccap < 32 ? 32 : f->ccap;
        int *ne;
        while (cap < need) { cap *= 2; }
        ne = (int *)lt_realloc(f->oend, (size_t)cap * sizeof(int));
        f->oend = ne;
        f->ccap = cap;
    }
    return LT_TRUE;
}

static double f2dot14(const lt_ttf *f, size_t off)
{
    return (double)rds16(f, off) / 16384.0;
}

/* Append glyph gid transformed by m[6] (X = m0 u + m2 v + m4, Y = m1 u +
 * m3 v + m5). Returns LT_FALSE on malformed data; the outline is rolled
 * back to its state on entry in that case. */
static lt_bool load_glyph(lt_ttf *f, int gid, const double *m, int depth, int *budget)
{
    size_t s, e, p;
    int nc, np0 = f->np, nc0 = f->nc;
    if (!glyph_range(f, gid, &s, &e)) { return LT_TRUE; } /* blank glyph is fine */
    if (depth > MAX_DEPTH) { return LT_FALSE; }
    nc = rds16(f, s);
    if (nc > 0) {
        int npts, i, prev = -1;
        size_t ilen;
        lt_u8 *fl;
        double v;
        p = s + 10;
        if (p + (size_t)nc * 2 + 2 > e) { return LT_FALSE; }
        npts = (int)rd16(f, p + (size_t)(nc - 1) * 2) + 1;
        if (!ol_reserve(f, npts, nc)) { return LT_FALSE; }
        for (i = 0; i < nc; i++) {
            int end = (int)rd16(f, p + (size_t)i * 2);
            if (end < prev || end >= npts) { return LT_FALSE; }
            prev = end;
            f->oend[f->nc + i] = f->np + end;
        }
        p += (size_t)nc * 2;
        ilen = (size_t)rd16(f, p);
        p += 2 + ilen;
        fl = f->on + f->np;
        i = 0;
        while (i < npts) {
            lt_u8 flag;
            if (p >= e) { return LT_FALSE; }
            flag = (lt_u8)rd8(f, p++);
            fl[i++] = flag;
            if ((flag & 8U) != 0) {
                int rep;
                if (p >= e) { return LT_FALSE; }
                rep = (int)rd8(f, p++);
                while (rep-- > 0 && i < npts) { fl[i++] = flag; }
            }
        }
        v = 0.0;
        for (i = 0; i < npts; i++) {
            lt_u8 flag = fl[i];
            if ((flag & 2U) != 0) {
                int dv;
                if (p >= e) { return LT_FALSE; }
                dv = (int)rd8(f, p++);
                v += (flag & 16U) != 0 ? (double)dv : (double)-dv;
            } else if ((flag & 16U) == 0) {
                if (p + 2 > e) { return LT_FALSE; }
                v += (double)rds16(f, p);
                p += 2;
            }
            f->ox[f->np + i] = v;
        }
        v = 0.0;
        for (i = 0; i < npts; i++) {
            lt_u8 flag = fl[i];
            if ((flag & 4U) != 0) {
                int dv;
                if (p >= e) { return LT_FALSE; }
                dv = (int)rd8(f, p++);
                v += (flag & 32U) != 0 ? (double)dv : (double)-dv;
            } else if ((flag & 32U) == 0) {
                if (p + 2 > e) { return LT_FALSE; }
                v += (double)rds16(f, p);
                p += 2;
            }
            f->oy[f->np + i] = v;
        }
        for (i = 0; i < npts; i++) {
            double u = f->ox[f->np + i], w = f->oy[f->np + i];
            f->ox[f->np + i] = m[0] * u + m[2] * w + m[4];
            f->oy[f->np + i] = m[1] * u + m[3] * w + m[5];
            f->on[f->np + i] = (lt_u8)(fl[i] & 1U);
        }
        f->np += npts;
        f->nc += nc;
        return LT_TRUE;
    }
    if (nc == 0) { return LT_TRUE; }
    /* composite */
    p = s + 10;
    for (;;) {
        lt_u32 flags;
        int cg, a1, a2;
        double ca = 1.0, cb = 0.0, cc = 0.0, cd = 1.0, dx = 0.0, dy = 0.0;
        double nm[6];
        if (p + 4 > e || *budget <= 0) { goto bad; }
        (*budget)--;
        flags = rd16(f, p);
        cg = (int)rd16(f, p + 2);
        p += 4;
        if ((flags & 1UL) != 0) {
            if (p + 4 > e) { goto bad; }
            a1 = rds16(f, p); a2 = rds16(f, p + 2);
            p += 4;
        } else {
            lt_u32 b1, b2;
            if (p + 2 > e) { goto bad; }
            b1 = rd8(f, p); b2 = rd8(f, p + 1);
            a1 = b1 >= 128UL ? (int)b1 - 256 : (int)b1;
            a2 = b2 >= 128UL ? (int)b2 - 256 : (int)b2;
            p += 2;
        }
        if ((flags & 2UL) != 0) { dx = (double)a1; dy = (double)a2; }
        if ((flags & 8UL) != 0) {
            if (p + 2 > e) { goto bad; }
            ca = cd = f2dot14(f, p);
            p += 2;
        } else if ((flags & 0x40UL) != 0) {
            if (p + 4 > e) { goto bad; }
            ca = f2dot14(f, p); cd = f2dot14(f, p + 2);
            p += 4;
        } else if ((flags & 0x80UL) != 0) {
            if (p + 8 > e) { goto bad; }
            ca = f2dot14(f, p); cb = f2dot14(f, p + 2);
            cc = f2dot14(f, p + 4); cd = f2dot14(f, p + 6);
            p += 8;
        }
        nm[0] = m[0] * ca + m[2] * cb;
        nm[1] = m[1] * ca + m[3] * cb;
        nm[2] = m[0] * cc + m[2] * cd;
        nm[3] = m[1] * cc + m[3] * cd;
        nm[4] = m[0] * dx + m[2] * dy + m[4];
        nm[5] = m[1] * dx + m[3] * dy + m[5];
        if (!load_glyph(f, cg, nm, depth + 1, budget)) { goto bad; }
        if ((flags & 0x20UL) == 0) { break; }
    }
    return LT_TRUE;
bad:
    f->np = np0;
    f->nc = nc0;
    return LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* coverage accumulation                                               */
static void acc_line(double *a, int sw, int h, double x0, double y0, double x1, double y1)
{
    double dir = 1.0, dxdy, x, ytop, ybot;
    int yi, yend;
    if (y0 == y1) { return; }
    if (y0 > y1) {
        double t;
        dir = -1.0;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    dxdy = (x1 - x0) / (y1 - y0);
    x = x0;
    ytop = y0;
    ybot = y1;
    if (ytop < 0.0) { x -= ytop * dxdy; ytop = 0.0; }
    if (ybot > (double)h) { ybot = (double)h; }
    if (ytop >= ybot) { return; }
    yi = (int)ytop;
    yend = (int)ceil(ybot);
    if (yend > h) { yend = h; }
    for (; yi < yend; yi++) {
        double *row = a + (size_t)yi * (size_t)sw;
        double lo = (double)yi > ytop ? (double)yi : ytop;
        double hi = (double)(yi + 1) < ybot ? (double)(yi + 1) : ybot;
        double dy = hi - lo, xn = x + dxdy * dy, d = dy * dir;
        double xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        double x0f, x1c;
        int x0i, x1i;
        if (xa < 0.0) { xa = 0.0; }
        if (xb < 0.0) { xb = 0.0; }
        if (xa > (double)(sw - 2)) { xa = (double)(sw - 2); }
        if (xb > (double)(sw - 2)) { xb = (double)(sw - 2); }
        x0f = floor(xa); x1c = ceil(xb);
        x0i = (int)x0f; x1i = (int)x1c;
        if (x1i <= x0i + 1) {
            double xmf = 0.5 * (xa + xb) - x0f;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            double s = 1.0 / (xb - xa), x0r = xa - x0f;
            double a0 = 0.5 * s * (1.0 - x0r) * (1.0 - x0r);
            double x1r = xb - x1c + 1.0;
            double am = 0.5 * s * x1r * x1r;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0 - a0 - am);
            } else {
                double a1 = s * (1.5 - x0r), a2;
                int xi;
                row[x0i + 1] += d * (a1 - a0);
                for (xi = x0i + 2; xi < x1i - 1; xi++) { row[xi] += d * s; }
                a2 = a1 + (double)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0 - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xn;
    }
}

/* flatten one quadratic bezier (already in bitmap-relative pixels) */
static void acc_quad(double *a, int sw, int h, double x0, double y0, double x1, double y1,
                     double x2, double y2)
{
    double ddx = x0 - 2.0 * x1 + x2, ddy = y0 - 2.0 * y1 + y2;
    double dd = sqrt(ddx * ddx + ddy * ddy);
    int n = (int)sqrt(dd * 12.0) + 1, i;
    double px = x0, py = y0;
    if (n > 64) { n = 64; }
    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n, mt = 1.0 - t;
        double qx = mt * mt * x0 + 2.0 * mt * t * x1 + t * t * x2;
        double qy = mt * mt * y0 + 2.0 * mt * t * y1 + t * t * y2;
        acc_line(a, sw, h, px, py, qx, qy);
        px = qx; py = qy;
    }
}

/* walk one contour [first, last] and emit segments relative to (bx, by) */
static void emit_contour(const lt_ttf *f, double *a, int sw, int h, int first, int last,
                         double bx, double by)
{
    int n = last - first + 1, i, start_i;
    double sx, sy, cx, cy;      /* contour start, current point */
    double qx = 0.0, qy = 0.0;  /* pending off-curve control point */
    int have_q = 0, cnt;
    const double *ox = f->ox + first, *oy = f->oy + first;
    const lt_u8 *on = f->on + first;
    if (n < 2) { return; }
    if (on[0]) {
        sx = ox[0] - bx; sy = oy[0] - by; start_i = 1;
    } else if (on[n - 1]) {
        sx = ox[n - 1] - bx; sy = oy[n - 1] - by; start_i = 0; n--;
    } else {
        sx = 0.5 * (ox[0] + ox[n - 1]) - bx;
        sy = 0.5 * (oy[0] + oy[n - 1]) - by;
        start_i = 0;
    }
    cx = sx; cy = sy;
    cnt = (on[0] ? n - 1 : n);
    for (i = 0; i < cnt; i++) {
        int k = start_i + i;
        double px = ox[k] - bx, py = oy[k] - by;
        if (on[k]) {
            if (have_q) {
                acc_quad(a, sw, h, cx, cy, qx, qy, px, py);
                have_q = 0;
            } else {
                acc_line(a, sw, h, cx, cy, px, py);
            }
            cx = px; cy = py;
        } else {
            if (have_q) {
                double mx = 0.5 * (qx + px), my = 0.5 * (qy + py);
                acc_quad(a, sw, h, cx, cy, qx, qy, mx, my);
                cx = mx; cy = my;
            }
            qx = px; qy = py; have_q = 1;
        }
    }
    if (have_q) {
        acc_quad(a, sw, h, cx, cy, qx, qy, sx, sy);
    } else {
        acc_line(a, sw, h, cx, cy, sx, sy);
    }
}

lt_bool lt_ttf_raster(lt_ttf *f, int gid, const double m[4], double fx, double fy,
                      lt_ttf_bitmap *out)
{
    double mm[6], minx, miny, maxx, maxy, bx, by;
    int budget = MAX_COMPONENTS, i, c, w, h, sw, first;
    size_t cells, y, x;
    out->a = NULL; out->w = out->h = 0; out->xoff = out->yoff = 0;
    mm[0] = m[0]; mm[1] = m[1]; mm[2] = m[2]; mm[3] = m[3]; mm[4] = fx; mm[5] = fy;
    f->np = 0; f->nc = 0;
    if (!load_glyph(f, gid, mm, 0, &budget) || f->np < 2 || f->nc < 1) { return LT_FALSE; }
    minx = maxx = f->ox[0];
    miny = maxy = f->oy[0];
    for (i = 1; i < f->np; i++) {
        if (f->ox[i] < minx) { minx = f->ox[i]; }
        if (f->ox[i] > maxx) { maxx = f->ox[i]; }
        if (f->oy[i] < miny) { miny = f->oy[i]; }
        if (f->oy[i] > maxy) { maxy = f->oy[i]; }
    }
    if (!(maxx - minx < 4096.0) || !(maxy - miny < 4096.0) ||
        !(fabs(minx) < 1.0e6) || !(fabs(miny) < 1.0e6) ||
        !(fabs(maxx) < 1.0e6) || !(fabs(maxy) < 1.0e6)) {
        return LT_FALSE;
    }
    bx = floor(minx);
    by = floor(miny);
    w = (int)(ceil(maxx) - bx);
    h = (int)(ceil(maxy) - by);
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    if ((long)w * (long)h > MAX_BMP_PIXELS) { return LT_FALSE; }
    sw = w + 2;
    cells = (size_t)sw * (size_t)h;
    if (cells > f->acc_cap) {
        double *na = (double *)lt_realloc(f->acc, cells * sizeof(double));
        f->acc = na;
        f->acc_cap = cells;
    }
    if ((size_t)w * (size_t)h > f->bmp_cap) {
        lt_u8 *nb = (lt_u8 *)lt_realloc(f->bmp, (size_t)w * (size_t)h);
        f->bmp = nb;
        f->bmp_cap = (size_t)w * (size_t)h;
    }
    memset(f->acc, 0, cells * sizeof(double));
    first = 0;
    for (c = 0; c < f->nc; c++) {
        emit_contour(f, f->acc, sw, h, first, f->oend[c], bx, by);
        first = f->oend[c] + 1;
    }
    for (y = 0; y < (size_t)h; y++) {
        const double *row = f->acc + y * (size_t)sw;
        lt_u8 *dst = f->bmp + y * (size_t)w;
        double run = 0.0;
        for (x = 0; x < (size_t)w; x++) {
            double v;
            run += row[x];
            v = run < 0.0 ? -run : run;
            if (v > 1.0) { v = 1.0; }
            dst[x] = (lt_u8)(int)(v * 255.0 + 0.5);
        }
    }
    out->a = f->bmp;
    out->w = w; out->h = h;
    out->xoff = (int)bx; out->yoff = (int)by;
    return LT_TRUE;
}

/* ------------------------------------------------------------------ */
/* scaling and cache                                                   */
void lt_ttf_scale_for(const lt_ttf *f, double px, double *sx, double *sy)
{
    double s = px / (double)f->upem, xh = (double)f->xh * s, r = 1.0;
    if (px >= 6.0 && xh >= 3.0) {
        r = floor(xh + 0.5) / xh;
        if (r < 0.88) { r = 0.88; }
        if (r > 1.12) { r = 1.12; }
    }
    *sx = s;
    *sy = s * r;
}

void lt_ttf_cache_limit(lt_ttf *f, size_t max_bytes)
{
    f->climit = max_bytes;
}

void lt_ttf_cache_stats(const lt_ttf *f, int *entries, size_t *bytes, long *hits, long *misses)
{
    if (entries != NULL) { *entries = f->nused; }
    if (bytes != NULL) { *bytes = f->cbytes; }
    if (hits != NULL) { *hits = f->hits; }
    if (misses != NULL) { *misses = f->misses; }
}

static int bucket_of(lt_u32 ka, lt_u32 kb)
{
    lt_u32 h = ((ka * 2654435761UL) ^ (kb * 40503UL)) & LT_U32_MASK;
    return (int)((h >> 7) % CACHE_BUCKETS);
}

static void cache_evict(lt_ttf *f, int i)
{
    cache_slot *sl = &f->slots[i];
    int b = bucket_of(sl->ka, sl->kb), *link = &f->buckets[b];
    while (*link >= 0 && *link != i) { link = &f->slots[*link].next; }
    if (*link == i) { *link = sl->next; }
    if (sl->data != NULL) {
        f->cbytes -= (size_t)sl->bm.w * (size_t)sl->bm.h;
        lt_free(sl->data);
    }
    sl->data = NULL;
    sl->used = 0;
    f->nused--;
}

static void cache_evict_oldest(lt_ttf *f)
{
    int i, best = -1;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (f->slots[i].used && (best < 0 || f->slots[i].stamp < f->slots[best].stamp)) {
            best = i;
        }
    }
    if (best >= 0) { cache_evict(f, best); }
}

const lt_ttf_bitmap *lt_ttf_glyph(lt_ttf *f, int gid, double px, int subpos)
{
    lt_u32 ka, kb;
    int b, i, slot = -1;
    double size, sx, sy, m[4];
    lt_ttf_bitmap bm;
    size_t need;
    if (!(px >= 1.0) || px > MAX_PX) { return NULL; }
    if (subpos < 0) { subpos = 0; }
    if (subpos >= LT_TTF_SUBPOS) { subpos = LT_TTF_SUBPOS - 1; }
    if (gid < 0 || gid >= f->nglyphs) { gid = 0; }
    kb = (lt_u32)(px * 64.0 + 0.5);
    ka = (lt_u32)gid | ((lt_u32)subpos << 16);
    if (f->slots == NULL) {
        f->slots = (cache_slot *)lt_calloc(CACHE_SLOTS, sizeof(cache_slot));
    }
    f->clock++;
    b = bucket_of(ka, kb);
    for (i = f->buckets[b]; i >= 0; i = f->slots[i].next) {
        if (f->slots[i].ka == ka && f->slots[i].kb == kb) {
            f->slots[i].stamp = f->clock;
            f->hits++;
            return f->slots[i].bm.a != NULL ? &f->slots[i].bm : NULL;
        }
    }
    f->misses++;
    size = (double)kb / 64.0;
    lt_ttf_scale_for(f, size, &sx, &sy);
    m[0] = sx; m[1] = 0.0; m[2] = 0.0; m[3] = -sy;
    if (!lt_ttf_raster(f, gid, m, (double)subpos / (double)LT_TTF_SUBPOS, 0.0, &bm)) {
        bm.a = NULL; bm.w = bm.h = 0; bm.xoff = bm.yoff = 0;
    }
    need = (size_t)bm.w * (size_t)bm.h;
    while (f->nused >= CACHE_SLOTS || (f->nused > 0 && f->cbytes + need > f->climit)) {
        cache_evict_oldest(f);
    }
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (!f->slots[i].used) { slot = i; break; }
    }
    if (slot < 0) { return NULL; }
    {
        cache_slot *sl = &f->slots[slot];
        sl->ka = ka; sl->kb = kb;
        sl->bm = bm;
        sl->data = NULL;
        if (need > 0) {
            sl->data = (lt_u8 *)lt_malloc(need);
            memcpy(sl->data, bm.a, need);
            sl->bm.a = sl->data;
            f->cbytes += need;
        } else {
            sl->bm.a = NULL;
        }
        sl->stamp = f->clock;
        sl->used = 1;
        sl->next = f->buckets[b];
        f->buckets[b] = slot;
        f->nused++;
        return sl->bm.a != NULL ? &sl->bm : NULL;
    }
}
