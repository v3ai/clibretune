/* test_agent.c - HTTP client/parser, assistant chat loop (against an in-process fake
 * OpenAI-compatible server), tools and proposals, JSON-RPC / MCP (stdio dispatcher and the
 * loopback HTTP transport), assistant settings (G-123 local form, G-124).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/agent/agent.h"
#include "../src/mcp/mcp.h"
#include "../src/net/http.h"
#include "../src/plat/plat.h"

/* ------------------------------------------------------------------ parser */
static int feed_split(lt_hparse *p, const char *text, size_t step)
{
    size_t i, n = strlen(text);
    int r = 0;
    for (i = 0; i < n && r == 0; i += step) {
        size_t k = (n - i < step) ? n - i : step;
        r = lt_hparse_feed(p, text + i, k);
    }
    return r;
}

LT_TEST(parse_content_length)
{
    static const char msg[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: 11\r\n\r\nhello world";
    size_t step;
    char v[64];
    for (step = 1; step <= 7; step += 3) {
        lt_hparse p;
        lt_hparse_init(&p, 0);
        LT_CHECK(feed_split(&p, msg, step) == 1);
        LT_CHECK(p.status == 200);
        LT_CHECK(p.body.len == 11 && memcmp(p.body.data, "hello world", 11) == 0);
        LT_CHECK(lt_hparse_header(&p, "content-type", v, sizeof(v)) && strcmp(v, "application/json") == 0);
        LT_CHECK(!lt_hparse_header(&p, "x-none", v, sizeof(v)));
        lt_hparse_free(&p);
    }
}

LT_TEST(parse_chunked)
{
    static const char msg[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                              "5\r\nhello\r\n6;ext=1\r\n world\r\nA\r\n0123456789\r\n0\r\nX-T: 1\r\n\r\n";
    size_t step;
    for (step = 1; step <= 9; step += 2) {
        lt_hparse p;
        lt_hparse_init(&p, 0);
        LT_CHECK(feed_split(&p, msg, step) == 1);
        LT_CHECK(p.body.len == 21 && memcmp(p.body.data, "hello world0123456789", 21) == 0);
        lt_hparse_free(&p);
    }
    {
        lt_hparse p;
        lt_hparse_init(&p, 0);
        LT_CHECK(feed_split(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", 100) < 0);
        LT_CHECK(p.err[0] != '\0');
        lt_hparse_free(&p);
    }
}

LT_TEST(parse_until_close_and_errors)
{
    lt_hparse p;
    lt_hparse_init(&p, 0);
    LT_CHECK(lt_hparse_feed(&p, "HTTP/1.0 200 OK\r\n\r\nabc", 22) == 0);
    LT_CHECK(lt_hparse_feed(&p, "def", 3) == 0);
    LT_CHECK(lt_hparse_eof(&p) == 1);
    LT_CHECK(p.body.len == 6);
    lt_hparse_free(&p);
    /* truncated Content-Length body */
    lt_hparse_init(&p, 0);
    LT_CHECK(feed_split(&p, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", 1000) == 0);
    LT_CHECK(lt_hparse_eof(&p) < 0);
    lt_hparse_free(&p);
    /* not HTTP at all */
    lt_hparse_init(&p, 0);
    LT_CHECK(feed_split(&p, "SSH-2.0-x\r\n", 1000) < 0);
    lt_hparse_free(&p);
    /* 204 has no body */
    lt_hparse_init(&p, 0);
    LT_CHECK(feed_split(&p, "HTTP/1.1 204 No Content\r\n\r\n", 1000) == 1);
    lt_hparse_free(&p);
}

typedef struct sink { lt_buf b; int calls; } sink;
static void sink_cb(void *ud, const char *d, size_t n)
{
    sink *s = (sink *)ud;
    lt_buf_append(&s->b, d, n);
    s->calls++;
}

LT_TEST(parse_stream_callback_and_request)
{
    lt_hparse p;
    sink s;
    lt_buf_init(&s.b);
    s.calls = 0;
    lt_hparse_init(&p, 0);
    lt_hparse_set_callback(&p, sink_cb, &s);
    LT_CHECK(feed_split(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", 4) == 1);
    LT_CHECK(s.b.len == 5 && memcmp(s.b.data, "abcde", 5) == 0);
    LT_CHECK(s.calls >= 2 && p.body.len == 0);
    lt_hparse_free(&p);
    lt_buf_free(&s.b);
    /* a request with a body */
    lt_hparse_init(&p, 1);
    LT_CHECK(feed_split(&p, "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nping", 5) == 1);
    LT_CHECK(strcmp(p.method, "POST") == 0 && strcmp(p.target, "/mcp") == 0);
    LT_CHECK(p.body.len == 4);
    lt_hparse_free(&p);
}

LT_TEST(url_parse)
{
    lt_http_url u;
    LT_CHECK(lt_http_url_parse("http://127.0.0.1:11434/v1/chat", &u) == 0);
    LT_CHECK(strcmp(u.host, "127.0.0.1") == 0 && u.port == 11434 && strcmp(u.path, "/v1/chat") == 0);
    LT_CHECK(lt_http_url_parse("http://localhost", &u) == 0);
    LT_CHECK(u.port == 80 && strcmp(u.path, "/") == 0);
    LT_CHECK(lt_http_url_parse("http://[::1]:8080/x", &u) == 0);
    LT_CHECK(strcmp(u.host, "::1") == 0 && u.port == 8080);
    LT_CHECK(lt_http_url_parse("https://example.com/v1", &u) < 0);
    LT_CHECK(strstr(lt_err_get(), "https://") != NULL && strstr(lt_err_get(), "local") != NULL);
    LT_CHECK(lt_http_url_parse("ftp://x", &u) < 0);
    LT_CHECK(lt_http_url_parse("http://h:99999/", &u) < 0);
    LT_CHECK(lt_http_url_parse("http:///x", &u) < 0);
}

/* ------------------------------------------------------------------ fake server */
#define FAKE_MAX 8
typedef struct fake {
    lt_server *srv;
    lt_stream *cl;
    lt_hparse p;
    int got_req;
    char **pieces[FAKE_MAX];
    size_t npieces[FAKE_MAX];
    int nscripts, cur;
    size_t pi;
    char *last_body;                 /* heap copies: the struct stays small */
    char *bodies[FAKE_MAX];
    char auth[FAKE_MAX][96];
    int served, hold_n;
} fake;

static void fake_init(fake *f)
{
    memset(f, 0, sizeof(*f));
    f->srv = lt_server_listen(0);
    LT_CHECK(f->srv != NULL);
}

static void fake_add_piece(fake *f, int s, const char *text, size_t n)
{
    f->pieces[s] = (char **)lt_realloc(f->pieces[s], (f->npieces[s] + 1) * sizeof(char *));
    f->pieces[s][f->npieces[s]] = lt_strndup(text, n);
    f->npieces[s]++;
}

/* script: status line + headers, then the body in slices (chunked or plain) */
static void fake_script(fake *f, int status, const char *ctype, const char *body, size_t slice,
                        int chunked)
{
    int s = f->nscripts++;
    char head[300], tmp[64];
    size_t i, n = strlen(body);
    lt_snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Type: %s\r\n%s\r\n", status, ctype,
                chunked ? "Transfer-Encoding: chunked\r\n" : "");
    if (!chunked) {
        lt_snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %lu\r\n\r\n",
                    status, ctype, (unsigned long)n);
    }
    fake_add_piece(f, s, head, strlen(head));
    for (i = 0; i < n; i += slice) {
        size_t k = (n - i < slice) ? n - i : slice;
        if (chunked) {
            lt_buf b;
            lt_buf_init(&b);
            lt_snprintf(tmp, sizeof(tmp), "%lx\r\n", (unsigned long)k);
            lt_buf_puts(&b, tmp);
            lt_buf_append(&b, body + i, k);
            lt_buf_puts(&b, "\r\n");
            fake_add_piece(f, s, b.data, b.len);
            lt_buf_free(&b);
        } else {
            fake_add_piece(f, s, body + i, k);
        }
    }
    if (chunked) { fake_add_piece(f, s, "0\r\n\r\n", 5); }
}

/* one step of the server: accept / read the request / send one piece */
static void fake_poll(fake *f)
{
    if (f->cl == NULL) {
        if (f->nscripts == 0 && f->hold_n > 0) { return; }     /* silent server: keep the first client */
        f->cl = lt_server_accept(f->srv, 0);
        if (f->cl != NULL && f->nscripts == 0) { f->hold_n++; }
        if (f->cl != NULL) { lt_hparse_init(&f->p, 1); f->got_req = 0; f->pi = 0; }
        return;
    }
    if (!f->got_req) {
        char buf[2048];
        int n = lt_stream_read(f->cl, buf, (int)sizeof(buf), 0), r = 0;
        if (n > 0) { r = lt_hparse_feed(&f->p, buf, (size_t)n); }
        if (r > 0) {
            f->got_req = 1;
            lt_free(f->bodies[f->served % FAKE_MAX]);
            f->bodies[f->served % FAKE_MAX] = lt_strdup(f->p.body.data != NULL ? f->p.body.data : "");
            f->last_body = f->bodies[f->served % FAKE_MAX];
            f->auth[f->served % FAKE_MAX][0] = '\0';
            (void)lt_hparse_header(&f->p, "authorization", f->auth[f->served % FAKE_MAX], 96);
        }
        return;
    }
    if (f->nscripts == 0) { return; }                          /* never answers */
    if (f->cur < f->nscripts && f->pi < f->npieces[f->cur]) {
        const char *s = f->pieces[f->cur][f->pi++];
        (void)lt_stream_write(f->cl, s, (int)strlen(s));
        return;
    }
    lt_stream_close(f->cl);
    lt_hparse_free(&f->p);
    f->cl = NULL;
    f->cur++;
    f->served++;
}

static void fake_free(fake *f)
{
    int s;
    size_t i;
    if (f->cl != NULL) { lt_stream_close(f->cl); lt_hparse_free(&f->p); }
    for (s = 0; s < FAKE_MAX; s++) { lt_free(f->bodies[s]); }
    lt_server_close(f->srv);
    for (s = 0; s < f->nscripts; s++) {
        for (i = 0; i < f->npieces[s]; i++) { lt_free(f->pieces[s][i]); }
        lt_free(f->pieces[s]);
    }
}

static void fake_url(fake *f, char *out, size_t cap, const char *path)
{
    lt_snprintf(out, cap, "http://127.0.0.1:%d%s", lt_server_port(f->srv), path);
}

LT_TEST(http_client_chunked_partial)
{
    fake f;
    char url[100];
    lt_http *h;
    int i, r = 0;
    fake_init(&f);
    fake_script(&f, 200, "application/json", "{\"answer\": 42, \"text\": \"chunked and sliced\"}", 7, 1);
    fake_url(&f, url, sizeof(url), "/v1/x");
    h = lt_http_start("POST", url, "X-Test: 1\r\n", "{\"a\":1}", 7, 3000, NULL, NULL);
    LT_CHECK(h != NULL);
    for (i = 0; i < 3000 && h != NULL && r == 0; i++) {
        fake_poll(&f);
        r = lt_http_step(h);
        lt_plat_sleep_ms(1);
    }
    LT_CHECK(r == 1);
    if (h != NULL) {
        LT_CHECK(lt_http_status(h) == 200);
        LT_CHECK(lt_http_body(h)->len == strlen("{\"answer\": 42, \"text\": \"chunked and sliced\"}"));
        LT_CHECK(strstr(lt_http_body(h)->data, "chunked and sliced") != NULL);
        LT_CHECK(strcmp(f.last_body, "{\"a\":1}") == 0);
        lt_http_free(h);
    }
    fake_free(&f);
}

LT_TEST(http_client_timeout_and_refused)
{
    fake f;
    char url[100];
    lt_http *h;
    int i, r = 0;
    fake_init(&f);                       /* accepts but never answers */
    fake_url(&f, url, sizeof(url), "/");
    h = lt_http_start("GET", url, NULL, NULL, 0, 150, NULL, NULL);
    LT_CHECK(h != NULL);
    for (i = 0; i < 2000 && h != NULL && r == 0; i++) {
        fake_poll(&f);
        r = lt_http_step(h);
        lt_plat_sleep_ms(1);
    }
    LT_CHECK(r < 0);
    if (h != NULL) {
        LT_CHECK(strstr(lt_http_error(h), "timed out") != NULL);
        lt_http_free(h);
    }
    fake_free(&f);
    /* the listener is gone: connecting fails cleanly */
    LT_CHECK(lt_http_start("GET", url, NULL, NULL, 0, 500, NULL, NULL) == NULL);
    LT_CHECK(lt_http_start("GET", "https://127.0.0.1:1/", NULL, NULL, 0, 500, NULL, NULL) == NULL);
}

/* ------------------------------------------------------------------ app fixture */
static lt_app *demo_app(void)
{
    char buf[512], cfg[128];
    lt_app *app;
    int i;
    lt_snprintf(cfg, sizeof(cfg), "build/test_agent_cfg_%lu", (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(cfg);
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", cfg) == 0);
    app = lt_app_new();
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    for (i = 0; i < 3; i++) { lt_plat_sleep_ms(60); lt_app_service(app); }
    return app;
}

static const char *first_table(lt_app *app)
{
    size_t i;
    for (i = 0; i < app->ini->n_tables; i++) {
        const lt_ini_table *t = app->ini->tables[i];
        if (t->is_3d && t->x_size >= 4 && t->y_size >= 4) { return t->name; }
    }
    return app->ini->tables[0]->name;
}

static char *run_tool(const lt_ag_env *env, int tier, const char *name, const char *args,
                      lt_ag_props *props, int *rc)
{
    lt_buf out;
    lt_json *j = NULL;
    char err[40];
    lt_buf_init(&out);
    if (args != NULL) { j = lt_json_parse(args, strlen(args), err, sizeof(err)); }
    *rc = lt_ag_tool_run(env, tier, name, j, props, &out);
    lt_json_free(j);
    return lt_buf_take(&out);
}

/* ------------------------------------------------------------------ tools */
LT_TEST(tools_read)
{
    lt_app *app = demo_app();
    lt_ag_env env;
    int rc = 0;
    char *o;
    char args[200];
    lt_json *j;
    char err[40];
    const char *tn = first_table(app);
    LT_CHECK(lt_feat_assistant_env(app, &env) == 0);
    o = run_tool(&env, LT_AG_TIER_READ, "list_tables", NULL, NULL, &rc);
    LT_CHECK(rc == 0 && strstr(o, "\"tables\"") != NULL && strstr(o, tn) != NULL);
    lt_free(o);
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\"}", tn);
    o = run_tool(&env, LT_AG_TIER_READ, "read_table", args, NULL, &rc);
    j = lt_json_parse(o, strlen(o), err, sizeof(err));
    LT_CHECK(rc == 0 && j != NULL);
    if (j != NULL) {
        const lt_json *z = lt_json_get(j, "values_by_row");
        LT_CHECK(z != NULL && z->n == (size_t)lt_json_get_num(j, "rows", 0));
        LT_CHECK(lt_json_get(j, "x_bins") != NULL);
        lt_json_free(j);
    }
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "tune_health_check", args, NULL, &rc);
    LT_CHECK(rc == 0 && strstr(o, "health_score") != NULL);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "read_table", "{\"table_name\":\"nope\"}", NULL, &rc);
    LT_CHECK(rc != 0 && strstr(o, "\"error\"") != NULL);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "read_constant", "{\"name\":\"nope\"}", NULL, &rc);
    LT_CHECK(rc != 0);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "list_features", NULL, NULL, &rc);
    LT_CHECK(rc == 0 && strstr(o, "features") != NULL);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "get_realtime_snapshot", NULL, NULL, &rc);
    LT_CHECK(rc == 0 && strstr(o, "channels") != NULL);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "query_datalog", NULL, NULL, &rc);
    LT_CHECK(rc != 0);                    /* no project: no datalogs folder */
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_READ, "no_such_tool", NULL, NULL, &rc);
    LT_CHECK(rc != 0);
    lt_free(o);
    lt_app_free(app);
}

