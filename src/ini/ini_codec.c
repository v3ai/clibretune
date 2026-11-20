/* ini_codec.c - data types, bit fields, constant value codec, realtime decoder.
 * Spec: docs/spec/01-ini-core.md 6.3, 7.1, 10; docs/spec/02-ini-expr-ui.md 3.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ini_priv.h"

/* ------------------------------------------------------------------ */
/* data types                                                          */
/* ------------------------------------------------------------------ */

static const struct { const char *name; int type; } dtype_names[] = {
    {"U08", LT_DT_U08}, {"UINT8", LT_DT_U08}, {"BYTE", LT_DT_U08}, {"BU08", LT_DT_U08},
    {"S08", LT_DT_S08}, {"INT8", LT_DT_S08}, {"SBYTE", LT_DT_S08}, {"BS08", LT_DT_S08},
    {"U16", LT_DT_U16}, {"UINT16", LT_DT_U16}, {"WORD", LT_DT_U16}, {"BU16", LT_DT_U16},
    {"S16", LT_DT_S16}, {"INT16", LT_DT_S16}, {"SWORD", LT_DT_S16}, {"BS16", LT_DT_S16},
    {"U32", LT_DT_U32}, {"UINT32", LT_DT_U32}, {"DWORD", LT_DT_U32}, {"BU32", LT_DT_U32},
    {"S32", LT_DT_S32}, {"INT32", LT_DT_S32}, {"SDWORD", LT_DT_S32}, {"BS32", LT_DT_S32},
    {"F32", LT_DT_F32}, {"FLOAT", LT_DT_F32}, {"BF32", LT_DT_F32},
    {"F64", LT_DT_F64}, {"DOUBLE", LT_DT_F64}, {"BF64", LT_DT_F64},
    {"ASCII", LT_DT_STRING}, {"STRING", LT_DT_STRING}, {"BITS", LT_DT_BITS}
};

