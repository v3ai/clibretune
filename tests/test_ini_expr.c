/* test_ini_expr.c - expression engine tests (spec 02 section 1, checklist 12). */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "ini/expr.h"
#include "lt_test.h"

static lt_map vars;
static double vals[16];
static size_t nvals;

static void setvar(const char *name, double v)
{
    double *p = (double *)lt_map_get(&vars, name);
    if (!p) {
        p = &vals[nvals++];
        lt_map_put(&vars, name, p);
    }
    *p = v;
}

static void reset_vars(void)
{
    lt_map_free(&vars);
    lt_map_init(&vars, LT_FALSE);
    nvals = 0;
}

/* evaluate; returns -1 for compile or eval error */
static int run(const char *src, const lt_expr_env *env, lt_value *out)
{
    char err[128];
    lt_expr *e = lt_expr_compile(src, err, sizeof err);
    int rc;
    lt_value_init(out);
    if (!e) return -1;
    rc = lt_expr_eval(e, env, out);
    lt_expr_free(e);
    return rc;
}

static double num(const char *src)
{
    lt_expr_env env;
    lt_value v;
    double d;
    lt_expr_env_from_map(&env, &vars);
    if (run(src, &env, &v) != 0) return -12345.678;
    d = lt_value_f64(&v);
    lt_value_clear(&v);
    return d;
}

static int truth(const char *src)
{
    lt_expr_env env;
    lt_value v;
    int b;
    lt_expr_env_from_map(&env, &vars);
    if (run(src, &env, &v) != 0) return -1;
    b = lt_value_bool(&v) ? 1 : 0;
    lt_value_clear(&v);
    return b;
}

static int compiles(const char *src)
{
    char err[128];
    lt_expr *e = lt_expr_compile(src, err, sizeof err);
    if (e) lt_expr_free(e);
    return e != NULL;
}

static char *str_of(const char *src, const lt_expr_env *env)
{
    lt_value v;
    char *s;
    if (run(src, env, &v) != 0) return lt_strdup("<err>");
    s = lt_value_to_string(&v);
    lt_value_clear(&v);
    return s;
}

LT_TEST(arith_and_precedence)
{
    reset_vars();
    LT_CHECK_NEAR(num("1 + 2 * 3"), 7, 1e-12);
    LT_CHECK_NEAR(num("(1 + 2) * 3"), 9, 1e-12);
    LT_CHECK_NEAR(num("10 - 4 - 3"), 3, 1e-12);
    LT_CHECK_NEAR(num("2 * 3 % 4"), 2, 1e-12);
    LT_CHECK_NEAR(num("-(-3)"), 3, 1e-12);
    LT_CHECK_NEAR(num("5 / 0"), 0, 1e-12);
    LT_CHECK_NEAR(num("5 % 0"), 0, 1e-12);
    LT_CHECK_NEAR(num("-7 % 3"), -1, 1e-12);
    LT_CHECK_NEAR(num("~0"), -1, 1e-12);
    LT_CHECK_NEAR(num("{ 1 + 1 } * { 3 }"), 4, 1e-12);   /* braces are whitespace: 1 + 1 * 3 */
    LT_CHECK_NEAR(num("{rpm} / 1000"), 0, 1e-12);
    LT_CHECK_NEAR(num("6 & 3"), 2, 1e-12);
    LT_CHECK_NEAR(num("6 | 3"), 7, 1e-12);
    LT_CHECK_NEAR(num("6 ^ 3"), 5, 1e-12);
    /* & binds lower than == (C rules): 4 & (4 == 4) = 4 & 1 = 0 */
    LT_CHECK_NEAR(num("4 & 4 == 4"), 0, 1e-12);
    LT_CHECK(truth("(5 & 4) == 4") == 1);
    LT_CHECK(truth("(3 & 4) == 4") == 0);
}

