/* mcp.c - see mcp.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcp.h"
#include "../net/http.h"

#define MCP_VERSION "2025-03-26"
#define MCP_MAX_CLIENTS 8

static const char instructions[] =
    "LibreTune's read-only tune inspection tools. All numeric analysis must come from these "
    "tools: never invent or compute tuning numbers yourself. You can NOT change the tune or "
    "burn to the ECU; no write tools are offered here. If a tool returns an object with an "
    "`error` field, report that reason instead of guessing.";

static lt_json *err_obj(int code, const char *msg)
{
    lt_json *e = lt_json_obj();
    lt_json_obj_set(e, "code", lt_json_num((double)code));
    lt_json_obj_set(e, "message", lt_json_str(msg));
    return e;
}

/* copy of the request id (number, string or null) */
static lt_json *copy_id(const lt_json *id)
{
    if (id != NULL && id->type == LT_JSON_NUM) { return lt_json_num(id->num); }
    if (id != NULL && id->type == LT_JSON_STR) { return lt_json_str(id->str); }
    return lt_json_null();
}

static lt_json *reply(const lt_json *id, lt_json *result, lt_json *error)
{
    lt_json *r = lt_json_obj();
    lt_json_obj_set(r, "jsonrpc", lt_json_str("2.0"));
    lt_json_obj_set(r, "id", copy_id(id));
    if (error != NULL) { lt_json_obj_set(r, "error", error); }
    else { lt_json_obj_set(r, "result", result); }
    return r;
}

static lt_json *do_call(const lt_json *params, lt_mcp_env_fn envf, void *ud)
{
    const char *name = lt_json_get_str(params, "name", "");
    const lt_json *args = lt_json_get(params, "arguments");
    lt_ag_env env;
    lt_buf out;
    lt_json *res = lt_json_obj(), *content = lt_json_arr(), *item = lt_json_obj(), *parsed;
    int rc;
    char perr[32];
    memset(&env, 0, sizeof(env));
    lt_buf_init(&out);
    if (envf == NULL || envf(ud, &env) != 0) { env.ini = NULL; }
    rc = lt_ag_tool_run(&env, LT_AG_TIER_READ, name, args, NULL, &out);
    parsed = (out.data != NULL) ? lt_json_parse(out.data, out.len, perr, sizeof(perr)) : NULL;
    if (rc != 0 && parsed != NULL) {
        /* MCP reports tool failures as data with isError, the message as plain text */
        const char *m = lt_json_get_str(parsed, "error", "tool failed");
        lt_json_obj_set(item, "type", lt_json_str("text"));
        lt_json_obj_set(item, "text", lt_json_str(m));
        lt_json_free(parsed);
        parsed = NULL;
    } else {
        lt_json_obj_set(item, "type", lt_json_str("text"));
        lt_json_obj_set(item, "text", lt_json_str(out.data != NULL ? out.data : "{}"));
        if (parsed != NULL && parsed->type == LT_JSON_OBJ) { lt_json_obj_set(res, "structuredContent", parsed); }
        else { lt_json_free(parsed); }
    }
    lt_json_arr_push(content, item);
    lt_json_obj_set(res, "content", content);
    lt_json_obj_set(res, "isError", lt_json_bool(rc != 0));
    lt_buf_free(&out);
    return res;
}

/* one request object -> response object, NULL for notifications */
static lt_json *handle_one(const lt_json *req, lt_mcp_env_fn envf, void *ud)
{
    const lt_json *id = lt_json_get(req, "id"), *params = lt_json_get(req, "params");
    const char *method;
    if (req->type != LT_JSON_OBJ || lt_json_get(req, "method") == NULL ||
        lt_json_get(req, "method")->type != LT_JSON_STR) {
        return reply(NULL, NULL, err_obj(-32600, "invalid request"));
    }
    method = lt_json_get_str(req, "method", "");
    if (id == NULL) { return NULL; }                  /* notification: no answer */
    if (strcmp(method, "initialize") == 0) {
        lt_json *r = lt_json_obj(), *caps = lt_json_obj(), *si = lt_json_obj();
        const char *pv = lt_json_get_str(params, "protocolVersion", MCP_VERSION);
        lt_json_obj_set(caps, "tools", lt_json_obj());
        lt_json_obj_set(si, "name", lt_json_str("libretune"));
        lt_json_obj_set(si, "version", lt_json_str("0.1.0"));
        lt_json_obj_set(r, "protocolVersion", lt_json_str(pv));
        lt_json_obj_set(r, "capabilities", caps);
        lt_json_obj_set(r, "serverInfo", si);
        lt_json_obj_set(r, "instructions", lt_json_str(instructions));
        return reply(id, r, NULL);
    }
    if (strcmp(method, "ping") == 0) { return reply(id, lt_json_obj(), NULL); }
    if (strcmp(method, "tools/list") == 0) {
        lt_json *r = lt_json_obj();
        lt_json_obj_set(r, "tools", lt_ag_catalogue(LT_AG_TIER_READ, 1, 0));
        return reply(id, r, NULL);
    }
    if (strcmp(method, "tools/call") == 0) {
        if (params == NULL || params->type != LT_JSON_OBJ || lt_json_get_str(params, "name", "")[0] == '\0') {
            return reply(id, NULL, err_obj(-32602, "tools/call needs params.name"));
        }
        return reply(id, do_call(params, envf, ud), NULL);
    }
    return reply(id, NULL, err_obj(-32601, "method not found"));
}

