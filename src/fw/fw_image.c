/* fw_image.c - firmware image files: raw binary, Intel HEX, Motorola S-record, ST DfuSe.
 * Every parser feeds lt_fw_image_add(); lt_fw_image_finish() sorts the segments, rejects
 * overlaps and merges touching ones. Also: writers for round trips, cheap STM32 / rusEFI
 * image facts and the board presets of the firmware screen.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "fw.h"

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0) { return; }
    va_start(ap, fmt);
    (void)lt_vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

void lt_fw_image_init(lt_fw_image *img)
{
    memset(img, 0, sizeof(*img));
    img->dfu_vid = 0xFFFFu;
    img->dfu_pid = 0xFFFFu;
}

void lt_fw_image_free(lt_fw_image *img)
{
    size_t i;
    if (img == NULL) { return; }
    for (i = 0; i < img->n_segs; i++) { lt_free(img->segs[i].data); }
    lt_free(img->segs);
    lt_fw_image_init(img);
}

static int seg_reserve(lt_fw_seg *s, size_t need)
{
    size_t nc;
    lt_u8 *p;
    if (need <= s->cap) { return 0; }
    nc = s->cap != 0 ? s->cap : 4096;
    while (nc < need) {
        if (nc > LT_SIZE_MAX / 2) { nc = need; break; }
        nc *= 2;
    }
    p = (lt_u8 *)lt_realloc(s->data, nc);
    if (p == NULL) { return -1; }
    s->data = p;
    s->cap = nc;
    return 0;
}

int lt_fw_image_add(lt_fw_image *img, lt_u32 addr, const lt_u8 *data, size_t n, char *err, size_t errcap)
{
    lt_fw_seg *s;
    if (n == 0) { return 0; }
    if (n > LT_FW_MAX_DATA || (lt_u32)(n - 1) > (lt_u32)(0xFFFFFFFFUL - addr)) {
        set_err(err, errcap, "data at 0x%08lX runs past the 32-bit address space", (unsigned long)addr);
        return -1;
    }
    if ((unsigned long)img->total + (unsigned long)n > LT_FW_MAX_DATA) {
        set_err(err, errcap, "image too large (more than %lu MB of data)", LT_FW_MAX_DATA / (1024UL * 1024UL));
        return -1;
    }
    s = img->n_segs > 0 ? &img->segs[img->n_segs - 1] : NULL;
    if (s == NULL || addr == 0 || s->addr + (s->len - 1u) != addr - 1u) {
        if (img->n_segs == img->cap_segs) {
            size_t nc = img->cap_segs != 0 ? img->cap_segs * 2 : 8;
            lt_fw_seg *ns = (lt_fw_seg *)lt_realloc(img->segs, nc * sizeof(lt_fw_seg));
            if (ns == NULL) { set_err(err, errcap, "out of memory"); return -1; }
            img->segs = ns;
            img->cap_segs = nc;
        }
        s = &img->segs[img->n_segs++];
        memset(s, 0, sizeof(*s));
        s->addr = addr;
    }
    if (seg_reserve(s, (size_t)s->len + n) != 0) { set_err(err, errcap, "out of memory"); return -1; }
    memcpy(s->data + s->len, data, n);
    s->len += (lt_u32)n;
    img->total += (lt_u32)n;
    return 0;
}

static int seg_cmp(const void *a, const void *b)
{
    const lt_fw_seg *x = (const lt_fw_seg *)a, *y = (const lt_fw_seg *)b;
    if (x->addr != y->addr) { return x->addr < y->addr ? -1 : 1; }
    return (x->len > y->len) - (x->len < y->len);
}

int lt_fw_image_finish(lt_fw_image *img, char *err, size_t errcap)
{
    size_t i, k;
    if (img->n_segs == 0) { set_err(err, errcap, "no data in the file"); return -1; }
    qsort(img->segs, img->n_segs, sizeof(lt_fw_seg), seg_cmp);
    k = 0;
    for (i = 1; i < img->n_segs; i++) {
        lt_fw_seg *p = &img->segs[k], *s = &img->segs[i];
        lt_u32 plast = p->addr + (p->len - 1u);
        if (s->addr <= plast) {
            set_err(err, errcap, "overlapping data at 0x%08lX", (unsigned long)s->addr);
            return -1;
        }
        if (s->addr - 1u == plast) {
            if (seg_reserve(p, (size_t)p->len + s->len) != 0) { set_err(err, errcap, "out of memory"); return -1; }
            memcpy(p->data + p->len, s->data, s->len);
            p->len += s->len;
            lt_free(s->data);
            s->data = NULL;
        } else {
            k++;
            if (k != i) { img->segs[k] = *s; s->data = NULL; }
        }
    }
    img->n_segs = k + 1;
    img->lo = img->segs[0].addr;
    img->last = img->segs[k].addr + (img->segs[k].len - 1u);
    img->total = 0;
    for (i = 0; i < img->n_segs; i++) { img->total += img->segs[i].len; }
    return 0;
}

/* ------------------------------------------------------------------ text records */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    switch (c) {
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return -1;
    }
}

