/* proto.c - ECU connection: framing exchange, handshake, pages, burn, realtime.
 * Spec: docs/spec/03-protocol.md sections 6-9.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "proto.h"

#include <string.h>

#define ADAPT_MAX 64

/* ---- asynchronous exchange state (see "asynchronous requests" in proto.h) ---- */
#define AX_IDLE  0
#define AX_WAIT  1      /* request sent, collecting the reply */
#define AX_DRAIN 2      /* a framed reply failed part way: discard its tail, then flush input */
#define AXK_RT    1
#define AXK_WRITE 2
#define AXK_RAW   3

typedef struct ax_state {
    int     phase, kind;
    lt_bool framed;          /* msEnvelope reply (else legacy raw) */
    lt_bool allow_empty;     /* legacy: silence until the timeout is an empty reply, no error */
    size_t  expected;        /* legacy: complete at this many bytes (0 = by silence) */
    size_t  need;            /* framed: 2 until the length is known, then 2+N+4 */
    size_t  pl_off, pl_len;  /* the decoded payload inside rx */
    size_t  want;            /* realtime: ochBlockSize (status byte strip) */
    lt_bool burst;
    int     timeout_ms, quiet_ms, pace_ms;
    double  t0, deadline, last_rx;
    size_t  drain_left;
    double  drain_until, drain_last;
    lt_buf  rx;
} ax_state;

typedef struct rt_result {   /* a finished request waiting for its pump */
    int ready, rc;
    unsigned long seq;
    lt_proto_err err;
    lt_buf data;
} rt_result;

typedef struct wr_chunk {
    int    page;
    size_t off, len;
    int    done;
} wr_chunk;

typedef struct wr_job {      /* the asynchronous RAM writer */
    lt_ecu_mem *mem;
    wr_chunk   *ch;
    size_t      n, cap, next;    /* chunks [0,next) were sent */
    int         inflight;        /* chunk next-1 waits for its acknowledgement */
    size_t      cmd_len;         /* its frame length (pacing) */
    int         burn_page;       /* >= 0: auto-burn sent, settling until next_tx_at */
    int         result;          /* 0 none, 1 all written, <0 failed (collected by write_pump) */
    lt_proto_err err;
} wr_job;

struct lt_proto {
    lt_proto_io    io;
    const lt_ini  *ini;
    lt_proto_cfg   cfg;
    lt_proto_clock clk;
    lt_proto_progress_fn prog;
    void          *prog_ud;
    volatile int   cancel;
    lt_conn_state  state;
    char           sig[128];
    lt_bool        modern;
    lt_env_order   order, declared_order;
    lt_proto_stats stats;
    lt_proto_err   err;
    int            last_written;
    lt_u8          dirty[256];
    lt_u8          failed[256];
    char           reason[96];
    /* adaptive timing (spec 6.3) */
    double         samples_us[ADAPT_MAX];
    int            n_samples, head;
    double         sum_us;
    double         cur_timeout;
    /* asynchronous requests */
    ax_state       ax;
    rt_result      rtr, rawr;       /* realtime / raw command results */
    wr_job         job;
    double         next_tx_at;      /* pacing: nothing is sent before this time */
    int            want_kind;       /* a request of this kind was refused: it goes next ... */
    double         want_at;         /* ... for 100 ms */
};

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */
void lt_proto_cfg_default(lt_proto_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->timeout_ms = LT_DEFAULT_TIMEOUT_MS;
    c->baud = LT_DEFAULT_BAUD;
    c->port_name = NULL;
    c->runtime_mode = LT_RT_AUTO;
    c->permissive_crc = LT_FALSE;
    c->auto_burn_on_page_change = LT_TRUE;
    c->adaptive_timing = LT_FALSE;
    c->env_order_hint = -1;
    c->legacy_read_min_wait_ms = 10;
}

static double now_ms(const lt_proto *p)
{
    return p->clk.now_ms ? p->clk.now_ms(p->clk.ud) : lt_plat_now_ms();
}

static void sleep_ms(const lt_proto *p, int ms)
{
    if (ms <= 0) {
        return;
    }
    if (p->clk.sleep_ms) {
        p->clk.sleep_ms(p->clk.ud, ms);
    } else {
        lt_plat_sleep_ms(ms);
    }
}

static int fail(lt_proto *p, int rc, lt_proto_errcode pc, const char *fmt, ...)
{
    va_list ap;

    memset(&p->err, 0, sizeof(p->err));
    p->err.code = pc;
    va_start(ap, fmt);
    lt_vsnprintf(p->err.msg, sizeof(p->err.msg), fmt, ap);
    va_end(ap);
    lt_err_set("%s", p->err.msg);
    return rc;
}

/* cancel seen: report. A cancel of a connected session is consumed so the
 * next call works (disconnect leaves the flag raised, state != CONNECTED). */
static int cancelled(lt_proto *p)
{
    if (p->state == LT_CONN_CONNECTED) {
        p->cancel = 0;
    }
    return fail(p, LT_ECANCEL, LT_PE_CANCELLED, "operation canceled");
}

static int progress(lt_proto *p, const char *phase, int page, size_t done, size_t total)
{
    if (p->cancel) {
        return cancelled(p);
    }
    if (p->prog && p->prog(p->prog_ud, phase, page, done, total) != 0) {
        return cancelled(p);
    }
    return LT_OK;
}

static int need_conn(lt_proto *p)
{
    if (p->state != LT_CONN_CONNECTED) {
        return fail(p, LT_ENOTCONN, LT_PE_NOT_CONNECTED, "not connected");
    }
    return LT_OK;
}