LT_TEST(logic_and_compare)
{
    reset_vars();
    LT_CHECK(truth("true && false || 1 == 1") == 1);
    LT_CHECK(truth("true && false") == 0);
    LT_CHECK(truth("!0") == 1);
    LT_CHECK(truth("!3") == 0);
    LT_CHECK(truth("1 < 2") == 1);
    LT_CHECK(truth("2 <= 2") == 1);
    LT_CHECK(truth("3 >= 4") == 0);
    LT_CHECK(truth("3 != 4") == 1);
    setvar("rpm", 1500);
    LT_CHECK(truth("rpm > 1000") == 1);
    setvar("rpm", 500);
    LT_CHECK(truth("rpm > 1000") == 0);
    setvar("flags", 5);
    LT_CHECK(truth("(flags & 4) == 4") == 1);
    setvar("flags", 3);
    LT_CHECK(truth("(flags & 4) == 4") == 0);
    LT_CHECK(truth("\"a\" == \"a\"") == 1);
    LT_CHECK(truth("\"a\" == \"b\"") == 0);
    LT_CHECK(truth("\"a\" != \"b\"") == 1);
}

LT_TEST(ternary)
{
    reset_vars();
    setvar("x", 1);
    LT_CHECK_NEAR(num("x ? 10 : 20"), 10, 1e-12);
    setvar("x", 0);
    LT_CHECK_NEAR(num("x ? 10 : 20"), 20, 1e-12);
    LT_CHECK_NEAR(num("0 ? 1 : 0 ? 2 : 3"), 3, 1e-12);
    LT_CHECK_NEAR(num("1 ? 0 ? 5 : 6 : 7"), 6, 1e-12);
    /* only the chosen arm is evaluated: unknown function in the other arm is harmless */
    LT_CHECK_NEAR(num("1 ? 4 : nosuch(1)"), 4, 1e-12);
    LT_CHECK(num("0 ? 4 : nosuch(1)") == -12345.678);
}

LT_TEST(functions)
{
    reset_vars();
    LT_CHECK_NEAR(num("abs(-3)"), 3, 1e-12);
    LT_CHECK_NEAR(num("round(2.7)"), 3, 1e-12);
    LT_CHECK_NEAR(num("round(-2.5)"), -3, 1e-12);
    LT_CHECK_NEAR(num("floor(2.7)"), 2, 1e-12);
    LT_CHECK_NEAR(num("ceil(2.1)"), 3, 1e-12);
    LT_CHECK_NEAR(num("sqrt(16)"), 4, 1e-12);
    LT_CHECK_NEAR(num("min(3,5,1)"), 1, 1e-12);
    LT_CHECK_NEAR(num("max(3,5,1)"), 5, 1e-12);
    LT_CHECK_NEAR(num("pow(2,10)"), 1024, 1e-9);
    LT_CHECK_NEAR(num("recip(4)"), 0.25, 1e-12);
    LT_CHECK_NEAR(num("recip(0)"), 0, 1e-12);
    LT_CHECK_NEAR(num("if(1,10,20)"), 10, 1e-12);
    LT_CHECK_NEAR(num("if(0,10,20)"), 20, 1e-12);
    LT_CHECK_NEAR(num("int(3.9)"), 3, 1e-12);
    LT_CHECK_NEAR(num("trunc(-2.8)"), -2, 1e-12);
    LT_CHECK(truth("not(0)") == 1);
    LT_CHECK(truth("boolean(1)") == 1);
    LT_CHECK(truth("bool(0)") == 0);
    LT_CHECK(truth("isNaN(sqrt(0-1))") == 1);
    LT_CHECK(truth("isNaN(1)") == 0);
    LT_CHECK(truth("isAdvancedMathAvailable()") == 1);
    LT_CHECK_NEAR(num("getAuxDigital(0)"), 0, 1e-12);
    LT_CHECK_NEAR(num("log10(1000)"), 3, 1e-12);
    LT_CHECK_NEAR(num("log10(0)"), 0, 1e-12);
    LT_CHECK_NEAR(num("log(1)"), 0, 1e-12);
    LT_CHECK_NEAR(num("exp(0)"), 1, 1e-12);
    LT_CHECK_NEAR(num("sin(0)"), 0, 1e-12);
    LT_CHECK_NEAR(num("cos(0)"), 1, 1e-12);
    LT_CHECK_NEAR(num("atan2(1,1)"), LT_PI / 4, 1e-12);
    LT_CHECK_NEAR(num("ABS(-3)"), 3, 1e-12);  /* names are case-insensitive */
    setvar("time", 1.5);
    LT_CHECK_NEAR(num("pastValue(time,1)"), 1.5, 1e-12);
    setvar("time", 42);
    LT_CHECK_NEAR(num("time - pastValue(time, 1)"), 0, 1e-12);
}