int lt_dtype_parse(const char *s, lt_dtype *t, int *endian_override)
{
    char buf[32];
    size_t i, n;

    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    if (n == 0 || n >= sizeof buf) return LT_EINVAL;
    for (i = 0; i < n; i++) {
        char c = s[i];
        buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    buf[n] = '\0';
    for (i = 0; i < LT_COUNTOF(dtype_names); i++) {
        if (strcmp(buf, dtype_names[i].name) == 0) {
            if (t) *t = (lt_dtype)dtype_names[i].type;
            if (endian_override) {
                *endian_override = (n > 1 && buf[0] == 'B' && buf[1] >= 'A' && buf[1] <= 'Z')
                                       ? (int)LT_ENDIAN_BIG : -1;
            }
            return 0;
        }
    }
    return LT_EINVAL;
}

const char *lt_dtype_name(lt_dtype t)
{
    switch (t) {
    case LT_DT_U08: return "U08";
    case LT_DT_S08: return "S08";
    case LT_DT_U16: return "U16";
    case LT_DT_S16: return "S16";
    case LT_DT_U32: return "U32";
    case LT_DT_S32: return "S32";
    case LT_DT_F32: return "F32";
    case LT_DT_F64: return "F64";
    case LT_DT_STRING: return "String";
    case LT_DT_BITS: return "Bits";
    }
    return "U08";
}

size_t lt_dtype_size(lt_dtype t)
{
    switch (t) {
    case LT_DT_U08: case LT_DT_S08: case LT_DT_BITS: return 1;
    case LT_DT_U16: case LT_DT_S16: return 2;
    case LT_DT_U32: case LT_DT_S32: case LT_DT_F32: return 4;
    case LT_DT_F64: return 8;
    case LT_DT_STRING: return 0;
    }
    return 0;
}

lt_bool lt_dtype_range(lt_dtype t, double *lo, double *hi)
{
    double l, h;
    switch (t) {
    case LT_DT_U08: case LT_DT_BITS: l = 0; h = 255; break;
    case LT_DT_S08: l = -128; h = 127; break;
    case LT_DT_U16: l = 0; h = 65535; break;
    case LT_DT_S16: l = -32768; h = 32767; break;
    case LT_DT_U32: l = 0; h = 4294967295.0; break;
    case LT_DT_S32: l = -2147483648.0; h = 2147483647.0; break;
    default: return LT_FALSE;
    }
    if (lo) *lo = l;
    if (hi) *hi = h;
    return LT_TRUE;
}

static double f64_from_words(lt_u32 hi, lt_u32 lo)
{
    int neg = (hi & 0x80000000UL) != 0;
    int ex = (int)((hi >> 20) & 0x7FF);
    double mant = (double)(hi & 0xFFFFFUL) * 4294967296.0 + (double)lo;
    double v;
    if (ex == 0) {
        v = ldexp(mant, -1074);
    } else if (ex == 0x7FF) {
        if (mant != 0.0) {
            v = HUGE_VAL - HUGE_VAL;
            if (v == v) v = 0.0; /* paranoia: keep a value even if NaN is unsupported */
        } else {
            v = HUGE_VAL;
        }
    } else {
        v = ldexp(mant + 4503599627370496.0, ex - 1075);
    }
    return neg ? -v : v;
}

static void f64_to_words(double v, lt_u32 *hi, lt_u32 *lo)
{
    int neg = 0, e;
    double m, frac, fh;
    long biased;

    if (lt_isnan(v)) {
        *hi = 0x7FF80000UL;
        *lo = 0;
        return;
    }
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    if (v == 0.0) {
        *hi = 0;
        *lo = 0;
    } else if (!lt_isfinite(v)) {
        *hi = 0x7FF00000UL;
        *lo = 0;
    } else {
        m = frexp(v, &e);              /* v = m * 2^e, m in [0.5,1) */
        biased = (long)e + 1022L;
        if (biased >= 0x7FF) {
            *hi = 0x7FF00000UL;
            *lo = 0;
        } else if (biased <= 0) {
            frac = lt_round(ldexp(v, 1074));
            fh = floor(frac / 4294967296.0);
            *hi = (lt_u32)fh;
            *lo = (lt_u32)(frac - fh * 4294967296.0);
        } else {
            frac = lt_round((m * 2.0 - 1.0) * 4503599627370496.0);
            if (frac >= 4503599627370496.0) {
                frac = 0;
                biased++;
            }
            if (biased >= 0x7FF) {
                *hi = 0x7FF00000UL;
                *lo = 0;
            } else {
                fh = floor(frac / 4294967296.0);
                *hi = ((lt_u32)biased << 20) | (lt_u32)fh;
                *lo = (lt_u32)(frac - fh * 4294967296.0);
            }
        }
    }
    if (neg) *hi |= 0x80000000UL;
}

int lt_dtype_read(lt_dtype t, const lt_u8 *data, size_t len, size_t off, lt_endian e,
                  double *out)
{
    size_t sz = lt_dtype_size(t);
    lt_u32 u;

    if (sz == 0) return LT_EINVAL;
    if (!data || off > len || sz > len - off) return LT_ERANGE;
    data += off;
    switch (t) {
    case LT_DT_U08: case LT_DT_BITS: *out = (double)data[0]; break;
    case LT_DT_S08: *out = (double)(data[0] >= 128 ? (int)data[0] - 256 : (int)data[0]); break;
    case LT_DT_U16:
        u = e == LT_ENDIAN_BIG ? lt_rd_be(data, 2) : lt_rd_le(data, 2);
        *out = (double)u;
        break;
    case LT_DT_S16:
        u = e == LT_ENDIAN_BIG ? lt_rd_be(data, 2) : lt_rd_le(data, 2);
        *out = (double)(u >= 0x8000UL ? (long)u - 65536L : (long)u);
        break;
    case LT_DT_U32:
        u = e == LT_ENDIAN_BIG ? lt_rd_be(data, 4) : lt_rd_le(data, 4);
        *out = (double)u;
        break;
    case LT_DT_S32:
        u = e == LT_ENDIAN_BIG ? lt_rd_be(data, 4) : lt_rd_le(data, 4);
        *out = u >= 0x80000000UL ? (double)u - 4294967296.0 : (double)u;
        break;
    case LT_DT_F32:
        u = e == LT_ENDIAN_BIG ? lt_rd_be(data, 4) : lt_rd_le(data, 4);
        *out = lt_f32_from_bits(u);
        break;
    case LT_DT_F64: {
        lt_u32 a, b;
        if (e == LT_ENDIAN_BIG) {
            a = lt_rd_be(data, 4);
            b = lt_rd_be(data + 4, 4);
        } else {
            b = lt_rd_le(data, 4);
            a = lt_rd_le(data + 4, 4);
        }
        *out = f64_from_words(a, b);
        break;
    }
    default:
        return LT_EINVAL;
    }
    return 0;
}

int lt_dtype_write(lt_dtype t, lt_u8 *data, size_t len, size_t off, lt_endian e, double v)
{
    size_t sz = lt_dtype_size(t);
    double lo = 0.0, hi = 0.0;
    lt_u32 u;

    if (sz == 0) return LT_EINVAL;
    if (!data || off > len || sz > len - off) return LT_ERANGE;
    data += off;
    if (t == LT_DT_F32) {
        u = lt_f32_to_bits(v);
        if (e == LT_ENDIAN_BIG) lt_wr_be(data, 4, u);
        else lt_wr_le(data, 4, u);
        return 0;
    }
    if (t == LT_DT_F64) {
        lt_u32 a, b;
        f64_to_words(v, &a, &b);
        if (e == LT_ENDIAN_BIG) {
            lt_wr_be(data, 4, a);
            lt_wr_be(data + 4, 4, b);
        } else {
            lt_wr_le(data, 4, b);
            lt_wr_le(data + 4, 4, a);
        }
        return 0;
    }
    lt_dtype_range(t, &lo, &hi);
    if (lt_isnan(v)) v = 0;
    v = lt_round(v);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v < 0) {
        /* two's complement of the magnitude, in the type's width */
        double mag = -v;
        u = (lt_u32)mag;
        u = (~u + 1UL) & LT_U32_MASK;
    } else {
        u = (lt_u32)v;
    }
    switch (sz) {
    case 1: data[0] = (lt_u8)(u & 0xFF); break;
    case 2:
        if (e == LT_ENDIAN_BIG) lt_wr_be(data, 2, u & 0xFFFFUL);
        else lt_wr_le(data, 2, u & 0xFFFFUL);
        break;
    default:
        if (e == LT_ENDIAN_BIG) lt_wr_be(data, 4, u);
        else lt_wr_le(data, 4, u);
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* bit fields                                                          */
/* ------------------------------------------------------------------ */

int lt_bits_get(const lt_u8 *data, size_t len, size_t off, int bit_pos, int bit_size,
                lt_u32 *out)
{
    size_t boff, need;
    int bit_in, i, got = 0;
    lt_u32 v = 0;

    *out = 0;
    if (bit_size <= 0 || bit_pos < 0 || !data || len == 0) return bit_size <= 0 ? 0 : LT_ERANGE;
    if (bit_size > 32) return LT_EINVAL;
    boff = off + (size_t)(bit_pos / 8);
    bit_in = bit_pos % 8;
    need = (size_t)((bit_in + bit_size + 7) / 8);
    if (boff >= len || need > len - boff) return LT_ERANGE;
    for (i = 0; i < (int)need; i++) {
        int lo = i == 0 ? bit_in : 0;
        int hi = (i == (int)need - 1) ? bit_in + bit_size - 8 * i : 8;
        int nb = hi - lo;
        lt_u32 bits = ((lt_u32)data[boff + (size_t)i] >> lo) & (((lt_u32)1 << nb) - (lt_u32)1);
        v |= bits << got;
        got += nb;
    }
    *out = v;
    return 0;
}

int lt_bits_set(lt_u8 *data, size_t len, size_t off, int bit_pos, int bit_size, lt_u32 value)
{
    size_t boff, need;
    int bit_in, i, got = 0;

    if (bit_size <= 0) return 0;
    if (bit_pos < 0 || !data || len == 0) return LT_ERANGE;
    if (bit_size > 32) return LT_EINVAL;
    boff = off + (size_t)(bit_pos / 8);
    bit_in = bit_pos % 8;
    need = (size_t)((bit_in + bit_size + 7) / 8);
    if (boff >= len || need > len - boff) return LT_ERANGE;
    for (i = 0; i < (int)need; i++) {
        int lo = i == 0 ? bit_in : 0;
        int hi = (i == (int)need - 1) ? bit_in + bit_size - 8 * i : 8;
        int nb = hi - lo;
        lt_u32 m = (((lt_u32)1 << nb) - (lt_u32)1);
        lt_u32 bits = (value >> got) & m;
        lt_u8 *p = data + boff + (size_t)i;
        *p = (lt_u8)((*p & ~(m << lo)) | (bits << lo));
        got += nb;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* constants: conversions                                              */
/* ------------------------------------------------------------------ */

const char *lt_ini_bit_option(const lt_ini_const *c, size_t idx)
{
    if (idx >= c->n_bit_options) return NULL;
    return c->bit_options[idx];
}

int lt_ini_bit_index(const lt_ini_const *c, const char *label)
{
    size_t i;
    for (i = 0; i < c->n_bit_options; i++)
        if (c->bit_options[i] && strcmp(c->bit_options[i], label) == 0) return (int)i;
    for (i = 0; i < c->n_bit_options; i++)
        if (c->bit_options[i] && lt_stricmp(c->bit_options[i], label) == 0) return (int)i;
    return -1;
}

double lt_const_raw_to_display(const lt_ini_const *c, double raw)
{
    return raw * c->scale + c->translate;
}

double lt_const_display_to_raw(const lt_ini_const *c, double display)
{
    double v = display, sc = c->scale == 0.0 ? 1.0 : c->scale;
    if (c->range_resolved) {
        double lo = c->min < c->max ? c->min : c->max;
        double hi = c->min < c->max ? c->max : c->min;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    }
    return (v - c->translate) / sc;
}

lt_bool lt_const_in_range(const lt_ini_const *c, double display)
{
    return (display >= c->min && display <= c->max) ? LT_TRUE : LT_FALSE;
}

lt_endian lt_ini_const_endian(const lt_ini *ini, const lt_ini_const *c)
{
    if (c->endian_override >= 0) return (lt_endian)c->endian_override;
    return ini->endian;
}

/* ------------------------------------------------------------------ */
/* constants: page access                                              */
/* ------------------------------------------------------------------ */

static const lt_u8 *get_page(const lt_pagesrc *src, const lt_ini_const *c, size_t *len)
{
    if (!src || !src->page || c->is_pc_var) return NULL;
    return src->page(src->ud, (int)c->page, len);
}

int lt_ini_const_get_raw(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                         size_t idx, double *raw)
{
    size_t len = 0, off;
    const lt_u8 *pg = get_page(src, c, &len);
    int rc;

    if (!pg) return LT_ENOENT;
    if (c->type == LT_DT_STRING) return LT_EINVAL;
    if (c->type == LT_DT_BITS) {
        lt_u32 v;
        rc = lt_bits_get(pg, len, c->offset, c->bit_pos < 0 ? 0 : c->bit_pos,
                         c->bit_size < 0 ? 8 : c->bit_size, &v);
        if (rc) return rc;
        *raw = (double)v;
        return 0;
    }
    if (idx >= lt_shape_count(&c->shape)) return LT_ERANGE;
    off = (size_t)c->offset + idx * lt_dtype_size(c->type);
    return lt_dtype_read(c->type, pg, len, off, lt_ini_const_endian(ini, c), raw);
}

/* write [off, off+n) of new bytes into the page, only the runs that differ */
static int write_changed(lt_pagesrc *src, int page, const lt_u8 *old, size_t len, size_t off,
                         const lt_u8 *nw, size_t n)
{
    size_t i = 0;
    int rc;
    if (off > len || n > len - off) return LT_ERANGE;
    while (i < n) {
        size_t j;
        if (old[off + i] == nw[i]) {
            i++;
            continue;
        }
        j = i;
        while (j < n && old[off + j] != nw[j]) j++;
        if (!src->write) return LT_EUNSUPPORTED;
        rc = src->write(src->ud, page, off + i, nw + i, j - i);
        if (rc) return rc;
        i = j;
    }
    return 0;
}

int lt_ini_const_set_raw(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                         size_t idx, double raw)
{
    size_t len = 0, off, sz;
    const lt_u8 *pg = get_page(src, c, &len);
    lt_u8 tmp[16];
    int rc;

    if (!pg) return LT_ENOENT;
    if (c->type == LT_DT_STRING) return LT_EINVAL;
    if (c->type == LT_DT_BITS) {
        int pos = c->bit_pos < 0 ? 0 : c->bit_pos, size = c->bit_size < 0 ? 8 : c->bit_size;
        size_t boff = (size_t)c->offset + (size_t)(pos / 8);
        size_t need = (size_t)((pos % 8 + size + 7) / 8);
        lt_u32 mask = size >= 32 ? (lt_u32)LT_U32_MASK : (((lt_u32)1 << size) - (lt_u32)1);
        lt_u32 v;
        if (lt_isnan(raw) || raw < 0) raw = 0;
        v = raw > (double)mask ? mask : (lt_u32)raw;
        if (boff >= len || need > len - boff || need > sizeof tmp) return LT_ERANGE;
        memcpy(tmp, pg + boff, need);
        rc = lt_bits_set(tmp, need, 0, pos % 8, size, v);
        if (rc) return rc;
        return write_changed(src, (int)c->page, pg, len, boff, tmp, need);
    }
    if (idx >= lt_shape_count(&c->shape)) return LT_ERANGE;
    sz = lt_dtype_size(c->type);
    off = (size_t)c->offset + idx * sz;
    if (off > len || sz > len - off) return LT_ERANGE;
    memcpy(tmp, pg + off, sz);
    rc = lt_dtype_write(c->type, tmp, sz, 0, lt_ini_const_endian(ini, c), raw);
    if (rc) return rc;
    return write_changed(src, (int)c->page, pg, len, off, tmp, sz);
}

int lt_ini_const_get(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                     size_t idx, double *display)
{
    double raw;
    int rc = lt_ini_const_get_raw(ini, c, src, idx, &raw);
    if (rc) return rc;
    *display = c->type == LT_DT_BITS ? raw : lt_const_raw_to_display(c, raw);
    return 0;
}

int lt_ini_const_set(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                     size_t idx, double display)
{
    double raw = c->type == LT_DT_BITS ? display : lt_const_display_to_raw(c, display);
    return lt_ini_const_set_raw(ini, c, src, idx, raw);
}

int lt_ini_const_get_array(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                           double *out, size_t cap)
{
    size_t n = lt_shape_count(&c->shape), i;
    size_t len = 0;
    const lt_u8 *pg;

    if (c->type == LT_DT_STRING) return LT_EINVAL;
    if (!out) return (int)n;
    pg = get_page(src, c, &len);
    if (!pg) return LT_ENOENT;
    if (cap < n) return LT_ERANGE;
    for (i = 0; i < n; i++) {
        int rc = lt_ini_const_get(ini, c, src, i, &out[i]);
        if (rc) return rc;
    }
    return (int)n;
}

int lt_ini_const_set_array(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                           const double *v, size_t n)
{
    size_t cnt = lt_shape_count(&c->shape), i, sz, len = 0, total;
    const lt_u8 *pg;
    lt_u8 *buf;
    lt_endian e;
    int rc = 0;

    if (c->type == LT_DT_STRING || c->type == LT_DT_BITS) return LT_EINVAL;
    pg = get_page(src, c, &len);
    if (!pg) return LT_ENOENT;
    if (n > cnt) n = cnt;
    sz = lt_dtype_size(c->type);
    total = n * sz;
    if ((size_t)c->offset > len || total > len - (size_t)c->offset) return LT_ERANGE;
    buf = (lt_u8 *)lt_malloc(total ? total : 1);
    memcpy(buf, pg + c->offset, total);
    e = lt_ini_const_endian(ini, c);
    for (i = 0; i < n; i++) {
        rc = lt_dtype_write(c->type, buf, total, i * sz, e, lt_const_display_to_raw(c, v[i]));
        if (rc) break;
    }
    if (rc == 0) rc = write_changed(src, (int)c->page, pg, len, c->offset, buf, total);
    lt_free(buf);
    return rc;
}

int lt_ini_const_get_string(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                            char *buf, size_t cap)
{
    size_t len = 0, n, i;
    const lt_u8 *pg = get_page(src, c, &len);

    LT_UNUSED(ini);
    if (!pg) return LT_ENOENT;
    if (c->type != LT_DT_STRING) return LT_EINVAL;
    n = c->size_bytes;
    if ((size_t)c->offset > len || n > len - (size_t)c->offset) return LT_ERANGE;
    if (cap == 0) return LT_EINVAL;
    for (i = 0; i < n && i + 1 < cap; i++) {
        if (pg[c->offset + i] == 0) break;
        buf[i] = (char)pg[c->offset + i];
    }
    buf[i] = '\0';
    return 0;
}

int lt_ini_const_set_string(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                            const char *s)
{
    size_t len = 0, n, sl;
    const lt_u8 *pg = get_page(src, c, &len);
    lt_u8 *tmp;
    int rc;

    LT_UNUSED(ini);
    if (!pg) return LT_ENOENT;
    if (c->type != LT_DT_STRING) return LT_EINVAL;
    n = c->size_bytes;
    if ((size_t)c->offset > len || n > len - (size_t)c->offset) return LT_ERANGE;
    tmp = (lt_u8 *)lt_calloc(n ? n : 1, 1);
    sl = strlen(s);
    if (sl > n) sl = n;
    memcpy(tmp, s, sl);
    rc = write_changed(src, (int)c->page, pg, len, c->offset, tmp, n);
    lt_free(tmp);
    return rc;
}

int lt_ini_get(const lt_ini *ini, const lt_pagesrc *src, const char *name, double *display)
{
    const lt_ini_const *c = lt_ini_find_const(ini, name);
    int rc;
    if (!c) return LT_ENOENT;
    rc = lt_ini_const_get(ini, c, src, 0, display);
    if (rc == 0) return 0;
    if (lt_ini_default_value(ini, name, display)) return 0;
    return rc;
}

int lt_ini_set(const lt_ini *ini, lt_pagesrc *src, const char *name, double display)
{
    const lt_ini_const *c = lt_ini_find_const(ini, name);
    if (!c) return LT_ENOENT;
    return lt_ini_const_set(ini, c, src, 0, display);
}

/* ------------------------------------------------------------------ */
/* dynamic scales                                                      */
/* ------------------------------------------------------------------ */

static lt_bool eval_field(const char *expr, const lt_expr_env *env, double *out)
{
    lt_expr *e;
    char err[96];
    double v;
    int rc;

    if (!expr) return LT_FALSE;
    e = lt_expr_compile(expr, err, sizeof err);
    if (!e) return LT_FALSE;
    rc = lt_expr_eval_f64(e, env, &v);
    lt_expr_free(e);
    if (rc != 0 || !lt_isfinite(v)) return LT_FALSE;
    *out = v;
    return LT_TRUE;
}

int lt_ini_resolve_dynamic(lt_ini *ini, const lt_expr_env *env)
{
    lt_ini_rt *rt = lt_ini_rt_new(ini);
    const lt_expr_env *e;
    size_t i;
    int changed = 0;

    lt_ini_rt_set_parent(rt, env);
    lt_ini_rt_update(rt, NULL, 0);   /* evaluates computed channels over the parent */
    e = lt_ini_rt_env(rt);
    for (i = 0; i < ini->n_constants; i++) {
        lt_ini_const *c = ini->constants[i];
        int ch = 0, all_range = 1;
        double v;
        if (!c->scale_expr && !c->translate_expr && !c->min_expr && !c->max_expr) continue;
        if (c->scale_expr && eval_field(c->scale_expr, e, &v) && v != 0.0 && v != c->scale) {
            c->scale = v;
            ch = 1;
        }
        if (c->translate_expr && eval_field(c->translate_expr, e, &v) && v != c->translate) {
            c->translate = v;
            ch = 1;
        }
        if (c->min_expr) {
            if (eval_field(c->min_expr, e, &v)) {
                if (v != c->min) {
                    c->min = v;
                    ch = 1;
                }
            } else {
                all_range = 0;
            }
        }
        if (c->max_expr) {
            if (eval_field(c->max_expr, e, &v)) {
                if (v != c->max) {
                    c->max = v;
                    ch = 1;
                }
            } else {
                all_range = 0;
            }
        }
        if (c->min_expr || c->max_expr) c->range_resolved = all_range ? LT_TRUE : LT_FALSE;
        changed += ch;
    }
    lt_ini_rt_free(rt);
    return changed;
}

static int feeds_in_expr(const lt_ini *ini, const char *expr, const char *name, int depth)
{
    char **ids;
    size_t n, i;
    int found = 0;

    if (!expr) return 0;
    n = lt_expr_identifiers(expr, &ids);
    for (i = 0; i < n && !found; i++) {
        const lt_ini_channel *ch;
        if (strcmp(ids[i], name) == 0) {
            found = 1;
            break;
        }
        if (depth < 3) {
            ch = lt_ini_find_channel(ini, ids[i]);
            if (ch && ch->expression && feeds_in_expr(ini, ch->expression, name, depth + 1))
                found = 1;
        }
    }
    lt_expr_names_free(ids, n);
    return found;
}

lt_bool lt_ini_feeds_dynamic_scale(const lt_ini *ini, const char *name)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (feeds_in_expr(ini, c->scale_expr, name, 0) ||
            feeds_in_expr(ini, c->translate_expr, name, 0))
            return LT_TRUE;
    }
    return LT_FALSE;
}

int lt_ini_patch_invalid_sizes(const lt_ini *ini, lt_pagesrc *src)
{
    size_t i;
    int patched = 0;
    lt_map seen;

    lt_map_init(&seen, LT_FALSE);
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (c->dyn) {
            if (c->dyn->cols_const) lt_map_put(&seen, c->dyn->cols_const, (void *)&seen);
            if (c->dyn->rows_const) lt_map_put(&seen, c->dyn->rows_const, (void *)&seen);
        }
    }
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        double v, d;
        if (!lt_map_has(&seen, c->name)) continue;
        if (c->type == LT_DT_BITS || c->type == LT_DT_STRING || c->is_pc_var) continue;
        if (lt_ini_const_get(ini, c, src, 0, &v) != 0) continue;
        if ((v < c->min || v > c->max) && lt_ini_default_value(ini, c->name, &d)) {
            if (lt_ini_const_set(ini, c, src, 0, d) == 0) patched++;
        }
    }
    lt_map_free(&seen);
    return patched;
}

