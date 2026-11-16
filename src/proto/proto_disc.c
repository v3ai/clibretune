/* proto_disc.c - serial port discovery / autodetect (spec 03 section 12, 6.1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "proto.h"

#include <string.h>

static const long default_bauds[] = { 115200L, 921600L, 460800L, 230400L, 57600L, 9600L };

void lt_disc_cfg_default(lt_disc_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->bauds = default_bauds;
    c->n_bauds = LT_COUNTOF(default_bauds);
    c->queries = "QS";
    c->post_open_settle_ms = 1500;
    c->pre_write_delay_ms = 40;
    c->read_window_ms = 600;
}

static int is_candidate(lt_disc_kind k)
{
    return k == LT_DISC_SIGNATURE || k == LT_DISC_BOOTLOADER || k == LT_DISC_BIGSTUFF3;
}

lt_disc_kind lt_disc_classify(const lt_u8 *b, size_t n, char query, char *sig, size_t sigcap)
{
    size_t i, printable = 0, s, e, k;

    if (sig && sigcap > 0) {
        sig[0] = '\0';
    }
    if (n == 0) {
        return LT_DISC_SILENT;
    }
    if (n >= 3 && b[0] == '$' && b[1] == 'G' && b[2] == 'P') {
        return LT_DISC_GPS;
    }
    for (i = 0; i < n; i++) {
        if (b[i] != (lt_u8)query) {
            break;
        }
    }
    if (i == n) {
        return LT_DISC_ECHO;
    }
    if (n >= 3 && (b[0] & 0xE0) == 0xE0 && (b[1] & 0xF0) == 0 && b[2] == '>') {
        return LT_DISC_BOOTLOADER;
    }
    if (b[0] == 0x01) {
        return LT_DISC_BIGSTUFF3;
    }
    for (i = 0; i < n; i++) {
        if ((b[i] >= 0x21 && b[i] <= 0x7E) || b[i] == ' ') {
            printable++;
        }
    }
    if (n >= 8 && printable * 100 / n >= 70) {
        if (sig && sigcap > 0) {
            s = 0;
            e = n;
            while (s < e && (b[s] <= ' ')) {
                s++;
            }
            while (e > s && (b[e - 1] <= ' ')) {
                e--;
            }
            k = e - s;
            if (k > sigcap - 1) {
                k = sigcap - 1;
            }
            memcpy(sig, b + s, k);
            sig[k] = '\0';
        }
        return LT_DISC_SIGNATURE;
    }
    return LT_DISC_UNKNOWN;
}

const lt_disc_result *lt_disc_best(const lt_disc_result *r, size_t n)
{
    static const lt_disc_kind prio[3] = { LT_DISC_SIGNATURE, LT_DISC_BOOTLOADER,
                                          LT_DISC_BIGSTUFF3 };
    size_t i, j;

    for (j = 0; j < 3; j++) {
        for (i = 0; i < n; i++) {
            if (r[i].kind == prio[j]) {
                return &r[i];
            }
        }
    }
    return NULL;
}

/* ---- default opener over lt_serial_open ---- */
static int default_open(void *ud, const char *port, long baud, lt_proto_io *io)
{
    lt_stream *s;

    LT_UNUSED(ud);
    s = lt_serial_open(port, baud);
    if (!s) {
        lt_err_set("cannot open %s", port);
        return LT_EIO;
    }
    *io = lt_proto_io_from_stream(s, LT_TRUE);
    return LT_OK;
}

static double d_now(const lt_disc_cfg *c)
{
    return c->clock.now_ms ? c->clock.now_ms(c->clock.ud) : lt_plat_now_ms();
}

static void d_sleep(const lt_disc_cfg *c, int ms)
{
    if (ms <= 0) {
        return;
    }
    if (c->clock.sleep_ms) {
        c->clock.sleep_ms(c->clock.ud, ms);
    } else {
        lt_plat_sleep_ms(ms);
    }
}

static void io_clear(lt_proto_io *io)
{
    lt_u8 tmp[256];
    int g = 0;

    if (io->clear_input) {
        io->clear_input(io->ud);
        return;
    }
    while (g++ < 1024 && io->read(io->ud, tmp, (int)sizeof(tmp), 0) > 0) {
    }
}

/* Collect a reply: first byte up to the window, then short follow-ups. */
static size_t read_window(const lt_disc_cfg *c, lt_proto_io *io, lt_u8 *buf, size_t cap)
{
    double t0 = d_now(c), t, rem;
    size_t total = 0;
    int r, to;

    for (;;) {
        t = d_now(c);
        rem = (double)c->read_window_ms - (t - t0);
        if (rem <= 0.0) {
            break;
        }
        to = total == 0 ? (int)rem + 1 : 30;
        if (to > (int)rem + 1) {
            to = (int)rem + 1;
        }
        r = io->read(io->ud, buf + total, (int)(cap - total), to);
        if (r <= 0) {
            if (r == 0 && total == 0 && d_now(c) - t < 1.0) {
                d_sleep(c, 1);
                continue;
            }
            break;
        }
        total += (size_t)r;
        if (total >= 32 || total >= cap) {
            break;
        }
    }
    return total;
}

