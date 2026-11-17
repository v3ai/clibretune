/* xml_write.c - streaming XML writer. See xml.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "xml.h"

void lt_xmlw_init(lt_xmlw *w, lt_buf *out, int indent, unsigned flags)
{
    w->out = out;
    w->indent = indent < 0 ? 0 : indent;
    w->flags = flags;
    lt_vec_init(&w->stack, sizeof(lt_xmlw_lvl));
    w->pending = 0;
    w->misuse = 0;
}

void lt_xmlw_free(lt_xmlw *w)
{
    size_t k;
    for (k = 0; k < w->stack.len; k++) lt_free(((lt_xmlw_lvl *)w->stack.data)[k].name);
    lt_vec_free(&w->stack);
    w->pending = 0;
}

static lt_xmlw_lvl *cur_lvl(lt_xmlw *w)
{
    return w->stack.len ? (lt_xmlw_lvl *)lt_vec_at(&w->stack, w->stack.len - 1) : NULL;
}

static void end_start_tag(lt_xmlw *w)
{
    if (w->pending) {
        lt_buf_putc(w->out, '>');
        w->pending = 0;
    }
}

static void newline_indent(lt_xmlw *w, size_t depth)
{
    size_t k, n;
    if (w->indent <= 0) return;
    if (w->out->len > 0 && w->out->data[w->out->len - 1] != '\n') lt_buf_putc(w->out, '\n');
    n = depth * (size_t)w->indent;
    for (k = 0; k < n; k++) lt_buf_putc(w->out, ' ');
}

/* prepare to emit a child node (element/comment): handles pending tag + indent */
static void begin_child(lt_xmlw *w)
{
    lt_xmlw_lvl *t;
    end_start_tag(w);
    t = cur_lvl(w);
    if (t) {
        t->has_kids = 1;
        if (!t->has_text) newline_indent(w, w->stack.len);
    } else {
        newline_indent(w, 0);
    }
}

void lt_xmlw_decl(lt_xmlw *w, const char *enc, const char *standalone)
{
    lt_buf_puts(w->out, "<?xml version=\"1.0\" encoding=\"");
    lt_xml_put_escaped(w->out, enc ? enc : "UTF-8", 0);
    lt_buf_putc(w->out, '"');
    if (standalone) {
        lt_buf_puts(w->out, " standalone=\"");
        lt_xml_put_escaped(w->out, standalone, 0);
        lt_buf_putc(w->out, '"');
    }
    lt_buf_puts(w->out, "?>");
    if (w->indent > 0) lt_buf_putc(w->out, '\n');
}

void lt_xmlw_comment(lt_xmlw *w, const char *text)
{
    char *e = lt_xml_comment_escape(text ? text : "");
    begin_child(w);
    lt_buf_puts(w->out, "<!--");
    lt_buf_puts(w->out, e);
    lt_buf_puts(w->out, "-->");
    lt_free(e);
}

void lt_xmlw_open(lt_xmlw *w, const char *name)
{
    lt_xmlw_lvl *l;
    begin_child(w);
    lt_buf_putc(w->out, '<');
    lt_buf_puts(w->out, name);
    l = (lt_xmlw_lvl *)lt_vec_push(&w->stack);
    l->name = lt_strdup(name);
    l->has_kids = 0;
    l->has_text = 0;
    w->pending = 1;
}

int lt_xmlw_attr(lt_xmlw *w, const char *name, const char *value)
{
    if (!w->pending) { w->misuse = 1; return -1; }
    lt_buf_putc(w->out, ' ');
    lt_buf_puts(w->out, name);
    lt_buf_puts(w->out, "=\"");
    lt_xml_put_escaped(w->out, value ? value : "", 2);
    lt_buf_putc(w->out, '"');
    return 0;
}

int lt_xmlw_attr_int(lt_xmlw *w, const char *name, long v)
{
    char t[40];
    lt_snprintf(t, sizeof t, "%ld", v);
    return lt_xmlw_attr(w, name, t);
}

int lt_xmlw_attr_f(lt_xmlw *w, const char *name, double v, int max_decimals)
{
    char t[64];
    lt_fmt_double(t, sizeof t, v, max_decimals);
    return lt_xmlw_attr(w, name, t);
}

static int content_ok(lt_xmlw *w)
{
    lt_xmlw_lvl *t = cur_lvl(w);
    if (t == NULL) { w->misuse = 1; return 0; }
    end_start_tag(w);
    t->has_text = 1;
    return 1;
}

void lt_xmlw_text(lt_xmlw *w, const char *s)
{
    if (!content_ok(w)) return;
    lt_xml_put_escaped(w->out, s, 1);
}

void lt_xmlw_raw(lt_xmlw *w, const char *s)
{
    if (!content_ok(w)) return;
    if (s) lt_buf_puts(w->out, s);
}

void lt_xmlw_cdata(lt_xmlw *w, const char *s)
{
    const char *p;
    if (!content_ok(w)) return;
    if (s == NULL) s = "";
    lt_buf_puts(w->out, "<![CDATA[");
    for (p = s; *p; p++) {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>') {
            lt_buf_puts(w->out, "]]]]><![CDATA[>");
            p += 2;
        } else {
            lt_buf_putc(w->out, *p);
        }
    }
    lt_buf_puts(w->out, "]]>");
}

int lt_xmlw_close(lt_xmlw *w)
{
    lt_xmlw_lvl *t = cur_lvl(w);
    if (t == NULL) { w->misuse = 1; return -1; }
    if (w->pending) {
        lt_buf_puts(w->out, (w->flags & LT_XMLW_SPACE_SLASH) ? " />" : "/>");
        w->pending = 0;
    } else {
        if (t->has_kids && !t->has_text) newline_indent(w, w->stack.len - 1);
        lt_buf_puts(w->out, "</");
        lt_buf_puts(w->out, t->name);
        lt_buf_putc(w->out, '>');
    }
    lt_free(t->name);
    w->stack.len--;
    return 0;
}

void lt_xmlw_leaf(lt_xmlw *w, const char *name, const char *text)
{
    lt_xmlw_open(w, name);
    lt_xmlw_text(w, text ? text : "");
    lt_xmlw_close(w);
}

int lt_xmlw_finish(lt_xmlw *w)
{
    int bad;
    while (w->stack.len > 0) lt_xmlw_close(w);
    if (w->indent > 0 && w->out->len > 0 && w->out->data[w->out->len - 1] != '\n') {
        lt_buf_putc(w->out, '\n');
    }
    bad = w->misuse;
    w->misuse = 0;
    return bad ? -1 : 0;
}

int lt_xmlw_ok(const lt_xmlw *w)
{
    return !w->misuse;
}