LT_TEST(arity_and_unknown)
{
    reset_vars();
    LT_CHECK(num("abs()") == -12345.678);
    LT_CHECK(num("abs(1,2)") == -12345.678);
    LT_CHECK(num("pow(1)") == -12345.678);
    LT_CHECK(num("min(1)") == -12345.678);
    LT_CHECK(num("unknownFn(1)") == -12345.678);
    LT_CHECK(num("$getUnknown()") == -12345.678);
    LT_CHECK(num("bitStringValue(1, 2)") == -12345.678);   /* first arg must be a name */
}

LT_TEST(shifts_and_bitops)
{
    reset_vars();
    LT_CHECK_NEAR(num("1 << 64"), 0, 1e-12);
    LT_CHECK_NEAR(num("1 << 200"), 0, 1e-12);
    LT_CHECK_NEAR(num("1 << (0 - 1)"), 0, 1e-12);
    LT_CHECK_NEAR(num("1 >> 64"), 0, 1e-12);
    LT_CHECK_NEAR(num("1 >> (0 - 3)"), 0, 1e-12);
    LT_CHECK_NEAR(num("1 << 3"), 8, 1e-12);
    LT_CHECK_NEAR(num("1 << 40"), 1099511627776.0, 1e-3);
    LT_CHECK_NEAR(num("1 << 62"), 4611686018427387904.0, 1.0);
    LT_CHECK_NEAR(num("256 >> 4"), 16, 1e-12);
    LT_CHECK_NEAR(num("(0 - 16) >> 2"), -4, 1e-12);   /* arithmetic shift */
    LT_CHECK_NEAR(num("4294967296 >> 32"), 1, 1e-12);
    LT_CHECK_NEAR(num("(0 - 1) >> 40"), -1, 1e-12);
    LT_CHECK_NEAR(num("4294967295 & 65535"), 65535, 1e-12);
    LT_CHECK_NEAR(num("4294967296 | 1"), 4294967297.0, 1e-3);
    LT_CHECK_NEAR(num("~5"), -6, 1e-12);
}

LT_TEST(parse_errors)
{
    reset_vars();
    LT_CHECK(!compiles("1 2"));
    LT_CHECK(!compiles("1 + 2 3"));
    LT_CHECK(!compiles("1 @ 2"));
    LT_CHECK(!compiles("a = 1"));
    LT_CHECK(!compiles(""));
    LT_CHECK(!compiles("(1 + 2"));
    LT_CHECK(!compiles("1 ? 2"));
    LT_CHECK(!compiles("f(1 2)"));
    LT_CHECK(!compiles("1.2.3"));
    LT_CHECK(!compiles("a.b"));
    LT_CHECK(!compiles("1exp"));
    LT_CHECK(compiles("f(1, 2)"));
    LT_CHECK(compiles("f()"));
    LT_CHECK(compiles("a ? b : c"));
}

