/* test_project.c - json, properties, projects, INI repository, restore points,
 * snapshots, TunerStudio import. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "plat/plat.h"
#include "project/project.h"
#include "project/proj_priv.h"
#include "lt_test.h"

extern int putenv(char *);

#define FIX "tests/fixtures/project/"
#define TMP "build/test_tmp/project_"

static char *fresh(const char *name)
{
    char *p = (char *)lt_malloc(strlen(TMP) + strlen(name) + 1);
    strcpy(p, TMP);
    strcat(p, name);
    lt_proj_rmtree(p);
    lt_plat_mkdirs(p);
    return p;
}

static char *slurp(const char *path)
{
    size_t n;
    char *d = lt_file_read(path, &n);
    return d;
}

/* ---------------------------------------------------------------- json */
LT_TEST(json_parse_basic)
{
    char err[100];
    const char *txt = " {\"a\": 1.5, \"b\": [true, false, null, \"x\\n\\u00e9\\ud83d\\ude00\"],"
                      " \"c\": {\"d\": -2e2}} ";
    lt_json *j = lt_json_parse(txt, strlen(txt), err, sizeof err);
    const lt_json *b;
    LT_CHECK(j != NULL);
    if (!j) return;
    LT_CHECK_NEAR(lt_json_get_num(j, "a", 0), 1.5, 1e-12);
    b = lt_json_get(j, "b");
    LT_CHECK(b && b->n == 4);
    LT_CHECK(lt_json_at(b, 0)->type == LT_JSON_BOOL && lt_json_at(b, 0)->boolean == 1);
    LT_CHECK(lt_json_at(b, 2)->type == LT_JSON_NULL);
    LT_CHECK_STR(lt_json_at(b, 3)->str, "x\n\xC3\xA9\xF0\x9F\x98\x80");
    LT_CHECK_NEAR(lt_json_get_num(lt_json_get(j, "c"), "d", 0), -200, 1e-12);
    LT_CHECK(lt_json_get(j, "zzz") == NULL);
    LT_CHECK_STR(lt_json_get_str(j, "zzz", "dflt"), "dflt");
    lt_json_free(j);
}

LT_TEST(json_errors)
{
    char err[100];
    const char *bad[] = {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "[1] x", "01", "\"abc",
                         "{'a':1}", "nul", "[1 2]", "\"\\q\"", "-", "1.", "\"\t\""};
    size_t i;
    for (i = 0; i < LT_COUNTOF(bad); i++) {
        lt_json *j = lt_json_parse(bad[i], strlen(bad[i]), err, sizeof err);
        LT_CHECK(j == NULL);
        LT_CHECK(err[0] != '\0');
        lt_json_free(j);
    }
    {
        char *deep = (char *)lt_malloc(2000);
        memset(deep, '[', 1000);
        deep[1000] = '\0';
        LT_CHECK(lt_json_parse(deep, 1000, err, sizeof err) == NULL);
        lt_free(deep);
    }
}

LT_TEST(json_write_layout)
{
    lt_json *o = lt_json_obj(), *a = lt_json_arr();
    char *s;
    lt_json_obj_set(o, "name", lt_json_str("q\"\\\n\x01"));
    lt_json_obj_set(o, "n", lt_json_num(3));
    lt_json_obj_set(o, "f", lt_json_num(0.1));
    lt_json_obj_set(o, "neg", lt_json_num(-0.0));
    lt_json_obj_set(o, "nul", lt_json_str(NULL));
    lt_json_obj_set(o, "e", lt_json_arr());
    lt_json_obj_set(o, "eo", lt_json_obj());
    lt_json_arr_push(a, lt_json_num(1));
    lt_json_arr_push(a, lt_json_bool(0));
    lt_json_obj_set(o, "arr", a);
    lt_json_obj_set(o, "n", lt_json_num(4)); /* replace keeps position */
    s = lt_json_to_string(o, 1);
    LT_CHECK_STR(s, "{\n  \"name\": \"q\\\"\\\\\\n\\u0001\",\n  \"n\": 4,\n  \"f\": 0.1,\n"
                    "  \"neg\": 0,\n  \"nul\": null,\n  \"e\": [],\n  \"eo\": {},\n"
                    "  \"arr\": [\n    1,\n    false\n  ]\n}");
    lt_free(s);
    s = lt_json_to_string(o, 0);
    LT_CHECK(strstr(s, "\"arr\":[1,false]") != NULL);
    lt_free(s);
    lt_json_free(o);
}

LT_TEST(json_number_roundtrip)
{
    double vals[] = {0.1, 1.0 / 3.0, 1e21, 123456789.125, -1e-7, 14.7, 2147483648.0};
    size_t i;
    for (i = 0; i < LT_COUNTOF(vals); i++) {
        lt_json *n = lt_json_num(vals[i]), *p;
        char *s = lt_json_to_string(n, 0);
        p = lt_json_parse(s, strlen(s), NULL, 0);
        LT_CHECK(p != NULL && p->num == vals[i]);
        lt_json_free(p);
        lt_free(s);
        lt_json_free(n);
    }
}

LT_TEST(json_file_io)
{
    char *d = fresh("json"), *f;
    lt_json *o = lt_json_obj(), *r;
    char err[100];
    f = lt_path_join(d, "x.json");
    lt_json_obj_set(o, "k", lt_json_str("v"));
    LT_CHECK(lt_json_save(o, f, 1) == 0);
    r = lt_json_load(f, err, sizeof err);
    LT_CHECK(r != NULL);
    LT_CHECK_STR(lt_json_get_str(r, "k", ""), "v");
    lt_json_free(r);
    LT_CHECK(lt_json_load("build/test_tmp/nonexistent.json", err, sizeof err) == NULL);
    lt_json_free(o);
    lt_free(f);
    lt_free(d);
}

