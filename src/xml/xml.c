/* xml.c - tolerant XML DOM parser and escaping helpers. See xml.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "xml.h"

#define XML_MAX_DEPTH 512

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

static void put_utf8(lt_buf *b, unsigned long cp)
{
    char t[4];
    if (cp < 0x80UL) {
        t[0] = (char)cp;
        lt_buf_append(b, t, 1);
    } else if (cp < 0x800UL) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        lt_buf_append(b, t, 2);
    } else if (cp < 0x10000UL) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        lt_buf_append(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        lt_buf_append(b, t, 4);
    }
}

/* strict UTF-8 validation (no overlongs, no surrogates, <= U+10FFFF) */
static int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            unsigned c1;
            if (i + 2 >= n) return 0;
            c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return 0;
            if (c == 0xE0 && c1 < 0xA0) return 0;
            if (c == 0xED && c1 > 0x9F) return 0;
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            unsigned c1;
            if (i + 3 >= n) return 0;
            c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80) return 0;
            if (c == 0xF0 && c1 < 0x90) return 0;
            if (c == 0xF4 && c1 > 0x8F) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* entity decoding                                                    */
/* ------------------------------------------------------------------ */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Try to decode the entity starting at s[i] == '&'. On success appends to out
 * and returns the number of bytes consumed; otherwise returns 0. */
static size_t decode_entity(const char *s, size_t n, size_t i, lt_buf *out)
{
    size_t j, semi = 0, len;
    const char *nm;
    for (j = 1; j <= 10 && i + j < n; j++) {
        if (s[i + j] == ';') { semi = j; break; }
    }
    if (semi < 2) return 0;
    nm = s + i + 1;
    len = semi - 1;
    if (nm[0] == '#') {
        unsigned long cp = 0;
        size_t k;
        int base = 10;
        k = 1;
        if (k < len && (nm[k] == 'x' || nm[k] == 'X')) { base = 16; k++; }
        if (k >= len) return 0;
        for (; k < len; k++) {
            int d = hexval((unsigned char)nm[k]);
            if (d < 0 || d >= base) return 0;
            cp = cp * (unsigned long)base + (unsigned long)d;
            if (cp > 0x10FFFFUL) return 0;
        }
        if (cp == 0 || (cp >= 0xD800UL && cp <= 0xDFFFUL)) return 0;
        put_utf8(out, cp);
        return semi + 1;
    }
    if (len == 2 && memcmp(nm, "lt", 2) == 0) { lt_buf_putc(out, '<'); return semi + 1; }
    if (len == 2 && memcmp(nm, "gt", 2) == 0) { lt_buf_putc(out, '>'); return semi + 1; }
    if (len == 3 && memcmp(nm, "amp", 3) == 0) { lt_buf_putc(out, '&'); return semi + 1; }
    if (len == 4 && memcmp(nm, "quot", 4) == 0) { lt_buf_putc(out, '"'); return semi + 1; }
    if (len == 4 && memcmp(nm, "apos", 4) == 0) { lt_buf_putc(out, '\''); return semi + 1; }
    return 0;
}

/* decode s[0..n) appending to out; normalize maps raw \t \n \r to ' ' */
static void decode_into(const char *s, size_t n, lt_buf *out, int normalize)
{
    size_t i = 0, start = 0;
    while (i < n) {
        char c = s[i];
        if (c == '&') {
            size_t used;
            if (i > start) lt_buf_append(out, s + start, i - start);
            used = decode_entity(s, n, i, out);
            if (used == 0) { lt_buf_putc(out, '&'); used = 1; }
            i += used;
            start = i;
        } else if (normalize && (c == '\t' || c == '\n' || c == '\r')) {
            if (i > start) lt_buf_append(out, s + start, i - start);
            lt_buf_putc(out, ' ');
            i++;
            start = i;
        } else {
            i++;
        }
    }
    if (i > start) lt_buf_append(out, s + start, i - start);
}