LT_TEST(nesting_limits)
{
    char buf[12000];
    int i, d;
    reset_vars();
    /* 16 nested parens ok; 5000 deep is an error rather than a stack overflow */
    buf[0] = '\0';
    for (i = 0; i < 16; i++) strcat(buf, "(");
    strcat(buf, "1");
    for (i = 0; i < 16; i++) strcat(buf, ")");
    LT_CHECK(compiles(buf));
    LT_CHECK_NEAR(num(buf), 1, 1e-12);
    d = 5000;
    for (i = 0; i < d; i++) buf[i] = '(';
    buf[d] = '1';
    for (i = 0; i < d; i++) buf[d + 1 + i] = ')';
    buf[2 * d + 1] = '\0';
    LT_CHECK(!compiles(buf));
    for (i = 0; i < 3000; i++) buf[i] = '-';
    buf[3000] = '1';
    buf[3001] = '\0';
    LT_CHECK(!compiles(buf));
    LT_CHECK(compiles("- - - 1"));
}

LT_TEST(number_literals)
{
    reset_vars();
    LT_CHECK_NEAR(num("1e3"), 1000, 1e-9);
    LT_CHECK_NEAR(num("1E3"), 1000, 1e-9);
    LT_CHECK_NEAR(num("1.5e-2"), 0.015, 1e-12);
    LT_CHECK_NEAR(num("2e+2"), 200, 1e-9);
    LT_CHECK_NEAR(num(".5"), 0.5, 1e-12);
    LT_CHECK_NEAR(num("10.0"), 10, 1e-12);
    LT_CHECK_NEAR(num("3."), 3, 1e-12);
}

LT_TEST(unknown_variables_and_case)
{
    reset_vars();
    LT_CHECK_NEAR(num("nothere"), 0, 1e-12);
    LT_CHECK_NEAR(num("nothere + 5"), 5, 1e-12);
    setvar("Rpm", 7);
    LT_CHECK_NEAR(num("Rpm"), 7, 1e-12);
    LT_CHECK_NEAR(num("rpm"), 0, 1e-12);   /* variable names are case-sensitive */
    LT_CHECK_NEAR(num("true"), 1, 1e-12);
    LT_CHECK_NEAR(num("false"), 0, 1e-12);
}

LT_TEST(strings)
{
    lt_expr_env env;
    char *s;
    reset_vars();
    lt_expr_env_from_map(&env, &vars);
    s = str_of("\"a\" + \"b\"", &env);
    LT_CHECK_STR(s, "ab");
    lt_free(s);
    s = str_of("\"x\" + 1", &env);   /* mixed: numeric addition, string counts 0 */
    LT_CHECK_STR(s, "1");
    lt_free(s);
    s = str_of("true", &env);
    LT_CHECK_STR(s, "true");
    lt_free(s);
    s = str_of("1.5", &env);
    LT_CHECK_STR(s, "1.5");
    lt_free(s);
    s = str_of("0.1 + 0.2", &env);
    LT_CHECK_STR(s, "0.30000000000000004");
    lt_free(s);
    s = str_of("100", &env);
    LT_CHECK_STR(s, "100");
    lt_free(s);
    s = str_of("0 - 3", &env);
    LT_CHECK_STR(s, "-3");
    lt_free(s);
    s = str_of("0.001", &env);
    LT_CHECK_STR(s, "0.001");
    lt_free(s);
}

/* ---- host callbacks ---- */
static const char *g_opts[] = {"RPM", "MAP", "TPS"};
static double g_gp_axis;

static lt_bool cb_get_var(void *ud, const char *name, double *out)
{
    LT_UNUSED(ud);
    if (strcmp(name, "gppwm1_loadAxis") == 0) {
        *out = g_gp_axis;
        return LT_TRUE;
    }
    return LT_FALSE;
}

static const char *cb_get_string(void *ud, const char *name)
{
    LT_UNUSED(ud);
    return strcmp(name, "gpPwmNote1") == 0 ? "Radiator Fan" : NULL;
}

static const char *const *cb_bit_opts(void *ud, const char *name, size_t *n)
{
    LT_UNUSED(ud);
    if (strcmp(name, "pwmAxisLabels") != 0) return NULL;
    *n = 3;
    return (const char *const *)g_opts;
}