static lt_bool contains_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle), i, len = strlen(s);

    for (i = 0; i + n <= len; i++) {
        if (lt_strnicmp(s + i, needle, n) == 0) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

/* ---- adaptive timing ---- */
static lt_bool adaptive_on(const lt_proto *p)
{
    return p->cfg.adaptive_timing ? LT_TRUE : LT_FALSE;
}

static void adapt_record(lt_proto *p, double elapsed_ms)
{
    double us = elapsed_ms * 1000.0, avg, t;
    int cap = 20;

    if (!adaptive_on(p)) {
        return;
    }
    if (p->n_samples == cap) {
        p->sum_us -= p->samples_us[p->head];
        p->samples_us[p->head] = us;
        p->head = (p->head + 1) % cap;
    } else {
        p->samples_us[(p->head + p->n_samples) % cap] = us;
        p->n_samples++;
    }
    p->sum_us += us;
    avg = p->sum_us / (double)p->n_samples;
    t = (double)(unsigned long)(avg * 2.5) / 1000.0;
    if (t < 10.0) {
        t = 10.0;
    }
    if (t > 500.0) {
        t = 500.0;
    }
    p->cur_timeout = t;
}

static void adapt_reset(lt_proto *p)
{
    p->n_samples = 0;
    p->head = 0;
    p->sum_us = 0.0;
    p->cur_timeout = 375.0;
}

static double adapt_avg_ms(const lt_proto *p)
{
    return p->n_samples > 0 ? p->sum_us / (double)p->n_samples / 1000.0 : 0.0;
}

/* INI delays are unsigned long: keep them in a sane int range. */
static int ini_ms(unsigned long v)
{
    return v > 600000UL ? 600000 : (int)v;
}

int lt_proto_effective_timeout_ms(const lt_proto *p)
{
    if (adaptive_on(p)) {
        return p->n_samples > 0 ? (int)p->cur_timeout : 500;
    }
    if (p->ini && p->ini->proto.block_read_timeout > 0) {
        return ini_ms(p->ini->proto.block_read_timeout);
    }
    return p->cfg.timeout_ms > 0 ? p->cfg.timeout_ms : LT_DEFAULT_TIMEOUT_MS;
}

static int inter_char_ms(const lt_proto *p)
{
    int t, v;

    if (adaptive_on(p)) {
        t = lt_proto_effective_timeout_ms(p) / 4;
        return t < 5 ? 5 : t;
    }
    v = (p->ini && p->ini->proto.block_read_timeout > 0) ?
        ini_ms(p->ini->proto.block_read_timeout) : 1000;
    v /= 4;
    return LT_CLAMP(v, 25, 100);
}

static int min_wait_ms(const lt_proto *p)
{
    int t, v;

    if (adaptive_on(p)) {
        t = lt_proto_effective_timeout_ms(p) / 3;
        return t < 5 ? 5 : t;
    }
    v = p->ini ? ini_ms(p->ini->proto.inter_write_delay) : 10;
    return v < 10 ? 10 : v;
}

static long baud_of(const lt_proto *p)
{
    return p->cfg.baud > 0 ? p->cfg.baud : LT_DEFAULT_BAUD;
}

int lt_proto_effective_write_chunk(const lt_proto *p)
{
    long bf = p->ini ? (long)p->ini->proto.blocking_factor : 256L;

    if (bf < 1) {
        bf = 1;
    }
    bf -= 8;
    return bf < 1 ? 1 : (int)bf;
}

static int read_chunk(const lt_proto *p)
{
    long bf = p->ini ? (long)p->ini->proto.blocking_factor : 256L;

    return bf < 1 ? 256 : (int)bf;
}

int lt_proto_inter_frame_delay_ms(const lt_proto *p, size_t frame_len)
{
    long baud = baud_of(p);
    long wire = (long)((frame_len * 10UL * 1000UL + (unsigned long)baud - 1UL) /
                       (unsigned long)baud);
    int settle = min_wait_ms(p);
    int floor_ms = p->modern ? 5 : LT_LEGACY_WRITE_SETTLE_MS;

    if (settle < floor_ms) {
        settle = floor_ms;
    }
    return wire > settle ? (int)wire : settle;
}

/* ---------------------------------------------------------------------- */
/* io adapter                                                              */
/* ---------------------------------------------------------------------- */
static int st_read(void *ud, void *buf, int n, int timeout_ms)
{
    return lt_stream_read((lt_stream *)ud, buf, n, timeout_ms);
}

static int st_write(void *ud, const void *buf, int n)
{
    return lt_stream_write((lt_stream *)ud, buf, n);
}

static void st_clear(void *ud)
{
    lt_stream_flush_input((lt_stream *)ud);
}

static void st_close(void *ud)
{
    lt_stream_close((lt_stream *)ud);
}

lt_proto_io lt_proto_io_from_stream(lt_stream *s, lt_bool own)
{
    lt_proto_io io;

    memset(&io, 0, sizeof(io));
    io.ud = s;
    io.read = st_read;
    io.write = st_write;
    io.flush = NULL;
    io.clear_input = st_clear;
    io.close = own ? st_close : NULL;
    return io;
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                               */
/* ---------------------------------------------------------------------- */
lt_proto *lt_proto_new(const lt_proto_io *io, const lt_ini *ini, const lt_proto_cfg *cfg)
{
    lt_proto *p = (lt_proto *)lt_calloc(1, sizeof(*p));

    p->io = *io;
    p->ini = ini;
    if (cfg) {
        p->cfg = *cfg;
    } else {
        lt_proto_cfg_default(&p->cfg);
    }
    p->state = LT_CONN_DISCONNECTED;
    p->last_written = -1;
    p->modern = (ini && ini->proto.uses_modern) ? LT_TRUE : LT_FALSE;
    if (p->cfg.env_order_hint == 0 || p->cfg.env_order_hint == 1) {
        p->declared_order = (lt_env_order)p->cfg.env_order_hint;
    } else if (ini && ini->ecu_type == LT_ECU_SPEEDUINO) {
        p->declared_order = LT_ENV_LITTLE;
    } else {
        p->declared_order = LT_ENV_BIG;
    }
    p->order = p->declared_order;
    p->cur_timeout = 375.0;
    lt_buf_init(&p->ax.rx);
    lt_buf_init(&p->rtr.data);
    lt_buf_init(&p->rawr.data);
    p->job.burn_page = -1;
    return p;
}

static void async_abort(lt_proto *p);

void lt_proto_free(lt_proto *p)
{
    if (!p) {
        return;
    }
    async_abort(p);
    lt_buf_free(&p->ax.rx);
    lt_buf_free(&p->rtr.data);
    lt_buf_free(&p->rawr.data);
    lt_free(p->job.ch);
    lt_free(p);
}

void lt_proto_close(lt_proto *p)
{
    if (!p) {
        return;
    }
    async_abort(p);
    p->cancel = 1;
    p->state = LT_CONN_DISCONNECTED;
    if (p->io.close) {
        p->io.close(p->io.ud);
        p->io.close = NULL;
    }
}

void lt_proto_set_progress(lt_proto *p, lt_proto_progress_fn fn, void *ud)
{
    p->prog = fn;
    p->prog_ud = ud;
}

void lt_proto_set_clock(lt_proto *p, const lt_proto_clock *clk)
{
    if (clk) {
        p->clk = *clk;
    } else {
        memset(&p->clk, 0, sizeof(p->clk));
    }
}

volatile int *lt_proto_cancel_flag(lt_proto *p)
{
    return &p->cancel;
}

void lt_proto_cancel(lt_proto *p)
{
    p->cancel = 1;
}

const lt_proto_err *lt_proto_errinfo(const lt_proto *p)
{
    return &p->err;
}

lt_conn_state lt_proto_state(const lt_proto *p)
{
    return p->state;
}

const char *lt_proto_signature(const lt_proto *p)
{
    return p->sig;
}

lt_bool lt_proto_is_modern(const lt_proto *p)
{
    return p->modern;
}

lt_env_order lt_proto_envelope_order(const lt_proto *p)
{
    return p->order;
}

const lt_proto_stats *lt_proto_stats_get(const lt_proto *p)
{
    return &p->stats;
}

/* ---------------------------------------------------------------------- */
/* low level I/O                                                           */
/* ---------------------------------------------------------------------- */
/* Blocking paths call this before they touch the wire: whatever asynchronous work is in
 * flight is completed first, so two exchanges can never share the byte stream. */
static void settle(lt_proto *p);

static lt_bool async_busy(const lt_proto *p)
{
    return (p->ax.phase != AX_IDLE || p->job.n > 0 || p->job.burn_page >= 0 ||
            p->next_tx_at > 0.0) ? LT_TRUE : LT_FALSE;
}

static void io_settle(lt_proto *p)
{
    if (async_busy(p)) {
        settle(p);
    }
}

/* Unguarded primitives: the asynchronous machinery itself (and blocking paths that settled). */
static void rx_clear_now(lt_proto *p)
{
    lt_u8 tmp[256];
    int guard = 0, r;

    if (p->io.clear_input) {
        p->io.clear_input(p->io.ud);
        return;
    }
    while (guard++ < 4096) {
        r = p->io.read(p->io.ud, tmp, (int)sizeof(tmp), 0);
        if (r <= 0) {
            break;
        }
    }
}

static int raw_write(lt_proto *p, const void *data, size_t n)
{
    if (n == 0) {
        return LT_OK;
    }
    if (p->io.write(p->io.ud, data, (int)n) != (int)n) {
        return fail(p, LT_EIO, LT_PE_SERIAL, "write failed");
    }
    p->stats.tx_bytes += (unsigned long)n;
    return LT_OK;
}

static void clear_rx(lt_proto *p)
{
    io_settle(p);
    rx_clear_now(p);
}

static int write_all(lt_proto *p, const void *data, size_t n)
{
    io_settle(p);
    return raw_write(p, data, n);
}

/* What write_and_wait sleeps after sending n bytes (spec 6.2). */
static int pace_ms(const lt_proto *p, size_t n)
{
    int wait, tx = (int)(((unsigned long)n * 10UL * 1000UL) / (unsigned long)baud_of(p));

    wait = tx + 5;
    return wait < min_wait_ms(p) ? min_wait_ms(p) : wait;
}

static int write_and_wait(lt_proto *p, const void *data, size_t n)
{
    int rc;

    rc = write_all(p, data, n);
    if (rc) {
        return rc;
    }
    if (p->io.flush) {
        p->io.flush(p->io.ud);
    }
    sleep_ms(p, pace_ms(p, n));
    return LT_OK;
}

/* Read exactly n bytes before `deadline`. */
static int rd_exact(lt_proto *p, lt_u8 *buf, size_t n, double deadline)
{
    size_t have = 0, want;
    double t, rem;
    int r, slice;

    io_settle(p);
    while (have < n) {
        if (p->cancel) {
            return cancelled(p);
        }
        t = now_ms(p);
        rem = deadline - t;
        if (rem <= 0.0) {
            return fail(p, LT_ETIMEOUT, LT_PE_TIMEOUT, "response timeout");
        }
        slice = rem > 20.0 ? 20 : (int)rem + 1;
        want = n - have;
        if (want > 4096) {
            want = 4096;
        }
        r = p->io.read(p->io.ud, buf + have, (int)want, slice);
        if (r < 0) {
            return fail(p, LT_EIO, LT_PE_SERIAL, "read failed (connection closed)");
        }
        if (r == 0) {
            if (now_ms(p) - t < 1.0) {
                sleep_ms(p, 1);
            }
            continue;
        }
        have += (size_t)r;
        p->stats.rx_bytes += (unsigned long)r;
    }
    return LT_OK;
}

static void drain_n(lt_proto *p, size_t n)
{
    lt_u8 tmp[256];
    double deadline;
    int r;

    io_settle(p);
    deadline = now_ms(p) + 500.0;
    while (n > 0 && now_ms(p) < deadline) {
        r = p->io.read(p->io.ud, tmp, (int)(n > sizeof(tmp) ? sizeof(tmp) : n), 10);
        if (r <= 0) {
            break;
        }
        n -= (size_t)r;
    }
    clear_rx(p);
}

static lt_u32 rd_order(lt_env_order o, const lt_u8 *b, int n)
{
    return o == LT_ENV_LITTLE ? lt_rd_le(b, n) : lt_rd_be(b, n);
}

/* Enveloped exchange: payload -> reply payload (appended to `reply`). */
static int send_packet(lt_proto *p, const lt_u8 *payload, size_t n, lt_buf *reply)
{
    lt_buf f;
    lt_u8 hdr[2];
    lt_u8 *body;
    size_t N;
    double t0, deadline;
    int rc;
    const lt_u8 *pl;
    size_t pn;

    t0 = now_ms(p);
    lt_buf_init(&f);
    lt_env_encode(p->order, payload, n, &f);
    p->stats.tx_packets++;
    rc = write_and_wait(p, f.data, f.len);
    lt_buf_free(&f);
    if (rc) {
        return rc;
    }
    deadline = now_ms(p) + (double)lt_proto_effective_timeout_ms(p);
    rc = rd_exact(p, hdr, 2, deadline);
    if (rc) {
        clear_rx(p);
        adapt_reset(p);
        return rc;
    }
    N = (size_t)rd_order(p->order, hdr, 2);
    body = (lt_u8 *)lt_malloc(2 + N + 4);
    body[0] = hdr[0];
    body[1] = hdr[1];
    rc = rd_exact(p, body + 2, N + 4, deadline);
    if (rc) {
        drain_n(p, N + 4);
        adapt_reset(p);
        lt_free(body);
        return rc;
    }
    adapt_record(p, now_ms(p) - t0);
    p->stats.rx_packets++;
    rc = lt_env_decode(p->order, p->cfg.permissive_crc, body, 2 + N + 4, &pl, &pn, NULL);
    if (rc == LT_OK) {
        if (reply) {
            lt_buf_append(reply, pl, pn);
        }
    } else if (rc == LT_ECRC) {
        char m[200];

        lt_strlcpy(m, lt_err_get(), sizeof(m));
        fail(p, LT_ECRC, LT_PE_CRC_MISMATCH, "%s", m);
        p->err.a = lt_proto_crc32(body + 2, N);
        p->err.b = rd_order(p->order, body + 2 + N, 4);
    } else {
        fail(p, rc, LT_PE_INVALID_RESPONSE, "%s", lt_err_get());
    }
    lt_free(body);
    return rc;
}

static int send_packet_no_response(lt_proto *p, const lt_u8 *payload, size_t n)
{
    lt_buf f;
    int rc;

    lt_buf_init(&f);
    lt_env_encode(p->order, payload, n, &f);
    p->stats.tx_packets++;
    rc = write_all(p, f.data, f.len);
    lt_buf_free(&f);
    if (rc == 0 && p->io.flush) {
        p->io.flush(p->io.ud);
    }
    return rc;
}

/* Legacy raw exchange. expected 0 = unknown. quiet_ms: end after this much
 * silence once data arrived. */
static int raw_exchange(lt_proto *p, const lt_u8 *cmd, size_t n, size_t expected,
                        int timeout_ms, int quiet_ms, lt_bool allow_empty, lt_buf *out)
{
    double t0, last, t, tb;
    lt_u8 tmp[512];
    size_t total = 0;
    int r, rc, poll = adaptive_on(p) ? 1 : 2;

    clear_rx(p);
    t0 = now_ms(p);
    rc = write_and_wait(p, cmd, n);
    if (rc) {
        return rc;
    }
    p->stats.tx_packets++;
    last = t0;
    for (;;) {
        t = now_ms(p);
        if (t - t0 > (double)timeout_ms) {
            break;
        }
        if (p->cancel) {
            adapt_reset(p);
            return cancelled(p);
        }
        tb = t;
        r = p->io.read(p->io.ud, tmp, (int)sizeof(tmp), poll);
        if (r < 0) {
            adapt_reset(p);
            return fail(p, LT_EIO, LT_PE_SERIAL, "read failed (connection closed)");
        }
        if (r > 0) {
            lt_buf_append(out, tmp, (size_t)r);
            total += (size_t)r;
            p->stats.rx_bytes += (unsigned long)r;
            last = now_ms(p);
            if (expected > 0 && total >= expected) {
                break;
            }
            continue;
        }
        if (now_ms(p) - tb < 1.0) {
            sleep_ms(p, 1);
        }
        if (total > 0 && now_ms(p) - last > (double)quiet_ms) {
            break;
        }
    }
    if (total == 0 && !allow_empty) {
        adapt_reset(p);
        return fail(p, LT_ETIMEOUT, LT_PE_TIMEOUT, "response timeout");
    }
    if (total > 0) {
        p->stats.rx_packets++;
        adapt_record(p, now_ms(p) - t0);
    }
    return LT_OK;
}

/* ---- pure helpers (spec 6.4, 7.1, 8, 9) ---- */
lt_bool lt_proto_response_complete(size_t received, size_t expected)
{
    return (expected > 0 && received >= expected) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_proto_signature_from_payload(const lt_u8 *pl, size_t n, char *sig, size_t cap)
{
    size_t b = 0, e = n, k;

    if (n == 0 || lt_rc_is_error(pl[0])) {
        return LT_FALSE;
    }
    if (pl[0] == 0) {
        b = 1;
    }
    while (b < e && (pl[b] == ' ' || pl[b] == '\t' || pl[b] == '\r' || pl[b] == '\n' ||
                     pl[b] == '\f' || pl[b] == '\v')) {
        b++;
    }
    while (e > b && (pl[e - 1] == ' ' || pl[e - 1] == '\t' || pl[e - 1] == '\r' ||
                     pl[e - 1] == '\n' || pl[e - 1] == '\f' || pl[e - 1] == '\v' ||
                     pl[e - 1] == 0)) {
        e--;
    }
    if (e <= b) {
        return LT_FALSE;
    }
    if (cap > 0) {
        k = e - b;
        if (k > cap - 1) {
            k = cap - 1;
        }
        memcpy(sig, pl + b, k);
        sig[k] = '\0';
    }
    return LT_TRUE;
}

int lt_proto_check_write_status(const lt_u8 *pl, size_t n, char *msg, size_t cap, int *code)
{
    size_t k;

    if (code) {
        *code = 0;
    }
    if (msg && cap > 0) {
        msg[0] = '\0';
    }
    if (n == 0 || !lt_rc_is_error(pl[0])) {
        return LT_OK;
    }
    if (code) {
        *code = pl[0];
    }
    if (msg && cap > 0) {
        if (lt_rc_carries_message(pl[0]) && n > 1) {
            k = n - 1;
            if (k > cap - 1) {
                k = cap - 1;
            }
            memcpy(msg, pl + 1, k);
            msg[k] = '\0';
        } else {
            lt_strlcpy(msg, lt_rc_message(pl[0]), cap);
        }
    }
    return LT_EPROTO;
}

int lt_proto_strip_status(const lt_u8 *pl, size_t n, size_t want, const char *label,
                          size_t *dlen)
{
    if (want > 0) {
        if (n == want + 1) {
            *dlen = want;
            return 1;
        }
        if (n == want) {
            *dlen = want;
            return 0;
        }
        lt_err_set("%s response: %lu bytes, expected %lu (or %lu with a status byte) - channel "
                   "offsets cannot be trusted", label, (unsigned long)n, (unsigned long)want,
                   (unsigned long)(want + 1));
        return LT_EPROTO;
    }
    if (n == 0) {
        *dlen = 0;
        return 0;
    }
    if (pl[0] == 0) {
        *dlen = n - 1;
        return 1;
    }
    *dlen = n;
    return 0;
}

static int status_error(lt_proto *p, const lt_u8 *pl, size_t n)
{
    char msg[200];
    int code = 0, rc;

    rc = lt_proto_check_write_status(pl, n, msg, sizeof(msg), &code);
    if (rc == LT_OK) {
        return LT_OK;
    }
    fail(p, LT_EPROTO, LT_PE_ECU_STATUS, "ECU error 0x%02x: %s", (unsigned)code, msg);
    p->err.a = (lt_u32)code;
    return LT_EPROTO;
}

/* ---------------------------------------------------------------------- */
/* handshake / connect                                                     */
/* ---------------------------------------------------------------------- */
static void trim_copy(const lt_u8 *b, size_t n, char *sig, size_t cap)
{
    size_t s = 0, e = n, k;

    while (s < e && (b[s] == ' ' || b[s] == '\t' || b[s] == '\r' || b[s] == '\n' || b[s] == 0)) {
        s++;
    }
    while (e > s && (b[e - 1] == ' ' || b[e - 1] == '\t' || b[e - 1] == '\r' ||
                     b[e - 1] == '\n' || b[e - 1] == 0)) {
        e--;
    }
    k = e - s;
    if (cap == 0) {
        return;
    }
    if (k > cap - 1) {
        k = cap - 1;
    }
    memcpy(sig, b + s, k);
    sig[k] = '\0';
}

int lt_proto_handshake(lt_proto *p, char *sig, size_t cap)
{
    lt_buf cmd, reply;
    lt_bool ini_modern = (p->ini && p->ini->proto.uses_modern) ? LT_TRUE : LT_FALSE;
    lt_u8 cmd_byte;
    int attempt, rc = LT_ETIMEOUT, rc2;
    lt_env_order declared = p->declared_order;
    char tmp[128];
    lt_proto_err saved;

    lt_buf_init(&cmd);
    lt_buf_init(&reply);
    if (p->ini && p->ini->proto.query_command.n > 0) {
        lt_cmd_parse_string(p->ini->proto.query_command.p, p->ini->proto.query_command.n, &cmd);
    } else {
        lt_buf_putc(&cmd, 'S');
    }
    cmd_byte = cmd.len > 0 ? (lt_u8)cmd.data[0] : (lt_u8)'Q';
    if (ini_modern) {
        for (attempt = 0; attempt < 2; attempt++) {
            p->order = attempt == 0 ? declared :
                       (declared == LT_ENV_BIG ? LT_ENV_LITTLE : LT_ENV_BIG);
            if (attempt > 0) {
                sleep_ms(p, 450);
                clear_rx(p);
            }
            lt_buf_clear(&reply);
            rc = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
            if (rc == LT_ECANCEL) {
                goto done;
            }
            if (rc == LT_OK &&
                lt_proto_signature_from_payload((const lt_u8 *)reply.data, reply.len, tmp,
                                                sizeof(tmp))) {
                p->modern = LT_TRUE;
                lt_strlcpy(p->sig, tmp, sizeof(p->sig));
                lt_strlcpy(sig, tmp, cap);
                rc = LT_OK;
                goto done;
            }
        }
        p->order = declared;
    }
    /* legacy */
    clear_rx(p);
    lt_buf_clear(&reply);
    rc = raw_exchange(p, &cmd_byte, 1, 0, lt_proto_effective_timeout_ms(p), inter_char_ms(p),
                      LT_FALSE, &reply);
    if (rc == LT_OK) {
        p->modern = LT_FALSE;
        trim_copy((const lt_u8 *)reply.data, reply.len, tmp, sizeof(tmp));
        lt_strlcpy(p->sig, tmp, sizeof(p->sig));
        lt_strlcpy(sig, tmp, cap);
        goto done;
    }
    if (rc == LT_ECANCEL) {
        goto done;
    }
    saved = p->err;
    if (!ini_modern) {
        clear_rx(p);
        sleep_ms(p, 50);
        lt_buf_clear(&reply);
        rc2 = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
        if (rc2 == LT_OK &&
            lt_proto_signature_from_payload((const lt_u8 *)reply.data, reply.len, tmp,
                                            sizeof(tmp))) {
            p->modern = LT_TRUE;
            lt_strlcpy(p->sig, tmp, sizeof(p->sig));
            lt_strlcpy(sig, tmp, cap);
            rc = LT_OK;
            goto done;
        }
    }
    p->err = saved;
    lt_err_set("%s", saved.msg);
done:
    lt_buf_free(&cmd);
    lt_buf_free(&reply);
    return rc;
}

int lt_proto_connect(lt_proto *p)
{
    char sig[128];
    int rc, delay;

    if (p->state == LT_CONN_CONNECTED) {
        return fail(p, LT_EBUSY, LT_PE_ALREADY_CONNECTED, "already connected");
    }
    async_abort(p);
    p->cancel = 0;
    p->state = LT_CONN_CONNECTING;
    p->sig[0] = '\0';
    delay = p->ini ? ini_ms(p->ini->proto.delay_after_port_open) : 1000;
    sleep_ms(p, delay);
    clear_rx(p);
    sleep_ms(p, 20);
    sig[0] = '\0';
    rc = lt_proto_handshake(p, sig, sizeof(sig));
    if (rc != LT_OK) {
        p->state = LT_CONN_ERROR;
        return rc;
    }
    p->state = LT_CONN_CONNECTED;
    p->last_written = -1;
    memset(p->dirty, 0, sizeof(p->dirty));
    memset(p->failed, 0, sizeof(p->failed));
    return LT_OK;
}

void lt_proto_disconnect(lt_proto *p)
{
    async_abort(p);
    p->cancel = 1;
    p->state = LT_CONN_DISCONNECTED;
    p->sig[0] = '\0';
}

lt_bool lt_proto_signature_mismatch(const lt_proto *p)
{
    const char *want, *pre;
    size_t pn;

    if (!p->ini || !p->ini->signature || !p->sig[0]) {
        return LT_FALSE;
    }
    want = p->ini->signature;
    if (lt_stricmp(want, p->sig) == 0) {
        return LT_FALSE;
    }
    pre = p->ini->signature_prefix;
    if (pre && pre[0]) {
        pn = strlen(pre);
        if (lt_strnicmp(want, pre, pn) == 0 && lt_strnicmp(p->sig, pre, pn) == 0) {
            return LT_FALSE;
        }
    }
    return LT_TRUE;
}

/* ---------------------------------------------------------------------- */
/* page commands                                                           */
/* ---------------------------------------------------------------------- */
static void get_cmd(const lt_ini_cmdlist *l, int idx, const char *dflt, const char **s,
                    size_t *n)
{
    if (l && l->v && idx >= 0 && (size_t)idx < l->n) {
        *s = l->v[idx].p ? l->v[idx].p : "";
        *n = l->v[idx].n;
    } else {
        *s = dflt;
        *n = strlen(dflt);
    }
}

static lt_u32 page_id(const lt_proto *p, int idx)
{
    const lt_ini_protocol *pr;

    if (p->ini && idx >= 0) {
        pr = &p->ini->proto;
        if ((size_t)idx < pr->n_page_identifiers && pr->page_identifiers) {
            const lt_ini_blob *b = &pr->page_identifiers[idx];

            if (b->n >= 2) {
                return lt_rd_le((const lt_u8 *)b->p, 2);
            }
            if (b->n == 1) {
                return (lt_u8)b->p[0];
            }
        }
    }
    return (lt_u32)idx;
}

static lt_bool cmd_le(const lt_proto *p)
{
    return (p->ini && p->ini->endian == LT_ENDIAN_LITTLE) ? LT_TRUE : LT_FALSE;
}

static int build_page_cmd(lt_proto *p, const lt_ini_cmdlist *l, const char *dflt, int page,
                          lt_u32 off, lt_u32 count, const lt_u8 *data, size_t nd,
                          const char *what, lt_buf *out)
{
    const char *f;
    size_t fn;

    get_cmd(l, page, dflt, &f, &fn);
    if (fn == 0) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL, "No %s command for page %d", what, page);
    }
    lt_cmd_build(out, f, fn, page_id(p, page), off, count, data, nd, cmd_le(p));
    return LT_OK;
}

#define PRO(p) (&(p)->ini->proto)
#define LIST(p, f) ((p)->ini ? &PRO(p)->f : NULL)

int lt_proto_read_memory(lt_proto *p, int page, size_t offset, size_t len, lt_buf *out)
{
    lt_buf cmd, reply;
    int rc;
    const lt_u8 *d;
    size_t dn;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    lt_buf_init(&cmd);
    lt_buf_init(&reply);
    rc = build_page_cmd(p, LIST(p, page_read_commands), "R%2i%2o%2c", page, (lt_u32)offset,
                        (lt_u32)len, NULL, 0, "read", &cmd);
    if (rc) {
        goto done;
    }
    if (p->modern) {
        rc = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
        if (rc) {
            goto done;
        }
        if (reply.len == 0) {
            rc = fail(p, LT_EDATA, LT_PE_INVALID_RESPONSE, "empty read response");
            goto done;
        }
        if ((lt_u8)reply.data[0] != 0) {
            rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL, "Read error, status: %u",
                      (unsigned)(lt_u8)reply.data[0]);
            goto done;
        }
        d = (const lt_u8 *)reply.data + 1;
        dn = reply.len - 1;
    } else {
        rc = raw_exchange(p, (const lt_u8 *)cmd.data, cmd.len, len,
                          lt_proto_effective_timeout_ms(p), inter_char_ms(p), LT_FALSE, &reply);
        if (rc) {
            goto done;
        }
        d = (const lt_u8 *)reply.data;
        dn = reply.len;
    }
    if (dn != len) {
        rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                  "wrong-length read on page %d offset %lu: got %lu of %lu bytes", page,
                  (unsigned long)offset, (unsigned long)dn, (unsigned long)len);
        goto done;
    }
    lt_buf_append(out, d, dn);
