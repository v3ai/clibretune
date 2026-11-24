/* test_proto_conn.c - lt_proto against an in-memory fake ECU (modern CRC envelope
 * and legacy raw), with a fake clock so nothing really sleeps. */
#include "lt.h"
#include "proto/proto.h"
#include "lt_test.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* fake clock + fake ECU                                                */
/* ------------------------------------------------------------------ */
typedef struct fclk { double t; } fclk;

static double f_now(void *ud)
{
    return ((fclk *)ud)->t;
}

static void f_sleep(void *ud, int ms)
{
    ((fclk *)ud)->t += ms;
}

#define MAXLOG 128

typedef struct fdev {
    fclk *clk;
    int legacy;
    lt_env_order order;
    lt_buf in, rx;
    size_t rxpos;
    lt_u8 page[3][512];
    unsigned long psize[3];
    char sig[64];
    int n_burn;
    int burn_pages[32];
    lt_buf plog[MAXLOG];
    double plog_t[MAXLOG];
    int nplog;
    int nwrites;
    size_t write_len[MAXLOG];
    double write_t[MAXLOG];
    /* faults */
    int stall, trunc, wr_status, crc_bad_env, short_read, och_nostatus, och_err;
    const char *wr_msg;
    int corrupt_page, corrupt_off;
    int crc_bad;
    int ring_cap;
    double ring_buffered, ring_t;
    unsigned dropped;
    long baud;
    double latency, ready_at;
    /* calibration */
    lt_u8 cal[3][1024];
    int cal_len[3];
    int cal_stream;
    int cal_bad, cal_reject;
    /* console */
    char console_out[256];
    int och_size;
} fdev;

static void rx_put(fdev *d, const lt_u8 *p, size_t n)
{
    lt_buf_append(&d->rx, p, n);
}

static void reply(fdev *d, const lt_u8 *p, size_t n)
{
    lt_buf f;
    size_t take;

    if (d->stall) {
        return;
    }
    lt_buf_init(&f);
    lt_env_encode(d->order, p, n, &f);
    if (d->crc_bad_env) {
        f.data[f.len - 1] = (char)(f.data[f.len - 1] ^ 0x55);
    }
    take = f.len;
    if (d->trunc > 0 && (size_t)d->trunc < take) {
        take = (size_t)d->trunc;
    }
    d->ready_at = d->clk->t + d->latency;
    rx_put(d, (const lt_u8 *)f.data, take);
    lt_buf_free(&f);
}

static void reply_status(fdev *d, lt_u8 st, const lt_u8 *data, size_t n)
{
    lt_buf b;

    lt_buf_init(&b);
    lt_buf_putc(&b, st);
    if (n) {
        lt_buf_append(&b, data, n);
    }
    reply(d, (const lt_u8 *)b.data, b.len);
    lt_buf_free(&b);
}

static void store_write(fdev *d, int pg, unsigned off, const lt_u8 *data, unsigned n)
{

    if (pg < 0 || pg > 2 || off + n > d->psize[pg]) {
        return;
    }
    memcpy(d->page[pg] + off, data, n);
    if (d->corrupt_page == pg && d->corrupt_off >= (int)off && d->corrupt_off < (int)(off + n)) {
        d->page[pg][d->corrupt_off] = (lt_u8)~d->page[pg][d->corrupt_off];
    }
}

static void log_payload(fdev *d, const lt_u8 *p, size_t n)
{
    if (d->nplog < MAXLOG) {
        lt_buf_init(&d->plog[d->nplog]);
        lt_buf_append(&d->plog[d->nplog], p, n);
        d->plog_t[d->nplog] = d->clk->t;
        d->nplog++;
    }
}

static void note_write(fdev *d, size_t len)
{
    if (d->nwrites < MAXLOG) {
        d->write_len[d->nwrites] = len;
        d->write_t[d->nwrites] = d->clk->t;
        d->nwrites++;
    }
}

/* one command (payload for modern, command bytes for legacy). Returns number of bytes the
 * command needs in legacy mode (0 = need more) so the caller can frame. */
static void modern_cmd(fdev *d, const lt_u8 *p, size_t n)
{
    lt_u8 tmp[600];
    unsigned pg, off, cnt, i;
    lt_u32 crc;

    log_payload(d, p, n);
    switch (p[0]) {
    case 'S':
    case 'Q':
        reply_status(d, 0, (const lt_u8 *)d->sig, strlen(d->sig));
        break;
    case 'R':
        pg = (unsigned)lt_rd_be(p + 1, 2);
        off = (unsigned)lt_rd_be(p + 3, 2);
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        if (pg > 2 || off + cnt > d->psize[pg]) {
            reply_status(d, 0x84, NULL, 0);
        } else {
            if (d->short_read && cnt > 0) {
                cnt--;
            }
            reply_status(d, 0, d->page[pg] + off, cnt);
        }
        break;
    case 'C':
        pg = (unsigned)lt_rd_be(p + 1, 2);
        off = (unsigned)lt_rd_be(p + 3, 2);
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        note_write(d, n);
        if (d->wr_status) {
            reply_status(d, (lt_u8)d->wr_status, (const lt_u8 *)d->wr_msg,
                         d->wr_msg ? strlen(d->wr_msg) : 0);
        } else {
            store_write(d, (int)pg, off, p + 7, cnt);
            reply_status(d, 0, NULL, 0);
        }
        break;
    case 'B':
        if (d->n_burn < 32) {
            d->burn_pages[d->n_burn] = (int)lt_rd_be(p + 1, 2);
        }
        d->n_burn++;
        reply_status(d, 2, NULL, 0);
        break;
    case 'k':
        if (n == 3) {
            int id = p[2];

            crc = lt_proto_crc32(d->cal[id], (size_t)d->cal_len[id]);
            if (d->cal_bad) {
                crc ^= 1;
            }
            lt_wr_be(tmp, 4, crc);
            reply_status(d, 0, tmp, 4);
        } else {
            pg = (unsigned)lt_rd_be(p + 1, 2);
            off = (unsigned)lt_rd_be(p + 3, 2);
            cnt = (unsigned)lt_rd_be(p + 5, 2);
            if (pg > 2 || off + cnt > d->psize[pg]) {           /* not one of this device's pages */
                reply_status(d, 0x84, NULL, 0);
                break;
            }
            crc = lt_proto_crc32(d->page[pg] + off, cnt);
            if (d->crc_bad) {
                crc ^= 0x100;
            }
            lt_wr_be(tmp, 4, crc);
            reply_status(d, 0, tmp, 4);
        }
        break;
    case 't': {
        int id = p[2];

        off = (unsigned)lt_rd_be(p + 3, 2);
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        if (d->cal_reject) {
            reply_status(d, 0x84, NULL, 0);
            break;
        }
        memcpy(d->cal[id] + off, p + 7, cnt);
        if ((int)(off + cnt) > d->cal_len[id]) {
            d->cal_len[id] = (int)(off + cnt);
        }
        reply_status(d, 0, NULL, 0);
        break;
    }
    case 'O':
        off = (unsigned)lt_rd_be(p + 1, 2);
        cnt = (unsigned)lt_rd_be(p + 3, 2);
        (void)cnt;
        for (i = 0; i < (unsigned)d->och_size; i++) {
            tmp[i] = (lt_u8)(0xA0 + i);
        }
        if (d->och_err) {
            reply_status(d, 0, tmp, (size_t)d->och_size - 3);
        } else if (d->och_nostatus) {
            reply(d, tmp, (size_t)d->och_size);
        } else {
            reply_status(d, 0, tmp, (size_t)d->och_size);
        }
        break;
    case 'A':
        for (i = 0; i < (unsigned)d->och_size; i++) {
            tmp[i] = (lt_u8)(0xB0 + i);
        }
        reply_status(d, 0, tmp, (size_t)d->och_size);
        break;
    case 'G':
        if (d->console_out[0]) {
            reply_status(d, 0, (const lt_u8 *)d->console_out, strlen(d->console_out));
            d->console_out[0] = '\0';
        } else {
            reply_status(d, 0, NULL, 0);
        }
        break;
    case 'E':
        lt_snprintf(d->console_out, sizeof(d->console_out), "msg`ran: %.*s`", (int)(n - 1),
                    (const char *)p + 1);
        reply_status(d, 0, NULL, 0);
        break;
    case 'Z':
        break;   /* controller button: no reply */
    default:
        reply_status(d, 0x83, NULL, 0);
        break;
    }
}

static void rawtx(fdev *d, const lt_u8 *p, size_t n)
{
    if (!d->stall) {
        d->ready_at = d->clk->t + d->latency;
        rx_put(d, p, n);
    }
}

