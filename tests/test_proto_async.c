/* test_proto_async.c - asynchronous realtime polling and RAM writes over a modelled slow link
 * (lt_simlink in front of the virtual ECU): partial reads, split packets, lost and cut-off
 * replies, noise, a dropped link; frames must equal the blocking path's, blocking calls made
 * mid-flight must never mix up the byte stream, and lt_app_service must never wait.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt.h"
#include "sim/sim.h"
#include "proto/proto.h"
#include "app/app.h"
#include "lt_test.h"

#include <string.h>

#define SPD_INI "tests/fixtures/ini/speeduino_202501.ini"
#define RUS_INI "tests/fixtures/sim/demo.ini"

/* modern (msEnvelope, little endian) with a small blocking factor: writes span chunks */
static const char *MODERN_INI =
    "[MegaTune]\n signature = \"rusEFI test.1\"\n queryCommand = \"S\"\n versionInfo = \"V\"\n"
    "[Constants]\n messageEnvelopeFormat = msEnvelope_1.0\n endianness = little\n nPages = 2\n"
    " pageSize = 64, 32\n blockingFactor = 24\n blockReadTimeout = 300\n"
    " pageReadCommand = \"R%2i%2o%2c\", \"R%2i%2o%2c\"\n"
    "page = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "[OutputChannels]\n ochBlockSize = 16\n"
    " rpm = scalar, U16, 0, \"rpm\", 1, 0\n"
    " map = scalar, U08, 2, \"kPa\", 1, 0\n";

/* legacy (raw, big endian): burst 'A' with a known block size, silent writes */
static const char *LEGACY_INI =
    "[MegaTune]\n signature = \"speeduino 202501-sim\"\n queryCommand = \"Q\"\n versionInfo = \"S\"\n"
    "[Constants]\n endianness = big\n nPages = 2\n pageSize = 64, 32\n blockingFactor = 24\n"
    " blockReadTimeout = 300\n"
    " pageReadCommand = \"p%2i%2o%2c\", \"p%2i%2o%2c\"\n"
    "page = 1\n a = scalar, U08, 0, \"\", 1, 0, 0, 255, 0\n"
    "[OutputChannels]\n ochBlockSize = 16\n"
    " rpm = scalar, U16, 0, \"rpm\", 1, 0\n";

/* ------------------------------------------------------------------ */
/* rig: fake clock shared by protocol, link and ECU                     */
/* ------------------------------------------------------------------ */
typedef struct rig {
    double t;
    long sleeps;
    lt_ini *ini;
    lt_ecusim *sim;
    lt_simlink *link;
    lt_proto *p;
    lt_ecu_mem *mem;
} rig;

static double r_now(void *ud)
{
    return ((rig *)ud)->t;
}

static void r_sleep(void *ud, int ms)
{
    ((rig *)ud)->t += (double)ms;
    ((rig *)ud)->sleeps++;
}

static int rig_open(rig *r, const char *text, const char *file, long baud)
{
    lt_proto_io io;
    lt_proto_cfg cfg;
    lt_proto_clock ck;
    lt_simlink_cfg lc;

    memset(r, 0, sizeof(*r));
    r->t = 10000.0;
    r->ini = file ? lt_ini_load_file(file, NULL) : lt_ini_load_string(text, strlen(text), NULL,
                                                                     NULL);
    if (!r->ini) {
        return -1;
    }
    r->sim = lt_ecusim_new(r->ini);
    lt_ecusim_set_clock(r->sim, r_now, r);
    lt_ecusim_tick_engine(r->sim, 3000);      /* manual ticks: a frame changes only on a tick */
    io = lt_ecusim_io(r->sim);
    lc.baud = baud;
    lc.latency_ms = 4.0;
    lc.chunk = 64;
    r->link = lt_simlink_new(&io, &lc);
    lt_simlink_set_clock(r->link, r_now, r_sleep, r);
    io = lt_simlink_io(r->link);
    lt_proto_cfg_default(&cfg);
    cfg.baud = baud;
    r->p = lt_proto_new(&io, r->ini, &cfg);
    ck.ud = r;
    ck.now_ms = r_now;
    ck.sleep_ms = r_sleep;
    lt_proto_set_clock(r->p, &ck);
    if (lt_proto_connect(r->p) != LT_OK) {
        return -1;
    }
    r->mem = lt_ecu_mem_from_ini(r->ini);
    return lt_proto_read_all(r->p, r->mem, LT_TRUE);
}

