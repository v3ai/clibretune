/* lua_obj.c - interpreter heap: counted allocation, strings, tables, object list.
 * There is no collector: every object is linked into the state's list and freed at the end of a
 * run; the byte budget (max_mem) bounds what a script can allocate.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "lua_priv.h"

#define ARENA_CHUNK 16384u

lt_lv lt_luai_nil(void) { lt_lv v; v.t = LV_NIL; v.u.n = 0.0; return v; }
lt_lv lt_luai_num(double d) { lt_lv v; v.t = LV_NUM; v.u.n = d; return v; }
lt_lv lt_luai_bool(int b) { lt_lv v; v.t = LV_BOOL; v.u.n = 0.0; v.u.b = b != 0; return v; }
lt_lv lt_luai_strv(lt_lstr *s) { lt_lv v; v.t = LV_STR; v.u.n = 0.0; v.u.s = s; return v; }
lt_lv lt_luai_tabv(lt_ltab *t) { lt_lv v; v.t = LV_TAB; v.u.n = 0.0; v.u.tab = t; return v; }

void *lt_luai_alloc(lt_lua *st, size_t n)
{
    void *p;
    if (n > st->max_mem || st->mem > st->max_mem - n) {
        lt_luai_fatal(st, "not enough memory");
    }
    p = lt_malloc(n == 0 ? 1 : n);
    st->mem += n;
    return p;
}

void lt_luai_release(lt_lua *st, void *p, size_t n)
{
    if (p == NULL) { return; }
    lt_free(p);
    st->mem = st->mem >= n ? st->mem - n : 0;
}

/* AST memory: bump allocation in chunks, zeroed */
void *lt_luai_arena(lt_lua *st, size_t n)
{
    lt_lchunk *c = st->arena;
    void *p;
    n = (n + 7u) & ~(size_t)7u;
    if (c == NULL || c->cap - c->used < n) {
        size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        lt_lchunk *nc = (lt_lchunk *)lt_luai_alloc(st, sizeof(lt_lchunk));
        nc->mem = NULL;
        nc->next = st->arena;
        nc->used = 0;
        nc->cap = 0;
        st->arena = nc;   /* linked first so a failing allocation below still frees nc */
        nc->mem = (double *)lt_luai_alloc(st, cap);
        nc->cap = cap;
        c = nc;
    }
    p = (char *)c->mem + c->used;
    memset(p, 0, n);
    c->used += n;
    return p;
}

static void link_obj(lt_lua *st, lt_lobj *o, int kind)
{
    o->kind = kind;
    o->id = ++st->next_id;
    o->prev = NULL;
    o->next = st->objs;
    if (st->objs != NULL) { st->objs->prev = o; }
    st->objs = o;
}

void lt_luai_reset(lt_lua *st)
{
    lt_lobj *o = st->objs;
    lt_lchunk *c = st->arena;
    while (o != NULL) {
        lt_lobj *nx = o->next;
        switch (o->kind) {
        case LO_STR: lt_free(((lt_lstr *)o)->s); break;
        case LO_TAB: lt_free(((lt_ltab *)o)->arr); lt_free(((lt_ltab *)o)->hash); break;
        case LO_FUN: lt_free(((lt_lfun *)o)->ups); break;
        default: break;
        }
        lt_free(o);
        o = nx;
    }
    st->objs = NULL;
    while (c != NULL) {
        lt_lchunk *nx = c->next;
        lt_free(c->mem);
        lt_free(c);
        c = nx;
    }
    st->arena = NULL;
    st->mem = 0;
    st->sp = 0;
    st->depth = 0;
    st->globals = NULL;
    st->slib = NULL;
}

lt_lstr *lt_luai_str_new(lt_lua *st, const char *s, size_t len)
{
    lt_lstr *o;
    if (len > LL_MAX_STR) { lt_luai_fatal(st, "string too large"); }
    o = (lt_lstr *)lt_luai_alloc(st, sizeof(lt_lstr));
    o->s = NULL;
    o->len = 0;
    o->hash = 0;
    o->hashed = 0;
    o->cap = 0;
    link_obj(st, &o->o, LO_STR);
    o->s = (char *)lt_luai_alloc(st, len + 1u);
    o->cap = len + 1u;
    if (len > 0) { memcpy(o->s, s, len); }
    o->s[len] = '\0';
    o->len = len;
    return o;
}