LT_TEST(tools_tiers_and_proposals)
{
    lt_app *app = demo_app();
    lt_ag_env env;
    lt_ag_props props;
    int rc = 0;
    char *o, args[300];
    const char *tn = first_table(app);
    lt_pagesrc ps;
    lt_tblview v;
    double before, after;
    size_t i;
    lt_ag_props_init(&props);
    LT_CHECK(lt_feat_assistant_env(app, &env) == 0);
    /* tiers */
    LT_CHECK(lt_ag_tool_allowed(LT_AG_TIER_READ, "read_table"));
    LT_CHECK(!lt_ag_tool_allowed(LT_AG_TIER_READ, "propose_table_edit"));
    LT_CHECK(lt_ag_tool_allowed(LT_AG_TIER_TUNE, "propose_bulk_operation"));
    LT_CHECK(!lt_ag_tool_allowed(LT_AG_TIER_TUNE, "propose_constant_change"));
    LT_CHECK(lt_ag_tool_allowed(LT_AG_TIER_CONFIG, "propose_constant_change"));
    LT_CHECK(lt_ag_tier_parse("config") == LT_AG_TIER_CONFIG && lt_ag_tier_parse("CONFIG ") == LT_AG_TIER_READ);
    {
        lt_json *c0 = lt_ag_catalogue(LT_AG_TIER_READ, 0, 1), *c2 = lt_ag_catalogue(LT_AG_TIER_CONFIG, 0, 1);
        LT_CHECK(c0->n == 7 && c2->n == 10);
        lt_json_free(c0);
        lt_json_free(c2);
    }
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\",\"x_index\":1,\"y_index\":1,\"new_value\":50,"
                "\"reason\":\"test\"}", tn);
    o = run_tool(&env, LT_AG_TIER_READ, "propose_table_edit", args, &props, &rc);
    LT_CHECK(rc != 0 && props.n == 0 && strstr(o, "tier") != NULL);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_table_edit", args, NULL, &rc);   /* read-only interface */
    LT_CHECK(rc != 0 && props.n == 0);
    lt_free(o);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_table_edit", args, &props, &rc);
    LT_CHECK(rc == 0 && props.n == 1 && props.v[0].valid);
    lt_free(o);
    /* huge change: clamped by the authority limits, flagged */
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\",\"x_index\":2,\"y_index\":2,\"new_value\":1e9}", tn);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_table_edit", args, &props, &rc);
    LT_CHECK(rc == 0 && props.n == 2 && props.v[1].was_clamped && props.v[1].param < 1e9);
    lt_free(o);
    /* out of bounds cell */
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\",\"x_index\":999,\"y_index\":0,\"new_value\":1}", tn);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_table_edit", args, &props, &rc);
    LT_CHECK(rc != 0 && props.n == 2);
    lt_free(o);
    /* bulk */
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\",\"operation\":\"scale\",\"value\":1.02,"
                "\"cells\":[[0,0],[1,0],[0,1],[1,1]]}", tn);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_bulk_operation", args, &props, &rc);
    LT_CHECK(rc == 0 && props.n == 3 && props.v[2].n_cells == 4);
    lt_free(o);
    lt_snprintf(args, sizeof(args), "{\"table_name\":\"%s\",\"operation\":\"explode\",\"cells\":[[0,0]]}", tn);
    o = run_tool(&env, LT_AG_TIER_TUNE, "propose_bulk_operation", args, &props, &rc);
    LT_CHECK(rc != 0 && props.n == 3);
    lt_free(o);
    /* constants: validation against the declared range */
    {
        const lt_ini_const *c = NULL;
        for (i = 0; i < app->ini->n_constants; i++) {
            const lt_ini_const *k = app->ini->constants[i];
            if (!k->is_pc_var && k->type != LT_DT_BITS && k->type != LT_DT_STRING &&
                lt_shape_count(&k->shape) == 1 && k->range_resolved && k->max > k->min) { c = k; break; }
        }
        LT_CHECK(c != NULL);
        if (c != NULL) {
            lt_snprintf(args, sizeof(args), "{\"name\":\"%s\",\"value\":%g}", c->name, c->max + 1000.0);
            o = run_tool(&env, LT_AG_TIER_CONFIG, "propose_constant_change", args, &props, &rc);
            LT_CHECK(rc == 0 && props.n == 4 && !props.v[3].valid && props.v[3].note[0] != '\0');
            lt_free(o);
            lt_snprintf(args, sizeof(args), "{\"name\":\"%s\",\"value\":%g}", c->name, c->min);
            o = run_tool(&env, LT_AG_TIER_CONFIG, "propose_constant_change", args, &props, &rc);
            LT_CHECK(rc == 0 && props.n == 5 && props.v[4].valid);
            lt_free(o);
        }
    }
    /* nothing is applied until accepted */
    ps = lt_app_pages(app);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, lt_ini_find_table(app->ini, tn), &ps) == 0);
    before = lt_tblview_get(&v, 1, 1);
    lt_tblview_free(&v);
    LT_CHECK(lt_ag_props_accepted(&props) == 0 && lt_ag_props_pending(&props) == 5);
    props.v[0].state = LT_AG_ACCEPTED;
    props.v[3].state = LT_AG_ACCEPTED;            /* invalid: never applied */
    LT_CHECK(lt_ag_props_accepted(&props) == 1);
    LT_CHECK(lt_ag_props_apply(&props, app->ini, &ps) == 1 && props.v[0].applied);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, app->ini, lt_ini_find_table(app->ini, tn), &ps) == 0);
    after = lt_tblview_get(&v, 1, 1);
    lt_tblview_free(&v);
    LT_CHECK_NEAR(after, props.v[0].param, 0.5);
    LT_CHECK(after != before || props.v[0].old_value == props.v[0].param);
    LT_CHECK(lt_ag_props_apply(&props, app->ini, &ps) == 0);       /* idempotent */
    lt_ag_props_free(&props);
    lt_app_free(app);
}

