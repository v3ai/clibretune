/* lt.h - LibreTune-C portability layer and core utilities.
 * Valid C89 .. C23. Only standard C headers. Follows docs/UNIVERSAL_C.md
 * (project prefix lt_/LT_ stands in for the standard's p_/P_).
 * This is the ONLY file that may test compiler/version macros.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_H
#define LT_H

#include <limits.h>
#include <stddef.h>
#include <stdarg.h>

/* ---- language version ---------------------------------------------- */
#if defined(__cplusplus)
#  define LT_STDC 0L
#elif defined(__STDC_VERSION__)
#  define LT_STDC __STDC_VERSION__
#else
#  define LT_STDC 198900L
#endif

/* ---- hard platform assumptions ------------------------------------- */
#if CHAR_BIT != 8
#  error "LibreTune requires 8-bit bytes"
#endif

/* ---- inline / restrict --------------------------------------------- */
#if defined(__cplusplus) || LT_STDC >= 199901L
#  define LT_INLINE inline
#elif defined(__GNUC__)
#  define LT_INLINE __inline__
#elif defined(_MSC_VER)
#  define LT_INLINE __inline
#else
#  define LT_INLINE
#endif

#if !defined(__cplusplus) && LT_STDC >= 199901L
#  define LT_RESTRICT restrict
#elif defined(__GNUC__)
#  define LT_RESTRICT __restrict__
#elif defined(_MSC_VER)
#  define LT_RESTRICT __restrict
#else
#  define LT_RESTRICT
#endif

/* ---- boolean (never collides with C23/C++ bool) -------------------- */
typedef int lt_bool;
#define LT_TRUE  1
#define LT_FALSE 0

/* ---- exact-width integers without <stdint.h> ----------------------- */
typedef unsigned char lt_u8;
typedef signed char   lt_s8;

#if USHRT_MAX == 0xFFFFU
typedef unsigned short lt_u16;
typedef short          lt_s16;
#elif UINT_MAX == 0xFFFFU
typedef unsigned int   lt_u16;
typedef int            lt_s16;
#else
#  error "no 16-bit integer type"
#endif

#if UINT_MAX == 0xFFFFFFFFUL
typedef unsigned int   lt_u32;
typedef int            lt_s32;
#elif ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long  lt_u32;
typedef long           lt_s32;
#else
#  error "no 32-bit integer type"
#endif
/* lt_u32 is exactly 32 bits; the mask is kept for readability. */
#define LT_U32_MASK 0xFFFFFFFFUL

/* 64-bit: optional. Code MUST check LT_HAS_64 before using lt_u64/lt_s64. */
#if (!defined(__cplusplus) && LT_STDC >= 199901L) || \
    (defined(__cplusplus) && __cplusplus >= 201103L)
#  include <stdint.h>
typedef uint64_t lt_u64;
typedef int64_t  lt_s64;
#  define LT_U64_C(c) UINT64_C(c)
#  define LT_HAS_64 1
#elif ((ULONG_MAX >> 31) >> 31) == 3
typedef unsigned long lt_u64;
typedef long          lt_s64;
#  define LT_U64_C(c) c##UL
#  define LT_HAS_64 1
#elif defined(__GNUC__)
__extension__ typedef unsigned long long lt_u64;
__extension__ typedef long long          lt_s64;
#  define LT_U64_C(c) (__extension__ c##ULL)
#  define LT_HAS_64 1
#elif defined(_MSC_VER)
typedef unsigned __int64 lt_u64;
typedef __int64          lt_s64;
#  define LT_U64_C(c) c##ui64
#  define LT_HAS_64 1
#else
#  define LT_HAS_64 0
#endif

#define LT_SIZE_MAX ((size_t)-1)

/* ---- utilities ------------------------------------------------------ */
#define LT_CAT_(a, b) a##b
#define LT_CAT(a, b)  LT_CAT_(a, b)
/* File scope only. tag must be a unique identifier fragment (<= 14 chars). */
#define LT_STATIC_ASSERT(cond, tag) \
    typedef char LT_CAT(lt_static_assert_, tag)[(cond) ? 1 : -1]

/* Wrap public headers: LT_BEGIN_DECLS ... LT_END_DECLS */
#ifdef __cplusplus
#  define LT_BEGIN_DECLS extern "C" {
#  define LT_END_DECLS   }
#else
#  define LT_BEGIN_DECLS
#  define LT_END_DECLS
#endif

LT_STATIC_ASSERT(sizeof(lt_u8) == 1, u8_size);
LT_STATIC_ASSERT(sizeof(lt_u16) == 2, u16_size);
LT_STATIC_ASSERT(sizeof(lt_u32) == 4, u32_size);
LT_STATIC_ASSERT(sizeof(lt_s32) == 4, s32_size);
LT_STATIC_ASSERT((-1 & 3) == 3, twos_compl);
#if LT_HAS_64
LT_STATIC_ASSERT(sizeof(lt_u64) == 8, u64_size);
LT_STATIC_ASSERT(sizeof(lt_s64) == 8, s64_size);
#endif

/* Safe worst-case buffer size for formatting an int in decimal. */
#define LT_INT_DEC_BUF (sizeof(int) * CHAR_BIT / 3 + 3)

#define LT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LT_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LT_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define LT_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define LT_UNUSED(x) ((void)(x))

#define LT_PI 3.14159265358979323846

LT_BEGIN_DECLS

/* ---- overflow-checked arithmetic (UNIVERSAL_C section 5) ----
 * Return LT_FALSE and leave *out alone on overflow. */
lt_bool lt_add_int(int a, int b, int *out);
lt_bool lt_mul_int(int a, int b, int *out);
lt_bool lt_mul_size(size_t a, size_t b, size_t *out);
lt_bool lt_add_size(size_t a, size_t b, size_t *out);

/* ---- endian-independent (de)serialization of 32-bit values ---- */
void   lt_store_u32le(lt_u8 *p, lt_u32 v);
lt_u32 lt_load_u32le(const lt_u8 *p);
void   lt_store_u32be(lt_u8 *p, lt_u32 v);
lt_u32 lt_load_u32be(const lt_u8 *p);
lt_u32 lt_rotl32(lt_u32 x, unsigned n);
#if LT_HAS_64
/* decimal text of a 64-bit value; buf must hold >= 21 bytes; returns buf */
char *lt_u64_to_dec(char *buf, lt_u64 v);
#endif

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

LT_END_DECLS

#endif /* LT_H */