/* ---------------------------------------------------------------- props */
LT_TEST(props_basic)
{
    lt_props p;
    int iv;
    double dv;
    lt_bool bv;
    lt_props_init(&p);
    lt_props_parse(&p, "key1=value1\nkey2:value2\nkey\\ with\\ spaces=value with spaces\n"
                       "multi=value1\\\nvalue2\\\nvalue3\nkey=Hello\nesc=line1\\nline2\\ttabbed\n"
                       "k3 = spaced value\n");
    LT_CHECK_STR(lt_props_get(&p, "key1"), "value1");
    LT_CHECK_STR(lt_props_get(&p, "key2"), "value2");
    LT_CHECK_STR(lt_props_get(&p, "key with spaces"), "value with spaces");
    LT_CHECK_STR(lt_props_get(&p, "multi"), "value1value2value3");
    LT_CHECK_STR(lt_props_get(&p, "key"), "Hello");
    LT_CHECK_STR(lt_props_get(&p, "esc"), "line1\nline2\ttabbed");
    LT_CHECK_STR(lt_props_get(&p, "k3"), "spaced value");
    LT_CHECK(lt_props_get(&p, "nope") == NULL);
    lt_props_free(&p);

    lt_props_init(&p);
    LT_CHECK(lt_props_load(&p, FIX "ts_proj/projectCfg/project.properties") == 0);
    LT_CHECK_STR(lt_props_get(&p, "commPort"), "COM8");
    LT_CHECK(lt_props_get_i32(&p, "baudRate", &iv) && iv == 115200);
    LT_CHECK_STR(lt_props_get(&p, "projectName"), "MS3-Example_Project");
    LT_CHECK_STR(lt_props_get(&p, "ecuConfigFile"), "mainController.ini");
    LT_CHECK(lt_props_get_bool(&p, "useCommonDashboardDir", &bv) && bv == LT_FALSE);
    LT_CHECK(lt_props_get_i32(&p, "recordsPerSec", &iv) && iv == 100);
    LT_CHECK(!lt_props_get_i32(&p, "commPort", &iv));
    LT_CHECK(p.n_header == 3);
    LT_CHECK_STR(p.header[0], "#Project Attributes.");
    lt_props_free(&p);

    lt_props_init(&p);
    lt_props_parse(&p, "weight=3200\nfirstGearRatio=3.42\nflag=Yes\nsp= 12\n");
    LT_CHECK(lt_props_get_i32(&p, "weight", &iv) && iv == 3200);
    LT_CHECK(lt_props_get_f64(&p, "firstGearRatio", &dv) && dv == 3.42);
    LT_CHECK(lt_props_get_bool(&p, "flag", &bv) && bv);
    LT_CHECK(!lt_props_get_f64(&p, "missing", &dv));
    lt_props_free(&p);
}

LT_TEST(props_edge)
{
    lt_props p;
    lt_props_init(&p);
    /* duplicate keeps first position; comments after entries dropped; CRLF; unicode */
    lt_props_parse(&p, "#hdr\r\na=1\r\n# later\r\nb=2\r\na=3\r\nu=\\u00e9\\u0041\r\n"
                       "lone\r\nesc\\=key=v\r\nlast=x\\\n");
    LT_CHECK(p.n == 6);
    LT_CHECK_STR(p.keys[0], "a");
    LT_CHECK_STR(p.vals[0], "3");
    LT_CHECK_STR(lt_props_get(&p, "u"), "\xC3\xA9" "A");
    LT_CHECK_STR(lt_props_get(&p, "lone"), "");
    LT_CHECK_STR(lt_props_get(&p, "esc=key"), "v");
    LT_CHECK_STR(lt_props_get(&p, "last"), "x");
    LT_CHECK(p.n_header == 1);
    LT_CHECK(lt_props_remove(&p, "b"));
    LT_CHECK(!lt_props_remove(&p, "b"));
    LT_CHECK(p.n == 5);
    lt_props_free(&p);
}

LT_TEST(props_roundtrip)
{
    lt_props p, q;
    char *s, *d = fresh("props"), *f;
    lt_props_init(&p);
    lt_props_set(&p, "key1", "value1");
    lt_props_set(&p, "key2", " lead\ttab\nnl \\ back");
    lt_props_set(&p, "has space=colon:", "v=1");
    lt_props_set(&p, "key1", "changed");
    LT_CHECK(p.n == 3);
    s = lt_props_to_string(&p);
    LT_CHECK_STR(s, "key1=changed\nkey2=\\ lead\\ttab\\nnl \\\\ back\nhas\\ space\\=colon\\:=v=1\n");
    lt_free(s);
    f = lt_path_join(d, "t.properties");
    LT_CHECK(lt_props_save(&p, f) == 0);
    lt_props_init(&q);
    LT_CHECK(lt_props_load(&q, f) == 0);
    LT_CHECK(q.n == 3);
    LT_CHECK_STR(lt_props_get(&q, "key2"), " lead\ttab\nnl \\ back");
    LT_CHECK_STR(lt_props_get(&q, "has space=colon:"), "v=1");
    lt_props_free(&p);
    lt_props_free(&q);
    lt_free(f);
    lt_free(d);
}

LT_TEST(props_latin1)
{
    char *d = fresh("props_l1"), *f = lt_path_join(d, "a.properties");
    lt_props p;
    lt_file_write(f, "name=caf\xE9\n", 9);
    lt_props_init(&p);
    LT_CHECK(lt_props_load(&p, f) == 0);
    LT_CHECK_STR(lt_props_get(&p, "name"), "caf\xC3\xA9");
    lt_props_free(&p);
    lt_free(f);
    lt_free(d);
}

