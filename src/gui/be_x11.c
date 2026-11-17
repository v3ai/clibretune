/* be_x11.c - X11 backend speaking the raw wire protocol (no Xlib/xcb).
 * Transport via lt_unix_connect / lt_tcp_connect from plat.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gui.h"
#include "../plat/plat.h"

#define XQ_CAP 256
#define XPKT_MAX 4096

typedef struct x11 {
    lt_stream *s;
    int msb;                 /* our byte order: 1 = MSB first */
    int img_msb;             /* server image byte order */
    lt_u32 base, mask, inc, nextn;
    int maxreq;              /* in 4-byte units */
    lt_u32 root, rootvis;
    int depth, bpp, spad;
    lt_u32 win, gc;
    int w, h;
    int seq;
    lt_u32 a_prot, a_del, a_netname, a_utf8;
    lt_u32 a_wmstate, a_maxh, a_maxv;
    int root_w, root_h;
    double xft_dpi;          /* from RESOURCE_MANAGER, 0 = none */
    int minw, minh;          /* WM_NORMAL_HINTS minimum, 0 = none */
    int init_x, init_y, init_flags;
    int min_kc, kpk, nkc;
    lt_u32 *syms;
    lt_u8 *in;
    size_t in_len, in_cap;
    lt_event q[XQ_CAP];
    int q_head, q_count;
    int dead;
    int waiting, want_seq, got_reply, got_err;
    lt_u8 *reply;
    size_t reply_len;
} x11;

/* ------------------------------------------------------------------ */
/* encoding                                                            */
static void p8(lt_buf *b, int v) { lt_buf_putc(b, v & 0xFF); }
static void p16(const x11 *x, lt_buf *b, lt_u32 v) {
    lt_u8 t[2];
    if (x->msb) lt_wr_be(t, 2, v); else lt_wr_le(t, 2, v);
    lt_buf_append(b, t, 2);
}
static void p32(const x11 *x, lt_buf *b, lt_u32 v) {
    lt_u8 t[4];
    if (x->msb) lt_wr_be(t, 4, v); else lt_wr_le(t, 4, v);
    lt_buf_append(b, t, 4);
}
static lt_u32 r16(const x11 *x, const lt_u8 *p) { return x->msb ? lt_rd_be(p, 2) : lt_rd_le(p, 2); }
static lt_u32 r32(const x11 *x, const lt_u8 *p) { return x->msb ? lt_rd_be(p, 4) : lt_rd_le(p, 4); }
static int r16s(const x11 *x, const lt_u8 *p) {
    lt_u32 v = r16(x, p);
    return (int)(v & 0x8000UL ? (long)v - 65536L : (long)v);
}
static void pad4(lt_buf *b) { while (b->len & 3) lt_buf_putc(b, 0); }

static void req_begin(lt_buf *b, int op, int b1) {
    lt_buf_clear(b);
    p8(b, op); p8(b, b1); p8(b, 0); p8(b, 0);
}
/* finish (pad, fill length), send. returns 0 ok */
static int req_end(x11 *x, lt_buf *b) {
    lt_u8 t[2];
    size_t words;
    pad4(b);
    words = b->len / 4;
    if (words > 65535UL || x->dead) return -1;
    if (x->msb) lt_wr_be(t, 2, (lt_u32)words); else lt_wr_le(t, 2, (lt_u32)words);
    b->data[2] = (char)t[0]; b->data[3] = (char)t[1];
    if (lt_stream_write(x->s, b->data, (int)b->len) < 0) { x->dead = 1; return -1; }
    x->seq = (x->seq + 1) & 0xFFFF;
    return 0;
}

/* ------------------------------------------------------------------ */
/* events                                                              */
static void q_push(x11 *x, const lt_event *ev) {
    int i;
    if (x->q_count >= XQ_CAP) return;
    i = (x->q_head + x->q_count) % XQ_CAP;
    x->q[i] = *ev;
    x->q_count++;
}
static void q_simple(x11 *x, lt_ev_type t) {
    lt_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = t;
    q_push(x, &ev);
}

static int key_of(lt_u32 s) {
    if (s >= 0x20 && s <= 0x7e) return (s >= 'A' && s <= 'Z') ? (int)s + 32 : (int)s;
    if (s >= 0xffb0 && s <= 0xffb9) return (int)('0' + (s - 0xffb0));
    if (s >= 0xffbe && s <= 0xffc9) return LT_KEY_F1 + (int)(s - 0xffbe);
    switch (s) {
    case 0xff08: return LT_KEY_BACKSPACE;
    case 0xff09: return LT_KEY_TAB;
    case 0xff0d: case 0xff8d: return LT_KEY_ENTER;
    case 0xff1b: return LT_KEY_ESCAPE;
    case 0xffff: case 0xff9f: return LT_KEY_DELETE;
    case 0xff50: case 0xff95: return LT_KEY_HOME;
    case 0xff51: case 0xff96: return LT_KEY_LEFT;
    case 0xff52: case 0xff97: return LT_KEY_UP;
    case 0xff53: case 0xff98: return LT_KEY_RIGHT;
    case 0xff54: case 0xff99: return LT_KEY_DOWN;
    case 0xff55: case 0xff9a: return LT_KEY_PAGEUP;
    case 0xff56: case 0xff9b: return LT_KEY_PAGEDOWN;
    case 0xff57: case 0xff9c: return LT_KEY_END;
    case 0xff63: case 0xff9e: return LT_KEY_INSERT;
    case 0xffab: return '+';
    case 0xffad: return '-';
    case 0xffaa: return '*';
    case 0xffaf: return '/';
    case 0xffae: return '.';
    case 0xffbd: return '=';
    default: break;
    }
    return 0;
}