static void rig_close(rig *r)
{
    if (r->p) {
        lt_proto_free(r->p);
    }
    lt_ecu_mem_free(r->mem);
    lt_simlink_free(r->link);
    lt_ecusim_free(r->sim);
    lt_ini_free(r->ini);
}

/* One asynchronous realtime request, pumped like a UI loop does (0.25 ms of other work between
 * pumps). *pumps = pump calls; *slept = clock sleeps made inside pumps (must stay 0). */
static int rt_async(rig *r, lt_buf *out, int *pumps, long *slept)
{
    int rc, n = 0;
    long s0;

    *pumps = 0;
    *slept = 0;
    rc = lt_proto_rt_begin(r->p);
    if (rc) {
        return rc;
    }
    *slept = 0;
    for (;;) {
        s0 = r->sleeps;
        rc = lt_proto_rt_pump(r->p, out);
        *slept += r->sleeps - s0;
        n++;
        if (rc != 0 || n > 200000) {
            break;
        }
        r->t += 0.25;
    }
    *pumps = n;
    return rc;
}

/* Let the wire go quiet (a resync after a failed reply may still be discarding bytes). */
static void rig_idle(rig *r, double ms)
{
    double end = r->t + ms;

    while (r->t < end) {
        (void)lt_proto_async_wait_ms(r->p);
        lt_proto_write_pump(r->p);
        r->t += 0.5;
    }
    lt_proto_async_finish(r->p);
}

/* Quiet, then flush the host side: both paths start the next case from the same state. */
static void rig_flush(rig *r)
{
    lt_proto_io io = lt_simlink_io(r->link);

    rig_idle(r, 50.0);
    io.clear_input(io.ud);
}

/* ------------------------------------------------------------------ */
/* 2: frames decode identically to the blocking path                    */
/* ------------------------------------------------------------------ */
static void same_frames(const char *text, const char *file, int rounds)
{
    rig r;
    lt_buf a, b;
    int i, rc, pumps = 0, max_pumps = 0, same = 0;
    long slept = 0, slept_sum = 0;
    const lt_simlink_stats *st;
    unsigned long reads0, pieces = 0;

    LT_CHECK(rig_open(&r, text, file, 115200L) == 0);
    if (!r.p || lt_proto_state(r.p) != LT_CONN_CONNECTED) {
        rig_close(&r);
        return;
    }
    lt_buf_init(&a);
    lt_buf_init(&b);
    st = lt_simlink_stats_get(r.link);
    for (i = 0; i < rounds; i++) {
        lt_ecusim_tick_engine(r.sim, 100);
        lt_buf_clear(&a);
        lt_buf_clear(&b);
        LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
        reads0 = st->reads;
        rc = rt_async(&r, &b, &pumps, &slept);
        if (st->reads - reads0 > pieces) {
            pieces = st->reads - reads0;
        }
        LT_CHECK(rc == 1);
        slept_sum += slept;
        if (pumps > max_pumps) {
            max_pumps = pumps;
        }
        if (a.len == b.len && a.len > 0 && memcmp(a.data, b.data, a.len) == 0) {
            same++;
        }
    }
    LT_CHECK_INT(same, rounds);
    LT_CHECK_INT(slept_sum, 0);                  /* pumps never wait */
    LT_CHECK(max_pumps > 10);                    /* the reply really took a while */
    if (b.len > 64) {
        LT_CHECK(pieces >= 2);                   /* frames arrived in pieces (64-byte packets) */
    }
    LT_CHECK(!lt_proto_rt_pending(r.p));
    LT_CHECK(lt_simlink_pending(r.link) == 0);
    LT_CHECK(lt_proto_async_wait_ms(r.p) < 0);   /* nothing in flight: the loop may sleep */
    lt_buf_free(&a);
    lt_buf_free(&b);
    rig_close(&r);
}