/* ---------------------------------------------------------------- project */
LT_TEST(project_lifecycle)
{
    char *d = fresh("proj"), *p1, *x;
    lt_project p, q;
    lt_project_info *list;
    size_t n;

    LT_CHECK(lt_project_create("My Turbo/Build:1", FIX "mini.ini", "speeduino 202501-mini", d, &p) == 0);
    p1 = lt_path_join(d, "My Turbo_Build_1");
    LT_CHECK_STR(p.path, p1);
    LT_CHECK_STR(p.cfg.name, "My Turbo/Build:1");
    LT_CHECK_STR(p.cfg.version, "1.0");
    LT_CHECK_STR(p.cfg.ecu_definition, "projectCfg/definition.ini");
    LT_CHECK(p.cfg.conn.baud_rate == 115200 && p.cfg.conn.timeout_ms == 1000);
    LT_CHECK(p.cfg.conn.port == NULL && p.cfg.dashboard == NULL);
    LT_CHECK(p.cfg.settings.max_restore_points == 20);
    LT_CHECK(strlen(p.cfg.created) == 20 && p.cfg.created[19] == 'Z');
    x = lt_path_join(p1, "datalogs");
    LT_CHECK(lt_plat_is_dir(x));
    lt_free(x);
    x = lt_path_join(p1, "dashboards");
    LT_CHECK(lt_plat_is_dir(x));
    lt_free(x);
    x = lt_path_join(p1, "restorePoints");
    LT_CHECK(!lt_plat_is_dir(x));
    lt_free(x);
    x = lt_project_ini_path(&p);
    {
        char *a = slurp(x), *b = slurp(FIX "mini.ini");
        LT_CHECK(a && b && strcmp(a, b) == 0);
        lt_free(a);
        lt_free(b);
    }
    lt_free(x);
    LT_CHECK(lt_project_create("My Turbo/Build:1", FIX "mini.ini", "s", d, &q) == LT_EEXIST);
    LT_CHECK(strstr(lt_err_get(), "already exists") != NULL);
    LT_CHECK(lt_project_create("x", FIX "nonexistent.ini", "s", d, &q) == LT_ENOENT);
    LT_CHECK(lt_project_create("", FIX "mini.ini", "s", d, &q) == LT_EINVAL);
    LT_CHECK(lt_project_create("..", FIX "mini.ini", "s", d, &q) == 0);
    LT_CHECK(strstr(q.path, "__") != NULL);
    lt_project_free(&q);

    /* ini loads through the project */
    {
        lt_ini *ini = lt_project_load_ini(&p, NULL);
        LT_CHECK(ini != NULL);
        if (ini) {
            LT_CHECK(lt_ini_find_const(ini, "reqFuel") != NULL);
            lt_ini_free(ini);
        }
    }

    /* tune save / reopen */
    p.tune = lt_tune_new("speeduino 202501-mini");
    {
        lt_tune_value v;
        lt_tune_value_init(&v);
        v.scalar = 12.6;
        lt_tune_set_constant(p.tune, "reqFuel", &v, 1);
        v.scalar = 7000;
        lt_tune_set_pc_variable(p.tune, "rpmhigh", &v);
    }
    LT_CHECK(lt_project_save_tune(&p) == 0);
    x = lt_proj_join3(p.path, "projectCfg", "pcVariableValues.msq");
    LT_CHECK(lt_file_exists(x));
    lt_free(x);
    p.cfg.conn.port = lt_strdup("COM8");
    LT_CHECK(lt_project_save_config(&p) == 0);
    LT_CHECK(lt_project_close(&p) == 0);
    LT_CHECK(p.path == NULL);

    LT_CHECK(lt_project_open(p1, &q) == 0);
    LT_CHECK(q.tune != NULL);
    if (q.tune) {
        const lt_tune_entry *e = lt_tune_get_constant(q.tune, "reqFuel");
        LT_CHECK(e && e->value.scalar > 12.59 && e->value.scalar < 12.61);
        e = lt_tune_get_pc_variable(q.tune, "rpmhigh");
        LT_CHECK(e && e->value.scalar == 7000);
    }
    LT_CHECK_STR(q.cfg.conn.port, "COM8");
    lt_project_free(&q);

    /* list: newest modified first; broken dirs skipped */
    {
        char *bad = lt_path_join(d, "broken"), *bf;
        lt_plat_mkdirs(bad);
        bf = lt_path_join(bad, "project.json");
        lt_file_write(bf, "{not json", 9);
        lt_free(bf);
        lt_free(bad);
    }
    LT_CHECK(lt_project_open(p1, &q) == 0);
    lt_proj_set_str(&q.cfg.modified, "2999-01-01T00:00:00Z");
    {
        char *jf = lt_path_join(q.path, "project.json");
        LT_CHECK(lt_proj_config_write(&q.cfg, jf) == 0);
        lt_free(jf);
    }
    lt_project_free(&q);
    LT_CHECK(lt_project_list(&list, &n, d) == 0);
    LT_CHECK(n == 2);
    if (n == 2) {
        LT_CHECK_STR(list[0].name, "My Turbo/Build:1");
        LT_CHECK_STR(list[0].modified, "2999-01-01T00:00:00Z");
        LT_CHECK_STR(list[1].name, "..");
        LT_CHECK_STR(list[0].signature, "speeduino 202501-mini");
    }
    lt_project_info_free(list, n);
    LT_CHECK(lt_project_list(&list, &n, "build/test_tmp/does_not_exist") == 0 && n == 0);
    lt_free(p1);
    lt_free(d);
}

