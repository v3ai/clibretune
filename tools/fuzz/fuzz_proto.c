/* fuzz_proto.c - ECU protocol: the msEnvelope frame decoder on the raw input, then a whole
 * session (handshake, page reads, realtime, writes, burn, console) whose ECU replies are the
 * input bytes. Byte 0 picks options (envelope order, permissive CRC, INI or none, chunking). */
#include "fuzz_common.h"
#include "proto/proto.h"

typedef struct feed { const lt_u8 *d; size_t n, pos; int chunk, reads; } feed;
static double g_t;

static double f_now(void *ud) { (void)ud; return g_t; }
static void f_sleep(void *ud, int ms) { (void)ud; g_t += ms > 0 ? ms : 1; }

static int f_read(void *ud, void *buf, int n, int to)
{
    feed *f = (feed *)ud;
    size_t k;
    (void)to;
    g_t += 1;
    if (++f->reads > 20000) return -1;
    if (f->pos >= f->n) { g_t += to > 0 ? to : 50; return 0; }
    k = f->n - f->pos;
    if (k > (size_t)n) k = (size_t)n;
    if (f->chunk && k > (size_t)f->chunk) k = (size_t)f->chunk;
    memcpy(buf, f->d + f->pos, k);
    f->pos += k;
    return (int)k;
}

static int f_write(void *ud, const void *buf, int n) { (void)ud; (void)buf; return n; }
static void f_clear(void *ud) { (void)ud; }

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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static lt_ini *ini;
    lt_u8 opt, wr[64];
    const lt_u8 *pl;
    size_t pn, used, off;
    feed f;
    lt_proto_io io;
    lt_proto_clock ck;
    lt_proto_cfg cfg;
    lt_proto *p;
    lt_buf out;
    lt_ecu_mem *m = NULL;
    lt_u32 crc;
    char *res;
    if (!ini) ini = lt_ini_load_string(k_ini, strlen(k_ini), NULL, NULL);
    if (size < 1) return 0;
    opt = data[0];
    data++; size--;

    /* frame decoder over the whole input, frame after frame */
    for (off = 0; off < size;) {
        if (lt_env_decode((lt_env_order)(opt & 1), (lt_bool)((opt >> 1) & 1), data + off,
                          size - off, &pl, &pn, &used) != LT_OK || used == 0) break;
        off += used;
    }

    memset(&f, 0, sizeof f);
    f.d = data; f.n = size; f.chunk = (opt >> 4) & 7;
    memset(&io, 0, sizeof io);
    io.ud = &f; io.read = f_read; io.write = f_write; io.clear_input = f_clear;
    ck.ud = NULL; ck.now_ms = f_now; ck.sleep_ms = f_sleep;
    lt_proto_cfg_default(&cfg);
    cfg.permissive_crc = (lt_bool)((opt >> 1) & 1);
    cfg.env_order_hint = (opt & 4) ? -1 : (int)(opt & 1);
    cfg.adaptive_timing = (lt_bool)((opt >> 3) & 1);
    p = lt_proto_new(&io, (opt & 0x80) ? NULL : ini, &cfg);
    lt_proto_set_clock(p, &ck);
    memset(wr, 0x5a, sizeof wr);
    if (lt_proto_connect(p) == LT_OK) {
        lt_buf_init(&out);
        if (!(opt & 0x80)) m = lt_ecu_mem_from_ini(ini);
        if (m) {
            (void)lt_proto_read_page(p, 0, m);
            (void)lt_proto_read_all(p, m, LT_TRUE);
        }
        (void)lt_proto_get_realtime(p, &out);
        (void)lt_proto_write_page(p, 1, wr, sizeof wr);
        (void)lt_proto_write_memory_verified(p, 0, 3, wr, 10);
        (void)lt_proto_burn(p, -1, m);
        (void)lt_proto_read_calibration_crc(p, 2, &crc);
        if (lt_proto_console(p, "help", 100, &res) == LT_OK) lt_free(res);
        lt_buf_free(&out);
        if (m) lt_ecu_mem_free(m);
    }
    lt_proto_free(p);
    return 0;
}