/* legacy: try to parse one command at the front of `in`; returns bytes used or 0 */
static size_t legacy_cmd(fdev *d)
{
    const lt_u8 *p = (const lt_u8 *)d->in.data;
    size_t n = d->in.len;
    unsigned pg, off, cnt, i;
    lt_u8 tmp[600];

    if (d->cal_stream >= 0) {
        int id = d->cal_stream;
        int want = id == 2 ? 1024 : 64;
        size_t take = n;

        if ((size_t)(want - d->cal_len[id]) < take) {
            take = (size_t)(want - d->cal_len[id]);
        }
        memcpy(d->cal[id] + d->cal_len[id], p, take);
        d->cal_len[id] += (int)take;
        if (d->cal_len[id] >= want) {
            d->cal_stream = -1;
        }
        return n;
    }
    switch (p[0]) {
    case 'Q':
    case 'S':
        log_payload(d, p, 1);
        rawtx(d, (const lt_u8 *)d->sig, strlen(d->sig));
        return 1;
    case 'r':
    case 'R':
        if (n < 7) {
            return 0;
        }
        log_payload(d, p, 7);
        pg = (unsigned)lt_rd_be(p + 1, 2);
        off = (unsigned)lt_rd_be(p + 3, 2);
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        if (pg <= 2 && off + cnt <= d->psize[pg]) {
            if (d->short_read && cnt > 0) {
                cnt--;
            }
            rawtx(d, d->page[pg] + off, cnt);
        }
        return 7;
    case 'C':
    case 'M':
        if (n < 7) {
            return 0;
        }
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        if (n < 7 + cnt) {
            return 0;
        }
        log_payload(d, p, 7 + cnt);
        note_write(d, 7 + cnt);
        pg = (unsigned)lt_rd_be(p + 1, 2);
        off = (unsigned)lt_rd_be(p + 3, 2);
        store_write(d, (int)pg, off, p + 7, cnt);
        return 7 + cnt;
    case 'B':
    case 'b':
        if (n < 3) {
            return 0;
        }
        log_payload(d, p, 3);
        if (d->n_burn < 32) {
            d->burn_pages[d->n_burn] = (int)lt_rd_be(p + 1, 2);
        }
        d->n_burn++;
        return 3;
    case 'k':
        if (n < 7) {
            return 0;
        }
        pg = (unsigned)lt_rd_be(p + 1, 2);
        off = (unsigned)lt_rd_be(p + 3, 2);
        cnt = (unsigned)lt_rd_be(p + 5, 2);
        lt_wr_be(tmp, 4, lt_proto_crc32(d->page[pg] + off, cnt) ^ (d->crc_bad ? 0x100UL : 0));
        rawtx(d, tmp, 4);
        return 7;
    case 'A':
        for (i = 0; i < (unsigned)d->och_size; i++) {
            tmp[i] = (lt_u8)(0xB0 + i);
        }
        log_payload(d, p, 1);
        rawtx(d, tmp, (size_t)d->och_size);
        return 1;
    case 'O':
        if (n < 5) {
            return 0;
        }
        for (i = 0; i < (unsigned)d->och_size; i++) {
            tmp[i] = (lt_u8)(0xA0 + i);
        }
        log_payload(d, p, 5);
        rawtx(d, tmp, (size_t)d->och_size);
        return 5;
    case 't':
        if (n < 2) {
            return 0;
        }
        log_payload(d, p, 2);
        d->cal_stream = p[1];
        d->cal_len[p[1]] = 0;
        return 2;
    default:
        return n;   /* garbage */
    }
}

static void process_in(fdev *d)
{
    for (;;) {
        if (d->in.len == 0) {
            return;
        }
        if (d->legacy) {
            size_t u = legacy_cmd(d);

            if (u == 0) {
                return;
            }
            memmove(d->in.data, d->in.data + u, d->in.len - u);
            d->in.len -= u;
        } else {
            size_t N, tot;
            lt_u8 *fr = (lt_u8 *)d->in.data;

            if (d->in.len < 2) {
                return;
            }
            N = (size_t)(d->order == LT_ENV_LITTLE ? lt_rd_le(fr, 2) : lt_rd_be(fr, 2));
            tot = 2 + N + 4;
            if (d->in.len < tot) {
                return;
            }
            if (N > 0) {
                modern_cmd(d, fr + 2, N);
            }
            memmove(d->in.data, d->in.data + tot, d->in.len - tot);
            d->in.len -= tot;
        }
    }
}

static int fd_read(void *ud, void *buf, int n, int timeout_ms)
{
    fdev *d = (fdev *)ud;
    size_t avail = d->rx.len - d->rxpos, k;

    if (avail > 0 && d->clk->t < d->ready_at) {
        double wait = d->ready_at - d->clk->t;

        d->clk->t += wait < timeout_ms ? wait : timeout_ms;
        if (d->clk->t < d->ready_at) {
            return 0;
        }
    }
    if (avail == 0) {
        d->clk->t += timeout_ms;
        return 0;
    }
    k = avail < (size_t)n ? avail : (size_t)n;
    memcpy(buf, d->rx.data + d->rxpos, k);
    d->rxpos += k;
    if (d->rxpos == d->rx.len) {
        lt_buf_clear(&d->rx);
        d->rxpos = 0;
    }
    return (int)k;
}

static int fd_write(void *ud, const void *buf, int n)
{
    fdev *d = (fdev *)ud;
    int accept = n;

    if (d->ring_cap > 0) {
        double drained = (d->clk->t - d->ring_t) * (double)d->baud / 10000.0;

        d->ring_buffered -= drained;
        if (d->ring_buffered < 0) {
            d->ring_buffered = 0;
        }
        d->ring_t = d->clk->t;
        if (d->ring_buffered + n > d->ring_cap) {
            accept = d->ring_cap - (int)d->ring_buffered;
            if (accept < 0) {
                accept = 0;
            }
            d->dropped += (unsigned)(n - accept);
        }
        d->ring_buffered += accept;
    }
    lt_buf_append(&d->in, buf, (size_t)accept);
    process_in(d);
    return n;
}

static void fd_clear(void *ud)
{
    fdev *d = (fdev *)ud;

    lt_buf_clear(&d->rx);
    d->rxpos = 0;
    lt_buf_clear(&d->in);
}

static void fd_init(fdev *d, fclk *clk)
{
    int i, j;

    memset(d, 0, sizeof(*d));
    d->clk = clk;
    d->order = LT_ENV_LITTLE;
    lt_buf_init(&d->in);
    lt_buf_init(&d->rx);
    d->psize[0] = 288;
    d->psize[1] = 128;
    d->psize[2] = 64;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < (int)d->psize[i]; j++) {
            d->page[i][j] = (lt_u8)(j * 7 + i * 13 + 1);
        }
    }
    lt_strlcpy(d->sig, "speeduino 202501-test", sizeof(d->sig));
    d->corrupt_page = -1;
    d->cal_stream = -1;
    d->och_size = 16;
    d->baud = 115200L;
}

static void fd_free(fdev *d)
{
    int i;

    lt_buf_free(&d->in);
    lt_buf_free(&d->rx);
    for (i = 0; i < d->nplog; i++) {
        lt_buf_free(&d->plog[i]);
    }
}

/* ------------------------------------------------------------------ */
/* test environment                                                     */
/* ------------------------------------------------------------------ */
typedef struct tenv {
    fclk clk;
    fdev dev;
    lt_ini *ini;
    lt_proto *p;
    lt_proto_clock pclk;
    lt_ecu_mem *mem;
} tenv;

typedef struct iniopt {
    const char *sig;
    int modern;
    int timeout;       /* blockReadTimeout */
    const char *rd, *wr, *burn, *crc;
    int interwrite;
    const char *extra_constants;
} iniopt;

static lt_ini *make_ini(const iniopt *o)
{
    static char text[6000];
    lt_ini *ini;

    lt_snprintf(text, sizeof(text),
        "[MegaTune]\n signature = \"%s\"\n"
        "[TunerStudio]\n queryCommand = \"S\"\n"
        "[Constants]\n%s endianness = big\n nPages = 3\n pageSize = 288, 128, 64\n"
        " pageReadCommand = %s\n burnCommand = %s\n pageChunkWrite = %s\n pageValueWrite = %s\n"
        "%s blockingFactor = 251\n pageActivationDelay = 500\n interWriteDelay = %d\n"
        " blockReadTimeout = %d\n%s\n"
        "[BurstMode]\n getCommand = \"A\"\n"
        "[OutputChannels]\n ochGetCommand = \"O%%2o%%2c\"\n ochBlockSize = 16\n"
        "[ControllerCommands]\n cmdZ = \"Z\\x00\\x12\"\n cmdZ2 = \"Z\\x01\"\n"
        " cmdBoth = \"Z\\x02\", \"Z\\x03\"\n",
        o->sig, o->modern ? " messageEnvelopeFormat = msEnvelope_1.0\n" : "",
        o->rd, o->burn, o->wr, o->wr, o->crc, o->interwrite, o->timeout,
        o->extra_constants ? o->extra_constants : "");
    ini = lt_ini_load_string(text, strlen(text), NULL, NULL);
    return ini;
}

static const char *R3 = "\"R%2i%2o%2c\", \"R%2i%2o%2c\", \"R%2i%2o%2c\"";
static const char *C3 = "\"C%2i%2o%2c%v\", \"C%2i%2o%2c%v\", \"C%2i%2o%2c%v\"";
static const char *B3 = "\"B%2i\", \"B%2i\", \"\"";
static const char *K3 = " crc32CheckCommand = \"k%2i%2o%2c\", \"k%2i%2o%2c\", \"k%2i%2o%2c\"\n";

static void tenv_open(tenv *e, const iniopt *o, lt_bool legacy, lt_env_order ecu_order,
                      const lt_proto_cfg *cfgin)
{
    lt_proto_io io;
    lt_proto_cfg cfg;

    memset(e, 0, sizeof(*e));
    fd_init(&e->dev, &e->clk);
    e->dev.legacy = legacy;
    e->dev.order = ecu_order;
    if (o->sig) {
        lt_strlcpy(e->dev.sig, o->sig, sizeof(e->dev.sig));
    }
    e->ini = make_ini(o);
    if (cfgin) {
        cfg = *cfgin;
    } else {
        lt_proto_cfg_default(&cfg);
    }
    memset(&io, 0, sizeof(io));
    io.ud = &e->dev;
    io.read = fd_read;
    io.write = fd_write;
    io.clear_input = fd_clear;
    e->p = lt_proto_new(&io, e->ini, &cfg);
    e->pclk.ud = &e->clk;
    e->pclk.now_ms = f_now;
    e->pclk.sleep_ms = f_sleep;
    lt_proto_set_clock(e->p, &e->pclk);
    e->mem = lt_ecu_mem_from_ini(e->ini);
}