/* ------------------------------------------------------------------ chat against the fake */
static const char sse_round1[] =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Let me look. \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
    "\"function\":{\"name\":\"list_tables\",\"arguments\":\"\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{}\"}}]}}]}\n\n"
    "data: [DONE]\n\n";
static const char sse_round2[] =
    "data: {\"choices\":[{\"delta\":{\"content\":\"You have \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"some tables.\"}}],\"usage\":{\"total_tokens\":77}}\n\n"
    "data: [DONE]\n\n";

static int drive(lt_ag_chat *c, const lt_ag_cfg *cfg, const lt_ag_env *env, fake *f)
{
    int i;
    for (i = 0; i < 6000 && c->busy; i++) {
        fake_poll(f);
        (void)lt_ag_chat_step(c, cfg, env);
        lt_plat_sleep_ms(1);
    }
    return !c->busy;
}

LT_TEST(chat_streaming_tool_loop)
{
    lt_app *app = demo_app();
    lt_ag_env env;
    lt_ag_cfg cfg;
    lt_ag_chat c;
    fake f;
    size_t n;
    char url[100];
    LT_CHECK(lt_feat_assistant_env(app, &env) == 0);
    fake_init(&f);
    fake_script(&f, 200, "text/event-stream", sse_round1, 19, 1);
    fake_script(&f, 200, "text/event-stream", sse_round2, 23, 1);
    lt_ag_cfg_defaults(&cfg);
    fake_url(&f, url, sizeof(url), "/v1");
    lt_strlcpy(cfg.base_url, url, sizeof(cfg.base_url));
    lt_strlcpy(cfg.model, "test-model", sizeof(cfg.model));
    lt_strlcpy(cfg.api_key, "sekret", sizeof(cfg.api_key));
    lt_ag_chat_init(&c);
    /* not enabled yet */
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "hi") < 0 && c.error[0] != '\0');
    cfg.enabled = 1; cfg.ack = 1;
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "What tables do I have?") == 0 && c.busy);
    LT_CHECK(drive(&c, &cfg, &env, &f));
    LT_CHECK(c.error[0] == '\0');
    n = c.msgs->n;
    LT_CHECK(n == 4);                      /* user, assistant+calls, tool, assistant */
    if (n == 4) {
        const lt_json *m1 = lt_json_at(c.msgs, 1), *m2 = lt_json_at(c.msgs, 2), *m3 = lt_json_at(c.msgs, 3);
        LT_CHECK_STR(lt_json_get_str(m1, "content", ""), "Let me look. ");
        LT_CHECK(lt_json_get(m1, "tool_calls") != NULL);
        LT_CHECK_STR(lt_json_get_str(m2, "role", ""), "tool");
        LT_CHECK_STR(lt_json_get_str(m2, "tool_call_id", ""), "c1");
        LT_CHECK(strstr(lt_json_get_str(m2, "content", ""), "\"tables\"") != NULL);
        LT_CHECK_STR(lt_json_get_str(m3, "content", ""), "You have some tables.");
    }
    LT_CHECK(c.n_tok == 77);
    /* what was sent */
    { int k; for (k = 0; k < 20; k++) { fake_poll(&f); } }
    LT_CHECK(f.served == 2);
    LT_CHECK(strstr(f.bodies[0], "\"model\":\"test-model\"") != NULL);
    LT_CHECK(strstr(f.bodies[0], "\"stream\":true") != NULL);
    LT_CHECK(strstr(f.bodies[0], "list_tables") != NULL && strstr(f.bodies[0], "propose_table_edit") == NULL);
    LT_CHECK(strstr(f.bodies[1], "\"role\":\"tool\"") != NULL && strstr(f.bodies[1], "\"tool_call_id\":\"c1\"") != NULL);
    LT_CHECK_STR(f.auth[0], "Bearer sekret");
    lt_ag_chat_free(&c);
    fake_free(&f);
    lt_app_free(app);
}