/* ------------------------------------------------------------------ */
/* output channels                                                     */
/* ------------------------------------------------------------------ */

size_t lt_ini_channel_size(const lt_ini_channel *ch)
{
    if (ch->expression) return 0;
    if (ch->type == LT_DT_BITS) return lt_dtype_size(ch->bit_storage);
    return lt_dtype_size(ch->type);
}

int lt_ini_channel_decode(const lt_ini_channel *ch, lt_endian e, const lt_u8 *block,
                          size_t len, double *out)
{
    double raw;
    int rc;

    if (ch->expression) return LT_EINVAL;
    if (ch->type == LT_DT_BITS && ch->bit_pos >= 0) {
        lt_u32 word, mask, v;
        int count = ch->bit_count < 1 ? 1 : (ch->bit_count > 32 ? 32 : ch->bit_count);
        rc = lt_dtype_read(ch->bit_storage, block, len, ch->offset, e, &raw);
        if (rc) return rc;
        /* two's complement word (sign kept by masking); bits beyond 32 read as 0 */
        if (!(raw == raw)) raw = 0.0;
        if (raw > 4294967295.0) raw = 4294967295.0;
        if (raw < -2147483648.0) raw = -2147483648.0;
        word = raw < 0 ? (lt_u32)(raw + 4294967296.0) : (lt_u32)raw;
        mask = count >= 32 ? (lt_u32)LT_U32_MASK : (((lt_u32)1 << count) - (lt_u32)1);
        v = ch->bit_pos >= 32 ? (lt_u32)0 : ((word >> ch->bit_pos) & mask);
        *out = (double)v * ch->scale + ch->translate;
        return 0;
    }
    rc = lt_dtype_read(ch->type == LT_DT_BITS ? LT_DT_U08 : ch->type, block, len, ch->offset, e,
                       &raw);
    if (rc) return rc;
    *out = raw * ch->scale + ch->translate;
    return 0;
}

