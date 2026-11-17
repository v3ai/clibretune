/* http.c - see http.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "http.h"

enum { HS_FIRST = 0, HS_HEAD, HS_BODY_LEN, HS_BODY_EOF, HS_CSIZE, HS_CDATA, HS_CCRLF,
       HS_TRAILER, HS_DONE, HS_ERR };

void lt_hparse_init(lt_hparse *p, int is_request)
{
    memset(p, 0, sizeof(*p));
    p->is_request = is_request;
    p->content_len = -1;
    lt_buf_init(&p->head);
    lt_buf_init(&p->line);
    lt_buf_init(&p->body);
}

void lt_hparse_free(lt_hparse *p)
{
    lt_buf_free(&p->head);
    lt_buf_free(&p->line);
    lt_buf_free(&p->body);
}

void lt_hparse_set_callback(lt_hparse *p, lt_http_body_fn cb, void *ud)
{
    p->cb = cb;
    p->cb_ud = ud;
}

static int fail(lt_hparse *p, const char *msg)
{
    p->state = HS_ERR;
    lt_strlcpy(p->err, msg, sizeof(p->err));
    return -1;
}

static void emit(lt_hparse *p, const char *d, size_t n)
{
    if (n == 0) { return; }
    p->got += (long)n;
    if (p->cb != NULL) { p->cb(p->cb_ud, d, n); }
    else { lt_buf_append(&p->body, d, n); }
}

lt_bool lt_hparse_header(const lt_hparse *p, const char *name, char *out, size_t cap)
{
    const char *s = p->head.data, *e;
    size_t nl = strlen(name);
    if (s == NULL) { return LT_FALSE; }
    while (*s != '\0') {
        e = strchr(s, '\n');
        if (e == NULL) { e = s + strlen(s); }
        if ((size_t)(e - s) > nl && s[nl] == ':' && lt_strnicmp(s, name, nl) == 0) {
            const char *v = s + nl + 1;
            size_t n;
            while (v < e && (*v == ' ' || *v == '\t')) { v++; }
            n = (size_t)(e - v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == ' ')) { n--; }
            if (cap > 0) {
                if (n >= cap) { n = cap - 1; }
                memcpy(out, v, n);
                out[n] = '\0';
            }
            return LT_TRUE;
        }
        s = (*e == '\n') ? e + 1 : e;
    }
    return LT_FALSE;
}

int lt_hparse_headers_done(const lt_hparse *p)
{
    return p->state > HS_HEAD;
}

/* first line: "HTTP/1.1 200 OK" or "POST /x HTTP/1.1" */
static int parse_first(lt_hparse *p, char *l)
{
    if (!p->is_request) {
        char *sp;
        if (strncmp(l, "HTTP/1.", 7) != 0) { return fail(p, "not an HTTP response"); }
        sp = strchr(l, ' ');
        if (sp == NULL) { return fail(p, "bad status line"); }
        p->status = atoi(sp + 1);
        if (p->status < 100 || p->status > 599) { return fail(p, "bad status code"); }
    } else {
        char *a = strchr(l, ' '), *b;
        if (a == NULL) { return fail(p, "bad request line"); }
        *a++ = '\0';
        b = strchr(a, ' ');
        if (b == NULL) { return fail(p, "bad request line"); }
        *b = '\0';
        if (strlen(l) >= sizeof(p->method) || strlen(a) >= sizeof(p->target)) {
            return fail(p, "request line too long");
        }
        lt_strlcpy(p->method, l, sizeof(p->method));
        lt_strlcpy(p->target, a, sizeof(p->target));
    }
    return 0;
}