LT_TEST(chat_plain_json_and_errors)
{
    lt_app *app = demo_app();
    lt_ag_env env;
    lt_ag_cfg cfg;
    lt_ag_chat c;
    fake f;
    char url[100];
    LT_CHECK(lt_feat_assistant_env(app, &env) == 0);
    fake_init(&f);
    /* a server that ignores "stream": one JSON document with a tool call, then text */
    fake_script(&f, 200, "application/json",
                "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
                "{\"id\":\"x9\",\"type\":\"function\",\"function\":{\"name\":\"read_constant\","
                "\"arguments\":\"{\\\"name\\\":\\\"nope\\\"}\"}}]}}]}", 50, 0);
    fake_script(&f, 200, "application/json",
                "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"No such constant.\"}}]}", 30, 0);
    fake_script(&f, 401, "application/json", "{\"error\":{\"message\":\"bad key\"}}", 100, 0);
    lt_ag_cfg_defaults(&cfg);
    fake_url(&f, url, sizeof(url), "/v1/");
    lt_strlcpy(cfg.base_url, url, sizeof(cfg.base_url));
    cfg.enabled = 1; cfg.ack = 1;
    lt_ag_chat_init(&c);
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "x") == 0);
    LT_CHECK(drive(&c, &cfg, &env, &f));
    LT_CHECK(c.error[0] == '\0' && c.msgs->n == 4);
    LT_CHECK(strstr(lt_json_get_str(lt_json_at(c.msgs, 2), "content", ""), "error") != NULL);
    LT_CHECK_STR(lt_json_get_str(lt_json_at(c.msgs, 3), "content", ""), "No such constant.");
    /* HTTP error surfaces the server's message */
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "again") == 0);
    LT_CHECK(drive(&c, &cfg, &env, &f));
    LT_CHECK(strstr(c.error, "401") != NULL && strstr(c.error, "bad key") != NULL);
    /* https endpoints are refused with advice */
    lt_strlcpy(cfg.base_url, "https://api.example.com/v1", sizeof(cfg.base_url));
    c.error[0] = '\0';
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "y") < 0 && strstr(c.error, "https://") != NULL);
    /* an unreachable port fails without blocking the caller for long */
    lt_strlcpy(cfg.base_url, "http://127.0.0.1:1/v1", sizeof(cfg.base_url));
    c.error[0] = '\0';
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "z") < 0 && c.error[0] != '\0');
    lt_ag_chat_free(&c);
    fake_free(&f);
    lt_app_free(app);
}