/* hex byte pairs from p[*pos] up to the end of the line into b (max cap); count or -1 */
static int read_hex_line(const char *p, size_t len, size_t *pos, lt_u8 *b, int cap)
{
    size_t i = *pos;
    int n = 0;
    while (i < len && hexval((unsigned char)p[i]) >= 0) {
        int hi = hexval((unsigned char)p[i]), lo;
        if (i + 1 >= len) { return -1; }
        lo = hexval((unsigned char)p[i + 1]);
        if (lo < 0 || n >= cap) { return -1; }
        b[n++] = (lt_u8)(hi * 16 + lo);
        i += 2;
    }
    while (i < len && (p[i] == ' ' || p[i] == '\t')) { i++; }
    if (i < len && p[i] != '\r' && p[i] != '\n') { return -1; }
    *pos = i;
    return n;
}

static int is_space_ch(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int parse_ihex(lt_fw_image *img, const char *p, size_t len, char *err, size_t errcap)
{
    size_t pos = 0;
    unsigned long line = 1;
    lt_u32 base = 0;
    lt_u8 b[300];
    int eof = 0;
    while (pos < len && !eof) {
        int n, i;
        unsigned sum = 0;
        lt_u32 off;
        while (pos < len && is_space_ch((unsigned char)p[pos])) {
            if (p[pos] == '\n') { line++; }
            pos++;
        }
        if (pos >= len) { break; }
        if (p[pos] != ':') { set_err(err, errcap, "Intel HEX line %lu: expected ':'", line); return -1; }
        pos++;
        n = read_hex_line(p, len, &pos, b, (int)sizeof(b));
        if (n < 5 || n != (int)b[0] + 5) {
            set_err(err, errcap, "Intel HEX line %lu: bad record length", line);
            return -1;
        }
        for (i = 0; i < n; i++) { sum += b[i]; }
        if ((sum & 0xFFu) != 0) { set_err(err, errcap, "Intel HEX line %lu: checksum error", line); return -1; }
        off = ((lt_u32)b[1] << 8) | b[2];
        switch (b[3]) {
        case 0x00:
            if (lt_fw_image_add(img, base + off, b + 4, b[0], err, errcap) != 0) { return -1; }
            break;
        case 0x01: eof = 1; break;
        case 0x02:
        case 0x04:
            if (b[0] != 2) { set_err(err, errcap, "Intel HEX line %lu: bad address record", line); return -1; }
            base = ((lt_u32)b[4] << 8) | b[5];
            base = b[3] == 0x02 ? base << 4 : base << 16;
            break;
        case 0x03:
        case 0x05:
            if (b[0] != 4) { set_err(err, errcap, "Intel HEX line %lu: bad start record", line); return -1; }
            if (b[3] == 0x05) { img->entry = lt_load_u32be(b + 4); }
            else { img->entry = (((lt_u32)b[4] << 8 | b[5]) << 4) + ((lt_u32)b[6] << 8 | b[7]); }
            img->has_entry = 1;
            break;
        default:
            set_err(err, errcap, "Intel HEX line %lu: unsupported record type %02X", line, (unsigned)b[3]);
            return -1;
        }
    }
    if (!eof) { set_err(err, errcap, "Intel HEX: no end-of-file record (file truncated?)"); return -1; }
    return 0;
}

static int parse_srec(lt_fw_image *img, const char *p, size_t len, char *err, size_t errcap)
{
    size_t pos = 0;
    unsigned long line = 1;
    lt_u8 b[300];
    int end = 0;
    while (pos < len && !end) {
        int n, i, t, alen;
        unsigned sum = 0;
        lt_u32 addr = 0;
        while (pos < len && is_space_ch((unsigned char)p[pos])) {
            if (p[pos] == '\n') { line++; }
            pos++;
        }
        if (pos >= len) { break; }
        if (p[pos] != 'S' || pos + 1 >= len || p[pos + 1] < '0' || p[pos + 1] > '9') {
            set_err(err, errcap, "S-record line %lu: expected S0..S9", line);
            return -1;
        }
        t = p[pos + 1] - '0';
        pos += 2;
        n = read_hex_line(p, len, &pos, b, (int)sizeof(b));
        if (n < 2 || n != (int)b[0] + 1) { set_err(err, errcap, "S-record line %lu: bad length", line); return -1; }
        for (i = 0; i < n; i++) { sum += b[i]; }
        if ((sum & 0xFFu) != 0xFFu) { set_err(err, errcap, "S-record line %lu: checksum error", line); return -1; }
        switch (t) {
        case 1: case 9: case 5: alen = 2; break;
        case 2: case 8: case 6: alen = 3; break;
        case 3: case 7: alen = 4; break;
        case 0: alen = 2; break;
        default: set_err(err, errcap, "S-record line %lu: unsupported type S%d", line, t); return -1;
        }
        if (n < 2 + alen) { set_err(err, errcap, "S-record line %lu: record too short", line); return -1; }
        for (i = 0; i < alen; i++) { addr = (addr << 8) | b[1 + i]; }
        if (t >= 1 && t <= 3) {
            if (lt_fw_image_add(img, addr, b + 1 + alen, (size_t)(n - 2 - alen), err, errcap) != 0) { return -1; }
        } else if (t >= 7) {
            img->entry = addr;
            img->has_entry = 1;
            end = 1;
        }
    }
    if (!end) { set_err(err, errcap, "S-record: no termination record S7/S8/S9 (file truncated?)"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ DfuSe */
lt_u32 lt_fw_dfu_crc(const lt_u8 *p, size_t n)
{
    return (lt_u32)(~lt_crc32(0, p, n) & LT_U32_MASK);
}

#define DFU_PREFIX 11
#define DFU_TARGET 274
#define DFU_SUFFIX 16

static int parse_dfuse(lt_fw_image *img, const lt_u8 *d, size_t len, char *err, size_t errcap)
{
    size_t pos, body, slen;
    lt_u32 crc;
    int nt, t;
    if (len < DFU_PREFIX + DFU_SUFFIX || memcmp(d, "DfuSe", 5) != 0) {
        set_err(err, errcap, "DfuSe: not a DfuSe file (missing prefix)");
        return -1;
    }
    if (d[5] != 0x01) { set_err(err, errcap, "DfuSe: unsupported version %u", (unsigned)d[5]); return -1; }
    slen = d[len - 5];
    if (d[len - 8] != 'U' || d[len - 7] != 'F' || d[len - 6] != 'D' || slen < DFU_SUFFIX ||
        slen > len - DFU_PREFIX) {
        set_err(err, errcap, "DfuSe: missing DFU suffix (\"UFD\" signature)");
        return -1;
    }
    crc = lt_load_u32le(d + len - 4);
    if (crc != lt_fw_dfu_crc(d, len - 4)) {
        set_err(err, errcap, "DfuSe: CRC mismatch (file damaged or truncated)");
        return -1;
    }
    body = len - slen;
    if ((size_t)lt_load_u32le(d + 6) != body) {
        set_err(err, errcap, "DfuSe: image size field does not match the file");
        return -1;
    }
    img->dfu_vid = (lt_u16)(d[len - 12] | (d[len - 11] << 8));
    img->dfu_pid = (lt_u16)(d[len - 14] | (d[len - 13] << 8));
    nt = d[10];
    if (nt == 0) { set_err(err, errcap, "DfuSe: no targets"); return -1; }
    pos = DFU_PREFIX;
    for (t = 0; t < nt; t++) {
        lt_u32 tsize, nel, e;
        size_t tend;
        int alt;
        if (body - pos < DFU_TARGET || memcmp(d + pos, "Target", 6) != 0) {
            set_err(err, errcap, "DfuSe: target %d header missing or truncated", t + 1);
            return -1;
        }
        alt = d[pos + 6];
        tsize = lt_load_u32le(d + pos + 266);
        nel = lt_load_u32le(d + pos + 270);
        if (t == 0) {
            img->dfu_alt = alt;
            if (lt_load_u32le(d + pos + 7) != 0) {
                size_t k;
                for (k = 0; k < sizeof(img->dfu_target) - 1 && d[pos + 11 + k] != 0; k++) {
                    char c = (char)d[pos + 11 + k];
                    img->dfu_target[k] = (c >= ' ' && c <= '~') ? c : '?';
                }
                img->dfu_target[k] = '\0';
            }
        }
        pos += DFU_TARGET;
        if ((size_t)tsize > body - pos) { set_err(err, errcap, "DfuSe: target %d size past the end", t + 1); return -1; }
        tend = pos + tsize;
        for (e = 0; e < nel; e++) {
            lt_u32 ea, es;
            if (tend - pos < 8) { set_err(err, errcap, "DfuSe: element %lu truncated", (unsigned long)e + 1); return -1; }
            ea = lt_load_u32le(d + pos);
            es = lt_load_u32le(d + pos + 4);
            pos += 8;
            if ((size_t)es > tend - pos) {
                set_err(err, errcap, "DfuSe: element at 0x%08lX past the end of its target", (unsigned long)ea);
                return -1;
            }
            /* only the first target's alternate setting is written (others: option bytes...) */
            if (alt == img->dfu_alt && lt_fw_image_add(img, ea, d + pos, es, err, errcap) != 0) { return -1; }
            pos += es;
        }
        if (pos != tend) { set_err(err, errcap, "DfuSe: target %d size does not match its elements", t + 1); return -1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ front end */
int lt_fw_format_from_name(const char *path)
{
    const char *e = path != NULL ? lt_path_ext(path) : NULL;
    if (e == NULL || *e == '\0') { return LT_FW_FMT_UNKNOWN; }
    if (*e == '.') { e++; }
    if (lt_stricmp(e, "bin") == 0) { return LT_FW_FMT_BIN; }
    if (lt_stricmp(e, "hex") == 0 || lt_stricmp(e, "ihex") == 0 || lt_stricmp(e, "ihx") == 0) { return LT_FW_FMT_IHEX; }
    if (lt_stricmp(e, "srec") == 0 || lt_stricmp(e, "s19") == 0 || lt_stricmp(e, "s28") == 0 ||
        lt_stricmp(e, "s37") == 0 || lt_stricmp(e, "mot") == 0) { return LT_FW_FMT_SREC; }
    if (lt_stricmp(e, "dfu") == 0) { return LT_FW_FMT_DFUSE; }
    return LT_FW_FMT_UNKNOWN;
}

const char *lt_fw_format_name(int format)
{
    switch (format) {
    case LT_FW_FMT_BIN: return "raw binary";
    case LT_FW_FMT_IHEX: return "Intel HEX";
    case LT_FW_FMT_SREC: return "Motorola S-record";
    case LT_FW_FMT_DFUSE: return "ST DfuSe";
    default: return "unknown";
    }
}

static int sniff(const lt_u8 *d, size_t len)
{
    size_t i = 0;
    if (len >= 5 && memcmp(d, "DfuSe", 5) == 0) { return LT_FW_FMT_DFUSE; }
    while (i < len && is_space_ch(d[i])) { i++; }
    if (i + 2 < len && d[i] == ':' && hexval(d[i + 1]) >= 0 && hexval(d[i + 2]) >= 0) { return LT_FW_FMT_IHEX; }
    if (i + 3 < len && d[i] == 'S' && d[i + 1] >= '0' && d[i + 1] <= '9' && hexval(d[i + 2]) >= 0) {
        return LT_FW_FMT_SREC;
    }
    return LT_FW_FMT_BIN;
}

int lt_fw_image_parse(lt_fw_image *img, const lt_u8 *data, size_t len, int format, lt_u32 bin_base,
                      char *err, size_t errcap)
{
    int rc;
    lt_fw_image_init(img);
    if (err != NULL && errcap > 0) { err[0] = '\0'; }
    if (data == NULL || len == 0) { set_err(err, errcap, "empty file"); return -1; }
    if (format == LT_FW_FMT_UNKNOWN) { format = sniff(data, len); }
    img->format = format;
    switch (format) {
    case LT_FW_FMT_IHEX: rc = parse_ihex(img, (const char *)data, len, err, errcap); break;
    case LT_FW_FMT_SREC: rc = parse_srec(img, (const char *)data, len, err, errcap); break;
    case LT_FW_FMT_DFUSE: rc = parse_dfuse(img, data, len, err, errcap); break;
    default: rc = lt_fw_image_add(img, bin_base, data, len, err, errcap); break;
    }
    if (rc == 0) { rc = lt_fw_image_finish(img, err, errcap); }
    if (rc != 0) {
        lt_fw_image_free(img);
        img->format = format;
        return -1;
    }
    return 0;
}

int lt_fw_image_load(lt_fw_image *img, const char *path, lt_u32 bin_base, char *err, size_t errcap)
{
    size_t len = 0;
    char *d = lt_file_read(path, &len);
    int rc;
    if (d == NULL) {
        lt_fw_image_init(img);
        set_err(err, errcap, "cannot read %s", path);
        return -1;
    }
    rc = lt_fw_image_parse(img, (const lt_u8 *)d, len, lt_fw_format_from_name(path), bin_base, err, errcap);
    lt_free(d);
    return rc;
}

lt_u32 lt_fw_image_read(const lt_fw_image *img, lt_u32 addr, lt_u8 *out, lt_u32 n, lt_u8 fill)
{
    size_t i;
    lt_u32 got = 0;
    if (n == 0) { return 0; }
    memset(out, fill, n);
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        lt_u32 a0, a1, slast = s->addr + (s->len - 1u), rlast = addr + (n - 1u);
        if (rlast < addr) { rlast = 0xFFFFFFFFUL; }
        if (s->addr > rlast || slast < addr) { continue; }
        a0 = s->addr > addr ? s->addr : addr;
        a1 = slast < rlast ? slast : rlast;
        memcpy(out + (a0 - addr), s->data + (a0 - s->addr), (size_t)(a1 - a0) + 1u);
        got += a1 - a0 + 1u;
    }
    return got;
}

int lt_fw_image_touches(const lt_fw_image *img, lt_u32 addr, lt_u32 len)
{
    size_t i;
    lt_u32 last;
    if (len == 0) { return 0; }
    last = addr + (len - 1u);
    if (last < addr) { last = 0xFFFFFFFFUL; }
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        if (s->addr <= last && s->addr + (s->len - 1u) >= addr) { return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ writers */
void lt_fw_write_ihex(const lt_fw_image *img, lt_buf *out)
{
    size_t i;
    lt_u32 upper = 0;
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        lt_u32 off = 0;
        while (off < s->len) {
            lt_u32 a = s->addr + off, n = s->len - off, k;
            unsigned sum;
            if (n > 16) { n = 16; }
            if (0x10000UL - (a & 0xFFFFUL) < n) { n = 0x10000UL - (a & 0xFFFFUL); }
            if ((a >> 16) != upper || (i == 0 && off == 0)) {
                upper = a >> 16;
                sum = 2u + 4u + (unsigned)(upper >> 8) + (unsigned)(upper & 0xFFu);
                lt_buf_printf(out, ":02000004%04X%02X\n", (unsigned)upper, (unsigned)((0x100u - (sum & 0xFFu)) & 0xFFu));
            }
            sum = (unsigned)n + (unsigned)((a >> 8) & 0xFFu) + (unsigned)(a & 0xFFu);
            lt_buf_printf(out, ":%02X%04X00", (unsigned)n, (unsigned)(a & 0xFFFFu));
            for (k = 0; k < n; k++) {
                sum += s->data[off + k];
                lt_buf_printf(out, "%02X", (unsigned)s->data[off + k]);
            }
            lt_buf_printf(out, "%02X\n", (unsigned)((0x100u - (sum & 0xFFu)) & 0xFFu));
            off += n;
        }
    }
    if (img->has_entry) {
        unsigned sum = 4u + 5u;
        int k;
        for (k = 0; k < 4; k++) { sum += (unsigned)((img->entry >> (24 - 8 * k)) & 0xFFu); }
        lt_buf_printf(out, ":04000005%08lX%02X\n", (unsigned long)img->entry, (unsigned)((0x100u - (sum & 0xFFu)) & 0xFFu));
    }
    lt_buf_puts(out, ":00000001FF\n");
}

static void srec_line(lt_buf *out, int type, lt_u32 addr, int alen, const lt_u8 *d, lt_u32 n)
{
    unsigned sum = (unsigned)(n + (lt_u32)alen + 1u);
    int k;
    lt_u32 i;
    lt_buf_printf(out, "S%d%02X", type, sum & 0xFFu);
    for (k = alen - 1; k >= 0; k--) {
        unsigned v = (unsigned)((addr >> (8 * k)) & 0xFFu);
        sum += v;
        lt_buf_printf(out, "%02X", v);
    }
    for (i = 0; i < n; i++) {
        sum += d[i];
        lt_buf_printf(out, "%02X", (unsigned)d[i]);
    }
    lt_buf_printf(out, "%02X\n", (~sum) & 0xFFu);
}

void lt_fw_write_srec(const lt_fw_image *img, lt_buf *out)
{
    static const lt_u8 hdr[9] = { 'l', 'i', 'b', 'r', 'e', 't', 'u', 'n', 'e' };
    size_t i;
    srec_line(out, 0, 0, 2, hdr, 9);
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        lt_u32 off;
        for (off = 0; off < s->len; off += 16) {
            lt_u32 n = s->len - off < 16 ? s->len - off : 16;
            srec_line(out, 3, s->addr + off, 4, s->data + off, n);
        }
    }
    srec_line(out, 7, img->has_entry ? img->entry : 0, 4, NULL, 0);
}

void lt_fw_write_dfuse(const lt_fw_image *img, lt_u16 vid, lt_u16 pid, int alt, const char *target,
                       lt_buf *out)
{
    size_t start = out->len, i;
    lt_u8 h[DFU_TARGET], w[8];
    lt_u32 tsize = 0;
    for (i = 0; i < img->n_segs; i++) { tsize += 8u + img->segs[i].len; }
    memset(h, 0, sizeof(h));
    memcpy(h, "DfuSe", 5);
    h[5] = 1;
    h[10] = 1;
    lt_buf_append(out, h, DFU_PREFIX);                     /* size patched below */
    memset(h, 0, sizeof(h));
    memcpy(h, "Target", 6);
    h[6] = (lt_u8)alt;
    if (target != NULL && target[0] != '\0') {
        lt_store_u32le(h + 7, 1);
        memcpy(h + 11, target, strlen(target) < 254 ? strlen(target) : 254);
    }
    lt_store_u32le(h + 266, tsize);
    lt_store_u32le(h + 270, (lt_u32)img->n_segs);
    lt_buf_append(out, h, DFU_TARGET);
    for (i = 0; i < img->n_segs; i++) {
        lt_store_u32le(w, img->segs[i].addr);
        lt_store_u32le(w + 4, img->segs[i].len);
        lt_buf_append(out, w, 8);
        lt_buf_append(out, img->segs[i].data, img->segs[i].len);
    }
    lt_store_u32le((lt_u8 *)out->data + start + 6, (lt_u32)(out->len - start));
    memset(h, 0, 16);
    h[0] = 0xFF; h[1] = 0xFF;                               /* bcdDevice: any */
    h[2] = (lt_u8)(pid & 0xFFu); h[3] = (lt_u8)(pid >> 8);
    h[4] = (lt_u8)(vid & 0xFFu); h[5] = (lt_u8)(vid >> 8);
    h[6] = 0x1A; h[7] = 0x01;                               /* bcdDFU 1.1a */
    h[8] = 'U'; h[9] = 'F'; h[10] = 'D';
    h[11] = DFU_SUFFIX;
    lt_buf_append(out, h, 12);
    lt_store_u32le(w, lt_fw_dfu_crc((const lt_u8 *)out->data + start, out->len - start));
    lt_buf_append(out, w, 4);
}

/* ------------------------------------------------------------------ facts */
static int sp_ok(lt_u32 sp)
{
    if ((sp & 3u) != 0) { return 0; }
    return (sp > 0x20000000UL && sp <= 0x20200000UL) || (sp > 0x24000000UL && sp <= 0x24200000UL);
}

static int vec_ok(const lt_fw_image *img, lt_u32 base, lt_u32 end_excl, lt_u32 *sp, lt_u32 *reset)
{
    lt_u8 v[8];
    lt_u32 r;
    if (lt_fw_image_read(img, base, v, 8, 0xFF) != 8) { return -1; }
    *sp = lt_load_u32le(v);
    *reset = lt_load_u32le(v + 4);
    r = *reset & ~(lt_u32)1u;
    return (sp_ok(*sp) && (*reset & 1u) != 0 && r >= base && r < end_excl && r <= img->last) ? 1 : 0;
}

static void add_warn(lt_fw_facts *f, const char *fmt, ...)
{
    va_list ap;
    if (f->n_warn >= 4) { return; }
    va_start(ap, fmt);
    (void)lt_vsnprintf(f->warn[f->n_warn], sizeof(f->warn[0]), fmt, ap);
    va_end(ap);
    f->n_warn++;
}

void lt_fw_image_facts(const lt_fw_image *img, lt_fw_facts *f)
{
    lt_u32 sp = 0, reset = 0, end;
    memset(f, 0, sizeof(*f));
    f->vectors = -1;
    f->app_vectors = -1;
    f->app_crc = -1;
    if (img->n_segs == 0) { return; }
    end = img->last + 1u;
    if (end == 0) { end = 0xFFFFFFFFUL; }
    if (img->lo >= 0x08000000UL && img->lo < 0x08200000UL) {
        f->vectors = vec_ok(img, img->lo, end, &f->sp, &f->reset);
        if (f->vectors == 0) {
            add_warn(f, "The vector table at 0x%08lX looks wrong (stack 0x%08lX, reset 0x%08lX): "
                        "wrong base address, or not an STM32 image?",
                     (unsigned long)img->lo, (unsigned long)f->sp, (unsigned long)f->reset);
        }
    } else if (img->lo < 0x08000000UL || img->lo >= 0x08200000UL) {
        add_warn(f, "The image starts at 0x%08lX, outside the usual STM32 flash at 0x08000000.",
                 (unsigned long)img->lo);
    }
    if (img->lo == LT_FW_STM32_FLASH && f->vectors == 1 && f->reset < LT_FW_RUSEFI_APP &&
        vec_ok(img, LT_FW_RUSEFI_APP, end, &sp, &reset) == 1) {
        f->merged_openblt = 1;
    }
    if (f->merged_openblt || img->lo == LT_FW_RUSEFI_APP) {
        lt_u8 h[0x24];
        f->app_vectors = vec_ok(img, LT_FW_RUSEFI_APP, end, &sp, &reset);
        if (lt_fw_image_read(img, LT_FW_RUSEFI_APP, h, sizeof(h), 0xFF) == sizeof(h)) {
            lt_u32 stored = lt_load_u32le(h + 0x1C), size = lt_load_u32le(h + 0x20);
            if (size >= 0x24 && size <= img->last - LT_FW_RUSEFI_APP + 1u) {
                lt_u8 *buf = (lt_u8 *)lt_malloc(size);
                if (buf != NULL) {
                    lt_u32 crc;
                    (void)lt_fw_image_read(img, LT_FW_RUSEFI_APP, buf, size, 0xFF);
                    crc = lt_crc32(0, buf, 0x1C);
                    crc = lt_crc32(crc, buf + 0x20, (size_t)size - 0x20u);
                    f->app_crc = crc == stored ? 1 : 0;
                    f->app_size = size;
                    lt_free(buf);
                }
            } else if (f->merged_openblt) {
                f->app_crc = 0;
            }
        }
        if (f->app_crc == 0) {
            add_warn(f, "The rusEFI app CRC header at 0x08008000 does not match: OpenBLT would not start "
                        "this app (fine for firmware without OpenBLT).");
        }
    }
    if (img->has_entry && (img->entry < img->lo || img->entry > img->last)) {
        add_warn(f, "The start address 0x%08lX lies outside the image.", (unsigned long)img->entry);
    }
}

void lt_fw_image_summary(const lt_fw_image *img, char *out, size_t cap)
{
    char sz[32];
    if (img->n_segs == 0) { lt_strlcpy(out, "no image", cap); return; }
    if (img->total < 1024u) { lt_snprintf(sz, sizeof(sz), "%lu bytes", (unsigned long)img->total); }
    else { lt_snprintf(sz, sizeof(sz), "%.1f KB", (double)img->total / 1024.0); }
    lt_snprintf(out, cap, "%s, %lu segment%s, %s, 0x%08lX-0x%08lX", lt_fw_format_name(img->format),
                (unsigned long)img->n_segs, img->n_segs == 1 ? "" : "s", sz, (unsigned long)img->lo,
                (unsigned long)img->last);
}

/* ------------------------------------------------------------------ presets */
static const lt_fw_range rusefi_f4_cfg[1] = {
    { LT_FW_RUSEFI_CFG, LT_FW_RUSEFI_CFG_LEN, "ECU settings (flash sectors 10-11)" }
};

static const lt_fw_preset presets[LT_FW_PRESET_COUNT] = {
    { "miata-ecus (rusEFI, STM32F407)", LT_FW_STM32_FLASH, LT_FW_RUSEFI_APP, rusefi_f4_cfg, 1, 1, 1, 1,
      "https://github.com/v3ai/miata-ecus/actions/workflows/firmware.yaml" },
    { "rusEFI / FOME / epicEFI (STM32F4/F7/H7 DFU)", LT_FW_STM32_FLASH, LT_FW_RUSEFI_APP, rusefi_f4_cfg, 1,
      1, 1, 0, NULL },
    { "Generic STM32 DFU (raw .bin)", LT_FW_STM32_FLASH, 0, NULL, 0, 1, 0, 0, NULL }
};

const lt_fw_preset *lt_fw_preset_get(int idx)
{
    if (idx < 0 || idx >= LT_FW_PRESET_COUNT) { idx = LT_FW_PRESET_GENERIC; }
    return &presets[idx];
}

static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay != '\0'; hay++) {
        if (lt_strnicmp(hay, needle, n) == 0) { return 1; }
    }
    return 0;
}

int lt_fw_preset_for_signature(const char *sig)
{
    if (sig == NULL) { return LT_FW_PRESET_GENERIC; }
    if (has_ci(sig, "miata-ecu") || has_ci(sig, "miata_ecu")) { return LT_FW_PRESET_MIATA; }
    if (has_ci(sig, "rusefi") || has_ci(sig, "fome") || has_ci(sig, "epicefi")) { return LT_FW_PRESET_RUSEFI; }
    return LT_FW_PRESET_GENERIC;
}