/* code point typed by a keysym, or 0 */
static lt_u32 sym_text(lt_u32 s) {
    if (s >= 0x20 && s <= 0x7e) return s;
    if (s >= 0xa0 && s <= 0xff) return s;
    if (s >= 0x1000100UL && s <= 0x110ffffUL) return s - (lt_u32)0x1000000UL;
    if (s >= 0xffb0 && s <= 0xffb9) return '0' + (s - 0xffb0);
    switch (s) {
    case 0xffab: return '+';
    case 0xffad: return '-';
    case 0xffaa: return '*';
    case 0xffaf: return '/';
    case 0xffae: return '.';
    case 0xffbd: return '=';
    default: break;
    }
    return 0;
}

static int utf8_put(char *d, lt_u32 c) {
    if (c < 0x80) { d[0] = (char)c; return 1; }
    if (c < 0x800) { d[0] = (char)(0xC0 | (c >> 6)); d[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000UL) {
        d[0] = (char)(0xE0 | (c >> 12)); d[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        d[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    d[0] = (char)(0xF0 | (c >> 18)); d[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((c >> 6) & 0x3F)); d[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static int conv_mods(int state) {
    int m = 0;
    if (state & 1) m |= LT_MOD_SHIFT;
    if (state & 4) m |= LT_MOD_CTRL;
    if (state & 8) m |= LT_MOD_ALT;
    if (state & 0x40) m |= LT_MOD_SUPER;
    return m;
}

static void handle_key(x11 *x, int down, int kc, int state) {
    lt_event ev;
    lt_u32 s0 = 0, s1 = 0, sym;
    int idx = kc - x->min_kc, shift, lock, numlock;
    int key;
    if (!x->syms || idx < 0 || idx >= x->nkc) return;
    s0 = x->syms[idx * x->kpk];
    if (x->kpk > 1) s1 = x->syms[idx * x->kpk + 1];
    shift = (state & 1) != 0;
    lock = (state & 2) != 0;
    numlock = (state & 0x10) != 0;
    sym = s0;
    if (s0 >= 0xff80 && s0 <= 0xffbd) {
        if ((shift != 0) != (numlock != 0) && s1) sym = s1;
    } else if (shift) {
        if (s1) sym = s1;
    } else if (lock && s0 >= 'a' && s0 <= 'z' && s1) {
        sym = s1;
    }
    if (s0 >= 0xff80 && s0 <= 0xffbd) key = key_of(sym);
    else {
        key = key_of(s0);
        if (!key) key = key_of(sym);
    }
    if (!key && !sym_text(sym)) return;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? LT_EV_KEY_DOWN : LT_EV_KEY_UP;
    ev.key = key;
    ev.mods = conv_mods(state);
    if (key) q_push(x, &ev);
    if (down && !(state & (4 | 8))) {
        lt_u32 cp = sym_text(sym);
        if (cp) {
            int n;
            memset(&ev, 0, sizeof ev);
            ev.type = LT_EV_TEXT;
            ev.mods = conv_mods(state);
            n = utf8_put(ev.text, cp);
            ev.text[n] = '\0';
            q_push(x, &ev);
        }
    }
}

static void handle_event(x11 *x, const lt_u8 *p) {
    int type = p[0] & 0x7F;
    lt_event ev;
    memset(&ev, 0, sizeof ev);
    switch (type) {
    case 2: case 3:
        handle_key(x, type == 2, p[1], (int)r16(x, p + 28));
        break;
    case 4: case 5: {
        int d = p[1];
        ev.x = r16s(x, p + 24); ev.y = r16s(x, p + 26);
        ev.mods = conv_mods((int)r16(x, p + 28));
        if (d >= 1 && d <= 3) {
            ev.type = (type == 4) ? LT_EV_MOUSE_DOWN : LT_EV_MOUSE_UP;
            ev.button = d;
            q_push(x, &ev);
        } else if (type == 4 && (d == 4 || d == 5)) {
            ev.type = LT_EV_WHEEL; ev.wheel = (d == 4) ? 1 : -1;
            q_push(x, &ev);
        } else if (type == 4 && (d == 6 || d == 7)) {
            ev.type = LT_EV_WHEEL; ev.wheel = 0; ev.button = (d == 6) ? -1 : 1;
            q_push(x, &ev);
        }
        break;
    }
    case 6:
        ev.type = LT_EV_MOUSE_MOVE;
        ev.x = r16s(x, p + 24); ev.y = r16s(x, p + 26);
        ev.mods = conv_mods((int)r16(x, p + 28));
        if (x->q_count > 0) { /* coalesce motion floods so QUIT/EXPOSE are never dropped */
            lt_event *last = &x->q[(x->q_head + x->q_count - 1) % XQ_CAP];
            if (last->type == LT_EV_MOUSE_MOVE) { *last = ev; break; }
        }
        q_push(x, &ev);
        break;
    case 10:
        if (p[8] == 0) q_simple(x, LT_EV_FOCUS_LOST);
        break;
    case 12:
        if (r16(x, p + 16) == 0) q_simple(x, LT_EV_EXPOSE);
        break;
    case 22: {
        int w = (int)r16(x, p + 20), h = (int)r16(x, p + 22);
        if (w > 0 && h > 0 && (w != x->w || h != x->h)) {
            x->w = w; x->h = h;
            ev.type = LT_EV_RESIZE; ev.x = w; ev.y = h;
            if (x->q_count > 0) {
                lt_event *last = &x->q[(x->q_head + x->q_count - 1) % XQ_CAP];
                if (last->type == LT_EV_RESIZE) { *last = ev; break; }
            }
            q_push(x, &ev);
        }
        break;
    }
    case 33:
        if (p[1] == 32 && r32(x, p + 8) == x->a_prot && r32(x, p + 12) == x->a_del)
            q_simple(x, LT_EV_QUIT);
        break;
    default:
        break;
    }
}

/* consume one complete packet from the input buffer; returns 1 if consumed */
static int process_packet(x11 *x) {
    size_t total;
    lt_u8 *p = x->in;
    int type;
    if (x->in_len < 32) return 0;
    type = p[0] & 0x7F;
    total = 32;
    if (p[0] == 1 || type == 35) total += 4 * (size_t)r32(x, p + 4);
    if (total > 4UL * 1024UL * 1024UL) { /* absurd length: protocol desync */
        fprintf(stderr, "x11: oversized packet, closing\n");
        x->dead = 1;
        x->in_len = 0;
        return 0;
    }
    if (x->in_len < total) return 0;
    if (p[0] == 0) {
        fprintf(stderr, "x11: error code %d major %d minor %d seq %u value 0x%lx\n",
                p[1], p[10], (int)r16(x, p + 8), (unsigned)r16(x, p + 2),
                (unsigned long)r32(x, p + 4));
        if (x->waiting && (int)r16(x, p + 2) == x->want_seq) x->got_err = 1;
    } else if (p[0] == 1) {
        if (x->waiting && (int)r16(x, p + 2) == x->want_seq && !x->got_reply) {
            x->reply = (lt_u8 *)lt_malloc(total);
            memcpy(x->reply, p, total);
            x->reply_len = total;
            x->got_reply = 1;
        }
    } else {
        handle_event(x, p);
    }
    memmove(x->in, x->in + total, x->in_len - total);
    x->in_len -= total;
    return 1;
}

/* read more bytes; returns >0 got data, 0 timeout, -1 closed */
static int fill(x11 *x, int timeout_ms) {
    int n;
    if (x->in_cap - x->in_len < XPKT_MAX) {
        x->in_cap = x->in_len + XPKT_MAX * 2;
        x->in = (lt_u8 *)lt_realloc(x->in, x->in_cap);
    }
    n = lt_stream_read(x->s, x->in + x->in_len, XPKT_MAX, timeout_ms);
    if (n < 0) { x->dead = 1; return -1; }
    x->in_len += (size_t)n;
    return n;
}

/* wait for the reply of request number `seq` (the seq it was sent with) */
static int wait_reply(x11 *x, int seq, int timeout_ms) {
    double end = lt_plat_now_ms() + timeout_ms;
    x->waiting = 1; x->want_seq = seq & 0xFFFF; x->got_reply = 0; x->got_err = 0;
    while (!x->got_reply && !x->got_err) {
        double left;
        if (process_packet(x)) continue;
        left = end - lt_plat_now_ms();
        if (left <= 0 || x->dead) break;
        if (fill(x, (int)left + 1) < 0) break;
    }
    x->waiting = 0;
    return x->got_reply ? 0 : -1;
}

static void drop_reply(x11 *x) {
    if (x->reply) lt_free(x->reply);
    x->reply = NULL; x->reply_len = 0;
}

static lt_u32 alloc_id(x11 *x) {
    lt_u32 id = x->base + x->nextn * x->inc;
    x->nextn++;
    return id;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
static int parse_display(const char *d, char *host, size_t hcap, int *num, char *path,
                         size_t pcap) {
    const char *colon;
    const char *p;
    host[0] = '\0'; path[0] = '\0';
    if (d[0] == '/') { /* full socket path (e.g. XQuartz); number after last ':' */
        lt_strlcpy(path, d, pcap);
        colon = strrchr(d, ':');
        *num = colon ? atoi(colon + 1) : 0;
        return 0;
    }
    colon = strrchr(d, ':');
    if (!colon) return -1;
    if ((size_t)(colon - d) >= hcap) return -1;
    memcpy(host, d, (size_t)(colon - d));
    host[colon - d] = '\0';
    p = colon + 1;
    if (*p == ':') p++; /* DECnet-style "::" ignored */
    if (!isdigit((unsigned char)*p)) return -1;
    *num = atoi(p);
    if (strcmp(host, "unix") == 0) host[0] = '\0';
    return 0;
}

static char *read_hostname(void) {
    const char *files[2];
    int i;
    char *e;
    files[0] = "/proc/sys/kernel/hostname";
    files[1] = "/etc/hostname";
    for (i = 0; i < 2; i++) {
        size_t n;
        char *s = lt_file_read(files[i], &n);
        if (s) { lt_str_trim(s); if (s[0]) return s; lt_free(s); }
    }
    e = getenv("HOSTNAME");
    if (!e) e = getenv("HOST");
    return e ? lt_strdup(e) : NULL;
}

/* find a MIT-MAGIC-COOKIE-1 for display num; returns malloc'd 16+ byte cookie or NULL */
static lt_u8 *find_cookie(const char *host, int num, size_t *clen) {
    char *path = NULL, *env, *data = NULL, *myhost;
    size_t len = 0, pos = 0;
    char numstr[16];
    lt_u8 *best = NULL;
    size_t bestlen = 0;
    int bestscore = 0;
    *clen = 0;
    env = getenv("XAUTHORITY");
    if (env && env[0]) path = lt_strdup(env);
    else {
        char *home = getenv("HOME");
        if (!home) return NULL;
        path = lt_path_join(home, ".Xauthority");
    }
    data = lt_file_read(path, &len);
    lt_free(path);
    if (!data) return NULL;
    myhost = read_hostname();
    lt_snprintf(numstr, sizeof numstr, "%d", num);
    while (pos + 2 <= len) {
        const lt_u8 *u = (const lt_u8 *)data;
        lt_u32 fam, al, nl, kl, dl;
        const lt_u8 *addr, *number, *name, *dat;
        int score = 0;
        fam = lt_rd_be(u + pos, 2); pos += 2;
        if (pos + 2 > len) break;
        al = lt_rd_be(u + pos, 2); pos += 2; addr = u + pos; pos += al;
        if (pos + 2 > len) break;
        nl = lt_rd_be(u + pos, 2); pos += 2; number = u + pos; pos += nl;
        if (pos + 2 > len) break;
        kl = lt_rd_be(u + pos, 2); pos += 2; name = u + pos; pos += kl;
        if (pos + 2 > len) break;
        dl = lt_rd_be(u + pos, 2); pos += 2; dat = u + pos; pos += dl;
        if (pos > len) break;
        if (kl != 18 || memcmp(name, "MIT-MAGIC-COOKIE-1", 18) != 0) continue;
        if (nl != 0 && !(nl == strlen(numstr) && memcmp(number, numstr, nl) == 0)) continue;
        if (fam == 65535UL) score = 3;
        else if (fam == 256) {
            const char *want = host[0] ? host : myhost;
            if (want && strlen(want) == al && memcmp(addr, want, al) == 0) score = 4;
            else score = 1;
        } else score = 1;
        if (score > bestscore) {
            bestscore = score;
            if (best) lt_free(best);
            best = (lt_u8 *)lt_malloc(dl ? dl : 1);
            memcpy(best, dat, dl);
            bestlen = dl;
        }
    }
    if (myhost) lt_free(myhost);
    lt_free(data);
    *clen = bestlen;
    return best;
}

static int do_setup(x11 *x, const char *host, int num) {
    lt_buf b;
    lt_u8 hdr[8];
    lt_u8 *d;
    size_t total, off, i, vlen;
    int nformats, nroots, j, k, found_fmt;
    lt_u8 *cookie;
    size_t clen = 0;
    lt_u32 vis_masks[3];
    lt_u32 want_vis;
    const char *an = "MIT-MAGIC-COOKIE-1";
    int status;

    cookie = find_cookie(host, num, &clen);
    lt_buf_init(&b);
    {
        lt_u32 one = 1;
        x->msb = (*(const lt_u8 *)&one == 0);
    }
    p8(&b, x->msb ? 'B' : 'l'); p8(&b, 0);
    p16(x, &b, 11); p16(x, &b, 0);
    if (cookie && clen) {
        p16(x, &b, 18); p16(x, &b, (lt_u32)clen); p16(x, &b, 0);
        lt_buf_append(&b, an, 18); pad4(&b);
        lt_buf_append(&b, cookie, clen); pad4(&b);
    } else {
        p16(x, &b, 0); p16(x, &b, 0); p16(x, &b, 0);
    }
    if (cookie) lt_free(cookie);
    status = lt_stream_write(x->s, b.data, (int)b.len);
    lt_buf_free(&b);
    if (status < 0) return -1;
    if (lt_stream_read_full(x->s, hdr, 8, 5000) != 8) return -1;
    total = 8 + 4 * (size_t)r16(x, hdr + 6);
    if (hdr[0] != 1) {
        d = (lt_u8 *)lt_calloc(1, total + 1);
        memcpy(d, hdr, 8);
        if ((size_t)hdr[1] > total - 8) hdr[1] = (lt_u8)(total - 8);
        if (total > 8) lt_stream_read_full(x->s, d + 8, (int)(total - 8), 2000);
        d[total] = 0;
        fprintf(stderr, "x11: connection refused: %.*s\n", (int)hdr[1], (char *)d + 8);
        lt_free(d);
        return -1;
    }
    d = (lt_u8 *)lt_malloc(total + 8);
    memcpy(d, hdr, 8);
    if (total > 8 && lt_stream_read_full(x->s, d + 8, (int)(total - 8), 5000) != (int)(total - 8)) {
        lt_free(d);
        return -1;
    }
    if (total < 48) { lt_free(d); return -1; }
    x->base = r32(x, d + 12);
    x->mask = r32(x, d + 16);
    x->maxreq = (int)r16(x, d + 26);
    if (x->maxreq < 16) x->maxreq = 4096;
    nroots = d[28]; nformats = d[29];
    x->img_msb = d[30] != 0;
    x->min_kc = d[34];
    vlen = r16(x, d + 24);
    if (x->mask == 0) { lt_free(d); return -1; }
    x->inc = x->mask & (~x->mask + 1);
    off = 40 + ((vlen + 3) & ~(size_t)3);
    if (off + 8 * (size_t)nformats > total) { lt_free(d); return -1; }
    {
        size_t fo = off;
        off += 8 * (size_t)nformats;
        if (nroots < 1 || off + 40 > total) { lt_free(d); return -1; }
        x->root = r32(x, d + off);
        x->rootvis = r32(x, d + off + 32);
        x->depth = d[off + 38];
        x->w = (int)r16(x, d + off + 20);
        x->h = (int)r16(x, d + off + 22);
        x->root_w = x->w; x->root_h = x->h;
        found_fmt = 0;
        for (i = 0; i < (size_t)nformats; i++) {
            if (d[fo + 8 * i] == x->depth) {
                x->bpp = d[fo + 8 * i + 1];
                x->spad = d[fo + 8 * i + 2];
                found_fmt = 1;
            }
        }
    }
    if (!found_fmt) { lt_free(d); return -1; }
    /* locate the root visual */
    want_vis = x->rootvis;
    vis_masks[0] = vis_masks[1] = vis_masks[2] = 0;
    {
        int ndepths = d[off + 39], vclass = -1;
        size_t o = off + 40;
        for (j = 0; j < ndepths && o + 8 <= total; j++) {
            int nv = (int)r16(x, d + o + 2);
            o += 8;
            for (k = 0; k < nv && o + 24 <= total; k++, o += 24) {
                if (r32(x, d + o) == want_vis) {
                    vclass = d[o + 4];
                    vis_masks[0] = r32(x, d + o + 8);
                    vis_masks[1] = r32(x, d + o + 12);
                    vis_masks[2] = r32(x, d + o + 16);
                }
            }
        }
        lt_free(d);
        if (vclass != 4) {
            fprintf(stderr, "x11: root visual is not TrueColor\n");
            return -1;
        }
    }
    if (x->bpp == 32 && (x->depth == 24 || x->depth == 32) && vis_masks[0] == 0xFF0000UL &&
        vis_masks[1] == 0xFF00UL && vis_masks[2] == 0xFFUL)
        return 0;
    if (x->bpp == 16 && x->depth == 16 && vis_masks[0] == 0xF800UL && vis_masks[1] == 0x7E0UL &&
        vis_masks[2] == 0x1FUL)
        return 0;
    fprintf(stderr, "x11: unsupported pixel format (depth %d bpp %d)\n", x->depth, x->bpp);
    return -1;
}

static lt_u32 intern_send(x11 *x, lt_buf *b, const char *name, int *seq) {
    size_t n = strlen(name);
    req_begin(b, 16, 0);
    p16(x, b, (lt_u32)n); p16(x, b, 0);
    lt_buf_append(b, name, n);
    req_end(x, b);
    *seq = x->seq;
    return 0;
}

static int fetch_keymap(x11 *x, lt_buf *b) {
    int seq, count, i;
    x->min_kc = x->min_kc ? x->min_kc : 8;
    count = 256 - x->min_kc;
    req_begin(b, 101, 0);
    p8(b, x->min_kc); p8(b, count); p16(x, b, 0);
    if (req_end(x, b) < 0) return -1;
    seq = x->seq;
    if (wait_reply(x, seq, 5000) < 0) { drop_reply(x); return -1; }
    x->kpk = x->reply[1];
    if (x->kpk > 0) {
        size_t words = (size_t)r32(x, x->reply + 4);
        x->nkc = (int)(words / (size_t)x->kpk);
        if (x->nkc > count) x->nkc = count;
        x->syms = (lt_u32 *)lt_malloc(sizeof(lt_u32) * (size_t)x->nkc * (size_t)x->kpk + 4);
        for (i = 0; i < x->nkc * x->kpk; i++) x->syms[i] = r32(x, x->reply + 32 + 4 * i);
    }
    drop_reply(x);
    return 0;
}

static void x_free(x11 *x) {
    if (!x) return;
    if (x->s) lt_stream_close(x->s);
    if (x->syms) lt_free(x->syms);
    if (x->in) lt_free(x->in);
    if (x->reply) lt_free(x->reply);
    lt_free(x);
}

/* ------------------------------------------------------------------ */
/* backend methods                                                     */
static void change_prop(x11 *x, lt_buf *b, lt_u32 prop, lt_u32 type, int fmt, const void *data,
                        size_t units) {
    size_t bytes = units * (size_t)(fmt / 8);
    req_begin(b, 18, 0);
    p32(x, b, x->win); p32(x, b, prop); p32(x, b, type);
    p8(b, fmt); p8(b, 0); p8(b, 0); p8(b, 0);
    p32(x, b, (lt_u32)units);
    lt_buf_append(b, data, bytes);
    req_end(x, b);
}

static void set_title_impl(lt_backend *be, const char *title) {
    x11 *x = (x11 *)be->impl;
    lt_buf b;
    size_t n;
    if (!x || !x->win || !title) return;
    n = strlen(title);
    if (n > 1024) n = 1024;
    lt_buf_init(&b);
    change_prop(x, &b, 39, 31, 8, title, n);
    if (x->a_netname && x->a_utf8) change_prop(x, &b, x->a_netname, x->a_utf8, 8, title, n);
    lt_buf_free(&b);
}


/* ---- display scale, geometry ---- */
/* "Xft.dpi: 144" -> 144.0; 0.0 when absent or out of range */
double lt_x11_parse_xft_dpi(const char *res) {
    const char *p = res;
    static const char key[] = "xft.dpi:";
    if (!res) return 0.0;
    while (*p) {
        const char *q = p;
        size_t i = 0;
        while (key[i] && q[i] && tolower((unsigned char)q[i]) == key[i]) i++;
        if (key[i] == '\0') {
            double v;
            q += i;
            while (*q == ' ' || *q == '\t') q++;
            v = atof(q);
            return (v >= 24.0 && v <= 1000.0) ? v : 0.0;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0.0;
}

/* GetProperty (op 20) of the whole property; reply stays in x->reply */
static int get_prop(x11 *x, lt_buf *b, lt_u32 win, lt_u32 prop) {
    req_begin(b, 20, 0);
    p32(x, b, win); p32(x, b, prop); p32(x, b, 0);
    p32(x, b, 0); p32(x, b, 16384);
    if (req_end(x, b) < 0) return -1;
    if (wait_reply(x, x->seq, 5000) < 0) { drop_reply(x); return -1; }
    return 0;
}

static void fetch_resources(x11 *x, lt_buf *b) {
    if (get_prop(x, b, x->root, 23) == 0) {
        if (x->reply[1] == 8) {
            size_t n = (size_t)r32(x, x->reply + 16), avail = x->reply_len - 32;
            char *t;
            if (n > avail) n = avail;
            t = (char *)lt_malloc(n + 1);
            memcpy(t, x->reply + 32, n);
            t[n] = '\0';
            x->xft_dpi = lt_x11_parse_xft_dpi(t);
            lt_free(t);
        }
        drop_reply(x);
    }
}

static double x_scale_hint(lt_backend *be) {
    x11 *x = (x11 *)be->impl;
    const char *e;
    double v;
    if (x && x->xft_dpi > 0.0) return x->xft_dpi / 96.0;
    e = getenv("GDK_SCALE");
    if (e && (v = atof(e)) >= 1.0 && v <= 8.0) return v;
    e = getenv("QT_SCALE_FACTOR");
    if (e && (v = atof(e)) >= 0.5 && v <= 8.0) return v;
    return 1.0;
}

static void send_size_hints(x11 *x, lt_buf *b) {
    lt_u32 h[18];
    lt_u8 raw[72];
    int i;
    memset(h, 0, sizeof h);
    if (x->minw > 0 && x->minh > 0) { h[0] |= 16; h[5] = (lt_u32)x->minw; h[6] = (lt_u32)x->minh; }
    if (x->init_flags & LT_GEOM_POS) {
        h[0] |= 4; h[1] = (lt_u32)x->init_x; h[2] = (lt_u32)x->init_y;
    }
    if (!h[0] || !x->win) return;
    for (i = 0; i < 18; i++) {
        if (x->msb) lt_wr_be(raw + 4 * i, 4, h[i]); else lt_wr_le(raw + 4 * i, 4, h[i]);
    }
    change_prop(x, b, 40, 41, 32, raw, 18);
}

static void x_set_min_size(lt_backend *be, int w, int h) {
    x11 *x = (x11 *)be->impl;
    lt_buf b;
    if (!x) return;
    x->minw = w; x->minh = h;
    if (x->win && !x->dead) {
        lt_buf_init(&b);
        send_size_hints(x, &b);
        lt_buf_free(&b);
    }
}

static int x_screen_size(lt_backend *be, int *w, int *h) {
    x11 *x = (x11 *)be->impl;
    if (!x || x->root_w <= 0 || x->root_h <= 0) return 0;
    if (w) *w = x->root_w;
    if (h) *h = x->root_h;
    return 1;
}

static void x_set_initial_geometry(lt_backend *be, int xx, int yy, int flags) {
    x11 *x = (x11 *)be->impl;
    if (!x) return;
    x->init_x = xx; x->init_y = yy; x->init_flags = flags;
}

static int x_get_geometry(lt_backend *be, int *px, int *py, int *maxd) {
    x11 *x = (x11 *)be->impl;
    lt_buf b;
    int ok = 0, mh = 0, mv = 0;
    if (!x || !x->win || x->dead) return 0;
    lt_buf_init(&b);
    req_begin(&b, 40, 0);
    p32(x, &b, x->win); p32(x, &b, x->root); p16(x, &b, 0); p16(x, &b, 0);
    if (req_end(x, &b) == 0 && wait_reply(x, x->seq, 3000) == 0) {
        if (px) *px = r16s(x, x->reply + 12);
        if (py) *py = r16s(x, x->reply + 14);
        ok = 1;
    }
    drop_reply(x);
    if (x->a_wmstate && get_prop(x, &b, x->win, x->a_wmstate) == 0) {
        if (x->reply[1] == 32) {
            size_t n = (size_t)r32(x, x->reply + 16), i;
            if (n > (x->reply_len - 32) / 4) n = (x->reply_len - 32) / 4;
            for (i = 0; i < n; i++) {
                lt_u32 a = r32(x, x->reply + 32 + 4 * i);
                if (a && a == x->a_maxh) mh = 1;
                if (a && a == x->a_maxv) mv = 1;
            }
        }
        drop_reply(x);
    }
    if (maxd) *maxd = (mh && mv) ? 1 : 0;
    lt_buf_free(&b);
    return ok;
}

static int x_open(lt_backend *be, const char *title, int w, int h) {
    x11 *x = (x11 *)be->impl;
    lt_buf b;
    lt_u8 tmp[4];
    const char *cls = "libretune\0LibreTune";
    if (!x || x->dead) return -1;
    if (x->win) return 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    x->w = w; x->h = h;
    x->win = alloc_id(x);
    x->gc = alloc_id(x);
    lt_buf_init(&b);
    req_begin(&b, 1, x->depth);
    p32(x, &b, x->win); p32(x, &b, x->root);
    p16(x, &b, (lt_u32)(x->init_flags & LT_GEOM_POS ? x->init_x : 0));
    p16(x, &b, (lt_u32)(x->init_flags & LT_GEOM_POS ? x->init_y : 0));
    p16(x, &b, (lt_u32)w); p16(x, &b, (lt_u32)h);
    p16(x, &b, 0); p16(x, &b, 1); /* border, InputOutput */
    p32(x, &b, x->rootvis);
    p32(x, &b, 0x2 | 0x800);
    p32(x, &b, 0); /* back pixel */
    p32(x, &b, 0x1UL | 0x2 | 0x4 | 0x8 | 0x40 | 0x8000UL | 0x20000UL | 0x200000UL);
    req_end(x, &b);
    /* GC: no values (graphics-exposures default True; turn off via mask 0x10000 = 0) */
    req_begin(&b, 55, 0);
    p32(x, &b, x->gc); p32(x, &b, x->win);
    p32(x, &b, 0x10000UL);
    p32(x, &b, 0);
    req_end(x, &b);
    set_title_impl(be, title ? title : "LibreTune");
    change_prop(x, &b, 67, 31, 8, cls, 19);
    if (x->a_prot && x->a_del) {
        if (x->msb) lt_wr_be(tmp, 4, x->a_del); else lt_wr_le(tmp, 4, x->a_del);
        change_prop(x, &b, x->a_prot, 4, 32, tmp, 1);
    }
    send_size_hints(x, &b);
    if ((x->init_flags & LT_GEOM_MAX) && x->a_wmstate && x->a_maxh && x->a_maxv) {
        lt_u8 at[8];
        if (x->msb) { lt_wr_be(at, 4, x->a_maxh); lt_wr_be(at + 4, 4, x->a_maxv); }
        else { lt_wr_le(at, 4, x->a_maxh); lt_wr_le(at + 4, 4, x->a_maxv); }
        change_prop(x, &b, x->a_wmstate, 4, 32, at, 2);
    }
    req_begin(&b, 8, 0);
    p32(x, &b, x->win);
    req_end(x, &b);
    lt_buf_free(&b);
    return x->dead ? -1 : 0;
}

static void x_close(lt_backend *be) {
    x11 *x = (x11 *)be->impl;
    lt_buf b;
    if (!x) return;
    if (x->win && !x->dead) {
        lt_buf_init(&b);
        req_begin(&b, 60, 0); p32(x, &b, x->gc); req_end(x, &b);
        req_begin(&b, 4, 0); p32(x, &b, x->win); req_end(x, &b);
        lt_buf_free(&b);
    }
    x_free(x);
    be->impl = NULL;
}

static int x_poll(lt_backend *be, lt_event *ev, int timeout_ms) {
    x11 *x = (x11 *)be->impl;
    double end = 0;
    int first = 1;
    if (!x) return 0;
    if (timeout_ms > 0) end = lt_plat_now_ms() + timeout_ms;
    for (;;) {
        int slice;
        if (x->q_count > 0) {
            *ev = x->q[x->q_head];
            x->q_head = (x->q_head + 1) % XQ_CAP;
            x->q_count--;
            return 1;
        }
        if (process_packet(x)) continue;
        if (x->dead) {
            if (first || !x->waiting) {
                x->waiting = 0;
            }
            if (x->win) { x->win = 0; q_simple(x, LT_EV_QUIT); continue; }
            return 0;
        }
        first = 0;
        if (timeout_ms < 0) slice = 1000;
        else if (timeout_ms == 0) slice = 0;
        else {
            double left = end - lt_plat_now_ms();
            if (left <= 0) slice = 0; else slice = (int)left + 1;
        }
        {
            int n = fill(x, slice);
            if (n > 0) continue;
            if (n < 0) continue; /* dead: handled above */
        }
        if (timeout_ms < 0) continue;
        return 0;
    }
}

static void x_size(lt_backend *be, int *w, int *h) {
    x11 *x = (x11 *)be->impl;
    if (w) *w = x ? x->w : 0;
    if (h) *h = x ? x->h : 0;
}

static void x_present(lt_backend *be, lt_canvas *c, lt_cells *cells, lt_rect dirty) {
    x11 *x = (x11 *)be->impl;
    lt_rect full, r;
    lt_buf b;
    int bpp_bytes, limit, cw, rows, y0, x0, yy, xx;
    LT_UNUSED(cells);
    if (!x || !c || !c->px || !x->win || x->dead) return;
    full = lt_rect_make(0, 0, LT_MIN(c->w, x->w), LT_MIN(c->h, x->h));
    r = lt_rect_intersect(dirty, full);
    if (r.w <= 0 || r.h <= 0) return;
    bpp_bytes = x->bpp / 8;
    limit = x->maxreq * 4 - 24 - 8; /* data bytes per request */
    if (limit > 262144 - 32) limit = 262144 - 32;
    cw = LT_MIN(r.w, limit / bpp_bytes);
    if (cw < 1) return;
    rows = limit / ((((cw * x->bpp) + x->spad - 1) / x->spad) * x->spad / 8);
    if (rows < 1) rows = 1;
    lt_buf_init(&b);
    for (y0 = r.y; y0 < r.y + r.h; y0 += rows) {
        int sh = LT_MIN(rows, r.y + r.h - y0);
        for (x0 = r.x; x0 < r.x + r.w; x0 += cw) {
            int sw = LT_MIN(cw, r.x + r.w - x0);
            int rowbytes = (((sw * x->bpp) + x->spad - 1) / x->spad) * x->spad / 8;
            req_begin(&b, 72, 2);
            p32(x, &b, x->win); p32(x, &b, x->gc);
            p16(x, &b, (lt_u32)sw); p16(x, &b, (lt_u32)sh);
            p16(x, &b, (lt_u32)x0); p16(x, &b, (lt_u32)y0);
            p8(&b, 0); p8(&b, x->depth); p8(&b, 0); p8(&b, 0);
            lt_buf_reserve(&b, (size_t)rowbytes * (size_t)sh);
            for (yy = 0; yy < sh; yy++) {
                const lt_u32 *src = c->px + (size_t)(y0 + yy) * (size_t)c->pitch + (size_t)x0;
                lt_u8 *dst;
                size_t base = b.len;
                int written = 0;
                for (xx = 0; xx < rowbytes; xx++) lt_buf_putc(&b, 0);
                dst = (lt_u8 *)b.data + base;
                for (xx = 0; xx < sw; xx++) {
                    lt_u32 px = src[xx];
                    int rr = (int)((px >> 16) & 0xFF), gg = (int)((px >> 8) & 0xFF);
                    int bb = (int)(px & 0xFF);
                    if (x->bpp == 32) {
                        if (x->img_msb) {
                            dst[written] = 0; dst[written + 1] = (lt_u8)rr;
                            dst[written + 2] = (lt_u8)gg; dst[written + 3] = (lt_u8)bb;
                        } else {
                            dst[written] = (lt_u8)bb; dst[written + 1] = (lt_u8)gg;
                            dst[written + 2] = (lt_u8)rr; dst[written + 3] = 0;
                        }
                        written += 4;
                    } else {
                        lt_u32 v = ((lt_u32)(rr >> 3) << 11) | ((lt_u32)(gg >> 2) << 5) |
                                   (lt_u32)(bb >> 3);
                        if (x->img_msb) { dst[written] = (lt_u8)(v >> 8); dst[written + 1] = (lt_u8)v; }
                        else { dst[written] = (lt_u8)v; dst[written + 1] = (lt_u8)(v >> 8); }
                        written += 2;
                    }
                }
            }
            if (req_end(x, &b) < 0) { lt_buf_free(&b); return; }
        }
    }
    lt_buf_free(&b);
}

lt_backend *lt_backend_x11_create(void) {
    const char *disp = getenv("DISPLAY");
    char host[256], path[512], sock[600];
    int num = 0;
    x11 *x;
    lt_backend *be;
    lt_buf b;
    int sq[7], i;
    lt_stream *s = NULL;
    static const char *names[7] = { "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME",
                                    "UTF8_STRING", "_NET_WM_STATE",
                                    "_NET_WM_STATE_MAXIMIZED_HORZ",
                                    "_NET_WM_STATE_MAXIMIZED_VERT" };
    if (!disp || !disp[0]) return NULL;
    if (parse_display(disp, host, sizeof host, &num, path, sizeof path) < 0) return NULL;
    if (path[0]) s = lt_unix_connect(path);
    else if (host[0] == '\0') {
        lt_snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", num);
        s = lt_unix_connect(sock);
        if (!s) s = lt_tcp_connect("127.0.0.1", 6000 + num, 2000);
    } else {
        s = lt_tcp_connect(host, 6000 + num, 3000);
    }
    if (!s) return NULL;
    x = (x11 *)lt_calloc(1, sizeof *x);
    x->s = s;
    if (do_setup(x, host, num) < 0) { x_free(x); return NULL; }
    lt_buf_init(&b);
    if (fetch_keymap(x, &b) < 0) {
        fprintf(stderr, "x11: GetKeyboardMapping failed\n");
    }
    for (i = 0; i < 7; i++) intern_send(x, &b, names[i], &sq[i]);
    for (i = 0; i < 7; i++) {
        lt_u32 atom = 0;
        if (wait_reply(x, sq[i], 5000) == 0) atom = r32(x, x->reply + 8);
        drop_reply(x);
        if (i == 0) x->a_prot = atom;
        else if (i == 1) x->a_del = atom;
        else if (i == 2) x->a_netname = atom;
        else if (i == 3) x->a_utf8 = atom;
        else if (i == 4) x->a_wmstate = atom;
        else if (i == 5) x->a_maxh = atom;
        else x->a_maxv = atom;
    }
    fetch_resources(x, &b);
    lt_buf_free(&b);
    if (x->dead) { x_free(x); return NULL; }
    be = (lt_backend *)lt_calloc(1, sizeof *be);
    be->name = "x11";
    be->cell_mode = LT_FALSE;
    be->open = x_open;
    be->close = x_close;
    be->poll = x_poll;
    be->present = x_present;
    be->size = x_size;
    be->set_title = set_title_impl;
    be->set_clipboard = NULL;
    be->get_clipboard = NULL;
    be->scale_hint = x_scale_hint;
    be->set_min_size = x_set_min_size;
    be->screen_size = x_screen_size;
    be->set_initial_geometry = x_set_initial_geometry;
    be->get_geometry = x_get_geometry;
    be->impl = x;
    return be;
}