LT_TEST(project_config_validation)
{
    char *d = fresh("cfg"), *f = lt_path_join(d, "project.json");
    lt_project q;
    const char *ok = "{ \"extra\": [1,2], \"version\":\"1.0\",\"name\":\"N\",\"created\":\"c\","
                     "\"modified\":\"m\",\"ecu_definition\":\"projectCfg/definition.ini\","
                     "\"signature\":\"S\",\"connection\":{\"port\":null,\"baud_rate\":0,"
                     "\"timeout_ms\":0},\"settings\":{\"auto_load_tune\":false,"
                     "\"auto_save_tune\":true,\"auto_connect\":false},\"dashboard\":null}";
    lt_file_write(f, ok, strlen(ok));
    LT_CHECK(lt_project_open(d, &q) == 0);
    LT_CHECK(q.cfg.settings.max_restore_points == 20);
    LT_CHECK(q.cfg.conn.baud_rate == 0);
    LT_CHECK(!q.cfg.settings.auto_load_tune && q.cfg.settings.auto_save_tune);
    LT_CHECK(q.tune == NULL);
    lt_project_free(&q);
    /* missing key -> EDATA */
    lt_file_write(f, "{\"version\":\"1.0\"}", 17);
    LT_CHECK(lt_project_open(d, &q) == LT_EDATA);
    lt_file_write(f, "garbage", 7);
    LT_CHECK(lt_project_open(d, &q) == LT_EDATA);
    lt_plat_remove(f);
    LT_CHECK(lt_project_open(d, &q) != 0);
    lt_free(f);
    lt_free(d);
}

LT_TEST(projects_dir_env)
{
    static char envbuf[] = "LIBRETUNE_PROJECTS=build/test_tmp/pd_env";
    char *p;
    putenv(envbuf);
    p = lt_projects_dir();
    LT_CHECK(p != NULL);
    if (p) LT_CHECK_STR(p, "build/test_tmp/pd_env");
    lt_free(p);
    {
        lt_project pr;
        char *base = fresh("envproj");
        static char env2[256];
        lt_snprintf(env2, sizeof env2, "LIBRETUNE_PROJECTS=%s", base);
        putenv(env2);
        LT_CHECK(lt_project_create("Env", FIX "mini.ini", "s", NULL, &pr) == 0);
        LT_CHECK(strncmp(pr.path, base, strlen(base)) == 0);
        lt_project_free(&pr);
        lt_free(base);
    }
}

/* ---------------------------------------------------------------- repo */
LT_TEST(repo_extract_info)
{
    char *sig = NULL, *name = NULL;
    LT_CHECK(lt_ini_extract_info("[MegaTune]\n  MTversion = 2.25\n[TunerStudio]\n"
                                 "  queryCommand = \"S\"\n  signature= \"rusEFI master.2026.02.20."
                                 "uaefi.4211171972\" ; comment\n", &sig, &name) == 0);
    LT_CHECK_STR(sig, "rusEFI master.2026.02.20.uaefi.4211171972");
    LT_CHECK_STR(name, sig);
    lt_free(sig);
    lt_free(name);
    LT_CHECK(lt_ini_extract_info("[MegaTune]\nsignature = \"TestECU 1.0\"\nnEmu = \"Test Engine "
                                 "Control Unit\"", &sig, &name) == 0);
    LT_CHECK_STR(sig, "TestECU 1.0");
    LT_CHECK_STR(name, "Test Engine Control Unit");
    lt_free(sig);
    lt_free(name);
    LT_CHECK(lt_ini_extract_info("[Other]\nsignature = \"x\"\n", &sig, &name) == LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "Could not find signature") != NULL);
    /* unquoted with comment; section header with trailing comment */
    LT_CHECK(lt_ini_extract_info("[ TunerStudio ] ; hi\nSIGNATURE = abc ; c\n", &sig, &name) == 0);
    LT_CHECK_STR(sig, "abc");
    lt_free(sig);
    lt_free(name);
}

