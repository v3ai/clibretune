/* agent_chat.c - OpenAI-compatible chat completions client with the tool-calling loop.
 * One request is in flight at a time; lt_ag_chat_step is called every frame and never blocks.
 * Responses are parsed incrementally (server-sent events) or, when the server ignores
 * "stream", as one JSON document.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "agent.h"

#define CHAT_TIMEOUT_MS 120000

static const char sys_prompt[] =
    "You are a tuning assistant inside LibreTune, an ECU tuning application. All numeric "
    "analysis must come from the tools: never invent or compute tuning numbers yourself. "
    "Call list_tables first to discover table names, then read what you need. You can only "
    "PROPOSE changes with the propose_* tools; the user reviews and applies them, and "
    "nothing is burned to the ECU. Say plainly when a tool returns an error. Keep answers "
    "short and concrete.";

const char *lt_ag_system_prompt(void) { return sys_prompt; }

void lt_ag_chat_init(lt_ag_chat *c)
{
    int i;
    memset(c, 0, sizeof(*c));
    c->msgs = lt_json_arr();
    lt_buf_init(&c->reply);
    lt_buf_init(&c->raw);
    lt_buf_init(&c->sse_line);
    for (i = 0; i < LT_AG_MAX_CALLS; i++) { lt_buf_init(&c->calls[i].args); }
    lt_ag_props_init(&c->props);
}

static void clear_calls(lt_ag_chat *c)
{
    int i;
    for (i = 0; i < LT_AG_MAX_CALLS; i++) {
        c->calls[i].id[0] = '\0';
        c->calls[i].name[0] = '\0';
        lt_buf_clear(&c->calls[i].args);
    }
    c->n_calls = 0;
}

void lt_ag_chat_free(lt_ag_chat *c)
{
    int i;
    lt_http_free(c->http);
    lt_json_free(c->msgs);
    lt_buf_free(&c->reply);
    lt_buf_free(&c->raw);
    lt_buf_free(&c->sse_line);
    for (i = 0; i < LT_AG_MAX_CALLS; i++) { lt_buf_free(&c->calls[i].args); }
    lt_ag_props_free(&c->props);
    memset(c, 0, sizeof(*c));
}

void lt_ag_chat_reset(lt_ag_chat *c)
{
    lt_http_free(c->http);
    c->http = NULL;
    lt_json_free(c->msgs);
    c->msgs = lt_json_arr();
    c->busy = 0;
    c->error[0] = '\0';
    c->status[0] = '\0';
    c->n_tok = 0;
    lt_buf_clear(&c->reply);
    clear_calls(c);
    lt_ag_props_clear(&c->props);
    c->changes++;
}

static void add_msg(lt_ag_chat *c, const char *role, const char *text)
{
    lt_json *m = lt_json_obj();
    lt_json_obj_set(m, "role", lt_json_str(role));
    lt_json_obj_set(m, "content", lt_json_str(text));
    lt_json_arr_push(c->msgs, m);
    c->changes++;
}

void lt_ag_chat_cancel(lt_ag_chat *c)
{
    if (!c->busy) { return; }
    lt_http_free(c->http);
    c->http = NULL;
    c->busy = 0;
    c->cancelled = 1;
    c->status[0] = '\0';
    if (c->reply.len > 0) { add_msg(c, "assistant", c->reply.data); }
    add_msg(c, "assistant", "(stopped)");
    lt_buf_clear(&c->reply);
    clear_calls(c);
}

/* ---- request ---- */
char *lt_ag_chat_request(const lt_ag_chat *c, const lt_ag_cfg *cfg, int stream)
{
    lt_buf b;
    lt_json *sys = lt_json_obj(), *tools;
    char prompt[1024];
    size_t i;
    lt_buf_init(&b);
    lt_snprintf(prompt, sizeof(prompt), "%s%s%s", sys_prompt, c->context[0] != '\0' ? "\n" : "",
                c->context);
    lt_json_obj_set(sys, "role", lt_json_str("system"));
    lt_json_obj_set(sys, "content", lt_json_str(prompt));
    lt_buf_puts(&b, "{\"model\":");
    lt_json_put_string(&b, cfg->model);
    lt_buf_printf(&b, ",\"stream\":%s,\"messages\":[", stream ? "true" : "false");
    lt_json_write(sys, 0, &b);
    lt_json_free(sys);
    for (i = 0; i < c->msgs->n; i++) {
        lt_buf_putc(&b, ',');
        lt_json_write(c->msgs->items[i], 0, &b);
    }
    lt_buf_putc(&b, ']');
    tools = lt_ag_catalogue(cfg->tier, 0, 1);
    if (tools->n > 0) {
        lt_buf_puts(&b, ",\"tools\":");
        lt_json_write(tools, 0, &b);
    }
    lt_json_free(tools);
    lt_buf_putc(&b, '}');
    return lt_buf_take(&b);
}