LT_TEST(async_frames_equal_sync_modern)
{
    same_frames(MODERN_INI, NULL, 10);
    same_frames(NULL, SPD_INI, 10);
    same_frames(NULL, RUS_INI, 4);
}

LT_TEST(async_frames_equal_sync_legacy)
{
    same_frames(LEGACY_INI, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* 4: timeouts, cut-off replies, noise and a dropped link               */
/* ------------------------------------------------------------------ */
static void faults(const char *text, const char *file)
{
    rig r;
    lt_buf a, b;
    int rc, rc_sync, pumps;
    long slept;
    double t0;
    int timeout;
    static const lt_u8 noise[3] = { 0x00, 0x05, 0xAA };

    LT_CHECK(rig_open(&r, text, file, 115200L) == 0);
    if (!r.p || lt_proto_state(r.p) != LT_CONN_CONNECTED) {
        rig_close(&r);
        return;
    }
    lt_buf_init(&a);
    lt_buf_init(&b);
    timeout = lt_proto_effective_timeout_ms(r.p);

    /* a lost reply: a timeout after the same wait as the blocking path */
    lt_simlink_drop_replies(r.link, 1);
    t0 = r.t;
    rc_sync = lt_proto_get_realtime(r.p, &a);
    LT_CHECK_INT(rc_sync, LT_ETIMEOUT);
    lt_simlink_drop_replies(r.link, 1);
    t0 = r.t;
    rc = rt_async(&r, &b, &pumps, &slept);
    LT_CHECK_INT(rc, LT_ETIMEOUT);
    LT_CHECK(lt_proto_errinfo(r.p)->code == LT_PE_TIMEOUT);
    LT_CHECK(r.t - t0 >= (double)timeout && r.t - t0 < (double)timeout + 60.0);
    LT_CHECK(lt_proto_state(r.p) == LT_CONN_CONNECTED);
    rig_idle(&r, 50.0);
    lt_buf_clear(&a);
    lt_buf_clear(&b);
    LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
    LT_CHECK(rt_async(&r, &b, &pumps, &slept) == 1);
    LT_CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);

    /* a reply cut off part way: timeout, the tail is resynchronised, the next frame is fine */
    lt_simlink_cut_reply(r.link, 9);
    lt_buf_clear(&b);
    rc = rt_async(&r, &b, &pumps, &slept);
    LT_CHECK(rc == LT_ETIMEOUT || rc == 1);      /* legacy: a short reply ends by silence */
    rig_idle(&r, 50.0);
    lt_buf_clear(&a);
    lt_buf_clear(&b);
    LT_CHECK(rt_async(&r, &b, &pumps, &slept) == 1);
    LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
    LT_CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);

    /* noise in front of a reply: both paths fail the same way, then recover */
    rig_flush(&r);
    lt_simlink_inject(r.link, noise, sizeof(noise));
    lt_buf_clear(&a);
    rc_sync = lt_proto_get_realtime(r.p, &a);
    rig_flush(&r);
    lt_simlink_inject(r.link, noise, sizeof(noise));
    lt_buf_clear(&b);
    rc = rt_async(&r, &b, &pumps, &slept);
    if (lt_proto_is_modern(r.p)) {
        LT_CHECK_INT(rc, rc_sync);
        LT_CHECK(rc < 0);
    } else {
        LT_CHECK(rc == 1 && rc_sync == LT_OK && a.len == b.len);   /* raw: noise is data */
    }
    rig_flush(&r);
    lt_buf_clear(&a);
    lt_buf_clear(&b);
    LT_CHECK(rt_async(&r, &b, &pumps, &slept) == 1);
    LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
    LT_CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);

    /* the link drops while a reply is on the wire: a serial error, as on the blocking path */
    LT_CHECK(lt_proto_rt_begin(r.p) == LT_OK);
    r.t += 1.0;
    LT_CHECK(lt_proto_rt_pump(r.p, &b) == 0);
    lt_simlink_set_dropped(r.link, LT_TRUE);
    LT_CHECK_INT(lt_proto_rt_pump(r.p, &b), LT_EIO);
    LT_CHECK(lt_proto_errinfo(r.p)->code == LT_PE_SERIAL);
    LT_CHECK_INT(lt_proto_get_realtime(r.p, &a), LT_EIO);
    LT_CHECK(!lt_proto_rt_pending(r.p));
    lt_buf_free(&a);
    lt_buf_free(&b);
    rig_close(&r);
}