static void tenv_close(tenv *e)
{
    lt_proto_free(e->p);
    lt_ecu_mem_free(e->mem);
    lt_ini_free(e->ini);
    fd_free(&e->dev);
}

static iniopt speed_modern(void)
{
    iniopt o;

    memset(&o, 0, sizeof(o));
    o.sig = "speeduino 202501-test";
    o.modern = 1;
    o.timeout = 500;
    o.rd = R3;
    o.wr = C3;
    o.burn = B3;
    o.crc = K3;
    return o;
}

static iniopt rusefi_modern(void)
{
    iniopt o = speed_modern();

    o.sig = "rusEFI master.2025.test";
    return o;
}

static iniopt legacy_opt(void)
{
    iniopt o = speed_modern();

    o.modern = 0;
    o.crc = "";
    return o;
}

/* ------------------------------------------------------------------ */
/* tests                                                                */
/* ------------------------------------------------------------------ */
LT_TEST(connect_modern_speeduino_le)
{
    tenv e;
    iniopt o = speed_modern();

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(e.ini && e.ini->ecu_type == LT_ECU_SPEEDUINO);
    LT_CHECK(lt_proto_state(e.p) == LT_CONN_DISCONNECTED);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_state(e.p) == LT_CONN_CONNECTED);
    LT_CHECK_STR(lt_proto_signature(e.p), "speeduino 202501-test");
    LT_CHECK(lt_proto_is_modern(e.p));
    LT_CHECK(lt_proto_envelope_order(e.p) == LT_ENV_LITTLE);
    LT_CHECK(!lt_proto_signature_mismatch(e.p));
    LT_CHECK(lt_proto_connect(e.p) == LT_EBUSY);
    LT_CHECK(lt_proto_stats_get(e.p)->tx_packets >= 1 && lt_proto_stats_get(e.p)->rx_bytes > 6);
    /* default delayAfterPortOpen 0 + 20 ms settle */
    LT_CHECK(e.clk.t >= 20.0);
    tenv_close(&e);
}

LT_TEST(connect_flips_envelope_order)
{
    tenv e;
    iniopt o = rusefi_modern();

    /* INI says rusEFI (BE) but this ECU speaks LE */
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_envelope_order(e.p) == LT_ENV_LITTLE);
    LT_CHECK_STR(lt_proto_signature(e.p), "rusEFI master.2025.test");
    LT_CHECK(e.clk.t >= 450.0);     /* flip retry waits 450 ms */
    tenv_close(&e);
    /* and the other way round: speeduino INI (LE), ECU BE */
    o = speed_modern();
    tenv_open(&e, &o, LT_FALSE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_envelope_order(e.p) == LT_ENV_BIG && lt_proto_is_modern(e.p));
    tenv_close(&e);
}

LT_TEST(connect_legacy_and_no_ini)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_proto_io io;
    lt_proto *p;
    lt_proto_clock ck;

    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(!lt_proto_is_modern(e.p));
    LT_CHECK_STR(lt_proto_signature(e.p), "speeduino 202501-test");
    tenv_close(&e);
    /* no INI at all: legacy plain, query "S", 1000 ms delay after open */
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    memset(&io, 0, sizeof(io));
    io.ud = &e.dev;
    io.read = fd_read;
    io.write = fd_write;
    p = lt_proto_new(&io, NULL, NULL);
    ck = e.pclk;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    LT_CHECK(e.clk.t >= 1000.0);
    LT_CHECK_STR(lt_proto_signature(p), "speeduino 202501-test");
    LT_CHECK(e.dev.plog[0].data[0] == 'S');
    lt_proto_free(p);
    tenv_close(&e);
}

LT_TEST(connect_silent_ecu_times_out)
{
    tenv e;
    iniopt o = speed_modern();

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    e.dev.stall = 1;
    LT_CHECK(lt_proto_connect(e.p) == LT_ETIMEOUT);
    LT_CHECK(lt_proto_state(e.p) == LT_CONN_ERROR);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_TIMEOUT);
    LT_CHECK(strstr(lt_err_get(), "timeout") != NULL);
    LT_CHECK(lt_proto_envelope_order(e.p) == LT_ENV_LITTLE);   /* restored to declared */
    /* operations refuse while not connected */
    {
        lt_buf b;
        lt_buf_init(&b);
        LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 4, &b) == LT_ENOTCONN);
        lt_buf_free(&b);
    }
    tenv_close(&e);
}

LT_TEST(signature_mismatch_flag)
{
    tenv e;
    iniopt o = speed_modern();

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    lt_strlcpy(e.dev.sig, "speeduino 202412-other", sizeof(e.dev.sig));
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_signature_mismatch(e.p));
    tenv_close(&e);
}

LT_TEST(read_page_modern_chunks)
{
    tenv e;
    iniopt o = speed_modern();
    const lt_u8 *img;
    size_t len = 0, i;
    int ok = 1;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_read_page(e.p, 0, e.mem) == 0);
    LT_CHECK(lt_ecu_mem_state(e.mem, 0) == LT_PAGE_CLEAN);
    img = lt_ecu_mem_page(e.mem, 0, &len);
    LT_CHECK(img != NULL && len == 288);
    for (i = 0; img && i < len; i++) {
        if (img[i] != e.dev.page[0][i]) {
            ok = 0;
        }
    }
    LT_CHECK(ok);
    /* blockingFactor 251: reads of 251 then 37, command bytes R page off count (BE) */
    LT_CHECK(e.dev.nplog == 3);   /* query + 2 reads */
    LT_CHECK(e.dev.plog[1].len == 7 && (lt_u8)e.dev.plog[1].data[0] == 'R');
    LT_CHECK(lt_rd_be((const lt_u8 *)e.dev.plog[1].data + 5, 2) == 251);
    LT_CHECK(lt_rd_be((const lt_u8 *)e.dev.plog[2].data + 3, 2) == 251);
    LT_CHECK(lt_rd_be((const lt_u8 *)e.dev.plog[2].data + 5, 2) == 37);
    tenv_close(&e);
}

static int prog_cancel_at;
static int prog_calls;

static int prog_fn(void *ud, const char *phase, int page, size_t done, size_t total)
{
    LT_UNUSED(ud);
    LT_UNUSED(phase);
    LT_UNUSED(page);
    LT_UNUSED(done);
    LT_UNUSED(total);
    prog_calls++;
    return (prog_cancel_at > 0 && prog_calls >= prog_cancel_at) ? 1 : 0;
}

LT_TEST(read_all_progress_and_cancel)
{
    tenv e;
    iniopt o = speed_modern();

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    prog_calls = 0;
    prog_cancel_at = 2;
    lt_proto_set_progress(e.p, prog_fn, NULL);
    LT_CHECK(lt_proto_read_all(e.p, e.mem, LT_FALSE) == LT_ECANCEL);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_CANCELLED);
    LT_CHECK(!lt_ecu_mem_fully_loaded(e.mem));
    LT_CHECK(lt_ecu_mem_state(e.mem, 0) == LT_PAGE_NOT_LOADED);
    LT_CHECK(lt_proto_state(e.p) == LT_CONN_CONNECTED);
    /* cancel consumed: the next call works */
    prog_cancel_at = 0;
    LT_CHECK(lt_proto_read_all(e.p, e.mem, LT_FALSE) == 0);
    LT_CHECK(lt_ecu_mem_fully_loaded(e.mem));
    LT_CHECK(prog_calls > 4);
    /* only NOT_LOADED pages are re-read */
    {
        int before = e.dev.nplog;

        LT_CHECK(lt_proto_read_all(e.p, e.mem, LT_FALSE) == 0);
        LT_CHECK(e.dev.nplog == before);
        LT_CHECK(lt_proto_read_all(e.p, e.mem, LT_TRUE) == 0);
        LT_CHECK(e.dev.nplog > before);
    }
    tenv_close(&e);
}

LT_TEST(read_errors)
{
    tenv e;
    iniopt o = speed_modern();
    lt_buf b;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    lt_buf_init(&b);
    e.dev.short_read = 1;
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 16, &b) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "wrong-length read on page 0 offset 0: got 15 of 16") != NULL);
    LT_CHECK(b.len == 0);
    e.dev.short_read = 0;
    /* out of range: status 0x84 -> "Read error, status: 132" */
    LT_CHECK(lt_proto_read_memory(e.p, 0, 1000, 16, &b) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "Read error, status: 132") != NULL);
    LT_CHECK(lt_proto_read_memory(e.p, 1, 0, 16, &b) == 0 && b.len == 16);
    /* page not in INI size table */
    LT_CHECK(lt_proto_read_page(e.p, 7, e.mem) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "Unknown page size for page 7") != NULL);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(write_page_modern_chunks_and_verify)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 img[288];
    int i;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    for (i = 0; i < 288; i++) {
        img[i] = (lt_u8)(255 - i);
    }
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_effective_write_chunk(e.p) == 243);
    LT_CHECK(lt_proto_write_page(e.p, 0, img, 288) == 0);
    LT_CHECK(memcmp(e.dev.page[0], img, 288) == 0);
    LT_CHECK(e.dev.nwrites == 2);
    LT_CHECK(e.dev.write_len[0] == 7 + 243 && e.dev.write_len[1] == 7 + 45);
    LT_CHECK(e.dev.write_t[1] > e.dev.write_t[0]);       /* inter-frame delay applied */
    LT_CHECK(lt_proto_dirty_pages(e.p, NULL, 0) == 1);
    /* the CRC verify command was issued */
    LT_CHECK((lt_u8)e.dev.plog[e.dev.nplog - 1].data[0] == 'k');
    tenv_close(&e);
}