static lt_bool cb_table(void *ud, const char *file, double x, double *out)
{
    LT_UNUSED(ud);
    if (strcmp(file, "thermistor.inc") != 0) return LT_FALSE;
    *out = x * 2.0;
    return LT_TRUE;
}

static lt_bool cb_array(void *ud, const char *name, double idx, double *out)
{
    LT_UNUSED(ud);
    if (strcmp(name, "veTable") != 0) return LT_FALSE;
    *out = 100.0 + idx;
    return LT_TRUE;
}

static lt_bool cb_online(void *ud)
{
    LT_UNUSED(ud);
    return LT_TRUE;
}

static const char *cb_dir(void *ud, int which)
{
    LT_UNUSED(ud);
    return which == 0 ? "/proj" : "/work";
}

static double cb_time(void *ud)
{
    LT_UNUSED(ud);
    return 12.5;
}

LT_TEST(host_callbacks)
{
    lt_expr_env env, none;
    char *s;
    lt_expr_env_init(&none);
    lt_expr_env_init(&env);
    env.get_var = cb_get_var;
    env.get_string = cb_get_string;
    env.get_bit_options = cb_bit_opts;
    env.table_lookup = cb_table;
    env.array_value = cb_array;
    env.is_online = cb_online;
    env.get_dir = cb_dir;
    env.time_now = cb_time;

    g_gp_axis = 1;
    s = str_of("{ bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }", &env);
    LT_CHECK_STR(s, "MAP");
    lt_free(s);
    s = str_of("bitStringValue(pwmAxisLabels, gppwm1_loadAxis)", &none);
    LT_CHECK_STR(s, "INVALID[0]");
    lt_free(s);
    g_gp_axis = 7;
    s = str_of("bitStringValue(pwmAxisLabels, gppwm1_loadAxis)", &env);
    LT_CHECK_STR(s, "INVALID[7]");
    lt_free(s);
    s = str_of("stringValue(gpPwmNote1)", &env);
    LT_CHECK_STR(s, "Radiator Fan");
    lt_free(s);
    s = str_of("stringValue(gpPwmNote1)", &none);
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = str_of("stringValue(\"gpPwmNote1\")", &env);
    LT_CHECK_STR(s, "Radiator Fan");
    lt_free(s);
    s = str_of("table(5, \"thermistor.inc\")", &env);
    LT_CHECK_STR(s, "10");
    lt_free(s);
    s = str_of("table(5, thermistor.inc)", &none);   /* '.' is a lex error */
    LT_CHECK_STR(s, "<err>");
    lt_free(s);
    s = str_of("table(5, \"thermistor.inc\")", &none);
    LT_CHECK_STR(s, "0");
    lt_free(s);
    s = str_of("arrayValue(veTable, 3)", &env);
    LT_CHECK_STR(s, "103");
    lt_free(s);
    s = str_of("arrayValue(veTable, 3)", &none);
    LT_CHECK_STR(s, "0");
    lt_free(s);
    s = str_of("isOnline()", &env);
    LT_CHECK_STR(s, "true");
    lt_free(s);
    s = str_of("isOnline()", &none);
    LT_CHECK_STR(s, "false");
    lt_free(s);
    s = str_of("timeNow()", &env);
    LT_CHECK_STR(s, "12.5");
    lt_free(s);
    s = str_of("timeNow()", &none);
    LT_CHECK_STR(s, "0");
    lt_free(s);
    s = str_of("$getProjectsDirPath()", &env);
    LT_CHECK_STR(s, "/proj");
    lt_free(s);
    s = str_of("$getWorkingDir()", &env);
    LT_CHECK_STR(s, "/work");
    lt_free(s);
    s = str_of("$getWorkingDir()", &none);
    LT_CHECK_STR(s, "");
    lt_free(s);
}

