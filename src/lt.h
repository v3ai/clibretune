/* lt.h - LibreTune-C portability layer and core utilities.
 * Valid C89 .. C23. Only standard C headers. See docs/CONVENTIONS.md.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_H
#define LT_H

#include <stddef.h>
#include <stdarg.h>
#include <limits.h>

/* ---- fixed-ish width integer types (no stdint.h in C89) ---- */
typedef unsigned char  lt_u8;
typedef signed char    lt_s8;
typedef unsigned short lt_u16;
typedef short          lt_s16;
#if UINT_MAX >= 0xFFFFFFFFUL
typedef unsigned int   lt_u32;
typedef int            lt_s32;
#else
typedef unsigned long  lt_u32;
typedef long           lt_s32;
#endif
/* lt_u32 is >= 32 bits; mask with LT_U32_MASK when relying on wraparound. */
#define LT_U32_MASK 0xFFFFFFFFUL

typedef int lt_bool;
#define LT_TRUE  1
#define LT_FALSE 0

#define LT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LT_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LT_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define LT_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define LT_UNUSED(x) ((void)(x))

#define LT_PI 3.14159265358979323846

/* ---- memory (abort on OOM) ---- */
void *lt_malloc(size_t n);
void *lt_calloc(size_t count, size_t size);
void *lt_realloc(void *p, size_t n);
void  lt_free(void *p);

/* ---- errors: last-error message, module-agnostic ---- */
void        lt_err_set(const char *fmt, ...);
const char *lt_err_get(void);
void        lt_err_clear(void);

/* ---- formatting (C89 has no snprintf) ----
 * Supports: %% %c %s %d %i %u %x %X %o %ld %lu %lx %f %e %g %p, flags "-+ 0#",
 * width and precision incl. '*'. Always NUL-terminates when cap > 0.
 * Returns the length that WOULD have been written (like C99 snprintf). */
int lt_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap);
int lt_snprintf(char *dst, size_t cap, const char *fmt, ...);
/* Shortest round-trippable-ish number formatting with given max decimals,
 * trailing zeros trimmed ("1.5", "100", "-0.25"). */
void lt_fmt_double(char *dst, size_t cap, double v, int max_decimals);

/* ---- strings ---- */
char  *lt_strdup(const char *s);
char  *lt_strndup(const char *s, size_t n);
int    lt_stricmp(const char *a, const char *b);
int    lt_strnicmp(const char *a, const char *b, size_t n);
lt_bool lt_str_starts(const char *s, const char *prefix);
lt_bool lt_str_ends(const char *s, const char *suffix);
lt_bool lt_str_istarts(const char *s, const char *prefix);
/* trim in place; returns s */
char  *lt_str_trim(char *s);
/* copy with truncation, always NUL-terminates; returns strlen(src) */
size_t lt_strlcpy(char *dst, const char *src, size_t cap);
size_t lt_strlcat(char *dst, const char *src, size_t cap);
/* parse helpers: return LT_TRUE on success (whole string consumed after trim) */
lt_bool lt_parse_double(const char *s, double *out);
lt_bool lt_parse_long(const char *s, long *out); /* accepts 0x hex */

/* math helpers missing from C89 */
double lt_round(double x);   /* half away from zero */
double lt_trunc(double x);
lt_bool lt_isnan(double x);
lt_bool lt_isfinite(double x);

/* ---- growable byte/char buffer ---- */
typedef struct lt_buf {
    char  *data;   /* always NUL-terminated when len>0 or after any append */
    size_t len;
    size_t cap;
} lt_buf;
void lt_buf_init(lt_buf *b);
void lt_buf_free(lt_buf *b);
void lt_buf_clear(lt_buf *b);
void lt_buf_reserve(lt_buf *b, size_t extra);
void lt_buf_append(lt_buf *b, const void *data, size_t n);
void lt_buf_puts(lt_buf *b, const char *s);
void lt_buf_putc(lt_buf *b, int c);
void lt_buf_printf(lt_buf *b, const char *fmt, ...);
void lt_buf_vprintf(lt_buf *b, const char *fmt, va_list ap);
/* detach data (caller frees); buffer reset */
char *lt_buf_take(lt_buf *b);

/* ---- generic growable array of fixed-size elements ---- */
typedef struct lt_vec {
    void  *data;
    size_t len;
    size_t cap;
    size_t elem;
} lt_vec;
void  lt_vec_init(lt_vec *v, size_t elem_size);
void  lt_vec_free(lt_vec *v);
void *lt_vec_push(lt_vec *v);              /* returns zeroed new slot */
void *lt_vec_at(const lt_vec *v, size_t i);
void  lt_vec_remove(lt_vec *v, size_t i);  /* order-preserving */
void  lt_vec_clear(lt_vec *v);
#define LT_VEC_AT(v, type, i) (((type *)(v)->data)[i])

/* ---- string -> pointer hash map (keys copied; case-sensitive or not) ---- */
typedef struct lt_map_entry {
    char *key;
    void *val;
    lt_u32 hash;
    int   used;
} lt_map_entry;
typedef struct lt_map {
    lt_map_entry *slots;
    size_t cap;
    size_t len;
    lt_bool icase;
} lt_map;
void  lt_map_init(lt_map *m, lt_bool case_insensitive);
void  lt_map_free(lt_map *m);  /* frees keys, not values */
void  lt_map_put(lt_map *m, const char *key, void *val);
void *lt_map_get(const lt_map *m, const char *key);
lt_bool lt_map_has(const lt_map *m, const char *key);
/* iterate: for (i = 0; i < m.cap; i++) if (m.slots[i].used == 1) ... */

/* ---- files (stdio only) ---- */
/* read whole file; returns malloc'd NUL-terminated buffer or NULL (err set) */
char *lt_file_read(const char *path, size_t *len_out);
int   lt_file_write(const char *path, const void *data, size_t len);
lt_bool lt_file_exists(const char *path);
/* path join with '/', result malloc'd */
char *lt_path_join(const char *a, const char *b);
/* pointer to basename within path; extension (incl dot) or "" */
const char *lt_path_base(const char *path);
const char *lt_path_ext(const char *path);

/* ---- hashing / checksums ---- */
lt_u32 lt_hash_str(const char *s, lt_bool icase); /* FNV-1a */
lt_u32 lt_crc32(lt_u32 crc, const void *data, size_t n); /* IEEE 802.3, init 0 */

/* ---- byte order helpers ---- */
lt_u32 lt_rd_be(const lt_u8 *p, int nbytes);
lt_u32 lt_rd_le(const lt_u8 *p, int nbytes);
void   lt_wr_be(lt_u8 *p, int nbytes, lt_u32 v);
void   lt_wr_le(lt_u8 *p, int nbytes, lt_u32 v);
double lt_f32_from_bits(lt_u32 bits);
lt_u32 lt_f32_to_bits(double v);

#endif /* LT_H */