static char *buf_take_str(lt_buf *b)
{
    char *r;
    if (b->len == 0 || b->data == NULL) {
        lt_buf_free(b);
        return lt_strdup("");
    }
    r = lt_buf_take(b);
    return r;
}

char *lt_xml_unescape(const char *s)
{
    lt_buf b;
    if (s == NULL) return lt_strdup("");
    lt_buf_init(&b);
    decode_into(s, strlen(s), &b, 0);
    return buf_take_str(&b);
}

void lt_xml_put_escaped(lt_buf *b, const char *s, int mode)
{
    const char *p, *run;
    if (s == NULL) return;
    run = s;
    for (p = s; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': if (mode != 1) rep = "&quot;"; break;
        case '\'': if (mode != 1) rep = "&apos;"; break;
        case '\n': if (mode == 2) rep = "&#10;"; break;
        case '\r': if (mode == 2) rep = "&#13;"; break;
        case '\t': if (mode == 2) rep = "&#9;"; break;
        default: break;
        }
        if (rep) {
            if (p > run) lt_buf_append(b, run, (size_t)(p - run));
            lt_buf_puts(b, rep);
            run = p + 1;
        }
    }
    if (p > run) lt_buf_append(b, run, (size_t)(p - run));
}

char *lt_xml_escape(const char *s)
{
    lt_buf b;
    lt_buf_init(&b);
    lt_xml_put_escaped(&b, s, 0);
    return buf_take_str(&b);
}

char *lt_xml_comment_escape(const char *s)
{
    lt_buf b;
    size_t i = 0;
    if (s == NULL) s = "";
    lt_buf_init(&b);
    while (s[i]) {
        if (s[i] == '-' && s[i + 1] == '-') {
            lt_buf_puts(&b, "- -");
            i += 2;
        } else {
            lt_buf_putc(&b, s[i]);
            i++;
        }
    }
    if (b.len > 0 && b.data[b.len - 1] == '-') lt_buf_putc(&b, ' ');
    return buf_take_str(&b);
}

/* ------------------------------------------------------------------ */
/* parser                                                             */
/* ------------------------------------------------------------------ */

typedef struct frame {
    lt_xml_node *node;
    lt_buf text;
    lt_buf raw;
    lt_vec kids;    /* of lt_xml_node * */
} frame;

typedef struct parser {
    const char *s;
    size_t n;
    size_t i;
    unsigned flags;
    lt_vec frames;      /* of frame */
    lt_xml_node *root;
    int failed;
    char msg[160];
} parser;