LT_TEST(write_page_crc_mismatch_blocks_burn)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 img[288];
    lt_u8 one[2] = { 1, 2 };
    int pages[8];

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    memset(img, 0x33, sizeof(img));
    LT_CHECK(lt_proto_connect(e.p) == 0);
    e.dev.crc_bad = 1;
    LT_CHECK(lt_proto_write_page(e.p, 0, img, 288) == LT_ECRC);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_PAGE_CRC_MISMATCH);
    LT_CHECK(lt_proto_errinfo(e.p)->a != lt_proto_errinfo(e.p)->b);
    /* burn refused */
    LT_CHECK(lt_proto_burn(e.p, 0, NULL) == LT_EBUSY);
    LT_CHECK(strstr(lt_err_get(), "refusing to burn: page(s) 0 failed") != NULL);
    LT_CHECK(lt_proto_burn(e.p, -1, NULL) == LT_EBUSY);
    LT_CHECK(e.dev.n_burn == 0);
    /* write to another page skips the auto burn instead of failing */
    LT_CHECK(lt_proto_write_memory(e.p, 1, 0, one, 2) == 0);
    LT_CHECK(e.dev.n_burn == 0);
    /* a good rewrite clears the failure */
    e.dev.crc_bad = 0;
    LT_CHECK(lt_proto_write_page(e.p, 0, img, 288) == 0);
    LT_CHECK(lt_proto_burn(e.p, -1, NULL) == 0);
    LT_CHECK(lt_proto_dirty_pages(e.p, pages, 8) == 0);
    tenv_close(&e);
}

LT_TEST(write_status_errors)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 d[4] = { 1, 2, 3, 4 };

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    e.dev.wr_status = 0x84;
    LT_CHECK(lt_proto_write_memory(e.p, 0, 0, d, 4) == LT_EPROTO);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_ECU_STATUS);
    LT_CHECK(lt_proto_errinfo(e.p)->a == 0x84);
    LT_CHECK_STR(lt_proto_errinfo(e.p)->msg, "ECU error 0x84: Controller Reported a Out of Range");
    e.dev.wr_status = 0x94;
    e.dev.wr_msg = "flash write failed";
    LT_CHECK(lt_proto_write_memory(e.p, 0, 0, d, 4) == LT_EPROTO);
    LT_CHECK_STR(lt_proto_errinfo(e.p)->msg, "ECU error 0x94: flash write failed");
    /* write_page fails and marks the page failed */
    LT_CHECK(lt_proto_write_page(e.p, 0, d, 4) == LT_EPROTO);
    LT_CHECK(lt_proto_burn(e.p, 0, NULL) == LT_EBUSY);
    tenv_close(&e);
}

LT_TEST(write_page_retries_transient_timeouts)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 d[4] = { 1, 2, 3, 4 };
    double t0;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    e.dev.stall = 1;
    t0 = e.clk.t;
    LT_CHECK(lt_proto_write_page(e.p, 0, d, 4) == LT_ETIMEOUT);
    /* 3 attempts of ~500 ms plus 50+100 ms backoff */
    LT_CHECK(e.clk.t - t0 >= 1650.0);
    LT_CHECK(e.dev.nwrites == 3);
    tenv_close(&e);
}

LT_TEST(burn_timing_and_dirty_tracking)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 d[2] = { 5, 6 };
    int pages[8];
    double t0;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_write_memory(e.p, 0, 0, d, 2) == 0);
    /* changing page auto-burns the previous one */
    LT_CHECK(lt_proto_write_memory(e.p, 1, 0, d, 2) == 0);
    LT_CHECK(e.dev.n_burn == 1 && e.dev.burn_pages[0] == 0);
    LT_CHECK(lt_proto_dirty_pages(e.p, pages, 8) == 1 && pages[0] == 1);
    t0 = e.clk.t;
    LT_CHECK(lt_proto_burn(e.p, 1, NULL) == 0);
    LT_CHECK(e.clk.t - t0 >= 2600.0 && e.clk.t - t0 < 2700.0);   /* max(500,2000)+600 */
    LT_CHECK(lt_proto_dirty_pages(e.p, NULL, 0) == 0);
    /* page 2 has an empty burnCommand: OK, nothing sent */
    LT_CHECK(lt_proto_write_memory(e.p, 2, 0, d, 2) == 0);
    {
        int nb = e.dev.n_burn;

        t0 = e.clk.t;
        LT_CHECK(lt_proto_burn(e.p, 2, NULL) == 0);
        LT_CHECK(e.dev.n_burn == nb && e.clk.t == t0);
        LT_CHECK(lt_proto_dirty_pages(e.p, pages, 8) == 1 && pages[0] == 2);
    }
    /* nothing dirty besides page 2: burn all burns just that one */
    LT_CHECK(lt_proto_burn(e.p, -1, NULL) == 0);
    /* with an empty set burn(-1) targets page 0 */
    tenv_close(&e);
    /* nothing written at all: burn(-1) targets page 0 */
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_burn(e.p, -1, NULL) == 0);
    LT_CHECK(e.dev.n_burn == 1 && e.dev.burn_pages[0] == 0);
    tenv_close(&e);
}

LT_TEST(write_dirty_sends_ranges_then_pending_then_burned)
{
    tenv e;
    iniopt o = speed_modern();
    static const lt_u8 w[3] = { 0xE1, 0xE2, 0xE3 };
    int r;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_read_all(e.p, e.mem, LT_FALSE) == 0);
    LT_CHECK(lt_ecu_mem_write(e.mem, 0, 10, w, 3) == 0);
    LT_CHECK(lt_ecu_mem_write(e.mem, 0, 100, w, 1) == 0);
    LT_CHECK(lt_ecu_mem_write(e.mem, 2, 60, w, 3) == 0);
    r = lt_proto_write_dirty(e.p, e.mem);
    LT_CHECK(r == 3 + 1 + 3);
    LT_CHECK(e.dev.nwrites == 3);
    LT_CHECK(memcmp(e.dev.page[0] + 10, w, 3) == 0 && e.dev.page[0][100] == 0xE1);
    LT_CHECK(lt_ecu_mem_state(e.mem, 0) == LT_PAGE_PENDING);
    LT_CHECK(!lt_ecu_mem_has_dirty(e.mem) && lt_ecu_mem_has_pending_burn(e.mem));
    LT_CHECK(lt_proto_burn(e.p, -1, e.mem) == 0);
    LT_CHECK(lt_ecu_mem_state(e.mem, 0) == LT_PAGE_CLEAN && !lt_ecu_mem_has_pending_burn(e.mem));
    tenv_close(&e);
}

LT_TEST(realtime_modern_status_stripped)
{
    tenv e;
    iniopt o = speed_modern();
    lt_buf b;
    const char *why = NULL;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);
    LT_CHECK_STR(why, "auto: OCH (modern protocol negotiated)");
    lt_buf_init(&b);
    LT_CHECK(lt_proto_get_realtime(e.p, &b) == 0);
    LT_CHECK(b.len == 16 && (lt_u8)b.data[0] == 0xA0 && (lt_u8)b.data[15] == 0xAF);
    /* command was O off=0 count=16 (BE) */
    LT_CHECK((lt_u8)e.dev.plog[e.dev.nplog - 1].data[0] == 'O');
    LT_CHECK(lt_rd_be((const lt_u8 *)e.dev.plog[e.dev.nplog - 1].data + 3, 2) == 16);
    /* ECU that omits the status byte (rusEFI style) */
    e.dev.och_nostatus = 1;
    lt_buf_clear(&b);
    LT_CHECK(lt_proto_get_realtime(e.p, &b) == 0 && b.len == 16 && (lt_u8)b.data[0] == 0xA0);
    /* wrong size */
    e.dev.och_nostatus = 0;
    e.dev.och_err = 1;
    lt_buf_clear(&b);
    LT_CHECK(lt_proto_get_realtime(e.p, &b) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "OCH response: 14 bytes, expected 16") != NULL);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(realtime_mode_selection)
{
    tenv e;
    iniopt o = speed_modern();
    lt_proto_cfg cfg;
    const char *why = NULL;

    /* Speeduino/MS: burst even with rfcomm port, unless modern+och */
    lt_proto_cfg_default(&cfg);
    cfg.port_name = "/dev/rfcomm0";
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, &cfg);
    /* before connect: legacy flag from INI says modern -> OCH */
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);
    tenv_close(&e);
    o = legacy_opt();
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_BURST);
    LT_CHECK(strstr(why, "Burst") != NULL && strstr(why, "Speeduino") != NULL);
    tenv_close(&e);
    cfg.runtime_mode = LT_RT_FORCE_OCH;
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);
    tenv_close(&e);
    /* rusEFI + rfcomm -> OCH ; rusEFI plain legacy -> burst ; disabled -> burst */
    o = rusefi_modern();
    o.modern = 0;
    lt_proto_cfg_default(&cfg);
    cfg.port_name = "rfcomm0";
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);
    tenv_close(&e);
    lt_proto_cfg_default(&cfg);
    cfg.port_name = "/dev/ttyACM0";
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_BURST);
    LT_CHECK_STR(why, "default: burst");
    tenv_close(&e);
    cfg.baud = 9600;
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);   /* slow link */
    tenv_close(&e);
    cfg.runtime_mode = LT_RT_DISABLED;
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_BURST);
    tenv_close(&e);
    cfg.runtime_mode = LT_RT_FORCE_BURST;     /* modern+och: burst is a no-op, uses OCH */
    o.modern = 1;
    tenv_open(&e, &o, LT_FALSE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_choose_runtime(e.p, &why) == LT_RT_KIND_OCH);
    LT_CHECK(strstr(why, "force") != NULL);
    tenv_close(&e);
}

