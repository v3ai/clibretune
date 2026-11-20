/* sim_link.c - a slow serial / USB / radio link between lt_proto and another lt_proto_io.
 *
 * The in-process simulator answers inside io.write, so without this model every reply is
 * complete before the host even looks for it and demo mode hides how long a real link keeps
 * the protocol waiting. The model releases the reply bytes over time:
 *   - request bytes take 10 bits each at `baud` to reach the ECU (8N1), back to back with the
 *     previous request;
 *   - the reply starts `latency_ms` after the request is complete (USB latency timer, radio,
 *     network) and then also takes 10 bits per byte;
 *   - bytes reach the host in packets of `chunk` bytes (USB full speed: 64), so reads see
 *     partial replies split at arbitrary points.
 * Fault injection: unsolicited noise bytes, lost replies, late replies, cut-off replies and a
 * dropped link. Reads with a timeout really wait (through the clock hooks), like a blocking
 * serial read does; clear_input discards only what has already arrived, exactly like
 * tcflush(): bytes still on the wire arrive afterwards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sim.h"

#include <string.h>

struct lt_simlink {
    lt_proto_io inner;
    lt_simlink_cfg cfg;
    double (*now_fn)(void *ud);
    void (*sleep_fn)(void *ud, int ms);
    void *clk_ud;
    lt_u8 *q;                 /* reply bytes not yet read by the host ... */
    double *qt;               /* ... and the time each one reaches the host */
    size_t head, len, cap;
    double tx_free, rx_free;  /* when the wire is free again in each direction */
    int drop_replies, cut_keep;
    double extra_delay;
    lt_bool dropped;
    lt_simlink_stats st;
};

static double sl_now(const lt_simlink *l)
{
    return l->now_fn ? l->now_fn(l->clk_ud) : lt_plat_now_ms();
}

static void sl_sleep(const lt_simlink *l, int ms)
{
    if (ms <= 0) {
        return;
    }
    if (l->sleep_fn) {
        l->sleep_fn(l->clk_ud, ms);
    } else {
        lt_plat_sleep_ms(ms);
    }
}

static double byte_ms(const lt_simlink *l)
{
    return l->cfg.baud > 0 ? 10000.0 / (double)l->cfg.baud : 0.0;
}

static void q_compact(lt_simlink *l)
{
    if (l->head > 0) {
        memmove(l->q, l->q + l->head, l->len);
        memmove(l->qt, l->qt + l->head, l->len * sizeof(double));
        l->head = 0;
    }
}

static void q_push(lt_simlink *l, lt_u8 b, double t)
{
    if (l->head + l->len >= l->cap) {
        q_compact(l);
        if (l->len >= l->cap) {
            size_t ncap, nbytes;

            if (!lt_mul_size(l->cap ? l->cap : 512, 2, &ncap) ||
                !lt_mul_size(ncap, sizeof(double), &nbytes)) {
                return;                       /* absurd backlog: drop the byte */
            }
            l->cap = ncap;
            l->q = (lt_u8 *)lt_realloc(l->q, ncap);
            l->qt = (double *)lt_realloc(l->qt, nbytes);
        }
    }
    l->q[l->head + l->len] = b;
    l->qt[l->head + l->len] = t;
    l->len++;
}

/* Queue n reply bytes that start leaving the ECU at `start`. */
static void q_reply(lt_simlink *l, const lt_u8 *b, size_t n, double start)
{
    double bm = byte_ms(l);
    size_t i, g, chunk = l->cfg.chunk > 1 ? (size_t)l->cfg.chunk : 1;

    if (start < l->rx_free) {
        start = l->rx_free;
    }
    for (i = 0; i < n; i++) {
        g = (i / chunk + 1) * chunk;          /* a packet reaches the host when complete */
        if (g > n) {
            g = n;
        }
        q_push(l, b[i], start + (double)g * bm);
    }
    l->rx_free = start + (double)n * bm;
}

/* Move whatever the inner device produced onto the wire. */
static void pull(lt_simlink *l, double reply_start)
{
    lt_u8 tmp[1024];
    lt_buf got;
    int r, guard;

    lt_buf_init(&got);
    for (guard = 0; guard < 4096; guard++) {
        r = l->inner.read(l->inner.ud, tmp, (int)sizeof(tmp), 0);
        if (r <= 0) {
            break;
        }
        lt_buf_append(&got, tmp, (size_t)r);
    }
    if (got.len > 0) {
        if (l->drop_replies > 0) {
            l->drop_replies--;
            l->st.replies_dropped++;
        } else {
            size_t n = got.len;

            if (l->cut_keep >= 0) {
                if ((size_t)l->cut_keep < n) {
                    n = (size_t)l->cut_keep;
                }
                l->cut_keep = -1;
            }
            q_reply(l, (const lt_u8 *)got.data, n, reply_start + l->extra_delay);
            l->extra_delay = 0.0;
        }
    }
    lt_buf_free(&got);
}

