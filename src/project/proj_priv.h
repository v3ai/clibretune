/* proj_priv.h - helpers shared by the project module's .c files (not public).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PROJ_PRIV_H
#define LT_PROJ_PRIV_H

#include "project.h"

/* UTC time "YYYY-MM-DDTHH:MM:SSZ" into buf[21] */
unsigned long lt_proj_secs_now(void);
void lt_proj_utc_from_secs(unsigned long secs, char buf[21]);
void lt_proj_utc_now(char buf[21]);

/* encodings */
lt_bool lt_proj_utf8_valid(const unsigned char *s, size_t n);
void    lt_proj_put_utf8(lt_buf *b, unsigned long cp);
/* bytes -> UTF-8 text: valid UTF-8 as is, else Latin-1. malloc'd, NUL-terminated. */
char   *lt_proj_decode_latin1(const unsigned char *s, size_t n);

/* strings / paths */
char  *lt_proj_dup_or_null(const char *s);                  /* NULL-safe strdup */
void   lt_proj_set_str(char **dst, const char *s);          /* frees *dst, copies s (NULL ok) */
char  *lt_proj_safe_name(const char *name, lt_bool allow_space);
lt_bool lt_proj_plain_component(const char *name);          /* no separators, not . / .. */
long   lt_proj_file_size(const char *path);                 /* -1 if unreadable */
int    lt_proj_copy_file(const char *from, const char *to); /* binary exact; 0 / LT_EIO */
int    lt_proj_write_atomic(const char *path, const void *data, size_t len);
int    lt_proj_rmtree(const char *path);                    /* files and dirs, 0 / LT_EIO */
char  *lt_proj_join3(const char *a, const char *b, const char *c);
void   lt_proj_strv_free(char **v, size_t n);
int    lt_proj_cmp_str(const void *a, const void *b);       /* qsort over char ** */
lt_bool lt_proj_is_file(const char *path);                  /* exists and not a directory */

/* config (de)serialisation */
void lt_proj_config_clear(lt_proj_config *c);
int  lt_proj_config_read(lt_proj_config *c, const char *path);
int  lt_proj_config_write(const lt_proj_config *c, const char *path);

#endif /* LT_PROJ_PRIV_H */