unsigned long lt_ini_realtime_size(const lt_ini *ini)
{
    unsigned long best = 0;
    size_t i;
    if (ini->proto.och_block_size) return ini->proto.och_block_size;
    for (i = 0; i < ini->n_channels; i++) {
        const lt_ini_channel *c = ini->channels[i];
        unsigned long end = (unsigned long)c->offset + (unsigned long)lt_ini_channel_size(c);
        if (!c->expression && end > best) best = end;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* realtime evaluator                                                  */
/* ------------------------------------------------------------------ */

struct lt_ini_rt {
    const lt_ini *ini;
    double *vals;
    unsigned char *valid;
    size_t n;
    size_t *computed;      /* channel indices, sorted by name */
    size_t n_computed;
    const lt_expr_env *parent;
    lt_expr_env env;
    lt_ini_rt_virtual_fn virt;   /* names that are not INI channels (math channels) */
    void *virt_ud;
};

static lt_bool rt_get_var(void *ud, const char *name, double *out)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    const lt_ini_channel *ch = (const lt_ini_channel *)lt_map_get(&rt->ini->channel_idx, name);
    if (ch && rt->valid[ch->index]) {
        *out = rt->vals[ch->index];
        return LT_TRUE;
    }
    if (rt->parent && rt->parent->get_var) return rt->parent->get_var(rt->parent->ud, name, out);
    return LT_FALSE;
}

static const char *rt_get_string(void *ud, const char *name)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->get_string)
               ? rt->parent->get_string(rt->parent->ud, name) : NULL;
}

