/* be_web.c - "web" GUI backend: a zero-dependency localhost HTTP server that
 * serves a single-page canvas client and streams the framebuffer to it.
 * Single-threaded: all sockets are serviced from poll() via lt_plat_wait().
 *
 * HTTP (all responses are "Connection: close"):
 *   GET  /?t=TOKEN        the page (sets cookie lt=TOKEN, SameSite=Strict)
 *   GET  /frame?since=N   long-poll (up to ~1s) until frame seq > N.
 *                         200 + binary body, or 204 on timeout.
 *   POST /input           text body, one event per line:
 *                           md x y b | mu x y b | mm x y | wh x y d (+1 up)
 *                           kd key mods | ku key mods | tx <utf8> | rs w h | fo
 * Everything except GET / (with the token) needs the cookie. Requests whose
 * Host header is not 127.0.0.1:PORT or localhost:PORT are rejected.
 *
 * Frame body (all integers little-endian):
 *   u32 seq, u16 w, u16 h, u16 ntiles, u16 titlelen, title bytes (UTF-8),
 *   then ntiles x { u16 x, u16 y, u16 w, u16 h, u32 enclen, enclen bytes }.
 *   (Header extension: the title rides in every frame header.)
 *   Optional trailer after the tiles: u32 bell, the number of beeps requested so far; it is
 *   only present once a beep happened (the client plays a short WebAudio tone whenever the
 *   count grows).
 *   enclen is the length of the RLE data that follows (the decoded size is
 *   always w*h*4 bytes).
 * Tile pixels are bytes R,G,B,255 (so the browser can use them as ImageData),
 * row-major inside the tile, RLE-coded as 32-bit pixels. Control byte c:
 *   c & 0x80 : run, count = (c & 0x7F) + 1, followed by ONE pixel (4 bytes)
 *   otherwise: literal, count = c + 1, followed by `count` pixels.
 * The server diffs each presented canvas against the previous one in 64x64
 * tiles and remembers the seq at which each tile last changed, so a client
 * that polls with since=N gets exactly the tiles changed after N. since=0,
 * a stale since, a seq gap > 256 or a canvas size change sends every tile.
 *
 * Known limitation: responses are written with a blocking write, so a
 * browser that stops reading mid-response can stall the UI for a moment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gui.h"
#include "../plat/plat.h"

#define WEB_DEFAULT_PORT 8640
#define WEB_PORT_TRIES 21
#define WEB_MAX_CONN 24
#define WEB_TILE 64
#define WEB_EVQ 512
#define WEB_MAX_HDR 16384
#define WEB_MAX_BODY 65536
#define WEB_LONGPOLL_MS 1000
#define WEB_SEQ_GAP 256
#define WEB_IDLE_MS 10000

/* The client page, as string chunks (each literal < 509 chars). */
static const char *const web_page_chunks[] = {
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>LibreTune</title>\n",
    "<style>html,body{margin:0;height:100%;overflow:hidden;background:#202020}canvas{display:block;outline:none}</style>\n",
    "</head><body><canvas id='c' tabindex='0'></canvas><script>\n",
    "var cv=document.getElementById('c'),cx=cv.getContext('2d'),seq=0,q=[],busy=false,rsT=0;\n",
    "function flush(){if(busy||!q.length)return;busy=true;var b=q.join('\\n')+'\\n';q=[];\n",
    "fetch('/input',{method:'POST',body:b,headers:{'Content-Type':'text/plain'}}).then(function(){busy=false;flush();},function(){busy=false;});}\n",
    "function send(s){var n=q.length;if(n&&s.slice(0,2)=='mm'&&q[n-1].slice(0,2)=='mm')q[n-1]=s;else q.push(s);flush();}\n",
    "function pos(e){var r=cv.getBoundingClientRect();return Math.floor(e.clientX-r.left)+' '+Math.floor(e.clientY-r.top);}\n",
    "function btn(e){return e.button==0?1:(e.button==1?2:3);}\n",
    "cv.onmousedown=function(e){cv.focus();send('md '+pos(e)+' '+btn(e));e.preventDefault();};\n",
    "window.onmouseup=function(e){send('mu '+pos(e)+' '+btn(e));};\n",
    "window.onmousemove=function(e){send('mm '+pos(e));};\n",
    "cv.onwheel=function(e){if(e.deltaY!=0)send('wh '+pos(e)+' '+(e.deltaY<0?1:-1));e.preventDefault();};\n",
    "cv.oncontextmenu=function(e){e.preventDefault();};\n",
    "function mods(e){return (e.shiftKey?1:0)|(e.ctrlKey?2:0)|(e.altKey?4:0)|(e.metaKey?8:0);}\n",
    "var K={Backspace:8,Tab:9,Enter:13,Escape:27,Delete:127,ArrowUp:256,ArrowDown:257,ArrowLeft:258,ArrowRight:259,\n",
    "Home:260,End:261,PageUp:262,PageDown:263,Insert:264};\n",
    "for(var i=1;i<=12;i++)K['F'+i]=264+i;\n",
    "var P={Space:32,Minus:45,Equal:61,Comma:44,Period:46,Slash:47,Semicolon:59,Quote:39,Backquote:96,BracketLeft:91,BracketRight:93,Backslash:92};\n",
    "function code(e){var c=e.code,k;if(K[e.key]!==undefined)return K[e.key];\n",
    "if(/^Key[A-Z]$/.test(c))return c.toLowerCase().charCodeAt(3);\n",
    "if(/^Digit[0-9]$/.test(c))return c.charCodeAt(5);\n",
    "if(P[c]!==undefined)return P[c];\n",
    "if(e.key.length==1){k=e.key.toLowerCase().charCodeAt(0);if(k<128)return k;}return 0;}\n",
    "window.onkeydown=function(e){var k=code(e),m=mods(e);\n",
    "if(k)send('kd '+k+' '+m);\n",
    "if(e.key.length==1&&!(e.ctrlKey&&!e.altKey)&&!e.metaKey)send('tx '+e.key);\n",
    "if(K[e.key]!==undefined||k==32||(e.ctrlKey&&!e.shiftKey&&k>=97&&k<=122))e.preventDefault();};\n",
    "window.onkeyup=function(e){var k=code(e);if(k)send('ku '+k+' '+mods(e));};\n",
    "window.onblur=function(){send('fo');};\n",
    "function rs(){clearTimeout(rsT);rsT=setTimeout(function(){send('rs '+window.innerWidth+' '+window.innerHeight);},50);}\n",
    "window.onresize=rs;\n",
    "function u16(d,o){return d.getUint16(o,true);}\n",
    "function rle(s,p,end,out){var o=0,c,n,i;while(p<end){c=s[p++];\n",
    "if(c&128){n=(c&127)+1;while(n--){out[o]=s[p];out[o+1]=s[p+1];out[o+2]=s[p+2];out[o+3]=s[p+3];o+=4;}p+=4;}\n",
    "else{n=(c+1)*4;out.set(s.subarray(p,p+n),o);o+=n;p+=n;}}}\n",
    "function frame(buf){var d=new DataView(buf),u=new Uint8Array(buf),o=12,i,n,w,h,tl,x,y,tw,th,el,im;\n",
    "seq=d.getUint32(0,true);w=u16(d,4);h=u16(d,6);n=u16(d,8);tl=u16(d,10);\n",
    "if(tl){document.title=new TextDecoder().decode(u.subarray(o,o+tl));o+=tl;}\n",
    "if(cv.width!=w||cv.height!=h){cv.width=w;cv.height=h;}\n",
    "for(i=0;i<n;i++){x=u16(d,o);y=u16(d,o+2);tw=u16(d,o+4);th=u16(d,o+6);el=d.getUint32(o+8,true);o+=12;\n",
    "im=cx.createImageData(tw,th);rle(u,o,o+el,im.data);cx.putImageData(im,x,y);o+=el;}\n",
    "if(o+4<=u.length){i=d.getUint32(o,true);if(bl>=0&&i!=bl)beep();bl=i;}else if(bl<0)bl=0;}\n",
    "var bl=-1;function beep(){try{var A=window.AudioContext||window.webkitAudioContext;if(!A)return;\n",
    "var c=beep.c||(beep.c=new A()),os=c.createOscillator(),g=c.createGain();os.frequency.value=880;g.gain.value=0.15;\n",
    "os.connect(g);g.connect(c.destination);os.start();os.stop(c.currentTime+0.15);}catch(e){}}\n",
    "function poll(){fetch('/frame?since='+seq,{cache:'no-store'}).then(function(r){\n",
    "if(r.status==200)return r.arrayBuffer().then(function(b){frame(b);poll();});\n",
    "if(r.status==204){poll();return;}throw 0;}).catch(function(){setTimeout(poll,500);});}\n",
    "rs();poll();cv.focus();\n",
    "</script></body></html>\n",
    NULL
};