LT_TEST(display_string)
{
    lt_expr_env env, none;
    char *s;
    lt_expr_env_init(&none);
    lt_expr_env_init(&env);
    env.get_var = cb_get_var;
    env.get_string = cb_get_string;
    env.get_bit_options = cb_bit_opts;
    g_gp_axis = 1;

    s = lt_expr_display("Engine Speed", &env);
    LT_CHECK_STR(s, "Engine Speed");
    lt_free(s);
    s = lt_expr_display("{ bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }", &env);
    LT_CHECK_STR(s, "MAP");
    lt_free(s);
    s = lt_expr_display("{ stringValue(gpPwmNote1) }", &env);
    LT_CHECK_STR(s, "Radiator Fan");
    lt_free(s);
    s = lt_expr_display("{ stringValue(gpPwmNote1) }", &none);
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = lt_expr_display("{ }", &env);
    LT_CHECK_STR(s, "{ }");
    lt_free(s);
    s = lt_expr_display("{ 1 + }", &env);   /* parse error -> raw text */
    LT_CHECK_STR(s, "{ 1 + }");
    lt_free(s);
    s = lt_expr_display("  { 2 * 21 }  ", &env);
    LT_CHECK_STR(s, "42");
    lt_free(s);
    s = lt_expr_display("a {1} b", &env);   /* only whole-string braces are evaluated */
    LT_CHECK_STR(s, "a {1} b");
    lt_free(s);
}

LT_TEST(visibility_semantics)
{
    lt_expr_env env;
    lt_expr_cache cache;
    reset_vars();
    lt_expr_env_from_map(&env, &vars);
    setvar("mode", 2);
    LT_CHECK(lt_expr_test(NULL, &env));
    LT_CHECK(lt_expr_test("", &env));
    LT_CHECK(lt_expr_test("mode == 2", &env));
    LT_CHECK(!lt_expr_test("mode == 3", &env));
    LT_CHECK(lt_expr_test("1 +", &env));               /* parse error -> visible */
    LT_CHECK(lt_expr_test("nosuchfn(1)", &env));        /* eval error -> visible */
    lt_expr_cache_init(&cache);
    LT_CHECK(lt_expr_cache_test(&cache, "mode == 2", &env));
    setvar("mode", 5);
    LT_CHECK(!lt_expr_cache_test(&cache, "mode == 2", &env));
    LT_CHECK(lt_expr_cache_test(&cache, "1 +", &env));
    LT_CHECK(lt_expr_cache_get(&cache, "1 +") == NULL);
    LT_CHECK(lt_expr_cache_get(&cache, "mode") != NULL);
    LT_CHECK_NEAR(lt_expr_cache_f64(&cache, "mode * 2", &env, -1), 10, 1e-12);
    LT_CHECK_NEAR(lt_expr_cache_f64(&cache, "1 +", &env, -1), -1, 1e-12);
    LT_CHECK_NEAR(lt_expr_cache_f64(&cache, NULL, &env, 77), 77, 1e-12);
    {
        char *s = lt_expr_cache_display(&cache, "{ mode + 1 }", &env);
        LT_CHECK_STR(s, "6");
        lt_free(s);
    }
    lt_expr_cache_free(&cache);
}