LT_TEST(async_faults_modern)
{
    faults(MODERN_INI, NULL);
    faults(NULL, SPD_INI);
}

LT_TEST(async_faults_legacy)
{
    faults(LEGACY_INI, NULL);
}

/* ------------------------------------------------------------------ */
/* 3: blocking calls in the middle of an asynchronous request           */
/* ------------------------------------------------------------------ */
static void midflight(const char *text, const char *file)
{
    rig r;
    lt_buf a, b, rd;
    int k, rc, pumps, frames = 0;
    long slept;
    size_t plen, off;
    const lt_u8 *ram;
    lt_u8 two[2];

    LT_CHECK(rig_open(&r, text, file, 115200L) == 0);
    if (!r.p || lt_proto_state(r.p) != LT_CONN_CONNECTED) {
        rig_close(&r);
        return;
    }
    lt_buf_init(&a);
    lt_buf_init(&b);
    lt_buf_init(&rd);
    lt_ecu_mem_page(r.mem, 1, &plen);
    for (k = 0; k < 40; k++) {
        lt_ecusim_tick_engine(r.sim, 50);
        LT_CHECK(lt_proto_rt_begin(r.p) == LT_OK);
        r.t += (double)(k % 12) * 0.7;           /* anywhere from nothing to all of it arrived */
        lt_buf_clear(&b);
        rc = lt_proto_rt_pump(r.p, &b);
        LT_CHECK(rc == 0 || rc == 1);
        off = (size_t)(k * 3) % (plen - 2);
        two[0] = (lt_u8)(k + 1);
        two[1] = (lt_u8)(0xA0 + k);
        switch (k % 4) {
        case 0:                                   /* a table edit written synchronously */
            LT_CHECK(lt_proto_write_memory(r.p, 1, off, two, 2) == LT_OK);
            ram = lt_ecusim_ram(r.sim, 1, &plen);
            LT_CHECK(ram[off] == two[0] && ram[off + 1] == two[1]);
            break;
        case 1:                                   /* a page read */
            lt_buf_clear(&rd);
            LT_CHECK(lt_proto_read_memory(r.p, 1, 0, plen, &rd) == LT_OK);
            ram = lt_ecusim_ram(r.sim, 1, &plen);
            LT_CHECK(rd.len == plen && memcmp(rd.data, ram, plen) == 0);
            break;
        case 2:                                   /* a blocking realtime poll */
            lt_buf_clear(&a);
            LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
            break;
        default:                                  /* a burn */
            LT_CHECK(lt_proto_burn(r.p, -1, NULL) == LT_OK);
            ram = lt_ecusim_flash(r.sim, 1, &plen);
            LT_CHECK(memcmp(ram, lt_ecusim_ram(r.sim, 1, &plen), plen) == 0);
            break;
        }
        /* the asynchronous frame survived, intact, and equals a blocking one */
        if (rc == 0) {
            rc = lt_proto_rt_pump(r.p, &b);
        }
        LT_CHECK(rc == 1);
        lt_buf_clear(&a);
        LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK);
        if (rc == 1 && a.len == b.len && memcmp(a.data, b.data, a.len) == 0) {
            frames++;
        }
    }
    LT_CHECK_INT(frames, 40);
    LT_CHECK(lt_simlink_pending(r.link) == 0);    /* no stray bytes left on the wire */
    LT_CHECK(rt_async(&r, &b, &pumps, &slept) == 1);
    lt_buf_free(&a);
    lt_buf_free(&b);
    lt_buf_free(&rd);
    rig_close(&r);
}

LT_TEST(sync_calls_midflight_modern)
{
    midflight(MODERN_INI, NULL);
    midflight(NULL, SPD_INI);
}

LT_TEST(sync_calls_midflight_legacy)
{
    midflight(LEGACY_INI, NULL);
}