typedef struct web_conn {
    lt_stream *s;
    lt_buf in;
    int done;      /* request handled (or parked) */
    int parked;    /* waiting for a frame */
    int dead;
    lt_u32 since;
    double born, park_t;
} web_conn;

typedef struct web {
    int req_port, port, w, h;
    lt_server *srv;
    char auth_key[40];
    char title[128];
    lt_buf page;
    lt_u32 *prev;          /* last presented canvas, tightly packed, 24-bit */
    int cw, ch, ntx, nty;
    lt_u32 *tseq;          /* per-tile seq of last change */
    lt_u32 seq, size_seq;
    lt_u32 bell;           /* beeps requested so far (frame trailer) */
    lt_event q[WEB_EVQ];
    int qh, qn;
    web_conn *conns[WEB_MAX_CONN];
    int nconn;
} web;

/* ---- small helpers ---- */

static void put_u16(lt_buf *b, unsigned v)
{
    lt_buf_putc(b, (int)(v & 0xFF));
    lt_buf_putc(b, (int)((v >> 8) & 0xFF));
}

static void put_u32(lt_buf *b, lt_u32 v)
{
    put_u16(b, (unsigned)(v & 0xFFFF));
    put_u16(b, (unsigned)((v >> 16) & 0xFFFF));
}