/* ---- response parsing ---- */
static void note_tokens(lt_ag_chat *c, const lt_json *root)
{
    const lt_json *u = lt_json_get(root, "usage");
    if (u != NULL) {
        double t = lt_json_get_num(u, "total_tokens", 0.0);
        if (t > 0.0) { c->n_tok += (long)t; }
    }
}

static void take_call(lt_ag_chat *c, const lt_json *tc, int whole)
{
    const lt_json *f = lt_json_get(tc, "function");
    int idx = (int)lt_json_get_num(tc, "index", (double)c->n_calls);
    const char *s;
    lt_ag_call *k;
    if (whole) { idx = c->n_calls; }
    if (idx < 0 || idx >= LT_AG_MAX_CALLS) { return; }
    k = &c->calls[idx];
    if (idx >= c->n_calls) { c->n_calls = idx + 1; }
    s = lt_json_get_str(tc, "id", "");
    if (s[0] != '\0') { lt_strlcpy(k->id, s, sizeof(k->id)); }
    if (f != NULL) {
        s = lt_json_get_str(f, "name", "");
        if (s[0] != '\0') {
            if (whole || k->name[0] == '\0') { lt_strlcpy(k->name, s, sizeof(k->name)); }
            else { lt_strlcat(k->name, s, sizeof(k->name)); }
        }
        s = lt_json_get_str(f, "arguments", "");
        lt_buf_puts(&k->args, s);
    }
}

static void take_error(lt_ag_chat *c, const lt_json *root)
{
    const lt_json *e = lt_json_get(root, "error");
    if (e == NULL) { return; }
    if (e->type == LT_JSON_OBJ) { lt_strlcpy(c->error, lt_json_get_str(e, "message", "the server reported an error"), sizeof(c->error)); }
    else if (e->type == LT_JSON_STR) { lt_strlcpy(c->error, e->str, sizeof(c->error)); }
}

/* one streamed chunk or one whole completion */
static void take_choice(lt_ag_chat *c, const lt_json *root)
{
    const lt_json *ch = lt_json_at(lt_json_get(root, "choices"), 0), *d;
    const lt_json *tcs;
    size_t i;
    note_tokens(c, root);
    take_error(c, root);
    if (ch == NULL) { return; }
    d = lt_json_get(ch, "delta");
    if (d != NULL) {
        const char *t = lt_json_get_str(d, "content", "");
        if (t[0] != '\0') { lt_buf_puts(&c->reply, t); c->changes++; }
        tcs = lt_json_get(d, "tool_calls");
        for (i = 0; tcs != NULL && i < tcs->n; i++) { take_call(c, lt_json_at(tcs, i), 0); }
    }
    d = lt_json_get(ch, "message");
    if (d != NULL) {
        const char *t = lt_json_get_str(d, "content", "");
        if (t[0] != '\0') { lt_buf_puts(&c->reply, t); c->changes++; }
        tcs = lt_json_get(d, "tool_calls");
        for (i = 0; tcs != NULL && i < tcs->n; i++) { take_call(c, lt_json_at(tcs, i), 1); }
    }
}

static void sse_line(lt_ag_chat *c, char *l)
{
    lt_json *j;
    char err[64];
    if (!lt_str_starts(l, "data:")) { return; }
    l += 5;
    while (*l == ' ') { l++; }
    if (l[0] == '\0' || strcmp(l, "[DONE]") == 0) { return; }
    j = lt_json_parse(l, strlen(l), err, sizeof(err));
    if (j == NULL) { return; }
    take_choice(c, j);
    lt_json_free(j);
}

void lt_ag_chat_feed(lt_ag_chat *c, const char *d, size_t n)
{
    size_t i;
    if (!c->sse) { lt_buf_append(&c->raw, d, n); return; }
    for (i = 0; i < n; i++) {
        if (d[i] == '\n') {
            if (c->sse_line.len > 0 && c->sse_line.data[c->sse_line.len - 1] == '\r') { c->sse_line.len--; }
            if (c->sse_line.data != NULL) {
                c->sse_line.data[c->sse_line.len] = '\0';
                sse_line(c, c->sse_line.data);
            }
            lt_buf_clear(&c->sse_line);
        } else if (c->sse_line.len < 1048576UL) {
            lt_buf_putc(&c->sse_line, d[i]);
        }
    }
}

