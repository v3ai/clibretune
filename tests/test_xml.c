/* test_xml.c - tests for src/xml (parser, escaping, writer) */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "xml/xml.h"
#include "lt_test.h"

static char err[256];

static lt_xml_node *P(const char *s, unsigned fl)
{
    return lt_xml_parse(s, strlen(s), fl, err, sizeof err);
}

LT_TEST(basic_tree)
{
    lt_xml_node *r = P("<a x=\"1\" y='two'><b>hi</b><c/><b z=\"&lt;\"> t </b></a>", 0);
    const lt_xml_node *b;
    lt_vec v;
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(r->name, "a");
    LT_CHECK_INT(r->n_attrs, 2);
    LT_CHECK_STR(lt_xml_attr_get(r, "x"), "1");
    LT_CHECK_STR(lt_xml_attr_get(r, "y"), "two");
    LT_CHECK(lt_xml_attr_get(r, "q") == NULL);
    LT_CHECK_INT(r->n_kids, 3);
    LT_CHECK(r->parent == NULL);
    b = lt_xml_child(r, "b");
    LT_CHECK(b != NULL && b->parent == r);
    LT_CHECK_STR(b->text, "hi");
    LT_CHECK(!b->self_closing);
    LT_CHECK(r->kids[1]->self_closing);
    LT_CHECK_STR(r->kids[1]->text, "");
    LT_CHECK_STR(r->kids[2]->text, " t ");
    LT_CHECK_STR(lt_xml_attr_get(r->kids[2], "z"), "<");
    lt_vec_init(&v, sizeof(const lt_xml_node *));
    lt_xml_find_all(r, "b", &v);
    LT_CHECK_INT(v.len, 2);
    lt_vec_free(&v);
    LT_CHECK(lt_xml_find(r, "c") == r->kids[1]);
    LT_CHECK(lt_xml_find(r, "zz") == NULL);
    lt_xml_free(r);
}

LT_TEST(prolog_comments_doctype_pi)
{
    lt_xml_node *r = P(
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<!-- c1 -->\n"
        "<!DOCTYPE msq PUBLIC \"-//rusEfi//DTD msq//EN\" \"msq.dtd\">\n"
        "<!DOCTYPE x [ <!ENTITY e \"a>b\"> ]>\n"
        "<?php foo ?><r><!-- inner <b/> --><?pi x?>t<!-- c -->u</r><!-- tail -->", 0);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(r->name, "r");
    LT_CHECK_INT(r->n_kids, 0);
    LT_CHECK_STR(r->text, "tu");
    lt_xml_free(r);
}

LT_TEST(cdata)
{
    lt_xml_node *r = P("<a>x &amp; <![CDATA[<b> &amp; ]]>y</a>", 0);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(r->text, "x & <b> &amp; y");
    LT_CHECK_STR(r->raw_text, "x &amp; <b> &amp; y");
    lt_xml_free(r);
}

LT_TEST(attr_quotes_gt)
{
    lt_xml_node *r = P("<a  x = \"a>b\"  y='c\"d' z=\"e'f\"\n w=\"&quot;&apos;\" />", 0);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(lt_xml_attr_get(r, "x"), "a>b");
    LT_CHECK_STR(lt_xml_attr_get(r, "y"), "c\"d");
    LT_CHECK_STR(lt_xml_attr_get(r, "z"), "e'f");
    LT_CHECK_STR(lt_xml_attr_get(r, "w"), "\"'");
    LT_CHECK(r->self_closing);
    lt_xml_free(r);
}

LT_TEST(attr_order_and_duplicates)
{
    lt_xml_node *r = P("<a k3=\"3\" k1=\"1\" k2=\"2\" k1=\"dup\"/>", 0);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_INT(r->n_attrs, 4);
    LT_CHECK_STR(r->attrs[0].name, "k3");
    LT_CHECK_STR(r->attrs[1].name, "k1");
    LT_CHECK_STR(lt_xml_attr_get(r, "k1"), "1");
    lt_xml_free(r);
}