static const char *const *rt_get_bit_options(void *ud, const char *name, size_t *n)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->get_bit_options)
               ? rt->parent->get_bit_options(rt->parent->ud, name, n) : NULL;
}

static lt_bool rt_table_lookup(void *ud, const char *file, double x, double *out)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->table_lookup)
               ? rt->parent->table_lookup(rt->parent->ud, file, x, out) : LT_FALSE;
}

static lt_bool rt_array_value(void *ud, const char *name, double idx, double *out)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->array_value)
               ? rt->parent->array_value(rt->parent->ud, name, idx, out) : LT_FALSE;
}

static lt_bool rt_is_online(void *ud)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->is_online) ? rt->parent->is_online(rt->parent->ud)
                                                 : LT_FALSE;
}

static const char *rt_get_dir(void *ud, int which)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->get_dir) ? rt->parent->get_dir(rt->parent->ud, which)
                                               : NULL;
}

static double rt_time_now(void *ud)
{
    lt_ini_rt *rt = (lt_ini_rt *)ud;
    return (rt->parent && rt->parent->time_now) ? rt->parent->time_now(rt->parent->ud) : 0.0;
}

static const lt_ini *sort_ini_;

static int cmp_computed(const void *a, const void *b)
{
    const lt_ini_channel *ca = sort_ini_->channels[*(const size_t *)a];
    const lt_ini_channel *cb = sort_ini_->channels[*(const size_t *)b];
    return strcmp(ca->name, cb->name);
}