done:
    lt_buf_free(&cmd);
    lt_buf_free(&reply);
    return rc;
}

int lt_proto_read_page(lt_proto *p, int page, lt_ecu_mem *mem)
{
    size_t size = 0, off = 0, chunk, bf;
    lt_buf img;
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (p->ini && page >= 0 && (size_t)page < p->ini->proto.n_page_sizes) {
        size = (size_t)p->ini->proto.page_sizes[page];
    }
    if (size == 0 && mem) {
        size = lt_ecu_mem_page_size(mem, page);
    }
    if (size == 0) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL, "Unknown page size for page %d", page);
    }
    bf = (size_t)read_chunk(p);
    lt_buf_init(&img);
    if (mem) {
        lt_ecu_mem_set_state(mem, page, LT_PAGE_LOADING);
    }
    while (off < size) {
        chunk = size - off < bf ? size - off : bf;
        rc = progress(p, "read page", page, off, size);
        if (rc == LT_OK) {
            rc = lt_proto_read_memory(p, page, off, chunk, &img);
        }
        if (rc) {
            if (mem) {
                lt_ecu_mem_set_state(mem, page,
                                     rc == LT_ECANCEL ? LT_PAGE_NOT_LOADED : LT_PAGE_ERROR);
            }
            lt_buf_free(&img);
            return rc;
        }
        off += chunk;
    }
    if (mem) {
        lt_ecu_mem_load_page(mem, page, (const lt_u8 *)img.data, img.len);
    }
    lt_buf_free(&img);
    return progress(p, "read page", page, size, size);
}

int lt_proto_read_all(lt_proto *p, lt_ecu_mem *mem, lt_bool force)
{
    size_t i, n = lt_ecu_mem_page_count(mem);
    int rc;
    lt_page_state s;

    for (i = 0; i < n; i++) {
        s = lt_ecu_mem_state(mem, (int)i);
        if (!force && s != LT_PAGE_NOT_LOADED && s != LT_PAGE_ERROR) {
            continue;
        }
        if (lt_ecu_mem_page_size(mem, (int)i) == 0 &&
            !(p->ini && i < p->ini->proto.n_page_sizes)) {
            continue;
        }
        rc = lt_proto_read_page(p, (int)i, mem);
        if (rc) {
            return rc;
        }
    }
    return LT_OK;
}