static void put_px(lt_buf *b, lt_u32 p)
{
    lt_buf_putc(b, (int)((p >> 16) & 0xFF));
    lt_buf_putc(b, (int)((p >> 8) & 0xFF));
    lt_buf_putc(b, (int)(p & 0xFF));
    lt_buf_putc(b, 0xFF);
}

/* RLE codec, see file comment. p holds n pixels (24-bit values). */
static void rle_encode(lt_buf *b, const lt_u32 *p, int n)
{
    int i = 0, j, r, k;
    while (i < n) {
        r = 1;
        while (i + r < n && r < 128 && p[i + r] == p[i]) r++;
        if (r >= 2) {
            lt_buf_putc(b, 0x80 | (r - 1));
            put_px(b, p[i]);
            i += r;
        } else {
            j = i + 1;
            while (j < n && j - i < 128 &&
                   !(j + 2 < n && p[j] == p[j + 1] && p[j] == p[j + 2])) j++;
            lt_buf_putc(b, j - i - 1);
            for (k = i; k < j; k++) put_px(b, p[k]);
            i = j;
        }
    }
}

static void make_auth_key(web *w)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char r[16];
    lt_u32 h[4], x;
    int i, got = 0;
    void *probe = lt_malloc(8);

    got = (lt_plat_random(r, sizeof(r)) == 0);
    if (!got) {
        h[0] = (lt_u32)time(NULL) & LT_U32_MASK;
        h[1] = (lt_u32)clock() & LT_U32_MASK;
        h[2] = (lt_u32)(size_t)probe & LT_U32_MASK;
        h[3] = (lt_u32)(lt_plat_now_ms() * 1000.0) & LT_U32_MASK;
        for (i = 0; i < 16; i++) {
            x = h[i & 3] ^ ((lt_u32)(size_t)w & LT_U32_MASK) ^ (lt_u32)i;
            x = (x * 2654435761UL) & LT_U32_MASK;
            x ^= x >> 15;
            x = (x * 2246822519UL) & LT_U32_MASK;
            x ^= x >> 13;
            h[(i + 1) & 3] ^= x;
            r[i] = (unsigned char)((x >> 8) & 0xFF);
        }
    }
    lt_free(probe);
    for (i = 0; i < 16; i++) {
        w->auth_key[i * 2] = hex[r[i] >> 4];
        w->auth_key[i * 2 + 1] = hex[r[i] & 15];
    }
    w->auth_key[32] = '\0';
}

/* ---- event queue ---- */

static void evq_push(web *w, const lt_event *e)
{
    int last;
    if (e->type == LT_EV_MOUSE_MOVE && w->qn > 0) {
        last = (w->qh + w->qn - 1) % WEB_EVQ;
        if (w->q[last].type == LT_EV_MOUSE_MOVE) {
            w->q[last] = *e;
            return;
        }
    }
    if (w->qn >= WEB_EVQ) return;
    w->q[(w->qh + w->qn) % WEB_EVQ] = *e;
    w->qn++;
}