LT_TEST(entities)
{
    char *s;
    s = lt_xml_unescape("&amp;&lt;&gt;&quot;&apos;");
    LT_CHECK_STR(s, "&<>\"'");
    lt_free(s);
    s = lt_xml_unescape("&#65;&#x42;&#X43;");
    LT_CHECK_STR(s, "ABC");
    lt_free(s);
    s = lt_xml_unescape("&#233;&#xE9;&#x20AC;&#x1F600;");
    LT_CHECK_STR(s, "\xC3\xA9\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    lt_free(s);
    /* & without ; within 10 chars stays */
    s = lt_xml_unescape("a & b");
    LT_CHECK_STR(s, "a & b");
    lt_free(s);
    s = lt_xml_unescape("a&b; c&");
    LT_CHECK_STR(s, "a&b; c&");
    lt_free(s);
    s = lt_xml_unescape("&abcdefghijklmnop;");
    LT_CHECK_STR(s, "&abcdefghijklmnop;");
    lt_free(s);
    /* unknown / invalid kept verbatim */
    s = lt_xml_unescape("&foo; &#0; &#xD800; &#x110000; &#; &#x; &#12a; &;");
    LT_CHECK_STR(s, "&foo; &#0; &#xD800; &#x110000; &#; &#x; &#12a; &;");
    lt_free(s);
    s = lt_xml_unescape("&amp;amp;");
    LT_CHECK_STR(s, "&amp;");
    lt_free(s);
    s = lt_xml_unescape("");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = lt_xml_unescape("&#10;&#13;");
    LT_CHECK_STR(s, "\n\r");
    lt_free(s);
}

LT_TEST(escape_fns)
{
    char *s = lt_xml_escape("a&b<c>d\"e'f");
    LT_CHECK_STR(s, "a&amp;b&lt;c&gt;d&quot;e&apos;f");
    lt_free(s);
    s = lt_xml_escape("");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = lt_xml_comment_escape("a--b");
    LT_CHECK_STR(s, "a- -b");
    lt_free(s);
    s = lt_xml_comment_escape("---");
    LT_CHECK_STR(s, "- -- ");
    lt_free(s);
    s = lt_xml_comment_escape("end-");
    LT_CHECK_STR(s, "end- ");
    lt_free(s);
    s = lt_xml_comment_escape("----");
    LT_CHECK_STR(s, "- -- - ");
    lt_free(s);
    {
        lt_buf b;
        lt_buf_init(&b);
        lt_xml_put_escaped(&b, "a\"'\n&<", 1);
        LT_CHECK_STR(b.data, "a\"'\n&amp;&lt;");
        lt_buf_clear(&b);
        lt_xml_put_escaped(&b, "a\"\n\r\t", 2);
        LT_CHECK_STR(b.data, "a&quot;&#10;&#13;&#9;");
        lt_buf_free(&b);
    }
}

LT_TEST(normalize)
{
    lt_xml_node *r = P("<a v=\"x\ty\nz\r\nw&#10;&#13;&#9;\"/>", LT_XML_NORMALIZE);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(lt_xml_attr_get(r, "v"), "x y z  w\n\r\t");
    lt_xml_free(r);
    r = P("<a v=\"x\ny\"/>", 0);
    LT_CHECK_STR(lt_xml_attr_get(r, "v"), "x\ny");
    lt_xml_free(r);
}

LT_TEST(strip_ns)
{
    const char *doc = "<ns:root xmlns:ns=\"u\" xmlns=\"d\" ns:a=\"1\" b=\"2\">"
                      "<x:y:z q:r=\"3\"/><ns:kid>t</ns:kid></ns:root>";
    lt_xml_node *r = P(doc, LT_XML_STRIP_NS);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(r->name, "root");
    LT_CHECK_INT(r->n_attrs, 2);
    LT_CHECK_STR(lt_xml_attr_get(r, "a"), "1");
    LT_CHECK_STR(lt_xml_attr_get(r, "b"), "2");
    LT_CHECK_STR(r->kids[0]->name, "z");
    LT_CHECK_STR(lt_xml_attr_get(r->kids[0], "r"), "3");
    LT_CHECK(lt_xml_child(r, "kid") != NULL);
    lt_xml_free(r);
    r = P(doc, 0);
    LT_CHECK_STR(r->name, "ns:root");
    LT_CHECK_STR(lt_xml_attr_get(r, "xmlns"), "d");
    LT_CHECK_STR(lt_xml_attr_get(r, "ns:a"), "1");
    LT_CHECK_STR(r->kids[0]->name, "x:y:z");
    lt_xml_free(r);
}

LT_TEST(latin1_fallback)
{
    const char doc[] = "<a v=\"caf\xE9\">na\xEFve \xB0" "C</a>";
    lt_xml_node *r = lt_xml_parse(doc, sizeof doc - 1, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(lt_xml_attr_get(r, "v"), "caf\xC3\xA9");
    LT_CHECK_STR(r->text, "na\xC3\xAFve \xC2\xB0" "C");
    lt_xml_free(r);
    /* valid UTF-8 untouched */
    r = P("<a>caf\xC3\xA9</a>", 0);
    LT_CHECK_STR(r->text, "caf\xC3\xA9");
    lt_xml_free(r);
    /* overlong / surrogate encodings are not valid UTF-8 -> Latin-1 */
    r = P("<a>\xC0\xAF</a>", 0);
    LT_CHECK(r != NULL);
    LT_CHECK_STR(r->text, "\xC3\x80\xC2\xAF");
    lt_xml_free(r);
}

LT_TEST(embedded_nul)
{
    const char doc[] = "<a>x\0y</a>";
    lt_xml_node *r = lt_xml_parse(doc, sizeof doc - 1, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_STR(r->text, "x y");
    lt_xml_free(r);
}

LT_TEST(errors_strict)
{
    LT_CHECK(P("<a><b></a>", 0) == NULL);
    LT_CHECK(strstr(err, "mismatched") != NULL);
    LT_CHECK(strstr(err, "line 1") != NULL);
    LT_CHECK(P("<a><b>", 0) == NULL);
    LT_CHECK(P("", 0) == NULL);
    LT_CHECK(P("   \n", 0) == NULL);
    LT_CHECK(P("just text", 0) == NULL);
    LT_CHECK(P("<!-- only -->", 0) == NULL);
    LT_CHECK(P("<a", 0) == NULL);
    LT_CHECK(P("<a x=\"1", 0) == NULL);
    LT_CHECK(P("<a><!-- x", 0) == NULL);
    LT_CHECK(P("<a><![CDATA[ x", 0) == NULL);
    LT_CHECK(P("<a>1 < 2</a>", 0) == NULL);
    LT_CHECK(P("</a>", 0) == NULL);
    LT_CHECK(P("<a>\n\n<b></c></a>", 0) == NULL);
    LT_CHECK(strstr(err, "line 3") != NULL);
    LT_CHECK(lt_xml_parse(NULL, 0, 0, NULL, 0) == NULL);
    LT_CHECK(strlen(lt_err_get()) > 0);
}

LT_TEST(lenient)
{
    lt_xml_node *r = P("<a><b>x<c>y</a>", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) {
        const lt_xml_node *b = lt_xml_child(r, "b");
        LT_CHECK(b != NULL);
        LT_CHECK(b && lt_xml_child(b, "c") != NULL);
        LT_CHECK(b && lt_xml_child(b, "c") && strcmp(lt_xml_child(b, "c")->text, "y") == 0);
        LT_CHECK_STR(b->text, "x");
        lt_xml_free(r);
    }
    /* unterminated at EOF keeps what was parsed */
    r = P("<a><b>1</b><c>2", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_INT(r->n_kids, 2);
        LT_CHECK_STR(r->kids[1]->text, "2");
        lt_xml_free(r);
    }
    /* stray end tag ignored */
    r = P("<a><b/></zzz><c/></a>", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) { LT_CHECK_INT(r->n_kids, 2); lt_xml_free(r); }
    /* stray '<' becomes text */
    r = P("<a>1 < 2</a>", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) { LT_CHECK_STR(r->text, "1 < 2"); lt_xml_free(r); }
    /* truncated start tag dropped */
    r = P("<a><b/><c x=\"1", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) { LT_CHECK_INT(r->n_kids, 1); lt_xml_free(r); }
    /* unquoted attribute and valueless attribute */
    r = P("<a x=1 y z='3'/>", LT_XML_LENIENT);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_STR(lt_xml_attr_get(r, "x"), "1");
        LT_CHECK_STR(lt_xml_attr_get(r, "y"), "");
        LT_CHECK_STR(lt_xml_attr_get(r, "z"), "3");
        lt_xml_free(r);
    }
    /* still an error with no element */
    LT_CHECK(P("hello", LT_XML_LENIENT) == NULL);
    LT_CHECK(P("", LT_XML_LENIENT) == NULL);
    LT_CHECK(P("<!-- x", LT_XML_LENIENT) == NULL);
}

LT_TEST(trailing_and_multiple_roots)
{
    lt_xml_node *r = P("<a/><b/>junk<<<", 0);
    LT_CHECK(r != NULL);
    if (r) { LT_CHECK_STR(r->name, "a"); lt_xml_free(r); }
}

LT_TEST(deep_nesting)
{
    lt_buf b;
    lt_xml_node *r;
    int i;
    lt_buf_init(&b);
    for (i = 0; i < 100; i++) lt_buf_puts(&b, "<a>");
    for (i = 0; i < 100; i++) lt_buf_puts(&b, "</a>");
    r = lt_xml_parse(b.data, b.len, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    lt_xml_free(r);
    lt_buf_clear(&b);
    for (i = 0; i < 100000; i++) lt_buf_puts(&b, "<a>");
    r = lt_xml_parse(b.data, b.len, LT_XML_LENIENT, err, sizeof err);
    LT_CHECK(r == NULL);
    LT_CHECK(strstr(err, "too deep") != NULL);
    r = lt_xml_parse(b.data, b.len, 0, err, sizeof err);
    LT_CHECK(r == NULL);
    lt_buf_free(&b);
}

LT_TEST(many_siblings)
{
    lt_buf b;
    lt_xml_node *r;
    int i;
    lt_buf_init(&b);
    lt_buf_puts(&b, "<r>");
    for (i = 0; i < 20000; i++) lt_buf_puts(&b, "<i n=\"1\"/>");
    lt_buf_puts(&b, "</r>");
    r = lt_xml_parse(b.data, b.len, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_INT(r->n_kids, 20000);
    lt_xml_free(r);
    lt_buf_free(&b);
}

LT_TEST(sample_msq_fixture)
{
    size_t n = 0;
    char *d = lt_file_read("tests/fixtures/xml/sample.msq", &n);
    lt_xml_node *r;
    const lt_xml_node *c;
    lt_vec v;
    LT_CHECK(d != NULL);
    if (!d) return;
    r = lt_xml_parse(d, n, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_STR(r->name, "msq");
        LT_CHECK_STR(lt_xml_attr_get(r, "xmlns"), "http://www.msefi.com/:msq");
        LT_CHECK_STR(lt_xml_attr_get(lt_xml_child(r, "bibliography"), "author"), "A & B");
        LT_CHECK_STR(lt_xml_attr_get(lt_xml_child(r, "versionInfo"), "signature"),
                     "speeduino 202501");
        lt_vec_init(&v, sizeof(const lt_xml_node *));
        lt_xml_find_all(r, "constant", &v);
        LT_CHECK_INT(v.len, 3);
        c = LT_VEC_AT(&v, const lt_xml_node *, 0);
        LT_CHECK_STR(c->text, "6.9");
        c = LT_VEC_AT(&v, const lt_xml_node *, 1);
        LT_CHECK_STR(lt_xml_attr_get(c, "name"), "veTable");
        LT_CHECK(strstr(c->text, "3 4") != NULL);
        c = LT_VEC_AT(&v, const lt_xml_node *, 2);
        LT_CHECK_STR(c->text, "\"a < b \xE2\x98\xBA A\"");
        LT_CHECK(strstr(lt_xml_child(r, "page")->text, " raw <data> & stuff ") != NULL);
        lt_vec_free(&v);
        lt_xml_free(r);
    }
    lt_free(d);
}

LT_TEST(basic_dash_fixture)
{
    size_t n = 0;
    char *d = lt_file_read("tests/fixtures/xml/basic.dash", &n);
    lt_xml_node *r;
    lt_vec v;
    LT_CHECK(d != NULL);
    if (!d) return;
    r = lt_xml_parse(d, n, 0, err, sizeof err);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_STR(r->name, "dashboard");
        LT_CHECK_STR(lt_xml_child(r, "title")->text, "Basic Dashboard");
        LT_CHECK_STR(lt_xml_child(r, "backgroundColor")->text, "-16777216");
        lt_vec_init(&v, sizeof(const lt_xml_node *));
        lt_xml_find_all(r, "dashComp", &v);
        LT_CHECK(v.len >= 6);
        LT_CHECK_STR(lt_xml_attr_get(LT_VEC_AT(&v, const lt_xml_node *, 0), "type"), "Gauge");
        lt_vec_free(&v);
        lt_xml_free(r);
    }
    lt_free(d);
}

LT_TEST(tableview_style)
{
    /* spec 04: tuneView writer escapes newlines in attrs; normalized read restores them */
    lt_xml_node *r = P("<x:view xmlns:x=\"u\"><x:c x:t=\"a&#10;b&#13;c\td\"/></x:view>",
                       LT_XML_STRIP_NS | LT_XML_NORMALIZE);
    LT_CHECK(r != NULL);
    if (!r) return;
    LT_CHECK_STR(lt_xml_attr_get(r->kids[0], "t"), "a\nb\rc d");
    lt_xml_free(r);
}

/* ---------------- writer ---------------- */

LT_TEST(writer_basic)
{
    lt_buf b;
    lt_xmlw w;
    lt_buf_init(&b);
    lt_xmlw_init(&w, &b, 4, 0);
    lt_xmlw_decl(&w, "ISO-8859-1", NULL);
    lt_xmlw_comment(&w, "x--y");
    lt_xmlw_open(&w, "msq");
    lt_xmlw_attr(&w, "xmlns", "http://www.msefi.com/:msq");
    lt_xmlw_open(&w, "bibliography");
    lt_xmlw_attr(&w, "author", "A & \"B\"");
    lt_xmlw_close(&w);
    lt_xmlw_open(&w, "page");
    lt_xmlw_attr_int(&w, "number", 0);
    lt_xmlw_attr_f(&w, "v", 1.5, 3);
    lt_xmlw_leaf(&w, "constant", "1 < 2");
    lt_xmlw_open(&w, "empty");
    lt_xmlw_text(&w, "");
    lt_xmlw_close(&w);
    lt_xmlw_close(&w);
    LT_CHECK_INT(lt_xmlw_finish(&w), 0);
    LT_CHECK_STR(b.data,
        "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n"
        "<!--x- -y-->\n"
        "<msq xmlns=\"http://www.msefi.com/:msq\">\n"
        "    <bibliography author=\"A &amp; &quot;B&quot;\"/>\n"
        "    <page number=\"0\" v=\"1.5\">\n"
        "        <constant>1 &lt; 2</constant>\n"
        "        <empty></empty>\n"
        "    </page>\n"
        "</msq>\n");
    lt_xmlw_free(&w);
    lt_buf_free(&b);
}

LT_TEST(writer_options)
{
    lt_buf b;
    lt_xmlw w;
    lt_buf_init(&b);
    lt_xmlw_init(&w, &b, 0, LT_XMLW_SPACE_SLASH);
    lt_xmlw_decl(&w, NULL, "no");
    lt_xmlw_open(&w, "a");
    lt_xmlw_attr(&w, "t", "l1\nl2\r\t");
    lt_xmlw_open(&w, "b");
    lt_xmlw_close(&w);
    lt_xmlw_open(&w, "c");
    lt_xmlw_cdata(&w, "x ]]> y");
    /* finish closes c and a */
    LT_CHECK_INT(lt_xmlw_finish(&w), 0);
    LT_CHECK_STR(b.data,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>"
        "<a t=\"l1&#10;l2&#13;&#9;\"><b /><c><![CDATA[x ]]]]><![CDATA[> y]]></c></a>");
    lt_xmlw_free(&w);
    lt_buf_free(&b);
}

LT_TEST(writer_misuse)
{
    lt_buf b;
    lt_xmlw w;
    lt_buf_init(&b);
    lt_xmlw_init(&w, &b, 2, 0);
    LT_CHECK_INT(lt_xmlw_close(&w), -1);
    LT_CHECK_INT(lt_xmlw_attr(&w, "a", "b"), -1);
    lt_xmlw_text(&w, "t");
    LT_CHECK(!lt_xmlw_ok(&w));
    LT_CHECK_INT(lt_xmlw_finish(&w), -1);
    LT_CHECK(lt_xmlw_ok(&w));
    lt_xmlw_open(&w, "a");
    lt_xmlw_text(&w, "x");
    LT_CHECK_INT(lt_xmlw_attr(&w, "late", "1"), -1);
    lt_xmlw_free(&w);
    lt_buf_free(&b);
}

LT_TEST(writer_roundtrip)
{
    lt_buf b;
    lt_xmlw w;
    lt_xml_node *r;
    const char *tricky = "a<b>&c\"d'e\nf\tg \xC3\xA9 ]]> --";
    lt_buf_init(&b);
    lt_xmlw_init(&w, &b, 2, 0);
    lt_xmlw_open(&w, "root");
    lt_xmlw_attr(&w, "v", tricky);
    lt_xmlw_leaf(&w, "t", tricky);
    lt_xmlw_open(&w, "cd");
    lt_xmlw_cdata(&w, tricky);
    lt_xmlw_close(&w);
    lt_xmlw_finish(&w);
    r = lt_xml_parse(b.data, b.len, LT_XML_NORMALIZE, err, sizeof err);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_STR(lt_xml_attr_get(r, "v"), tricky);
        LT_CHECK_STR(lt_xml_child(r, "t")->text, tricky);
        LT_CHECK_STR(lt_xml_child(r, "cd")->text, tricky);
        lt_xml_free(r);
    }
    lt_xmlw_free(&w);
    lt_buf_free(&b);
}

/* ---------------- fuzz ---------------- */

static lt_u32 rng_state = 12345UL;
static lt_u32 rnd(void)
{
    rng_state = (rng_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return rng_state >> 8;
}

LT_TEST(fuzz_mutations)
{
    size_t n = 0, m, k;
    char *d = lt_file_read("tests/fixtures/xml/sample.msq", &n);
    char *d2 = lt_file_read("tests/fixtures/xml/basic.dash", &m);
    static const char *frag[] = { "<", ">", "</", "/>", "&", ";", "\"", "'", "<![CDATA[", "]]>",
                                  "<!--", "-->", "<?", "?>", "<!DOCTYPE", "[", "]", "=", "&#x",
                                  "\xFF", "\xC3", "\n", ":", "xmlns:" };
    int iter, flagsets[4];
    unsigned ok = 0, bad = 0;
    char *tmp;
    flagsets[0] = 0;
    flagsets[1] = LT_XML_LENIENT;
    flagsets[2] = LT_XML_STRIP_NS | LT_XML_NORMALIZE;
    flagsets[3] = LT_XML_STRIP_NS | LT_XML_LENIENT | LT_XML_NORMALIZE;
    LT_CHECK(d != NULL && d2 != NULL);
    if (!d || !d2) { lt_free(d); lt_free(d2); return; }
    tmp = (char *)lt_malloc(n + m + 64);
    for (iter = 0; iter < 6000; iter++) {
        const char *src = (iter & 1) ? d : d2;
        size_t len = (iter & 1) ? n : m;
        size_t len2 = len;
        int nm = 1 + (int)(rnd() % 6);
        lt_xml_node *r;
        memcpy(tmp, src, len);
        for (k = 0; k < (size_t)nm && len2 > 4; k++) {
            size_t pos = rnd() % len2;
            switch (rnd() % 5) {
            case 0: tmp[pos] = (char)(rnd() & 0xFF); break;
            case 1: /* delete a run */
            {
                size_t cnt = 1 + rnd() % 20;
                if (pos + cnt > len2) cnt = len2 - pos;
                memmove(tmp + pos, tmp + pos + cnt, len2 - pos - cnt);
                len2 -= cnt;
                break;
            }
            case 2: /* truncate */
                len2 = pos + 1;
                break;
            case 3: /* insert fragment */
            {
                const char *f = frag[rnd() % LT_COUNTOF(frag)];
                size_t fl = strlen(f);
                if (len2 + fl < n + m + 60) {
                    memmove(tmp + pos + fl, tmp + pos, len2 - pos);
                    memcpy(tmp + pos, f, fl);
                    len2 += fl;
                }
                break;
            }
            default: /* duplicate a chunk */
            {
                size_t cnt = 1 + rnd() % 30;
                if (pos + cnt > len2) cnt = len2 - pos;
                if (len2 + cnt < n + m + 60) {
                    memmove(tmp + pos + cnt, tmp + pos, len2 - pos);
                    len2 += cnt;
                }
                break;
            }
            }
        }
        r = lt_xml_parse(tmp, len2, (unsigned)flagsets[iter & 3], err, sizeof err);
        if (r) { ok++; lt_xml_free(r); } else { bad++; LT_CHECK(err[0] != '\0'); }
    }
    LT_CHECK(ok > 500);     /* lenient runs mostly succeed */
    LT_CHECK(bad > 100);    /* strict runs mostly fail */
    lt_free(tmp);
    lt_free(d);
    lt_free(d2);
}

LT_TEST(fuzz_random_bytes)
{
    char buf[200];
    int iter, i;
    for (iter = 0; iter < 3000; iter++) {
        size_t len = rnd() % sizeof buf;
        lt_xml_node *r;
        for (i = 0; i < (int)len; i++) {
            switch (rnd() % 4) {
            case 0: buf[i] = "<>/&;\"'=![]-?"[rnd() % 14]; break;
            case 1: buf[i] = (char)('a' + rnd() % 26); break;
            case 2: buf[i] = (char)(rnd() & 0xFF); break;
            default: buf[i] = ' '; break;
            }
        }
        r = lt_xml_parse(buf, len, (unsigned)(iter & 7), NULL, 0);
        lt_xml_free(r);
        r = lt_xml_parse(buf, len, LT_XML_LENIENT, err, 1);
        lt_xml_free(r);
    }
    LT_CHECK(1);
}

LT_TEST(fuzz_prefix_truncations)
{
    size_t n = 0, k;
    char *d = lt_file_read("tests/fixtures/xml/sample.msq", &n);
    LT_CHECK(d != NULL);
    if (!d) return;
    for (k = 0; k <= n; k++) {
        lt_xml_node *r = lt_xml_parse(d, k, LT_XML_LENIENT, err, sizeof err);
        lt_xml_free(r);
        r = lt_xml_parse(d, k, 0, err, sizeof err);
        lt_xml_free(r);
    }
    LT_CHECK(1);
    lt_free(d);
}

int main(void)
{
    LT_RUN(basic_tree);
    LT_RUN(prolog_comments_doctype_pi);
    LT_RUN(cdata);
    LT_RUN(attr_quotes_gt);
    LT_RUN(attr_order_and_duplicates);
    LT_RUN(entities);
    LT_RUN(escape_fns);
    LT_RUN(normalize);
    LT_RUN(strip_ns);
    LT_RUN(latin1_fallback);
    LT_RUN(embedded_nul);
    LT_RUN(errors_strict);
    LT_RUN(lenient);
    LT_RUN(trailing_and_multiple_roots);
    LT_RUN(deep_nesting);
    LT_RUN(many_siblings);
    LT_RUN(sample_msq_fixture);
    LT_RUN(basic_dash_fixture);
    LT_RUN(tableview_style);
    LT_RUN(writer_basic);
    LT_RUN(writer_options);
    LT_RUN(writer_misuse);
    LT_RUN(writer_roundtrip);
    LT_RUN(fuzz_mutations);
    LT_RUN(fuzz_random_bytes);
    LT_RUN(fuzz_prefix_truncations);
    LT_TEST_MAIN_END
}