static void p_fail(parser *p, size_t pos, const char *what)
{
    unsigned long line = 1;
    size_t k;
    if (p->failed) return;
    for (k = 0; k < pos && k < p->n; k++) if (p->s[k] == '\n') line++;
    lt_snprintf(p->msg, sizeof p->msg, "XML error at line %lu: %s", line, what);
    p->failed = 1;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int is_name_start(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80;
}

static int is_name_stop(int c)
{
    return is_ws(c) || c == '/' || c == '>' || c == '=' || c == '<' || c == '"' || c == '\'';
}

static char *xml_drop_prefix(const char *a, size_t n, unsigned flags)
{
    size_t k, start = 0;
    if (flags & LT_XML_STRIP_NS) {
        for (k = 0; k < n; k++) if (a[k] == ':') start = k + 1;
        if (start == n) start = 0;  /* trailing ':' -> keep whole name */
    }
    return lt_strndup(a + start, n - start);
}

/* find sub (len m) in s[from..n); returns index or n */
static size_t find_sub(const char *s, size_t n, size_t from, const char *sub, size_t m)
{
    size_t k;
    if (n < m) return n;
    for (k = from; k + m <= n; k++) {
        if (s[k] == sub[0] && memcmp(s + k, sub, m) == 0) return k;
    }
    return n;
}

static frame *xml_cur_frame(parser *p)
{
    if (p->frames.len == 0) return NULL;
    return (frame *)lt_vec_at(&p->frames, p->frames.len - 1);
}

/* Finalize the top frame into its node and pop it. */
static void pop_frame(parser *p)
{
    frame *f = xml_cur_frame(p);
    lt_xml_node *nd;
    if (f == NULL) return;
    nd = f->node;
    nd->text = buf_take_str(&f->text);
    nd->raw_text = buf_take_str(&f->raw);
    if (f->kids.len > 0) {
        nd->n_kids = f->kids.len;
        nd->kids = (lt_xml_node **)lt_malloc(f->kids.len * sizeof(lt_xml_node *));
        memcpy(nd->kids, f->kids.data, f->kids.len * sizeof(lt_xml_node *));
    }
    lt_vec_free(&f->kids);
    p->frames.len--;
}

static void add_text(parser *p, const char *s, size_t n, int decode, int cdata)
{
    frame *f = xml_cur_frame(p);
    if (f == NULL || n == 0) return;
    lt_buf_append(&f->raw, s, n);
    if (decode && !cdata) decode_into(s, n, &f->text, 0);
    else lt_buf_append(&f->text, s, n);
}

/* skip <!DOCTYPE ...> or other <!...> declaration: balanced [] and quotes */
static size_t skip_decl(const parser *p, size_t i)
{
    int depth = 0;
    char q = 0;
    while (i < p->n) {
        char c = p->s[i];
        if (q) {
            if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (depth > 0) depth--;
        } else if (c == '>' && depth == 0) {
            return i + 1;
        }
        i++;
    }
    return p->n + 1;    /* unterminated */
}

static void free_attrs(lt_vec *a)
{
    size_t k;
    for (k = 0; k < a->len; k++) {
        lt_xml_attr *at = (lt_xml_attr *)lt_vec_at(a, k);
        lt_free(at->name);
        lt_free(at->value);
    }
    lt_vec_free(a);
}

/* Parse a start tag at p->i ('<' + name start). Pushes a frame unless the tag
 * is self-closing (then the node is attached and finalized at once). */
static void parse_start_tag(parser *p)
{
    const char *s = p->s;
    size_t n = p->n, i = p->i + 1, ns;
    char *name;
    lt_vec attrs;
    int selfclose = 0, complete = 0;
    lt_xml_node *nd;
    frame *par;

    ns = i;
    while (i < n && !is_name_stop((unsigned char)s[i])) i++;
    name = xml_drop_prefix(s + ns, i - ns, p->flags);
    lt_vec_init(&attrs, sizeof(lt_xml_attr));

    for (;;) {
        size_t as, ae;
        while (i < n && is_ws((unsigned char)s[i])) i++;
        if (i >= n) { p_fail(p, p->i, "unterminated start tag"); break; }
        if (s[i] == '>') { i++; complete = 1; break; }
        if (s[i] == '/') {
            if (i + 1 < n && s[i + 1] == '>') { i += 2; selfclose = 1; complete = 1; break; }
            i++;
            continue;
        }
        if (s[i] == '<') {
            p_fail(p, i, "'<' inside start tag");
            complete = 1;   /* lenient: tag ends here, '<' starts the next markup */
            break;
        }
        as = i;
        while (i < n && !is_name_stop((unsigned char)s[i])) i++;
        ae = i;
        if (ae == as) {     /* stray quote or '=' */
            i++;
            p_fail(p, i, "malformed attribute");
            if (!(p->flags & LT_XML_LENIENT)) break;
            p->failed = 0;
            continue;
        }
        {
            lt_xml_attr *at;
            lt_buf v;
            char *an = xml_drop_prefix(s + as, ae - as, p->flags);
            int drop = 0;
            lt_buf_init(&v);
            while (i < n && is_ws((unsigned char)s[i])) i++;
            if (i < n && s[i] == '=') {
                i++;
                while (i < n && is_ws((unsigned char)s[i])) i++;
                if (i < n && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i];
                    size_t vs = i + 1, ve = vs;
                    while (ve < n && s[ve] != q) ve++;
                    if (ve >= n) {
                        p_fail(p, i, "unterminated attribute value");
                        lt_free(an);
                        lt_buf_free(&v);
                        i = n;
                        break;
                    }
                    decode_into(s + vs, ve - vs, &v, (p->flags & LT_XML_NORMALIZE) != 0);
                    i = ve + 1;
                } else {
                    size_t vs = i;
                    while (i < n && !is_ws((unsigned char)s[i]) && s[i] != '>' && s[i] != '<') i++;
                    if (i < n && s[i] == '>' && i > vs && s[i - 1] == '/') i--;
                    decode_into(s + vs, i - vs, &v, (p->flags & LT_XML_NORMALIZE) != 0);
                }
            }
            if ((p->flags & LT_XML_STRIP_NS) &&
                (strcmp(an, "xmlns") == 0 ||
                 (ae - as >= 6 && memcmp(s + as, "xmlns:", 6) == 0))) drop = 1;
            if (drop) {
                lt_free(an);
                lt_buf_free(&v);
            } else {
                at = (lt_xml_attr *)lt_vec_push(&attrs);
                at->name = an;
                at->value = buf_take_str(&v);
            }
        }
    }

    if (p->failed || !complete) {
        if (!(p->flags & LT_XML_LENIENT) || i >= n) {
            /* strict error, or EOF inside the tag: drop the element */
            if (!p->failed) p_fail(p, p->i, "unterminated start tag");
            if (p->flags & LT_XML_LENIENT) p->failed = 0;
            free_attrs(&attrs);
            lt_free(name);
            p->i = n;
            return;
        }
    }
    if (p->failed) {
        /* lenient recovery from a recoverable problem: keep going */
        p->failed = 0;
    }

    if (name[0] == '\0' || !is_name_start((unsigned char)name[0])) {
        p_fail(p, p->i, "invalid element name");
        free_attrs(&attrs);
        lt_free(name);
        if (p->flags & LT_XML_LENIENT) { p->failed = 0; p->i = i; }
        else p->i = n;
        return;
    }
    if (p->frames.len >= XML_MAX_DEPTH && !selfclose) {
        p_fail(p, p->i, "nesting too deep");
        free_attrs(&attrs);
        lt_free(name);
        p->i = n;
        return;
    }

    nd = (lt_xml_node *)lt_calloc(1, sizeof *nd);
    nd->name = name;
    nd->self_closing = selfclose ? LT_TRUE : LT_FALSE;
    if (attrs.len > 0) {
        nd->n_attrs = attrs.len;
        nd->attrs = (lt_xml_attr *)lt_malloc(attrs.len * sizeof(lt_xml_attr));
        memcpy(nd->attrs, attrs.data, attrs.len * sizeof(lt_xml_attr));
    }
    lt_vec_free(&attrs);

    par = xml_cur_frame(p);
    if (par) {
        lt_xml_node **slot = (lt_xml_node **)lt_vec_push(&par->kids);
        *slot = nd;
        nd->parent = par->node;
    } else {
        p->root = nd;
    }
    {
        frame *f = (frame *)lt_vec_push(&p->frames);
        f->node = nd;
        lt_buf_init(&f->text);
        lt_buf_init(&f->raw);
        lt_vec_init(&f->kids, sizeof(lt_xml_node *));
    }
    if (selfclose) pop_frame(p);
    p->i = i;
}

