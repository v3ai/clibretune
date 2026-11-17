/* http.h - minimal HTTP/1.1 over plain TCP (no TLS is available, see UNIVERSAL_C.md).
 *
 * Layers:
 *   lt_hparse  incremental message parser (status/request line, headers, Content-Length,
 *              chunked, read-until-close), for responses and for requests.  Pure: fed with
 *              whatever bytes arrive, so partial reads are unit-testable.  Body bytes are
 *              stored, or streamed to a callback.
 *   lt_http    one client request on a lt_stream, stepped without blocking (lt_http_step is
 *              called once per UI frame and never waits).  Only the connect and the small
 *              request write block, bounded by the timeout.
 * https:// is refused with a message suggesting a local endpoint or proxy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_NET_HTTP_H
#define LT_NET_HTTP_H

#include "../lt.h"
#include "../plat/plat.h"

LT_BEGIN_DECLS

#define LT_HTTP_MAX_HEAD 16384
#define LT_HTTP_MAX_BODY (8L * 1024L * 1024L)

/* receives body bytes as they arrive (after chunk decoding) */
typedef void (*lt_http_body_fn)(void *ud, const char *data, size_t n);

/* ---- message parser ---- */
typedef struct lt_hparse {
    int     state;
    int     is_request;          /* 0 response, 1 request (no read-until-close body) */
    int     status;              /* response status code */
    char    method[12], target[512];   /* request line */
    long    content_len;         /* -1 = unknown */
    int     chunked;
    long    chunk_left;
    long    got;                 /* body bytes seen */
    lt_buf  head;                /* header lines, '\n' separated */
    lt_buf  line;                /* partial line accumulator */
    lt_buf  body;                /* body when no callback */
    lt_http_body_fn cb;
    void   *cb_ud;
    char    err[96];
} lt_hparse;

void lt_hparse_init(lt_hparse *p, int is_request);
void lt_hparse_free(lt_hparse *p);
void lt_hparse_set_callback(lt_hparse *p, lt_http_body_fn cb, void *ud);
/* feed bytes: 1 = message complete, 0 = need more, -1 = malformed (p->err) */
int  lt_hparse_feed(lt_hparse *p, const char *data, size_t n);
/* the peer closed the connection: 1 complete (body ended by close), -1 truncated */
int  lt_hparse_eof(lt_hparse *p);
/* header value (case-insensitive name) copied to out; LT_FALSE if absent */
lt_bool lt_hparse_header(const lt_hparse *p, const char *name, char *out, size_t cap);
int  lt_hparse_headers_done(const lt_hparse *p);

/* ---- URL ---- */
typedef struct lt_http_url {
    char host[128];
    int  port;
    char path[512];              /* always starts with '/' */
} lt_http_url;
/* 0 ok; -1 with lt_err_get(): https refused, bad scheme/host/port */
int lt_http_url_parse(const char *url, lt_http_url *u);

/* ---- client ---- */
typedef struct lt_http lt_http;
/* start a request; headers is "Name: value\r\n" lines or NULL (Host, Content-Length and
 * Connection are added).  timeout_ms bounds the silence between received chunks (the
 * blocking connect is capped at 3 s).  NULL on failure (lt_err_get).  cb != NULL streams the body instead of storing it. */
lt_http *lt_http_start(const char *method, const char *url, const char *headers,
                       const char *body, size_t body_len, int timeout_ms,
                       lt_http_body_fn cb, void *cb_ud);
/* advance without blocking: 0 in progress, 1 complete, -1 failed (lt_http_error) */
int  lt_http_step(lt_http *h);
int  lt_http_status(const lt_http *h);
const lt_buf *lt_http_body(const lt_http *h);
lt_bool lt_http_header(const lt_http *h, const char *name, char *out, size_t cap);
const char *lt_http_error(const lt_http *h);
void lt_http_free(lt_http *h);     /* also cancels */

LT_END_DECLS

#endif