/* ------------------------------------------------------------------ */
/* asynchronous RAM writes                                              */
/* ------------------------------------------------------------------ */
/* UI-loop style: pump the writer and keep realtime requests going until the writer reports. */
static int drive_writes(rig *r, int *frames)
{
    lt_buf b;
    int rc, wr = 0, guard;

    lt_buf_init(&b);
    for (guard = 0; guard < 400000 && wr == 0; guard++) {
        wr = lt_proto_write_pump(r->p);
        if (lt_proto_rt_pending(r->p)) {
            lt_buf_clear(&b);
            rc = lt_proto_rt_pump(r->p, &b);
            if (rc == 1) {
                (*frames)++;
            }
        } else {
            (void)lt_proto_rt_begin(r->p);
        }
        r->t += 0.25;
    }
    lt_proto_async_finish(r->p);
    if (lt_proto_rt_pending(r->p)) {
        lt_buf_clear(&b);
        (void)lt_proto_rt_pump(r->p, &b);
    }
    lt_buf_free(&b);
    return wr;
}

static void writes(const char *text, long *tx_packets)
{
    rig r;
    lt_u8 v[40];
    int i, frames = 0, pages[4];
    size_t plen;
    const lt_u8 *img, *ram;
    unsigned long tx0;

    LT_CHECK(rig_open(&r, text, NULL, 115200L) == 0);
    if (!r.p || lt_proto_state(r.p) != LT_CONN_CONNECTED) {
        rig_close(&r);
        return;
    }
    /* 40 bytes across chunk boundaries (chunk = 16) plus a separate range */
    for (i = 0; i < 40; i++) {
        v[i] = (lt_u8)(0x30 + i);
    }
    LT_CHECK(lt_ecu_mem_write(r.mem, 0, 5, v, 40) == LT_OK);
    LT_CHECK(lt_ecu_mem_write(r.mem, 0, 60, v, 3) == LT_OK);
    LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 43);
    LT_CHECK(!lt_ecu_mem_has_dirty(r.mem));           /* taken by the writer */
    LT_CHECK(lt_proto_write_pending(r.p));
    /* the same cell edited again and again before it went out: merged, not queued 20 times */
    tx0 = lt_proto_stats_get(r.p)->tx_packets;
    for (i = 0; i < 20; i++) {
        v[0] = (lt_u8)i;
        LT_CHECK(lt_ecu_mem_write(r.mem, 0, 61, v, 1) == LT_OK);
        LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 1);
    }
    LT_CHECK_INT(drive_writes(&r, &frames), 1);
    *tx_packets = (long)(lt_proto_stats_get(r.p)->tx_packets - tx0);
    LT_CHECK(*tx_packets < 20);
    LT_CHECK(frames >= 1);                            /* realtime kept going between chunks */
    img = lt_ecu_mem_page(r.mem, 0, &plen);
    ram = lt_ecusim_ram(r.sim, 0, &plen);
    LT_CHECK(img && ram && memcmp(img, ram, plen) == 0);
    LT_CHECK(!lt_ecu_mem_has_dirty(r.mem));
    LT_CHECK(lt_ecu_mem_has_pending_burn(r.mem));
    LT_CHECK(lt_ecu_mem_state(r.mem, 0) == LT_PAGE_PENDING);
    LT_CHECK(lt_proto_dirty_pages(r.p, pages, 4) == 1 && pages[0] == 0);

    /* an edit of the other page: the writer burns page 0 first (auto burn on page change) */
    v[0] = 0x5A;
    LT_CHECK(lt_ecu_mem_write(r.mem, 1, 3, v, 1) == LT_OK);
    LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 1);
    LT_CHECK_INT(drive_writes(&r, &frames), 1);
    LT_CHECK(memcmp(lt_ecusim_flash(r.sim, 0, &plen), lt_ecu_mem_page(r.mem, 0, &plen), plen) == 0);
    LT_CHECK(lt_ecusim_ram(r.sim, 1, &plen)[3] == 0x5A);
    LT_CHECK(lt_proto_burn(r.p, -1, r.mem) == LT_OK);
    LT_CHECK(lt_ecusim_flash(r.sim, 1, &plen)[3] == 0x5A);
    LT_CHECK(!lt_ecu_mem_has_pending_burn(r.mem));

    /* a blocking read right after queueing sees the queued bytes: the writer goes first */
    v[0] = 0x77;
    LT_CHECK(lt_ecu_mem_write(r.mem, 1, 9, v, 1) == LT_OK);
    LT_CHECK(lt_proto_rt_begin(r.p) == LT_OK);       /* and a realtime request is in flight */
    LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 1);
    {
        lt_buf rd;

        lt_buf_init(&rd);
        LT_CHECK(lt_proto_read_memory(r.p, 1, 9, 1, &rd) == LT_OK);
        LT_CHECK(rd.len == 1 && (lt_u8)rd.data[0] == 0x77);
        lt_buf_free(&rd);
    }
    LT_CHECK_INT(lt_proto_write_pump(r.p), 1);
    {
        lt_buf b;

        lt_buf_init(&b);
        LT_CHECK(lt_proto_rt_pump(r.p, &b) == 1 && b.len == 16);   /* the frame was kept */
        lt_buf_free(&b);
    }

    /* a write whose acknowledgement never comes (modern) / a dropped link (both): the ranges
     * are dirty again, the page is marked failed and a burn is refused until it is rewritten */
    v[0] = 0x42;
    LT_CHECK(lt_ecu_mem_write(r.mem, 1, 20, v, 1) == LT_OK);
    if (lt_proto_is_modern(r.p)) {
        lt_simlink_drop_replies(r.link, 1);
        LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 1);
        LT_CHECK_INT(drive_writes(&r, &frames), LT_ETIMEOUT);
    } else {
        lt_simlink_set_dropped(r.link, LT_TRUE);
        LT_CHECK(lt_proto_write_dirty_begin(r.p, r.mem) == 1);
        LT_CHECK_INT(lt_proto_write_pump(r.p), LT_EIO);
        lt_simlink_set_dropped(r.link, LT_FALSE);
    }
    LT_CHECK(lt_ecu_mem_has_dirty(r.mem) && lt_ecu_mem_is_dirty(r.mem, 1, 20));
    LT_CHECK_INT(lt_proto_burn(r.p, -1, r.mem), LT_EBUSY);
    rig_idle(&r, 50.0);
    LT_CHECK(lt_proto_write_dirty(r.p, r.mem) == 1);
    LT_CHECK(lt_proto_burn(r.p, -1, r.mem) == LT_OK);
    LT_CHECK(lt_ecusim_flash(r.sim, 1, &plen)[20] == 0x42);
    rig_close(&r);
}

