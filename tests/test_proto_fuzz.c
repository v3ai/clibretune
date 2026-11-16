/* test_proto_fuzz.c - robustness: random truncation / mutation / garbage must
 * never crash or overflow (run under -fsanitize=address,undefined). Also
 * regression tests for review fixes. */
#include "lt.h"
#include "proto/proto.h"
#include "lt_test.h"

#include <string.h>

static unsigned long g_seed = 12345UL;

static unsigned rnd(void)
{
    g_seed = (g_seed * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return (unsigned)(g_seed >> 8);
}

/* ---- fake clock ---- */
static double g_t;
static double f_now(void *ud)
{
    LT_UNUSED(ud);
    return g_t;
}
static void f_sleep(void *ud, int ms)
{
    LT_UNUSED(ud);
    g_t += ms;
}

/* ---- noise io: replies are random bytes (optionally mutated valid frames) ---- */
typedef struct noise {
    lt_u8 buf[600];
    size_t n, pos;
    int mode;           /* 0 random, 1 mutated valid frame */
    int closed_after;   /* read returns -1 after this many reads (0 = never) */
    int reads;
    int skip_arm;       /* keep the pre-loaded frame for the next write(s) */
} noise;

static void noise_arm(noise *z)
{
    size_t i, k;
    lt_buf f;
    lt_u8 pl[40];

    z->pos = 0;
    if (z->mode == 0) {
        z->n = rnd() % 300;
        for (i = 0; i < z->n; i++) {
            z->buf[i] = (lt_u8)rnd();
        }
        return;
    }
    k = rnd() % 30;
    pl[0] = (lt_u8)(rnd() % 3 == 0 ? rnd() : 0);
    for (i = 1; i < sizeof(pl); i++) {
        pl[i] = (lt_u8)('A' + rnd() % 20);
    }
    lt_buf_init(&f);
    lt_env_encode((lt_env_order)(rnd() & 1), pl, k + 1, &f);
    z->n = f.len;
    memcpy(z->buf, f.data, f.len);
    lt_buf_free(&f);
    k = rnd() % 4;
    for (i = 0; i < k; i++) {
        z->buf[rnd() % z->n] = (lt_u8)rnd();
    }
    if (rnd() % 3 == 0) {
        z->n = rnd() % (z->n + 1);
    }
}

static int n_read(void *ud, void *buf, int n, int to)
{
    noise *z = (noise *)ud;
    size_t k;

    LT_UNUSED(to);
    z->reads++;
    if (z->closed_after && z->reads > z->closed_after) {
        return -1;
    }
    if (z->pos >= z->n) {
        return 0;
    }
    k = z->n - z->pos;
    if (k > (size_t)n) {
        k = (size_t)n;
    }
    if (k > 7) {
        k = 1 + rnd() % k;
    }
    memcpy(buf, z->buf + z->pos, k);
    z->pos += k;
    return (int)k;
}

static int n_write(void *ud, const void *buf, int n)
{
    noise *z = (noise *)ud;

    LT_UNUSED(buf);
    if (z->skip_arm > 0) {
        z->skip_arm--;
        return n;
    }
    noise_arm(z);   /* every request gets a fresh random answer */
    return n;
}

static void n_clear(void *ud)
{
    noise *z = (noise *)ud;

    if (z->skip_arm == 0) {
        z->pos = z->n;
    }
}

static const char *k_ini =
    "[MegaTune]\n signature = \"speeduino 202501\"\n"
    "[TunerStudio]\n queryCommand = \"Q\"\n"
    " messageEnvelopeFormat = msEnvelope_1.0\n endianness = big\n"
    "[Constants]\n pageSize = 128, 64\n blockingFactor = 32\n"
    " pageReadCommand = \"R%2i%2o%2c\", \"R%2i%2o%2c\"\n"
    " pageChunkWrite = \"C%2i%2o%2c%v\", \"C%2i%2o%2c%v\"\n"
    " burnCommand = \"B%2i\", \"B%2i\"\n"
    " crc32CheckCommand = \"d%2i\", \"d%2i\"\n"
    " ochGetCommand = \"r\\x00\\x30%2o%2c\"\n ochBlockSize = 20\n"
    "[OutputChannels]\n ochGetCommand = \"r\\x00\\x30%2o%2c\"\n ochBlockSize = 20\n";

LT_TEST(fuzz_envelope_decode)
{
    int it, i;
    lt_u8 pl[64], d[128];
    lt_buf f;
    const lt_u8 *op;
    size_t on, used, len;

    for (it = 0; it < 4000; it++) {
        len = rnd() % 40;
        for (i = 0; i < (int)len; i++) {
            pl[i] = (lt_u8)rnd();
        }
        lt_buf_init(&f);
        lt_env_encode((lt_env_order)(rnd() & 1), pl, len, &f);
        memcpy(d, f.data, f.len);
        for (i = rnd() % 4; i > 0; i--) {
            d[rnd() % f.len] = (lt_u8)rnd();
        }
        used = rnd() % (f.len + 3);
        if (used > f.len) {
            used = f.len;
        }
        if (lt_env_decode((lt_env_order)(rnd() & 1), (lt_bool)(rnd() & 1), d, used, &op, &on,
                          &used) == LT_OK) {
            LT_CHECK(on + 6 == used);
        }
        lt_buf_free(&f);
    }
    LT_CHECK(1);
}

LT_TEST(fuzz_parsers)
{
    int it, i, n;
    char s[200];
    lt_buf b;
    lt_u8 data[8];
    lt_u8 sg[100];
    char sig[40], msg[40];
    size_t dl;
    int code;
    char *r;
    static const char alpha[] = "\\xX0123456789abcnrt%2iocv`|msgemu wave_chart\n \x01\xff";

    memset(data, 7, sizeof(data));
    for (it = 0; it < 3000; it++) {
        n = (int)(rnd() % 120);
        for (i = 0; i < n; i++) {
            s[i] = alpha[rnd() % (sizeof(alpha) - 1)];
        }
        s[n] = '\0';
        lt_buf_init(&b);
        lt_cmd_parse_string(s, (size_t)n, &b);
        lt_buf_clear(&b);
        lt_cmd_build(&b, s, (size_t)n, rnd(), rnd(), rnd(), data, sizeof(data), (lt_bool)(rnd() & 1));
        lt_buf_free(&b);
        r = lt_parse_rusefi_text_output(s);
        LT_CHECK(r != NULL);
        lt_free(r);
        for (i = 0; i < n; i++) {
            sg[i % 100] = (lt_u8)s[i];
        }
        n = n > 100 ? 100 : n;
        (void)lt_proto_signature_from_payload(sg, (size_t)n, sig, sizeof(sig));
        (void)lt_proto_check_write_status(sg, (size_t)n, msg, sizeof(msg), &code);
        (void)lt_proto_strip_status(sg, (size_t)n, rnd() % 50, "x", &dl);
        (void)lt_disc_classify(sg, (size_t)n, 'Q', sig, sizeof(sig));
    }
    /* extreme / tiny buffers */
    (void)lt_proto_signature_from_payload(sg, 5, sig, 1);
    (void)lt_proto_check_write_status(sg, 5, msg, 1, &code);
    (void)lt_disc_classify(sg, 50, 'Q', sig, 1);
}

LT_TEST(fuzz_calibration_nan)
{
    double t[32], a[1024];
    lt_u8 w[64], o[1024];
    int i;

    for (i = 0; i < 32; i++) {
        t[i] = (i & 1) ? 1e300 : -1e300;
    }
    t[3] = 0.0 / (g_t - g_t + 0.0 + (double)(rnd() & 0));  /* NaN at run time */
    lt_cal_encode_temperature(t, w);
    for (i = 0; i < 1024; i++) {
        a[i] = (i & 1) ? 1e300 : -1e300;
    }
    a[5] = t[3];
    lt_cal_encode_o2(a, o);
    LT_CHECK(o[5] == 0 && o[1] == 255 && o[0] == 0);
}

static void run_noise(const lt_ini *ini, int mode)
{
    noise z;
    lt_proto_io io;
    lt_proto_clock ck;
    lt_proto *p;
    lt_buf out;
    int it, rc;
    lt_proto_cfg cfg;
    lt_ecu_mem *m;
    lt_u8 data[64];
    char *res;
    lt_u32 crc;

    memset(&z, 0, sizeof(z));
    z.mode = mode;
    memset(data, 0x5a, sizeof(data));
    memset(&io, 0, sizeof(io));
    io.ud = &z;
    io.read = n_read;
    io.write = n_write;
    io.clear_input = n_clear;
    ck.ud = NULL;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_cfg_default(&cfg);
    cfg.permissive_crc = (lt_bool)(mode & 1);
    for (it = 0; it < 60; it++) {
        cfg.adaptive_timing = (lt_bool)(it & 1);
        p = lt_proto_new(&io, ini, &cfg);
        lt_proto_set_clock(p, &ck);
        z.reads = 0;
        z.closed_after = (it % 7 == 6) ? 5 : 0;
        noise_arm(&z);
        rc = lt_proto_connect(p);
        if (rc != LT_OK) {
            /* force a connected state through a cooperative reply */
            lt_proto_free(p);
            continue;
        }
        lt_buf_init(&out);
        m = lt_ecu_mem_from_ini(ini);
        (void)lt_proto_read_page(p, 0, m);
        (void)lt_proto_read_all(p, m, LT_TRUE);
        (void)lt_proto_get_realtime(p, &out);
        (void)lt_proto_write_page(p, 1, data, sizeof(data));
        (void)lt_proto_write_memory_verified(p, 0, 3, data, 10);
        (void)lt_proto_burn(p, -1, m);
        (void)lt_proto_write_calibration(p, LT_CAL_CLT, data, 64);
        (void)lt_proto_read_calibration_crc(p, 2, &crc);
        if (lt_proto_console(p, "help", 100, &res) == LT_OK) {
            lt_free(res);
        }
        lt_buf_free(&out);
        lt_ecu_mem_free(m);
        lt_proto_free(p);
    }
}

LT_TEST(fuzz_proto_garbage_io)
{
    lt_ini *ini = lt_ini_load_string(k_ini, strlen(k_ini), NULL, NULL);
    int mode;

    LT_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    for (mode = 0; mode < 4; mode++) {
        run_noise(ini, mode);
        run_noise(NULL, mode);
    }
    lt_ini_free(ini);
}

LT_TEST(fuzz_proto_garbage_connected)
{
    /* Force a connected session by answering the handshake properly, then fuzz. */
    lt_ini *ini = lt_ini_load_string(k_ini, strlen(k_ini), NULL, NULL);
    noise z;
    lt_proto_io io;
    lt_proto_clock ck;
    lt_proto *p;
    lt_proto_cfg cfg;
    lt_buf out, f;
    lt_u8 pl[20];
    int it, i, rc;
    lt_ecu_mem *m;

    LT_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    memset(&z, 0, sizeof(z));
    memset(&io, 0, sizeof(io));
    io.ud = &z;
    io.read = n_read;
    io.write = n_write;
    io.clear_input = n_clear;
    ck.ud = NULL;
    ck.now_ms = f_now;
    ck.sleep_ms = f_sleep;
    lt_proto_cfg_default(&cfg);
    cfg.env_order_hint = LT_ENV_BIG;
    for (it = 0; it < 200; it++) {
        p = lt_proto_new(&io, ini, &cfg);
        lt_proto_set_clock(p, &ck);
        /* valid handshake reply (BE: ini says big, speeduino -> LE declared; send LE) */
        pl[0] = 0;
        memcpy(pl + 1, "speeduino 202501", 16);
        lt_buf_init(&f);
        lt_env_encode(LT_ENV_BIG, pl, 17, &f);
        z.n = f.len;
        memcpy(z.buf, f.data, f.len);
        z.pos = 0;
        lt_buf_free(&f);
        z.mode = it & 1;
        z.closed_after = 0;
        z.reads = 0;
        z.skip_arm = 1;
        rc = lt_proto_connect(p);
        LT_CHECK(rc == LT_OK);
        if (rc == LT_OK) {
            lt_buf_init(&out);
            m = lt_ecu_mem_from_ini(ini);
            for (i = 0; i < 5; i++) {
                (void)lt_proto_read_page(p, i & 1, m);
                (void)lt_proto_get_realtime(p, &out);
                (void)lt_proto_write_memory(p, i & 1, (size_t)i * 3, pl, 15);
            }
            lt_buf_free(&out);
            lt_ecu_mem_free(m);
        }
        lt_proto_free(p);
    }
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(fuzz_envelope_decode);
    LT_RUN(fuzz_parsers);
    LT_RUN(fuzz_calibration_nan);
    LT_RUN(fuzz_proto_garbage_io);
    LT_RUN(fuzz_proto_garbage_connected);
    LT_TEST_MAIN_END
}