lt_lstr *lt_luai_str_buf(lt_lua *st, size_t len)
{
    lt_lstr *o = lt_luai_str_new(st, "", 0);
    if (len > LL_MAX_STR) { lt_luai_fatal(st, "string too large"); }
    lt_luai_release(st, o->s, o->cap);
    o->s = NULL;
    o->cap = 0;
    o->s = (char *)lt_luai_alloc(st, len + 1u);
    o->cap = len + 1u;
    o->len = len;
    o->s[0] = '\0';
    o->s[len] = '\0';
    return o;
}

lt_lstr *lt_luai_sb_new(lt_lua *st, size_t hint)
{
    lt_lstr *o = lt_luai_str_buf(st, hint < 16 ? 16 : (hint > 4096 ? 4096 : hint));
    o->len = 0;
    return o;
}

void lt_luai_sb_add(lt_lua *st, lt_lstr *sb, const char *data, size_t n)
{
    if (n > LL_MAX_STR || sb->len > LL_MAX_STR - n) { lt_luai_fatal(st, "string too large"); }
    if (sb->len + n + 1u > sb->cap) {
        size_t ncap = sb->cap * 2u;
        char *ns;
        if (ncap < sb->len + n + 1u) { ncap = sb->len + n + 1u; }
        ns = (char *)lt_luai_alloc(st, ncap);
        memcpy(ns, sb->s, sb->len);
        lt_luai_release(st, sb->s, sb->cap);
        sb->s = ns;
        sb->cap = ncap;
    }
    if (n > 0) { memcpy(sb->s + sb->len, data, n); }
    sb->len += n;
    sb->s[sb->len] = '\0';
    sb->hashed = 0;
}

void lt_luai_sb_addc(lt_lua *st, lt_lstr *sb, int c)
{
    char ch = (char)c;
    lt_luai_sb_add(st, sb, &ch, 1);
}

unsigned long lt_luai_str_hash(lt_lstr *s)
{
    if (!s->hashed) {
        unsigned long h = 2166136261ul;
        size_t i;
        for (i = 0; i < s->len; i++) {
            h ^= (unsigned long)(unsigned char)s->s[i];
            h = (h * 16777619ul) & 0xFFFFFFFFul;
        }
        s->hash = h;
        s->hashed = 1;
    }
    return s->hash;
}

lt_ltab *lt_luai_tab_new(lt_lua *st)
{
    lt_ltab *t = (lt_ltab *)lt_luai_alloc(st, sizeof(lt_ltab));
    memset(t, 0, sizeof(*t));
    link_obj(st, &t->o, LO_TAB);
    return t;
}

lt_lcell *lt_luai_cell_new(lt_lua *st, lt_lv v)
{
    lt_lcell *c = (lt_lcell *)lt_luai_alloc(st, sizeof(lt_lcell));
    c->v = v;
    link_obj(st, &c->o, LO_CELL);
    return c;
}

lt_lfun *lt_luai_fun_new(lt_lua *st, lt_lproto *p, int nups)
{
    lt_lfun *f = (lt_lfun *)lt_luai_alloc(st, sizeof(lt_lfun));
    f->p = p;
    f->ups = NULL;
    f->nups = 0;
    link_obj(st, &f->o, LO_FUN);
    if (nups > 0) {
        f->ups = (lt_lcell **)lt_luai_alloc(st, (size_t)nups * sizeof(lt_lcell *));
        memset(f->ups, 0, (size_t)nups * sizeof(lt_lcell *));
        f->nups = nups;
    }
    return f;
}

lt_lcfun *lt_luai_cfun_new(lt_lua *st, int (*fn)(lt_lua *, int, int, void *), void *ud,
                           const char *name)
{
    lt_lcfun *f = (lt_lcfun *)lt_luai_alloc(st, sizeof(lt_lcfun));
    f->fn = fn;
    f->ud = ud;
    f->name = name;
    link_obj(st, &f->o, LO_CFN);
    return f;
}

/* ---- keys ---- */
int lt_luai_raw_equal(lt_lv a, lt_lv b)
{
    if (a.t != b.t) { return 0; }
    switch (a.t) {
    case LV_NIL: return 1;
    case LV_BOOL: return a.u.b == b.u.b;
    case LV_NUM: return a.u.n == b.u.n;
    case LV_STR:
        return a.u.s == b.u.s || (a.u.s->len == b.u.s->len &&
               memcmp(a.u.s->s, b.u.s->s, a.u.s->len) == 0);
    case LV_TAB: return a.u.tab == b.u.tab;
    case LV_FUN: return a.u.f == b.u.f;
    case LV_CFN: return a.u.c == b.u.c;
    default: return a.u.cell == b.u.cell;
    }
}