int lt_ag_chat_finish_body(lt_ag_chat *c)
{
    lt_json *j;
    char err[64];
    if (c->sse) {
        if (c->sse_line.len > 0) {
            c->sse_line.data[c->sse_line.len] = '\0';
            sse_line(c, c->sse_line.data);
            lt_buf_clear(&c->sse_line);
        }
        return 0;
    }
    if (c->raw.len == 0) { lt_strlcpy(c->error, "empty response from the server", sizeof(c->error)); return -1; }
    j = lt_json_parse(c->raw.data, c->raw.len, err, sizeof(err));
    if (j == NULL) { lt_strlcpy(c->error, "the server did not answer with JSON", sizeof(c->error)); return -1; }
    take_choice(c, j);
    lt_json_free(j);
    return c->error[0] != '\0' ? -1 : 0;
}

/* streamed body bytes from lt_http */
static void body_cb(void *ud, const char *d, size_t n)
{
    lt_ag_chat *c = (lt_ag_chat *)ud;
    if (!c->sse && c->raw.len == 0 && c->sse_line.len == 0 && c->http != NULL) {
        char ct[80];
        if (lt_http_status(c->http) == 200 && lt_http_header(c->http, "content-type", ct, sizeof(ct)) &&
            strstr(ct, "event-stream") != NULL) {
            c->sse = 1;
        }
    }
    lt_ag_chat_feed(c, d, n);
}

static int start_request(lt_ag_chat *c, const lt_ag_cfg *cfg)
{
    char url[300], key[200], *body;
    lt_buf hdr;
    size_t n;
    n = strlen(cfg->base_url);
    while (n > 0 && cfg->base_url[n - 1] == '/') { n--; }
    lt_snprintf(url, sizeof(url), "%.*s/chat/completions", (int)n, cfg->base_url);
    lt_buf_init(&hdr);
    lt_buf_puts(&hdr, "Content-Type: application/json\r\nAccept: text/event-stream, application/json\r\n");
    if (lt_ag_cfg_key(cfg, key, sizeof(key)) > 0) {
        if (strchr(key, '\r') != NULL || strchr(key, '\n') != NULL) {
            lt_buf_free(&hdr);
            lt_strlcpy(c->error, "the API key contains a line break", sizeof(c->error));
            return -1;
        }
        lt_buf_printf(&hdr, "Authorization: Bearer %s\r\n", key);
    }
    body = lt_ag_chat_request(c, cfg, 1);
    lt_buf_clear(&c->reply);
    lt_buf_clear(&c->raw);
    lt_buf_clear(&c->sse_line);
    clear_calls(c);
    c->sse = 0;
    c->http = lt_http_start("POST", url, hdr.data, body, strlen(body), CHAT_TIMEOUT_MS, body_cb, c);
    lt_free(body);
    lt_buf_free(&hdr);
    if (c->http == NULL) {
        lt_strlcpy(c->error, lt_err_get(), sizeof(c->error));
        return -1;
    }
    c->busy = 1;
    lt_snprintf(c->status, sizeof(c->status), "thinking (round %d)...", c->round + 1);
    return 0;
}

int lt_ag_chat_send(lt_ag_chat *c, const lt_ag_cfg *cfg, const char *text)
{
    c->error[0] = '\0';
    c->cancelled = 0;
    if (c->busy) { lt_strlcpy(c->error, "a request is already running", sizeof(c->error)); return -1; }
    if (!cfg->enabled || !cfg->ack) {
        lt_strlcpy(c->error, "the assistant is not enabled (Settings > Assistant)", sizeof(c->error));
        return -1;
    }
    add_msg(c, "user", text);
    c->round = 0;
    return start_request(c, cfg);
}

/* ---- finishing a round ---- */
static void push_assistant(lt_ag_chat *c)
{
    lt_json *m = lt_json_obj();
    int i;
    lt_json_obj_set(m, "role", lt_json_str("assistant"));
    lt_json_obj_set(m, "content", c->reply.len > 0 ? lt_json_str(c->reply.data) : lt_json_null());
    if (c->n_calls > 0) {
        lt_json *arr = lt_json_arr();
        for (i = 0; i < c->n_calls; i++) {
            lt_json *tc = lt_json_obj(), *f = lt_json_obj();
            lt_ag_call *k = &c->calls[i];
            if (k->id[0] == '\0') { lt_snprintf(k->id, sizeof(k->id), "call_%d", i); }
            lt_json_obj_set(f, "name", lt_json_str(k->name));
            lt_json_obj_set(f, "arguments", lt_json_str(k->args.len > 0 ? k->args.data : "{}"));
            lt_json_obj_set(tc, "id", lt_json_str(k->id));
            lt_json_obj_set(tc, "type", lt_json_str("function"));
            lt_json_obj_set(tc, "function", f);
            lt_json_arr_push(arr, tc);
        }
        lt_json_obj_set(m, "tool_calls", arr);
    }
    lt_json_arr_push(c->msgs, m);
    c->changes++;
}