LT_TEST(chat_cancel_and_proposals_flow)
{
    lt_app *app = demo_app();
    lt_ag_env env;
    lt_ag_cfg cfg;
    lt_ag_chat c;
    fake f;
    char url[100], body[600];
    const char *tn = first_table(app);
    LT_CHECK(lt_feat_assistant_env(app, &env) == 0);
    fake_init(&f);
    lt_snprintf(body, sizeof(body),
                "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"p1\",\"type\":"
                "\"function\",\"function\":{\"name\":\"propose_table_edit\",\"arguments\":\"{\\\"table_name\\\":"
                "\\\"%s\\\",\\\"x_index\\\":0,\\\"y_index\\\":0,\\\"new_value\\\":40}\"}}]}}]}", tn);
    fake_script(&f, 200, "application/json", body, 64, 0);
    fake_script(&f, 200, "application/json", "{\"choices\":[{\"message\":{\"content\":\"Proposed.\"}}]}", 64, 0);
    lt_ag_cfg_defaults(&cfg);
    fake_url(&f, url, sizeof(url), "/v1");
    lt_strlcpy(cfg.base_url, url, sizeof(cfg.base_url));
    cfg.enabled = 1; cfg.ack = 1; cfg.tier = LT_AG_TIER_TUNE;
    lt_ag_chat_init(&c);
    LT_CHECK(lt_ag_chat_send(&c, &cfg, "raise the first cell") == 0);
    LT_CHECK(drive(&c, &cfg, &env, &f));
    LT_CHECK(c.props.n == 1 && c.props.v[0].valid && c.props.v[0].state == LT_AG_PENDING);
    LT_CHECK(strstr(f.bodies[0], "propose_table_edit") != NULL);      /* offered at this tier */
    lt_ag_chat_free(&c);
    fake_free(&f);
    /* read tier: the model is not offered the tool and a call is rejected anyway */
    {
        fake g;
        fake_init(&g);
        fake_script(&g, 200, "application/json", body, 64, 0);
        fake_script(&g, 200, "application/json", "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}", 64, 0);
        fake_url(&g, url, sizeof(url), "/v1");
        lt_strlcpy(cfg.base_url, url, sizeof(cfg.base_url));
        cfg.tier = LT_AG_TIER_READ;
        lt_ag_chat_init(&c);
        LT_CHECK(lt_ag_chat_send(&c, &cfg, "go") == 0);
        LT_CHECK(drive(&c, &cfg, &env, &g));
        LT_CHECK(c.props.n == 0 && strstr(g.bodies[0], "propose_table_edit") == NULL);
        LT_CHECK(strstr(lt_json_get_str(lt_json_at(c.msgs, 2), "content", ""), "tier") != NULL);
        lt_ag_chat_free(&c);
        /* cancel while waiting */
        lt_ag_chat_init(&c);
        LT_CHECK(lt_ag_chat_send(&c, &cfg, "slow") == 0);
        lt_ag_chat_cancel(&c);
        LT_CHECK(!c.busy && c.cancelled);
        lt_ag_chat_free(&c);
        fake_free(&g);
    }
    lt_app_free(app);
}