static void parse_line(web *w, const char *line)
{
    lt_event e;
    int a, b, c, n, len;

    memset(&e, 0, sizeof(e));
    if (strncmp(line, "tx ", 3) == 0) {
        len = (int)strlen(line + 3);
        if (len > 7) {
            len = 7;
            while (len > 0 && (((unsigned char)line[3 + len]) & 0xC0) == 0x80) len--;
        }
        if (len <= 0) return;
        e.type = LT_EV_TEXT;
        memcpy(e.text, line + 3, (size_t)len);
        e.text[len] = '\0';
    } else if (strcmp(line, "fo") == 0) {
        e.type = LT_EV_FOCUS_LOST;
    } else if ((n = sscanf(line, "md %d %d %d", &a, &b, &c)) == 3 && line[0] == 'm' &&
               line[1] == 'd') {
        e.type = LT_EV_MOUSE_DOWN; e.x = a; e.y = b; e.button = c;
    } else if (sscanf(line, "mu %d %d %d", &a, &b, &c) == 3 && line[1] == 'u') {
        e.type = LT_EV_MOUSE_UP; e.x = a; e.y = b; e.button = c;
    } else if (sscanf(line, "mm %d %d", &a, &b) == 2 && line[1] == 'm') {
        e.type = LT_EV_MOUSE_MOVE; e.x = a; e.y = b;
    } else if (sscanf(line, "wh %d %d %d", &a, &b, &c) == 3 && line[0] == 'w') {
        e.type = LT_EV_WHEEL; e.x = a; e.y = b; e.wheel = c > 0 ? 1 : -1;
    } else if (sscanf(line, "kd %d %d", &a, &b) == 2 && line[0] == 'k' && line[1] == 'd') {
        e.type = LT_EV_KEY_DOWN; e.key = a; e.mods = b & 15;
    } else if (sscanf(line, "ku %d %d", &a, &b) == 2 && line[0] == 'k' && line[1] == 'u') {
        e.type = LT_EV_KEY_UP; e.key = a; e.mods = b & 15;
    } else if (sscanf(line, "rs %d %d", &a, &b) == 2 && line[0] == 'r') {
        if (a < 16) a = 16;
        if (b < 16) b = 16;
        if (a > 8192) a = 8192;
        if (b > 8192) b = 8192;
        w->w = a;
        w->h = b;
        e.type = LT_EV_RESIZE; e.x = a; e.y = b; e.button = 0;
    } else {
        return;
    }
    evq_push(w, &e);
}

static void parse_input(web *w, const char *body, size_t len)
{
    char line[256];
    size_t i = 0, n;
    while (i < len) {
        n = 0;
        while (i < len && body[i] != '\n') {
            if (n < sizeof(line) - 1) line[n++] = body[i];
            i++;
        }
        i++;
        while (n > 0 && line[n - 1] == '\r') n--;
        line[n] = '\0';
        if (n > 1) parse_line(w, line);
    }
}

/* ---- frame model ---- */

static void web_update(web *w, const lt_canvas *c)
{
    int x, y, t, tx, ty, tw, th, changed = 0, ntiles;
    lt_u32 nseq = (w->seq + 1) & LT_U32_MASK;
    int resized = (w->prev == NULL || w->cw != c->w || w->ch != c->h);

    if (c->px == NULL || c->w <= 0 || c->h <= 0 || c->w > 65535 || c->h > 65535) return;
    if (resized) {
        lt_free(w->prev);
        lt_free(w->tseq);
        w->cw = c->w;
        w->ch = c->h;
        w->ntx = (c->w + WEB_TILE - 1) / WEB_TILE;
        w->nty = (c->h + WEB_TILE - 1) / WEB_TILE;
        ntiles = w->ntx * w->nty;
        w->prev = (lt_u32 *)lt_calloc((size_t)c->w * (size_t)c->h, sizeof(lt_u32));
        w->tseq = (lt_u32 *)lt_calloc((size_t)ntiles, sizeof(lt_u32));
        w->size_seq = nseq;
    }
    for (ty = 0; ty < w->nty; ty++) {
        for (tx = 0; tx < w->ntx; tx++) {
            int diff = resized;
            t = ty * w->ntx + tx;
            tw = LT_MIN(WEB_TILE, w->cw - tx * WEB_TILE);
            th = LT_MIN(WEB_TILE, w->ch - ty * WEB_TILE);
            for (y = 0; y < th; y++) {
                const lt_u32 *src = c->px + (size_t)(ty * WEB_TILE + y) * (size_t)c->pitch +
                                    (size_t)(tx * WEB_TILE);
                lt_u32 *dst = w->prev + (size_t)(ty * WEB_TILE + y) * (size_t)w->cw +
                              (size_t)(tx * WEB_TILE);
                for (x = 0; x < tw; x++) {
                    lt_u32 p = src[x] & 0xFFFFFFUL;
                    if (dst[x] != p) {
                        diff = 1;
                        dst[x] = p;
                    }
                }
            }
            if (diff) {
                w->tseq[t] = nseq;
                changed = 1;
            }
        }
    }
    if (changed) w->seq = nseq;
}