LT_TEST(realtime_adaptive_timeout_math)
{
    tenv e;
    iniopt o = speed_modern();
    lt_proto_cfg cfg;
    lt_buf b;
    int i;

    lt_proto_cfg_default(&cfg);
    cfg.adaptive_timing = LT_TRUE;
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, &cfg);
    LT_CHECK(lt_proto_effective_timeout_ms(e.p) == 500);   /* no samples: max_timeout */
    e.dev.latency = 0.0;
    LT_CHECK(lt_proto_connect(e.p) == 0);
    e.dev.latency = 190.0;
    lt_buf_init(&b);
    for (i = 0; i < 22; i++) {
        lt_buf_clear(&b);
        LT_CHECK(lt_proto_get_realtime(e.p, &b) == 0);
    }
    /* 190 ms * 2.5 = 475 ms (clamped 10..500) */
    LT_CHECK(lt_proto_effective_timeout_ms(e.p) >= 475 && lt_proto_effective_timeout_ms(e.p) <= 480);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(stalled_payload_costs_one_timeout)
{
    tenv e;
    iniopt o = speed_modern();
    lt_buf b;
    double t0;

    o.timeout = 800;
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    lt_buf_init(&b);
    e.dev.trunc = 40;      /* header + part of the payload, then silence */
    t0 = e.clk.t;
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 100, &b) == LT_ETIMEOUT);
    LT_CHECK(e.clk.t - t0 < 800.0 + 100.0);
    /* ECU stalls at the header */
    e.dev.trunc = 1;
    t0 = e.clk.t;
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 100, &b) == LT_ETIMEOUT);
    LT_CHECK(e.clk.t - t0 < 800.0 + 100.0);
    /* recovers afterwards: stale bytes were flushed */
    e.dev.trunc = 0;
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 100, &b) == 0 && b.len == 100);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(bad_reply_crc_is_reported)
{
    tenv e;
    iniopt o = speed_modern();
    lt_buf b;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    lt_buf_init(&b);
    e.dev.crc_bad_env = 1;
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 8, &b) == LT_ECRC);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_CRC_MISMATCH);
    LT_CHECK(strstr(lt_proto_errinfo(e.p)->msg, "CRC mismatch: expected 0x") != NULL);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(cancel_flag_lifecycle)
{
    tenv e;
    iniopt o = speed_modern();
    lt_buf b;
    volatile int *flag;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    flag = lt_proto_cancel_flag(e.p);
    *flag = 1;                                   /* stale flag */
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(*flag == 0);                        /* connect clears it */
    lt_buf_init(&b);
    lt_proto_cancel(e.p);
    LT_CHECK(lt_proto_read_page(e.p, 0, e.mem) == LT_ECANCEL);
    LT_CHECK(lt_proto_read_page(e.p, 0, e.mem) == 0);   /* consumed by the failed call */
    lt_proto_disconnect(e.p);
    LT_CHECK(*flag != 0);                        /* disconnect leaves it raised */
    LT_CHECK(lt_proto_state(e.p) == LT_CONN_DISCONNECTED);
    LT_CHECK(lt_proto_read_memory(e.p, 0, 0, 4, &b) == LT_ENOTCONN);
    LT_CHECK(lt_proto_connect(e.p) == 0 && *flag == 0);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(timing_vectors)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_proto_cfg cfg;

    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_effective_write_chunk(e.p) == 243);
    LT_CHECK(lt_proto_inter_frame_delay_ms(e.p, 250) >= 30);
    LT_CHECK(lt_proto_inter_frame_delay_ms(e.p, 20) >= 30);
    LT_CHECK(lt_proto_effective_timeout_ms(e.p) == 500);
    tenv_close(&e);
    lt_proto_cfg_default(&cfg);
    cfg.baud = 9600;
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    LT_CHECK(lt_proto_inter_frame_delay_ms(e.p, 250) >= 260);
    tenv_close(&e);
    o.interwrite = 100;
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_inter_frame_delay_ms(e.p, 8) >= 100 && lt_proto_inter_frame_delay_ms(e.p, 8) <= 105);
    tenv_close(&e);
    /* modern with default settings settles faster than the legacy 30 ms floor */
    o = speed_modern();
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_inter_frame_delay_ms(e.p, 20) < 30);
    tenv_close(&e);
}

/* ---- legacy "Mega2560" simulator: 257 byte RX ring, frames M/r ---- */
LT_TEST(legacy_mega_write_bursts_and_verify)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_u8 img[288];
    int i;
    lt_proto_cfg cfg;
    static const char *rd = "\"r%2i%2o%2c\", \"r%2i%2o%2c\", \"r%2i%2o%2c\"";
    static const char *wr = "\"M%2i%2o%2c%v\", \"M%2i%2o%2c%v\", \"M%2i%2o%2c%v\"";

    o.rd = rd;
    o.wr = wr;
    o.timeout = 100;
    lt_proto_cfg_default(&cfg);
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, &cfg);
    e.dev.ring_cap = 257;
    for (i = 0; i < 288; i++) {
        img[i] = (lt_u8)(i * 3 + 7);
    }
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_write_page(e.p, 0, img, 288) == 0);   /* CRC unavailable: only a warning */
    LT_CHECK(e.dev.nwrites == 2 && e.dev.write_len[0] == 250 && e.dev.write_len[1] == 52);
    LT_CHECK(e.dev.dropped == 0);
    LT_CHECK(memcmp(e.dev.page[0], img, 288) == 0);
    LT_CHECK(e.dev.write_t[1] - e.dev.write_t[0] >= 30.0);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_NONE);
    /* honest verified write passes */
    LT_CHECK(lt_proto_write_memory_verified(e.p, 1, 90, img, 20) == 0);
    /* a flipped byte at page 1 offset 100 is detected */
    e.dev.corrupt_page = 1;
    e.dev.corrupt_off = 100;
    LT_CHECK(lt_proto_write_memory_verified(e.p, 1, 90, img, 20) == LT_EPROTO);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_WRITE_VERIFY_FAILED);
    LT_CHECK(lt_proto_errinfo(e.p)->page == 1 && lt_proto_errinfo(e.p)->offset == 100);
    LT_CHECK(lt_proto_errinfo(e.p)->expected == img[10]);
    LT_CHECK(lt_proto_errinfo(e.p)->actual == (lt_u8)~img[10]);
    tenv_close(&e);
}

LT_TEST(legacy_ring_overflows_without_pacing)
{
    /* sanity check of the simulator itself: back-to-back 250 byte frames drop bytes */
    fclk clk;
    fdev d;
    lt_u8 frame[250];

    clk.t = 0;
    fd_init(&d, &clk);
    d.legacy = 1;
    d.ring_cap = 257;
    memset(frame, 0, sizeof(frame));
    frame[0] = 'M';
    lt_wr_be(frame + 5, 2, 243);
    fd_write(&d, frame, 250);
    fd_write(&d, frame, 250);
    LT_CHECK(d.dropped > 0);
    fd_free(&d);
}

LT_TEST(legacy_read_and_realtime)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_buf b;
    int i, ok = 1;

    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_read_page(e.p, 0, e.mem) == 0);
    for (i = 0; i < 288; i++) {
        if (lt_ecu_mem_read(e.mem, 0, 0, 288)[i] != e.dev.page[0][i]) {
            ok = 0;
        }
    }
    LT_CHECK(ok);
    lt_buf_init(&b);
    /* Speeduino legacy: burst 'A', raw block returned as is */
    LT_CHECK(lt_proto_get_realtime(e.p, &b) == 0);
    LT_CHECK(b.len == 16 && (lt_u8)b.data[0] == 0xB0);
    /* short legacy read reports wrong-length */
    e.dev.short_read = 1;
    lt_buf_clear(&b);
    LT_CHECK(lt_proto_read_memory(e.p, 1, 0, 10, &b) == LT_EPROTO);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(legacy_burn_does_not_wait_for_reply)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_u8 d[2] = { 1, 2 };
    double t0;

    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_write_memory(e.p, 0, 0, d, 2) == 0);
    t0 = e.clk.t;
    LT_CHECK(lt_proto_burn(e.p, 0, NULL) == 0);
    LT_CHECK(e.clk.t - t0 >= 2600.0 && e.clk.t - t0 < 2700.0);
    LT_CHECK(e.dev.n_burn == 1);
    tenv_close(&e);
}