static unsigned long hash_num(double n)
{
    int e = 0;
    double m = frexp(n, &e);
    long mi = (long)(m * 1073741824.0);
    return ((unsigned long)mi * 2654435761ul) ^ ((unsigned long)(e + 2048) * 40503ul);
}

static unsigned long key_hash(lt_lv k)
{
    switch (k.t) {
    case LV_BOOL: return (unsigned long)k.u.b + 1ul;
    case LV_NUM: return hash_num(k.u.n);
    case LV_STR: return lt_luai_str_hash(k.u.s);
    case LV_TAB: return k.u.tab->o.id * 2654435761ul;
    case LV_FUN: return k.u.f->o.id * 2654435761ul;
    case LV_CFN: return k.u.c->o.id * 2654435761ul;
    default: return 0;
    }
}

/* positive integral number -> array position (1-based) */
static int arr_index(double n, size_t *idx)
{
    if (n >= 1.0 && n <= 2147483647.0 && n == floor(n)) {
        *idx = (size_t)n;
        return 1;
    }
    return 0;
}

static lt_lhnode *hfind(const lt_ltab *t, lt_lv k)
{
    size_t mask, i;
    if (t->hcap == 0) { return NULL; }
    mask = t->hcap - 1u;
    i = (size_t)(key_hash(k) & (unsigned long)mask);
    for (;;) {
        lt_lhnode *nd = &t->hash[i];
        if (nd->k.t == LV_NIL) { return NULL; }
        if (lt_luai_raw_equal(nd->k, k)) { return nd; }
        i = (i + 1u) & mask;
    }
}

lt_lv lt_luai_tab_get(const lt_ltab *t, lt_lv k)
{
    size_t idx = 0;
    lt_lhnode *nd;
    if (k.t == LV_NIL) { return lt_luai_nil(); }
    if (k.t == LV_NUM && arr_index(k.u.n, &idx) && idx <= t->narr) { return t->arr[idx - 1u]; }
    nd = hfind(t, k);
    return nd != NULL ? nd->v : lt_luai_nil();
}

lt_lv lt_luai_tab_getstr(const lt_ltab *t, lt_lstr *k)
{
    return lt_luai_tab_get(t, lt_luai_strv(k));
}

static void arr_grow(lt_lua *st, lt_ltab *t, size_t need)
{
    size_t ncap;
    lt_lv *na;
    if (need <= t->carr) { return; }
    ncap = t->carr < 4 ? 4 : t->carr;
    while (ncap < need) {
        if (ncap > (size_t)1 << 28) { lt_luai_fatal(st, "table overflow"); }
        ncap *= 2u;
    }
    na = (lt_lv *)lt_luai_alloc(st, ncap * sizeof(lt_lv));
    if (t->narr > 0) { memcpy(na, t->arr, t->narr * sizeof(lt_lv)); }
    lt_luai_release(st, t->arr, t->carr * sizeof(lt_lv));
    t->arr = na;
    t->carr = ncap;
}

void lt_luai_tab_reserve(lt_lua *st, lt_ltab *t, size_t n)
{
    if (n > 0 && n < ((size_t)1 << 26)) { arr_grow(st, t, n); }
}

static void hash_rehash(lt_lua *st, lt_ltab *t)
{
    size_t live = 0, i, ncap = 4, mask;
    lt_lhnode *nh;
    for (i = 0; i < t->hcap; i++) {
        if (t->hash[i].k.t != LV_NIL && t->hash[i].v.t != LV_NIL) { live++; }
    }
    while (ncap < (live + 1u) * 2u) { ncap *= 2u; }
    nh = (lt_lhnode *)lt_luai_alloc(st, ncap * sizeof(lt_lhnode));
    memset(nh, 0, ncap * sizeof(lt_lhnode));
    mask = ncap - 1u;
    for (i = 0; i < t->hcap; i++) {
        lt_lhnode *o = &t->hash[i];
        if (o->k.t != LV_NIL && o->v.t != LV_NIL) {
            size_t j = (size_t)(key_hash(o->k) & (unsigned long)mask);
            while (nh[j].k.t != LV_NIL) { j = (j + 1u) & mask; }
            nh[j] = *o;
        }
    }
    lt_luai_release(st, t->hash, t->hcap * sizeof(lt_lhnode));
    t->hash = nh;
    t->hcap = ncap;
    t->hused = live;
}