static void build_frame(web *w, lt_buf *out, lt_u32 since)
{
    int full = (since == 0 || since > w->seq || w->seq - since > WEB_SEQ_GAP ||
                since < w->size_seq);
    int t, n = 0, tx, ty, tw, th, x, y, nt = w->ntx * w->nty;
    size_t tl = strlen(w->title), at, start;
    lt_u32 tmp[WEB_TILE * WEB_TILE];
    lt_u32 enclen;

    for (t = 0; t < nt; t++) if (full || w->tseq[t] > since) n++;
    lt_buf_clear(out);
    put_u32(out, w->seq);
    put_u16(out, (unsigned)w->cw);
    put_u16(out, (unsigned)w->ch);
    put_u16(out, (unsigned)n);
    put_u16(out, (unsigned)tl);
    lt_buf_append(out, w->title, tl);
    for (t = 0; t < nt; t++) {
        if (!(full || w->tseq[t] > since)) continue;
        tx = t % w->ntx;
        ty = t / w->ntx;
        tw = LT_MIN(WEB_TILE, w->cw - tx * WEB_TILE);
        th = LT_MIN(WEB_TILE, w->ch - ty * WEB_TILE);
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                tmp[y * tw + x] = w->prev[(size_t)(ty * WEB_TILE + y) * (size_t)w->cw +
                                          (size_t)(tx * WEB_TILE + x)];
        put_u16(out, (unsigned)(tx * WEB_TILE));
        put_u16(out, (unsigned)(ty * WEB_TILE));
        put_u16(out, (unsigned)tw);
        put_u16(out, (unsigned)th);
        at = out->len;
        put_u32(out, 0);
        start = out->len;
        rle_encode(out, tmp, tw * th);
        enclen = (lt_u32)(out->len - start);
        lt_wr_le((lt_u8 *)out->data + at, 4, enclen);
    }
    if (w->bell != 0) put_u32(out, w->bell);
}

/* ---- HTTP ---- */

static void respond(web_conn *c, int status, const char *reason, const char *ctype,
                    const char *extra, const void *body, size_t len)
{
    lt_buf r;
    lt_buf_init(&r);
    lt_buf_printf(&r, "HTTP/1.1 %d %s\r\n", status, reason);
    if (ctype != NULL) lt_buf_printf(&r, "Content-Type: %s\r\n", ctype);
    lt_buf_printf(&r, "Content-Length: %lu\r\n", (unsigned long)len);
    lt_buf_puts(&r, "Cache-Control: no-store\r\nConnection: close\r\n"
                    "X-Content-Type-Options: nosniff\r\n"
                    "Cross-Origin-Resource-Policy: same-origin\r\n");
    if (extra != NULL) lt_buf_puts(&r, extra);
    lt_buf_puts(&r, "\r\n");
    if (len > 0) lt_buf_append(&r, body, len);
    lt_stream_write(c->s, r.data, (int)r.len);
    lt_buf_free(&r);
    c->dead = 1;
}

static void respond_text(web_conn *c, int status, const char *reason)
{
    respond(c, status, reason, "text/plain", NULL, reason, strlen(reason));
}

static int header_get(const char *h, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name), i;
    const char *p = strchr(h, '\n');
    out[0] = '\0';
    while (p != NULL) {
        p++;
        if (lt_strnicmp(p, name, nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            for (i = 0; *p && *p != '\r' && *p != '\n' && i + 1 < cap; p++) out[i++] = *p;
            out[i] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
    }
    return 0;
}

static int query_get(const char *q, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name), i;
    out[0] = '\0';
    while (q != NULL && *q) {
        if (strncmp(q, name, nl) == 0 && q[nl] == '=') {
            q += nl + 1;
            for (i = 0; *q && *q != '&' && i + 1 < cap; q++) out[i++] = *q;
            out[i] = '\0';
            return 1;
        }
        q = strchr(q, '&');
        if (q != NULL) q++;
    }
    return 0;
}