int lt_disc_run(const lt_disc_cfg *cfg, const char *const *ports, size_t n_ports,
                lt_disc_result **results, size_t *n)
{
    lt_vec v;
    size_t pi, bi, qi, nq, got;
    lt_disc_open_fn open_fn = cfg->open ? cfg->open : default_open;
    lt_disc_result *r;
    lt_u8 buf[64];

    lt_vec_init(&v, sizeof(lt_disc_result));
    nq = strlen(cfg->queries ? cfg->queries : "QS");
    for (pi = 0; pi < n_ports; pi++) {
        lt_bool stop_port = LT_FALSE;

        for (bi = 0; bi < cfg->n_bauds && !stop_port; bi++) {
            lt_proto_io io;
            lt_bool got_any = LT_FALSE;
            const char *qs = cfg->queries ? cfg->queries : "QS";

            if (cfg->cancel && *cfg->cancel) {
                *results = (lt_disc_result *)v.data;
                *n = v.len;
                return LT_ECANCEL;
            }
            memset(&io, 0, sizeof(io));
            if (open_fn(cfg->open_ud, ports[pi], cfg->bauds[bi], &io) != 0) {
                r = (lt_disc_result *)lt_vec_push(&v);
                lt_strlcpy(r->port, ports[pi], sizeof(r->port));
                r->baud = cfg->bauds[bi];
                r->kind = LT_DISC_OPEN_FAILED;
                lt_strlcpy(r->error, lt_err_get(), sizeof(r->error));
                continue;
            }
            d_sleep(cfg, cfg->post_open_settle_ms);
            io_clear(&io);
            for (qi = 0; qi < nq; qi++) {
                lt_u8 q = (lt_u8)qs[qi];
                lt_disc_kind k;

                d_sleep(cfg, cfg->pre_write_delay_ms);
                io_clear(&io);
                if (io.write(io.ud, &q, 1) != 1) {
                    continue;
                }
                if (io.flush) {
                    io.flush(io.ud);
                }
                got = read_window(cfg, &io, buf, sizeof(buf));
                if (got == 0) {
                    continue;
                }
                got_any = LT_TRUE;
                r = (lt_disc_result *)lt_vec_push(&v);
                lt_strlcpy(r->port, ports[pi], sizeof(r->port));
                r->baud = cfg->bauds[bi];
                r->query = (char)q;
                k = lt_disc_classify(buf, got, (char)q, r->signature, sizeof(r->signature));
                r->kind = k;
                if (k == LT_DISC_GPS || is_candidate(k)) {
                    stop_port = LT_TRUE;
                    break;
                }
            }
            if (!got_any) {
                r = (lt_disc_result *)lt_vec_push(&v);
                lt_strlcpy(r->port, ports[pi], sizeof(r->port));
                r->baud = cfg->bauds[bi];
                r->kind = LT_DISC_SILENT;
            }
            if (io.close) {
                io.close(io.ud);
            }
        }
    }
    *results = (lt_disc_result *)v.data;
    *n = v.len;
    return LT_OK;
}

/* ---- port listing ---- */
static void port_key(const char *name, int *cls, long *num, const char **base)
{
    const char *b = strrchr(name, '/');
    const char *d;

    b = b ? b + 1 : name;
    *base = b;
    *num = 0;
    if (strncmp(b, "ttyACM", 6) == 0) {
        *cls = 0;
        d = b + 6;
    } else if (strncmp(b, "ttyUSB", 6) == 0) {
        *cls = 1;
        d = b + 6;
    } else {
        *cls = 2;
        return;
    }
    if (*d >= '0' && *d <= '9') {
        char *end;
        long v = 0;

        for (end = (char *)d; *end >= '0' && *end <= '9' && v < 100000000L; end++) {
            v = v * 10 + (*end - '0');
        }
        *num = *end == '\0' ? v : 2147483647L;
    } else {
        *num = 2147483647L;
    }
}

static int port_cmp(const char *a, const char *b)
{
    int ca, cb;
    long na, nb;
    const char *ba, *bb;

    port_key(a, &ca, &na, &ba);
    port_key(b, &cb, &nb, &bb);
    if (ca != cb) {
        return ca < cb ? -1 : 1;
    }
    if (ca < 2 && na != nb) {
        return na < nb ? -1 : 1;
    }
    return strcmp(ba, bb);
}

void lt_ports_sort(char **names, int n)
{
    int i, j;
    char *t;

    for (i = 1; i < n; i++) {
        t = names[i];
        for (j = i - 1; j >= 0 && port_cmp(names[j], t) > 0; j--) {
            names[j + 1] = names[j];
        }
        names[j + 1] = t;
    }
}

int lt_proto_list_ports(char ***names)
{
    int n = lt_serial_list(names);

    if (n > 1) {
        lt_ports_sort(*names, n);
    }
    return n;
}