char *lt_mcp_handle(const char *req, size_t len, lt_mcp_env_fn envf, void *ud)
{
    lt_json *j, *resp;
    char perr[64], *text;
    j = lt_json_parse(req, len, perr, sizeof(perr));
    if (j == NULL) {
        resp = reply(NULL, NULL, err_obj(-32700, "parse error"));
    } else if (j->type == LT_JSON_ARR) {
        size_t i;
        resp = lt_json_arr();
        if (j->n == 0) {
            lt_json_free(resp);
            resp = reply(NULL, NULL, err_obj(-32600, "empty batch"));
        } else {
            for (i = 0; i < j->n; i++) {
                lt_json *one = handle_one(j->items[i], envf, ud);
                if (one != NULL) { lt_json_arr_push(resp, one); }
            }
            if (resp->n == 0) { lt_json_free(resp); resp = NULL; }
        }
    } else {
        resp = handle_one(j, envf, ud);
    }
    lt_json_free(j);
    if (resp == NULL) { return NULL; }
    text = lt_json_to_string(resp, 0);
    lt_json_free(resp);
    return text;
}

/* ---- stdio ---- */
int lt_mcp_stdio(lt_mcp_env_fn envf, void *ud)
{
    lt_buf line;
    int c;
    lt_buf_init(&line);
    for (;;) {
        c = getc(stdin);
        if (c == '\n' || c == EOF) {
            if (line.len > 0) {
                char *r = lt_mcp_handle(line.data, line.len, envf, ud);
                if (r != NULL) {
                    fputs(r, stdout);
                    fputc('\n', stdout);
                    fflush(stdout);
                    lt_free(r);
                }
            }
            lt_buf_clear(&line);
            if (c == EOF) { break; }
        } else if (line.len < (size_t)LT_HTTP_MAX_BODY) {
            lt_buf_putc(&line, c);
        }
    }
    lt_buf_free(&line);
    return 0;
}

/* ---- HTTP transport ---- */
typedef struct mcp_client {
    lt_stream *s;
    lt_hparse  p;
    double     born;
} mcp_client;

struct lt_mcp_server {
    lt_server *srv;
    char       auth_tok[80];
    lt_mcp_env_fn envf;
    void      *ud;
    mcp_client cl[MCP_MAX_CLIENTS];
};

lt_mcp_server *lt_mcp_server_start(int port, const char *token, lt_mcp_env_fn envf, void *ud)
{
    lt_mcp_server *s;
    lt_server *srv;
    if (token == NULL || strlen(token) < 16 || strlen(token) >= 80) {
        lt_err_set("the MCP token is missing or malformed");
        return NULL;
    }
    srv = lt_server_listen(port);
    if (srv == NULL) {
        lt_err_set("cannot listen on 127.0.0.1:%d (is the port in use?)", port);
        return NULL;
    }
    s = (lt_mcp_server *)lt_calloc(1, sizeof(*s));
    s->srv = srv;
    lt_strlcpy(s->auth_tok, token, sizeof(s->auth_tok));
    s->envf = envf;
    s->ud = ud;
    return s;
}

int lt_mcp_server_port(const lt_mcp_server *s) { return lt_server_port(s->srv); }

static void drop(mcp_client *c)
{
    if (c->s == NULL) { return; }
    lt_stream_close(c->s);
    lt_hparse_free(&c->p);
    c->s = NULL;
}