static int cookie_ok(const web *w, const char *cookie)
{
    size_t tl = strlen(w->auth_key);
    const char *p = cookie;
    while ((p = strstr(p, "lt=")) != NULL) {
        if ((p == cookie || p[-1] == ' ' || p[-1] == ';') && strncmp(p + 3, w->auth_key, tl) == 0 &&
            (p[3 + tl] == '\0' || p[3 + tl] == ';' || p[3 + tl] == ' '))
            return 1;
        p += 3;
    }
    return 0;
}

static int host_ok(const web *w, const char *host)
{
    char a[40], b[40];
    lt_snprintf(a, sizeof(a), "127.0.0.1:%d", w->port);
    lt_snprintf(b, sizeof(b), "localhost:%d", w->port);
    return lt_stricmp(host, a) == 0 || lt_stricmp(host, b) == 0;
}

static void send_frame(web *w, web_conn *c)
{
    lt_buf f;
    lt_buf_init(&f);
    build_frame(w, &f, c->since);
    respond(c, 200, "OK", "application/octet-stream", NULL, f.data, f.len);
    lt_buf_free(&f);
}

static int frame_ready(const web *w, const web_conn *c)
{
    return w->seq > 0 && (w->seq > c->since || c->since > w->seq);
}

static void handle_request(web *w, web_conn *c, const char *hdrs, const char *method,
                           char *target, const char *body, size_t blen)
{
    char host[128], cookie[256], val[64], *query;
    int is_get = strcmp(method, "GET") == 0, is_post = strcmp(method, "POST") == 0;
    int authed;
    long n;

    if (!header_get(hdrs, "Host", host, sizeof(host)) || !host_ok(w, host)) {
        respond_text(c, 403, "Forbidden");
        return;
    }
    query = strchr(target, '?');
    if (query != NULL) *query++ = '\0';
    if (!is_get && !is_post) {
        respond_text(c, 405, "Method Not Allowed");
        return;
    }
    if (is_get && strcmp(target, "/favicon.ico") == 0) {
        respond(c, 204, "No Content", NULL, NULL, NULL, 0);
        return;
    }
    cookie[0] = '\0';
    header_get(hdrs, "Cookie", cookie, sizeof(cookie));
    authed = cookie_ok(w, cookie);
    if (is_get && strcmp(target, "/") == 0) {
        if (!authed && !(query_get(query, "t", val, sizeof(val)) && strcmp(val, w->auth_key) == 0)) {
            respond_text(c, 403, "Forbidden");
            return;
        }
        {
            char extra[400];
            lt_snprintf(extra, sizeof(extra), "Set-Cookie: lt=%s; Path=/; HttpOnly; "
                        "SameSite=Strict\r\nContent-Security-Policy: default-src 'self'; "
                        "script-src 'unsafe-inline'; style-src 'unsafe-inline'\r\n", w->auth_key);
            respond(c, 200, "OK", "text/html; charset=utf-8", extra, w->page.data, w->page.len);
        }
        return;
    }
    if (!authed) {
        respond_text(c, 403, "Forbidden");
        return;
    }
    if (is_get && strcmp(target, "/frame") == 0) {
        n = 0;
        if (query_get(query, "since", val, sizeof(val))) {
            if (!lt_parse_long(val, &n) || n < 0) n = 0;
        }
        c->since = (lt_u32)n & LT_U32_MASK;
        c->done = 1;
        if (frame_ready(w, c)) {
            send_frame(w, c);
        } else {
            c->parked = 1;
            c->park_t = lt_plat_now_ms();
        }
        return;
    }
    if (is_post && strcmp(target, "/input") == 0) {
        parse_input(w, body, blen);
        respond(c, 204, "No Content", NULL, NULL, NULL, 0);
        return;
    }
    respond_text(c, 404, "Not Found");
}