/* ---- calibration ---- */
LT_TEST(calibration_modern_frames)
{
    tenv e;
    iniopt o = speed_modern();
    lt_u8 w[1024];
    lt_u8 tw[64];
    int i, base;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    for (i = 0; i < 1024; i++) {
        w[i] = (lt_u8)(i * 5 + 1);
    }
    for (i = 0; i < 64; i++) {
        tw[i] = (lt_u8)(i + 100);
    }
    base = e.dev.nplog;
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_O2, w, 1024) == 0);
    LT_CHECK(e.dev.nplog == base + 5);   /* 4 chunks + 'k' */
    {
        static const lt_u8 h0[7] = { 't', 0, 2, 0, 0, 1, 0 };
        static const lt_u8 h1[7] = { 't', 0, 2, 1, 0, 1, 0 };
        static const lt_u8 h2[7] = { 't', 0, 2, 2, 0, 1, 0 };
        static const lt_u8 h3[7] = { 't', 0, 2, 3, 0, 1, 0 };
        static const lt_u8 *const hs[4] = { h0, h1, h2, h3 };

        for (i = 0; i < 4; i++) {
            LT_CHECK(e.dev.plog[base + i].len == 7 + 256);
            LT_CHECK(memcmp(e.dev.plog[base + i].data, hs[i], 7) == 0);
        }
    }
    LT_CHECK(memcmp(e.dev.cal[2], w, 1024) == 0);
    LT_CHECK(e.dev.plog[base + 4].len == 3 && e.dev.plog[base + 4].data[0] == 'k');
    base = e.dev.nplog;
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_CLT, tw, 64) == 0);
    {
        static const lt_u8 th[7] = { 't', 0, 0, 0, 0, 0, 0x40 };

        LT_CHECK(memcmp(e.dev.plog[base].data, th, 7) == 0 && e.dev.plog[base].len == 7 + 64);
    }
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_IAT, tw, 64) == 0);
    LT_CHECK(e.dev.plog[e.dev.nplog - 2].data[2] == 1);
    /* wrong length */
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_O2, tw, 64) == LT_EPROTO);
    /* verify failure and chunk rejection */
    e.dev.cal_bad = 1;
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_CLT, tw, 64) == LT_ECRC);
    LT_CHECK(strstr(lt_err_get(), "calibration verify failed for CLT: ECU stored CRC") != NULL);
    e.dev.cal_bad = 0;
    e.dev.cal_reject = 1;
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_CLT, tw, 64) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "calibration chunk at offset 0 rejected: 0x84") != NULL);
    {
        lt_u32 crc = 0;

        e.dev.cal_reject = 0;
        LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_CLT, tw, 64) == 0);
        LT_CHECK(lt_proto_read_calibration_crc(e.p, LT_CAL_CLT, &crc) == 0);
        LT_CHECK(crc == lt_proto_crc32(tw, 64));
    }
    tenv_close(&e);
}

LT_TEST(calibration_legacy_stream_and_ecu_check)
{
    tenv e;
    iniopt o = legacy_opt();
    lt_u8 w[1024];
    int i;
    double t0;
    lt_u32 crc;

    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    for (i = 0; i < 1024; i++) {
        w[i] = (lt_u8)(i ^ 0x5A);
    }
    t0 = e.clk.t;
    LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_O2, w, 1024) == 0);
    LT_CHECK(e.dev.plog[e.dev.nplog - 1].len == 2 && e.dev.plog[e.dev.nplog - 1].data[0] == 't');
    LT_CHECK(e.dev.plog[e.dev.nplog - 1].data[1] == 2);
    LT_CHECK(e.dev.cal_len[2] == 1024 && memcmp(e.dev.cal[2], w, 1024) == 0);
    LT_CHECK(e.clk.t - t0 >= 500.0);
    LT_CHECK(lt_proto_read_calibration_crc(e.p, 0, &crc) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "requires the CRC protocol") != NULL);
    tenv_close(&e);
    {
        iniopt r = rusefi_modern();

        tenv_open(&e, &r, LT_FALSE, LT_ENV_BIG, NULL);
        LT_CHECK(lt_proto_connect(e.p) == 0);
        LT_CHECK(lt_proto_write_calibration(e.p, LT_CAL_CLT, w, 64) == LT_EPROTO);
        LT_CHECK(strstr(lt_err_get(),
                        "only implemented for Speeduino (connected ECU type: rusEFI)") != NULL);
        tenv_close(&e);
    }
}

/* ---- console, controller commands, raw ---- */
LT_TEST(console_modern_rusefi)
{
    tenv e;
    iniopt o = rusefi_modern();
    char *out = NULL;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_console(e.p, "hello", 1000, &out) == 0);
    LT_CHECK(out != NULL);
    LT_CHECK_STR(out, "ran: hello");
    lt_free(out);
    tenv_close(&e);
    o = speed_modern();
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    LT_CHECK(lt_proto_console(e.p, "hello", 1000, &out) == LT_EUNSUPPORTED && out == NULL);
    tenv_close(&e);
}

static int cancel_second(void *ud, const char *phase, int page, size_t done, size_t total)
{
    int *n = (int *)ud;

    LT_UNUSED(phase);
    LT_UNUSED(page);
    LT_UNUSED(done);
    LT_UNUSED(total);
    (*n)++;
    return *n >= 2 ? 1 : 0;
}

/* Review fixes: verify-unavailable keeps its reason, a cancelled page write
 * blocks burn, raw/verify/calibration-crc need a connection. */