static void hash_set(lt_lua *st, lt_ltab *t, lt_lv k, lt_lv v)
{
    lt_lhnode *nd = hfind(t, k);
    size_t mask, i;
    if (nd != NULL) { nd->v = v; return; }
    if (v.t == LV_NIL) { return; }
    if (t->hcap == 0 || (t->hused + 1u) * 2u > t->hcap) { hash_rehash(st, t); }
    mask = t->hcap - 1u;
    i = (size_t)(key_hash(k) & (unsigned long)mask);
    while (t->hash[i].k.t != LV_NIL) { i = (i + 1u) & mask; }
    t->hash[i].k = k;
    t->hash[i].v = v;
    t->hused++;
}

void lt_luai_tab_set(lt_lua *st, lt_ltab *t, lt_lv k, lt_lv v)
{
    size_t idx = 0;
    if (k.t == LV_NUM && arr_index(k.u.n, &idx)) {
        if (idx <= t->narr) {
            t->arr[idx - 1u] = v;
            if (v.t == LV_NIL) {
                while (t->narr > 0 && t->arr[t->narr - 1u].t == LV_NIL) { t->narr--; }
            }
            return;
        }
        if (idx == t->narr + 1u) {
            if (v.t == LV_NIL) {
                lt_lhnode *nd = hfind(t, k);
                if (nd != NULL) { nd->v = v; }
                return;
            }
            arr_grow(st, t, idx);
            t->arr[t->narr++] = v;
            {
                lt_lhnode *nd = hfind(t, k);
                if (nd != NULL) { nd->v = lt_luai_nil(); }
            }
            /* migrate the following keys that waited in the hash part */
            while (t->hused > 0) {
                lt_lhnode *nx = hfind(t, lt_luai_num((double)(t->narr + 1u)));
                if (nx == NULL || nx->v.t == LV_NIL) { break; }
                arr_grow(st, t, t->narr + 1u);
                t->arr[t->narr++] = nx->v;
                nx->v = lt_luai_nil();
            }
            return;
        }
    }
    hash_set(st, t, k, v);
}

void lt_luai_tab_append(lt_lua *st, lt_ltab *t, lt_lv v)
{
    lt_lhnode *nd;
    arr_grow(st, t, t->narr + 1u);
    t->arr[t->narr++] = v;
    nd = hfind(t, lt_luai_num((double)t->narr));
    if (nd != NULL) { nd->v = lt_luai_nil(); }
}

void lt_luai_tab_trim(lt_ltab *t)
{
    while (t->narr > 0 && t->arr[t->narr - 1u].t == LV_NIL) { t->narr--; }
}

size_t lt_luai_tab_len(const lt_ltab *t)
{
    return t->narr;
}

int lt_luai_tab_next(const lt_ltab *t, lt_lv *k, lt_lv *v)
{
    size_t pos = 0, idx = 0, i;   /* pos: 0..narr array, then narr+1+slot */
    if (k->t != LV_NIL) {
        if (k->t == LV_NUM && arr_index(k->u.n, &idx) && idx <= t->narr) {
            pos = idx;
        } else {
            lt_lhnode *nd = hfind(t, *k);
            if (nd == NULL) {
                if (k->t == LV_NUM && arr_index(k->u.n, &idx)) { pos = t->narr; }
                else { return -1; }
            } else {
                pos = t->narr + 1u + (size_t)(nd - t->hash);
            }
        }
    }
    for (i = pos; i < t->narr; i++) {
        if (t->arr[i].t != LV_NIL) {
            *k = lt_luai_num((double)(i + 1u));
            *v = t->arr[i];
            return 1;
        }
    }
    i = pos > t->narr ? pos - t->narr - 1u : 0u;
    if (pos > t->narr) { i++; }
    for (; i < t->hcap; i++) {
        if (t->hash[i].k.t != LV_NIL && t->hash[i].v.t != LV_NIL) {
            *k = t->hash[i].k;
            *v = t->hash[i].v;
            return 1;
        }
    }
    return 0;
}