LT_TEST(repo_import)
{
    char *d = fresh("repo"), *src, *id = NULL, *id2 = NULL, *f;
    lt_ini_repo r;
    const lt_ini_entry *e;
    char **ids;
    size_t n;
    const char *ini1 = "[MegaTune]\nsignature = \"TestECU 1.0\"\nnEmu = \"Test Engine Control "
                       "Unit\"\n";

    LT_CHECK(lt_ini_repo_open(&r, d) == 0 && r.n == 0);
    src = lt_path_join(d, "src_a.ini");
    lt_file_write(src, ini1, strlen(ini1));
    LT_CHECK(lt_ini_repo_import(&r, src, &id) == 0);
    LT_CHECK_STR(id, "TestECU_1.0");
    e = lt_ini_repo_get(&r, id);
    LT_CHECK(e != NULL);
    if (e) {
        LT_CHECK_STR(e->name, "Test Engine Control Unit");
        LT_CHECK_STR(e->signature, "TestECU 1.0");
        LT_CHECK_STR(e->path, "TestECU_1.0.ini");
        LT_CHECK_STR(e->source, src);
    }
    f = lt_ini_repo_path(&r, id);
    LT_CHECK(f && lt_file_exists(f));
    lt_free(f);
    /* same signature again: existing id, nothing rewritten */
    LT_CHECK(lt_ini_repo_import(&r, src, &id2) == 0);
    LT_CHECK_STR(id2, "TestECU_1.0");
    LT_CHECK(r.n == 1);
    lt_free(id2);

    /* BOM + cp1252 (0x93 0x94 curly quotes, 0xE9) */
    {
        const char raw[] = "\xEF\xBB\xBF[MegaTune]\nsignature = \"Caf\xE9 \xE2\x80\x93 1\"\nnEmu = "
                           "\"\x93Quoted\x94\"\n";
        char *s2 = lt_path_join(d, "src_b.ini");
        char *id3 = NULL, *txt;
        lt_file_write(s2, raw, sizeof raw - 1);
        LT_CHECK(lt_ini_repo_import(&r, s2, &id3) == 0);
        LT_CHECK(id3 != NULL);
        e = lt_ini_repo_get(&r, id3);
        LT_CHECK(e != NULL);
        if (e) LT_CHECK(strstr(e->name, "\xE2\x80\x9C" "Quoted" "\xE2\x80\x9D") != NULL);
        f = lt_ini_repo_path(&r, id3);
        txt = slurp(f);
        LT_CHECK(txt && (unsigned char)txt[0] == '[');
        lt_free(txt);
        lt_free(f);
        lt_free(id3);
        lt_free(s2);
    }
    LT_CHECK(r.n == 2);

    /* invalid ini without signature */
    {
        char *s3 = lt_path_join(d, "src_c.ini");
        lt_file_write(s3, "[Foo]\nx=1\n", 10);
        LT_CHECK(lt_ini_repo_import(&r, s3, NULL) == LT_EDATA);
        lt_free(s3);
    }
    LT_CHECK(lt_ini_repo_import(&r, "build/test_tmp/none.ini", NULL) == LT_ENOENT);

    /* signature search */
    LT_CHECK(lt_ini_repo_find_signature(&r, "TestECU 1.0", LT_FALSE) == r.e + 0);
    LT_CHECK(lt_ini_repo_find_signature(&r, "TestECU 1.1", LT_FALSE) == NULL);
    LT_CHECK(lt_ini_repo_find_signature(&r, "TestECU", LT_TRUE) != NULL);

    /* index persists */
    lt_ini_repo_close(&r);
    LT_CHECK(lt_ini_repo_open(&r, d) == 0 && r.n == 2);
    LT_CHECK(lt_ini_repo_get(&r, "TestECU_1.0") != NULL);
    {
        char *jf = lt_path_join(d, "index.json"), *txt = slurp(jf);
        LT_CHECK(txt && strstr(txt, "\"id\": \"TestECU_1.0\"") != NULL);
        LT_CHECK(strstr(txt, "\"imported\": \"20") != NULL);
        lt_free(txt);
        lt_free(jf);
    }
    /* scan dir imports *.INI files */
    {
        char *sd = lt_path_join(d, "scan"), *a, *b, *c;
        const char *ini2 = "[TunerStudio]\nsignature = \"Scan ECU 2\"\n";
        lt_plat_mkdirs(sd);
        a = lt_path_join(sd, "a.INI");
        b = lt_path_join(sd, "b.txt");
        c = lt_path_join(sd, "c.ini");
        lt_file_write(a, ini2, strlen(ini2));
        lt_file_write(b, ini2, strlen(ini2));
        lt_file_write(c, "junk", 4);
        LT_CHECK(lt_ini_repo_scan_dir(&r, sd, &ids, &n) == 0);
        LT_CHECK(n == 1);
        if (n == 1) LT_CHECK_STR(ids[0], "Scan_ECU_2");
        lt_proj_strv_free(ids, n);
        LT_CHECK(lt_ini_repo_scan_dir(&r, "build/test_tmp/nodir", &ids, &n) == 0 && n == 0);
        lt_free(a);
        lt_free(b);
        lt_free(c);
        lt_free(sd);
    }
    LT_CHECK(r.n == 3);
    /* remove */
    f = lt_ini_repo_path(&r, "TestECU_1.0");
    LT_CHECK(lt_ini_repo_remove(&r, "TestECU_1.0") == 0);
    LT_CHECK(!lt_file_exists(f));
    LT_CHECK(lt_ini_repo_get(&r, "TestECU_1.0") == NULL);
    LT_CHECK(lt_ini_repo_remove(&r, "unknown") == 0);
    lt_free(f);
    lt_ini_repo_close(&r);
    /* corrupt index */
    f = lt_path_join(d, "index.json");
    lt_file_write(f, "{\"a\":1}", 7);
    LT_CHECK(lt_ini_repo_open(&r, d) == LT_EDATA);
    lt_file_write(f, "[{\"id\":\"x\"}]", 12);
    LT_CHECK(lt_ini_repo_open(&r, d) == LT_EDATA);
    lt_free(f);
    lt_free(id);
    lt_free(src);
    lt_free(d);
}

/* ---------------------------------------------------------------- restore */
static void make_project(const char *dir, lt_project *p)
{
    lt_tune_value v;
    LT_CHECK(lt_project_create("NA6 Speeduino", FIX "mini.ini", "speeduino 202501-mini", dir,
                               p) == 0);
    p->tune = lt_tune_new("speeduino 202501-mini");
    lt_tune_value_init(&v);
    v.scalar = 12.6;
    lt_tune_set_constant(p->tune, "reqFuel", &v, 1);
}