LT_TEST(identifiers_and_collect)
{
    char **names;
    size_t n, i;
    int has_a = 0, has_b = 0, has_f = 0, has_num = 0;
    lt_expr *e;
    lt_map set;
    char err[64];

    n = lt_expr_identifiers("a + b * f(a) + 12abc + _x1", &names);
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], "a") == 0) has_a++;
        if (strcmp(names[i], "b") == 0) has_b++;
        if (strcmp(names[i], "f") == 0) has_f++;
        if (strcmp(names[i], "12abc") == 0 || strcmp(names[i], "abc") == 0) has_num++;
    }
    LT_CHECK(has_a == 1 && has_b == 1 && has_f == 1);   /* unique; function names included */
    LT_CHECK(has_num == 0);                              /* numeric-leading runs dropped */
    LT_CHECK_INT(n, 4);                                  /* a b f _x1 */
    lt_expr_names_free(names, n);

    e = lt_expr_compile("{ (a & 4) == 4 ? bitStringValue(lbl, idx) : max(b, c) }", err,
                        sizeof err);
    LT_CHECK(e != NULL);
    lt_map_init(&set, LT_FALSE);
    lt_expr_collect_vars(e, &set);
    LT_CHECK(lt_map_has(&set, "a"));
    LT_CHECK(lt_map_has(&set, "lbl"));
    LT_CHECK(lt_map_has(&set, "idx"));
    LT_CHECK(lt_map_has(&set, "b"));
    LT_CHECK(lt_map_has(&set, "c"));
    LT_CHECK(!lt_map_has(&set, "max"));
    LT_CHECK(!lt_map_has(&set, "bitStringValue"));
    lt_map_free(&set);
    lt_expr_free(e);
}

LT_TEST(eval_error_state)
{
    lt_expr *e;
    lt_value v;
    char err[64];
    lt_expr_env env;
    double d;
    lt_bool b;

    reset_vars();
    lt_expr_env_from_map(&env, &vars);
    e = lt_expr_compile("nosuch(1)", err, sizeof err);
    LT_CHECK(e != NULL);
    lt_value_init(&v);
    LT_CHECK(lt_expr_eval(e, &env, &v) == LT_EDATA);
    LT_CHECK(v.t == LT_V_NUM && v.n == 0.0);
    LT_CHECK(strstr(lt_err_get(), "Unknown function") != NULL);
    LT_CHECK(lt_expr_eval_f64(e, &env, &d) == LT_EDATA);
    LT_CHECK_NEAR(d, 0, 0);
    LT_CHECK(lt_expr_eval_bool(e, &env, &b) == LT_EDATA);
    lt_expr_free(e);
    e = lt_expr_compile("2 > 1", err, sizeof err);
    LT_CHECK(lt_expr_eval_bool(e, &env, &b) == 0 && b);
    lt_expr_free(e);
    LT_CHECK(lt_expr_compile("1 +", err, sizeof err) == NULL);
    LT_CHECK(err[0] != '\0');
}

LT_TEST(value_semantics)
{
    lt_value v;
    lt_value_init(&v);
    v.t = LT_V_NUM;
    v.n = 0.0 / 1.0;
    LT_CHECK(!lt_value_bool(&v));
    v.n = 2.0;
    LT_CHECK(lt_value_bool(&v));
    v.t = LT_V_BOOL;
    v.b = 1;
    LT_CHECK_NEAR(lt_value_f64(&v), 1.0, 0);
    v.t = LT_V_STR;
    v.s = lt_strdup("");
    LT_CHECK(!lt_value_bool(&v));
    LT_CHECK_NEAR(lt_value_f64(&v), 0.0, 0);
    lt_value_clear(&v);
    v.t = LT_V_STR;
    v.s = lt_strdup("x");
    LT_CHECK(lt_value_bool(&v));
    lt_value_clear(&v);
}

int main(void)
{
    lt_map_init(&vars, LT_FALSE);
    LT_RUN(arith_and_precedence);
    LT_RUN(logic_and_compare);
    LT_RUN(ternary);
    LT_RUN(functions);
    LT_RUN(arity_and_unknown);
    LT_RUN(shifts_and_bitops);
    LT_RUN(parse_errors);
    LT_RUN(nesting_limits);
    LT_RUN(number_literals);
    LT_RUN(unknown_variables_and_case);
    LT_RUN(strings);
    LT_RUN(host_callbacks);
    LT_RUN(display_string);
    LT_RUN(visibility_semantics);
    LT_RUN(identifiers_and_collect);
    LT_RUN(eval_error_state);
    LT_RUN(value_semantics);
    lt_map_free(&vars);
    LT_TEST_MAIN_END
}