lt_ini_rt *lt_ini_rt_new(const lt_ini *ini)
{
    lt_ini_rt *rt = (lt_ini_rt *)lt_calloc(1, sizeof(lt_ini_rt));
    size_t i;

    rt->ini = ini;
    rt->n = ini->n_channels;
    rt->vals = (double *)lt_calloc(rt->n ? rt->n : 1, sizeof(double));
    rt->valid = (unsigned char *)lt_calloc(rt->n ? rt->n : 1, 1);
    rt->computed = (size_t *)lt_calloc(rt->n ? rt->n : 1, sizeof(size_t));
    for (i = 0; i < rt->n; i++)
        if (ini->channels[i]->expression) rt->computed[rt->n_computed++] = i;
    if (rt->n_computed > 1) {
        sort_ini_ = ini;
        qsort(rt->computed, rt->n_computed, sizeof(size_t), cmp_computed);
    }
    lt_expr_env_init(&rt->env);
    rt->env.ud = rt;
    rt->env.get_var = rt_get_var;
    rt->env.get_string = rt_get_string;
    rt->env.get_bit_options = rt_get_bit_options;
    rt->env.table_lookup = rt_table_lookup;
    rt->env.array_value = rt_array_value;
    rt->env.is_online = rt_is_online;
    rt->env.get_dir = rt_get_dir;
    rt->env.time_now = rt_time_now;
    return rt;
}