LT_TEST(request_json_shape)
{
    lt_ag_cfg cfg;
    lt_ag_chat c;
    char *req;
    lt_json *j;
    char err[40];
    lt_ag_cfg_defaults(&cfg);
    cfg.tier = LT_AG_TIER_CONFIG;
    lt_ag_chat_init(&c);
    lt_strlcpy(c.context, "ECU signature: x.", sizeof(c.context));
    lt_json_arr_push(c.msgs, lt_json_obj());
    lt_json_obj_set(c.msgs->items[0], "role", lt_json_str("user"));
    lt_json_obj_set(c.msgs->items[0], "content", lt_json_str("quote \" and \n newline"));
    req = lt_ag_chat_request(&c, &cfg, 1);
    j = lt_json_parse(req, strlen(req), err, sizeof(err));
    LT_CHECK(j != NULL);
    if (j != NULL) {
        const lt_json *m = lt_json_get(j, "messages");
        LT_CHECK(m != NULL && m->n == 2);
        LT_CHECK(strstr(lt_json_get_str(lt_json_at(m, 0), "content", ""), "ECU signature: x.") != NULL);
        LT_CHECK(lt_json_get(j, "tools") != NULL && lt_json_get(j, "tools")->n == 10);
        LT_CHECK(lt_json_get_bool(j, "stream", 0) == 1);
        lt_json_free(j);
    }
    lt_free(req);
    lt_ag_chat_free(&c);
}