/* ---- writes ---- */
static void set_bit(lt_u8 *set, int page, int on)
{
    if (page >= 0 && page < 256) {
        set[page] = (lt_u8)(on ? 1 : 0);
    }
}

static lt_bool any_set(const lt_u8 *set)
{
    int i;

    for (i = 0; i < 256; i++) {
        if (set[i]) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

size_t lt_proto_dirty_pages(const lt_proto *p, int *out, size_t cap)
{
    size_t c = 0;
    int i;

    for (i = 0; i < 256; i++) {
        if (p->dirty[i]) {
            if (out && c < cap) {
                out[c] = i;
            }
            c++;
        }
    }
    return c;
}

int lt_proto_write_memory(lt_proto *p, int page, size_t offset, const lt_u8 *data, size_t n)
{
    size_t chunk = (size_t)lt_proto_effective_write_chunk(p), done = 0, k;
    lt_buf cmd, reply;
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    while (done < n) {
        k = n - done < chunk ? n - done : chunk;
        if (p->cancel) {
            return cancelled(p);
        }
        if (p->cfg.auto_burn_on_page_change && p->last_written >= 0 &&
            p->last_written != page && !any_set(p->failed)) {
            rc = lt_proto_burn(p, p->last_written, NULL);
            if (rc) {
                return rc;
            }
        }
        lt_buf_init(&cmd);
        lt_buf_init(&reply);
        rc = build_page_cmd(p, LIST(p, page_chunk_write_commands), "C%2i%2o%2c%v", page,
                            (lt_u32)(offset + done), (lt_u32)k, data + done, k, "write", &cmd);
        if (rc == LT_OK) {
            if (p->modern) {
                rc = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
                if (rc == LT_OK) {
                    rc = status_error(p, (const lt_u8 *)reply.data, reply.len);
                }
            } else {
                rc = write_and_wait(p, cmd.data, cmd.len);
                if (rc == LT_OK) {
                    p->stats.tx_packets++;
                }
            }
        }
        if (rc == LT_OK) {
            p->last_written = page;
            set_bit(p->dirty, page, 1);
            sleep_ms(p, lt_proto_inter_frame_delay_ms(p, cmd.len));
        }
        lt_buf_free(&cmd);
        lt_buf_free(&reply);
        if (rc) {
            return rc;
        }
        done += k;
    }
    return LT_OK;
}

static int clamp_progress(lt_proto *p, const char *ph, int page, size_t d, size_t t)
{
    return progress(p, ph, page, d, t);
}

int lt_proto_verify_page_crc(lt_proto *p, int page, const lt_u8 *expected, size_t n)
{
    lt_buf cmd, reply;
    int rc;
    lt_u32 reported, local;
    const char *f;
    size_t fn;
    const lt_u8 *d;
    size_t dn;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    lt_buf_init(&cmd);
    lt_buf_init(&reply);
    f = NULL;
    fn = 0;
    if (p->ini && (size_t)page < p->ini->proto.crc32_check_commands.n) {
        f = p->ini->proto.crc32_check_commands.v[page].p;
        fn = p->ini->proto.crc32_check_commands.v[page].n;
    }
    if (fn == 0) {
        return fail(p, LT_EPROTO, LT_PE_VERIFY_UNAVAILABLE,
                    "no crc32CheckCommand declared for page %d", page);
    }
    lt_cmd_build(&cmd, f, fn, page_id(p, page), 0, (lt_u32)n, NULL, 0, cmd_le(p));
    clear_rx(p);
    rc = lt_proto_send_raw_response(p, (const lt_u8 *)cmd.data, cmd.len,
                                    lt_proto_effective_timeout_ms(p), &reply);
    if (rc) {
        goto done;
    }
    d = (const lt_u8 *)reply.data;
    dn = reply.len;
    if (dn < 4) {
        rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL, "page %d CRC reply too short (%lu bytes)", page,
                  (unsigned long)dn);
        goto done;
    }
    reported = lt_rd_be(d, 4);
    local = lt_proto_crc32(expected, n);
    if (reported != local) {
        rc = fail(p, LT_ECRC, LT_PE_PAGE_CRC_MISMATCH,
                  "page %d CRC mismatch: local 0x%08lx, ECU 0x%08lx", page,
                  (unsigned long)local, (unsigned long)reported);
        p->err.a = local;
        p->err.b = reported;
        p->err.page = page;
    }
done:
    lt_buf_free(&cmd);
    lt_buf_free(&reply);
    return rc;
}

int lt_proto_write_page(lt_proto *p, int page, const lt_u8 *data, size_t n)
{
    size_t chunk = (size_t)lt_proto_effective_write_chunk(p), off = 0, k;
    int rc = LT_OK, attempt;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    while (off < n) {
        k = n - off < chunk ? n - off : chunk;
        rc = clamp_progress(p, "write page", page, off, n);
        if (rc) {
            break;
        }
        for (attempt = 0; attempt < 3; attempt++) {
            rc = lt_proto_write_memory(p, page, off, data + off, k);
            if (rc == LT_OK) {
                break;
            }
            if (attempt == 2 || rc == LT_ECANCEL ||
                !(contains_ci(p->err.msg, "121") || contains_ci(p->err.msg, "timeout") ||
                  contains_ci(p->err.msg, "semaphore"))) {
                break;
            }
            clear_rx(p);
            sleep_ms(p, 50 * (attempt + 1));
        }
        if (rc) {
            break;
        }
        off += k;
    }
    if (rc == LT_OK) {
        rc = lt_proto_verify_page_crc(p, page, data, n);
        if (rc == LT_ECRC) {
            /* real mismatch: fail */
        } else if (rc == LT_ECANCEL) {
            /* propagate */
        } else if (rc != LT_OK) {
            /* CRC not available: only a warning */
            memset(&p->err, 0, sizeof(p->err));
            lt_err_clear();
            rc = LT_OK;
        }
    }
    set_bit(p->failed, page, rc != LT_OK);
    return rc;
}

int lt_proto_write_memory_verified(lt_proto *p, int page, size_t offset, const lt_u8 *data,
                                   size_t n)
{
    size_t chunk = (size_t)lt_proto_effective_write_chunk(p), at = 0, k, i;
    lt_buf got;
    int rc;

    rc = lt_proto_write_memory(p, page, offset, data, n);
    if (rc || p->modern || n == 0) {
        return rc;
    }
    while (at < n) {
        k = n - at < chunk ? n - at : chunk;
        lt_buf_init(&got);
        rc = lt_proto_read_memory(p, page, offset + at, k, &got);
        if (rc) {
            char why[200];

            lt_buf_free(&got);
            if (rc == LT_ECANCEL) {
                return rc;
            }
            /* fail() wipes p->err: keep the reason in a local copy first */
            lt_strlcpy(why, p->err.msg, sizeof(why));
            return fail(p, LT_EPROTO, LT_PE_VERIFY_UNAVAILABLE,
                        "write verification unavailable on page %d offset %lu: %s", page,
                        (unsigned long)(offset + at), why);
        }
        for (i = 0; i < k; i++) {
            if ((lt_u8)got.data[i] != data[at + i]) {
                size_t o = offset + at + i;

                rc = fail(p, LT_EPROTO, LT_PE_WRITE_VERIFY_FAILED,
                          "write verification failed on page %d offset %lu: wrote 0x%02x, read "
                          "0x%02x", page, (unsigned long)o, (unsigned)data[at + i],
                          (unsigned)(lt_u8)got.data[i]);
                p->err.page = page;
                p->err.offset = (int)(o > 65535UL ? 65535UL : o);
                p->err.expected = data[at + i];
                p->err.actual = (lt_u8)got.data[i];
                lt_buf_free(&got);
                return rc;
            }
        }
        lt_buf_free(&got);
        at += k;
    }
    return LT_OK;
}

int lt_proto_write_dirty(lt_proto *p, lt_ecu_mem *mem)
{
    int pages[256];
    size_t np, i, nr, r, total = 0, all, done = 0;
    lt_range *rg;
    const lt_u8 *img;
    size_t ilen;
    int rc = LT_OK, cap;

    np = lt_ecu_mem_dirty_pages(mem, pages, 256);
    if (np > 256) {
        np = 256;   /* proto tracks page ids 0..255 only */
    }
    all = lt_ecu_mem_dirty_bytes(mem);
    for (i = 0; i < np; i++) {
        img = lt_ecu_mem_page(mem, pages[i], &ilen);
        if (!img) {
            continue;
        }
        nr = lt_ecu_mem_dirty_ranges(mem, pages[i], NULL, 0);
        cap = (int)nr;
        rg = (lt_range *)lt_malloc((nr + 1) * sizeof(lt_range));
        lt_ecu_mem_dirty_ranges(mem, pages[i], rg, (size_t)cap);
        for (r = 0; r < nr && rc == LT_OK; r++) {
            rc = progress(p, "write dirty", pages[i], done, all);
            if (rc == LT_OK) {
                rc = lt_proto_write_memory(p, pages[i], rg[r].start, img + rg[r].start,
                                           rg[r].len);
            }
            if (rc == LT_OK) {
                total += rg[r].len;
                done += rg[r].len;
            }
        }
        lt_free(rg);
        if (rc) {
            set_bit(p->failed, pages[i], 1);
            return rc;
        }
        set_bit(p->failed, pages[i], 0);
    }
    lt_ecu_mem_mark_pending(mem);
    return (int)total;
}

/* ---- burn ---- */
/* After the burn settle time: look at what the ECU sent back. Burns are sent without waiting
 * for a reply (spec 9: legacy firmware never answers), so this is the only evidence. A read
 * error (cable pulled, port gone) or an explicit error status means the burn of `page` is NOT
 * confirmed and the page may be partially written; silence is accepted. */
static int burn_check(lt_proto *p, int page)
{
    lt_u8 tmp[256], got[64];
    size_t ng = 0;
    int r, guard;

    for (guard = 0; guard < 64; guard++) {
        r = p->io.read(p->io.ud, tmp, (int)sizeof(tmp), 0);
        if (r < 0) {
            return fail(p, LT_EIO, LT_PE_SERIAL, "connection lost while burning page %d: the "
                        "burn is not confirmed and the page may be only partly written", page);
        }
        if (r == 0) {
            break;
        }
        p->stats.rx_bytes += (unsigned long)r;
        if (ng < sizeof(got)) {
            size_t k = (size_t)r < sizeof(got) - ng ? (size_t)r : sizeof(got) - ng;
            memcpy(got + ng, tmp, k);
            ng += k;
        }
    }
    if (p->modern && ng >= 7) {
        const lt_u8 *pl;
        size_t pn, used;
        if (lt_env_decode(p->order, p->cfg.permissive_crc, got, ng, &pl, &pn, &used) == LT_OK &&
            pn >= 1 && (pl[0] & 0x80u) != 0) {
            p->err.a = pl[0];
            return fail(p, LT_EPROTO, LT_PE_ECU_STATUS, "the ECU reported an error burning page "
                        "%d: %s", page, lt_rc_message(pl[0]));
        }
    }
    return LT_OK;
}

/* Send the burn command of one page without waiting for anything. 0 sent (*settle_ms: how
 * long the ECU needs before burn_finish), 1 the page has no burn command, <0 refused/failed. */
static int burn_send(lt_proto *p, int page, int *settle_ms)
{
    lt_buf cmd, f;
    int rc, i, delay;
    char pages_txt[128];
    size_t used;
    const char *fm;
    size_t fn;

    if (any_set(p->failed)) {
        pages_txt[0] = '\0';
        used = 0;
        for (i = 0; i < 256; i++) {
            if (p->failed[i] && used < 100) {
                lt_snprintf(pages_txt + used, sizeof(pages_txt) - used, "%s%d",
                            used ? ", " : "", i);
                used = strlen(pages_txt);
            }
        }
        return fail(p, LT_EBUSY, LT_PE_PROTOCOL,
                    "refusing to burn: page(s) %s failed to write or verify; write the tune "
                    "again or reconnect first", pages_txt);
    }
    get_cmd(LIST(p, burn_commands), page, "B%2i", &fm, &fn);
    if (fn == 0) {
        return 1;   /* not burnable */
    }
    lt_buf_init(&cmd);
    lt_cmd_build(&cmd, fm, fn, page_id(p, page), 0, 0, NULL, 0, cmd_le(p));
    if (p->modern) {
        lt_buf_init(&f);
        lt_env_encode(p->order, (const lt_u8 *)cmd.data, cmd.len, &f);
        p->stats.tx_packets++;
        rc = raw_write(p, f.data, f.len);
        lt_buf_free(&f);
    } else {
        rc = raw_write(p, cmd.data, cmd.len);
    }
    if (rc == LT_OK && p->io.flush) {
        p->io.flush(p->io.ud);
    }
    lt_buf_free(&cmd);
    delay = p->ini ? ini_ms(p->ini->proto.page_activation_delay) : 2000;
    if (delay < 2000) {
        delay = 2000;
    }
    *settle_ms = delay + LT_BURN_SETTLE_MS;
    return rc;
}

/* After the settle time: check what the ECU said and update the page bookkeeping. */
static int burn_finish(lt_proto *p, int page, lt_ecu_mem *mem)
{
    int rc = burn_check(p, page);

    rx_clear_now(p);
    if (rc) {
        return rc;                      /* page stays in the dirty (unburned) set */
    }
    if (p->last_written == page) {
        p->last_written = -1;
    }
    set_bit(p->dirty, page, 0);
    if (mem && !any_set(p->dirty)) {
        lt_ecu_mem_mark_burned(mem);
    }
    return LT_OK;
}

int lt_proto_burn(lt_proto *p, int page, lt_ecu_mem *mem)
{
    int rc, list[256], i, np, settle_ms = 0;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    io_settle(p);          /* queued RAM writes go first: never burn half an update */
    if (page < 0) {
        np = (int)lt_proto_dirty_pages(p, list, 256);
        if (np == 0) {
            list[0] = 0;
            np = 1;
        }
        for (i = 0; i < np; i++) {
            rc = lt_proto_burn(p, list[i], NULL);
            if (rc) {
                return rc;
            }
        }
        if (mem) {
            lt_ecu_mem_mark_burned(mem);
        }
        return LT_OK;
    }
    rc = burn_send(p, page, &settle_ms);
    if (rc == 1) {
        return LT_OK;
    }
    if (rc) {
        return rc;
    }
    sleep_ms(p, settle_ms);
    return burn_finish(p, page, mem);
}

/* ---------------------------------------------------------------------- */
/* realtime                                                                */
/* ---------------------------------------------------------------------- */
static lt_ecu_type ecu_of(const lt_proto *p)
{
    return p->ini ? p->ini->ecu_type : LT_ECU_UNKNOWN;
}

static lt_bool is_slow_link(const lt_proto *p)
{
    const char *n = p->cfg.port_name;

    if (n && (contains_ci(n, "rfcomm") || contains_ci(n, "bluetooth") ||
              contains_ci(n, "tcp") || contains_ci(n, "telnet") || contains_ci(n, "wifi"))) {
        return LT_TRUE;
    }
    return baud_of(p) < 57600 ? LT_TRUE : LT_FALSE;
}

lt_rt_kind lt_proto_choose_runtime(const lt_proto *pc, const char **reason)
{
    lt_proto *p = (lt_proto *)pc;   /* reason text lives in the object */
    lt_bool och = (p->ini && p->ini->proto.och_get_command.n > 0) ? LT_TRUE : LT_FALSE;
    lt_ecu_type t = ecu_of(p);
    lt_bool burst_ecu = (t == LT_ECU_SPEEDUINO || t == LT_ECU_MS2 || t == LT_ECU_MS3 ||
                         t == LT_ECU_UNKNOWN) ? LT_TRUE : LT_FALSE;
    lt_rt_kind k = LT_RT_KIND_BURST;
    const char *r = "default: burst";

    switch (p->cfg.runtime_mode) {
    case LT_RT_FORCE_BURST:
        if (p->modern && och) {
            k = LT_RT_KIND_OCH;
            r = "force: ForceBurst (burst is a no-op in new-comms; using OCH)";
        } else {
            r = "force: ForceBurst";
        }
        break;
    case LT_RT_FORCE_OCH:
        if (och) {
            k = LT_RT_KIND_OCH;
            r = "force: ForceOCH";
        } else {
            r = "force: ForceOCH requested but no ochGetCommand; using burst";
        }
        break;
    case LT_RT_DISABLED:
        r = "disabled: Burst";
        break;
    default:
        if (p->modern && burst_ecu && och) {
            k = LT_RT_KIND_OCH;
            r = "auto: OCH (modern protocol negotiated)";
        } else if (burst_ecu) {
            lt_snprintf(p->reason, sizeof(p->reason), "auto: Burst (ecu=%s)",
                        lt_ecu_type_name(t));
            r = p->reason;
        } else if (och && p->ini->proto.max_unused_runtime_range > 0) {
            k = LT_RT_KIND_OCH;
            r = "maxUnusedRuntimeRange: OCH";
        } else if (och && is_slow_link(p)) {
            k = LT_RT_KIND_OCH;
            r = "slow link: OCH";
        } else if (och && adaptive_on(p) && adapt_avg_ms(p) > 50.0) {
            k = LT_RT_KIND_OCH;
            r = "adaptive: OCH (slow responses)";
        }
        break;
    }
    if (reason) {
        *reason = r;
    }
    return k;
}

/* The realtime command of spec 8 and the reply it expects (want = ochBlockSize or 0). */
static void rt_command(lt_proto *p, lt_buf *cmd, size_t *want, lt_bool *burst)
{
    lt_rt_kind k = lt_proto_choose_runtime(p, NULL);
    size_t w = p->ini ? (size_t)p->ini->proto.och_block_size : 0;

    if (k == LT_RT_KIND_OCH && w == 0) {
        k = LT_RT_KIND_BURST;
    }
    *want = w;
    *burst = (k == LT_RT_KIND_BURST) ? LT_TRUE : LT_FALSE;
    if (*burst) {
        if (p->ini && p->ini->proto.burst_get_command.n > 0) {
            lt_cmd_parse_string(p->ini->proto.burst_get_command.p,
                                p->ini->proto.burst_get_command.n, cmd);
        } else {
            lt_buf_putc(cmd, 'A');
        }
        if (cmd->len == 0) {
            lt_buf_putc(cmd, 'A');
        }
        if (!p->modern && cmd->len > 1) {
            cmd->len = 1;
        }
    } else {
        const lt_ini_blob *b = &p->ini->proto.och_get_command;

        if (memchr(b->p, '%', b->n)) {
            lt_cmd_build(cmd, b->p, b->n, 0, 0, (lt_u32)w, NULL, 0, cmd_le(p));
        } else {
            lt_buf_append(cmd, b->p, b->n);
        }
    }
}

int lt_proto_get_realtime(lt_proto *p, lt_buf *out)
{
    lt_buf cmd, reply;
    size_t want, dlen;
    int rc, off;
    lt_bool use_burst;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    lt_buf_init(&cmd);
    lt_buf_init(&reply);
    rt_command(p, &cmd, &want, &use_burst);
    if (p->modern) {
        rc = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
        if (rc == LT_OK) {
            off = lt_proto_strip_status((const lt_u8 *)reply.data, reply.len, want,
                                        use_burst ? "Burst" : "OCH", &dlen);
            if (off < 0) {
                rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL, "%s", lt_err_get());
            } else {
                lt_buf_append(out, reply.data + off, dlen);
            }
        }
    } else {
        rc = raw_exchange(p, (const lt_u8 *)cmd.data, cmd.len, want,
                          lt_proto_effective_timeout_ms(p), inter_char_ms(p), LT_FALSE, &reply);
        if (rc == LT_OK) {
            lt_buf_append(out, reply.data, reply.len);
        }
    }
    lt_buf_free(&cmd);
    lt_buf_free(&reply);
    return rc;
}