LT_TEST(review_regressions)
{
    tenv e;
    iniopt o = legacy_opt();
    iniopt m = speed_modern();
    lt_buf b;
    lt_u8 img[288];
    lt_u32 crc;
    int i, calls = 0;

    for (i = 0; i < 288; i++) {
        img[i] = (lt_u8)i;
    }
    /* legacy: short read during verification -> UNAVAILABLE with the reason text */
    tenv_open(&e, &o, LT_TRUE, LT_ENV_BIG, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    e.dev.short_read = 1;
    LT_CHECK(lt_proto_write_memory_verified(e.p, 1, 0, img, 20) == LT_EPROTO);
    LT_CHECK(lt_proto_errinfo(e.p)->code == LT_PE_VERIFY_UNAVAILABLE);
    LT_CHECK(strstr(lt_proto_errinfo(e.p)->msg, "wrong-length") != NULL);
    tenv_close(&e);
    /* cancel between chunks of a page write leaves the page failed: burn refuses */
    tenv_open(&e, &m, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    lt_proto_set_progress(e.p, cancel_second, &calls);
    LT_CHECK(lt_proto_write_page(e.p, 0, img, 288) == LT_ECANCEL);
    LT_CHECK(lt_proto_burn(e.p, 0, NULL) == LT_EBUSY);
    tenv_close(&e);
    /* not connected: no traffic, LT_ENOTCONN */
    tenv_open(&e, &m, LT_FALSE, LT_ENV_LITTLE, NULL);
    lt_buf_init(&b);
    LT_CHECK(lt_proto_send_raw_response(e.p, img, 2, 100, &b) == LT_ENOTCONN);
    LT_CHECK(lt_proto_verify_page_crc(e.p, 0, img, 10) == LT_ENOTCONN);
    LT_CHECK(lt_proto_read_calibration_crc(e.p, 0, &crc) == LT_ENOTCONN);
    LT_CHECK(e.dev.nplog == 0);
    lt_buf_free(&b);
    tenv_close(&e);
}

LT_TEST(run_command_and_send_raw)
{
    tenv e;
    iniopt o = speed_modern();
    const lt_ini_cmd *c;
    static const lt_u8 raw[3] = { 'Z', 9, 9 };
    int base;

    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, NULL);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    base = e.dev.nplog;
    c = (const lt_ini_cmd *)lt_map_get(&e.ini->command_idx, "cmdZ");
    LT_CHECK(c != NULL);
    LT_CHECK(lt_proto_run_command(e.p, c) == 0);
    LT_CHECK(e.dev.nplog == base + 1);
    LT_CHECK(e.dev.plog[base].len == 3 && (lt_u8)e.dev.plog[base].data[0] == 'Z' &&
             e.dev.plog[base].data[1] == 0 && e.dev.plog[base].data[2] == 0x12);
    c = (const lt_ini_cmd *)lt_map_get(&e.ini->command_idx, "cmdBoth");
    LT_CHECK(c != NULL && lt_proto_run_command(e.p, c) == 0);
    LT_CHECK(e.dev.nplog == base + 3);
    LT_CHECK(e.dev.plog[base + 1].data[1] == 2 && e.dev.plog[base + 2].data[1] == 3);
    LT_CHECK(lt_proto_send_raw(e.p, raw, 3) == 0 && e.dev.nplog == base + 4);
    LT_CHECK(lt_proto_send_raw(e.p, raw, 0) == 0 && e.dev.nplog == base + 4);
    tenv_close(&e);
}

LT_TEST(permissive_crc_config)
{
    tenv e;
    iniopt o = speed_modern();
    lt_proto_cfg cfg;

    lt_proto_cfg_default(&cfg);
    cfg.permissive_crc = LT_TRUE;
    tenv_open(&e, &o, LT_FALSE, LT_ENV_LITTLE, &cfg);
    LT_CHECK(lt_proto_connect(e.p) == 0);
    tenv_close(&e);
}

/* ------------------------------------------------------------------ */
/* discovery                                                            */
/* ------------------------------------------------------------------ */
typedef struct dport {
    const char *name;
    int fail_open;
    const lt_u8 *reply_q, *reply_s;
    size_t nq, ns;
    int echo;
} dport;

typedef struct dsim {
    fclk clk;
    dport *ports;
    size_t nports;
    /* per-open state */
    dport *cur;
    lt_buf rx;
    size_t rxpos;
    int opens;
    long last_baud;
    int closes;
} dsim;

static int ds_read(void *ud, void *buf, int n, int to)
{
    dsim *s = (dsim *)ud;
    size_t av = s->rx.len - s->rxpos, k;

    if (av == 0) {
        s->clk.t += to;
        return 0;
    }
    k = av < (size_t)n ? av : (size_t)n;
    memcpy(buf, s->rx.data + s->rxpos, k);
    s->rxpos += k;
    return (int)k;
}

static int ds_write(void *ud, const void *buf, int n)
{
    dsim *s = (dsim *)ud;
    lt_u8 q = *(const lt_u8 *)buf;

    if (s->cur->echo) {
        lt_u8 e4[4];

        memset(e4, q, 4);
        lt_buf_append(&s->rx, e4, 4);
    } else if (q == 'Q' && s->cur->nq) {
        lt_buf_append(&s->rx, s->cur->reply_q, s->cur->nq);
    } else if (q == 'S' && s->cur->ns) {
        lt_buf_append(&s->rx, s->cur->reply_s, s->cur->ns);
    }
    return n;
}

static void ds_clear(void *ud)
{
    dsim *s = (dsim *)ud;

    lt_buf_clear(&s->rx);
    s->rxpos = 0;
}

static void ds_close(void *ud)
{
    ((dsim *)ud)->closes++;
}

static int ds_open(void *ud, const char *port, long baud, lt_proto_io *io)
{
    dsim *s = (dsim *)ud;
    size_t i;

    s->opens++;
    s->last_baud = baud;
    for (i = 0; i < s->nports; i++) {
        if (strcmp(s->ports[i].name, port) == 0) {
            if (s->ports[i].fail_open) {
                lt_err_set("permission denied");
                return LT_EIO;
            }
            s->cur = &s->ports[i];
            lt_buf_clear(&s->rx);
            s->rxpos = 0;
            io->ud = s;
            io->read = ds_read;
            io->write = ds_write;
            io->clear_input = ds_clear;
            io->close = ds_close;
            return 0;
        }
    }
    return LT_ENOENT;
}

static double ds_now(void *ud)
{
    return ((dsim *)ud)->clk.t;
}

static void ds_sleep(void *ud, int ms)
{
    ((dsim *)ud)->clk.t += ms;
}

static void disc_cfg(dsim *s, lt_disc_cfg *c, const long *bauds, size_t nb)
{
    lt_disc_cfg_default(c);
    c->bauds = bauds;
    c->n_bauds = nb;
    c->post_open_settle_ms = 0;
    c->pre_write_delay_ms = 0;
    c->read_window_ms = 20;
    c->open = ds_open;
    c->open_ud = s;
    c->clock.ud = s;
    c->clock.now_ms = ds_now;
    c->clock.sleep_ms = ds_sleep;
}

static size_t count_kind(const lt_disc_result *r, size_t n, lt_disc_kind k)
{
    size_t i, c = 0;

    for (i = 0; i < n; i++) {
        if (r[i].kind == k) {
            c++;
        }
    }
    return c;
}

LT_TEST(discovery_cases)
{
    static const long bauds[1] = { 115200L };
    static const long bauds3[3] = { 115200L, 9600L, 57600L };
    static const lt_u8 sig[] = "Speeduino 2024.10  ";
    static const lt_u8 boot[] = { 0xE0, 0x05, 0x3E };
    static const lt_u8 bs3[] = { 0x01, 0xAA, 0xBB, 0xCC };
    static const lt_u8 gps[] = "$GPGGA,123519,4807.038";
    dport ports[8];
    dsim s;
    lt_disc_cfg c;
    lt_disc_result *r = NULL;
    size_t n = 0;
    const lt_disc_result *best;
    const char *names[8];

    memset(&s, 0, sizeof(s));
    lt_buf_init(&s.rx);
    memset(ports, 0, sizeof(ports));
    s.ports = ports;
    s.nports = 8;
    ports[0].name = "sig";
    ports[0].reply_s = sig;
    ports[0].ns = 19;
    ports[1].name = "boot";
    ports[1].reply_q = boot;
    ports[1].nq = 3;
    ports[2].name = "bs3";
    ports[2].reply_q = bs3;
    ports[2].nq = 4;
    ports[3].name = "gps";
    ports[3].reply_q = gps;
    ports[3].nq = 22;
    ports[4].name = "echo";
    ports[4].echo = 1;
    ports[5].name = "silent";
    ports[6].name = "bad";
    ports[6].fail_open = 1;
    ports[7].name = "missing";
    disc_cfg(&s, &c, bauds, 1);

    names[0] = "sig";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_SIGNATURE && r[0].query == 'S');
    LT_CHECK(strncmp(r[0].signature, "Speeduino", 9) == 0 && r[0].baud == 115200L);
    LT_CHECK_STR(r[0].signature, "Speeduino 2024.10");
    best = lt_disc_best(r, n);
    LT_CHECK(best == &r[0]);
    lt_free(r);

    names[0] = "boot";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_BOOTLOADER);
    lt_free(r);
    names[0] = "bs3";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_BIGSTUFF3);
    lt_free(r);
    names[0] = "gps";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_GPS && lt_disc_best(r, n) == NULL);
    lt_free(r);
    names[0] = "echo";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 2 && count_kind(r, n, LT_DISC_ECHO) == 2 && lt_disc_best(r, n) == NULL);
    lt_free(r);
    names[0] = "silent";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_SILENT);
    lt_free(r);
    names[0] = "bad";
    LT_CHECK(lt_disc_run(&c, names, 1, &r, &n) == 0);
    LT_CHECK(n == 1 && r[0].kind == LT_DISC_OPEN_FAILED);
    LT_CHECK(strstr(r[0].error, "permission denied") != NULL);
    lt_free(r);

    /* sweep: silent port tries every baud; a signature stops the sweep for that port */
    s.opens = 0;
    s.closes = 0;
    disc_cfg(&s, &c, bauds3, 3);
    names[0] = "silent";
    names[1] = "sig";
    LT_CHECK(lt_disc_run(&c, names, 2, &r, &n) == 0);
    LT_CHECK(s.opens == 4);                    /* 3 silent bauds + first baud of sig */
    LT_CHECK(s.closes == 4);
    LT_CHECK(n == 4 && count_kind(r, n, LT_DISC_SILENT) == 3 && r[3].kind == LT_DISC_SIGNATURE);
    LT_CHECK(r[1].baud == 9600L && r[2].baud == 57600L);
    best = lt_disc_best(r, n);
    LT_CHECK(best == &r[3]);
    lt_free(r);
    /* best(): signature beats bootloader beats bigstuff3 irrespective of order */
    names[0] = "bs3";
    names[1] = "boot";
    names[2] = "sig";
    LT_CHECK(lt_disc_run(&c, names, 3, &r, &n) == 0);
    LT_CHECK(n == 3);
    LT_CHECK(lt_disc_best(r, n)->kind == LT_DISC_SIGNATURE);
    LT_CHECK(lt_disc_best(r, 2)->kind == LT_DISC_BOOTLOADER);
    lt_free(r);
    /* cancel */
    {
        volatile int cancel = 1;

        c.cancel = &cancel;
        LT_CHECK(lt_disc_run(&c, names, 3, &r, &n) == LT_ECANCEL);
        lt_free(r);
    }
    lt_buf_free(&s.rx);
}

LT_TEST(io_from_stream_adapter_shape)
{
    lt_proto_io io = lt_proto_io_from_stream(NULL, LT_FALSE);

    LT_CHECK(io.read != NULL && io.write != NULL && io.clear_input != NULL);
    LT_CHECK(io.close == NULL);
    io = lt_proto_io_from_stream(NULL, LT_TRUE);
    LT_CHECK(io.close != NULL);
}

LT_TEST(real_demo_ini_command_encoding)
{
    fclk clk;
    fdev d;
    lt_ini *ini = lt_ini_load_file("tests/fixtures/ini/demo.ini", NULL);
    lt_proto_io io;
    lt_proto *p;
    lt_proto_clock ck;
    lt_buf b;
    lt_ecu_mem *m;
    static const lt_u8 want[7] = { 'R', 0x00, 0x01, 0x00, 0x00, 0x10, 0x00 };

    if (!ini) {
        LT_CHECK(0);
        return;
    }
    clk.t = 0;
    fd_init(&d, &clk);
    d.order = LT_ENV_BIG;
    lt_strlcpy(d.sig, ini->signature, sizeof(d.sig));
    memset(&io, 0, sizeof(io));
    io.ud = &d;
    io.read = fd_read;
    io.write = fd_write;
    io.clear_input = fd_clear;
    p = lt_proto_new(&io, ini, NULL);
    ck.ud = &clk;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    LT_CHECK(lt_proto_envelope_order(p) == LT_ENV_BIG && lt_proto_is_modern(p));
    LT_CHECK(!lt_proto_signature_mismatch(p));
    /* INI: little endian params, pageIdentifier "\x00\x01" for page 1 -> id 0x0100 -> 00 01 */
    d.stall = 1;
    lt_buf_init(&b);
    LT_CHECK(lt_proto_read_memory(p, 1, 0, 16, &b) == LT_ETIMEOUT);
    LT_CHECK(d.nplog >= 2 && d.plog[d.nplog - 1].len == 7);
    LT_CHECK(memcmp(d.plog[d.nplog - 1].data, want, 7) == 0);
    LT_CHECK(lt_proto_effective_write_chunk(p) == 2552);
    m = lt_ecu_mem_from_ini(ini);
    LT_CHECK(lt_ecu_mem_page_count(m) == 3 && lt_ecu_mem_page_size(m, 0) == 40560);
    lt_ecu_mem_free(m);
    lt_buf_free(&b);
    lt_proto_free(p);
    fd_free(&d);
    lt_ini_free(ini);
}

/* MegaSquirt: a big-endian INI whose page identifiers are "\$tsCanId\x04" ... goes out as
 * written, CAN id first (r 00 04 ..), in reads, burns and CRC checks */