/* Try to parse one complete request out of c->in. */
static void conn_try_request(web *w, web_conn *c)
{
    char *d = c->in.data, *he, *body;
    char method[16], target[1024], clen[32];
    size_t hl, avail;
    long cl = 0;

    if (c->in.len == 0) return;
    he = strstr(d, "\r\n\r\n");
    if (he == NULL) {
        if (c->in.len > WEB_MAX_HDR) {
            c->done = 1;
            respond_text(c, 431, "Request Header Fields Too Large");
        }
        return;
    }
    hl = (size_t)(he - d);
    *he = '\0';
    if (header_get(d, "Content-Length", clen, sizeof(clen))) {
        if (!lt_parse_long(clen, &cl) || cl < 0 || cl > WEB_MAX_BODY) {
            c->done = 1;
            respond_text(c, 413, "Payload Too Large");
            return;
        }
    }
    avail = c->in.len - (hl + 4);
    if (avail < (size_t)cl) {
        *he = '\r';
        return;
    }
    method[0] = target[0] = '\0';
    if (sscanf(d, "%15s %1023s", method, target) != 2 || target[0] != '/') {
        c->done = 1;
        respond_text(c, 400, "Bad Request");
        return;
    }
    body = (char *)lt_malloc((size_t)cl + 1);
    memcpy(body, he + 4, (size_t)cl);
    body[cl] = '\0';
    c->done = 1;
    handle_request(w, c, d, method, target, body, (size_t)cl);
    lt_free(body);
}

/* ---- connection management ---- */

static void conn_free(web_conn *c)
{
    if (c->s != NULL) lt_stream_close(c->s);
    lt_buf_free(&c->in);
    lt_free(c);
}

static void web_service(web *w, int wait_ms)
{
    lt_stream *ss[WEB_MAX_CONN + 1];
    lt_server *sv[2];
    lt_stream *ns;
    web_conn *c;
    char tmp[4096];
    int i, n, got, closed, k;
    double now;

    for (i = 0, n = 0; i < w->nconn; i++) ss[n++] = w->conns[i]->s;
    ss[n] = NULL;
    sv[0] = w->srv;
    sv[1] = NULL;
    lt_plat_wait(ss, sv, -1, wait_ms);
    now = lt_plat_now_ms();

    while ((ns = lt_server_accept(w->srv, 0)) != NULL) {
        if (w->nconn >= WEB_MAX_CONN) {
            lt_stream_close(ns);
            continue;
        }
        c = (web_conn *)lt_calloc(1, sizeof(*c));
        c->s = ns;
        c->born = now;
        lt_buf_init(&c->in);
        w->conns[w->nconn++] = c;
    }

    for (i = 0; i < w->nconn; i++) {
        c = w->conns[i];
        closed = 0;
        for (k = 0; k < 64; k++) {
            got = lt_stream_read(c->s, tmp, (int)sizeof(tmp), 0);
            if (got < 0) { closed = 1; break; }
            if (got == 0) break;
            if (!c->done && c->in.len < (size_t)(WEB_MAX_HDR + WEB_MAX_BODY))
                lt_buf_append(&c->in, tmp, (size_t)got);
        }
        if (!c->done) conn_try_request(w, c);
        if (closed && (c->parked || !c->done)) c->dead = 1;
        if (!c->done && now - c->born > WEB_IDLE_MS) c->dead = 1;
        if (c->parked && !c->dead && now - c->park_t >= WEB_LONGPOLL_MS) {
            c->parked = 0;
            respond(c, 204, "No Content", NULL, NULL, NULL, 0);
        }
    }
    for (i = 0; i < w->nconn;) {
        if (w->conns[i]->dead) {
            conn_free(w->conns[i]);
            w->conns[i] = w->conns[--w->nconn];
        } else {
            i++;
        }
    }
}

static void wake_parked(web *w)
{
    int i;
    for (i = 0; i < w->nconn; i++) {
        web_conn *c = w->conns[i];
        if (c->parked && !c->dead && frame_ready(w, c)) {
            c->parked = 0;
            send_frame(w, c);
        }
    }
}

/* ---- backend vtable ---- */

static web *W(lt_backend *b)
{
    return b != NULL ? (web *)b->impl : NULL;
}