static void parse_end_tag(parser *p)
{
    const char *s = p->s;
    size_t n = p->n, i = p->i + 2, ns = i, k;
    char *name;
    size_t gt;
    while (i < n && !is_name_stop((unsigned char)s[i])) i++;
    name = xml_drop_prefix(s + ns, i - ns, p->flags);
    gt = i;
    while (gt < n && s[gt] != '>') gt++;
    if (gt >= n) {
        lt_free(name);
        if (!(p->flags & LT_XML_LENIENT)) p_fail(p, p->i, "unterminated end tag");
        p->i = n;
        return;
    }
    p->i = gt + 1;
    {
        frame *f = xml_cur_frame(p);
        if (f && strcmp(f->node->name, name) == 0) {
            pop_frame(p);
        } else if (p->flags & LT_XML_LENIENT) {
            /* close up to a matching ancestor, else ignore the stray end tag */
            for (k = p->frames.len; k > 0; k--) {
                frame *g = (frame *)lt_vec_at(&p->frames, k - 1);
                if (strcmp(g->node->name, name) == 0) {
                    while (p->frames.len >= k) pop_frame(p);
                    break;
                }
            }
        } else {
            char m[120];
            lt_snprintf(m, sizeof m, "mismatched end tag </%.40s>", name);
            p_fail(p, p->i, m);
        }
    }
    lt_free(name);
}