void lt_ini_rt_free(lt_ini_rt *rt)
{
    if (!rt) return;
    lt_free(rt->vals);
    lt_free(rt->valid);
    lt_free(rt->computed);
    lt_free(rt);
}

void lt_ini_rt_set_virtual(lt_ini_rt *rt, lt_ini_rt_virtual_fn fn, void *ud)
{
    rt->virt = fn;
    rt->virt_ud = ud;
}

void lt_ini_rt_set_parent(lt_ini_rt *rt, const lt_expr_env *parent)
{
    rt->parent = parent;
}

static double sanitize(double v)
{
    return lt_isfinite(v) ? v : 0.0;
}

int lt_ini_rt_update(lt_ini_rt *rt, const lt_u8 *frame, size_t len)
{
    const lt_ini *ini = rt->ini;
    size_t i;
    int pass, rc = 0;

    for (i = 0; i < rt->n; i++) {
        const lt_ini_channel *ch = ini->channels[i];
        double v;
        if (ch->expression) {
            rt->valid[i] = 0;
            continue;
        }
        if (frame && lt_ini_channel_decode(ch, ini->endian, frame, len, &v) == 0) {
            rt->vals[i] = v;
            rt->valid[i] = 1;
        } else {
            rt->valid[i] = 0;
            rc = LT_ERANGE;
        }
    }
    for (pass = 0; pass < 3; pass++) {
        int changes = 0;
        for (i = 0; i < rt->n_computed; i++) {
            size_t k = rt->computed[i];
            const lt_ini_channel *ch = ini->channels[k];
            double v;
            if (!ch->ast) continue;
            if (lt_expr_eval_f64(ch->ast, &rt->env, &v) != 0) continue;
            v = sanitize(v);
            if (!rt->valid[k] || fabs(rt->vals[k] - v) > 1e-7) {
                rt->vals[k] = v;
                rt->valid[k] = 1;
                changes++;
            }
        }
        if (!changes) break;
    }
    return rc;
}