/* Hand over the bytes that have arrived by now. */
static int take(lt_simlink *l, lt_u8 *buf, int n)
{
    double t = sl_now(l);
    int k = 0;

    while (k < n && l->len > 0 && l->qt[l->head] <= t) {
        buf[k++] = l->q[l->head];
        l->head++;
        l->len--;
    }
    if (l->len == 0) {
        l->head = 0;
    }
    if (k > 0) {
        l->st.reads++;
        l->st.bytes_delivered += (unsigned long)k;
        if ((unsigned long)k > l->st.max_read) {
            l->st.max_read = (unsigned long)k;
        }
    }
    return k;
}

static int sl_read(void *ud, void *buf, int n, int timeout_ms)
{
    lt_simlink *l = (lt_simlink *)ud;
    int got, wait;
    double dt;

    if (l->dropped) {
        return -1;
    }
    if (n <= 0) {
        return 0;
    }
    pull(l, sl_now(l) + l->cfg.latency_ms);
    got = take(l, (lt_u8 *)buf, n);
    if (got == 0 && timeout_ms > 0) {
        wait = timeout_ms;
        if (l->len > 0) {
            dt = l->qt[l->head] - sl_now(l);
            if (dt < (double)wait) {
                wait = dt < 1.0 ? 1 : (int)dt + (dt > (double)(int)dt ? 1 : 0);
            }
        }
        l->st.blocked_ms += (double)wait;
        sl_sleep(l, wait);
        if (l->dropped) {
            return -1;
        }
        got = take(l, (lt_u8 *)buf, n);
    }
    return got;
}

static int sl_write(void *ud, const void *buf, int n)
{
    lt_simlink *l = (lt_simlink *)ud;
    double t, done;

    if (l->dropped) {
        return -1;
    }
    if (n <= 0) {
        return n < 0 ? -1 : 0;
    }
    t = sl_now(l);
    if (l->tx_free < t) {
        l->tx_free = t;
    }
    done = l->tx_free + (double)n * byte_ms(l);
    l->tx_free = done;
    l->st.bytes_written += (unsigned long)n;
    pull(l, t + l->cfg.latency_ms);           /* anything the device said on its own first */
    if (l->inner.write(l->inner.ud, buf, n) != n) {
        return -1;
    }
    pull(l, done + l->cfg.latency_ms);
    return n;
}

static void sl_flush(void *ud)
{
    lt_simlink *l = (lt_simlink *)ud;

    if (l->inner.flush) {
        l->inner.flush(l->inner.ud);
    }
}

static void sl_clear(void *ud)
{
    lt_simlink *l = (lt_simlink *)ud;
    lt_u8 tmp[256];

    pull(l, sl_now(l) + l->cfg.latency_ms);
    while (take(l, tmp, (int)sizeof(tmp)) > 0) {
    }
}

static void sl_close(void *ud)
{
    lt_simlink *l = (lt_simlink *)ud;

    if (l->inner.close) {
        l->inner.close(l->inner.ud);
    }
}

lt_simlink *lt_simlink_new(const lt_proto_io *inner, const lt_simlink_cfg *cfg)
{
    lt_simlink *l = (lt_simlink *)lt_calloc(1, sizeof(*l));

    l->inner = *inner;
    if (cfg) {
        l->cfg = *cfg;
    } else {
        l->cfg.baud = 115200L;
        l->cfg.latency_ms = 4.0;
        l->cfg.chunk = 64;
    }
    l->cut_keep = -1;
    return l;
}

void lt_simlink_free(lt_simlink *l)
{
    if (!l) {
        return;
    }
    lt_free(l->q);
    lt_free(l->qt);
    lt_free(l);
}

void lt_simlink_set_clock(lt_simlink *l, double (*now_fn)(void *ud),
                          void (*sleep_fn)(void *ud, int ms), void *ud)
{
    l->now_fn = now_fn;
    l->sleep_fn = sleep_fn;
    l->clk_ud = ud;
}

lt_proto_io lt_simlink_io(lt_simlink *l)
{
    lt_proto_io io;

    io.ud = l;
    io.read = sl_read;
    io.write = sl_write;
    io.flush = sl_flush;
    io.clear_input = sl_clear;
    io.close = sl_close;
    return io;
}

void lt_simlink_inject(lt_simlink *l, const lt_u8 *bytes, size_t n)
{
    q_reply(l, bytes, n, sl_now(l) + l->cfg.latency_ms);
}

void lt_simlink_drop_replies(lt_simlink *l, int n)
{
    l->drop_replies = n > 0 ? n : 0;
}

void lt_simlink_delay_reply(lt_simlink *l, double ms)
{
    l->extra_delay = ms > 0.0 ? ms : 0.0;
}

void lt_simlink_cut_reply(lt_simlink *l, int keep)
{
    l->cut_keep = keep;
}

void lt_simlink_set_dropped(lt_simlink *l, lt_bool dropped)
{
    l->dropped = dropped ? LT_TRUE : LT_FALSE;
}

size_t lt_simlink_pending(const lt_simlink *l)
{
    return l->len;
}

double lt_simlink_next_arrival(const lt_simlink *l)
{
    return l->len > 0 ? l->qt[l->head] : -1.0;
}

const lt_simlink_stats *lt_simlink_stats_get(const lt_simlink *l)
{
    return &l->st;
}
