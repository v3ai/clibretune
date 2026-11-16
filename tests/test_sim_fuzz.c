/* test_sim_fuzz.c - robustness: random/mutated input must never crash the virtual ECU. */
#include "lt.h"
#include "sim/sim.h"
#include "lt_test.h"
#include <string.h>
#include <stdlib.h>

#define DEMO_INI "tests/fixtures/sim/demo.ini"
static lt_u32 g_rng = 12345u;
static lt_u32 rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
static double g_now;
static double clk(void *ud) { LT_UNUSED(ud); return g_now; }

static void drain(lt_ecusim *s)
{
    lt_u8 buf[512];
    int i;
    for (i = 0; i < 64 && lt_ecusim_bytes_to_read(s) > 0; i++) {
        long r = lt_ecusim_read(s, buf, sizeof buf);
        if (r <= 0) break;
    }
}

static void put_frame(lt_u8 *out, size_t *n, const lt_u8 *pl, size_t len, int le)
{
    lt_u32 crc = lt_crc32(0, pl, len);
    size_t i = 0;
    if (le) { out[i++] = (lt_u8)len; out[i++] = (lt_u8)(len >> 8); }
    else { out[i++] = (lt_u8)(len >> 8); out[i++] = (lt_u8)len; }
    memcpy(out + i, pl, len); i += len;
    out[i++] = (lt_u8)(crc >> 24); out[i++] = (lt_u8)(crc >> 16);
    out[i++] = (lt_u8)(crc >> 8); out[i++] = (lt_u8)crc;
    *n = i;
}

LT_TEST(fuzz_random_bytes)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    int variant, it;
    LT_CHECK(ini != NULL);
    for (variant = 0; variant < 3; variant++) {
        lt_ecusim *s = lt_ecusim_new(variant == 2 ? NULL : ini);
        LT_CHECK(s != NULL);
        lt_ecusim_set_clock(s, clk, NULL);
        for (it = 0; it < 4000; it++) {
            lt_u8 buf[300], fr[400];
            size_t n = 1 + rnd() % 40, i, fn;
            int mode = (int)(rnd() % 3);
            for (i = 0; i < n; i++) buf[i] = (lt_u8)rnd();
            if (mode == 1) {
                static const char cmds[] = "QSApRMCbBkOrwtvVFcdelm";
                buf[0] = (lt_u8)cmds[rnd() % (sizeof cmds - 1)];
                if (n > 1) buf[1] = (lt_u8)(rnd() % 6);
            }
            g_now += (double)(rnd() % 600);
            if (mode == 2) { /* valid-CRC envelope around random payload */
                put_frame(fr, &fn, buf, n, (int)(rnd() & 1));
                (void)lt_ecusim_write(s, fr, fn);
            } else {
                (void)lt_ecusim_write(s, buf, n);
            }
            if (it % 7 == 0) drain(s);
            if (it % 500 == 0) lt_ecusim_reboot(s);
            if (it % 100 == 0) lt_ecusim_tick_engine(s, 50);
            if (it % 997 == 0) lt_ecusim_clear_input(s);
        }
        lt_ecusim_free(s);
    }
    lt_ini_free(ini);
}

LT_TEST(fuzz_huge_declared_lengths)
{
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    lt_ecusim *s = lt_ecusim_new(ini);
    lt_u8 m[8];
    int i;
    for (i = 0; i < 4; i++) {
        m[0] = (lt_u8)"MCrR"[i]; m[1] = 0; m[2] = 0; m[3] = 0xFF; m[4] = 0xFF; m[5] = 0xFF; m[6] = 0xFF;
        (void)lt_ecusim_write(s, m, 7);
        drain(s);
        g_now += 1000;
        (void)lt_ecusim_write(s, m, 1);
        drain(s);
    }
    lt_ecusim_free(s);
    lt_ini_free(ini);
}

LT_TEST(fuzz_mutated_ini)
{
    FILE *f = fopen(DEMO_INI, "rb");
    char *txt;
    long len;
    int it;
    LT_CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    txt = (char *)malloc((size_t)len + 1);
    LT_CHECK(fread(txt, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    for (it = 0; it < 300; it++) {
        char *m = (char *)malloc((size_t)len + 1);
        size_t cut = (size_t)len, k, nm = 1 + rnd() % 20;
        lt_ini *ini;
        memcpy(m, txt, (size_t)len);
        for (k = 0; k < nm; k++) m[rnd() % (lt_u32)len] = (char)rnd();
        if (it % 3 == 0) cut = rnd() % (lt_u32)len;
        ini = lt_ini_load_string(m, cut, NULL, NULL);
        if (ini) {
            lt_sim_snapshot sn;
            lt_ecusim *s = lt_ecusim_new(ini);
            lt_u8 blk[4096];
            size_t bs = lt_sim_block_size(ini);
            memset(&sn, 0, sizeof sn);
            if (bs > 0 && bs <= sizeof blk) lt_sim_encode_och(ini, &sn, blk, bs);
            if (s) {
                lt_u8 q = 'Q', r[7];
                (void)lt_ecusim_write(s, &q, 1);
                drain(s);
                r[0] = 'r'; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = 0; r[5] = 0; r[6] = 20;
                (void)lt_ecusim_write(s, r, 7);
                lt_ecusim_tick_engine(s, 200);
                drain(s);
                lt_ecusim_free(s);
            }
            lt_ini_free(ini);
        }
        free(m);
    }
    free(txt);
}

static double g_jump;
static double clk_jump(void *ud) { LT_UNUSED(ud); return g_jump; }

LT_TEST(flood_and_clock_jump)
{
    lt_ecusim *s = lt_ecusim_new(NULL);
    lt_ecusim *e = lt_ecusim_new(NULL);
    lt_u8 junk[1000], a = 'A';
    int i;

    memset(junk, 0xEE, sizeof junk);
    for (i = 0; i < 400; i++) (void)lt_ecusim_write(s, junk, sizeof junk);
    LT_CHECK(s != NULL);
    /* still answers once the junk is cleared */
    lt_ecusim_clear_output(s);
    a = 'Q';
    (void)lt_ecusim_write(s, &a, 1);
    LT_CHECK(lt_ecusim_bytes_to_read(s) > 0);
    lt_ecusim_free(s);
    /* a huge clock jump must not hang or invoke UB */
    lt_ecusim_set_clock(e, clk_jump, NULL);
    g_jump = 0;
    a = 'A';
    (void)lt_ecusim_write(e, &a, 1);
    g_jump = 1e15;
    (void)lt_ecusim_write(e, &a, 1);
    LT_CHECK(lt_ecusim_bytes_to_read(e) > 0);
    lt_ecusim_free(e);
}

int main(void)
{
    LT_RUN(fuzz_random_bytes);
    LT_RUN(fuzz_huge_declared_lengths);
    LT_RUN(fuzz_mutated_ini);
    LT_RUN(flood_and_clock_jump);
    LT_TEST_MAIN_END
}