static int web_open(lt_backend *b, const char *title, int width, int height)
{
    web *w = W(b);
    int base, i, srv_ok = 0;
    char *nb;
    char url[160];
    const char *const *p;

    if (w == NULL) return -1;
    lt_strlcpy(w->title, title != NULL ? title : "LibreTune", sizeof(w->title));
    w->w = width > 0 ? width : 1024;
    w->h = height > 0 ? height : 768;
    lt_buf_clear(&w->page);
    for (p = web_page_chunks; *p != NULL; p++) lt_buf_puts(&w->page, *p);
    make_auth_key(w);
    base = w->req_port > 0 ? w->req_port : WEB_DEFAULT_PORT;
    for (i = 0; i < WEB_PORT_TRIES && base + i < 65536; i++) {
        w->srv = lt_server_listen(base + i);
        if (w->srv != NULL) {
            w->port = lt_server_port(w->srv);
            if (w->port <= 0) w->port = base + i;
            srv_ok = 1;
            break;
        }
    }
    if (!srv_ok) {
        lt_err_set("web: cannot listen on ports %d..%d", base, base + WEB_PORT_TRIES - 1);
        return -1;
    }
    lt_snprintf(url, sizeof(url), "http://127.0.0.1:%d/?t=%s", w->port, w->auth_key);
    fprintf(stderr, "LibreTune UI: %s\n", url);
    nb = lt_plat_getenv("LT_NO_BROWSER");
    if (nb == NULL) lt_plat_open_url(url);
    lt_free(nb);
    return 0;
}

static void web_close(lt_backend *b)
{
    web *w = W(b);
    int i;
    if (w == NULL) return;
    for (i = 0; i < w->nconn; i++) conn_free(w->conns[i]);
    if (w->srv != NULL) lt_server_close(w->srv);
    lt_buf_free(&w->page);
    lt_free(w->prev);
    lt_free(w->tseq);
    lt_free(w);
    b->impl = NULL;
}

static int web_poll(lt_backend *b, lt_event *ev, int timeout_ms)
{
    web *w = W(b);
    double start, el;
    int slice;

    if (w == NULL || w->srv == NULL) return 0;
    start = lt_plat_now_ms();
    for (;;) {
        if (w->qn > 0) {
            *ev = w->q[w->qh];
            w->qh = (w->qh + 1) % WEB_EVQ;
            w->qn--;
            return 1;
        }
        el = lt_plat_now_ms() - start;
        if (timeout_ms >= 0 && el >= timeout_ms && el > 0) return 0;
        slice = 50;
        if (timeout_ms >= 0 && timeout_ms - el < slice) slice = (int)(timeout_ms - el);
        if (slice < 0) slice = 0;
        web_service(w, slice);
        if (w->qn == 0 && timeout_ms == 0) return 0;
    }
}

static void web_present(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty)
{
    web *w = W(b);
    lt_u32 old;
    LT_UNUSED(cells);
    LT_UNUSED(dirty);
    if (w == NULL || canvas == NULL) return;
    old = w->seq;
    web_update(w, canvas);
    if (w->seq != old) wake_parked(w);
}

static void web_size(lt_backend *b, int *wd, int *ht)
{
    web *w = W(b);
    *wd = w != NULL ? w->w : 0;
    *ht = w != NULL ? w->h : 0;
}

static void web_set_title(lt_backend *b, const char *title)
{
    web *w = W(b);
    char old[128];
    if (w == NULL || title == NULL) return;
    lt_strlcpy(old, w->title, sizeof(old));
    lt_strlcpy(w->title, title, sizeof(w->title));
    if (strcmp(old, w->title) != 0 && w->seq > 0) {
        w->seq = (w->seq + 1) & LT_U32_MASK;
        wake_parked(w);
    }
}

/* a new frame carries the grown bell count; the browser plays the tone */
static void web_beep(lt_backend *b)
{
    web *w = W(b);
    if (w == NULL) return;
    w->bell = (w->bell + 1) & LT_U32_MASK;
    if (w->bell == 0) w->bell = 1;
    if (w->seq > 0) {
        w->seq = (w->seq + 1) & LT_U32_MASK;
        wake_parked(w);
    }
}

lt_backend *lt_backend_web_create(int port)
{
    lt_backend *b = (lt_backend *)lt_calloc(1, sizeof(*b));
    web *w = (web *)lt_calloc(1, sizeof(*w));

    w->req_port = port;
    lt_buf_init(&w->page);
    lt_strlcpy(w->title, "LibreTune", sizeof(w->title));
    b->name = "web";
    b->cell_mode = LT_FALSE;
    b->open = web_open;
    b->close = web_close;
    b->poll = web_poll;
    b->present = web_present;
    b->size = web_size;
    b->set_title = web_set_title;
    b->set_clipboard = NULL;
    b->get_clipboard = NULL;
    b->beep = web_beep;
    b->impl = w;
    return b;
}