LT_TEST(restore_timestamp_parse)
{
    char t[21];
    LT_CHECK(lt_restore_parse_timestamp("NA6_SPEEDUINO_2026-08-11_08.47.31.msq", t));
    LT_CHECK_STR(t, "2026-08-11T08:47:31Z");
    LT_CHECK(lt_restore_parse_timestamp("My_Turbo_Build_2025-01-02_03.04.05.msq", t));
    LT_CHECK_STR(t, "2025-01-02T03:04:05Z");
    LT_CHECK(lt_restore_parse_timestamp("_2025-01-02_03.04.05.msq", t));
    LT_CHECK(lt_restore_parse_timestamp("x_2025-01-02_03.04.05-3.msq", t));
    LT_CHECK_STR(t, "2025-01-02T03:04:05Z");
    LT_CHECK(!lt_restore_parse_timestamp("backup_before_turbo.msq", t));
    LT_CHECK(!lt_restore_parse_timestamp("proj_xxxxxxxxxx_yy.zz.ww.msq", t));
    LT_CHECK(!lt_restore_parse_timestamp("a_2025-01-02_03.04.05.txt", t));
    LT_CHECK(!lt_restore_parse_timestamp("2025-01-02_03.04.05.msq", t));
    LT_CHECK(!lt_restore_parse_timestamp("a_2025-01-02_03.04.0x.msq", t));
}

LT_TEST(restore_points)
{
    char *d = fresh("restore"), *path = NULL, *path2 = NULL, *rp;
    lt_project p;
    lt_restore_info *v;
    size_t n, del = 0;
    char *outside;

    make_project(d, &p);
    LT_CHECK(lt_restore_list(&p, &v, &n) == 0 && n == 0);
    LT_CHECK(lt_restore_create(&p, &path) == 0);
    LT_CHECK(lt_restore_create(&p, &path2) == 0);   /* same second: must not overwrite */
    LT_CHECK(path && path2 && strcmp(path, path2) != 0);
    LT_CHECK(strstr(path, "/NA6Speeduino_") != NULL);
    LT_CHECK(lt_file_exists(path) && lt_file_exists(path2));
    /* manual older files */
    rp = lt_path_join(p.path, "restorePoints");
    {
        char *a = lt_path_join(rp, "Old_2020-01-01_00.00.00.msq");
        char *b = lt_path_join(rp, "junk.msq");
        char *c = lt_path_join(rp, "readme.txt");
        lt_file_write(a, "<msq/>", 6);
        lt_file_write(b, "<msq/>", 6);
        lt_file_write(c, "x", 1);
        lt_free(a);
        lt_free(b);
        lt_free(c);
    }
    LT_CHECK(lt_restore_list(&p, &v, &n) == 0);
    LT_CHECK(n == 4);
    if (n == 4) {
        LT_CHECK(strcmp(v[0].filename, v[1].filename) != 0);
        if (strcmp(v[0].created, v[1].created) == 0)    /* same second: -2 suffix is newer */
            LT_CHECK(strlen(v[0].filename) > strlen(v[1].filename));
        LT_CHECK_STR(v[2].filename, "Old_2020-01-01_00.00.00.msq");
        LT_CHECK_STR(v[2].created, "2020-01-01T00:00:00Z");
        LT_CHECK_STR(v[3].filename, "junk.msq");
        LT_CHECK_STR(v[3].created, "1970-01-01T00:00:00Z");
        LT_CHECK(v[0].size_bytes > 100);
    }
    lt_restore_info_free(v, n);
    /* load */
    p.dirty = LT_FALSE;
    {
        char *name = lt_strdup(lt_path_base(path));
        LT_CHECK(lt_restore_load(&p, name) == 0);
        LT_CHECK(p.dirty);
        LT_CHECK(p.tune && lt_tune_get_constant(p.tune, "reqFuel") != NULL);
        lt_free(name);
    }

    /* path security */
    outside = lt_path_join(p.path, "CurrentTune.msq");
    lt_file_write(outside, "keep", 4);
    {
        const char *bad[] = {"../CurrentTune.msq", "..\\CurrentTune.msq", "sub/x.msq",
                             "sub\\x.msq", "..", ".", "", "C:x.msq"};
        size_t i;
        char *abs;
        for (i = 0; i < LT_COUNTOF(bad); i++) {
            char *pp = NULL;
            LT_CHECK(lt_restore_delete(&p, bad[i]) == LT_EINVAL);
            LT_CHECK(lt_restore_path(&p, bad[i], &pp) == LT_EINVAL);
            LT_CHECK(lt_restore_load(&p, bad[i]) == LT_EINVAL);
        }
        abs = lt_path_join(p.path, "CurrentTune.msq");
        LT_CHECK(lt_restore_delete(&p, abs) == LT_EINVAL);
        lt_free(abs);
        LT_CHECK(lt_file_exists(outside));
        LT_CHECK(lt_restore_delete(&p, "missing.msq") == LT_ENOENT);
        LT_CHECK(lt_restore_delete(&p, "readme.txt") == 0);   /* plain file inside the dir */
    }
    {
        char *name = lt_strdup(lt_path_base(path2));
        LT_CHECK(lt_restore_delete(&p, name) == 0);
        LT_CHECK(!lt_file_exists(path2));
        LT_CHECK(lt_file_exists(path));
        lt_free(name);
    }
    /* prune keeps newest */
    LT_CHECK(lt_restore_prune(&p, 1, &del) == 0);
    LT_CHECK(del == 2);
    LT_CHECK(lt_restore_list(&p, &v, &n) == 0 && n == 1);
    lt_restore_info_free(v, n);
    /* no tune */
    lt_tune_free(p.tune);
    p.tune = NULL;
    lt_free(path2);
    path2 = NULL;
    LT_CHECK(lt_restore_create(&p, &path2) == LT_ENOENT);
    lt_free(outside);
    lt_free(rp);
    lt_free(path);
    lt_project_free(&p);
    lt_free(d);
}

/* ---------------------------------------------------------------- snapshots */
LT_TEST(snap_format_message)
{
    char *s = lt_snap_format_message("msg", "  note ");
    LT_CHECK_STR(s, "msg\n\nLT-Note: note");
    lt_free(s);
    s = lt_snap_format_message("msg", NULL);
    LT_CHECK_STR(s, "msg");
    lt_free(s);
    s = lt_snap_format_message("msg", "  ");
    LT_CHECK_STR(s, "msg");
    lt_free(s);
}