/* end of the header block: choose the body framing; 1 = message complete */
static int head_done(lt_hparse *p)
{
    char v[64];
    p->chunked = 0;
    if (lt_hparse_header(p, "transfer-encoding", v, sizeof(v)) && strstr(v, "chunked") != NULL) {
        p->chunked = 1;
    }
    if (lt_hparse_header(p, "content-length", v, sizeof(v))) {
        long n = 0;
        if (!lt_parse_long(v, &n) || n < 0) { return fail(p, "bad Content-Length"); }
        if (n > LT_HTTP_MAX_BODY) { return fail(p, "body too large"); }
        p->content_len = n;
    }
    if (!p->is_request && (p->status < 200 || p->status == 204 || p->status == 304)) {
        p->state = HS_DONE;
        return 1;
    }
    if (p->chunked) { p->state = HS_CSIZE; }
    else if (p->content_len == 0) { p->state = HS_DONE; return 1; }
    else if (p->content_len > 0) { p->state = HS_BODY_LEN; }
    else if (p->is_request) { p->state = HS_DONE; return 1; }
    else { p->state = HS_BODY_EOF; }
    return 0;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* a complete line (CR/LF stripped) in p->line; returns 0 continue, 1 done, -1 error */
static int on_line(lt_hparse *p)
{
    const char *q = p->line.data != NULL ? p->line.data : "";
    switch (p->state) {
    case HS_FIRST:
        if (p->line.len == 0) { return 0; }       /* tolerate leading blank lines */
        if (parse_first(p, p->line.data) != 0) { return -1; }
        p->state = HS_HEAD;
        return 0;
    case HS_HEAD:
        if (p->line.len == 0) { return head_done(p); }
        if (p->head.len + p->line.len > LT_HTTP_MAX_HEAD) { return fail(p, "headers too large"); }
        lt_buf_append(&p->head, p->line.data, p->line.len);
        lt_buf_putc(&p->head, '\n');
        return 0;
    case HS_CSIZE: {
        long sz = 0;
        size_t k = 0;
        int v;
        while (q[k] == ' ') { k++; }
        if (hex_val((unsigned char)q[k]) < 0) { return fail(p, "bad chunk size"); }
        while ((v = hex_val((unsigned char)q[k])) >= 0) {
            if (sz > (LT_HTTP_MAX_BODY >> 4)) { return fail(p, "chunk too large"); }
            sz = sz * 16 + v;
            k++;
        }
        if (q[k] != '\0' && q[k] != ';' && q[k] != ' ') { return fail(p, "bad chunk size"); }
        if (sz == 0) { p->state = HS_TRAILER; }
        else { p->chunk_left = sz; p->state = HS_CDATA; }
        return 0;
    }
    case HS_CCRLF:
        p->state = HS_CSIZE;
        return 0;
    case HS_TRAILER:
        if (p->line.len == 0) { p->state = HS_DONE; return 1; }
        return 0;
    default:
        return 0;
    }
}

int lt_hparse_feed(lt_hparse *p, const char *d, size_t n)
{
    size_t i = 0;
    if (p->state == HS_ERR) { return -1; }
    while (i < n) {
        char c;
        int r;
        if (p->state == HS_DONE) { return 1; }
        if (p->state == HS_BODY_LEN) {
            size_t take = n - i, need = (size_t)(p->content_len - p->got);
            if (take > need) { take = need; }
            emit(p, d + i, take);
            i += take;
            if (p->got >= p->content_len) { p->state = HS_DONE; return 1; }
            continue;
        }
        if (p->state == HS_BODY_EOF) {
            if (p->got + (long)(n - i) > LT_HTTP_MAX_BODY) { return fail(p, "body too large"); }
            emit(p, d + i, n - i);
            return 0;
        }
        if (p->state == HS_CDATA) {
            size_t take = n - i;
            if ((long)take > p->chunk_left) { take = (size_t)p->chunk_left; }
            if (p->got + (long)take > LT_HTTP_MAX_BODY) { return fail(p, "body too large"); }
            emit(p, d + i, take);
            i += take;
            p->chunk_left -= (long)take;
            if (p->chunk_left == 0) { p->state = HS_CCRLF; lt_buf_clear(&p->line); }
            continue;
        }
        c = d[i++];
        if (c != '\n') {
            if (p->line.len > LT_HTTP_MAX_HEAD) { return fail(p, "header line too long"); }
            lt_buf_putc(&p->line, c);
            continue;
        }
        if (p->line.len > 0 && p->line.data[p->line.len - 1] == '\r') { p->line.len--; }
        if (p->line.data != NULL) { p->line.data[p->line.len] = '\0'; }
        r = on_line(p);
        lt_buf_clear(&p->line);
        if (r != 0) { return r; }
    }
    return p->state == HS_DONE ? 1 : 0;
}

int lt_hparse_eof(lt_hparse *p)
{
    if (p->state == HS_DONE) { return 1; }
    if (p->state == HS_BODY_EOF) { p->state = HS_DONE; return 1; }
    if (p->state != HS_ERR) { (void)fail(p, "connection closed before the response was complete"); }
    return -1;
}

/* ---- URL ---- */
int lt_http_url_parse(const char *url, lt_http_url *u)
{
    const char *s = url, *h, *e, *path;
    size_t n;
    if (url == NULL) { lt_err_set("empty URL"); return -1; }
    while (*s == ' ') { s++; }
    if (lt_str_istarts(s, "https://")) {
        lt_err_set("https:// is not supported (no TLS). Use a local endpoint such as "
                   "http://127.0.0.1:11434/v1 or a local proxy that terminates TLS");
        return -1;
    }
    if (!lt_str_istarts(s, "http://")) { lt_err_set("URL must start with http://"); return -1; }
    h = s + 7;
    memset(u, 0, sizeof(*u));
    u->port = 80;
    if (*h == '[') {                      /* [::1]:port */
        e = strchr(h, ']');
        if (e == NULL) { lt_err_set("bad IPv6 host in URL"); return -1; }
        n = (size_t)(e - h - 1);
        if (n == 0 || n >= sizeof(u->host)) { lt_err_set("bad host in URL"); return -1; }
        memcpy(u->host, h + 1, n);
        u->host[n] = '\0';
        e++;
    } else {
        e = h;
        while (*e != '\0' && *e != ':' && *e != '/' && *e != '?') { e++; }
        n = (size_t)(e - h);
        if (n == 0 || n >= sizeof(u->host)) { lt_err_set("bad host in URL"); return -1; }
        memcpy(u->host, h, n);
        u->host[n] = '\0';
    }
    if (*e == ':') {
        long pv = 0;
        char pb[8];
        size_t k = 0;
        e++;
        while (*e >= '0' && *e <= '9' && k < sizeof(pb) - 1) { pb[k++] = *e++; }
        pb[k] = '\0';
        if (k == 0 || !lt_parse_long(pb, &pv) || pv < 1 || pv > 65535) {
            lt_err_set("bad port in URL");
            return -1;
        }
        u->port = (int)pv;
    }
    path = e;
    if (*path == '\0') { lt_strlcpy(u->path, "/", sizeof(u->path)); }
    else if (*path == '?') { lt_snprintf(u->path, sizeof(u->path), "/%s", path); }
    else { lt_strlcpy(u->path, path, sizeof(u->path)); }
    return 0;
}

/* ---- client ---- */
struct lt_http {
    lt_stream *s;
    lt_hparse  p;
    int        timeout_ms;
    double     last_rx;
    int        done;               /* 0 running, 1 ok, -1 failed */
    char       err[160];
};

lt_http *lt_http_start(const char *method, const char *url, const char *headers,
                       const char *body, size_t body_len, int timeout_ms,
                       lt_http_body_fn cb, void *cb_ud)
{
    lt_http_url u;
    lt_http *h;
    lt_buf req;
    char hostport[160];
    if (lt_http_url_parse(url, &u) != 0) { return NULL; }
    h = (lt_http *)lt_calloc(1, sizeof(*h));
    h->s = lt_tcp_connect(u.host, u.port, (timeout_ms > 0 && timeout_ms < 3000) ? timeout_ms : 3000);
    if (h->s == NULL) { lt_free(h); return NULL; }
    lt_hparse_init(&h->p, 0);
    lt_hparse_set_callback(&h->p, cb, cb_ud);
    h->timeout_ms = timeout_ms > 0 ? timeout_ms : 5000;
    h->last_rx = lt_plat_now_ms();
    if (strchr(u.host, ':') != NULL) { lt_snprintf(hostport, sizeof(hostport), "[%s]:%d", u.host, u.port); }
    else { lt_snprintf(hostport, sizeof(hostport), "%s:%d", u.host, u.port); }
    lt_buf_init(&req);
    lt_buf_printf(&req, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                  "Accept-Encoding: identity\r\n", method, u.path, hostport);
    if (headers != NULL) { lt_buf_puts(&req, headers); }
    if (body != NULL) { lt_buf_printf(&req, "Content-Length: %lu\r\n", (unsigned long)body_len); }
    lt_buf_puts(&req, "\r\n");
    if (body != NULL && body_len > 0) { lt_buf_append(&req, body, body_len); }
    if (lt_stream_write(h->s, req.data, (int)req.len) < 0) {
        lt_err_set("cannot send the request to %s", hostport);
        lt_buf_free(&req);
        lt_http_free(h);
        return NULL;
    }
    lt_buf_free(&req);
    return h;
}

int lt_http_step(lt_http *h)
{
    int guard;
    if (h->done != 0) { return h->done; }
    for (guard = 0; guard < 64; guard++) {
        char buf[4096];
        int n = lt_stream_read(h->s, buf, (int)sizeof(buf), 0);
        int r;
        if (n < 0) {
            if (lt_hparse_eof(&h->p) == 1) { h->done = 1; return 1; }
            lt_strlcpy(h->err, h->p.err[0] != '\0' ? h->p.err : "connection lost", sizeof(h->err));
            h->done = -1;
            return -1;
        }
        if (n == 0) { break; }
        h->last_rx = lt_plat_now_ms();
        r = lt_hparse_feed(&h->p, buf, (size_t)n);
        if (r < 0) { lt_strlcpy(h->err, h->p.err, sizeof(h->err)); h->done = -1; return -1; }
        if (r > 0) { h->done = 1; return 1; }
    }
    if (lt_plat_now_ms() - h->last_rx > (double)h->timeout_ms) {
        lt_strlcpy(h->err, "timed out waiting for the server", sizeof(h->err));
        h->done = -1;
        return -1;
    }
    return 0;
}

int lt_http_status(const lt_http *h) { return h->p.status; }
const lt_buf *lt_http_body(const lt_http *h) { return &h->p.body; }
const char *lt_http_error(const lt_http *h) { return h->err; }

lt_bool lt_http_header(const lt_http *h, const char *name, char *out, size_t cap)
{
    return lt_hparse_header(&h->p, name, out, cap);
}

void lt_http_free(lt_http *h)
{
    if (h == NULL) { return; }
    if (h->s != NULL) { lt_stream_close(h->s); }
    lt_hparse_free(&h->p);
    lt_free(h);
}