static void parse_all(parser *p)
{
    const char *s = p->s;
    size_t n = p->n;
    int lenient = (p->flags & LT_XML_LENIENT) != 0;

    while (p->i < n && !p->failed) {
        size_t i = p->i;
        if (s[i] != '<') {
            const char *lt = (const char *)memchr(s + i, '<', n - i);
            size_t e = lt ? (size_t)(lt - s) : n;
            add_text(p, s + i, e - i, 1, 0);
            p->i = e;
            continue;
        }
        if (i + 4 <= n && memcmp(s + i, "<!--", 4) == 0) {
            size_t e = find_sub(s, n, i + 4, "-->", 3);
            if (e >= n) {
                if (!lenient) p_fail(p, i, "unterminated comment");
                p->i = n;
            } else {
                p->i = e + 3;
            }
        } else if (i + 9 <= n && memcmp(s + i, "<![CDATA[", 9) == 0) {
            size_t e = find_sub(s, n, i + 9, "]]>", 3);
            if (e >= n) {
                if (!lenient) p_fail(p, i, "unterminated CDATA");
                add_text(p, s + i + 9, n - (i + 9), 0, 1);
                p->i = n;
            } else {
                add_text(p, s + i + 9, e - (i + 9), 0, 1);
                p->i = e + 3;
            }
        } else if (i + 1 < n && s[i + 1] == '?') {
            size_t e = find_sub(s, n, i + 2, "?>", 2);
            if (e >= n) {
                if (!lenient) p_fail(p, i, "unterminated processing instruction");
                p->i = n;
            } else {
                p->i = e + 2;
            }
        } else if (i + 1 < n && s[i + 1] == '!') {
            size_t e = skip_decl(p, i + 2);
            if (e > n) {
                if (!lenient) p_fail(p, i, "unterminated declaration");
                p->i = n;
            } else {
                p->i = e;
            }
        } else if (i + 1 < n && s[i + 1] == '/') {
            if (p->frames.len == 0) {
                if (p->root != NULL) break;     /* trailing junk after root */
                if (!lenient) p_fail(p, i, "end tag outside root");
                {
                    size_t e = i;
                    while (e < n && s[e] != '>') e++;
                    p->i = e < n ? e + 1 : n;
                }
            } else {
                parse_end_tag(p);
            }
        } else if (i + 1 < n && is_name_start((unsigned char)s[i + 1])) {
            if (p->frames.len == 0 && p->root != NULL) break;   /* second root: ignore */
            parse_start_tag(p);
        } else {
            /* stray '<' */
            if (!lenient) {
                p_fail(p, i, "unexpected '<'");
            } else {
                add_text(p, "<", 1, 0, 0);
                p->i = i + 1;
            }
        }
        if (p->frames.len == 0 && p->root != NULL) break;   /* root closed: done */
    }
    if (!p->failed && p->frames.len > 0 && !lenient) {
        p_fail(p, n, "unexpected end of input inside element");
    }
}