LT_TEST(snap_commit_restore)
{
    char *d = fresh("snap"), *f, *fcrlf;
    lt_project p;
    char id1[9], id2[9], id3[9];
    lt_snap_info *h;
    size_t n;
    lt_snap_diff df;
    const char crlf[] = "line1\r\nline2\r\n\xFF\x00\x01 bin";

    make_project(d, &p);
    lt_project_save_tune(&p);
    f = lt_path_join(p.path, "dashboards");
    fcrlf = lt_path_join(f, "a.dash");
    lt_file_write(fcrlf, crlf, sizeof crlf - 1);
    lt_free(f);
    /* no snapshot yet: changes since files exist */
    LT_CHECK(lt_snap_has_changes(p.path) == 1);
    LT_CHECK(lt_snap_commit(p.path, "first\ttab", "  tuned idle ", "Rhett", id1) == 0);
    LT_CHECK(strlen(id1) == 8);
    LT_CHECK(lt_snap_has_changes(p.path) == 0);
    /* excluded files do not count */
    {
        char *x = lt_path_join(p.path, "datalogs"), *y;
        y = lt_path_join(x, "log.mlg");
        lt_file_write(y, "abc", 3);
        lt_free(y);
        y = lt_path_join(p.path, "dashboards/x.tmp");
        lt_file_write(y, "abc", 3);
        lt_free(y);
        y = lt_path_join(p.path, "dashboards/.hidden");
        lt_file_write(y, "abc", 3);
        lt_free(y);
        lt_free(x);
    }
    LT_CHECK(lt_snap_has_changes(p.path) == 0);

    /* modify the tune */
    {
        lt_tune_value v;
        lt_tune_value_init(&v);
        v.scalar = 14.5;
        lt_tune_set_constant(p.tune, "reqFuel", &v, 1);
        v.scalar = 3;
        lt_tune_set_constant(p.tune, "divider", &v, 1);
        lt_project_save_tune(&p);
    }
    LT_CHECK(lt_snap_has_changes(p.path) == 1);
    LT_CHECK(lt_snap_restore(p.path, id1, LT_FALSE) == LT_EBUSY);
    LT_CHECK(lt_snap_commit(p.path, "second", NULL, "Rhett", id2) == 0);
    LT_CHECK(strcmp(id1, id2) != 0);

    /* history */
    LT_CHECK(lt_snap_history(p.path, 0, &h, &n) == 0 && n == 2);
    if (n == 2) {
        LT_CHECK_STR(h[0].message, "second");
        LT_CHECK(h[0].is_head && !h[1].is_head);
        LT_CHECK_STR(h[0].id, id2);
        LT_CHECK_STR(h[1].message, "first\ttab");
        LT_CHECK_STR(h[1].annotation, "tuned idle");
        LT_CHECK(h[0].annotation == NULL);
        LT_CHECK_STR(h[1].author, "Rhett");
        LT_CHECK(strlen(h[0].timestamp) == 19 && h[0].timestamp[10] == ' ');
    }
    lt_snap_info_free(h, n);
    LT_CHECK(lt_snap_history(p.path, 1, &h, &n) == 0 && n == 1);
    lt_snap_info_free(h, n);

    /* diff first -> second */
    LT_CHECK(lt_snap_diff_compute(p.path, id1, id2, &df) == 0);
    LT_CHECK(df.n_files == 1);
    if (df.n_files == 1) LT_CHECK_STR(df.files_changed[0], "CurrentTune.msq (modified)");
    {
        size_t i;
        int saw_req = 0, saw_div = 0;
        for (i = 0; i < df.n_changes; i++) {
            if (strcmp(df.changes[i].name, "reqFuel") == 0) {
                saw_req = 1;
                LT_CHECK_STR(df.changes[i].old_value, "12.6");
                LT_CHECK_STR(df.changes[i].new_value, "14.5");
                LT_CHECK_STR(df.changes[i].change_type, "modified");
            }
            if (strcmp(df.changes[i].name, "divider") == 0) {
                saw_div = 1;
                LT_CHECK(df.changes[i].old_value == NULL);
                LT_CHECK_STR(df.changes[i].change_type, "added");
            }
        }
        LT_CHECK(saw_req && saw_div);
    }
    lt_snap_diff_free(&df);
    /* working copy diff: delete the dash and add a file */
    lt_plat_remove(fcrlf);
    LT_CHECK(lt_snap_diff_compute(p.path, id2, NULL, &df) == 0);
    LT_CHECK(df.n_files == 1);
    if (df.n_files == 1) LT_CHECK_STR(df.files_changed[0], "dashboards/a.dash (removed)");
    lt_snap_diff_free(&df);
    LT_CHECK(lt_snap_diff_compute(p.path, "../../etc", NULL, &df) == LT_EINVAL);
    LT_CHECK(lt_snap_diff_compute(p.path, "deadbeef", NULL, &df) == LT_ENOENT);

    /* restore byte exact, repeatable, removes added files */
    {
        char *extra = lt_path_join(p.path, "dashboards/new.dash"), *got;
        size_t gl = 0;
        lt_file_write(extra, "zzz", 3);
        LT_CHECK(lt_snap_restore(p.path, id2, LT_FALSE) == LT_EBUSY);
        LT_CHECK(lt_snap_restore(p.path, id2, LT_TRUE) == 0);
        LT_CHECK(!lt_file_exists(extra));
        got = lt_file_read(fcrlf, &gl);
        LT_CHECK(got && gl == sizeof crlf - 1 && memcmp(got, crlf, gl) == 0);
        lt_free(got);
        LT_CHECK(lt_snap_has_changes(p.path) == 0);
        LT_CHECK(lt_snap_restore(p.path, id2, LT_FALSE) == 0);
        LT_CHECK(lt_snap_restore(p.path, id1, LT_FALSE) == 0);
        {
            char *tp = lt_project_file(&p, "CurrentTune.msq");
            lt_tune *t = lt_tune_load(tp);
            const lt_tune_entry *e;
            lt_free(tp);
            e = t ? lt_tune_get_constant(t, "reqFuel") : NULL;
            LT_CHECK(e && e->value.scalar > 12.59 && e->value.scalar < 12.61);
            LT_CHECK(t && lt_tune_get_constant(t, "divider") == NULL);
            if (t) lt_tune_free(t);
        }
        LT_CHECK(lt_snap_restore(p.path, "00000000", LT_TRUE) == LT_ENOENT);
        LT_CHECK(lt_snap_restore(p.path, "../x", LT_TRUE) == LT_EINVAL);
        lt_free(extra);
    }

    /* third commit, then prune */
    LT_CHECK(lt_snap_commit(p.path, "third", NULL, "R", id3) == 0);
    LT_CHECK(lt_snap_prune(p.path, 0) == LT_EINVAL);
    LT_CHECK(lt_snap_prune(p.path, 2) == 0);
    LT_CHECK(lt_snap_history(p.path, 0, &h, &n) == 0 && n == 2);
    if (n == 2) {
        LT_CHECK_STR(h[0].message, "third");
        LT_CHECK_STR(h[1].message, "second");
    }
    lt_snap_info_free(h, n);
    {
        char *old = lt_proj_join3(p.path, "snapshots", id1);
        LT_CHECK(!lt_plat_is_dir(old));
        lt_free(old);
    }
    LT_CHECK(lt_snap_prune(p.path, 10) == 0);
    lt_free(fcrlf);
    lt_project_free(&p);
    lt_free(d);
}

