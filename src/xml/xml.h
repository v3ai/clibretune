/* xml.h - minimal tolerant XML DOM parser + escaping helpers.
 *
 * One parser for all XML dialects in the project: .msq tunes, TunerStudio
 * .table / .tuneView, .dash / .gauge dashboards (specs 04 section 0, 05 2.2).
 * Supported subset: prolog, <!DOCTYPE> (skipped, balanced [...] ok), comments,
 * processing instructions, CDATA, elements with single/double quoted
 * attributes (a '>' inside quotes does not end the tag), the five named
 * entities plus &#N; / &#xH; with the lenient rules of spec 04 section 0.
 * No DTD entities, no namespace semantics.
 *
 * MEMORY: a parse returns the document root element; free the whole tree with
 * lt_xml_free(root). All strings/children belong to their node.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_XML_H
#define LT_XML_H

#include "../lt.h"
#include "../lt_codes.h"

typedef struct lt_xml_attr { char *name; char *value; } lt_xml_attr;   /* value entity-decoded */

typedef struct lt_xml_node {
    char *name;                         /* element name (prefix stripped if LT_XML_STRIP_NS) */
    lt_xml_attr *attrs; size_t n_attrs; /* document order */
    struct lt_xml_node **kids; size_t n_kids;   /* element children, document order */
    char *text;                         /* direct character data, entity decoded, CDATA included, concatenated */
    char *raw_text;                     /* same but NOT entity decoded (CDATA verbatim) */
    lt_bool self_closing;               /* written as <x/> */
    struct lt_xml_node *parent;         /* NULL for the root */
} lt_xml_node;

/* parse flags */
#define LT_XML_STRIP_NS    1   /* drop everything up to and including the last ':' of element and attribute names */
#define LT_XML_LENIENT     2   /* on mismatched/unterminated structure keep what was parsed so far */
#define LT_XML_NORMALIZE   4   /* attribute values: \t \n \r -> space before entity decoding (tuneView) */

/* Parse a document held in memory. Returns the root element or NULL (message
 * copied to err when non-NULL, also lt_err_set). A document with no element is
 * an error even in lenient mode. */
lt_xml_node *lt_xml_parse(const char *buf, size_t len, unsigned flags, char *err, size_t errsz);
void         lt_xml_free(lt_xml_node *root);

/* ---- read helpers ---- */
const char *lt_xml_attr_get(const lt_xml_node *n, const char *name);          /* NULL if absent */
const lt_xml_node *lt_xml_child(const lt_xml_node *n, const char *name);      /* first direct child */
/* all DESCENDANTS (document order, excluding n itself) called `name` appended
 * to out (a lt_vec of `const lt_xml_node *`; caller inits/frees). */
void        lt_xml_find_all(const lt_xml_node *n, const char *name, lt_vec *out);
/* first descendant named `name` or NULL */
const lt_xml_node *lt_xml_find(const lt_xml_node *n, const char *name);

/* ---- escaping (spec 04 section 1.7) ---- */
/* & < > " '  -> entities. Attribute and quoted-value flavour. malloc'd. */
char *lt_xml_escape(const char *s);
/* Appends the escaped text to b. mode 0: attribute/quoted value (& < > " '),
 * mode 1: text node (& < > only), mode 2: like 0 plus \n \r \t as &#10; &#13; &#9;
 * (what the tuneView writer needs so that NORMALIZE parsing round-trips) */
void  lt_xml_put_escaped(lt_buf *b, const char *s, int mode);
/* Decode entities with the Rust-compatible leniency: '&' without ';' within 10
 * chars stays, unknown/invalid entity kept verbatim. malloc'd. */
char *lt_xml_unescape(const char *s);
/* "--" -> "- -", append a space if result ends with '-'. malloc'd. */
char *lt_xml_comment_escape(const char *s);

/* ---- streaming writer ----
 * Appends to a caller-owned lt_buf. indent = spaces per level (0 = no
 * whitespace at all). An element that receives text is written inline
 * (no indentation inside it); an element with neither text nor children is
 * written self-closing. Always succeeds (OOM aborts); misuse (attr outside a
 * start tag, close with nothing open) is ignored and reported by a non-zero
 * return / lt_xmlw_ok(). Example output with indent 4:
 *   <a x="1">\n    <b>text</b>\n    <c/>\n</a>\n
 */
#define LT_XMLW_SPACE_SLASH 1   /* write "<c />" instead of "<c/>" */

typedef struct lt_xmlw_lvl { char *name; int has_kids; int has_text; } lt_xmlw_lvl;
typedef struct lt_xmlw {
    lt_buf *out;
    int indent;
    unsigned flags;
    lt_vec stack;       /* of lt_xmlw_lvl */
    int pending;        /* a start tag is still open ('>' not yet written) */
    int misuse;         /* set by an ignored invalid call */
} lt_xmlw;

void lt_xmlw_init(lt_xmlw *w, lt_buf *out, int indent, unsigned flags);
void lt_xmlw_free(lt_xmlw *w);                     /* frees internal state only, not out */
/* <?xml version="1.0" encoding="enc" [standalone="no"]?> + newline. enc NULL -> "UTF-8";
 * standalone NULL -> omitted. */
void lt_xmlw_decl(lt_xmlw *w, const char *enc, const char *standalone);
void lt_xmlw_comment(lt_xmlw *w, const char *text);   /* escaped with lt_xml_comment_escape */
void lt_xmlw_open(lt_xmlw *w, const char *name);
int  lt_xmlw_attr(lt_xmlw *w, const char *name, const char *value);  /* escaped, mode 2 */
int  lt_xmlw_attr_int(lt_xmlw *w, const char *name, long v);
int  lt_xmlw_attr_f(lt_xmlw *w, const char *name, double v, int max_decimals);
void lt_xmlw_text(lt_xmlw *w, const char *s);         /* escaped (mode 1) */
void lt_xmlw_cdata(lt_xmlw *w, const char *s);        /* "]]>" inside s is split correctly */
void lt_xmlw_raw(lt_xmlw *w, const char *s);          /* verbatim, counts as text content */
int  lt_xmlw_close(lt_xmlw *w);                       /* close innermost element; -1 if none */
void lt_xmlw_leaf(lt_xmlw *w, const char *name, const char *text);  /* <name>text</name> */
/* close every open element, then a final newline when indent > 0. Returns 0 if
 * no misuse occurred. The writer may be reused afterwards. */
int  lt_xmlw_finish(lt_xmlw *w);
int  lt_xmlw_ok(const lt_xmlw *w);

#endif /* LT_XML_H */
