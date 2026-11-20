/* proto_mem.c - ECU memory shadow with per-page state and per-byte dirty bits.
 * Spec 03 section 13 and spec 04 section 2.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "proto.h"

#include <string.h>

typedef struct mem_page {
    unsigned long declared;  /* declared page size */
    lt_u8        *data;      /* image (len bytes) when loaded */
    size_t        len;
    lt_u8        *dirty;     /* bitmap, ndirty_cap bytes */
    size_t        dirty_cap;
    size_t        ndirty;    /* number of dirty bytes */
    lt_page_state state;
    int           pend;      /* written to RAM, not burned */
} mem_page;

struct lt_ecu_mem {
    mem_page *pages;
    size_t    n;
};

lt_ecu_mem *lt_ecu_mem_new(const unsigned long *page_sizes, size_t n_pages)
{
    lt_ecu_mem *m = (lt_ecu_mem *)lt_calloc(1, sizeof(*m));
    size_t i;

    m->n = n_pages;
    if (n_pages > 0) {
        m->pages = (mem_page *)lt_calloc(n_pages, sizeof(mem_page));
        for (i = 0; i < n_pages; i++) {
            m->pages[i].declared = page_sizes ? page_sizes[i] : 0;
            m->pages[i].state = LT_PAGE_NOT_LOADED;
        }
    }
    return m;
}

lt_ecu_mem *lt_ecu_mem_from_ini(const lt_ini *ini)
{
    size_t n = ini ? ini->proto.n_page_sizes : 0;

    if (n == 0) {
        return lt_ecu_mem_new(NULL, 0);
    }
    return lt_ecu_mem_new(ini->proto.page_sizes, n);
}

void lt_ecu_mem_free(lt_ecu_mem *m)
{
    size_t i;

    if (!m) {
        return;
    }
    for (i = 0; i < m->n; i++) {
        lt_free(m->pages[i].data);
        lt_free(m->pages[i].dirty);
    }
    lt_free(m->pages);
    lt_free(m);
}

static mem_page *pg(const lt_ecu_mem *m, int page)
{
    if (!m || page < 0 || (size_t)page >= m->n) {
        return NULL;
    }
    return &m->pages[page];
}

static lt_bool loaded(const mem_page *p)
{
    return (p->state == LT_PAGE_CLEAN || p->state == LT_PAGE_DIRTY ||
            p->state == LT_PAGE_PENDING) ? LT_TRUE : LT_FALSE;
}

static void ensure_bitmap(mem_page *p, size_t bytes)
{
    size_t need = (bytes + 7) / 8;

    if (need > p->dirty_cap) {
        lt_u8 *nb = (lt_u8 *)lt_calloc(need, 1);

        if (p->dirty_cap > 0) {
            memcpy(nb, p->dirty, p->dirty_cap);
        }
        lt_free(p->dirty);
        p->dirty = nb;
        p->dirty_cap = need;
    }
}

static void clear_bits(mem_page *p)
{
    if (p->dirty_cap > 0) {
        memset(p->dirty, 0, p->dirty_cap);
    }
    p->ndirty = 0;
}

size_t lt_ecu_mem_page_count(const lt_ecu_mem *m)
{
    return m ? m->n : 0;
}

size_t lt_ecu_mem_page_size(const lt_ecu_mem *m, int page)
{
    mem_page *p = pg(m, page);

    return p ? (size_t)p->declared : 0;
}

lt_page_state lt_ecu_mem_state(const lt_ecu_mem *m, int page)
{
    mem_page *p = pg(m, page);

    return p ? p->state : LT_PAGE_NOT_LOADED;
}

void lt_ecu_mem_set_state(lt_ecu_mem *m, int page, lt_page_state s)
{
    mem_page *p = pg(m, page);

    if (p) {
        p->state = s;
    }
}

