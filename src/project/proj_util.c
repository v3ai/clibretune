/* proj_util.c - small helpers: UTC time, encodings, file copy, tree removal.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "proj_priv.h"
#include "../plat/plat.h"

unsigned long lt_proj_secs_now(void)
{
    time_t t = time(NULL);
    if (t == (time_t)-1) return 0;
    return (unsigned long)t;
}

/* Howard Hinnant's civil_from_days */
void lt_proj_utc_from_secs(unsigned long secs, char buf[21])
{
    long days = (long)(secs / 86400UL);
    long rem = (long)(secs % 86400UL);
    long z = days + 719468L;
    long era = z / 146097L;
    long doe = z - era * 146097L;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    lt_snprintf(buf, 21, "%04ld-%02ld-%02ldT%02ld:%02ld:%02ldZ", y, m, d, rem / 3600,
                (rem % 3600) / 60, rem % 60);
}

void lt_proj_utc_now(char buf[21])
{
    lt_proj_utc_from_secs(lt_proj_secs_now(), buf);
}

lt_bool lt_proj_utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t k;
        unsigned long cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
        else return LT_FALSE;
        if (i + k >= n) return LT_FALSE;
        {
            size_t j;
            for (j = 1; j <= k; j++) {
                if ((s[i + j] & 0xC0) != 0x80) return LT_FALSE;
                cp = (cp << 6) | (unsigned long)(s[i + j] & 0x3F);
            }
        }
        if (k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return LT_FALSE;
        if (k == 3 && (cp < 0x10000UL || cp > 0x10FFFFUL)) return LT_FALSE;
        i += k + 1;
    }
    return LT_TRUE;
}

void lt_proj_put_utf8(lt_buf *b, unsigned long cp)
{
    if (cp < 0x80) {
        lt_buf_putc(b, (int)cp);
    } else if (cp < 0x800) {
        lt_buf_putc(b, (int)(0xC0 | (cp >> 6)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000UL) {
        lt_buf_putc(b, (int)(0xE0 | (cp >> 12)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        lt_buf_putc(b, (int)(0xF0 | (cp >> 18)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

char *lt_proj_decode_latin1(const unsigned char *s, size_t n)
{
    lt_buf b;
    size_t i;
    if (lt_proj_utf8_valid(s, n)) {
        char *r = (char *)lt_malloc(n + 1);
        if (n > 0) memcpy(r, s, n);
        r[n] = '\0';
        return r;
    }
    lt_buf_init(&b);
    for (i = 0; i < n; i++) lt_proj_put_utf8(&b, s[i]);
    if (b.data == NULL) return lt_strdup("");
    return lt_buf_take(&b);
}

char *lt_proj_dup_or_null(const char *s)
{
    return s ? lt_strdup(s) : NULL;
}

void lt_proj_set_str(char **dst, const char *s)
{
    char *n = lt_proj_dup_or_null(s);
    lt_free(*dst);
    *dst = n;
}

/* Windows device names: a file or folder called CON, con.txt or "Nul " opens the device instead
 * (COM and LPT also with the superscript digits 1-3, which Windows counts as digits) */
static lt_bool win_reserved(const char *s, size_t n)
{
    static const char *const fixed[4] = { "CON", "PRN", "AUX", "NUL" };
    size_t i;
    while (n > 0 && s[n - 1] == ' ') n--;
    for (i = 0; i < 4; i++) {
        if (n == 3 && lt_strnicmp(s, fixed[i], 3) == 0) return LT_TRUE;
    }
    if (n >= 4 && (lt_strnicmp(s, "COM", 3) == 0 || lt_strnicmp(s, "LPT", 3) == 0)) {
        if (n == 4 && s[3] >= '1' && s[3] <= '9') return LT_TRUE;
        if (n == 5 && (unsigned char)s[3] == 0xC2 &&
            ((unsigned char)s[4] == 0xB9 || (unsigned char)s[4] == 0xB2 ||
             (unsigned char)s[4] == 0xB3)) return LT_TRUE;          /* UTF-8 superscript 1, 2, 3 */
    }
    return LT_FALSE;
}

/* A directory name that works on every OS, whichever one the project is created on (projects
 * move between machines): characters other than letters, digits, '-', '_', UTF-8 and (when
 * allowed) spaces become '_'; trailing dots and spaces go (Windows drops them silently, so two
 * names would meet); a Windows device name gets '_' after it ("con" -> "con_", "LPT1.x" ->
 * "LPT1_.x"). Never empty: "_" when nothing is left. */
char *lt_proj_safe_name(const char *name, lt_bool allow_space)
{
    size_t n = strlen(name), i, base;
    char *r = (char *)lt_malloc(n + 3);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c >= 0x80 || (allow_space && c == ' '))
            r[i] = (char)c;
        else
            r[i] = '_';
    }
    while (n > 0 && (r[n - 1] == ' ' || r[n - 1] == '.')) n--;
    if (n == 0) r[n++] = '_';
    r[n] = '\0';
    for (base = 0; base < n && r[base] != '.'; base++) {
    }
    if (win_reserved(r, base)) {
        memmove(r + base + 1, r + base, n - base + 1);
        r[base] = '_';
    }
    return r;
}

lt_bool lt_proj_plain_component(const char *name)
{
    if (name == NULL || name[0] == '\0') return LT_FALSE;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return LT_FALSE;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return LT_FALSE;
    /* drive letter "X:" */
    if (name[0] != '\0' && name[1] == ':') return LT_FALSE;
    return LT_TRUE;
}

long lt_proj_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

lt_bool lt_proj_is_file(const char *path)
{
    if (lt_plat_is_dir(path)) return LT_FALSE;
    return lt_file_exists(path);
}

int lt_proj_copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[8192];
    size_t n;
    int rc = LT_OK;
    if (!in) {
        lt_err_set("cannot open '%s' for reading", from);
        return LT_EIO;
    }
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        lt_err_set("cannot open '%s' for writing", to);
        return LT_EIO;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = LT_EIO;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) rc = LT_EIO;
    if (rc != LT_OK) lt_err_set("copy '%s' -> '%s' failed", from, to);
    return rc;
}

int lt_proj_write_atomic(const char *path, const void *data, size_t len)
{
    /* the old version removed the destination when the first rename failed, so a second
     * failure lost the file; lt_plat_write_atomic never touches it on failure */
    return lt_plat_write_atomic(path, data, len) == 0 ? LT_OK : LT_EIO;
}

int lt_proj_rmtree(const char *path)
{
    int rc = LT_OK;
    if (lt_plat_is_dir(path)) {
        char **names = NULL;
        int n = lt_plat_list_dir(path, &names), i;
        for (i = 0; i < n; i++) {
            char *sub = lt_path_join(path, names[i]);
            if (lt_proj_rmtree(sub) != LT_OK) rc = LT_EIO;
            lt_free(sub);
        }
        if (n >= 0) lt_plat_free_list(names, n);
    }
    if (lt_plat_remove(path) != 0) rc = LT_EIO;
    return rc;
}

char *lt_proj_join3(const char *a, const char *b, const char *c)
{
    char *ab = lt_path_join(a, b);
    char *r = lt_path_join(ab, c);
    lt_free(ab);
    return r;
}

void lt_proj_strv_free(char **v, size_t n)
{
    size_t i;
    if (v == NULL) return;
    for (i = 0; i < n; i++) lt_free(v[i]);
    lt_free(v);
}

int lt_proj_cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