LT_TEST(async_writes)
{
    long tx = 0;

    writes(MODERN_INI, &tx);
    writes(LEGACY_INI, &tx);
}

/* ------------------------------------------------------------------ */
/* raw commands (logger data reads)                                     */
/* ------------------------------------------------------------------ */
static void raw_requests(const char *text)
{
    rig r;
    lt_buf a, b;
    int t1, t2, rc, n;
    static const lt_u8 cmd_a[1] = { 'A' };
    static const lt_u8 cmd_x[1] = { 0x7E };

    LT_CHECK(rig_open(&r, text, NULL, 115200L) == 0);
    if (!r.p || lt_proto_state(r.p) != LT_CONN_CONNECTED) {
        rig_close(&r);
        return;
    }
    lt_buf_init(&a);
    lt_buf_init(&b);
    LT_CHECK(lt_proto_send_raw_response(r.p, cmd_a, 1, 200, &a) == LT_OK && a.len == 16);
    t1 = lt_proto_raw_begin(r.p, cmd_a, 1, 200);
    LT_CHECK(t1 > 0);
    LT_CHECK(lt_proto_rt_begin(r.p) == LT_EBUSY);        /* one exchange on the wire */
    for (n = 0, rc = 0; rc == 0 && n < 100000; n++) {
        rc = lt_proto_raw_pump(r.p, t1, &b);
        r.t += 0.25;
    }
    LT_CHECK(rc == 1 && n > 4);
    LT_CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
    LT_CHECK_INT(lt_proto_raw_pump(r.p, t1, &b), LT_ENOENT);   /* collected: gone */
    rig_idle(&r, 150.0);                         /* the refused realtime request's turn lapses */
    /* a command the ECU does not answer: legacy waits for the timeout and reports an empty
     * reply (like the blocking call), modern reports the ECU's status / timeout */
    lt_buf_clear(&a);
    lt_buf_clear(&b);
    rc = lt_proto_send_raw_response(r.p, cmd_x, 1, 200, &a);
    rig_flush(&r);
    t2 = lt_proto_raw_begin(r.p, cmd_x, 1, 200);
    LT_CHECK(t2 > 0 && t2 != t1);
    for (n = 0; n < 100000; n++) {
        int k = lt_proto_raw_pump(r.p, t2, &b);

        if (k != 0) {
            LT_CHECK_INT(k == 1 ? LT_OK : k, rc);
            break;
        }
        r.t += 0.25;
    }
    LT_CHECK(a.len == b.len);
    rig_idle(&r, 500.0);                         /* a plain ECU drops the unknown byte */
    rig_flush(&r);
    /* a blocking call while a raw read is in flight: the read completes first */
    t1 = lt_proto_raw_begin(r.p, cmd_a, 1, 200);
    LT_CHECK(t1 > 0);
    lt_buf_clear(&a);
    LT_CHECK(lt_proto_get_realtime(r.p, &a) == LT_OK && a.len == 16);
    lt_buf_clear(&b);
    LT_CHECK(lt_proto_raw_pump(r.p, t1, &b) == 1 && b.len == 16);
    LT_CHECK(lt_simlink_pending(r.link) == 0);
    lt_buf_free(&a);
    lt_buf_free(&b);
    rig_close(&r);
}