/* ---------------------------------------------------------------------- */
/* asynchronous requests                                                   */
/* ---------------------------------------------------------------------- */
/* Send one request without waiting and arm the reply collector. Framed requests get the
 * deadline the blocking send_packet would have (pacing wait + effective timeout); legacy ones
 * flush input first and end like raw_exchange (expected length, silence, timeout). */
static int ax_send(lt_proto *p, const lt_u8 *cmd, size_t n, int kind, size_t expected,
                   int timeout_ms, int quiet_ms, lt_bool allow_empty)
{
    ax_state *a = &p->ax;
    lt_buf f;
    int rc;

    lt_buf_clear(&a->rx);
    a->kind = kind;
    a->framed = p->modern;
    a->need = 2;
    a->pl_off = a->pl_len = 0;
    a->want = 0;
    a->burst = LT_FALSE;
    a->expected = expected;
    a->allow_empty = allow_empty;
    a->timeout_ms = timeout_ms > 0 && !p->modern ? timeout_ms : lt_proto_effective_timeout_ms(p);
    a->quiet_ms = quiet_ms > 0 ? quiet_ms : inter_char_ms(p);
    if (p->modern) {
        lt_buf_init(&f);
        lt_env_encode(p->order, cmd, n, &f);
        p->stats.tx_packets++;
        a->t0 = now_ms(p);
        a->pace_ms = pace_ms(p, f.len);
        rc = raw_write(p, f.data, f.len);
        lt_buf_free(&f);
    } else {
        rx_clear_now(p);
        a->t0 = now_ms(p);
        a->pace_ms = pace_ms(p, n);
        rc = raw_write(p, cmd, n);
        if (rc == LT_OK) {
            p->stats.tx_packets++;
        }
    }
    if (rc) {
        return rc;
    }
    if (p->io.flush) {
        p->io.flush(p->io.ud);
    }
    a->deadline = a->t0 + (double)a->pace_ms + (double)a->timeout_ms;
    a->last_rx = a->t0;
    a->phase = AX_WAIT;
    return LT_OK;
}

static void adapt_record_async(lt_proto *p)
{
    double el = now_ms(p) - p->ax.t0;

    /* the blocking path measures from before its pacing sleep: record the same quantity */
    adapt_record(p, el > (double)p->ax.pace_ms ? el : (double)p->ax.pace_ms);
}

static int ax_frame_done(lt_proto *p)
{
    ax_state *a = &p->ax;
    const lt_u8 *body = (const lt_u8 *)a->rx.data, *pl;
    size_t N = a->need - 6, pn;
    int rc;

    a->phase = AX_IDLE;
    adapt_record_async(p);
    p->stats.rx_packets++;
    rc = lt_env_decode(p->order, p->cfg.permissive_crc, body, a->need, &pl, &pn, NULL);
    if (rc == LT_OK) {
        a->pl_off = (size_t)(pl - body);
        a->pl_len = pn;
        return 1;
    }
    if (rc == LT_ECRC) {
        char m[200];

        lt_strlcpy(m, lt_err_get(), sizeof(m));
        fail(p, LT_ECRC, LT_PE_CRC_MISMATCH, "%s", m);
        p->err.a = lt_proto_crc32(body + 2, N);
        p->err.b = rd_order(p->order, body + 2 + N, 4);
        return rc;
    }
    return fail(p, rc, LT_PE_INVALID_RESPONSE, "%s", lt_err_get());
}

static int ax_raw_done(lt_proto *p)
{
    ax_state *a = &p->ax;

    a->phase = AX_IDLE;
    if (a->rx.len > 0) {
        p->stats.rx_packets++;
        adapt_record_async(p);
    }
    a->pl_off = 0;
    a->pl_len = a->rx.len;
    return 1;
}

/* Discard the tail of a failed framed reply (the blocking drain_n, spread over pumps). */
static void ax_drain(lt_proto *p, int wait_ms)
{
    ax_state *a = &p->ax;
    lt_u8 tmp[256];
    int r, first = 1;
    double t;

    while (a->drain_left > 0) {
        r = p->io.read(p->io.ud, tmp,
                       (int)(a->drain_left > sizeof(tmp) ? sizeof(tmp) : a->drain_left),
                       first ? wait_ms : 0);
        first = 0;
        if (r < 0) {
            a->drain_left = 0;
            break;
        }
        if (r == 0) {
            break;
        }
        a->drain_left -= (size_t)r;
        a->drain_last = now_ms(p);
    }
    t = now_ms(p);
    if (a->drain_left == 0 || t - a->drain_last >= 10.0 || t >= a->drain_until) {
        rx_clear_now(p);
        a->phase = AX_IDLE;
    }
}

/* Collect what has arrived (waiting at most wait_ms for the first byte). 0 still waiting,
 * 1 complete (payload at rx+pl_off), <0 failed (error set; the exchange is over or draining). */