LT_TEST(big_endian_page_identifier_sent_as_written)
{
    static const char txt[] =
        "[MegaTune]\n signature = \"MS3 Format 0238.003\"\n"
        "[TunerStudio]\n queryCommand = \"Q\"\n"
        "[PcVariables]\n tsCanId = bits, U08, [0:3], \"CAN ID 0\", \"CAN ID 1\"\n"
        "[Constants]\n endianness = big\n messageEnvelopeFormat = msEnvelope_1.0\n nPages = 2\n"
        " pageSize = 64, 64\n pageIdentifier = \"\\$tsCanId\\x04\", \"\\$tsCanId\\x0a\"\n"
        " pageReadCommand = \"r%2i%2o%2c\", \"r%2i%2o%2c\"\n burnCommand = \"b%2i\", \"b%2i\"\n"
        " pageValueWrite = \"w%2i%2o%2c%v\", \"w%2i%2o%2c%v\"\n"
        " crc32CheckCommand = \"k%2i\\x00\\x00\\x00\\x04\", \"k%2i\\x00\\x00\\x00\\x04\"\n";
    static const lt_u8 want_rd[7] = { 'r', 0x00, 0x0a, 0x00, 0x10, 0x00, 0x08 };
    static const lt_u8 want_b[3] = { 'b', 0x00, 0x0a };
    static const lt_u8 want_k[7] = { 'k', 0x00, 0x04, 0x00, 0x00, 0x00, 0x04 };
    static const lt_u8 zero[64] = { 0 };
    fclk clk;
    fdev d;
    lt_ini *ini = lt_ini_load_string(txt, strlen(txt), NULL, NULL);
    lt_proto_io io;
    lt_proto *p;
    lt_proto_clock ck;
    lt_buf b;

    if (!ini) {
        LT_CHECK(0);
        return;
    }
    clk.t = 0;
    fd_init(&d, &clk);
    d.order = LT_ENV_BIG;
    lt_strlcpy(d.sig, ini->signature, sizeof(d.sig));
    memset(&io, 0, sizeof(io));
    io.ud = &d;
    io.read = fd_read;
    io.write = fd_write;
    io.clear_input = fd_clear;
    p = lt_proto_new(&io, ini, NULL);
    ck.ud = &clk;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    d.stall = 1;
    lt_buf_init(&b);
    LT_CHECK(lt_proto_read_memory(p, 1, 16, 8, &b) == LT_ETIMEOUT);
    LT_CHECK(d.nplog >= 2 && d.plog[d.nplog - 1].len == 7 &&
             memcmp(d.plog[d.nplog - 1].data, want_rd, 7) == 0);
    LT_CHECK(lt_proto_burn(p, 1, NULL) == 0);
    LT_CHECK(d.plog[d.nplog - 1].len == 3 && memcmp(d.plog[d.nplog - 1].data, want_b, 3) == 0);
    LT_CHECK(lt_proto_verify_page_crc(p, 0, zero, sizeof(zero)) != 0);
    LT_CHECK(d.plog[d.nplog - 1].len == 7 && memcmp(d.plog[d.nplog - 1].data, want_k, 7) == 0);
    lt_buf_free(&b);
    lt_proto_free(p);
    fd_free(&d);
    lt_ini_free(ini);
}

/* a fixed ochGetCommand with escapes (MegaSquirt over CAN) goes out decoded */
LT_TEST(och_command_escapes_decoded)
{
    static const char txt[] =
        "[MegaTune]\n signature = \"MS3 Format 0238.003\"\n"
        "[TunerStudio]\n queryCommand = \"Q\"\n"
        "[Constants]\n endianness = big\n messageEnvelopeFormat = msEnvelope_1.0\n nPages = 1\n"
        " pageSize = 64\n pageReadCommand = \"r%2i%2o%2c\"\n"
        "[OutputChannels]\n ochGetCommand = \"r\\x00\\x07\\x00\\x00\\x00\\x10\"\n ochBlockSize = 16\n";
    static const lt_u8 want[7] = { 'r', 0x00, 0x07, 0x00, 0x00, 0x00, 0x10 };
    fclk clk;
    fdev d;
    lt_ini *ini = lt_ini_load_string(txt, strlen(txt), NULL, NULL);
    lt_proto_io io;
    lt_proto *p;
    lt_proto_clock ck;
    lt_buf b;

    if (!ini) {
        LT_CHECK(0);
        return;
    }
    clk.t = 0;
    fd_init(&d, &clk);
    d.order = LT_ENV_BIG;
    lt_strlcpy(d.sig, ini->signature, sizeof(d.sig));
    memset(&io, 0, sizeof(io));
    io.ud = &d;
    io.read = fd_read;
    io.write = fd_write;
    io.clear_input = fd_clear;
    p = lt_proto_new(&io, ini, NULL);
    ck.ud = &clk;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0);
    LT_CHECK(lt_proto_choose_runtime(p, NULL) == LT_RT_KIND_OCH);
    d.stall = 1;
    lt_buf_init(&b);
    LT_CHECK(lt_proto_get_realtime(p, &b) != 0);
    LT_CHECK(d.nplog >= 2 && d.plog[d.nplog - 1].len == 7 &&
             memcmp(d.plog[d.nplog - 1].data, want, 7) == 0);
    lt_buf_free(&b);
    lt_proto_free(p);
    fd_free(&d);
    lt_ini_free(ini);
}

/* a legacy device that answers the query and records the last command */
typedef struct recdev {
    fclk *clk;
    lt_u8 last[32];
    size_t nlast;
    lt_buf rx;
} recdev;

static int rec_read(void *ud, void *buf, int n, int timeout_ms)
{
    recdev *d = (recdev *)ud;
    size_t k = d->rx.len < (size_t)n ? d->rx.len : (size_t)n;

    if (k == 0) {
        d->clk->t += timeout_ms > 0 ? timeout_ms : 1;
        return 0;
    }
    memcpy(buf, d->rx.data, k);
    memmove(d->rx.data, d->rx.data + k, d->rx.len - k);
    d->rx.len -= k;
    return (int)k;
}

static int rec_write(void *ud, const void *buf, int n)
{
    recdev *d = (recdev *)ud;

    d->nlast = (size_t)n < sizeof(d->last) ? (size_t)n : sizeof(d->last);
    memcpy(d->last, buf, d->nlast);
    if (n == 1 && ((const lt_u8 *)buf)[0] == 'Q') {
        lt_buf_puts(&d->rx, "MSII Rev 2.88000");
    }
    return n;
}

/* MegaSquirt-II 2.x: the legacy burst command "a\x00\x06" goes out whole */
LT_TEST(legacy_burst_command_sent_whole)
{
    static const char txt[] =
        "[MegaTune]\n signature = \"MSII Rev 2.88000\"\n queryCommand = \"Q\"\n"
        "[Constants]\n endianness = big\n nPages = 1\n pageSize = 64\n"
        " pageReadCommand = \"r%2i%2o%2c\"\n"
        "[BurstMode]\n getCommand = \"a\\x00\\x06\"\n"
        "[OutputChannels]\n ochGetCommand = \"a\\x00\\x06\"\n ochBlockSize = 16\n";
    static const lt_u8 want[3] = { 'a', 0x00, 0x06 };
    fclk clk;
    recdev d;
    lt_ini *ini = lt_ini_load_string(txt, strlen(txt), NULL, NULL);
    lt_proto_io io;
    lt_proto *p;
    lt_proto_clock ck;
    lt_buf b;

    if (!ini) {
        LT_CHECK(0);
        return;
    }
    clk.t = 0;
    memset(&d, 0, sizeof(d));
    d.clk = &clk;
    lt_buf_init(&d.rx);
    memset(&io, 0, sizeof(io));
    io.ud = &d;
    io.read = rec_read;
    io.write = rec_write;
    p = lt_proto_new(&io, ini, NULL);
    ck.ud = &clk;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_set_clock(p, &ck);
    LT_CHECK(lt_proto_connect(p) == 0 && !lt_proto_is_modern(p));
    LT_CHECK(lt_proto_choose_runtime(p, NULL) == LT_RT_KIND_BURST);
    lt_buf_init(&b);
    LT_CHECK(lt_proto_get_realtime(p, &b) != 0);
    LT_CHECK(d.nlast == 3 && memcmp(d.last, want, 3) == 0);
    lt_buf_free(&b);
    lt_buf_free(&d.rx);
    lt_proto_free(p);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(connect_modern_speeduino_le);
    LT_RUN(connect_flips_envelope_order);
    LT_RUN(connect_legacy_and_no_ini);
    LT_RUN(connect_silent_ecu_times_out);
    LT_RUN(signature_mismatch_flag);
    LT_RUN(read_page_modern_chunks);
    LT_RUN(read_all_progress_and_cancel);
    LT_RUN(read_errors);
    LT_RUN(write_page_modern_chunks_and_verify);
    LT_RUN(write_page_crc_mismatch_blocks_burn);
    LT_RUN(write_status_errors);
    LT_RUN(write_page_retries_transient_timeouts);
    LT_RUN(burn_timing_and_dirty_tracking);
    LT_RUN(write_dirty_sends_ranges_then_pending_then_burned);
    LT_RUN(realtime_modern_status_stripped);
    LT_RUN(realtime_mode_selection);
    LT_RUN(realtime_adaptive_timeout_math);
    LT_RUN(stalled_payload_costs_one_timeout);
    LT_RUN(bad_reply_crc_is_reported);
    LT_RUN(cancel_flag_lifecycle);
    LT_RUN(timing_vectors);
    LT_RUN(legacy_mega_write_bursts_and_verify);
    LT_RUN(legacy_ring_overflows_without_pacing);
    LT_RUN(legacy_read_and_realtime);
    LT_RUN(legacy_burn_does_not_wait_for_reply);
    LT_RUN(calibration_modern_frames);
    LT_RUN(calibration_legacy_stream_and_ecu_check);
    LT_RUN(console_modern_rusefi);
    LT_RUN(review_regressions);
    LT_RUN(run_command_and_send_raw);
    LT_RUN(permissive_crc_config);
    LT_RUN(real_demo_ini_command_encoding);
    LT_RUN(big_endian_page_identifier_sent_as_written);
    LT_RUN(och_command_escapes_decoded);
    LT_RUN(legacy_burst_command_sent_whole);
    LT_RUN(discovery_cases);
    LT_RUN(io_from_stream_adapter_shape);
    LT_TEST_MAIN_END
}
