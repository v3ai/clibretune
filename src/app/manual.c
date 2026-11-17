/* manual.c - embedded manual access, Markdown parsing, link resolution, search (see manual.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "manual.h"

/* ------------------------------------------------------------------ */
/* pages                                                               */
int lt_man_find(const char *id)
{
    int i;
    if (id == NULL) { return -1; }
    for (i = 0; i < lt_man_count; i++) {
        if (strcmp(lt_man_pages[i].id, id) == 0) { return i; }
    }
    return -1;
}

int lt_man_line_count(int page)
{
    int i, n = 0;
    if (page < 0 || page >= lt_man_count) { return 0; }
    for (i = 0; i < lt_man_pages[page].n_parts; i++) { n += lt_man_pages[page].parts[i].n; }
    return n;
}

const char *lt_man_line(int page, int i)
{
    int k;
    if (page < 0 || page >= lt_man_count || i < 0) { return ""; }
    for (k = 0; k < lt_man_pages[page].n_parts; k++) {
        const lt_man_part *p = &lt_man_pages[page].parts[k];
        if (i < p->n) { return p->lines[i]; }
        i -= p->n;
    }
    return "";
}

const char *lt_man_context(const char *kind, const char *target)
{
    if (kind == NULL) { return "introduction"; }
    if (strcmp(kind, "table") == 0) { return "features/table-editing"; }
    if (strcmp(kind, "curve") == 0) { return "features/table-editing/2d-tables"; }
    if (strcmp(kind, "dashboard") == 0) { return "features/dashboards"; }
    if (strcmp(kind, "datalog") == 0 || strcmp(kind, "datalog-viewer") == 0) { return "features/datalog"; }
    if (strcmp(kind, "autotune") == 0) { return "features/autotune"; }
    if (strcmp(kind, "toothlog") == 0 || strcmp(kind, "complog") == 0) { return "features/diagnostic-loggers"; }
    if (strcmp(kind, "console") == 0) { return "features/ecu-console"; }
    if (strcmp(kind, "special") == 0 && target != NULL && strcmp(target, "och-status") == 0) {
        return "features/tools";
    }
    return "introduction";
}

/* ------------------------------------------------------------------ */
/* link resolution                                                     */
int lt_man_resolve(int from, const char *target)
{
    char path[256], seg[256], *hash;
    const char *id;
    size_t n, k = 0, w = 0;
    char out[256];
    if (target == NULL || target[0] == '\0' || from < 0 || from >= lt_man_count) { return -1; }
    if (strstr(target, "://") != NULL || strncmp(target, "mailto:", 7) == 0) { return -1; }
    lt_strlcpy(seg, target, sizeof(seg));
    hash = strchr(seg, '#');
    if (hash != NULL) { *hash = '\0'; }
    if (seg[0] == '\0') { return -1; }
    path[0] = '\0';
    if (seg[0] != '/') {
        const char *slash = strrchr(lt_man_pages[from].id, '/');
        if (slash != NULL) {
            size_t dn = (size_t)(slash - lt_man_pages[from].id);
            if (dn >= sizeof(path) - 2) { return -1; }
            memcpy(path, lt_man_pages[from].id, dn);
            path[dn] = '\0';
            lt_strlcat(path, "/", sizeof(path));
        }
    }
    lt_strlcat(path, seg[0] == '/' ? seg + 1 : seg, sizeof(path));
    /* normalise . and .. segments */
    n = strlen(path);
    out[0] = '\0';
    while (k <= n) {
        size_t e = k;
        char part[128];
        while (e < n && path[e] != '/') { e++; }
        if (e - k >= sizeof(part)) { return -1; }
        memcpy(part, path + k, e - k);
        part[e - k] = '\0';
        if (strcmp(part, "..") == 0) {
            while (w > 0 && out[w - 1] != '/') { w--; }
            if (w > 0) { w--; }
            out[w] = '\0';
        } else if (part[0] != '\0' && strcmp(part, ".") != 0) {
            if (w > 0) { out[w++] = '/'; }
            if (w + strlen(part) + 1 >= sizeof(out)) { return -1; }
            memcpy(out + w, part, strlen(part) + 1);
            w += strlen(part);
        }
        k = e + 1;
    }
    n = strlen(out);
    if (n > 3 && strcmp(out + n - 3, ".md") == 0) { out[n - 3] = '\0'; }
    id = out;
    return lt_man_find(id);
}