static int ax_step(lt_proto *p, int wait_ms)
{
    ax_state *a = &p->ax;
    lt_u8 tmp[512];
    size_t want;
    int r, first = 1;
    double t;

    if (a->phase == AX_DRAIN) {
        ax_drain(p, wait_ms);
        return 0;
    }
    if (a->phase != AX_WAIT) {
        return 0;
    }
    for (;;) {
        want = sizeof(tmp);
        if (a->framed && a->need - a->rx.len < want) {
            want = a->need - a->rx.len;      /* never read past the frame */
        }
        r = p->io.read(p->io.ud, tmp, (int)want, first ? wait_ms : 0);
        first = 0;
        if (r < 0) {
            a->phase = AX_IDLE;
            adapt_reset(p);
            return fail(p, LT_EIO, LT_PE_SERIAL, "read failed (connection closed)");
        }
        if (r == 0) {
            break;
        }
        lt_buf_append(&a->rx, tmp, (size_t)r);
        p->stats.rx_bytes += (unsigned long)r;
        a->last_rx = now_ms(p);
        if (a->framed) {
            if (a->need == 2 && a->rx.len >= 2) {
                a->need = 2 + (size_t)rd_order(p->order, (const lt_u8 *)a->rx.data, 2) + 4;
            }
            if (a->rx.len >= a->need) {
                return ax_frame_done(p);
            }
        } else if (a->expected > 0 && a->rx.len >= a->expected) {
            return ax_raw_done(p);
        }
    }
    t = now_ms(p);
    if (a->framed) {
        if (t >= a->deadline) {
            adapt_reset(p);
            if (a->rx.len < 2) {
                rx_clear_now(p);
                a->phase = AX_IDLE;
            } else {
                a->phase = AX_DRAIN;
                a->drain_left = a->need - a->rx.len;
                a->drain_until = t + 500.0;
                a->drain_last = t;
            }
            return fail(p, LT_ETIMEOUT, LT_PE_TIMEOUT, "response timeout");
        }
        return 0;
    }
    if (a->rx.len > 0 && t - a->last_rx > (double)a->quiet_ms) {
        return ax_raw_done(p);
    }
    if (t - a->t0 > (double)a->timeout_ms) {
        if (a->rx.len == 0 && !a->allow_empty) {
            a->phase = AX_IDLE;
            adapt_reset(p);
            return fail(p, LT_ETIMEOUT, LT_PE_TIMEOUT, "response timeout");
        }
        return ax_raw_done(p);
    }
    return 0;
}

/* A realtime or raw reply is complete (rc 1) or failed: keep the result for its pump. Status
 * byte handling as in lt_proto_get_realtime / lt_proto_send_raw_response. */
static void rt_complete(lt_proto *p, rt_result *res, int rc)
{
    ax_state *a = &p->ax;
    size_t dlen;
    int off;

    lt_buf_clear(&res->data);
    if (rc == 1 && a->framed) {
        off = lt_proto_strip_status((const lt_u8 *)a->rx.data + a->pl_off, a->pl_len, a->want,
                                    a->kind == AXK_RAW ? "raw command" :
                                    (a->burst ? "Burst" : "OCH"), &dlen);
        if (off < 0) {
            rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL, "%s", lt_err_get());
        } else {
            lt_buf_append(&res->data, a->rx.data + a->pl_off + (size_t)off, dlen);
        }
    } else if (rc == 1) {
        lt_buf_append(&res->data, a->rx.data, a->rx.len);
    }
    res->ready = 1;
    res->rc = rc == 1 ? LT_OK : rc;
    res->err = p->err;
}

/* Fairness between request kinds: one refused because the wire was busy goes next. */
static lt_bool yield_to_other(lt_proto *p, int kind)
{
    if (p->want_kind == 0 || p->want_kind == kind) {
        return LT_FALSE;
    }
    if (now_ms(p) - p->want_at >= 100.0) {
        p->want_kind = 0;
        return LT_FALSE;
    }
    return LT_TRUE;
}

static void want_wire(lt_proto *p, int kind)
{
    if (!yield_to_other(p, kind) && p->want_kind != kind) {
        p->want_kind = kind;
        p->want_at = now_ms(p);
    }
}

static void got_wire(lt_proto *p, int kind)
{
    if (p->want_kind == kind) {
        p->want_kind = 0;
    }
}

/* ---- the asynchronous RAM writer ---- */
static void job_reset(lt_proto *p)
{
    p->job.n = p->job.next = 0;
    p->job.inflight = 0;
    p->job.burn_page = -1;
    p->job.mem = NULL;
}

/* Give every range that is not safely in ECU RAM back to the dirty set and report rc. */
static void job_fail(lt_proto *p, int rc, int page)
{
    wr_job *j = &p->job;
    size_t i, k;
    lt_bool open_page;

    for (i = 0; i < j->n; i++) {
        open_page = j->ch[i].done ? LT_FALSE : LT_TRUE;
        for (k = 0; k < j->n && !open_page; k++) {
            if (j->ch[k].page == j->ch[i].page && !j->ch[k].done) {
                open_page = LT_TRUE;
            }
        }
        if (open_page && j->mem) {
            lt_ecu_mem_mark_dirty(j->mem, j->ch[i].page, j->ch[i].off, j->ch[i].len);
        }
    }
    if (page >= 0) {
        set_bit(p->failed, page, 1);
    }
    j->result = rc;
    j->err = p->err;
    job_reset(p);
}

/* Chunk `idx` reached ECU RAM: page bookkeeping as write_memory / write_dirty do it. */
static void job_chunk_done(lt_proto *p, size_t idx)
{
    wr_job *j = &p->job;
    int page = j->ch[idx].page;
    size_t k;

    j->ch[idx].done = 1;
    p->last_written = page;
    set_bit(p->dirty, page, 1);
    for (k = 0; k < j->n; k++) {
        if (j->ch[k].page == page && !j->ch[k].done) {
            return;
        }
    }
    if (j->mem) {
        lt_ecu_mem_mark_page_pending(j->mem, page);
    }
    set_bit(p->failed, page, 0);
}

static void job_check_done(lt_proto *p)
{
    if (p->job.n > 0 && p->job.next >= p->job.n && !p->job.inflight && p->job.burn_page < 0) {
        if (p->job.result == 0) {
            p->job.result = 1;          /* an uncollected failure is not overwritten */
        }
        job_reset(p);
    }
}

/* Is the pacing gap over? (An in-process device needs none.) */
static lt_bool gate_open(lt_proto *p)
{
    if (p->next_tx_at <= 0.0) {
        return LT_TRUE;
    }
    if (p->cfg.instant_link || now_ms(p) >= p->next_tx_at) {
        p->next_tx_at = 0.0;
        return LT_TRUE;
    }
    return LT_FALSE;
}

/* Send the next queued chunk (or the auto-burn it needs first). */
static void job_send(lt_proto *p)
{
    wr_job *j = &p->job;
    wr_chunk *c = &j->ch[j->next];
    const lt_u8 *data;
    lt_buf cmd;
    int rc, settle_ms = 0, page = c->page;

    if (p->cfg.auto_burn_on_page_change && p->last_written >= 0 &&
        p->last_written != page && !any_set(p->failed)) {
        rc = burn_send(p, p->last_written, &settle_ms);
        if (rc == 1) {
            p->last_written = -1;          /* nothing to burn */
            return;
        }
        if (rc) {
            job_fail(p, rc, -1);
            return;
        }
        j->burn_page = p->last_written;
        p->next_tx_at = now_ms(p) + (double)settle_ms;
        return;
    }
    data = j->mem ? lt_ecu_mem_read(j->mem, page, c->off, c->len) : NULL;
    if (!data) {
        job_fail(p, fail(p, LT_ERANGE, LT_PE_PROTOCOL, "page %d changed while it was being "
                         "written", page), page);
        return;
    }
    lt_buf_init(&cmd);
    rc = build_page_cmd(p, LIST(p, page_chunk_write_commands), "C%2i%2o%2c%v", page,
                        (lt_u32)c->off, (lt_u32)c->len, data, c->len, "write", &cmd);
    if (rc == LT_OK) {
        j->cmd_len = cmd.len;
        if (p->modern) {
            rc = ax_send(p, (const lt_u8 *)cmd.data, cmd.len, AXK_WRITE, 0, 0, 0, LT_FALSE);
            if (rc == LT_OK) {
                j->next++;
                j->inflight = 1;
            }
        } else {
            /* legacy writes are silent: pace like write_and_wait + inter-frame delay */
            rc = raw_write(p, cmd.data, cmd.len);
            if (rc == LT_OK) {
                if (p->io.flush) {
                    p->io.flush(p->io.ud);
                }
                p->stats.tx_packets++;
                p->next_tx_at = now_ms(p) + (double)pace_ms(p, cmd.len) +
                                (double)lt_proto_inter_frame_delay_ms(p, cmd.len);
                j->next++;
                job_chunk_done(p, j->next - 1);
                job_check_done(p);
            }
        }
    }
    lt_buf_free(&cmd);
    if (rc) {
        p->ax.phase = AX_IDLE;
        job_fail(p, rc, page);
    }
}

/* A framed write acknowledgement is complete (rc 1) or failed. */
static void write_complete(lt_proto *p, int rc)
{
    wr_job *j = &p->job;
    ax_state *a = &p->ax;
    int page = j->ch[j->next - 1].page;

    j->inflight = 0;
    if (rc == 1) {
        rc = status_error(p, (const lt_u8 *)a->rx.data + a->pl_off, a->pl_len);
    }
    if (rc < 0) {
        job_fail(p, rc, page);
        return;
    }
    p->next_tx_at = now_ms(p) + (double)lt_proto_inter_frame_delay_ms(p, j->cmd_len);
    job_chunk_done(p, j->next - 1);
    job_check_done(p);
}

static void ax_complete(lt_proto *p, int rc)
{
    if (p->ax.kind == AXK_RT) {
        rt_complete(p, &p->rtr, rc);
    } else if (p->ax.kind == AXK_RAW) {
        rt_complete(p, &p->rawr, rc);
    } else if (p->ax.kind == AXK_WRITE && p->job.n > 0 && p->job.inflight) {
        write_complete(p, rc);
    }
}

/* Drive everything that is in flight as far as it goes without waiting (wait_ms > 0: the
 * first read may wait that long; used by settle). */
static void advance(lt_proto *p, int wait_ms)
{
    int rc, rounds, was;

    for (rounds = 0; rounds < 64; rounds++) {
        if (p->ax.phase != AX_IDLE) {
            was = p->ax.phase;
            rc = ax_step(p, wait_ms);
            wait_ms = 0;
            if (was == AX_WAIT && rc != 0) {
                ax_complete(p, rc);
            }
            if (p->ax.phase != AX_IDLE) {
                if (p->job.n > p->job.next && p->ax.kind != AXK_WRITE) {
                    want_wire(p, AXK_WRITE);
                }
                return;                  /* still on the wire */
            }
        }
        if (p->job.burn_page >= 0) {
            if (!gate_open(p)) {
                return;
            }
            rc = burn_finish(p, p->job.burn_page, NULL);
            p->job.burn_page = -1;
            if (rc) {
                job_fail(p, rc, -1);
                return;
            }
            continue;
        }
        if (p->job.n == 0 || p->job.next >= p->job.n || yield_to_other(p, AXK_WRITE) ||
            !gate_open(p)) {
            job_check_done(p);
            return;
        }
        got_wire(p, AXK_WRITE);
        job_send(p);
    }
}

/* The cancel flag stops what is in flight; the blocking caller reports it. */
static void settle_cancel(lt_proto *p)
{
    ax_state *a = &p->ax;

    if (a->phase == AX_WAIT) {
        a->phase = AX_IDLE;
        adapt_reset(p);
        if (a->framed && a->rx.len >= 2 && a->need > a->rx.len) {
            a->phase = AX_DRAIN;                 /* leave the wire clean */
            a->drain_left = a->need - a->rx.len;
            a->drain_until = now_ms(p) + 500.0;
            a->drain_last = now_ms(p);
        } else {
            rx_clear_now(p);
        }
        fail(p, LT_ECANCEL, LT_PE_CANCELLED, "operation canceled");
        ax_complete(p, LT_ECANCEL);
    }
    if (p->job.n > 0 && a->phase == AX_IDLE) {
        job_fail(p, fail(p, LT_ECANCEL, LT_PE_CANCELLED, "operation canceled"), -1);
    }
}