lt_xml_node *lt_xml_parse(const char *buf, size_t len, unsigned flags, char *err, size_t errsz)
{
    parser p;
    lt_buf work;
    lt_xml_node *root = NULL;
    const unsigned char *u = (const unsigned char *)buf;
    size_t k, start = 0;

    if (err && errsz) err[0] = '\0';
    if (buf == NULL) len = 0;
    memset(&p, 0, sizeof p);

    /* working copy: UTF-8 (BOM stripped, Latin-1 transcoded if invalid UTF-8) */
    lt_buf_init(&work);
    if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) start = 3;
    if (utf8_valid(u + start, len - start)) {
        lt_buf_append(&work, buf + start, len - start);
    } else {
        for (k = start; k < len; k++) {
            if (u[k] < 0x80) lt_buf_putc(&work, (int)u[k]);
            else put_utf8(&work, (unsigned long)u[k]);
        }
    }
    for (k = 0; k < work.len; k++) if (work.data[k] == '\0') work.data[k] = ' ';
    if (work.data == NULL) {
        lt_buf_putc(&work, ' ');
        work.len = 0;
    }

    p.s = work.data;
    p.n = work.len;
    p.flags = flags;
    lt_vec_init(&p.frames, sizeof(frame));

    parse_all(&p);

    /* finalize whatever is still open (lenient EOF, or cleanup after failure) */
    while (p.frames.len > 0) pop_frame(&p);
    lt_vec_free(&p.frames);

    if (!p.failed && p.root == NULL) {
        p_fail(&p, p.n, "no root element");
    }
    if (p.failed) {
        if (p.root) lt_xml_free(p.root);
        if (err && errsz) lt_strlcpy(err, p.msg, errsz);
        lt_err_set("%s", p.msg);
        root = NULL;
    } else {
        root = p.root;
    }
    lt_buf_free(&work);
    return root;
}

void lt_xml_free(lt_xml_node *n)
{
    size_t k;
    if (n == NULL) return;
    for (k = 0; k < n->n_kids; k++) lt_xml_free(n->kids[k]);
    for (k = 0; k < n->n_attrs; k++) {
        lt_free(n->attrs[k].name);
        lt_free(n->attrs[k].value);
    }
    lt_free(n->kids);
    lt_free(n->attrs);
    lt_free(n->name);
    lt_free(n->text);
    lt_free(n->raw_text);
    lt_free(n);
}

/* ------------------------------------------------------------------ */
/* read helpers                                                       */
/* ------------------------------------------------------------------ */

const char *lt_xml_attr_get(const lt_xml_node *n, const char *name)
{
    size_t k;
    if (n == NULL || name == NULL) return NULL;
    for (k = 0; k < n->n_attrs; k++) {
        if (strcmp(n->attrs[k].name, name) == 0) return n->attrs[k].value;
    }
    return NULL;
}

const lt_xml_node *lt_xml_child(const lt_xml_node *n, const char *name)
{
    size_t k;
    if (n == NULL || name == NULL) return NULL;
    for (k = 0; k < n->n_kids; k++) {
        if (strcmp(n->kids[k]->name, name) == 0) return n->kids[k];
    }
    return NULL;
}

void lt_xml_find_all(const lt_xml_node *n, const char *name, lt_vec *out)
{
    size_t k;
    if (n == NULL || name == NULL) return;
    for (k = 0; k < n->n_kids; k++) {
        const lt_xml_node *c = n->kids[k];
        if (strcmp(c->name, name) == 0) {
            const lt_xml_node **slot = (const lt_xml_node **)lt_vec_push(out);
            *slot = c;
        }
        lt_xml_find_all(c, name, out);
    }
}

const lt_xml_node *lt_xml_find(const lt_xml_node *n, const char *name)
{
    size_t k;
    if (n == NULL || name == NULL) return NULL;
    for (k = 0; k < n->n_kids; k++) {
        const lt_xml_node *c = n->kids[k], *r;
        if (strcmp(c->name, name) == 0) return c;
        r = lt_xml_find(c, name);
        if (r) return r;
    }
    return NULL;
}