/* ------------------------------------------------------------------ */
/* Markdown                                                            */
static void *grow(void **arr, int *n, int *cap, size_t esz)
{
    if (*n == *cap) {
        size_t bytes = 0;
        *cap = *cap ? *cap * 2 : 64;
        if (!lt_mul_size(esz, (size_t)*cap, &bytes)) { bytes = (size_t)-1; }
        *arr = lt_realloc(*arr, bytes);
    }
    (*n)++;
    return (char *)*arr + esz * (size_t)(*n - 1);
}

void lt_md_free(lt_md_doc *doc)
{
    int i;
    for (i = 0; i < doc->n_blocks; i++) { lt_free(doc->blocks[i].text); }
    lt_free(doc->blocks);
    lt_free(doc->links);
    memset(doc, 0, sizeof(*doc));
}

static const char *bstr(const lt_buf *b) { return b->data != NULL ? b->data : ""; }

static int is_alnum_c(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static void add_link(lt_md_doc *doc, const char *label, size_t ll, const char *target, size_t tl)
{
    lt_md_link *l = (lt_md_link *)grow((void **)&doc->links, &doc->n_links, &doc->cap_links,
                                       sizeof(lt_md_link));
    if (ll >= sizeof(l->label)) { ll = sizeof(l->label) - 1; }
    if (tl >= sizeof(l->target)) { tl = sizeof(l->target) - 1; }
    memcpy(l->label, label, ll); l->label[ll] = '\0';
    memcpy(l->target, target, tl); l->target[tl] = '\0';
}

static void put(char *out, size_t cap, size_t *w, char c)
{
    if (*w + 1 < cap) { out[(*w)++] = c; }
}

static void put_s(char *out, size_t cap, size_t *w, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { put(out, cap, w, s[i]); }
}

void lt_md_inline(lt_md_doc *doc, const char *src, char *out, size_t cap, int *l0, int *ln)
{
    size_t w = 0;
    const char *p = src;
    int first = doc->n_links;
    if (cap == 0) { return; }
    while (*p != '\0') {
        char c = *p;
        if (c == '`') {                         /* code span: contents verbatim */
            const char *e = strchr(p + 1, '`');
            if (e != NULL) { put_s(out, cap, &w, p + 1, (size_t)(e - p - 1)); p = e + 1; continue; }
        } else if (c == '!' && p[1] == '[') {   /* image: show the alt text */
            const char *e = strchr(p + 2, ']');
            if (e != NULL && e[1] == '(') {
                const char *u = strchr(e + 2, ')');
                if (u != NULL) {
                    put(out, cap, &w, '[');
                    put_s(out, cap, &w, p + 2, (size_t)(e - p - 2));
                    put(out, cap, &w, ']');
                    p = u + 1;
                    continue;
                }
            }
        } else if (c == '[') {
            const char *e = strchr(p + 1, ']');
            if (e != NULL && e[1] == '(') {
                const char *u = strchr(e + 2, ')');
                if (u != NULL && u > e + 2) {
                    size_t ll = (size_t)(e - p - 1), tl = (size_t)(u - e - 2);
                    char lab[96];
                    size_t k, lw = 0;
                    int ext;
                    char tgt[200];
                    /* the label may carry marks of its own */
                    for (k = 0; k < ll && lw + 1 < sizeof(lab); k++) {
                        if (p[1 + k] != '`' && p[1 + k] != '*') { lab[lw++] = p[1 + k]; }
                    }
                    lab[lw] = '\0';
                    if (tl >= sizeof(tgt)) { tl = sizeof(tgt) - 1; }
                    memcpy(tgt, e + 2, tl); tgt[tl] = '\0';
                    ext = strstr(tgt, "://") != NULL || strncmp(tgt, "mailto:", 7) == 0;
                    if (ext) {
                        put_s(out, cap, &w, lab, lw);
                        put_s(out, cap, &w, " (", 2);
                        put_s(out, cap, &w, tgt, tl);
                        put(out, cap, &w, ')');
                    } else {
                        put_s(out, cap, &w, lab, lw);
                        add_link(doc, lab, lw, tgt, tl);
                    }
                    p = u + 1;
                    continue;
                }
            }
        } else if (c == '*' || c == '_') {
            if (p[1] == c) { p += 2; continue; }                  /* ** and __ */
            if (c == '*') {
                char prev = (p == src) ? ' ' : p[-1];
                char next = p[1];
                if (((prev == ' ' || prev == '(') && is_alnum_c(next)) ||
                    (is_alnum_c(prev) && (next == ' ' || next == '\0' || next == '.' || next == ',' ||
                                          next == ')' || next == ':' || next == ';'))) {
                    p++;
                    continue;
                }
            }
        } else if (c == '<' && (strncmp(p, "<br>", 4) == 0 || strncmp(p, "<br/>", 5) == 0)) {
            put(out, cap, &w, ' ');
            p += (p[3] == '>') ? 4 : 5;
            continue;
        } else if (c == '&') {
            if (strncmp(p, "&amp;", 5) == 0) { put(out, cap, &w, '&'); p += 5; continue; }
            if (strncmp(p, "&lt;", 4) == 0) { put(out, cap, &w, '<'); p += 4; continue; }
            if (strncmp(p, "&gt;", 4) == 0) { put(out, cap, &w, '>'); p += 4; continue; }
            if (strncmp(p, "&nbsp;", 6) == 0) { put(out, cap, &w, ' '); p += 6; continue; }
        }
        put(out, cap, &w, c);
        p++;
    }
    out[w] = '\0';
    if (l0 != NULL) { *l0 = first; }
    if (ln != NULL) { *ln = doc->n_links - first; }
}

static lt_md_block *add_block(lt_md_doc *doc, int kind, int level, const char *raw)
{
    lt_md_block *b;
    char *tmp;
    size_t cap = strlen(raw) + 8;
    int l0 = 0, ln = 0;
    tmp = (char *)lt_malloc(cap);
    lt_md_inline(doc, raw, tmp, cap, &l0, &ln);
    b = (lt_md_block *)grow((void **)&doc->blocks, &doc->n_blocks, &doc->cap_blocks,
                            sizeof(lt_md_block));
    b->kind = kind; b->level = level; b->text = tmp; b->link0 = l0; b->nlinks = ln;
    return b;
}

static int all_in(const char *s, const char *set)
{
    for (; *s != '\0'; s++) { if (strchr(set, *s) == NULL) { return 0; } }
    return 1;
}

typedef struct md_acc { int kind, level; lt_buf buf; int open; } md_acc;

static void flush(lt_md_doc *doc, md_acc *a)
{
    if (a->open && a->buf.len > 0) {
        (void)add_block(doc, a->kind, a->level, bstr(&a->buf));
    }
    lt_buf_clear(&a->buf);
    a->open = 0;
}

static void acc_add(md_acc *a, const char *text)
{
    if (a->buf.len > 0) { lt_buf_puts(&a->buf, " "); }
    lt_buf_puts(&a->buf, text);
}

static void table_row(lt_md_doc *doc, char *line)
{
    char *p = line;
    lt_buf row;
    int cells = 0, l0 = -1, ln = 0, sep = 1;
    char *c;
    lt_md_block *b;
    char tmp[1200];
    /* separator row |---|:--:| */
    {
        char *q = line;
        while (*q == '|' || *q == ' ' || *q == '-' || *q == ':' || *q == '\t') { q++; }
        if (*q == '\0') {
            if (doc->n_blocks > 0 && doc->blocks[doc->n_blocks - 1].kind == LT_MD_TABLE) {
                doc->blocks[doc->n_blocks - 1].level = 1;
            }
            return;
        }
    }
    (void)sep;
    lt_buf_init(&row);
    if (*p == '|') { p++; }
    while (*p != '\0') {
        char *e = p;
        int i0 = 0, in = 0;
        char cell[600];
        while (*e != '\0' && *e != '|') { e++; }
        {
            size_t n = (size_t)(e - p);
            if (n >= sizeof(cell)) { n = sizeof(cell) - 1; }
            memcpy(cell, p, n);
            cell[n] = '\0';
        }
        c = lt_str_trim(cell);
        if (*e == '\0' && c[0] == '\0') { break; }
        lt_md_inline(doc, c, tmp, sizeof(tmp), &i0, &in);
        if (l0 < 0) { l0 = i0; }
        ln += in;
        if (cells > 0) { lt_buf_puts(&row, "  |  "); }
        lt_buf_puts(&row, tmp);
        cells++;
        p = (*e == '|') ? e + 1 : e;
    }
    b = (lt_md_block *)grow((void **)&doc->blocks, &doc->n_blocks, &doc->cap_blocks,
                            sizeof(lt_md_block));
    b->kind = LT_MD_TABLE; b->level = 0;
    b->text = lt_strdup(bstr(&row));
    b->link0 = l0 < 0 ? doc->n_links : l0; b->nlinks = ln;
    lt_buf_free(&row);
}

void lt_md_parse(lt_md_doc *doc, const char *const *lines, int n)
{
    md_acc acc;
    int i, in_code = 0;
    memset(doc, 0, sizeof(*doc));
    memset(&acc, 0, sizeof(acc));
    lt_buf_init(&acc.buf);
    for (i = 0; i < n; i++) {
        char line[640];
        char *t;
        size_t len, indent = 0;
        lt_strlcpy(line, lines[i], sizeof(line));
        len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r')) { line[--len] = '\0'; }
        t = line;
        while (*t == ' ') { t++; indent++; }
        if (strncmp(t, "```", 3) == 0 || strncmp(t, "~~~", 3) == 0) {
            flush(doc, &acc);
            in_code = !in_code;
            continue;
        }
        if (in_code) {
            lt_md_block *b = add_block(doc, LT_MD_CODE, 0, "");
            lt_free(b->text);
            b->text = lt_strdup(line);
            continue;
        }
        if (*t == '\0') { flush(doc, &acc); continue; }
        if (t[0] == '<' && t[strlen(t) - 1] == '>' && strchr(t, ' ') == NULL) { continue; }
        if (t[0] == '#') {
            int lv = 0;
            while (t[lv] == '#') { lv++; }
            if (lv <= 6 && t[lv] == ' ') {
                flush(doc, &acc);
                (void)add_block(doc, LT_MD_HEAD, lv, lt_str_trim(t + lv));
                continue;
            }
        }
        if ((t[0] == '-' || t[0] == '*' || t[0] == '_') && strlen(t) >= 3 && all_in(t, "-*_ ") &&
            t[1] == t[0] && t[2] == t[0]) {
            flush(doc, &acc);
            (void)add_block(doc, LT_MD_RULE, 0, "");
            continue;
        }
        if (t[0] == '|') {
            flush(doc, &acc);
            table_row(doc, t);
            continue;
        }
        if (t[0] == '>') {
            const char *rest = t + 1;
            if (*rest == ' ') { rest++; }
            if (!(acc.open && acc.kind == LT_MD_QUOTE)) { flush(doc, &acc); acc.open = 1; acc.kind = LT_MD_QUOTE; acc.level = 0; }
            acc_add(&acc, rest);
            continue;
        }
        {
            const char *m = t;
            int bullet = 0;
            if ((m[0] == '-' || m[0] == '*' || m[0] == '+') && m[1] == ' ') { bullet = 1; m += 2; }
            else if (m[0] >= '0' && m[0] <= '9') {
                const char *d = m;
                while (*d >= '0' && *d <= '9') { d++; }
                if (d[0] == '.' && d[1] == ' ') { bullet = 2; }
            }
            if (bullet) {
                int lv = (int)(indent / 2);
                flush(doc, &acc);
                acc.open = 1; acc.kind = LT_MD_BULLET; acc.level = lv > 4 ? 4 : lv;
                acc_add(&acc, bullet == 2 ? m : m);
                continue;
            }
        }
        if (acc.open && acc.kind == LT_MD_QUOTE) { flush(doc, &acc); }
        if (!acc.open) { acc.open = 1; acc.kind = LT_MD_PARA; acc.level = 0; }
        acc_add(&acc, t);
    }
    flush(doc, &acc);
    lt_buf_free(&acc.buf);
}

void lt_md_parse_page(lt_md_doc *doc, int page)
{
    int n = lt_man_line_count(page), i;
    const char **lines;
    memset(doc, 0, sizeof(*doc));
    if (n <= 0) { return; }
    lines = (const char **)lt_malloc(sizeof(char *) * (size_t)n);
    for (i = 0; i < n; i++) { lines[i] = lt_man_line(page, i); }
    lt_md_parse(doc, lines, n);
    lt_free((void *)lines);
}

/* ------------------------------------------------------------------ */
/* search                                                              */
static int ci_has(const char *hay, const char *needle_lc)
{
    size_t nl = strlen(needle_lc), i;
    if (nl == 0) { return 1; }
    for (i = 0; hay[i] != '\0'; i++) {
        size_t k = 0;
        while (k < nl && hay[i + k] != '\0') {
            char c = hay[i + k];
            if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
            if (c != needle_lc[k]) { break; }
            k++;
        }
        if (k == nl) { return 1; }
    }
    return 0;
}

#define MAN_WORDS 6

int lt_man_search(const char *query, lt_man_hit *hits, int cap)
{
    char words[MAN_WORDS][40];
    int nw = 0, p, n = 0, i, j;
    const char *q = query;
    while (q != NULL && *q != '\0' && nw < MAN_WORDS) {
        size_t k = 0;
        while (*q == ' ') { q++; }
        while (*q != '\0' && *q != ' ' && k + 1 < sizeof(words[0])) {
            char c = *q++;
            if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
            words[nw][k++] = c;
        }
        while (*q != '\0' && *q != ' ') { q++; }
        words[nw][k] = '\0';
        if (k > 0) { nw++; }
    }
    if (nw == 0) { return 0; }
    for (p = 0; p < lt_man_count; p++) {
        int nl = lt_man_line_count(p), found[MAN_WORDS], score = 0, best = -1, best_sc = -1, all = 1;
        for (j = 0; j < nw; j++) {
            found[j] = 0;
            if (ci_has(lt_man_pages[p].title, words[j])) { score += 50; found[j] = 1; }
        }
        for (i = 0; i < nl; i++) {
            const char *ln = lt_man_line(p, i);
            int m = 0, head = (ln[0] == '#');
            for (j = 0; j < nw; j++) {
                if (ci_has(ln, words[j])) { m++; found[j] = 1; score += head ? 10 : 1; }
            }
            if (m > 0) {
                int sc = m * 10 + (head ? 5 : 0);
                if (sc > best_sc) { best_sc = sc; best = i; }
            }
        }
        for (j = 0; j < nw; j++) { if (!found[j]) { all = 0; } }
        if (!all) { continue; }
        if (n < cap) {
            hits[n].page = p; hits[n].line = best < 0 ? 0 : best; hits[n].score = score;
            n++;
        }
    }
    /* best first (stable insertion sort; the list is short) */
    for (i = 1; i < n; i++) {
        lt_man_hit h = hits[i];
        j = i - 1;
        while (j >= 0 && hits[j].score < h.score) { hits[j + 1] = hits[j]; j--; }
        hits[j + 1] = h;
    }
    return n;
}