/* Blocking completion of everything asynchronous (the guard of every blocking path). */
static void settle(lt_proto *p)
{
    double t, rem;
    int wait, guard;

    for (guard = 0; guard < 1000000 && async_busy(p); guard++) {
        p->want_kind = 0;                       /* the blocking caller is next, nobody else */
        if (p->cancel) {
            settle_cancel(p);
        }
        t = now_ms(p);
        if (p->ax.phase == AX_WAIT) {
            rem = (p->ax.framed ? p->ax.deadline : p->ax.t0 + (double)p->ax.timeout_ms) - t;
            wait = rem > 20.0 ? 20 : (rem < 1.0 ? 1 : (int)rem + 1);
            if (!p->ax.framed && p->ax.rx.len > 0 && wait > 2) {
                wait = 2;                       /* silence ends a legacy reply: look often */
            }
        } else if (p->ax.phase == AX_DRAIN) {
            wait = 10;
        } else {
            wait = 0;
            if (!gate_open(p)) {
                sleep_ms(p, (int)(p->next_tx_at - t) + 1);
                p->next_tx_at = 0.0;           /* waited as the blocking path sleeps */
            }
        }
        advance(p, wait);
        if (async_busy(p) && wait > 0 && now_ms(p) - t < 1.0) {
            sleep_ms(p, 1);                     /* io without a real wait (tests, sims) */
        }
    }
}

static void async_abort(lt_proto *p)
{
    if (p->job.n > 0) {
        job_fail(p, LT_ENOTCONN, -1);
    }
    p->job.result = 0;
    job_reset(p);
    p->ax.phase = AX_IDLE;
    p->rtr.ready = 0;
    p->rawr.ready = 0;
    p->next_tx_at = 0.0;
    p->want_kind = 0;
}

int lt_proto_rt_begin(lt_proto *p)
{
    lt_buf cmd;
    size_t want;
    lt_bool burst;
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    advance(p, 0);
    if (p->rtr.ready) {
        return LT_EBUSY;                         /* collect the previous frame first */
    }
    if (p->ax.phase != AX_IDLE || p->job.burn_page >= 0 || !gate_open(p) ||
        yield_to_other(p, AXK_RT)) {
        if (p->ax.phase != AX_IDLE || (p->job.n > 0 && p->job.burn_page < 0)) {
            want_wire(p, AXK_RT);                /* goes next when the wire frees */
        }
        return LT_EBUSY;
    }
    got_wire(p, AXK_RT);
    lt_buf_init(&cmd);
    rt_command(p, &cmd, &want, &burst);
    rc = ax_send(p, (const lt_u8 *)cmd.data, cmd.len, AXK_RT, p->modern ? 0 : want, 0, 0,
                 LT_FALSE);
    lt_buf_free(&cmd);
    if (rc) {
        p->ax.phase = AX_IDLE;
        return rc;
    }
    p->ax.want = want;
    p->ax.burst = burst;
    return LT_OK;
}

int lt_proto_rt_pump(lt_proto *p, lt_buf *out)
{
    if (p->state != LT_CONN_CONNECTED) {
        if (!lt_proto_rt_pending(p)) {
            return 0;
        }
        async_abort(p);
        return need_conn(p);
    }
    advance(p, 0);
    if (!p->rtr.ready) {
        return 0;
    }
    p->rtr.ready = 0;
    if (p->rtr.rc < 0) {
        p->err = p->rtr.err;
        lt_err_set("%s", p->err.msg);
        return p->rtr.rc;
    }
    lt_buf_append(out, p->rtr.data.data, p->rtr.data.len);
    return 1;
}

lt_bool lt_proto_rt_pending(const lt_proto *p)
{
    return (p->rtr.ready || (p->ax.phase == AX_WAIT && p->ax.kind == AXK_RT)) ? LT_TRUE
                                                                             : LT_FALSE;
}

static int raw_ticket(const lt_proto *p)
{
    return (int)(p->rawr.seq % 32767UL) + 1;
}

int lt_proto_raw_begin(lt_proto *p, const lt_u8 *bytes, size_t n, int timeout_ms)
{
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    advance(p, 0);
    if (p->ax.phase != AX_IDLE || p->job.burn_page >= 0 || !gate_open(p) ||
        yield_to_other(p, AXK_RAW)) {
        if (p->ax.phase != AX_IDLE || (p->job.n > 0 && p->job.burn_page < 0)) {
            want_wire(p, AXK_RAW);
        }
        return LT_EBUSY;
    }
    got_wire(p, AXK_RAW);
    p->rawr.ready = 0;                           /* an uncollected older reply is dropped */
    p->rawr.seq++;
    rc = ax_send(p, bytes, n, AXK_RAW, 0, timeout_ms, 50, LT_TRUE);
    if (rc) {
        p->ax.phase = AX_IDLE;
        return rc;
    }
    return raw_ticket(p);
}

int lt_proto_raw_pump(lt_proto *p, int ticket, lt_buf *out)
{
    lt_bool inflight = (p->ax.phase == AX_WAIT && p->ax.kind == AXK_RAW) ? LT_TRUE : LT_FALSE;

    if (ticket != raw_ticket(p) || (!inflight && !p->rawr.ready)) {
        return fail(p, LT_ENOENT, LT_PE_PROTOCOL, "the command's reply is gone");
    }
    if (p->state != LT_CONN_CONNECTED) {
        async_abort(p);
        return need_conn(p);
    }
    advance(p, 0);
    if (!p->rawr.ready) {
        return 0;
    }
    p->rawr.ready = 0;
    if (p->rawr.rc < 0) {
        p->err = p->rawr.err;
        lt_err_set("%s", p->err.msg);
        return p->rawr.rc;
    }
    lt_buf_append(out, p->rawr.data.data, p->rawr.data.len);
    return 1;
}

/* Queue [off, off+len) of page, merged into a queued, unsent chunk where it fits. */
static void job_add(lt_proto *p, int page, size_t off, size_t len, size_t chunk)
{
    wr_job *j = &p->job;
    size_t i, lo, hi, k, nb;

    while (len > 0) {
        k = len < chunk ? len : chunk;
        for (i = j->next; i < j->n; i++) {
            wr_chunk *c = &j->ch[i];

            if (c->page != page || off > c->off + c->len || c->off > off + k) {
                continue;
            }
            lo = c->off < off ? c->off : off;
            hi = c->off + c->len > off + k ? c->off + c->len : off + k;
            if (hi - lo <= chunk) {
                c->off = lo;
                c->len = hi - lo;
                break;
            }
        }
        if (i == j->n) {
            if (j->n == j->cap) {
                if (!lt_mul_size(j->cap ? j->cap * 2 : 16, sizeof(wr_chunk), &nb)) {
                    return;
                }
                j->cap = j->cap ? j->cap * 2 : 16;
                j->ch = (wr_chunk *)lt_realloc(j->ch, nb);
            }
            j->ch[j->n].page = page;
            j->ch[j->n].off = off;
            j->ch[j->n].len = k;
            j->ch[j->n].done = 0;
            j->n++;
        }
        off += k;
        len -= k;
    }
}

int lt_proto_write_dirty_begin(lt_proto *p, lt_ecu_mem *mem)
{
    int pages[256];
    size_t np, i, nr, r, total = 0, chunk;
    lt_range *rg;
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (p->job.n > 0 && p->job.mem != mem) {
        settle(p);                               /* another shadow: finish that one first */
    }
    chunk = (size_t)lt_proto_effective_write_chunk(p);
    np = lt_ecu_mem_dirty_pages(mem, pages, 256);
    if (np > 256) {
        np = 256;   /* proto tracks page ids 0..255 only */
    }
    for (i = 0; i < np; i++) {
        if (!lt_ecu_mem_page(mem, pages[i], NULL)) {
            continue;
        }
        nr = lt_ecu_mem_dirty_ranges(mem, pages[i], NULL, 0);
        rg = (lt_range *)lt_malloc((nr + 1) * sizeof(lt_range));
        nr = lt_ecu_mem_dirty_ranges(mem, pages[i], rg, nr);
        for (r = 0; r < nr; r++) {
            job_add(p, pages[i], rg[r].start, rg[r].len, chunk);
            lt_ecu_mem_clear_dirty(mem, pages[i], rg[r].start, rg[r].len);
            total += rg[r].len;
        }
        lt_free(rg);
    }
    if (p->job.n > 0) {
        p->job.mem = mem;
    }
    advance(p, 0);
    return total > 0x7fffffffUL ? 0x7fffffff : (int)total;
}

int lt_proto_write_pump(lt_proto *p)
{
    int r;

    if (p->state == LT_CONN_CONNECTED) {
        advance(p, 0);
    } else if (p->job.n > 0) {
        async_abort(p);
    }
    r = p->job.result;
    if (r == 0) {
        return 0;
    }
    p->job.result = 0;
    if (r < 0) {
        p->err = p->job.err;
        lt_err_set("%s", p->err.msg);
    }
    return r;
}

lt_bool lt_proto_write_pending(const lt_proto *p)
{
    return (p->job.n > 0 || p->job.burn_page >= 0 || p->job.result != 0) ? LT_TRUE : LT_FALSE;
}

int lt_proto_async_wait_ms(const lt_proto *p)
{
    const ax_state *a = &p->ax;
    double t = now_ms(p), est, bms = 10000.0 / (double)baud_of(p), end;
    size_t left;

    if (p->rtr.ready || p->job.result != 0) {
        return 0;
    }
    if (a->phase == AX_WAIT) {
        if (a->framed) {
            left = a->need > 2 ? a->need - a->rx.len : 2 + a->want + 1 + 4 - a->rx.len;
            end = a->deadline;
        } else {
            left = a->expected > a->rx.len ? a->expected - a->rx.len : 1;
            end = a->rx.len > 0 ? a->last_rx + (double)a->quiet_ms + 1.0
                                : a->t0 + (double)a->timeout_ms + 1.0;
        }
        est = (double)left * bms;
        if (est > end - t) {
            est = end - t;
        }
        return est < 1.0 ? 1 : (est > 10.0 ? 10 : (int)est);
    }
    if (a->phase == AX_DRAIN) {
        return 5;
    }
    if (p->job.n > 0 || p->job.burn_page >= 0) {
        return p->next_tx_at > t ? (int)(p->next_tx_at - t) + 1 : 0;
    }
    return p->next_tx_at > t ? (int)(p->next_tx_at - t) + 1 : -1;   /* only a pacing gap */
}

void lt_proto_async_finish(lt_proto *p)
{
    io_settle(p);
}

/* ---------------------------------------------------------------------- */
/* misc commands                                                           */
/* ---------------------------------------------------------------------- */
int lt_proto_send_raw(lt_proto *p, const lt_u8 *bytes, size_t n)
{
    int rc = need_conn(p);

    if (rc) {
        return rc;
    }
    if (n == 0) {
        return LT_OK;
    }
    if (p->modern) {
        return send_packet_no_response(p, bytes, n);
    }
    return write_and_wait(p, bytes, n);
}

int lt_proto_send_raw_response(lt_proto *p, const lt_u8 *bytes, size_t n, int timeout_ms,
                               lt_buf *reply)
{
    lt_buf r;
    int rc, off;
    size_t dlen;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (p->modern) {
        lt_buf_init(&r);
        rc = send_packet(p, bytes, n, &r);
        if (rc == LT_OK) {
            off = lt_proto_strip_status((const lt_u8 *)r.data, r.len, 0, "raw command", &dlen);
            if (off >= 0 && dlen > 0) {
                lt_buf_append(reply, r.data + off, dlen);
            }
        }
        lt_buf_free(&r);
        return rc;
    }
    return raw_exchange(p, bytes, n, 0, timeout_ms, 50, LT_TRUE, reply);
}

static int run_cmd_depth(lt_proto *p, const lt_ini_cmd *cmd, int depth)
{
    size_t i;
    int rc;
    lt_buf b;
    const lt_ini_cmd *ref;

    if (depth > 8) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL, "controller command nesting too deep");
    }
    for (i = 0; i < cmd->n_parts; i++) {
        const lt_ini_cmd_part *part = &cmd->parts[i];

        if (part->is_ref) {
            ref = p->ini ? (const lt_ini_cmd *)lt_map_get(&p->ini->command_idx, part->data.p)
                         : NULL;
            if (!ref) {
                return fail(p, LT_ENOENT, LT_PE_PROTOCOL, "unknown controller command '%s'",
                            part->data.p);
            }
            rc = run_cmd_depth(p, ref, depth + 1);
        } else {
            lt_buf_init(&b);
            lt_cmd_parse_string(part->data.p, part->data.n, &b);
            rc = lt_proto_send_raw(p, (const lt_u8 *)b.data, b.len);
            lt_buf_free(&b);
        }
        if (rc) {
            return rc;
        }
    }
    return LT_OK;
}