void lt_ini_rt_set(lt_ini_rt *rt, size_t channel_index, double v)
{
    if (channel_index >= rt->n) return;
    rt->vals[channel_index] = v;
    rt->valid[channel_index] = 1;
}

lt_bool lt_ini_rt_get(const lt_ini_rt *rt, const char *name, double *out)
{
    const lt_ini_channel *ch = (const lt_ini_channel *)lt_map_get(&rt->ini->channel_idx, name);
    if (!ch) return rt->virt != NULL ? rt->virt(rt->virt_ud, name, out) : LT_FALSE;
    if (!rt->valid[ch->index]) return LT_FALSE;
    *out = rt->vals[ch->index];
    return LT_TRUE;
}

double lt_ini_rt_value(const lt_ini_rt *rt, size_t channel_index)
{
    return channel_index < rt->n ? rt->vals[channel_index] : 0.0;
}

lt_bool lt_ini_rt_valid(const lt_ini_rt *rt, size_t channel_index)
{
    return (channel_index < rt->n && rt->valid[channel_index]) ? LT_TRUE : LT_FALSE;
}

size_t lt_ini_rt_count(const lt_ini_rt *rt)
{
    return rt->n;
}

const lt_expr_env *lt_ini_rt_env(lt_ini_rt *rt)
{
    return &rt->env;
}
