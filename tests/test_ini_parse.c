/* test_ini_parse.c - INI parser tests (spec 01 + 02), string based. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "ini/ini.h"
#include "ini/ini_priv.h"
#include "lt_test.h"

/* NULL-terminated list of text pieces, concatenated at run time (C89 limits string literals
 * to 509 characters, so long INI texts are passed in several pieces). */
static lt_ini *load_n(const char *first, ...)
{
    lt_buf b;
    va_list ap;
    const char *s = first;
    lt_ini *ini;
    lt_buf_init(&b);
    va_start(ap, first);
    while (s) {
        lt_buf_puts(&b, s);
        s = va_arg(ap, const char *);
    }
    va_end(ap);
    ini = lt_ini_load_string(b.data ? b.data : "", b.len, NULL, NULL);
    lt_buf_free(&b);
    return ini;
}

static lt_ini *load_syms(const char *text, const char *const *syms, size_t n)
{
    lt_ini_options o;
    lt_ini_options_default(&o);
    o.symbols = syms;
    o.n_symbols = n;
    return lt_ini_load_string(text, strlen(text), NULL, &o);
}

/* ---------------------------------------------------------------- */
/* encoding                                                          */
/* ---------------------------------------------------------------- */

static char *dec_n(const char *bytes, size_t n)
{
    size_t out;
    return lt_ini_decode((const lt_u8 *)bytes, n, &out);
}

#define dec(s, n_) dec_n((s), strlen(s))

LT_TEST(encoding)
{
    char *s;
    int c;
    s = dec("Configura\xE7\xF5" "es", 12);
    LT_CHECK_STR(s, "Configura\xC3\xA7\xC3\xB5" "es");
    lt_free(s);
    s = dec("90\xB0" "C", 5);
    LT_CHECK_STR(s, "90\xC2\xB0" "C");
    lt_free(s);
    s = dec("acent\xE3o ma\xF1" "ana \xFC" "ber", 20);
    LT_CHECK_STR(s, "acent\xC3\xA3o ma\xC3\xB1" "ana \xC3\xBC" "ber");
    lt_free(s);
    s = dec("\xEF\xBB\xBFMapa de Combust\xC3\xAD" "vel", 24);
    LT_CHECK_STR(s, "Mapa de Combust\xC3\xAD" "vel");
    lt_free(s);
    s = dec("plain ascii", 11);
    LT_CHECK_STR(s, "plain ascii");
    lt_free(s);
    /* cp1252 0x80-0x9F block */
    s = dec("\x80", 1);
    LT_CHECK_STR(s, "\xE2\x82\xAC");          /* euro sign */
    lt_free(s);
    s = dec("\x99", 1);
    LT_CHECK_STR(s, "\xE2\x84\xA2");          /* trade mark */
    lt_free(s);
    /* one invalid byte makes the whole file cp1252 */
    s = dec("\xC3\xA9 \xE9", 4);
    LT_CHECK_STR(s, "\xC3\x83\xC2\xA9 \xC3\xA9");
    lt_free(s);
    /* never emits U+FFFD (EF BF BD) */
    for (c = 0x80; c <= 0xFF; c++) {
        char b[2];
        b[0] = (char)c;
        b[1] = '\0';
        s = lt_ini_decode((const lt_u8 *)b, 1, NULL);
        LT_CHECK(strstr(s, "\xEF\xBF\xBD") == NULL);
        lt_free(s);
    }
    /* overlong / surrogates are invalid UTF-8 -> cp1252 */
    s = dec("\xC0\x80", 2);
    LT_CHECK(strcmp(s, "\xC0\x80") != 0);
    lt_free(s);
    s = dec("\xED\xA0\x80", 3);
    LT_CHECK(strlen(s) > 3);
    lt_free(s);
}