void lt_ecu_mem_load_page(lt_ecu_mem *m, int page, const lt_u8 *data, size_t len)
{
    mem_page *p = pg(m, page);

    if (!p) {
        return;
    }
    lt_free(p->data);
    p->data = (lt_u8 *)lt_malloc(len > 0 ? len : 1);
    if (len > 0) {
        memcpy(p->data, data, len);
    }
    p->len = len;
    ensure_bitmap(p, len > p->declared ? len : (size_t)p->declared);
    p->state = LT_PAGE_CLEAN;
}

const lt_u8 *lt_ecu_mem_page(const lt_ecu_mem *m, int page, size_t *len)
{
    mem_page *p = pg(m, page);

    if (!p || !loaded(p)) {
        return NULL;
    }
    if (len) {
        *len = p->len;
    }
    return p->data;
}

const lt_u8 *lt_ecu_mem_read(const lt_ecu_mem *m, int page, size_t off, size_t len)
{
    mem_page *p = pg(m, page);

    if (!p || !loaded(p) || off > p->len || len > p->len - off) {
        return NULL;
    }
    return p->data + off;
}

int lt_ecu_mem_write(lt_ecu_mem *m, int page, size_t off, const lt_u8 *data, size_t n)
{
    mem_page *p = pg(m, page);
    size_t i;

    if (!p || !loaded(p)) {
        return LT_ENOENT;
    }
    if (off > p->len || n > p->len - off) {
        return LT_ERANGE;
    }
    if (n == 0) {
        return LT_OK;
    }
    memcpy(p->data + off, data, n);
    ensure_bitmap(p, p->len);
    for (i = off; i < off + n; i++) {
        if (!(p->dirty[i >> 3] & (lt_u8)(1u << (i & 7)))) {
            p->dirty[i >> 3] = (lt_u8)(p->dirty[i >> 3] | (lt_u8)(1u << (i & 7)));
            p->ndirty++;
        }
    }
    p->state = LT_PAGE_DIRTY;
    return LT_OK;
}

lt_bool lt_ecu_mem_fully_loaded(const lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        if (!loaded(&m->pages[i])) {
            return LT_FALSE;
        }
    }
    return LT_TRUE;
}

size_t lt_ecu_mem_pages_to_load(const lt_ecu_mem *m, int *out, size_t cap)
{
    size_t i, c = 0;

    for (i = 0; m && i < m->n; i++) {
        if (m->pages[i].state == LT_PAGE_NOT_LOADED) {
            if (out && c < cap) {
                out[c] = (int)i;
            }
            c++;
        }
    }
    return c;
}