LT_TEST(snap_empty_and_unique)
{
    char *d = fresh("snap2");
    lt_project p;
    char ids[6][9];
    int i, same = 0;
    make_project(d, &p);
    lt_project_save_tune(&p);
    for (i = 0; i < 6; i++) LT_CHECK(lt_snap_commit(p.path, "same", NULL, "a", ids[i]) == 0);
    for (i = 1; i < 6; i++)
        if (strcmp(ids[0], ids[i]) == 0) same = 1;
    LT_CHECK(!same);
    {
        lt_snap_info *h;
        size_t n;
        LT_CHECK(lt_snap_history(p.path, 0, &h, &n) == 0 && n == 6);
        lt_snap_info_free(h, n);
    }
    lt_project_free(&p);
    lt_free(d);
}

/* ---------------------------------------------------------------- TS import */
LT_TEST(ts_import)
{
    char *d = fresh("tsimp"), *x;
    lt_project p;
    lt_restore_info *v;
    size_t n;

    LT_CHECK(lt_project_import_ts(FIX "ts_proj", d, &p) == 0);
    LT_CHECK_STR(p.cfg.name, "MS3-Example_Project");
    LT_CHECK_STR(p.cfg.signature, "speeduino 202501-mini");
    LT_CHECK_STR(p.cfg.conn.port, "COM8");
    LT_CHECK(p.cfg.conn.baud_rate == 115200);
    LT_CHECK(p.tune != NULL);
    if (p.tune) {
        const lt_tune_entry *e = lt_tune_get_constant(p.tune, "reqFuel");
        const lt_tune_entry *pc = lt_tune_get_pc_variable(p.tune, "rpmhigh");
        LT_CHECK(e != NULL);
        LT_CHECK(pc && pc->value.scalar == 7500);
    }
    LT_CHECK(lt_restore_list(&p, &v, &n) == 0);
    LT_CHECK(n == 1);
    if (n == 1) LT_CHECK_STR(v[0].filename, "MS3_2025-01-02_03.04.05.msq");
    lt_restore_info_free(v, n);
    x = lt_proj_join3(p.path, "projectCfg", "definition.ini");
    LT_CHECK(lt_file_exists(x));
    lt_free(x);
    lt_project_free(&p);
    /* second import into same parent collides */
    LT_CHECK(lt_project_import_ts(FIX "ts_proj", d, &p) == LT_EEXIST);
    /* not a TS project */
    LT_CHECK(lt_project_import_ts(FIX "ts_proj/projectCfg", d, &p) == LT_ENOENT);
    LT_CHECK(strstr(lt_err_get(), "project.properties not found") != NULL);
    lt_free(d);
}

int main(void)
{
    lt_plat_mkdirs("build/test_tmp");
    LT_RUN(json_parse_basic);
    LT_RUN(json_errors);
    LT_RUN(json_write_layout);
    LT_RUN(json_number_roundtrip);
    LT_RUN(json_file_io);
    LT_RUN(props_basic);
    LT_RUN(props_edge);
    LT_RUN(props_roundtrip);
    LT_RUN(props_latin1);
    LT_RUN(project_lifecycle);
    LT_RUN(project_config_validation);
    LT_RUN(projects_dir_env);
    LT_RUN(repo_extract_info);
    LT_RUN(repo_import);
    LT_RUN(restore_timestamp_parse);
    LT_RUN(restore_points);
    LT_RUN(snap_format_message);
    LT_RUN(snap_commit_restore);
    LT_RUN(snap_empty_and_unique);
    LT_RUN(ts_import);
    LT_TEST_MAIN_END
}