/* ------------------------------------------------------------------ MCP */
static int test_env(void *ud, lt_ag_env *env)
{
    return lt_feat_assistant_env((lt_app *)ud, env);
}

static lt_json *rpc(lt_app *app, const char *req)
{
    char *r = lt_mcp_handle(req, strlen(req), test_env, app);
    lt_json *j = NULL;
    char err[40];
    if (r != NULL) { j = lt_json_parse(r, strlen(r), err, sizeof(err)); lt_free(r); }
    return j;
}

LT_TEST(mcp_jsonrpc)
{
    lt_app *app = demo_app();
    lt_json *j;
    char req[300];
    const lt_json *res;
    j = rpc(app, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}");
    LT_CHECK(j != NULL);
    if (j != NULL) {
        res = lt_json_get(j, "result");
        LT_CHECK(res != NULL && lt_json_get(res, "capabilities") != NULL);
        LT_CHECK_STR(lt_json_get_str(lt_json_get(res, "serverInfo"), "name", ""), "libretune");
        LT_CHECK(lt_json_get_num(j, "id", 0) == 1.0);
        lt_json_free(j);
    }
    /* notification: no response */
    {
        const char *nt = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        char *r = lt_mcp_handle(nt, strlen(nt), test_env, app);
        LT_CHECK(r == NULL);
    }
    j = rpc(app, "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/list\"}");
    LT_CHECK(j != NULL);
    if (j != NULL) {
        const lt_json *tools = lt_json_get(lt_json_get(j, "result"), "tools");
        LT_CHECK(tools != NULL && tools->n == 7);
        LT_CHECK(lt_json_get(lt_json_at(tools, 0), "inputSchema") != NULL);
        LT_CHECK_STR(lt_json_get_str(j, "id", ""), "a");
        {
            char *t = lt_json_to_string(j, 0);
            LT_CHECK(strstr(t, "propose_") == NULL);          /* read-only surface */
            lt_free(t);
        }
        lt_json_free(j);
    }
    j = rpc(app, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_tables\",\"arguments\":{}}}");
    LT_CHECK(j != NULL);
    if (j != NULL) {
        res = lt_json_get(j, "result");
        LT_CHECK(res != NULL && lt_json_get_bool(res, "isError", 1) == 0);
        LT_CHECK(lt_json_get(res, "structuredContent") != NULL);
        LT_CHECK(strstr(lt_json_get_str(lt_json_at(lt_json_get(res, "content"), 0), "text", ""), "tables") != NULL);
        lt_json_free(j);
    }
    /* a propose tool is refused, as tool-level data */
    lt_snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
                "{\"name\":\"propose_table_edit\",\"arguments\":{\"table_name\":\"%s\",\"x_index\":0,"
                "\"y_index\":0,\"new_value\":1}}}", first_table(app));
    j = rpc(app, req);
    LT_CHECK(j != NULL);
    if (j != NULL) {
        res = lt_json_get(j, "result");
        LT_CHECK(res != NULL && lt_json_get_bool(res, "isError", 0) == 1);
        lt_json_free(j);
    }
    j = rpc(app, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"nope\"}");
    LT_CHECK(j != NULL && lt_json_get_num(lt_json_get(j, "error"), "code", 0) == -32601.0);
    lt_json_free(j);
    j = rpc(app, "{ not json");
    LT_CHECK(j != NULL && lt_json_get_num(lt_json_get(j, "error"), "code", 0) == -32700.0);
    lt_json_free(j);
    j = rpc(app, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{}}");
    LT_CHECK(j != NULL && lt_json_get_num(lt_json_get(j, "error"), "code", 0) == -32602.0);
    lt_json_free(j);
    /* batch */
    j = rpc(app, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},{\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"},"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}]");
    LT_CHECK(j != NULL && j->type == LT_JSON_ARR && j->n == 2);
    lt_json_free(j);
    lt_app_free(app);
    /* nothing loaded: a tool call reports it as an error result */
    {
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"list_tables\"}}";
        char *r = lt_mcp_handle(q, strlen(q), NULL, NULL);
        LT_CHECK(r != NULL && strstr(r, "\"isError\":true") != NULL);
        lt_free(r);
    }
}

static lt_json *http_rpc(lt_mcp_server *srv, const char *headers, const char *body, int *status)
{
    char url[80], *dummy;
    lt_http *h;
    int i, r = 0;
    lt_json *j = NULL;
    char err[40];
    lt_snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", lt_mcp_server_port(srv));
    h = lt_http_start("POST", url, headers, body, strlen(body), 3000, NULL, NULL);
    dummy = NULL;
    LT_UNUSED(dummy);
    *status = 0;
    if (h == NULL) { return NULL; }
    for (i = 0; i < 3000 && r == 0; i++) {
        lt_mcp_server_poll(srv);
        r = lt_http_step(h);
        lt_plat_sleep_ms(1);
    }
    if (r == 1) {
        *status = lt_http_status(h);
        if (lt_http_body(h)->len > 0) { j = lt_json_parse(lt_http_body(h)->data, lt_http_body(h)->len, err, sizeof(err)); }
    }
    lt_http_free(h);
    return j;
}