/* constant-time comparison: no early exit on the first differing byte */
static int auth_match(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned diff;
    if (la != lb) { return 0; }
    diff = 0;
    for (i = 0; i < la; i++) { diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]); }
    return diff == 0;
}

/* "localhost", "127.0.0.1", "[::1]" with an optional :port */
static int loopback_host(const char *h)
{
    static const char *const ok[] = { "localhost", "127.0.0.1", "[::1]" };
    size_t i;
    for (i = 0; i < LT_COUNTOF(ok); i++) {
        size_t n = strlen(ok[i]);
        if (lt_strnicmp(h, ok[i], n) == 0 && (h[n] == '\0' || h[n] == ':')) { return 1; }
    }
    return 0;
}

static void respond(mcp_client *c, int code, const char *reason, const char *body)
{
    lt_buf b;
    size_t n = body != NULL ? strlen(body) : 0;
    lt_buf_init(&b);
    lt_buf_printf(&b, "HTTP/1.1 %d %s\r\nContent-Length: %lu\r\nConnection: close\r\n", code, reason,
                  (unsigned long)n);
    if (n > 0) { lt_buf_puts(&b, "Content-Type: application/json\r\n"); }
    lt_buf_puts(&b, "\r\n");
    if (n > 0) { lt_buf_append(&b, body, n); }
    (void)lt_stream_write(c->s, b.data, (int)b.len);
    lt_buf_free(&b);
}

static void serve(lt_mcp_server *s, mcp_client *c)
{
    char v[256], auth[128];
    if (!lt_hparse_header(&c->p, "host", v, sizeof(v)) || !loopback_host(v)) {
        respond(c, 403, "Forbidden", NULL);
        return;
    }
    if (lt_hparse_header(&c->p, "origin", v, sizeof(v))) {
        if (!lt_str_istarts(v, "http://") || !loopback_host(v + 7)) {
            respond(c, 403, "Forbidden", NULL);
            return;
        }
    }
    auth[0] = '\0';
    (void)lt_hparse_header(&c->p, "authorization", auth, sizeof(auth));
    if (!lt_str_starts(auth, "Bearer ") || !auth_match(auth + 7, s->auth_tok)) {
        respond(c, 401, "Unauthorized", NULL);       /* bare 401: nothing is echoed back */
        return;
    }
    if (strcmp(c->p.target, "/mcp") != 0) { respond(c, 404, "Not Found", NULL); return; }
    if (strcmp(c->p.method, "POST") != 0) { respond(c, 405, "Method Not Allowed", NULL); return; }
    {
        char *r = lt_mcp_handle(c->p.body.data != NULL ? c->p.body.data : "", c->p.body.len, s->envf, s->ud);
        if (r == NULL) { respond(c, 202, "Accepted", NULL); }
        else { respond(c, 200, "OK", r); lt_free(r); }
    }
}

void lt_mcp_server_poll(lt_mcp_server *s)
{
    int i;
    lt_stream *n;
    while ((n = lt_server_accept(s->srv, 0)) != NULL) {
        for (i = 0; i < MCP_MAX_CLIENTS && s->cl[i].s != NULL; i++) { }
        if (i == MCP_MAX_CLIENTS) { lt_stream_close(n); continue; }
        s->cl[i].s = n;
        s->cl[i].born = lt_plat_now_ms();
        lt_hparse_init(&s->cl[i].p, 1);
    }
    for (i = 0; i < MCP_MAX_CLIENTS; i++) {
        mcp_client *c = &s->cl[i];
        char buf[4096];
        int k, r = 0;
        if (c->s == NULL) { continue; }
        for (k = 0; k < 32 && r == 0; k++) {
            int got = lt_stream_read(c->s, buf, (int)sizeof(buf), 0);
            if (got < 0) { r = -1; break; }
            if (got == 0) { break; }
            r = lt_hparse_feed(&c->p, buf, (size_t)got);
        }
        if (r > 0) { serve(s, c); drop(c); }
        else if (r < 0) { respond(c, 400, "Bad Request", NULL); drop(c); }
        else if (lt_plat_now_ms() - c->born > 10000.0) { drop(c); }
    }
}

void lt_mcp_server_stop(lt_mcp_server *s)
{
    int i;
    if (s == NULL) { return; }
    for (i = 0; i < MCP_MAX_CLIENTS; i++) { drop(&s->cl[i]); }
    lt_server_close(s->srv);
    lt_free(s);
}