LT_TEST(encoding_files)
{
    static const char *want = "Configura\xC3\xA7\xC3\xB5" "es de Igni\xC3\xA7\xC3\xA3o 90\xC2\xB0" "C";
    lt_ini *ini;
    const char *u8 = "[MegaTune]\nsignature = \"Configura\xC3\xA7\xC3\xB5" "es de Igni\xC3\xA7\xC3\xA3o 90\xC2\xB0" "C\"\n";
    const char *bom = "\xEF\xBB\xBF[MegaTune]\nsignature=\"Mapa de Combust\xC3\xAD" "vel\"\n";
    const char *cp = "[MegaTune]\nsignature = \"Configura\xE7\xF5" "es de Igni\xE7\xE3o 90\xB0" "C\"\n";
    ini = load_n(u8, (const char *)NULL);
    LT_CHECK_STR(ini->signature, want);
    lt_ini_free(ini);
    ini = load_n(bom, (const char *)NULL);
    LT_CHECK_STR(ini->signature, "Mapa de Combust\xC3\xAD" "vel");
    lt_ini_free(ini);
    ini = load_n(cp, (const char *)NULL);
    LT_CHECK_STR(ini->signature, want);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* lexical layer                                                     */
/* ---------------------------------------------------------------- */

static char *trim_text(const char *line)
{
    char *r = lt_strndup(line, ini_comment_cut(line));
    return r;
}

LT_TEST(strip_comment)
{
    char *s;
    s = trim_text("key = value ; comment");
    LT_CHECK_STR(lt_str_trim(s), "key = value");
    lt_free(s);
    s = trim_text("key = \"value ; with semi\"");
    LT_CHECK_STR(s, "key = \"value ; with semi\"");
    lt_free(s);
    s = trim_text("# comment line");
    LT_CHECK_STR(s, "# comment line");
    lt_free(s);
    s = trim_text("fieldname;+help text = value");
    LT_CHECK_STR(s, "fieldname;+help text = value");
    lt_free(s);
    s = trim_text("fieldname;+help = value ; comment");
    LT_CHECK_STR(lt_str_trim(s), "fieldname;+help = value");
    lt_free(s);
    s = trim_text("      ;fuelLoadBins = array, U08, 272, [ 16], \"kPa\", 2.0, 0.0, 0.0, 511.0, 0");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = trim_text("   ; keyword = referenceName, DisplayName");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = trim_text(";settingOption = BOOSTPSI, \"PSI\"");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = trim_text("bias_resistor;+Pull-up resistor = 4700");
    LT_CHECK_STR(s, "bias_resistor;+Pull-up resistor = 4700");
    lt_free(s);
    s = trim_text("someField = 42 ; trailing note");
    LT_CHECK_STR(lt_str_trim(s), "someField = 42");
    lt_free(s);
    s = trim_text("[MegaTune] ; this is a comment");
    LT_CHECK_STR(lt_str_trim(s), "[MegaTune]");
    lt_free(s);
    s = trim_text("blockReadTimeout = 3000; Milliseconds");
    LT_CHECK_STR(s, "blockReadTimeout = 3000");
    lt_free(s);
}

LT_TEST(split_line)
{
    char **p;
    size_t n;
    n = ini_split("a, \"b,c\", {f(x, y)}, d", &p);
    LT_CHECK_INT(n, 4);
    LT_CHECK_STR(p[0], "a");
    LT_CHECK_STR(p[1], "\"b,c\"");
    LT_CHECK_STR(p[2], "{f(x, y)}");
    LT_CHECK_STR(p[3], "d");
    ini_strv_free(p, n);
    n = ini_split("a,b,", &p);
    LT_CHECK_INT(n, 3);
    LT_CHECK_STR(p[2], "");
    ini_strv_free(p, n);
    n = ini_split("", &p);
    LT_CHECK_INT(n, 1);
    LT_CHECK_STR(p[0], "");
    ini_strv_free(p, n);
    n = ini_split("array, U08, 272, [16], { bitStringValue(algorithmUnits , algorithm) }, 1.0, 0.0, 0, 255, 0", &p);
    LT_CHECK_INT(n, 10);
    LT_CHECK_STR(p[4], "{ bitStringValue(algorithmUnits , algorithm) }");
    ini_strv_free(p, n);
    /* stray '}' suppresses all later splits */
    n = ini_split("a}, b, c", &p);
    LT_CHECK_INT(n, 1);
    ini_strv_free(p, n);
    n = ini_split_naive("a, \"b,c\", d", &p);
    LT_CHECK_INT(n, 4);
    ini_strv_free(p, n);
}

LT_TEST(dtype_names)
{
    lt_dtype t;
    int ov;
    LT_CHECK(lt_dtype_parse("U08", &t, &ov) == 0 && t == LT_DT_U08 && ov == -1);
    LT_CHECK(lt_dtype_parse("uint8", &t, &ov) == 0 && t == LT_DT_U08);
    LT_CHECK(lt_dtype_parse(" Word ", &t, &ov) == 0 && t == LT_DT_U16);
    LT_CHECK(lt_dtype_parse("SWORD", &t, &ov) == 0 && t == LT_DT_S16);
    LT_CHECK(lt_dtype_parse("DWORD", &t, &ov) == 0 && t == LT_DT_U32);
    LT_CHECK(lt_dtype_parse("sdword", &t, &ov) == 0 && t == LT_DT_S32);
    LT_CHECK(lt_dtype_parse("FLOAT", &t, &ov) == 0 && t == LT_DT_F32);
    LT_CHECK(lt_dtype_parse("double", &t, &ov) == 0 && t == LT_DT_F64);
    LT_CHECK(lt_dtype_parse("ASCII", &t, &ov) == 0 && t == LT_DT_STRING);
    LT_CHECK(lt_dtype_parse("BITS", &t, &ov) == 0 && t == LT_DT_BITS);
    LT_CHECK(lt_dtype_parse("BU16", &t, &ov) == 0 && t == LT_DT_U16 && ov == LT_ENDIAN_BIG);
    LT_CHECK(lt_dtype_parse("BS32", &t, &ov) == 0 && t == LT_DT_S32 && ov == LT_ENDIAN_BIG);
    LT_CHECK(lt_dtype_parse("BU08", &t, &ov) == 0 && ov == LT_ENDIAN_BIG);
    LT_CHECK(lt_dtype_parse("BF32", &t, &ov) == 0 && t == LT_DT_F32 && ov == LT_ENDIAN_BIG);
    LT_CHECK(lt_dtype_parse("U99", &t, &ov) != 0);
    LT_CHECK(lt_dtype_parse("", &t, &ov) != 0);
    LT_CHECK_INT(lt_dtype_size(LT_DT_U16), 2);
    LT_CHECK_INT(lt_dtype_size(LT_DT_F64), 8);
    LT_CHECK_INT(lt_dtype_size(LT_DT_STRING), 0);
    LT_CHECK_INT(lt_dtype_size(LT_DT_BITS), 1);
    LT_CHECK_STR(lt_dtype_name(LT_DT_BITS), "Bits");
    LT_CHECK_STR(lt_dtype_name(LT_DT_S16), "S16");
}

LT_TEST(shapes)
{
    lt_shape s;
    lt_dynsize *d;
    ini_parse_shape("[16]", &s, &d);
    LT_CHECK(s.kind == LT_SH_1D && s.n == 16 && d == NULL);
    ini_parse_shape("[ 16]", &s, &d);
    LT_CHECK(s.kind == LT_SH_1D && s.n == 16);
    ini_parse_shape("[16x16]", &s, &d);
    LT_CHECK(s.kind == LT_SH_2D && s.rows == 16 && s.cols == 16);
    ini_parse_shape("[8X12]", &s, &d);
    LT_CHECK(s.kind == LT_SH_2D && s.cols == 8 && s.rows == 12);
    ini_parse_shape("[6x2]", &s, &d);
    LT_CHECK(s.kind == LT_SH_2D && s.cols == 6 && s.rows == 2);
    LT_CHECK_INT(lt_shape_count(&s), 12);
    LT_CHECK_INT(lt_shape_x_size(&s), 6);
    LT_CHECK_INT(lt_shape_y_size(&s), 2);
    ini_parse_shape("", &s, &d);
    LT_CHECK(s.kind == LT_SH_SCALAR);
    LT_CHECK_INT(lt_shape_count(&s), 1);
    ini_parse_shape("[{veTableCols}x{veTableRows}]", &s, &d);
    LT_CHECK(s.kind == LT_SH_2D && d != NULL);
    LT_CHECK_STR(d->cols_const, "veTableCols");
    LT_CHECK_STR(d->rows_const, "veTableRows");
    lt_free(d->cols_const);
    lt_free(d->rows_const);
    lt_free(d);
    ini_parse_shape("[{veTableRows}]", &s, &d);
    LT_CHECK(s.kind == LT_SH_1D && d != NULL && d->cols_const == NULL);
    LT_CHECK_STR(d->rows_const, "veTableRows");
    lt_free(d->rows_const);
    lt_free(d);
    ini_parse_shape("[abc]", &s, &d);
    LT_CHECK(s.kind == LT_SH_SCALAR);
    ini_parse_shape("[4xq]", &s, &d);
    LT_CHECK(s.kind == LT_SH_SCALAR);
}

LT_TEST(bit_ranges)
{
    int pos, size, disp;
    LT_CHECK(ini_parse_bit_range("[6:6]", &pos, &size, &disp));
    LT_CHECK(pos == 6 && size == 1 && disp == 0);
    LT_CHECK(ini_parse_bit_range("[4:7+1]", &pos, &size, &disp));
    LT_CHECK(pos == 4 && size == 4 && disp == 1);
    LT_CHECK(ini_parse_bit_range("[0:3-1]", &pos, &size, &disp));
    LT_CHECK(pos == 0 && size == 4 && disp == -1);
    LT_CHECK(ini_parse_bit_range("[0:7]", &pos, &size, &disp));
    LT_CHECK(pos == 0 && size == 8 && disp == 0);
    LT_CHECK(ini_parse_bit_range("[0:3]", &pos, &size, &disp));
    LT_CHECK(size == 4);
    LT_CHECK(ini_parse_bit_range("[5]", &pos, &size, &disp));
    LT_CHECK(pos == 5 && size == 1);
    LT_CHECK(ini_parse_bit_range("[16:31]", &pos, &size, &disp));
    LT_CHECK(pos == 16 && size == 16);
    LT_CHECK(!ini_parse_bit_range("[0:32]", &pos, &size, &disp));
    LT_CHECK(!ini_parse_bit_range("[x:3]", &pos, &size, &disp));
}

LT_TEST(escapes_and_substitution)
{
    lt_ini_blob b;
    lt_map pcv;
    b = ini_parse_escape("R\\x00\\x0f\\n\\r\\\\\\0Z", 18);
    LT_CHECK_INT(b.n, 8);
    LT_CHECK(b.p[0] == 'R' && b.p[1] == 0 && b.p[2] == 0x0f && b.p[3] == '\n' &&
             b.p[4] == '\r' && b.p[5] == '\\' && b.p[6] == 0 && b.p[7] == 'Z');
    ini_blob_free(&b);
    b = ini_parse_escape("\\q\\x1", 5);   /* unknown escape and short hex keep the backslash */
    LT_CHECK_INT(b.n, 5);
    LT_CHECK(b.p[0] == '\\' && b.p[1] == 'q' && b.p[2] == '\\' && b.p[3] == 'x');
    ini_blob_free(&b);
    b = ini_parse_escape("\\x41", 4);      /* \x41 at the very end decodes */
    LT_CHECK(b.n == 1 && b.p[0] == 'A');
    ini_blob_free(&b);

    lt_map_init(&pcv, LT_FALSE);
    lt_map_put(&pcv, "tsCanId", (void *)(size_t)1);          /* byte 0 */
    b = ini_substitute("$tsCanId\\x04", 12, &pcv);
    LT_CHECK_INT(b.n, 5);
    LT_CHECK(b.p[0] == 0 && b.p[1] == '\\' && b.p[2] == 'x' && b.p[3] == '0' && b.p[4] == '4');
    ini_blob_free(&b);
    b = ini_substitute("\\$tsCanId\\x04", 13, &pcv);
    LT_CHECK_INT(b.n, 5);
    LT_CHECK(b.p[0] == 0 && b.p[1] == '\\');
    ini_blob_free(&b);
    b = ini_substitute("prefix$tsCanId", 14, &pcv);
    LT_CHECK_INT(b.n, 7);
    LT_CHECK(b.p[6] == 0);
    ini_blob_free(&b);
    b = ini_substitute("$unknownVar", 11, &pcv);
    LT_CHECK_INT(b.n, 11);
    LT_CHECK_STR(b.p, "$unknownVar");
    ini_blob_free(&b);
    b = ini_substitute("\\$unknownVar", 12, &pcv);
    LT_CHECK_STR(b.p, "\\$unknownVar");
    ini_blob_free(&b);
    lt_map_put(&pcv, "tsCanId", (void *)(size_t)(5 + 1));
    b = ini_substitute("a$tsCanId", 9, &pcv);
    LT_CHECK(b.n == 2 && b.p[1] == 5);
    ini_blob_free(&b);
    lt_map_free(&pcv);
}

/* ---------------------------------------------------------------- */
/* preprocessor                                                      */
/* ---------------------------------------------------------------- */

LT_TEST(preprocessor_basic)
{
    static const char *src =
        "[MegaTune]\n"
        "#if CAN_COMMANDS\n"
        "queryCommand=\"r\\x00\\x0f\\x00\\x00\\x00\\x14\"\n"
        "#else\n"
        "queryCommand=\"Q\"\n"
        "#endif\n";
    const char *syms[] = {"CAN_COMMANDS"};
    lt_ini *ini = load_syms(src, syms, 1);
    LT_CHECK(ini->proto.query_command.p[0] == 'r');
    lt_ini_free(ini);
    ini = load_n(src, (const char *)NULL);
    LT_CHECK_STR(ini->proto.query_command.p, "Q");
    LT_CHECK(lt_ini_symbol_tested(ini, "CAN_COMMANDS"));
    LT_CHECK(!lt_ini_symbol_active(ini, "CAN_COMMANDS"));
    lt_ini_free(ini);
    ini = load_n("#set CAN_COMMANDS\n[MegaTune]\n#if CAN_COMMANDS\nqueryCommand=\"r\"\n#else\nqueryCommand=\"Q\"\n#endif\n", (const char *)NULL);
    LT_CHECK_STR(ini->proto.query_command.p, "r");
    LT_CHECK(lt_ini_symbol_active(ini, "CAN_COMMANDS"));
    lt_ini_free(ini);
}

LT_TEST(preprocessor_set_unset)
{
    lt_ini *ini = load_n("[MegaTune]\n#set FEATURE\n#unset FEATURE\n#if FEATURE\nsignature=enabled\n#else\nsignature=disabled\n#endif\n", (const char *)NULL);
    LT_CHECK_STR(ini->signature, "disabled");
    LT_CHECK(!lt_ini_symbol_active(ini, "FEATURE"));
    lt_ini_free(ini);
    /* set in a dead arm does not leak; unset in a dead arm does not clear */
    ini = load_n("#set KEEPME\n#if NEVER_DEFINED\n#set LEAKED\n#unset KEEPME\n#endif\n", (const char *)NULL);
    LT_CHECK(!lt_ini_symbol_active(ini, "LEAKED"));
    LT_CHECK(lt_ini_symbol_active(ini, "KEEPME"));
    lt_ini_free(ini);
    /* '!X' is just an undefined symbol; #ifdef etc are skipped */
    ini = load_n("[MegaTune]\n#if !X\nsignature=neg\n#endif\n", (const char *)NULL);
    LT_CHECK_STR(ini->signature, "");
    lt_ini_free(ini);
}

LT_TEST(preprocessor_seed_units)
{
    static const char *src =
        "[Constants]\npage = 1\n"
        "#if CELSIUS\n"
        "tempTest = scalar, U08, 0, \"C\", 1.0, -40, -40, 102.0, 0\n"
        "#else\n"
        "tempTest = scalar, U08, 0, \"F\", 1.8, -22.23, -40, 215.0, 0\n"
        "#endif\n";
    const char *syms[] = {"CELSIUS"};
    lt_ini *ini = load_syms(src, syms, 1);
    const lt_ini_const *c = lt_ini_find_const(ini, "tempTest");
    LT_CHECK(c != NULL);
    LT_CHECK_STR(c->units, "C");
    LT_CHECK(lt_ini_symbol_active(ini, "CELSIUS"));
    LT_CHECK(lt_ini_symbol_tested(ini, "CELSIUS"));
    lt_ini_free(ini);
    ini = load_n(src, (const char *)NULL);
    c = lt_ini_find_const(ini, "tempTest");
    LT_CHECK_STR(c->units, "F");
    LT_CHECK_NEAR(c->scale, 1.8, 1e-12);
    lt_ini_free(ini);
    ini = load_n("[Constants]\nx = scalar, U08, 0, \"\", 1, 0, 0, 1, 0\n", (const char *)NULL);
    LT_CHECK(!lt_ini_symbol_tested(ini, "CELSIUS"));
    lt_ini_free(ini);
}

LT_TEST(conditional_headers)
{
    static const char *src =
        "[MegaTune]\nsignature = \"BaseECU\"\n"
        "[TunerStudio &EXTRA_TUNERSTUDIO]\nsignature = \"OverrideECU\"\n";
    const char *syms[] = {"EXTRA_TUNERSTUDIO"};
    lt_ini *ini = load_syms(src, syms, 1);
    LT_CHECK_STR(ini->signature, "OverrideECU");
    lt_ini_free(ini);
    ini = load_n(src, (const char *)NULL);
    LT_CHECK_STR(ini->signature, "BaseECU");
    lt_ini_free(ini);
    ini = load_n("[MegaTune]\nsignature=\"Base\"\n[TunerStudio &!CAN_COMMANDS]\nsignature=\"NoCan\"\n", (const char *)NULL);
    LT_CHECK_STR(ini->signature, "NoCan");
    lt_ini_free(ini);
    /* a suppressed header silences following keys until the next header */
    ini = load_n("[MegaTune]\nsignature=\"A\"\n[TunerStudio &NOPE]\nsignature=\"B\"\nversionInfo=\"V\"\n[MegaTune]\nqueryCommand=\"S\"\n", (const char *)NULL);
    LT_CHECK_STR(ini->signature, "A");
    LT_CHECK_STR(ini->version_info, "");
    LT_CHECK_STR(ini->proto.query_command.p, "S");
    lt_ini_free(ini);
    /* [UiDialogs] aliases [UserDefined] */
    ini = load_n("[UiDialogs]\ndialog = d1, \"One\"\n", (const char *)NULL);
    LT_CHECK(lt_ini_find_dialog(ini, "d1") != NULL);
    lt_ini_free(ini);
}

LT_TEST(define_lists)
{
    lt_ini *ini = load_n(
        "#define gpio_list=\"NONE\", \"INVALID\", \"PA0\", \"PA1\"\n",
        "#define more = $gpio_list, \"PB0\"\n",
        "[Constants]\npage = 1\n",
        "pin = bits, U08, 0, [0:7], $gpio_list, \"X\"\n",
        "pin2 = bits, U08, 1, [0:7], $more\n",
        "pin3 = bits, U08, 2, [0:7], $nosuch\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "pin");
    LT_CHECK(c != NULL);
    LT_CHECK_INT(c->n_bit_options, 5);
    LT_CHECK_STR(c->bit_options[0], "NONE");
    LT_CHECK_STR(c->bit_options[3], "PA1");
    LT_CHECK_STR(c->bit_options[4], "X");
    c = lt_ini_find_const(ini, "pin2");
    LT_CHECK_INT(c->n_bit_options, 5);
    LT_CHECK_STR(c->bit_options[4], "PB0");
    c = lt_ini_find_const(ini, "pin3");
    LT_CHECK_INT(c->n_bit_options, 1);
    LT_CHECK_STR(c->bit_options[0], "$nosuch");
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* includes                                                          */
/* ---------------------------------------------------------------- */

static void wfile(const char *path, const char *text)
{
    LT_CHECK(lt_file_write(path, text, strlen(text)) == 0);
}

LT_TEST(includes)
{
    lt_ini *ini;
    const char *dir = "tests/fixtures/ini/_tmp_inc";
    char *p;
    LT_UNUSED(dir);
    /* tests are run from the project root by build.sh */
    wfile("build/inc_main.ini",
          "[MegaTune]\nsignature = \"Main\"\n#include \"inc_level1.ini\"\n[Constants]\npage = 1\nmainConst = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n");
    wfile("build/inc_level1.ini",
          "[Constants]\npage = 1\nlevel1Const = scalar, U08, 1, \"\", 1, 0, 0, 255, 0\n#include \"inc_level2.ini\"\n");
    wfile("build/inc_level2.ini",
          "[Constants]\npage = 1\nlevel2Const = scalar, U08, 2, \"\", 1, 0, 0, 255, 0\n");
    ini = lt_ini_load_file("build/inc_main.ini", NULL);
    LT_CHECK(ini != NULL);
    if (ini) {
        LT_CHECK(lt_ini_find_const(ini, "mainConst") != NULL);
        LT_CHECK(lt_ini_find_const(ini, "level1Const") != NULL);
        LT_CHECK(lt_ini_find_const(ini, "level2Const") != NULL);
        LT_CHECK_STR(ini->signature, "Main");
        lt_ini_free(ini);
    }

    /* circular */
    wfile("build/inc_a.ini", "#include \"inc_b.ini\"\n");
    wfile("build/inc_b.ini", "#include \"inc_a.ini\"\n");
    ini = lt_ini_load_file("build/inc_a.ini", NULL);
    LT_CHECK(ini == NULL);
    LT_CHECK(strstr(lt_err_get(), "circular") != NULL);

    /* conditional include */
    wfile("build/inc_cond.ini",
          "#set INCLUDE_EXTRA\n#if INCLUDE_EXTRA\n#include \"inc_extra.ini\"\n#endif\n");
    wfile("build/inc_cond2.ini", "#if INCLUDE_EXTRA\n#include \"inc_extra.ini\"\n#endif\n");
    wfile("build/inc_extra.ini", "[Constants]\npage = 1\nextra = scalar, U08, 0, \"\", 1, 0, 0, 1, 0\n");
    ini = lt_ini_load_file("build/inc_cond.ini", NULL);
    LT_CHECK(ini != NULL && lt_ini_find_const(ini, "extra") != NULL);
    lt_ini_free(ini);
    ini = lt_ini_load_file("build/inc_cond2.ini", NULL);
    LT_CHECK(ini != NULL && lt_ini_find_const(ini, "extra") == NULL);
    lt_ini_free(ini);

    /* missing include / missing top-level file */
    wfile("build/inc_missing.ini", "#include \"does_not_exist.ini\"\n");
    ini = lt_ini_load_file("build/inc_missing.ini", NULL);
    LT_CHECK(ini == NULL);
    LT_CHECK(strstr(lt_err_get(), "not found") != NULL);
    ini = lt_ini_load_file("build/definitely_missing.ini", NULL);
    LT_CHECK(ini == NULL);

    /* include path relative to the including file's directory */
    p = lt_path_join("build", "sub");
    LT_CHECK(p != NULL);
    lt_free(p);
    wfile("build/inc_sub_main.ini", "#include \"inc_extra.ini\"\n");
    ini = lt_ini_load_file("build/inc_sub_main.ini", NULL);
    LT_CHECK(ini != NULL && lt_ini_find_const(ini, "extra") != NULL);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* identity + protocol                                               */
/* ---------------------------------------------------------------- */

LT_TEST(protocol_keys)
{
    lt_ini *ini = load_n(
        "[MegaTune]\nsignature = \"sig 1\"\nqueryCommand = \"S\"\nversionInfo = \"S\"\n",
        "[TunerStudio]\niniSpecVersion = 3.64\nendianness = little\nnPages = 3\n",
        "pageSize = 40560, 256, 2048\nmessageEnvelopeFormat = msEnvelope_1.0\n",
        "queryCommand = \"S\"\nsignaturePrefix = \"sig\"\ndefaultIpPort = 29001\n",
        "[Constants]\npageIdentifier = \"\\x00\\x00\", \"\\x00\\x01\"\n",
        "pageReadCommand = \"R%2i%2o%2c\", \"R%2i%2o%2c\"\n",
        "burnCommand = \"B%2i\", \"\", \"\"\n",
        "crc32CheckCommand = \"k%2i%2o%2c\"\n",
        "retrieveConfigError = \"e\"\n",
        "pageActivationDelay = 500\nblockingFactor = 2560 ; max chunk\n",
        "interWriteDelay = 10\nblockReadTimeout = 3000\ndelayAfterPortOpen = 1000 ; settle\n",
        "tsWriteBlocks = on\nenable2ndByteCanID = false\nnoCommReadDelay = true;\n",
        "[OutputChannels]\nochGetCommand = \"O%2o%2c\"\nochBlockSize = 2268\n",
        "[BurstMode]\ngetCommand = \"A\"\n", (const char *)NULL);
    const lt_ini_protocol *p = &ini->proto;
    LT_CHECK_STR(ini->signature, "sig 1");
    LT_CHECK_STR(ini->signature_prefix, "sig");
    LT_CHECK_STR(ini->ini_spec_version, "3.64");
    LT_CHECK(ini->endian == LT_ENDIAN_LITTLE);
    LT_CHECK_INT(ini->n_pages, 3);
    LT_CHECK_INT(p->n_page_sizes, 3);
    LT_CHECK_INT(p->page_sizes[0], 40560);
    LT_CHECK_INT(p->page_sizes[2], 2048);
    LT_CHECK_INT(lt_ini_total_memory(ini), 40560 + 256 + 2048);
    LT_CHECK_STR(p->message_envelope_format, "msEnvelope_1.0");
    LT_CHECK(p->uses_modern);
    LT_CHECK_STR(p->query_command.p, "S");
    LT_CHECK_INT(p->n_page_identifiers, 2);
    LT_CHECK(p->page_identifiers[0].n == 2 && p->page_identifiers[0].p[0] == 0 &&
             p->page_identifiers[0].p[1] == 0);
    LT_CHECK(p->page_identifiers[1].n == 2 && p->page_identifiers[1].p[1] == 1);
    LT_CHECK_INT(p->page_read_commands.n, 2);
    LT_CHECK_STR(p->page_read_commands.v[0].p, "R%2i%2o%2c");
    LT_CHECK_INT(p->burn_commands.n, 3);
    LT_CHECK_STR(p->burn_commands.v[0].p, "B%2i");
    LT_CHECK_INT(p->burn_commands.v[1].n, 0);
    LT_CHECK_STR(p->crc32_check_commands.v[0].p, "k%2i%2o%2c");
    LT_CHECK_STR(p->retrieve_config_error, "e");
    LT_CHECK_INT(p->page_activation_delay, 500);
    LT_CHECK_INT(p->blocking_factor, 2560);
    LT_CHECK_INT(p->inter_write_delay, 10);
    LT_CHECK_INT(p->block_read_timeout, 3000);
    LT_CHECK_INT(p->delay_after_port_open, 1000);
    LT_CHECK_INT(p->write_blocks, 1);
    LT_CHECK_INT(p->enable_can_id, 0);
    LT_CHECK_INT(p->no_comm_read_delay, 0);
    LT_CHECK_STR(p->och_get_command.p, "O%2o%2c");
    LT_CHECK_INT(p->och_block_size, 2268);
    LT_CHECK_STR(p->burst_get_command.p, "A");
    LT_CHECK_INT(p->default_ip_port, 29001);
    lt_ini_free(ini);

    /* defaults */
    ini = load_n("[MegaTune]\nsignature=\"x\"\n", (const char *)NULL);
    LT_CHECK_STR(ini->proto.query_command.p, "Q");
    LT_CHECK_STR(ini->proto.burst_get_command.p, "A");
    LT_CHECK_INT(ini->proto.page_activation_delay, 500);
    LT_CHECK_INT(ini->proto.blocking_factor, 256);
    LT_CHECK_INT(ini->proto.block_read_timeout, 1000);
    LT_CHECK_INT(ini->proto.write_blocks, 1);
    LT_CHECK_INT(ini->proto.enable_can_id, 1);
    LT_CHECK_INT(ini->proto.default_ip_port, 29001);
    LT_CHECK_INT(ini->proto.table_blocking_factor, -1);
    LT_CHECK_INT(ini->proto.default_runtime_record_per_sec, -1);
    LT_CHECK(ini->endian == LT_ENDIAN_BIG);
    LT_CHECK_STR(ini->ini_spec_version, "3.64");
    lt_ini_free(ini);
    /* top-level key before any section is ignored (rusEFI) */
    ini = load_n("enable2ndByteCanID = false\n[MegaTune]\nsignature=\"x\"\n", (const char *)NULL);
    LT_CHECK_INT(ini->proto.enable_can_id, 1);
    lt_ini_free(ini);
    /* write blocks: only on/true/1 enable */
    ini = load_n("[Constants]\nwriteBlocks = off\n", (const char *)NULL);
    LT_CHECK_INT(ini->proto.write_blocks, 0);
    lt_ini_free(ini);
    ini = load_n("[Constants]\nwriteBlocks = maybe\n", (const char *)NULL);
    LT_CHECK_INT(ini->proto.write_blocks, 0);
    lt_ini_free(ini);
    /* protocol keys are also accepted in [TunerStudio] */
    ini = load_n("[TunerStudio]\nnoCommReadDelay = 50\nuseLegacyFTempUnits = true\ndefaultRuntimeRecordPerSec = 20\n", (const char *)NULL);
    LT_CHECK_INT(ini->proto.no_comm_read_delay, 50);
    LT_CHECK_INT(ini->proto.use_legacy_f_temp_units, 1);
    LT_CHECK_INT(ini->proto.default_runtime_record_per_sec, 20);
    lt_ini_free(ini);
}

LT_TEST(tscanid_substitution)
{
    lt_ini *ini = load_n(
        "[PcVariables]\ntsCanId = bits, U08, [0:3], \"CAN ID 0\", \"CAN ID 1\", \"CAN ID 2\"\n",
        "[Constants]\npageIdentifier = \"\\$tsCanId\\x04\", \"\\$tsCanId\\x05\"\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "tsCanId");
    LT_CHECK(c != NULL);
    LT_CHECK(c->type == LT_DT_BITS && c->bit_pos == 0 && c->bit_size == 4 &&
             c->bit_disp == 0 && c->n_bit_options == 3 && c->is_pc_var && c->page == 255);
    LT_CHECK_INT(lt_ini_pcvar_byte(ini, "tsCanId"), 0);
    LT_CHECK_INT(ini->proto.n_page_identifiers, 2);
    LT_CHECK(ini->proto.page_identifiers[0].n == 2 && ini->proto.page_identifiers[0].p[0] == 0 &&
             ini->proto.page_identifiers[0].p[1] == 4);
    LT_CHECK(ini->proto.page_identifiers[1].p[1] == 5);
    LT_CHECK_INT(lt_ini_pcvar_byte(ini, "nope"), -1);
    lt_ini_free(ini);

    /* declared default in [Defaults]; Constants parsed BEFORE PcVariables */
    ini = load_n(
        "[Constants]\npageIdentifier=\"\\x00$tsCanId\"\npageReadCommand=\"r$tsCanId%2o%2c\"\n",
        "[PcVariables]\ntsCanId = scalar, U08, \"\", 1, 0, 0, 15, 0\n",
        "[Defaults]\ndefaultValue = tsCanId, 3\n", (const char *)NULL);
    LT_CHECK_INT(lt_ini_pcvar_byte(ini, "tsCanId"), 3);
    LT_CHECK(ini->proto.page_identifiers[0].n == 2 && ini->proto.page_identifiers[0].p[1] == 3);
    LT_CHECK(ini->proto.page_read_commands.v[0].p[0] == 'r' &&
             ini->proto.page_read_commands.v[0].p[1] == 3);
    LT_CHECK_STR(ini->proto.page_read_commands.v[0].p + 2, "%2o%2c");
    lt_ini_free(ini);

    /* query command: NUL after substitution, escapes stay text */
    ini = load_n(
        "[MegaTune]\nqueryCommand = \"r\\$tsCanId\\x0f\\x00\\x00\\x00\\x14\"\n",
        "[PcVariables]\ntsCanId = bits, U08, [0:3], \"A\", \"B\"\n", (const char *)NULL);
    LT_CHECK(ini->proto.query_command.p[0] == 'r' && ini->proto.query_command.p[1] == 0);
    LT_CHECK_STR(ini->proto.query_command.p + 2, "\\x0f\\x00\\x00\\x00\\x14");
    LT_CHECK_INT(ini->proto.query_command.n, 2 + 20);
    lt_ini_free(ini);

    /* unknown var stays literal */
    ini = load_n("[Constants]\npageReadCommand=\"r$nope%2o\"\n", (const char *)NULL);
    LT_CHECK_STR(ini->proto.page_read_commands.v[0].p, "r$nope%2o");
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* constants                                                         */
/* ---------------------------------------------------------------- */

LT_TEST(constant_lines)
{
    lt_ini *ini = load_n(
        "[Constants]\npageSize = 256, 128\npage = 1\n",
        "reqFuel = scalar, U16, 0, \"ms\", 0.1, 0.0, 0, 25.5, 1\n",
        "IAT_X_Voltage = array, U16, 19, [16], \"V\", 0.001, 0, 0, 5.000, 3\n",
        "afrTable = array, U08, lastOffset, [16x16], \"AFR\", 0.1, 0.0, 7, 25.5, 1\n",
        "loadBinsAFR = array, U08, 272, [16], { bitStringValue(algorithmUnits , algorithm) }, 1.0, 0.0, 0, 255, 0\n",
        "bare = scalar, U08, 5\n",
        "page = 2\n",
        "p2const = scalar, S16, 10, \"x\", 2, 1, -5, 5, 0\n",
        "page = 0\n",
        "p0const = scalar, U08, 0, \"\"\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "reqFuel");
    LT_CHECK(c && c->type == LT_DT_U16 && c->offset == 0 && c->page == 0);
    LT_CHECK_NEAR(c->scale, 0.1, 1e-12);
    LT_CHECK_NEAR(c->max, 25.5, 1e-12);
    LT_CHECK_INT(c->digits, 1);
    LT_CHECK_STR(c->units, "ms");
    LT_CHECK_INT(c->size_bytes, 2);
    c = lt_ini_find_const(ini, "IAT_X_Voltage");
    LT_CHECK(c && c->shape.kind == LT_SH_1D && c->shape.n == 16 && c->digits == 3);
    LT_CHECK_INT(c->offset, 19);
    LT_CHECK_INT(c->size_bytes, 32);
    c = lt_ini_find_const(ini, "afrTable");
    LT_CHECK(c && c->shape.kind == LT_SH_2D && c->shape.rows == 16 && c->shape.cols == 16);
    LT_CHECK_INT(c->offset, 19);   /* lastOffset = previous constant's own offset */
    LT_CHECK_NEAR(c->min, 7, 1e-12);
    c = lt_ini_find_const(ini, "loadBinsAFR");
    LT_CHECK(c && c->shape.n == 16);
    LT_CHECK_STR(c->units, "{ bitStringValue(algorithmUnits , algorithm) }");
    c = lt_ini_find_const(ini, "bare");
    LT_CHECK_NEAR(c->min, 0, 0);
    LT_CHECK_NEAR(c->max, 255, 0);
    LT_CHECK_NEAR(c->scale, 1, 0);
    c = lt_ini_find_const(ini, "p2const");
    LT_CHECK(c && c->page == 1 && c->type == LT_DT_S16);
    LT_CHECK_NEAR(c->translate, 1, 0);
    c = lt_ini_find_const(ini, "p0const");
    LT_CHECK(c && c->page == 0);
    lt_ini_free(ini);
}

LT_TEST(last_offset_overlays)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "flagsByte = bits, U08, 100, [0:3], \"A\", \"B\"\n",
        "afterBits = scalar, U08, lastOffset, \"\", 1, 0, 0, 255, 0\n",
        "lambdaTable = array, U08, 0, [16x16], \"\", 1, 0, 0, 255, 0\n",
        "afrTable = array, U08, lastOffset, [16x16], \"\", 1, 0, 0, 255, 0\n",
        "rpmBinsAFR = array, U08, 256, [16], \"\", 1, 0, 0, 255, 0\n",
        "ego_min_afr = scalar, U08, 8, \"\", 1, 0, 0, 255, 0\n",
        "ego_min_lambda = scalar, U08, lastOffset, \"\", 1, 0, 0, 255, 0\n",
        "ego_max_afr = scalar, U08, 9, \"\", 1, 0, 0, 255, 0\n",
        "ego_max_lambda = scalar, U08, lastOffset, \"\", 1, 0, 0, 255, 0\n", (const char *)NULL);
    LT_CHECK_INT(lt_ini_find_const(ini, "afterBits")->offset, 100);
    LT_CHECK_INT(lt_ini_find_const(ini, "afrTable")->offset, 0);
    LT_CHECK_INT(lt_ini_find_const(ini, "rpmBinsAFR")->offset, 256);
    LT_CHECK_INT(lt_ini_find_const(ini, "ego_min_lambda")->offset, 8);
    LT_CHECK_INT(lt_ini_find_const(ini, "ego_max_lambda")->offset, 9);
    lt_ini_free(ini);
}

LT_TEST(constant_bits_and_help)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "injectionMode;+\"Pick the mode\" = bits, U08, 9, [0:1], \"Simultaneous\",\"Sequential\",\"Semi-Sequential\"\n",
        "bias_resistor;+Pull-up resistor value on your board = scalar, U16, 0, \"Ohm\", 1, 0, 0, 65535, 0\n",
        "baseFuel;+\"Base mass\" ;\"Ohm\" = scalar, U16, 2, \"mg\", 1, 0, 0, 65535, 0\n",
        "field_name;This text is no help = scalar, U08, 4, \"\", 1, 0, 0, 255, 0\n",
        "  spaced  ;  +  Help text with spaces   = scalar, U08, 5, \"\", 1, 0, 0, 255, 0\n",
        "nCylinders = bits, U08, 6, [4:7+1], \"1\", \"2\", \"3\"\n",
        "lvl = bits, U08, 7, [0:3-1], \"a\", \"b\"\n",
        "algo = bits, U08, 8, [0:2], \"Speed Density\", \"\", \"Alpha-N\", \"\"\n",
        "pins = bits, U16, 10, [0:6], 0=\"DEFAULT\", 2=\"TWO\", 22=\"BMW\"\n",
        "be = scalar, BU16, 12, \"\", 1, 0, 0, 65535, 0\n",
        "str = string, ASCII, 20, 32\n",
        "[SettingContextHelp]\nreqFuelX = \"nothing\"\nnCylinders = \"Number of cylinders\\nfoo\"\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "injectionMode");
    LT_CHECK(c && c->type == LT_DT_BITS && c->bit_pos == 0 && c->bit_size == 2);
    LT_CHECK_STR(c->help, "Pick the mode");
    LT_CHECK_INT(c->n_bit_options, 3);
    LT_CHECK_STR(lt_ini_bit_option(c, 2), "Semi-Sequential");
    LT_CHECK(lt_ini_bit_option(c, 3) == NULL);
    LT_CHECK_INT(lt_ini_bit_index(c, "Sequential"), 1);
    LT_CHECK_INT(lt_ini_bit_index(c, "sequential"), 1);
    LT_CHECK_INT(lt_ini_bit_index(c, "nope"), -1);
    LT_CHECK_STR(lt_ini_find_const(ini, "bias_resistor")->help, "Pull-up resistor value on your board");
    LT_CHECK_STR(lt_ini_find_const(ini, "baseFuel")->help, "Base mass");
    LT_CHECK(lt_ini_find_const(ini, "field_name") != NULL);
    LT_CHECK(lt_ini_find_const(ini, "field_name")->help == NULL);
    c = lt_ini_find_const(ini, "spaced");
    LT_CHECK(c != NULL);
    LT_CHECK_STR(c->help, "Help text with spaces");
    c = lt_ini_find_const(ini, "nCylinders");
    LT_CHECK(c->bit_pos == 4 && c->bit_size == 4 && c->bit_disp == 1);
    LT_CHECK_STR(c->help, "Number of cylinders\\nfoo");   /* escape text stays literal */
    c = lt_ini_find_const(ini, "lvl");
    LT_CHECK(c->bit_disp == -1 && c->bit_size == 4);
    /* empty positional entries keep their position */
    c = lt_ini_find_const(ini, "algo");
    LT_CHECK_INT(c->n_bit_options, 3);
    LT_CHECK_STR(c->bit_options[0], "Speed Density");
    LT_CHECK(c->bit_options[1] == NULL);
    LT_CHECK_STR(c->bit_options[2], "Alpha-N");
    /* sparse N="label" form */
    c = lt_ini_find_const(ini, "pins");
    LT_CHECK_INT(c->n_bit_options, 23);
    LT_CHECK_STR(c->bit_options[0], "DEFAULT");
    LT_CHECK(c->bit_options[1] == NULL);
    LT_CHECK_STR(c->bit_options[2], "TWO");
    LT_CHECK_STR(c->bit_options[22], "BMW");
    c = lt_ini_find_const(ini, "be");
    LT_CHECK(c->endian_override == LT_ENDIAN_BIG);
    c = lt_ini_find_const(ini, "str");
    LT_CHECK(c->type == LT_DT_STRING && c->shape.n == 32 && c->size_bytes == 32);
    lt_ini_free(ini);
}

LT_TEST(raw_display_conversion)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "x = scalar, U08, 0, \"\", 0.1, 0.0, 0, 25.5, 1\n",
        "y = scalar, U08, 1, \"\", {sc}, 0, 0, {mx}, 1\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "x");
    LT_CHECK_NEAR(lt_const_raw_to_display(c, 147), 14.7, 0.01);
    LT_CHECK_NEAR(lt_const_display_to_raw(c, 14.7), 147, 0.01);
    LT_CHECK_NEAR(lt_const_display_to_raw(c, 99.0), 255, 0.01);   /* clamped to max 25.5 */
    LT_CHECK(lt_const_in_range(c, 10));
    LT_CHECK(!lt_const_in_range(c, 30));
    c = lt_ini_find_const(ini, "y");
    LT_CHECK_STR(c->scale_expr, "sc");
    LT_CHECK_STR(c->max_expr, "mx");
    LT_CHECK(c->min_expr == NULL && c->translate_expr == NULL);
    LT_CHECK(!c->range_resolved);
    LT_CHECK_NEAR(c->scale, 1.0, 0);
    LT_CHECK_NEAR(lt_const_display_to_raw(c, 1000), 1000, 1e-9);   /* no clamp while unresolved */
    lt_ini_free(ini);
}

LT_TEST(pc_variables)
{
    lt_ini *ini = load_n(
        "[PcVariables]\n",
        "rpmwarn = scalar, U16, \"rpm\", 1, 0, 0, 30000, 0\n",
        "wueAfrTargetOffset = array, S16, [ 16], \":1\", 0.1, 0.0, -3.0, 3.0, 1\n",
        "lim = scalar, U08, \"\", 1, 0, 0, { 10 - 2 }, 0\n",
        "tuneCrcPcVariable = continuousChannelValue, tuneCrc16\n",
        "[Defaults]\ndefaultValue = rpmwarn, 6500\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "rpmwarn");
    LT_CHECK(c && c->page == 255 && c->is_pc_var && c->type == LT_DT_U16);
    LT_CHECK_STR(c->units, "rpm");
    LT_CHECK_NEAR(c->max, 30000, 0);
    c = lt_ini_find_const(ini, "wueAfrTargetOffset");
    LT_CHECK(c && c->shape.kind == LT_SH_1D && c->shape.n == 16 && c->type == LT_DT_S16);
    LT_CHECK_NEAR(c->min, -3.0, 0);
    LT_CHECK_NEAR(c->scale, 0.1, 0);
    c = lt_ini_find_const(ini, "lim");
    LT_CHECK_STR(c->max_expr, "10 - 2");
    LT_CHECK(lt_ini_find_const(ini, "tuneCrcPcVariable") == NULL);
    LT_CHECK_INT(lt_ini_pcvar_byte(ini, "rpmwarn"), 255);   /* default 6500 clamped to a byte */
    LT_CHECK_INT(lt_ini_pcvar_byte(ini, "wueAfrTargetOffset"), -1);   /* arrays not registered */
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* output channels                                                   */
/* ---------------------------------------------------------------- */

LT_TEST(output_channel_lines)
{
    lt_ini *ini = load_n(
        "[OutputChannels]\n",
        "rpm = U16, 0, \"RPM\", 1.0, 0.0\n",
        "clt = scalar, S16, 2, \"C\", 0.1, -40.0\n",
        "crank = bits, U32, 852, [11:11]\n",
        "flag = bits, U32, 1, [8:8]\n",
        "run = bits, U08, 0, [1:1]\n",
        "nSquirts = bits, U08, 84, [5:7]\n",
        "errorNum = bits, U08, 75, [0:1]\n",
        "currentError = bits, U08, 75, [2:7]\n",
        "afr_type = bits, S08, 753, [0:2]\n",
        "launchMode = bits, S08, 753, [6:7]\n",
        "hasSync = bits, U08, 31, [7:7]\n",
        "afr = { ego1 / 10.0 }, \"AFR\"\n",
        "plain = bits, U08, 3\n",
        "scatteredGetEnabled = { 0 }\n",
        "broken = { 1 + \n",
        "ochBlockSize = 40\n", (const char *)NULL);
    const lt_ini_channel *c = lt_ini_find_channel(ini, "rpm");
    LT_CHECK(c && c->type == LT_DT_U16 && c->offset == 0);
    LT_CHECK_STR(c->units, "RPM");
    c = lt_ini_find_channel(ini, "clt");
    LT_CHECK(c && c->type == LT_DT_S16 && c->offset == 2);
    LT_CHECK_NEAR(c->scale, 0.1, 0);
    LT_CHECK_NEAR(c->translate, -40.0, 0);
    c = lt_ini_find_channel(ini, "crank");
    LT_CHECK(c->type == LT_DT_BITS && c->bit_storage == LT_DT_U32 && c->bit_pos == 11 &&
             c->bit_count == 1);
    LT_CHECK_INT(lt_ini_channel_size(c), 4);
    c = lt_ini_find_channel(ini, "flag");
    LT_CHECK_INT(lt_ini_channel_size(c), 4);
    c = lt_ini_find_channel(ini, "nSquirts");
    LT_CHECK(c->bit_pos == 5 && c->bit_count == 3);
    c = lt_ini_find_channel(ini, "currentError");
    LT_CHECK(c->bit_pos == 2 && c->bit_count == 6);
    c = lt_ini_find_channel(ini, "afr");
    LT_CHECK(c && c->expression != NULL && c->ast != NULL);
    LT_CHECK_STR(c->expression, "ego1 / 10.0");
    LT_CHECK_STR(c->units, "AFR");
    LT_CHECK_INT(lt_ini_channel_size(c), 0);
    LT_CHECK(lt_ini_find_channel(ini, "scatteredGetEnabled") == NULL);
    LT_CHECK(lt_ini_find_channel(ini, "broken") == NULL);
    LT_CHECK_INT(ini->proto.och_block_size, 40);
    LT_CHECK(lt_ini_find_channel(ini, "ochBlockSize") == NULL);
    c = lt_ini_find_channel(ini, "plain");
    LT_CHECK(c && c->bit_pos == -1);
    LT_CHECK_INT(c->index, 12);
    LT_CHECK_INT(ini->n_channels, 13);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* gauges, datalog, frontpage                                        */
/* ---------------------------------------------------------------- */

LT_TEST(gauges)
{
    lt_ini *ini = load_n(
        "[GaugeConfigurations]\n",
        "gaugeCategory = \"Engine\"\n",
        "rpmGauge = rpm, \"Engine Speed\", \"RPM\", 0, 8000, 300, 600, 6500, 7000, 0\n",
        "tachometer = rpm, \"Engine Speed\", \"RPM\", 0, {rpmhigh}, 300, 600, {rpmwarn}, {rpmdang}, 0, 0\n",
        "gaugeCategory = \"Fuel\"\n",
        "gppwmOutput1 = gppwmOutput1, { bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }, \"%\", 0, 100, 0, 0, 100, 100, 1\n",
        "zones = rpm, \"Z\", \"RPM\", 0, 9000, 500, 800, 6500, 7000, 0\n", (const char *)NULL);
    const lt_ini_gauge *g = lt_ini_find_gauge(ini, "rpmGauge");
    LT_CHECK(g != NULL);
    LT_CHECK_STR(g->channel, "rpm");
    LT_CHECK_STR(g->title, "Engine Speed");
    LT_CHECK_STR(g->units, "RPM");
    LT_CHECK_NEAR(g->hi, 8000, 0);
    LT_CHECK_NEAR(g->low_danger, 300, 0);
    LT_CHECK_NEAR(g->high_danger, 7000, 0);
    LT_CHECK_STR(g->category, "Engine");
    LT_CHECK(lt_ini_find_gauge(ini, "gaugeCategory") == NULL);
    g = lt_ini_find_gauge(ini, "tachometer");
    LT_CHECK_STR(g->hi_expr, "rpmhigh");
    LT_CHECK_STR(g->high_warning_expr, "rpmwarn");
    LT_CHECK_STR(g->high_danger_expr, "rpmdang");
    LT_CHECK(g->lo_expr == NULL);
    LT_CHECK_NEAR(g->low_danger, 300, 0);
    LT_CHECK_NEAR(g->lo, 0, 0);
    LT_CHECK_NEAR(g->hi, 100, 0);   /* fallback while unresolved */
    g = lt_ini_find_gauge(ini, "gppwmOutput1");
    LT_CHECK_STR(g->title, "{ bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }");
    LT_CHECK_STR(g->units, "%");
    LT_CHECK_INT(g->digits, 1);
    LT_CHECK_STR(g->category, "Fuel");
    g = lt_ini_find_gauge(ini, "zones");
    LT_CHECK(lt_ini_gauge_is_danger(g, 400));
    LT_CHECK(lt_ini_gauge_is_danger(g, 7500));
    LT_CHECK(lt_ini_gauge_is_warning(g, 600));
    LT_CHECK(lt_ini_gauge_is_warning(g, 6800));
    LT_CHECK(lt_ini_gauge_is_normal(g, 3000));
    LT_CHECK(!lt_ini_gauge_is_normal(g, 600));
    LT_CHECK(!lt_ini_gauge_is_danger(g, 3000));
    LT_CHECK_INT(ini->n_gauges, 4);
    lt_ini_free(ini);
}

LT_TEST(datalog_section)
{
    lt_ini *ini = load_n(
        "[Datalog]\n",
        "entry = rpm, \"RPM\", int, \"%d\"\n",
        "entry = time, \"Time\", float, \"%.3f\"\n",
        "entry = stagingActive, \"Fuel Staging\", int, \"onOff\", { stagingEnabled }\n",
        "entry = boostDuty, \"Boost Duty\", int, \"%d\", { bitStringValue(boostLabels, boostType) }\n",
        "entry = afr, \"AFR\"\n",
        "[DatalogViews]\nview1 = \"Basic\", rpm, afr\n", (const char *)NULL);
    LT_CHECK_INT(ini->n_dl_entries, 5);
    LT_CHECK_STR(ini->dl_entries[0].channel, "rpm");
    LT_CHECK_STR(ini->dl_entries[0].type, "int");
    LT_CHECK_STR(ini->dl_entries[0].format, "%d");
    LT_CHECK(ini->dl_entries[0].condition == NULL && ini->dl_entries[0].enabled);
    LT_CHECK_STR(ini->dl_entries[1].format, "%.3f");
    LT_CHECK_STR(ini->dl_entries[2].format, "onOff");
    LT_CHECK_STR(ini->dl_entries[2].condition, "stagingEnabled");
    LT_CHECK_STR(ini->dl_entries[3].condition, "bitStringValue(boostLabels, boostType)");
    LT_CHECK_STR(ini->dl_entries[4].type, "float");
    LT_CHECK_STR(ini->dl_entries[4].format, "%.2f");
    LT_CHECK_STR(ini->dl_entries[4].label, "AFR");
    LT_CHECK_INT(ini->n_dl_views, 1);
    LT_CHECK_STR(ini->dl_views[0].label, "Basic");
    LT_CHECK_INT(ini->dl_views[0].n_channels, 2);
    lt_ini_free(ini);
}

LT_TEST(frontpage)
{
    lt_ini *ini = load_n(
        "[FrontPage]\n",
        "gauge1 = tachometer\ngauge2 = throttleGauge\ngauge8 = iatGauge\n",
        "gauge4000000000 = bogus\ngauge0 = zero\n",
        "indicator = { (tps > tpsflood) && (rpm < crankRPM) }, \"FLOOD OFF\", \"FLOOD CLEAR\", white, black, red, black\n",
        "indicator = { running }, \"Not Running\", { isRunning ? \"On\" : \"Off\" }, white, black, green, black\n",
        "indicator = { bad }, \"only\", \"three\"\n", (const char *)NULL);
    LT_CHECK(ini->frontpage != NULL);
    LT_CHECK_INT(ini->frontpage->n_gauges, 8);
    LT_CHECK_STR(ini->frontpage->gauges[0], "tachometer");
    LT_CHECK_STR(ini->frontpage->gauges[1], "throttleGauge");
    LT_CHECK_STR(ini->frontpage->gauges[2], "");
    LT_CHECK_STR(ini->frontpage->gauges[7], "iatGauge");
    LT_CHECK_INT(ini->frontpage->n_inds, 2);
    LT_CHECK_STR(ini->frontpage->inds[0].expression, "(tps > tpsflood) && (rpm < crankRPM)");
    LT_CHECK_STR(ini->frontpage->inds[0].label_off, "FLOOD OFF");
    LT_CHECK_STR(ini->frontpage->inds[0].bg_on, "red");
    LT_CHECK_STR(ini->frontpage->inds[0].bg_off, "white");
    LT_CHECK_STR(ini->frontpage->inds[0].fg_off, "black");
    LT_CHECK_STR(ini->frontpage->inds[1].label_on, "{ isRunning ? \"On\" : \"Off\" }");
    lt_ini_free(ini);
    ini = load_n("[MegaTune]\nsignature=\"x\"\n", (const char *)NULL);
    LT_CHECK(ini->frontpage == NULL);
    lt_ini_free(ini);
}

LT_TEST(colors)
{
    lt_u32 c;
    LT_CHECK(lt_ini_parse_color("white", &c) && c == 0xFFFFFFFFUL);
    LT_CHECK(lt_ini_parse_color("Red", &c) && c == 0xFFFF0000UL);
    LT_CHECK(lt_ini_parse_color("lime", &c) && c == 0xFF00FF00UL);
    LT_CHECK(lt_ini_parse_color("GREY", &c) && c == 0xFF808080UL);
    LT_CHECK(lt_ini_parse_color("#ffa500", &c) && c == 0xFFFFA500UL);
    LT_CHECK(lt_ini_parse_color("#f80", &c) && c == 0xFFFF8800UL);
    LT_CHECK(!lt_ini_parse_color("chartreuse", &c));
    LT_CHECK(!lt_ini_parse_color("#12345", &c));
    LT_CHECK(!lt_ini_parse_color(NULL, &c));
}

/* ---------------------------------------------------------------- */
/* menus, dialogs                                                    */
/* ---------------------------------------------------------------- */

LT_TEST(menus)
{
    lt_ini *ini = load_n(
        "[Menu]\n",
        "menu = \"Test Menu\"\n",
        "subMenu = dwell_tblMap    \"Dwell Map\", { useDwellMap }\n",
        "subMenu = stagingMap      \"Fuel Staging\", { stagingMode == 0 }\n",
        "subMenu = normalMenu, \"Normal Label\", { someCondition }\n",
        "subMenu = dcMotorActuatorHw  \"DC motor actuator(s) hardware\", { 1 }, { uiMode == 0 || uiMode == 1 }\n",
        "subMenu = onlyTarget\n",
        "std_separator\n",
        "subMenu = std_realtime, \"Realtime\"\n",
        "subMenu = std_separator\n",
        "subMenu = veTable1Tbl, \"VE\"\n",
        "subMenu = veMapName, \"VE by map name\"\n",
        "subMenu = warmCurve, \"Warm\"\n",
        "subMenu = helpTopic, \"Help\"\n",
        "subMenu = vis_and_en, \"Both\", {a}, {b}\n",
        "groupMenu = \"Group\", {gvis}, {gen}\n",
        "groupChildMenu = child1, \"Child 1\"\n",
        "groupChildMenu = child2, \"Child 2\", {c2}\n",
        "subMenu = after_group, \"After\"\n",
        "menu = \"&Second\"\n",
        "subMenu = a2, \"A2\"\n",
        "[TableEditor]\ntable = veTable1Tbl, veMapName, \"VE Table\", 2\n",
        "[CurveEditor]\ncurve = warmCurve, \"Warmup\"\n",
        "[UserDefined]\nhelp = helpTopic, \"Topic\"\ndialog = child1, \"C1\"\n", (const char *)NULL);
    const lt_ini_menu *m;
    LT_CHECK_INT(ini->n_menus, 2);
    m = &ini->menus[0];
    LT_CHECK_STR(m->name, "Test Menu");
    LT_CHECK_STR(m->title, "Test Menu");
    LT_CHECK_INT(m->n_items, 15);
    LT_CHECK(m->items[0]->kind == LT_MENU_DIALOG);
    LT_CHECK_STR(m->items[0]->target, "dwell_tblMap");
    LT_CHECK_STR(m->items[0]->label, "Dwell Map");
    LT_CHECK_STR(m->items[0]->en_cond, "useDwellMap");
    LT_CHECK(m->items[0]->vis_cond == NULL);
    LT_CHECK_STR(m->items[1]->en_cond, "stagingMode == 0");
    LT_CHECK_STR(m->items[2]->label, "Normal Label");
    LT_CHECK_STR(m->items[2]->en_cond, "someCondition");
    LT_CHECK_STR(m->items[3]->target, "dcMotorActuatorHw");
    LT_CHECK_STR(m->items[3]->label, "DC motor actuator(s) hardware");
    LT_CHECK(m->items[3]->vis_cond == NULL);
    LT_CHECK_STR(m->items[3]->en_cond, "uiMode == 0 || uiMode == 1");
    LT_CHECK_STR(m->items[4]->label, "onlyTarget");
    LT_CHECK(m->items[5]->kind == LT_MENU_SEPARATOR);
    LT_CHECK(m->items[6]->kind == LT_MENU_STD);
    LT_CHECK_STR(m->items[6]->target, "std_realtime");
    LT_CHECK(m->items[7]->kind == LT_MENU_SEPARATOR);
    LT_CHECK(m->items[8]->kind == LT_MENU_TABLE);    /* table by name */
    LT_CHECK(m->items[9]->kind == LT_MENU_TABLE);    /* retyped: table by map name */
    LT_CHECK(m->items[10]->kind == LT_MENU_TABLE);   /* curve */
    LT_CHECK(m->items[11]->kind == LT_MENU_HELP);
    LT_CHECK_STR(m->items[12]->vis_cond, "a");
    LT_CHECK_STR(m->items[12]->en_cond, "b");
    LT_CHECK(m->items[13]->kind == LT_MENU_SUBMENU);
    LT_CHECK_STR(m->items[13]->label, "Group");
    LT_CHECK_STR(m->items[13]->vis_cond, "gvis");
    LT_CHECK_STR(m->items[13]->en_cond, "gen");
    /* after the submenu: children went into it, later subMenu is a sibling menu */
    LT_CHECK_INT(m->items[13]->n_items, 2);
    LT_CHECK_STR(m->items[13]->items[0]->label, "Child 1");
    LT_CHECK_STR(m->items[13]->items[1]->en_cond, "c2");
    LT_CHECK(m->items[13]->items[0]->kind == LT_MENU_DIALOG);
    LT_CHECK_STR(ini->menus[1].name, "&Second");
    LT_CHECK_INT(ini->menus[1].n_items, 1);
    lt_ini_free(ini);
}

LT_TEST(menu_after_group_goes_to_menu)
{
    lt_ini *ini = load_n(
        "[Menu]\nmenu = \"M\"\ngroupMenu = \"G\"\ngroupChildMenu = c, \"C\"\nsubMenu = s, \"S\"\n", (const char *)NULL);
    LT_CHECK_INT(ini->menus[0].n_items, 2);
    LT_CHECK_INT(ini->menus[0].items[0]->n_items, 1);
    LT_CHECK_STR(ini->menus[0].items[1]->label, "S");
    lt_ini_free(ini);
    /* items before any menu are dropped */
    ini = load_n("[Menu]\nsubMenu = x, \"X\"\n", (const char *)NULL);
    LT_CHECK_INT(ini->n_menus, 0);
    lt_ini_free(ini);
}

LT_TEST(dialogs)
{
    lt_ini *ini = load_n(
        "[UserDefined]\n",
        "dialog = userTable1left, \"\"\n",
        "  field = \"Enabled\", userTable1Enabled\n",
        "  runtimeValue = \"X Axis value\", userTableXAxis1, { userTable1Enabled }\n",
        "  runtimeValue = \"Output value\", userTableOutput1\n",
        "dialog = someOtherDialog, \"Unrelated\"\n",
        "field = \"Unrelated field\", someConstant\n",
        "indicatorPanel = fuelClosedLoopIndicatorsPanel\n",
        "indicator = { isTuningNow }, \"No tuning happening\", \"Tuning Detected\", white, black, green, black\n",
        "indicator = { Correction using bitStringValue(a, b) }, \"{ x }\", \"{ y }\"\n",
        "dialog = fuelClosedLoopDialog, \"Short term fuel trim/Closed loop\", xAxis\n",
        "panel = fuelClosedLoopBank1\n",
        "dialog = plainDialog, \"No hint\"\n",
        "checkbox = \"Use Prediction\", use_prediction, { !use_prediction }\n",
        "checkbox = \"Don't use Prediction\", use_prediction, flipValue, { use_prediction }\n",
        "checkbox = \"VisEn\", cbx, { vv }, { ee }\n",
        "field = \"Both\", fld, { fv }, { fe }\n",
        "field = \"OnlyEn\", fld2, { fe2 }\n",
        "field = \"# blue header\"\n",
        "field = \"! red note\"\n",
        "slider = \"Sld\", sl, horizontal, { sv }\n",
        "displayOnlyField = \"Disp\", dispName, { dv }\n",
        "displayOnlyField = \"Disp2\", blankfield\n",
        "displayOnlyField = \"Disp3\"\n",
        "panel = pnlA, West, { pen }, { pvis }\n",
        "panel = pnlB, { pen2 }\n",
        "panel = pnlC, { 1 }\n",
        "liveGraph = lg1, \"Graph\", East\n",
        "graphLine = rpm\ngraphLine = map\n",
        "table = tbl1\n",
        "commandButton = \"Go\", cmdGo, { cen }, clickOnCloseIfEnabled\n",
        "gauge = gg, North\n",
        "settingSelector = \"Common MAP Sensors\", {iat_adcChannel != 0}\n",
        "settingOption = \"MPX4115\", EngineCylinder=10.6, EngineVolume=121.7\n",
        "settingOption = \"GM 1-BAR\", EngineCylinder=10, EngineVolume=105\n",
        "field = \"After selector\", afterSel\n",
        "settingOption = \"dropped\", X=1\n",
        "topicHelp = \"http://help/dialog\"\n", (const char *)NULL);
    const lt_ini_dialog *d;
    const lt_ini_comp *c;
    const lt_ini_indicator_panel *ip;

    d = lt_ini_find_dialog(ini, "userTable1left");
    LT_CHECK(d != NULL);
    LT_CHECK_STR(d->title, "");
    LT_CHECK_INT(d->n_comps, 3);
    LT_CHECK(d->comps[0].kind == LT_COMP_FIELD);
    LT_CHECK_STR(d->comps[0].label, "Enabled");
    LT_CHECK_STR(d->comps[0].name, "userTable1Enabled");
    LT_CHECK(d->comps[1].kind == LT_COMP_RUNTIMEVALUE);
    LT_CHECK_STR(d->comps[1].vis_cond, "userTable1Enabled");
    LT_CHECK(d->comps[2].vis_cond == NULL);
    d = lt_ini_find_dialog(ini, "someOtherDialog");
    LT_CHECK_INT(d->n_comps, 1);
    LT_CHECK_INT(ini->n_ind_panels, 1);
    ip = ini->ind_panels[0];
    LT_CHECK_STR(ip->name, "fuelClosedLoopIndicatorsPanel");
    LT_CHECK_INT(ip->columns, 2);
    LT_CHECK_INT(ip->n_inds, 2);
    LT_CHECK_STR(ip->inds[0].expression, "isTuningNow");
    LT_CHECK_STR(ip->inds[0].label_on, "Tuning Detected");
    LT_CHECK_STR(ip->inds[0].off_fg, "white");
    LT_CHECK_STR(ip->inds[0].off_bg, "black");
    LT_CHECK_STR(ip->inds[0].on_fg, "green");
    LT_CHECK_STR(ip->inds[0].on_bg, "black");
    LT_CHECK_STR(ip->inds[1].label_off, "{ x }");
    d = lt_ini_find_dialog(ini, "fuelClosedLoopDialog");
    LT_CHECK_STR(d->layout_hint, "xAxis");
    LT_CHECK_INT(d->n_comps, 1);
    LT_CHECK(d->comps[0].kind == LT_COMP_PANEL);
    LT_CHECK_STR(d->comps[0].name, "fuelClosedLoopBank1");
    d = lt_ini_find_dialog(ini, "plainDialog");
    LT_CHECK(d->layout_hint == NULL);
    LT_CHECK_STR(d->help_topic, "http://help/dialog");
    LT_CHECK_INT(d->n_comps, 20);
    c = &d->comps[0];
    LT_CHECK(c->kind == LT_COMP_CHECKBOX && c->en_cond && !c->vis_cond && !c->flip);
    LT_CHECK_STR(c->en_cond, "!use_prediction");
    c = &d->comps[1];
    LT_CHECK(c->flip);
    LT_CHECK_STR(c->en_cond, "use_prediction");
    c = &d->comps[2];
    LT_CHECK_STR(c->vis_cond, "vv");
    LT_CHECK_STR(c->en_cond, "ee");
    c = &d->comps[3];
    LT_CHECK(c->kind == LT_COMP_FIELD);
    LT_CHECK_STR(c->vis_cond, "fv");
    LT_CHECK_STR(c->en_cond, "fe");
    c = &d->comps[4];
    LT_CHECK(c->vis_cond == NULL);
    LT_CHECK_STR(c->en_cond, "fe2");
    c = &d->comps[5];
    LT_CHECK(c->kind == LT_COMP_LABEL);
    LT_CHECK_STR(c->label, "# blue header");
    LT_CHECK_STR(d->comps[6].label, "! red note");
    c = &d->comps[7];
    LT_CHECK(c->kind == LT_COMP_SLIDER);
    LT_CHECK_STR(c->orientation, "horizontal");
    LT_CHECK_STR(c->en_cond, "sv");
    c = &d->comps[8];
    LT_CHECK(c->kind == LT_COMP_DISPLAYONLY);
    LT_CHECK_STR(c->name, "dispName");
    LT_CHECK_STR(c->en_cond, "dv");
    LT_CHECK(d->comps[9].name == NULL);
    LT_CHECK_STR(d->comps[9].label, "Disp2");
    LT_CHECK(d->comps[10].name == NULL);
    c = &d->comps[11];
    LT_CHECK(c->kind == LT_COMP_PANEL);
    LT_CHECK_STR(c->position, "West");
    LT_CHECK_STR(c->en_cond, "pen");     /* panels: enable first, visibility second */
    LT_CHECK_STR(c->vis_cond, "pvis");
    c = &d->comps[12];
    LT_CHECK(c->position == NULL);
    LT_CHECK_STR(c->en_cond, "pen2");
    LT_CHECK(c->vis_cond == NULL);
    c = &d->comps[13];
    LT_CHECK(c->en_cond == NULL && c->vis_cond == NULL);   /* trivial { 1 } dropped */
    c = &d->comps[14];
    LT_CHECK(c->kind == LT_COMP_LIVEGRAPH);
    LT_CHECK_STR(c->name, "lg1");
    LT_CHECK_STR(c->label, "Graph");
    LT_CHECK_STR(c->position, "East");
    LT_CHECK_INT(c->n_channels, 2);
    LT_CHECK_STR(c->channels[1], "map");
    LT_CHECK(d->comps[15].kind == LT_COMP_TABLE);
    LT_CHECK_STR(d->comps[15].name, "tbl1");
    c = &d->comps[16];
    LT_CHECK(c->kind == LT_COMP_CMDBUTTON);
    LT_CHECK_STR(c->label, "Go");
    LT_CHECK_STR(c->name, "cmdGo");
    LT_CHECK_STR(c->en_cond, "cen");
    LT_CHECK(c->close == LT_CLOSE_IF_ENABLED);
    c = &d->comps[17];
    LT_CHECK(c->kind == LT_COMP_GAUGE);
    LT_CHECK_STR(c->position, "North");
    c = &d->comps[18];
    LT_CHECK(c->kind == LT_COMP_SETTINGSEL);
    LT_CHECK_STR(c->vis_cond, "iat_adcChannel != 0");
    LT_CHECK_INT(c->n_options, 2);
    LT_CHECK_STR(c->options[0].label, "MPX4115");
    LT_CHECK_INT(c->options[0].n_assigns, 2);
    LT_CHECK_STR(c->options[0].assigns[0].name, "EngineCylinder");
    LT_CHECK_NEAR(c->options[0].assigns[0].value, 10.6, 1e-12);
    LT_CHECK_NEAR(c->options[1].assigns[1].value, 105, 1e-12);
    LT_CHECK(d->comps[19].kind == LT_COMP_FIELD);
    lt_ini_free(ini);
}

LT_TEST(help_and_readouts)
{
    lt_ini *ini = load_n(
        "[UserDefined]\n",
        "help = myTopic, \"My Topic\"\nwebHelp = \"http://x/y\"\ntext = \"line one\"\ntext = \"line two\"\n",
        "readoutPanel = rp1, 3, { vis1 }\n",
        "readout = rpm, \"Engine RPM\", \"rpm\", 0, 1\n",
        "readout = map, \"MAP\"\n",
        "readoutPanel = rp2\n",
        "indicatorPanel = ip1, 4, { v2 }\n", (const char *)NULL);
    const lt_ini_help *h = lt_ini_find_help(ini, "myTopic");
    LT_CHECK(h != NULL);
    LT_CHECK_STR(h->title, "My Topic");
    LT_CHECK_STR(h->web_url, "http://x/y");
    LT_CHECK_INT(h->n_lines, 2);
    LT_CHECK_STR(h->lines[1], "line two");
    LT_CHECK_INT(ini->n_readout_panels, 2);
    LT_CHECK_INT(ini->readout_panels[0]->columns, 3);
    LT_CHECK_STR(ini->readout_panels[0]->vis_cond, "vis1");
    LT_CHECK_INT(ini->readout_panels[0]->n_readouts, 2);
    LT_CHECK_STR(ini->readout_panels[0]->readouts[0].units, "rpm");
    LT_CHECK_INT(ini->readout_panels[0]->readouts[0].digits, 0);
    LT_CHECK_INT(ini->readout_panels[0]->readouts[0].precision, 1);
    LT_CHECK_STR(ini->readout_panels[0]->readouts[1].units, "");
    LT_CHECK_INT(ini->readout_panels[1]->columns, 1);
    LT_CHECK_INT(ini->ind_panels[0]->columns, 4);
    LT_CHECK_STR(ini->ind_panels[0]->vis_cond, "v2");
    lt_ini_free(ini);
}

LT_TEST(std_panels)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\nalgorithm = bits, U08, 0, [0:1], \"A\", \"B\"\n",
        "reqFuel = scalar, U08, 1, \"ms\", 1, 0, 0, 25, 1\n", (const char *)NULL);
    const lt_ini_dialog *d = lt_ini_std_panel(ini, "std_injection");
    LT_CHECK(d != NULL);
    LT_CHECK_STR(d->title, "Injection Setup");
    LT_CHECK_INT(d->n_comps, 2);
    LT_CHECK_STR(d->comps[0].label, "Control Algorithm");
    LT_CHECK_STR(d->comps[1].label, "Required Fuel");
    LT_CHECK(lt_ini_std_panel(ini, "std_injection") == d);   /* cached */
    LT_CHECK(lt_ini_std_panel(ini, "std_ms3Rtc") == NULL);   /* no rtc_trim constant */
    LT_CHECK(lt_ini_std_panel(ini, "std_other") == NULL);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* tables, curves                                                    */
/* ---------------------------------------------------------------- */

LT_TEST(tables_and_curves)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "veRpmBins = array, U08, 0, [16], \"rpm\", 1, 0, 0, 255, 0\n",
        "veFuelBins = array, U08, 16, [16], \"kPa\", 1, 0, 0, 255, 0\n",
        "veTable = array, U08, 32, [16x16], \"%\", 1, 0, 0, 255, 0\n",
        "oneZ = array, U08, 300, [8x12], \"%\", 1, 0, 0, 255, 0\n",
        "[TableEditor]\n",
        "table = veTable1Tbl, veTable1Map, \"VE Table\", 2\n",
        "xBins = veRpmBins, rpm\nyBins = veFuelBins, fuelLoad\nzBins = veTable\n",
        "gridHeight = 2.0\ngridOrient = 250, 0, 340\ntopicHelp = \"http://ve\"\n",
        "xyLabels = \"RPM\", { bitStringValue(a, b) }\nupDownLabel = \"(RICHER)\", \"(LEANER)\"\n",
        "table = onlyZ, onlyZMap, \"Only Z\"\nzBins = oneZ\n",
        "[CurveEditor]\n",
        "curve = warmup_analyzer_curve, \"Warmup Enrichment\"\n",
        "columnLabel = \"Coolant\", \"WUE\"\nxAxis = 0, 8000, 9\nyAxis = 50, 250, 5\n",
        "xBins = knockNoiseRpmBins, RPMValue\nyBins = wueRates\nyBins = wueRates2\n",
        "size = 10\ngauge = cltGauge\ntopicHelp = \"http://w\"\n",
        "lineLabel = \"Current WUE\"\nlineLabel = \"Recommended WUE\"\n", (const char *)NULL);
    const lt_ini_table *t = lt_ini_find_table(ini, "veTable1Tbl");
    const lt_ini_curve *c;
    LT_CHECK(t != NULL);
    LT_CHECK(lt_ini_find_table(ini, "veTable1Map") == t);   /* by map name too */
    LT_CHECK_STR(t->title, "VE Table");
    LT_CHECK_INT(t->page, 2);                                /* not normalised */
    LT_CHECK_STR(t->x_const, "veRpmBins");
    LT_CHECK_STR(t->x_channel, "rpm");
    LT_CHECK_STR(t->y_const, "veFuelBins");
    LT_CHECK_STR(t->y_channel, "fuelLoad");
    LT_CHECK_STR(t->z_const, "veTable");
    LT_CHECK(t->is_3d);
    LT_CHECK(t->has_grid_height);
    LT_CHECK_NEAR(t->grid_height, 2.0, 0);
    LT_CHECK(t->has_orient);
    LT_CHECK_NEAR(t->orient[0], 250, 0);
    LT_CHECK_NEAR(t->orient[2], 340, 0);
    LT_CHECK_STR(t->help, "http://ve");
    LT_CHECK_STR(t->x_label, "RPM");
    LT_CHECK_STR(t->y_label, "{ bitStringValue(a, b) }");
    LT_CHECK_STR(t->up_label, "(RICHER)");
    LT_CHECK_STR(t->down_label, "(LEANER)");
    LT_CHECK_INT(t->x_size, 16);
    LT_CHECK_INT(t->y_size, 16);
    t = lt_ini_find_table(ini, "onlyZ");
    LT_CHECK(t && !t->is_3d && t->x_size == 8 && t->y_size == 12);
    LT_CHECK_INT(ini->n_tables, 2);

    c = lt_ini_find_curve(ini, "warmup_analyzer_curve");
    LT_CHECK(c != NULL);
    LT_CHECK_STR(c->title, "Warmup Enrichment");
    LT_CHECK_STR(c->col_x, "Coolant");
    LT_CHECK_STR(c->col_y, "WUE");
    LT_CHECK(c->has_xaxis && c->has_yaxis);
    LT_CHECK_NEAR(c->xaxis[1], 8000, 0);
    LT_CHECK_NEAR(c->xaxis[2], 9, 0);
    LT_CHECK_NEAR(c->yaxis[0], 50, 0);
    LT_CHECK_STR(c->x_const, "knockNoiseRpmBins");
    LT_CHECK_STR(c->x_channel, "RPMValue");
    LT_CHECK_STR(c->y_const, "wueRates");
    LT_CHECK_STR(c->y_const2, "wueRates2");
    LT_CHECK_INT(c->size, 10);
    LT_CHECK_STR(c->gauge, "cltGauge");
    LT_CHECK_STR(c->help, "http://w");
    LT_CHECK_INT(c->n_line_labels, 2);
    LT_CHECK_STR(c->line_labels[0], "Current WUE");
    LT_CHECK_STR(c->line_labels[1], "Recommended WUE");
    lt_ini_free(ini);
}

LT_TEST(dynamic_table_sizes)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "veTableRows = scalar, U08, 0, \"\", 1.0, 0, 8, 32, 0\n",
        "veTableCols = scalar, U08, 1, \"\", 1.0, 0, 8, 32, 0\n",
        "veTable = array, U16, 2, [{veTableCols}x{veTableRows}], \"%\", 0.01, 0, 0, 650, 2\n",
        "veLoadBins = array, U16, 514, [{veTableRows}], \"kPa\", 1, 0, 0, 255, 0\n",
        "veRpmBins = array, U16, 546, [{veTableCols}], \"rpm\", 1, 0, 0, 255, 0\n",
        "[ConstantsExtensions]\ndefaultValue = veTableRows, 16\ndefaultValue = veTableCols, 16\n",
        "maximumElements = veTable, 256\nrequiresPowerCycle = veTableRows\n",
        "maintainConstantValue = veTableRows, { veTableCols * 2 }\n",
        "[TableEditor]\ntable = veTbl, veMap, \"VE\", 1\nxBins = veRpmBins\nyBins = veLoadBins\nzBins = veTable\n", (const char *)NULL);
    const lt_ini_const *c = lt_ini_find_const(ini, "veTable");
    const lt_ini_table *t = lt_ini_find_table(ini, "veTbl");
    LT_CHECK(c && c->shape.kind == LT_SH_2D);
    LT_CHECK_INT(lt_shape_count(&c->shape), 256);
    LT_CHECK(c->dyn != NULL);
    LT_CHECK_INT(c->size_bytes, 512);
    LT_CHECK_INT(lt_shape_count(&lt_ini_find_const(ini, "veLoadBins")->shape), 32);
    LT_CHECK_INT(lt_shape_count(&lt_ini_find_const(ini, "veRpmBins")->shape), 32);
    LT_CHECK_INT(t->x_size, 16);
    LT_CHECK_INT(t->y_size, 16);
    LT_CHECK_STR(t->cols_size_const, "veTableCols");
    LT_CHECK_STR(t->rows_size_const, "veTableRows");
    LT_CHECK_INT(t->max_elements, 256);
    LT_CHECK_INT(ini->n_requires_power_cycle, 1);
    LT_CHECK_INT(ini->n_maintain_constants, 1);
    LT_CHECK_STR(ini->maintain_constants[0], "veTableRows");
    LT_CHECK_STR(ini->maintain_exprs[0], "veTableCols * 2");
    lt_ini_free(ini);
}

LT_TEST(table_roles)
{
    lt_ini *ini = load_n(
        "[TableEditor]\n",
        "table = veTable1Tbl, veTable1Map, \"VE 1\"\n",
        "table = veTable2Tbl, veTable2Map, \"VE 2\"\n",
        "table = afrTable1Tbl, afrTable1Map, \"AFR 1\"\n",
        "table = afrTable2Tbl, afrTable2Map, \"AFR 2\"\n",
        "table = sparkTbl, sparkMap, \"Spark\"\n",
        "table = veTableMafTbl, veTableMafMap, \"MAF\"\n",
        "table = boostTbl, boostMap, \"Boost\"\n",
        "[VeAnalyze]\nveAnalyzeMap = veTable1Tbl, afrTable1Tbl, lambda, egoCorrection\n",
        "filter = std_xAxisMin\n",
        "filter = minCltFilter, \"Minimum CLT\", coolant, <, 71, , true\n",
        "filter = minRPMFilter, \"Minimum RPM\", RPMValue, <, 500, true\n",
        "filter = accelFilter, \"Accel Flag\", engine, &, 16, , false\n",
        "filter = oneFilter, \"One\", ch, =, 1, 1\n",
        "filter = yesFilter, \"Yes\", ch, !=, 1, yes\n",
        "filter = commentFilter, \"C\", ch, |, 1, true ; adjustable\n",
        "filter = junk, two\n",
        "[WueAnalyze]\nwueAnalyzeMap = wueCurve, afrComp, afrTable2Tbl, lam, clt, wueCh, egoCh\n",
        "wuePercentOffset = 5.5\n", (const char *)NULL);
    LT_CHECK(lt_ini_find_table(ini, "veTable1Tbl")->role == LT_ROLE_VE);
    LT_CHECK(lt_ini_find_table(ini, "veTable2Tbl")->role == LT_ROLE_VE);    /* digit sibling */
    LT_CHECK(lt_ini_find_table(ini, "afrTable1Tbl")->role == LT_ROLE_AFR_TARGET);
    LT_CHECK(lt_ini_find_table(ini, "afrTable2Tbl")->role == LT_ROLE_AFR_TARGET);
    LT_CHECK(lt_ini_find_table(ini, "sparkTbl")->role == LT_ROLE_IGNITION);
    LT_CHECK(lt_ini_find_table(ini, "veTableMafTbl")->role == LT_ROLE_OTHER);
    LT_CHECK(lt_ini_find_table(ini, "boostTbl")->role == LT_ROLE_OTHER);
    LT_CHECK(ini->ve_analyze.present);
    LT_CHECK_STR(ini->ve_analyze.ve_table, "veTable1Tbl");
    LT_CHECK_STR(ini->ve_analyze.target_table, "afrTable1Tbl");
    LT_CHECK_STR(ini->ve_analyze.lambda_channel, "lambda");
    LT_CHECK_STR(ini->ve_analyze.ego_channel, "egoCorrection");
    LT_CHECK_INT(ini->ve_analyze.n_filters, 7);
    LT_CHECK_STR(ini->ve_analyze.filters[0].name, "std_xAxisMin");
    LT_CHECK(ini->ve_analyze.filters[0].op == LT_FOP_EQ && !ini->ve_analyze.filters[0].adjustable);
    LT_CHECK_STR(ini->ve_analyze.filters[1].display, "Minimum CLT");
    LT_CHECK(ini->ve_analyze.filters[1].op == LT_FOP_LT && ini->ve_analyze.filters[1].adjustable);
    LT_CHECK_NEAR(ini->ve_analyze.filters[1].dflt, 71, 0);
    LT_CHECK(ini->ve_analyze.filters[2].adjustable);
    LT_CHECK(ini->ve_analyze.filters[3].op == LT_FOP_BITAND && !ini->ve_analyze.filters[3].adjustable);
    LT_CHECK(ini->ve_analyze.filters[4].adjustable);
    LT_CHECK(ini->ve_analyze.filters[5].op == LT_FOP_NE && ini->ve_analyze.filters[5].adjustable);
    LT_CHECK(ini->ve_analyze.filters[6].op == LT_FOP_BITOR && ini->ve_analyze.filters[6].adjustable);
    LT_CHECK(ini->wue_analyze.present);
    LT_CHECK_STR(ini->wue_analyze.wue_curve, "wueCurve");
    LT_CHECK_STR(ini->wue_analyze.afr_comp_curve, "afrComp");
    LT_CHECK_STR(ini->wue_analyze.target_table, "afrTable2Tbl");
    LT_CHECK_STR(ini->wue_analyze.coolant_channel, "clt");
    LT_CHECK_STR(ini->wue_analyze.wue_channel, "wueCh");
    LT_CHECK_NEAR(ini->wue_analyze.wue_percent_offset, 5.5, 0);
    LT_CHECK(!ini->gamma_e.present);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* misc sections                                                     */
/* ---------------------------------------------------------------- */

LT_TEST(setting_groups)
{
    lt_ini *ini = load_n(
        "[SettingGroups]\n",
        "settingGroup = g1, \"Group One\"\nsettingOption = A, \"Option A\"\nsettingOption = B, \"Option B\"\n",
        ";  settingGroup = commented, \"X\"\n",
        "settingGroup = g2, \"Group Two\"\nsettingOption = C, \"Option C\"\n", (const char *)NULL);
    LT_CHECK_INT(ini->n_setting_groups, 2);
    LT_CHECK_STR(ini->setting_groups[0]->label, "Group One");
    LT_CHECK_INT(ini->setting_groups[0]->n_options, 2);
    LT_CHECK_STR(ini->setting_groups[0]->options[1].label, "Option B");
    LT_CHECK_STR(ini->setting_groups[0]->options[1].value, "B");
    LT_CHECK_INT(ini->setting_groups[1]->n_options, 1);
    lt_ini_free(ini);
}

LT_TEST(controller_commands_and_misc)
{
    lt_ini *ini = load_n(
        "[ControllerCommands]\n",
        "cmd_a = \"Z\\x00\\x12\\x00\\x01\"\n",
        "cmd_chain = \"X\", cmd_a, \"Y\", { canEnabled }\n",
        "cmd_dfu = \"d\"\n",
        "[PortEditor]\nportA = \"Port A\", { en1 }\ntopicHelp = \"x\"\n",
        "[KeyActions]\nshowPanel = Ctrl+G, gaugePanel\nCtrl+S = burn, \"Burn\", { online }\n", (const char *)NULL);
    const lt_ini_cmd *c = lt_ini_find_command(ini, "cmd_a");
    LT_CHECK(c != NULL && c->n_parts == 1 && !c->parts[0].is_ref);
    LT_CHECK_STR(c->parts[0].data.p, "Z\\x00\\x12\\x00\\x01");
    c = lt_ini_find_command(ini, "cmd_chain");
    LT_CHECK_INT(c->n_parts, 3);
    LT_CHECK(!c->parts[0].is_ref && c->parts[1].is_ref && !c->parts[2].is_ref);
    LT_CHECK_STR(c->parts[1].data.p, "cmd_a");
    LT_CHECK_STR(c->enable_cond, "canEnabled");
    LT_CHECK_INT(ini->n_port_editors, 1);
    LT_CHECK_STR(ini->port_editors[0].label, "Port A");
    LT_CHECK_STR(ini->port_editors[0].enable_cond, "en1");
    LT_CHECK_INT(ini->n_key_actions, 2);
    LT_CHECK_STR(ini->key_actions[0].key, "Ctrl+G");
    LT_CHECK_STR(ini->key_actions[0].action, "showPanel:gaugePanel");
    LT_CHECK_STR(ini->key_actions[0].label, "Show gaugePanel");
    LT_CHECK_STR(ini->key_actions[1].action, "burn");
    LT_CHECK_STR(ini->key_actions[1].cond, "online");
    LT_CHECK(!lt_ini_supports_console(ini));   /* unknown ecu type */
    lt_ini_free(ini);
}

LT_TEST(ecu_detect)
{
    LT_CHECK(lt_ecu_detect("epicEFI dev.2026.09.12.alphax-8chan_f7.134812558", NULL) == LT_ECU_EPICEFI);
    LT_CHECK(lt_ecu_detect("rusEFI 2025", "rusEFI2025.epicECU.ini") == LT_ECU_EPICEFI);
    LT_CHECK(lt_ecu_detect("rusEFI 2025", NULL) == LT_ECU_RUSEFI);
    LT_CHECK(lt_ecu_detect("rusEFI FOME 2025", NULL) == LT_ECU_FOME);
    LT_CHECK(lt_ecu_detect("rusEFI", "fome.ini") == LT_ECU_FOME);
    LT_CHECK(lt_ecu_detect("speeduino 202501-NA6", NULL) == LT_ECU_SPEEDUINO);
    LT_CHECK(lt_ecu_detect("MS3 Format 0123", NULL) == LT_ECU_MS3);
    LT_CHECK(lt_ecu_detect("MS2Extra comms 321", NULL) == LT_ECU_MS2);
    LT_CHECK(lt_ecu_detect("MegaSquirt II", NULL) == LT_ECU_UNKNOWN);
    LT_CHECK(lt_ecu_detect("", NULL) == LT_ECU_UNKNOWN);
    LT_CHECK(lt_ecu_supports_console(LT_ECU_RUSEFI));
    LT_CHECK(lt_ecu_supports_console(LT_ECU_FOME));
    LT_CHECK(lt_ecu_supports_console(LT_ECU_EPICEFI));
    LT_CHECK(!lt_ecu_supports_console(LT_ECU_SPEEDUINO));
    LT_CHECK_STR(lt_ecu_type_name(LT_ECU_EPICEFI), "epicEFI");
    LT_CHECK_STR(lt_ecu_type_name(LT_ECU_MS3), "MegaSquirt 3");
    LT_CHECK_STR(lt_ecu_type_name(LT_ECU_UNKNOWN), "Unknown");
}

LT_TEST(reference_tables)
{
    lt_ini *ini = load_n(
        "[ReferenceTables]\n",
        "tableBlockingFactor = 64\n",
        "tableWriteCommand = \"w\\$tsCanId%2i%2o%2c%v\"\n",
        "tableLimits = 0, -40, 350, 180\n",
        "referenceTable = std_ms2gentherm, \"Calibrate Thermistor Tables.\"\n",
        "topicHelp = \"http://wiki\"\n",
        "tableIdentifier = 000, \"Coolant Temperature Sensor\", 001, \"Air Temperature Sensor\"\n",
        "adcCount = 32\nbytesPerAdc = 2\nscale = 10\nsolutionsLabel = \"Sensor\"\n",
        "tableLimits = 0, -40, 350, 180\ntableLimits = 1, -40, 350, 70\n",
        "tableGenerator = linearGenerator, \"Custom Linear WB\", \"Volts\", \"AFR\", 1, 4, 9.7, 18.7\n",
        "tableGenerator = fileBrowseGenerator, \"Browse\"\n",
        "thermOption = \"Mazda\", 50000, -40, 2022088, 20, 5000, 100, 100\n",
        "thermOption = \"bad\", 1, 2\n",
        "solution = \"Linear\", { (a + b) / 2, 3 }\n",
        "solution = \"Empty\", { }\n",
        "solution = \"Gen\", linearGenerator\n",
        "referenceTable = afr2, \"AFR\"\nadcCount = 1024\ntableIdentifier = 002, \"Wideband\"\n", (const char *)NULL);
    const lt_ini_ref_table *t;
    LT_CHECK_INT(ini->proto.table_blocking_factor, 64);
    LT_CHECK_STR(ini->proto.table_write_command, "w\\$tsCanId%2i%2o%2c%v");
    LT_CHECK_INT(ini->n_ref_tables, 2);
    t = lt_ini_find_ref_table(ini, "std_ms2gentherm");
    LT_CHECK(t != NULL);
    LT_CHECK_STR(t->label, "Calibrate Thermistor Tables.");
    LT_CHECK_STR(t->help_topic, "http://wiki");
    LT_CHECK_INT(t->adc_count, 32);
    LT_CHECK_INT(t->bytes_per_adc, 2);
    LT_CHECK_NEAR(t->scale, 10, 0);
    LT_CHECK_INT(lt_ini_ref_wire_bytes(t), 64);
    LT_CHECK_INT(t->n_idents, 2);
    LT_CHECK_INT(t->idents[0].id, 0);
    LT_CHECK_STR(t->idents[0].label, "Coolant Temperature Sensor");
    LT_CHECK(t->idents[0].has_limits);
    LT_CHECK_NEAR(t->idents[0].lim_min, -40, 0);
    LT_CHECK_NEAR(t->idents[0].lim_max, 350, 0);
    LT_CHECK_NEAR(t->idents[0].lim_default, 180, 0);
    LT_CHECK_NEAR(t->idents[1].lim_default, 70, 0);
    LT_CHECK_INT(t->n_generators, 2);
    LT_CHECK_STR(t->generators[0].x_units, "Volts");
    LT_CHECK(t->generators[0].has_bounds);
    LT_CHECK_NEAR(t->generators[0].x_lo, 1, 0);
    LT_CHECK_NEAR(t->generators[0].y_hi, 18.7, 0);
    LT_CHECK(!t->generators[1].has_bounds);
    LT_CHECK_INT(t->n_therm_opts, 1);
    LT_CHECK_STR(t->therm_opts[0].name, "Mazda");
    LT_CHECK_NEAR(t->therm_opts[0].bias_resistor, 50000, 0);
    LT_CHECK_NEAR(t->therm_opts[0].t[0], -40, 0);
    LT_CHECK_NEAR(t->therm_opts[0].r[0], 2022088, 0);
    LT_CHECK_NEAR(t->therm_opts[0].r[2], 100, 0);
    LT_CHECK_INT(t->n_solutions, 2);
    LT_CHECK_STR(t->solutions[0].expression, "(a + b) / 2, 3");
    LT_CHECK_STR(t->solutions[1].generator, "linearGenerator");
    t = lt_ini_find_ref_table(ini, "afr2");
    LT_CHECK(t && t->n_idents == 1 && t->idents[0].id == 2 && !t->idents[0].has_limits);
    LT_CHECK_INT(t->bytes_per_adc, 1);
    LT_CHECK_NEAR(t->scale, 1.0, 0);
    LT_CHECK(lt_ini_find_ref_table(ini, "nope") == NULL);
    lt_ini_free(ini);
    /* sub-keys before any referenceTable do not create blocks */
    ini = load_n("[ReferenceTables]\nadcCount = 5\nsolution = \"x\", { 1 }\n", (const char *)NULL);
    LT_CHECK_INT(ini->n_ref_tables, 0);
    lt_ini_free(ini);
}

LT_TEST(settings_misc)
{
    lt_ini *ini = load_n("garbage with no sections\n=\n[Unknown]\nfoo = bar\n[TunerStudio]\nsignature = \"x\"\n", (const char *)NULL);
    LT_CHECK_STR(ini->signature, "x");
    lt_ini_free(ini);
    ini = load_n("", (const char *)NULL);
    LT_CHECK(ini != NULL && ini->n_constants == 0);
    lt_ini_free(ini);
    ini = load_n("[Constants]\nline1 = scalar, U08, 0, \"a\", \\\n   1, 0, 0, 100, 0\n", (const char *)NULL);
    LT_CHECK(lt_ini_find_const(ini, "line1") != NULL);
    LT_CHECK_NEAR(lt_ini_find_const(ini, "line1")->max, 100, 0);
    lt_ini_free(ini);
    /* duplicate constants: later definition wins, order stays */
    ini = load_n("[Constants]\nd = scalar, U08, 0, \"a\", 1, 0, 0, 10, 0\nz = scalar, U08, 1\nd = scalar, U08, 5, \"b\", 1, 0, 0, 20, 0\n", (const char *)NULL);
    LT_CHECK_INT(ini->n_constants, 2);
    LT_CHECK_STR(lt_ini_find_const(ini, "d")->units, "b");
    LT_CHECK_INT(lt_ini_find_const(ini, "d")->offset, 5);
    LT_CHECK_STR(ini->constants[0]->name, "d");
    lt_ini_free(ini);
}

LT_TEST(dynamic_scale_resolution)
{
    lt_ini *ini = load_n(
        "[Constants]\npage = 1\n",
        "algorithm = bits, U08, 0, [0:1], \"A\", \"B\", \"C\"\n",
        "fuelLoadBins = array, U08, 1, [16], \"kPa\", {fuelLoadRes}, 0.0, 0.0, 511.0, 0\n",
        "plain = scalar, U08, 20, \"\", 1.0, 0, 0, 100, 0\n",
        "rng = scalar, U08, 21, \"\", 1.0, 0, 0, {algorithm * 10}, 0\n",
        "[OutputChannels]\n",
        "fuelLoadRes = { ((algorithm == 0) || (algorithm == 2)) ? 2.000 : 0.500 }\n", (const char *)NULL);
    lt_map vars;
    double algo = 0.0;
    lt_expr_env env;
    lt_ini_const *c;
    lt_map_init(&vars, LT_FALSE);
    lt_map_put(&vars, "algorithm", &algo);
    lt_expr_env_from_map(&env, &vars);
    LT_CHECK(lt_ini_feeds_dynamic_scale(ini, "algorithm"));
    LT_CHECK(lt_ini_feeds_dynamic_scale(ini, "fuelLoadRes"));
    LT_CHECK(!lt_ini_feeds_dynamic_scale(ini, "plain"));
    LT_CHECK(!lt_ini_feeds_dynamic_scale(ini, "rng"));
    c = (lt_ini_const *)lt_ini_find_const(ini, "fuelLoadBins");
    LT_CHECK_STR(c->scale_expr, "fuelLoadRes");
    LT_CHECK_NEAR(c->scale, 1.0, 0);
    LT_CHECK_INT(lt_ini_resolve_dynamic(ini, &env), 2);   /* scale + max of "rng" */
    LT_CHECK_NEAR(c->scale, 2.0, 0);
    c = (lt_ini_const *)lt_ini_find_const(ini, "rng");
    LT_CHECK(c->range_resolved);
    LT_CHECK_NEAR(c->max, 0, 0);
    algo = 1.0;
    LT_CHECK_INT(lt_ini_resolve_dynamic(ini, &env), 2);
    LT_CHECK_NEAR(lt_ini_find_const(ini, "fuelLoadBins")->scale, 0.5, 0);
    LT_CHECK_NEAR(c->max, 10, 0);
    LT_CHECK_INT(lt_ini_resolve_dynamic(ini, &env), 0);   /* nothing changed */
    lt_map_free(&vars);
    lt_ini_free(ini);
    /* literal / empty braces are not deferred */
    ini = load_n("[Constants]\nl = scalar, U08, 0, \"\",  0.5 , 0, 0, 1, 0\ne = scalar, U08, 1, \"\", {}, 0, 0, 1, 0\n", (const char *)NULL);
    LT_CHECK(lt_ini_find_const(ini, "l")->scale_expr == NULL);
    LT_CHECK_NEAR(lt_ini_find_const(ini, "l")->scale, 0.5, 0);
    LT_CHECK(lt_ini_find_const(ini, "e")->scale_expr == NULL);
    lt_ini_free(ini);
}

LT_TEST(field_missing_comma)
{
    lt_ini *ini = load_n("[UserDefined]\ndialog = dm, \"M\"\n",
                         "field = \"Plain\"\t\tfa\n",
                         "field = \"Cond\" fb, { fa }\n",
                         "field = \"Ok\", fc\n", (const char *)NULL);
    const lt_ini_dialog *d = lt_ini_find_dialog(ini, "dm");
    LT_CHECK(d != NULL && d->n_comps == 3);
    if (d != NULL && d->n_comps == 3) {
        LT_CHECK(d->comps[0].kind == LT_COMP_FIELD);
        LT_CHECK_STR(d->comps[0].label, "Plain");
        LT_CHECK_STR(d->comps[0].name, "fa");
        LT_CHECK_STR(d->comps[1].label, "Cond");
        LT_CHECK_STR(d->comps[1].name, "fb");
        LT_CHECK_STR(d->comps[2].name, "fc");
    }
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(field_missing_comma);
    LT_RUN(encoding);
    LT_RUN(encoding_files);
    LT_RUN(strip_comment);
    LT_RUN(split_line);
    LT_RUN(dtype_names);
    LT_RUN(shapes);
    LT_RUN(bit_ranges);
    LT_RUN(escapes_and_substitution);
    LT_RUN(preprocessor_basic);
    LT_RUN(preprocessor_set_unset);
    LT_RUN(preprocessor_seed_units);
    LT_RUN(conditional_headers);
    LT_RUN(define_lists);
    LT_RUN(includes);
    LT_RUN(protocol_keys);
    LT_RUN(tscanid_substitution);
    LT_RUN(constant_lines);
    LT_RUN(last_offset_overlays);
    LT_RUN(constant_bits_and_help);
    LT_RUN(raw_display_conversion);
    LT_RUN(pc_variables);
    LT_RUN(output_channel_lines);
    LT_RUN(gauges);
    LT_RUN(datalog_section);
    LT_RUN(frontpage);
    LT_RUN(colors);
    LT_RUN(menus);
    LT_RUN(menu_after_group_goes_to_menu);
    LT_RUN(dialogs);
    LT_RUN(help_and_readouts);
    LT_RUN(std_panels);
    LT_RUN(tables_and_curves);
    LT_RUN(dynamic_table_sizes);
    LT_RUN(table_roles);
    LT_RUN(setting_groups);
    LT_RUN(controller_commands_and_misc);
    LT_RUN(ecu_detect);
    LT_RUN(reference_tables);
    LT_RUN(settings_misc);
    LT_RUN(dynamic_scale_resolution);
    LT_TEST_MAIN_END
}