LT_TEST(mcp_http_transport)
{
    lt_app *app = demo_app();
    lt_mcp_server *srv;
    lt_json *j;
    int st = 0;
    static const char tok[] = "0123456789abcdef0123456789abcdef";
    static const char ping[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
    LT_CHECK(lt_mcp_server_start(0, "short", test_env, app) == NULL);
    srv = lt_mcp_server_start(0, tok, test_env, app);
    LT_CHECK(srv != NULL);
    if (srv != NULL) {
        j = http_rpc(srv, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n", ping, &st);
        LT_CHECK(st == 200 && j != NULL);
        LT_CHECK(j != NULL && lt_json_get(lt_json_get(j, "result"), "tools") != NULL);
        lt_json_free(j);
        j = http_rpc(srv, "Authorization: Bearer 0123456789abcdef0123456789abcdeX\r\n", ping, &st);
        LT_CHECK(st == 401 && j == NULL);
        j = http_rpc(srv, "", ping, &st);
        LT_CHECK(st == 401);
        j = http_rpc(srv, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\nOrigin: http://evil.example\r\n", ping, &st);
        LT_CHECK(st == 403);
        j = http_rpc(srv, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\nOrigin: http://localhost:3000\r\n", ping, &st);
        LT_CHECK(st == 200);
        lt_json_free(j);
        j = http_rpc(srv, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n",
                     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &st);
        LT_CHECK(st == 202);
        lt_mcp_server_stop(srv);
    }
    lt_app_free(app);
}

/* ------------------------------------------------------------------ config */
LT_TEST(config_and_token)
{
    char dir[128], t1[65], t2[65], t3[65], key[200];
    lt_ag_cfg a, b;
    size_t i;
    lt_snprintf(dir, sizeof(dir), "build/test_agent_conf_%lu", (unsigned long)lt_plat_now_ms());
    lt_ag_cfg_defaults(&a);
    LT_CHECK_STR(a.base_url, "http://127.0.0.1:11434/v1");
    LT_CHECK(a.tier == LT_AG_TIER_READ && !a.enabled && !a.mcp_on);
    a.enabled = 1; a.ack = 1; a.tier = LT_AG_TIER_TUNE; a.mcp_on = 1; a.mcp_port = 9001;
    lt_strlcpy(a.model, "qwen", sizeof(a.model));
    lt_strlcpy(a.api_key, "k-123", sizeof(a.api_key));
    LT_CHECK(lt_ag_cfg_save(&a, dir) == 0);
    LT_CHECK(lt_ag_cfg_load(&b, dir) == 0);
    LT_CHECK(b.enabled && b.ack && b.tier == LT_AG_TIER_TUNE && b.mcp_on && b.mcp_port == 9001);
    LT_CHECK_STR(b.model, "qwen");
    LT_CHECK_STR(b.api_key, "k-123");
    /* environment variable wins over the stored key */
    LT_CHECK(lt_plat_setenv("LT_AI_KEY", "") == 0);
    LT_CHECK(lt_ag_cfg_key(&b, key, sizeof(key)) == 5 && strcmp(key, "k-123") == 0);
    LT_CHECK(lt_plat_setenv("LT_AI_KEY", "from-env") == 0);
    LT_CHECK(lt_ag_cfg_key(&b, key, sizeof(key)) == 8 && strcmp(key, "from-env") == 0);
    LT_CHECK(lt_plat_setenv("LT_AI_KEY", "") == 0);
    /* corrupted tier never widens */
    {
        char *p = lt_path_join(dir, "assistant.conf");
        LT_CHECK(lt_file_write(p, "tier=root\nenabled=true\nmcp_port=80\n", 35) == 0);
        lt_free(p);
        LT_CHECK(lt_ag_cfg_load(&b, dir) == 0);
        LT_CHECK(b.tier == LT_AG_TIER_READ && b.mcp_port == 8765);
    }
    LT_CHECK(lt_ag_token(dir, 0, t1) == 0 && strlen(t1) == 64);
    LT_CHECK(lt_ag_token(dir, 0, t2) == 0 && strcmp(t1, t2) == 0);
    LT_CHECK(lt_ag_token(dir, 1, t3) == 0 && strlen(t3) == 64 && strcmp(t1, t3) != 0);
    for (i = 0; i < 64; i++) { LT_CHECK((t3[i] >= '0' && t3[i] <= '9') || (t3[i] >= 'a' && t3[i] <= 'f')); }
}

LT_TEST(registered_in_shell)
{
    lt_app *app = lt_app_new();
    LT_CHECK(lt_ext_has_target(app, "assistant"));
    LT_CHECK(lt_ext_n_settings_pages(app) >= 1);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(parse_content_length);
    LT_RUN(parse_chunked);
    LT_RUN(parse_until_close_and_errors);
    LT_RUN(parse_stream_callback_and_request);
    LT_RUN(url_parse);
    LT_RUN(http_client_chunked_partial);
    LT_RUN(http_client_timeout_and_refused);
    LT_RUN(tools_read);
    LT_RUN(tools_tiers_and_proposals);
    LT_RUN(chat_streaming_tool_loop);
    LT_RUN(chat_plain_json_and_errors);
    LT_RUN(chat_cancel_and_proposals_flow);
    LT_RUN(request_json_shape);
    LT_RUN(mcp_jsonrpc);
    LT_RUN(mcp_http_transport);
    LT_RUN(config_and_token);
    LT_RUN(registered_in_shell);
    LT_TEST_MAIN_END
}