static void run_calls(lt_ag_chat *c, const lt_ag_cfg *cfg, const lt_ag_env *env)
{
    int i;
    for (i = 0; i < c->n_calls; i++) {
        lt_ag_call *k = &c->calls[i];
        lt_json *args = NULL, *m;
        lt_buf out;
        char err[64];
        lt_buf_init(&out);
        if (k->args.len > 0) { args = lt_json_parse(k->args.data, k->args.len, err, sizeof(err)); }
        lt_snprintf(c->status, sizeof(c->status), "running %s...", k->name);
        if (k->args.len > 0 && args == NULL) {
            lt_buf_puts(&out, "{\"error\":\"the arguments are not valid JSON\"}");
        } else {
            (void)lt_ag_tool_run(env, cfg->tier, k->name, args, &c->props, &out);
        }
        lt_json_free(args);
        m = lt_json_obj();
        lt_json_obj_set(m, "role", lt_json_str("tool"));
        lt_json_obj_set(m, "tool_call_id", lt_json_str(k->id));
        lt_json_obj_set(m, "content", lt_json_str(out.data != NULL ? out.data : "{}"));
        lt_json_arr_push(c->msgs, m);
        lt_buf_free(&out);
    }
    c->changes++;
}

static void fail_chat(lt_ag_chat *c, const char *msg)
{
    lt_http_free(c->http);
    c->http = NULL;
    c->busy = 0;
    c->status[0] = '\0';
    if (msg != NULL && c->error[0] == '\0') { lt_strlcpy(c->error, msg, sizeof(c->error)); }
    if (c->reply.len > 0) { add_msg(c, "assistant", c->reply.data); lt_buf_clear(&c->reply); }
    clear_calls(c);
    c->changes++;
}

int lt_ag_chat_step(lt_ag_chat *c, const lt_ag_cfg *cfg, const lt_ag_env *env)
{
    int r, status;
    unsigned long before = c->changes;
    if (!c->busy || c->http == NULL) { return 0; }
    r = lt_http_step(c->http);
    if (r == 0) { return c->changes != before; }
    if (r < 0) {
        char msg[200];
        lt_snprintf(msg, sizeof(msg), "%s", lt_http_error(c->http));
        fail_chat(c, msg);
        return 1;
    }
    status = lt_http_status(c->http);
    lt_http_free(c->http);
    c->http = NULL;
    if (status != 200) {
        char msg[200];
        lt_json *j;
        char perr[32];
        msg[0] = '\0';
        j = c->raw.len > 0 ? lt_json_parse(c->raw.data, c->raw.len, perr, sizeof(perr)) : NULL;
        if (j != NULL) { take_error(c, j); lt_json_free(j); }
        if (c->error[0] == '\0') {
            lt_snprintf(msg, sizeof(msg), "HTTP %d from the server%s", status,
                        status == 401 ? " (check the API key)" : (status == 404 ? " (check the base URL and model)" : ""));
            lt_strlcpy(c->error, msg, sizeof(c->error));
        } else {
            lt_snprintf(msg, sizeof(msg), "HTTP %d: %s", status, c->error);
            lt_strlcpy(c->error, msg, sizeof(c->error));
        }
        fail_chat(c, NULL);
        return 1;
    }
    if (lt_ag_chat_finish_body(c) != 0) { fail_chat(c, NULL); return 1; }
    if (c->error[0] != '\0') { fail_chat(c, NULL); return 1; }
    /* drop calls without a name (malformed deltas) */
    {
        int i, k = 0;
        for (i = 0; i < c->n_calls; i++) {
            if (c->calls[i].name[0] != '\0') {
                if (k != i) {
                    lt_ag_call tmp = c->calls[k];
                    c->calls[k] = c->calls[i];
                    c->calls[i] = tmp;
                }
                k++;
            }
        }
        c->n_calls = k;
    }
    if (c->n_calls == 0 && c->reply.len == 0) {
        fail_chat(c, "the model returned an empty answer (does it support tool calling?)");
        return 1;
    }
    push_assistant(c);
    if (c->n_calls == 0) {
        c->busy = 0;
        c->status[0] = '\0';
        lt_buf_clear(&c->reply);
        return 1;
    }
    run_calls(c, cfg, env);
    lt_buf_clear(&c->reply);
    c->round++;
    clear_calls(c);
    if (c->round >= LT_AG_MAX_ROUNDS) {
        c->busy = 0;
        c->status[0] = '\0';
        lt_strlcpy(c->error, "stopped after too many tool rounds", sizeof(c->error));
        return 1;
    }
    if (start_request(c, cfg) != 0) { c->busy = 0; c->status[0] = '\0'; }
    return 1;
}