lt_bool lt_ecu_mem_has_dirty(const lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        if (m->pages[i].ndirty > 0) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

size_t lt_ecu_mem_dirty_bytes(const lt_ecu_mem *m)
{
    size_t i, c = 0;

    for (i = 0; m && i < m->n; i++) {
        c += m->pages[i].ndirty;
    }
    return c;
}

size_t lt_ecu_mem_dirty_pages(const lt_ecu_mem *m, int *out, size_t cap)
{
    size_t i, c = 0;

    for (i = 0; m && i < m->n; i++) {
        if (m->pages[i].ndirty > 0) {
            if (out && c < cap) {
                out[c] = (int)i;
            }
            c++;
        }
    }
    return c;
}

lt_bool lt_ecu_mem_is_dirty(const lt_ecu_mem *m, int page, size_t off)
{
    mem_page *p = pg(m, page);

    if (!p || off >= p->dirty_cap * 8) {
        return LT_FALSE;
    }
    return (p->dirty[off >> 3] & (1u << (off & 7))) ? LT_TRUE : LT_FALSE;
}

size_t lt_ecu_mem_dirty_ranges(const lt_ecu_mem *m, int page, lt_range *out, size_t cap)
{
    mem_page *p = pg(m, page);
    size_t i, c = 0, start = 0, lim;
    lt_bool in = LT_FALSE, d;

    if (!p || p->ndirty == 0) {
        return 0;
    }
    lim = p->len > p->declared ? p->len : (size_t)p->declared;
    if (lim > p->dirty_cap * 8) {
        lim = p->dirty_cap * 8;
    }
    for (i = 0; i <= lim; i++) {
        d = (i < lim && (p->dirty[i >> 3] & (1u << (i & 7)))) ? LT_TRUE : LT_FALSE;
        if (d && !in) {
            in = LT_TRUE;
            start = i;
        } else if (!d && in) {
            in = LT_FALSE;
            if (out && c < cap) {
                out[c].start = start;
                out[c].len = i - start;
            }
            c++;
        }
    }
    return c;
}

/* Set (on) or clear the dirty bits of [off, off+n) without touching the image. */
static void set_range(mem_page *p, size_t off, size_t n, lt_bool on)
{
    size_t i, end;
    lt_u8 bit;

    if (off >= p->len) {
        return;
    }
    end = n > p->len - off ? p->len : off + n;
    ensure_bitmap(p, p->len);
    for (i = off; i < end; i++) {
        bit = (lt_u8)(1u << (i & 7));
        if (on && !(p->dirty[i >> 3] & bit)) {
            p->dirty[i >> 3] = (lt_u8)(p->dirty[i >> 3] | bit);
            p->ndirty++;
        } else if (!on && (p->dirty[i >> 3] & bit)) {
            p->dirty[i >> 3] = (lt_u8)(p->dirty[i >> 3] & (lt_u8)~bit);
            p->ndirty--;
        }
    }
}

void lt_ecu_mem_clear_dirty(lt_ecu_mem *m, int page, size_t off, size_t n)
{
    mem_page *p = pg(m, page);

    if (p && loaded(p)) {
        set_range(p, off, n, LT_FALSE);
    }
}

void lt_ecu_mem_mark_dirty(lt_ecu_mem *m, int page, size_t off, size_t n)
{
    mem_page *p = pg(m, page);

    if (p && loaded(p) && n > 0) {
        set_range(p, off, n, LT_TRUE);
        p->state = LT_PAGE_DIRTY;
    }
}

void lt_ecu_mem_mark_page_pending(lt_ecu_mem *m, int page)
{
    mem_page *p = pg(m, page);

    if (p && loaded(p)) {
        p->pend = 1;
        if (p->ndirty == 0) {
            p->state = LT_PAGE_PENDING;
        }
    }
}

void lt_ecu_mem_mark_pending(lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        mem_page *p = &m->pages[i];

        if (p->ndirty > 0) {
            clear_bits(p);
            p->state = LT_PAGE_PENDING;
            p->pend = 1;
        }
    }
}

void lt_ecu_mem_mark_burned(lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        mem_page *p = &m->pages[i];

        if (p->pend) {
            p->pend = 0;
            if (p->state == LT_PAGE_PENDING) {
                p->state = LT_PAGE_CLEAN;
            }
        }
    }
}

lt_bool lt_ecu_mem_has_pending_burn(const lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        if (m->pages[i].pend) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

void lt_ecu_mem_revert(lt_ecu_mem *m)
{
    size_t i;

    for (i = 0; m && i < m->n; i++) {
        mem_page *p = &m->pages[i];

        clear_bits(p);
        if (p->state == LT_PAGE_DIRTY) {
            p->state = LT_PAGE_NOT_LOADED;
        }
    }
}

static const lt_u8 *ps_page(void *ud, int page, size_t *len)
{
    return lt_ecu_mem_page((const lt_ecu_mem *)ud, page, len);
}

static int ps_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    return lt_ecu_mem_write((lt_ecu_mem *)ud, page, off, data, n);
}

lt_pagesrc lt_ecu_mem_pagesrc(lt_ecu_mem *m)
{
    lt_pagesrc s;

    s.ud = m;
    s.page = ps_page;
    s.write = ps_write;
    return s;
}