int lt_proto_run_command(lt_proto *p, const lt_ini_cmd *cmd)
{
    return run_cmd_depth(p, cmd, 0);
}

/* ---- console (spec 10) ---- */
static void append_lossy(lt_buf *b, const lt_u8 *d, size_t n)
{
    lt_buf_append(b, d, n);
}

int lt_proto_console(lt_proto *p, const char *text, int timeout_ms, char **out)
{
    int rc, i, empty_polls = 0, polls;
    lt_buf coll, reply, cmd;
    double t0, t, last;
    lt_u8 tmp[512];
    int r;
    char *res;

    *out = NULL;
    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (!lt_ecu_supports_console(ecu_of(p))) {
        return fail(p, LT_EUNSUPPORTED, LT_PE_PROTOCOL,
                    "console commands are only supported on rusEFI/FOME/epicEFI");
    }
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }
    lt_buf_init(&coll);
    lt_buf_init(&reply);
    lt_buf_init(&cmd);
    if (p->modern) {
        static const lt_u8 g[1] = { 'G' };
        size_t dl;

        for (i = 0; i < 3; i++) {
            lt_buf_clear(&reply);
            if (send_packet(p, g, 1, &reply) != LT_OK) {
                break;
            }
            dl = (reply.len > 1 && reply.data[0] == 0) ? reply.len - 1 : 0;
            if (dl == 0) {
                break;
            }
            sleep_ms(p, 30);
        }
        lt_buf_putc(&cmd, 'E');
        lt_buf_puts(&cmd, text);
        lt_buf_clear(&reply);
        rc = send_packet(p, (const lt_u8 *)cmd.data, cmd.len, &reply);
        if (rc) {
            goto done;
        }
        if (reply.len > 0 && reply.data[0] != 0) {
            if ((lt_u8)reply.data[0] == 0x83) {
                rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                          "ECU does not support console commands (unrecognized 'E' command)");
                goto done;
            }
            if (lt_rc_is_error((lt_u8)reply.data[0])) {
                rc = status_error(p, (const lt_u8 *)reply.data, reply.len);
                goto done;
            }
        }
        sleep_ms(p, 50);
        t0 = now_ms(p);
        for (polls = 0; polls < 10 && now_ms(p) - t0 <= (double)timeout_ms; polls++) {
            lt_buf_clear(&reply);
            rc = send_packet(p, g, 1, &reply);
            if (rc) {
                if (coll.len > 0) {
                    rc = LT_OK;
                    break;
                }
                goto done;
            }
            if (reply.len > 1 && reply.data[0] == 0) {
                append_lossy(&coll, (const lt_u8 *)reply.data + 1, reply.len - 1);
                empty_polls = 0;
                sleep_ms(p, 20);
            } else if (reply.len == 0 || reply.data[0] == 0) {
                empty_polls++;
                if (coll.len > 0 || empty_polls >= 2) {
                    break;
                }
                sleep_ms(p, 50);
            } else {
                append_lossy(&coll, (const lt_u8 *)reply.data, reply.len);
                empty_polls = 0;
                sleep_ms(p, 20);
            }
        }
        res = lt_parse_rusefi_text_output(coll.len > 0 ? coll.data : "");
        if (res[0] == '\0') {
            lt_free(res);
            res = lt_strdup("(command accepted, no output)");
        }
        *out = res;
        rc = LT_OK;
    } else {
        clear_rx(p);
        lt_buf_puts(&cmd, text);
        lt_buf_putc(&cmd, '\n');
        rc = write_and_wait(p, cmd.data, cmd.len);
        if (rc) {
            goto done;
        }
        t0 = now_ms(p);
        last = t0;
        for (;;) {
            t = now_ms(p);
            if (t - t0 > (double)timeout_ms) {
                break;
            }
            if (p->cancel) {
                rc = cancelled(p);
                goto done;
            }
            r = p->io.read(p->io.ud, tmp, (int)sizeof(tmp), 1);
            if (r < 0) {
                rc = fail(p, LT_EIO, LT_PE_SERIAL, "read failed (connection closed)");
                goto done;
            }
            if (r > 0) {
                lt_buf_append(&coll, tmp, (size_t)r);
                last = now_ms(p);
                continue;
            }
            if (now_ms(p) - t < 1.0) {
                sleep_ms(p, 1);
            }
            if (coll.len > 0 && now_ms(p) - last > (double)inter_char_ms(p)) {
                break;
            }
        }
        if (coll.len == 0) {
            rc = fail(p, LT_ETIMEOUT, LT_PE_TIMEOUT, "console response timeout");
            goto done;
        }
        {
            char *sbuf = (char *)lt_malloc(coll.len + 1);

            trim_copy((const lt_u8 *)coll.data, coll.len, sbuf, coll.len + 1);
            *out = sbuf;
        }
        rc = LT_OK;
    }
done:
    lt_buf_free(&coll);
    lt_buf_free(&reply);
    lt_buf_free(&cmd);
    return rc;
}

/* ---- Speeduino calibration (spec 11) ---- */
int lt_proto_read_calibration_crc(lt_proto *p, int table, lt_u32 *crc)
{
    lt_u8 cmd[3];
    lt_buf reply;
    int rc;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (!p->modern) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                    "calibration CRC ('k') requires the CRC protocol (msEnvelope)");
    }
    cmd[0] = 'k';
    cmd[1] = 0;
    cmd[2] = (lt_u8)table;
    lt_buf_init(&reply);
    rc = send_packet(p, cmd, 3, &reply);
    if (rc == LT_OK) {
        if (reply.len < 5 || reply.data[0] != 0) {
            rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                      "calibration CRC read failed, response: %lu bytes, status 0x%02x",
                      (unsigned long)reply.len,
                      reply.len ? (unsigned)(lt_u8)reply.data[0] : 0xFFu);
        } else {
            *crc = lt_rd_be((const lt_u8 *)reply.data + 1, 4);
        }
    }
    lt_buf_free(&reply);
    return rc;
}

int lt_proto_write_calibration(lt_proto *p, int table, const lt_u8 *wire, size_t n)
{
    lt_ecu_type t = ecu_of(p);
    size_t want = table == LT_CAL_O2 ? 1024 : 64, chunk, off, k;
    lt_buf pl, reply;
    int rc;
    lt_u32 crc;
    static const char *const names[] = { "CLT", "IAT", "O2" };

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (table < 0 || table > 2) {
        return fail(p, LT_EINVAL, LT_PE_PROTOCOL, "bad calibration table %d", table);
    }
    if (t != LT_ECU_SPEEDUINO && t != LT_ECU_UNKNOWN) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                    "sensor calibration write is only implemented for Speeduino (connected ECU "
                    "type: %s)", lt_ecu_type_name(t));
    }
    if (n != want) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                    "calibration table %s needs %lu bytes, got %lu", names[table],
                    (unsigned long)want, (unsigned long)n);
    }
    if (!p->modern) {
        lt_u8 hdr[2];

        hdr[0] = 't';
        hdr[1] = (lt_u8)table;
        rc = write_and_wait(p, hdr, 2);
        for (off = 0; rc == LT_OK && off < n; off += 128) {
            k = n - off < 128 ? n - off : 128;
            rc = progress(p, "calibration", table, off, n);
            if (rc == LT_OK) {
                rc = write_and_wait(p, wire + off, k);
            }
            if (rc == LT_OK) {
                sleep_ms(p, min_wait_ms(p) > 5 ? min_wait_ms(p) : 5);
            }
        }
        if (rc == LT_OK) {
            int d = p->ini ? ini_ms(p->ini->proto.page_activation_delay) : 500;

            sleep_ms(p, d > 500 ? d : 500);
        }
        return rc;
    }
    chunk = table == LT_CAL_O2 ? 256 : 64;
    for (off = 0; off < n; off += chunk) {
        k = n - off < chunk ? n - off : chunk;
        rc = progress(p, "calibration", table, off, n);
        if (rc) {
            return rc;
        }
        lt_buf_init(&pl);
        lt_buf_init(&reply);
        lt_buf_putc(&pl, 't');
        lt_buf_putc(&pl, 0);
        lt_buf_putc(&pl, table);
        lt_buf_putc(&pl, (int)((off >> 8) & 0xFF));
        lt_buf_putc(&pl, (int)(off & 0xFF));
        lt_buf_putc(&pl, (int)((k >> 8) & 0xFF));
        lt_buf_putc(&pl, (int)(k & 0xFF));
        lt_buf_append(&pl, wire + off, k);
        rc = send_packet(p, (const lt_u8 *)pl.data, pl.len, &reply);
        if (rc == LT_OK) {
            unsigned st = reply.len ? (unsigned)(lt_u8)reply.data[0] : 0xFFu;

            if (st != 0) {
                rc = fail(p, LT_EPROTO, LT_PE_PROTOCOL,
                          "calibration chunk at offset %lu rejected: 0x%02x (%s)",
                          (unsigned long)off, st, lt_rc_message((lt_u8)st));
            }
        }
        lt_buf_free(&pl);
        lt_buf_free(&reply);
        if (rc) {
            return rc;
        }
    }
    rc = lt_proto_read_calibration_crc(p, table, &crc);
    if (rc) {
        return rc;
    }
    if (crc != lt_proto_crc32(wire, n)) {
        return fail(p, LT_ECRC, LT_PE_PROTOCOL,
                    "calibration verify failed for %s: ECU stored CRC %08lx, expected %08lx",
                    names[table], (unsigned long)crc, (unsigned long)lt_proto_crc32(wire, n));
    }
    return LT_OK;
}

/* ---- logger ---- */
static int blob_send(lt_proto *p, const lt_ini_blob *b)
{
    lt_buf c;
    int rc;

    if (b->n == 0) {
        return LT_OK;
    }
    lt_buf_init(&c);
    lt_cmd_parse_string(b->p, b->n, &c);
    if (p->modern && c.len > 0) {
        /* every framed command is answered: consume the reply, or it would be taken for the
         * answer to the next command; an ECU that stays silent is not an error */
        lt_buf r;

        lt_buf_init(&r);
        rc = lt_proto_send_raw_response(p, (const lt_u8 *)c.data, c.len,
                                        lt_proto_effective_timeout_ms(p), &r);
        lt_buf_free(&r);
        if (rc == LT_ETIMEOUT) {
            rc = LT_OK;
        }
    } else {
        rc = lt_proto_send_raw(p, (const lt_u8 *)c.data, c.len);
    }
    lt_buf_free(&c);
    return rc;
}

int lt_proto_logger_start(lt_proto *p, const lt_ini_logger *lg)
{
    return blob_send(p, &lg->start_command);
}

int lt_proto_logger_stop(lt_proto *p, const lt_ini_logger *lg)
{
    return blob_send(p, &lg->stop_command);
}

int lt_proto_logger_read_begin(lt_proto *p, const lt_ini_logger *lg)
{
    lt_buf c;
    int rc, to;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (lg->data_read_command.n == 0) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL, "logger has no data read command");
    }
    lt_buf_init(&c);
    lt_cmd_parse_string(lg->data_read_command.p, lg->data_read_command.n, &c);
    to = lg->data_read_timeout_ms > 0 ? (int)lg->data_read_timeout_ms
                                      : lt_proto_effective_timeout_ms(p);
    rc = lt_proto_raw_begin(p, (const lt_u8 *)c.data, c.len, to);
    lt_buf_free(&c);
    return rc;
}

int lt_proto_logger_read(lt_proto *p, const lt_ini_logger *lg, lt_buf *out)
{
    lt_buf c;
    int rc, to;

    rc = need_conn(p);
    if (rc) {
        return rc;
    }
    if (lg->data_read_command.n == 0) {
        return fail(p, LT_EPROTO, LT_PE_PROTOCOL, "logger has no data read command");
    }
    lt_buf_init(&c);
    lt_cmd_parse_string(lg->data_read_command.p, lg->data_read_command.n, &c);
    to = lg->data_read_timeout_ms > 0 ? (int)lg->data_read_timeout_ms
                                      : lt_proto_effective_timeout_ms(p);
    rc = lt_proto_send_raw_response(p, (const lt_u8 *)c.data, c.len, to, out);
    lt_buf_free(&c);
    return rc;
}