LT_TEST(async_raw_requests)
{
    raw_requests(MODERN_INI);
    raw_requests(LEGACY_INI);
}

/* ------------------------------------------------------------------ */
/* 1: the application loop never waits for the ECU (real time)          */
/* ------------------------------------------------------------------ */
static const lt_ini_const *scalar_on(const lt_ini *ini, int page)
{
    size_t i;

    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];

        if (!c->is_pc_var && c->page == page && c->shape.cols <= 1 && c->shape.rows <= 1 &&
            c->type != LT_DT_BITS && c->size_bytes > 0 && c->max > c->min) {
            return c;
        }
    }
    return NULL;
}

LT_TEST(app_service_never_waits)
{
    lt_app *app = lt_app_new();
    double t, t_end, d, max_d = 0.0, sum = 0.0, sync_ms, edit_ms;
    long max_at = -1;
    double blocked0;
    long calls = 0, slow = 0;
    unsigned long f0;
    int i, w;
    lt_buf blk;
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v = 0.0;
    size_t plen, mlen;
    const lt_u8 *ram, *img;

    lt_plat_setenv("LT_DEMO_BAUD", "115200");
    LT_CHECK(lt_app_start_demo(app, SPD_INI) == 0);
    lt_plat_setenv("LT_DEMO_BAUD", "");
    LT_CHECK(app->simlink != NULL && app->conn == LT_APP_DEMO);
    if (app->simlink == NULL || app->proto == NULL) {
        lt_app_free(app);
        return;
    }
    /* about a second of main loop: service, then sleep as long as the service allows */
    f0 = app->frames_ok;
    blocked0 = lt_simlink_stats_get(app->simlink)->blocked_ms;
    t_end = lt_plat_now_ms() + 1000.0;
    while (lt_plat_now_ms() < t_end) {
        t = lt_plat_now_ms();
        lt_app_service(app);
        d = lt_plat_now_ms() - t;
        sum += d;
        calls++;
        if (d > max_d) {
            max_d = d;
            max_at = calls;
        }
        if (d >= 1.0) {
            slow++;
        }
        w = lt_app_service_wait_ms(app);
        LT_CHECK(w >= 0 && w <= 50);
        lt_plat_sleep_ms(w > 2 ? 2 : (w < 1 ? 1 : w));
    }
    /* the service never let a read wait for reply bytes (deterministic, unlike wall time) */
    LT_CHECK(lt_simlink_stats_get(app->simlink)->blocked_ms - blocked0 == 0.0);
    /* the same link through the blocking call, for comparison */
    lt_proto_async_finish(app->proto);
    lt_buf_init(&blk);
    t = lt_plat_now_ms();
    for (i = 0; i < 5; i++) {
        lt_buf_clear(&blk);
        LT_CHECK(lt_proto_get_realtime(app->proto, &blk) == LT_OK);
    }
    sync_ms = (lt_plat_now_ms() - t) / 5.0;
    lt_buf_free(&blk);
    /* the counter does see a waiting caller: the blocking polls just now waited */
    LT_CHECK(lt_simlink_stats_get(app->simlink)->blocked_ms - blocked0 > 5.0);
    printf("  service: %ld calls, avg %.3f ms, max %.3f ms, %ld >= 1 ms; %lu frames/s; "
           "blocking poll %.1f ms; slowest call #%ld\n", calls, sum / (double)(calls > 0 ? calls : 1), max_d, slow,
           app->frames_ok - f0, sync_ms, max_at);
    LT_CHECK(app->frames_ok - f0 >= 10);         /* poll_hz 20 */
    LT_CHECK(sync_ms > 8.0);                     /* the link really is slow ... */
    /* ... and the service never waited for it (checked exactly above through the link). In
     * wall time a service that waits would be slow on every poll (20 a second); shared CI
     * machines (macOS VMs) stall single calls for tens of ms, so only systematic slowness
     * fails here. */
    LT_CHECK(slow * 10 <= calls);
    LT_CHECK(sum / (double)(calls > 0 ? calls : 1) < 2.0);

    /* a table edit: queued in well under a millisecond, in ECU RAM a little later */
    c = scalar_on(app->ini, 1);
    LT_CHECK(c != NULL);
    if (c != NULL) {
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        LT_CHECK(lt_ini_set(app->ini, &ps, c->name, v == c->min ? c->max : c->min) == 0);
        t = lt_plat_now_ms();
        lt_app_edited(app);
        edit_ms = lt_plat_now_ms() - t;
        printf("  lt_app_edited: %.3f ms\n", edit_ms);
        LT_CHECK(edit_ms < 1.0 || edit_ms < sync_ms / 4.0);
        LT_CHECK(lt_app_can_burn(app));
        t_end = lt_plat_now_ms() + 3000.0;
        while (lt_proto_write_pending(app->proto) && lt_plat_now_ms() < t_end) {
            lt_app_service(app);
            lt_plat_sleep_ms(1);
        }
        LT_CHECK(!lt_proto_write_pending(app->proto));
        ram = lt_ecusim_ram(app->sim, c->page, &plen);
        img = lt_ecu_mem_page(app->mem, c->page, &mlen);
        LT_CHECK(ram && img && plen == mlen && memcmp(ram, img, plen) == 0);
        ps = lt_app_pages(app);
        /* for comparison: the same edit written the blocking way (what lt_app_edited did) */
        LT_CHECK(lt_ini_set(app->ini, &ps, c->name, v) == 0);
        t = lt_plat_now_ms();
        LT_CHECK(lt_proto_write_dirty(app->proto, app->mem) >= 0);
        printf("  blocking write of the same edit: %.1f ms\n", lt_plat_now_ms() - t);
        LT_CHECK(lt_proto_burn(app->proto, -1, app->mem) == LT_OK);  /* (the app gates pins) */
        LT_CHECK(memcmp(lt_ecusim_flash(app->sim, c->page, &plen), img, plen) == 0);
    }
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(async_frames_equal_sync_modern);
    LT_RUN(async_frames_equal_sync_legacy);
    LT_RUN(async_faults_modern);
    LT_RUN(async_faults_legacy);
    LT_RUN(sync_calls_midflight_modern);
    LT_RUN(sync_calls_midflight_legacy);
    LT_RUN(async_writes);
    LT_RUN(async_raw_requests);
    LT_RUN(app_service_never_waits);
    LT_TEST_MAIN_END
}
