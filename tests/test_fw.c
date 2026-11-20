/* test_fw.c - firmware image files (Intel HEX, S-record, DfuSe, raw), round trips, malformed
 * and truncated input, image facts, DfuSe descriptor/memory-map helpers and the erase plan. */
#include <string.h>
#include "lt.h"
#include "fw/fw.h"
#include "lt_test.h"

static int parse_str(lt_fw_image *img, const char *s, char *err, size_t cap)
{
    return lt_fw_image_parse(img, (const lt_u8 *)s, strlen(s), LT_FW_FMT_UNKNOWN, LT_FW_STM32_FLASH, err, cap);
}

static int images_equal(const lt_fw_image *a, const lt_fw_image *b)
{
    size_t i;
    if (a->n_segs != b->n_segs || a->total != b->total) { return 0; }
    for (i = 0; i < a->n_segs; i++) {
        if (a->segs[i].addr != b->segs[i].addr || a->segs[i].len != b->segs[i].len) { return 0; }
        if (memcmp(a->segs[i].data, b->segs[i].data, a->segs[i].len) != 0) { return 0; }
    }
    return 1;
}

LT_TEST(ihex_basic)
{
    /* extended linear 0x0800, two data records at 0x0000 and 0x0010, start linear, EOF */
    const char *h =
        ":020000040800F2\n"
        ":1000000000000220010000080000000000000000C5\n"
        ":10001000000102030405060708090A0B0C0D0E0F68\n"
        ":0400000508000101ED\n"
        ":00000001FF\n";
    lt_fw_image img;
    char err[200];
    LT_CHECK_INT(parse_str(&img, h, err, sizeof(err)), 0);
    LT_CHECK_STR(err, "");
    LT_CHECK_INT(img.format, LT_FW_FMT_IHEX);
    LT_CHECK_INT((long)img.n_segs, 1);
    LT_CHECK(img.lo == 0x08000000UL);
    LT_CHECK(img.last == 0x0800001FUL);
    LT_CHECK_INT((long)img.total, 32);
    LT_CHECK(img.has_entry && img.entry == 0x08000101UL);
    LT_CHECK_INT(img.segs[0].data[0x13], 3);
    lt_fw_image_free(&img);
}

LT_TEST(ihex_errors)
{
    lt_fw_image img;
    char err[200];
    /* checksum */
    LT_CHECK_INT(parse_str(&img, ":0400000001020304F1\n:00000001FF\n", err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "checksum") != NULL);
    /* no EOF record */
    LT_CHECK_INT(parse_str(&img, ":0400000001020304F2\n", err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "end-of-file") != NULL);
    /* bad length */
    LT_CHECK_INT(parse_str(&img, ":0500000001020304F2\n:00000001FF\n", err, sizeof(err)), -1);
    /* garbage between records */
    LT_CHECK_INT(parse_str(&img, ":0400000001020304F2\nhello\n:00000001FF\n", err, sizeof(err)), -1);
    /* unsupported record type 07 */
    LT_CHECK_INT(parse_str(&img, ":00000007F9\n:00000001FF\n", err, sizeof(err)), -1);
    /* overlap: the same address twice */
    LT_CHECK_INT(parse_str(&img, ":0400000001020304F2\n:020002001122C9\n:00000001FF\n", err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "overlap") != NULL);
    /* only an EOF record: no data */
    LT_CHECK_INT(parse_str(&img, ":00000001FF\n", err, sizeof(err)), -1);
    LT_CHECK(img.n_segs == 0);
}

LT_TEST(srec_basic)
{
    const char *s =
        "S010000068656C6C6F20202020202000001B\n"
        "S30908000000AABBCCDDE0\n"
        "S70508000101F0\n";
    lt_fw_image img;
    char err[200];
    LT_CHECK_INT(parse_str(&img, s, err, sizeof(err)), 0);
    LT_CHECK_INT(img.format, LT_FW_FMT_SREC);
    LT_CHECK(img.lo == 0x08000000UL && img.total == 4);
    LT_CHECK_INT(img.segs[0].data[3], 0xDD);
    LT_CHECK(img.has_entry && img.entry == 0x08000101UL);
    lt_fw_image_free(&img);
    LT_CHECK_INT(parse_str(&img, "S30908000000AABBCCDDE1\nS70508000101F0\n", err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "checksum") != NULL);
    LT_CHECK_INT(parse_str(&img, "S30908000000AABBCCDDE0\n", err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "termination") != NULL);
    LT_CHECK_INT(parse_str(&img, "S4030000FC\nS9030000FC\n", err, sizeof(err)), -1);
}

LT_TEST(bin_and_names)
{
    lt_fw_image img;
    char err[200];
    static const lt_u8 d[8] = { 0, 0, 2, 0x20, 0x89, 1, 0, 8 };
    LT_CHECK_INT(lt_fw_image_parse(&img, d, 8, LT_FW_FMT_BIN, 0x08004000UL, err, sizeof(err)), 0);
    LT_CHECK(img.lo == 0x08004000UL && img.last == 0x08004007UL);
    lt_fw_image_free(&img);
    LT_CHECK_INT(lt_fw_image_parse(&img, d, 0, LT_FW_FMT_BIN, 0, err, sizeof(err)), -1);
    LT_CHECK_INT(lt_fw_format_from_name("x/rusefi_miata-ecu.bin"), LT_FW_FMT_BIN);
    LT_CHECK_INT(lt_fw_format_from_name("a.HEX"), LT_FW_FMT_IHEX);
    LT_CHECK_INT(lt_fw_format_from_name("a.s19"), LT_FW_FMT_SREC);
    LT_CHECK_INT(lt_fw_format_from_name("a.dfu"), LT_FW_FMT_DFUSE);
    LT_CHECK_INT(lt_fw_format_from_name("a.zip"), LT_FW_FMT_UNKNOWN);
    /* data past the 32-bit address space */
    LT_CHECK_INT(lt_fw_image_parse(&img, d, 8, LT_FW_FMT_BIN, 0xFFFFFFFCUL, err, sizeof(err)), -1);
}

LT_TEST(merge_out_of_order)
{
    lt_fw_image img;
    lt_u8 a[16], b[16], out[56];
    char err[100];
    int i;
    for (i = 0; i < 16; i++) { a[i] = (lt_u8)i; b[i] = (lt_u8)(100 + i); }
    lt_fw_image_init(&img);
    LT_CHECK_INT(lt_fw_image_add(&img, 0x1010, b, 16, err, sizeof(err)), 0);
    LT_CHECK_INT(lt_fw_image_add(&img, 0x1000, a, 16, err, sizeof(err)), 0);
    LT_CHECK_INT(lt_fw_image_add(&img, 0x1030, a, 4, err, sizeof(err)), 0);
    LT_CHECK_INT(lt_fw_image_finish(&img, err, sizeof(err)), 0);
    LT_CHECK_INT((long)img.n_segs, 2);
    LT_CHECK_INT((long)img.segs[0].len, 32);
    LT_CHECK(lt_fw_image_read(&img, 0x1000, out, 56, 0xEE) == 36);
    LT_CHECK(out[15] == 15 && out[16] == 100 && out[32] == 0xEE && out[48] == 0 && out[51] == 3);
    LT_CHECK(lt_fw_image_touches(&img, 0x1020, 0x10) == 0);
    LT_CHECK(lt_fw_image_touches(&img, 0x1020, 0x11) == 1);
    lt_fw_image_free(&img);
}

LT_TEST(dfu_crc_value)
{
    /* CRC-32 of "123456789" is 0xCBF43926; DfuSe keeps it without the final inversion */
    LT_CHECK(lt_fw_dfu_crc((const lt_u8 *)"123456789", 9) == 0x340BC6D9UL);
}

static void roundtrip(int fmt)
{
    lt_fw_image a, b;
    lt_buf buf;
    char err[200];
    lt_fw_sim_demo_image(&a, 1, 40);
    a.entry = 0x08008201UL;
    a.has_entry = 1;
    lt_buf_init(&buf);
    if (fmt == LT_FW_FMT_IHEX) { lt_fw_write_ihex(&a, &buf); }
    else if (fmt == LT_FW_FMT_SREC) { lt_fw_write_srec(&a, &buf); }
    else { lt_fw_write_dfuse(&a, 0x0483, 0xDF11, 0, "ST...", &buf); }
    LT_CHECK_INT(lt_fw_image_parse(&b, (const lt_u8 *)buf.data, buf.len, LT_FW_FMT_UNKNOWN, 0, err, sizeof(err)), 0);
    LT_CHECK_INT(b.format, fmt);
    LT_CHECK(images_equal(&a, &b));
    if (fmt != LT_FW_FMT_DFUSE) { LT_CHECK(b.has_entry && b.entry == 0x08008201UL); }
    else {
        LT_CHECK(b.dfu_vid == 0x0483 && b.dfu_pid == 0xDF11 && b.dfu_alt == 0);
        LT_CHECK_STR(b.dfu_target, "ST...");
    }
    lt_fw_image_free(&a);
    lt_fw_image_free(&b);
    lt_buf_free(&buf);
}

LT_TEST(roundtrips)
{
    roundtrip(LT_FW_FMT_IHEX);
    roundtrip(LT_FW_FMT_SREC);
    roundtrip(LT_FW_FMT_DFUSE);
}

LT_TEST(dfuse_damage)
{
    lt_fw_image a, b;
    lt_buf buf;
    char err[200];
    lt_u8 *d;
    lt_fw_sim_demo_image(&a, 0, 4);
    lt_buf_init(&buf);
    lt_fw_write_dfuse(&a, 0x0483, 0xDF11, 0, NULL, &buf);
    d = (lt_u8 *)buf.data;
    d[400] ^= 0x40;
    LT_CHECK_INT(lt_fw_image_parse(&b, d, buf.len, LT_FW_FMT_DFUSE, 0, err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "CRC") != NULL);
    d[400] ^= 0x40;
    d[buf.len - 7] = 'X';
    LT_CHECK_INT(lt_fw_image_parse(&b, d, buf.len, LT_FW_FMT_DFUSE, 0, err, sizeof(err)), -1);
    LT_CHECK(strstr(err, "UFD") != NULL);
    d[buf.len - 7] = 'F';
    LT_CHECK_INT(lt_fw_image_parse(&b, d, buf.len, LT_FW_FMT_DFUSE, 0, err, sizeof(err)), 0);
    lt_fw_image_free(&b);
    LT_CHECK_INT(lt_fw_image_parse(&b, d, buf.len - 100, LT_FW_FMT_DFUSE, 0, err, sizeof(err)), -1);
    lt_fw_image_free(&a);
    lt_buf_free(&buf);
}

static lt_u32 rnd(lt_u32 *x)
{
    lt_u32 v = *x;
    v ^= (v << 13) & 0xFFFFFFFFUL;
    v ^= v >> 17;
    v ^= (v << 5) & 0xFFFFFFFFUL;
    *x = v & 0xFFFFFFFFUL;
    return *x;
}

/* every truncation and many random corruptions must parse or fail cleanly */
LT_TEST(fuzz_truncations)
{
    lt_fw_image a, b;
    lt_buf enc[3];
    char err[200];
    int f, i, ok = 1, accepted = 0;
    lt_u32 x = 0xBEEF1234UL;
    lt_fw_sim_demo_image(&a, 0, 2);
    for (f = 0; f < 3; f++) { lt_buf_init(&enc[f]); }
    lt_fw_write_ihex(&a, &enc[0]);
    lt_fw_write_srec(&a, &enc[1]);
    lt_fw_write_dfuse(&a, 0x0483, 0xDF11, 0, "x", &enc[2]);
    for (f = 0; f < 3; f++) {
        size_t n, step = enc[f].len > 3000 ? 7 : 1;
        for (n = 0; n < enc[f].len; n += step) {
            int rc = lt_fw_image_parse(&b, (const lt_u8 *)enc[f].data, n, LT_FW_FMT_UNKNOWN, 0, err, sizeof(err));
            if (rc == 0) {
                accepted++;
                if (b.total > a.total || b.n_segs == 0) { ok = 0; }
                lt_fw_image_free(&b);
            } else if (err[0] == '\0' || b.n_segs != 0) {
                ok = 0;
            }
        }
        for (i = 0; i < 300; i++) {
            lt_u8 *c = (lt_u8 *)lt_malloc(enc[f].len);
            size_t k, nflip = 1 + rnd(&x) % 4;
            memcpy(c, enc[f].data, enc[f].len);
            for (k = 0; k < nflip; k++) { c[rnd(&x) % enc[f].len] = (lt_u8)rnd(&x); }
            if (lt_fw_image_parse(&b, c, enc[f].len, LT_FW_FMT_UNKNOWN, 0, err, sizeof(err)) == 0) {
                lt_fw_image_free(&b);
            }
            lt_free(c);
        }
        lt_buf_free(&enc[f]);
    }
    LT_CHECK(ok);
    /* a truncated hex/srec/dfu never passes as complete (raw binary sniffing aside) */
    LT_CHECK(accepted < 40);
    lt_fw_image_free(&a);
}

LT_TEST(facts)
{
    lt_fw_image img;
    lt_fw_facts f;
    lt_u8 *p;
    char sum[160];
    lt_fw_sim_demo_image(&img, 1, 64);
    lt_fw_image_facts(&img, &f);
    LT_CHECK_INT(f.vectors, 1);
    LT_CHECK_INT(f.merged_openblt, 1);
    LT_CHECK_INT(f.app_vectors, 1);
    LT_CHECK_INT(f.app_crc, 1);
    LT_CHECK(f.app_size == 64u * 1024u);
    LT_CHECK_INT(f.n_warn, 0);
    lt_fw_image_summary(&img, sum, sizeof(sum));
    LT_CHECK(strstr(sum, "0x08000000-0x08017FFF") != NULL);
    /* one flipped byte in the app: OpenBLT would refuse it */
    p = img.segs[0].data + 0x8000 + 0x5000;
    *p ^= 1;
    lt_fw_image_facts(&img, &f);
    LT_CHECK_INT(f.app_crc, 0);
    LT_CHECK_INT(f.n_warn, 1);
    *p ^= 1;
    lt_fw_image_free(&img);
    /* the merged .bin loaded at the app address: reset vector below the base */
    {
        lt_fw_image a;
        char err[100];
        lt_fw_sim_demo_image(&a, 1, 8);
        LT_CHECK_INT(lt_fw_image_parse(&img, a.segs[0].data, a.segs[0].len, LT_FW_FMT_BIN, 0x08008000UL, err,
                                       sizeof(err)), 0);
        lt_fw_image_facts(&img, &f);
        LT_CHECK_INT(f.vectors, 0);
        LT_CHECK(f.n_warn >= 1);
        lt_fw_image_free(&img);
        lt_fw_image_free(&a);
    }
}

LT_TEST(layout)
{
    lt_fw_region r[8];
    char name[64];
    int n = lt_fw_dfuse_layout("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg", r, 8, name, sizeof(name));
    LT_CHECK_INT(n, 3);
    LT_CHECK_STR(name, "Internal Flash");
    LT_CHECK(r[0].addr == 0x08000000UL && r[0].count == 4 && r[0].size == 16384u && r[0].type == 7);
    LT_CHECK(r[1].addr == 0x08010000UL && r[1].size == 65536UL);
    LT_CHECK(r[2].addr == 0x08020000UL && r[2].count == 7 && r[2].size == 131072UL);
    n = lt_fw_dfuse_layout("@Option Bytes  /0x1FFFC000/01*016 e", r, 8, NULL, 0);
    LT_CHECK_INT(n, 1);
    LT_CHECK(r[0].size == 16 && r[0].type == 5);
    n = lt_fw_dfuse_layout("@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg/0x08100000/04*016Kg", r, 8, NULL, 0);
    LT_CHECK_INT(n, 4);
    LT_CHECK(r[3].addr == 0x08100000UL);
    LT_CHECK_INT(lt_fw_dfuse_layout("Internal Flash/0x0800/1*1Kg", r, 8, NULL, 0), -1);
    LT_CHECK_INT(lt_fw_dfuse_layout("@x/0x08000000/04016Kg", r, 8, NULL, 0), -1);
    LT_CHECK_INT(lt_fw_dfuse_layout("@x/08000000/1*1Kg", r, 8, NULL, 0), -1);
    LT_CHECK_INT(lt_fw_dfuse_layout("@x/0xFFFFF000/2*4Kg", r, 8, NULL, 0), -1);
    LT_CHECK_INT(lt_fw_dfuse_layout(NULL, r, 8, NULL, 0), -1);
}

static double zero_clock(void *u) { LT_UNUSED(u); return 0.0; }

LT_TEST(descriptor_alts)
{
    lt_fw_sim_opts o;
    lt_fw_sim *s;
    lt_fw_io io;
    lt_u8 cfg[256];
    lt_fw_dfu_alt alts[8];
    int n, len;
    memset(&o, 0, sizeof(o));
    o.kind = LT_FW_SIM_DFUSE;
    s = lt_fw_sim_new(&o, zero_clock, NULL);
    lt_fw_sim_io(s, &io);
    len = io.config_desc(io.ctx, cfg, (int)sizeof(cfg));
    n = lt_fw_dfuse_alts(cfg, len, alts, 8);
    LT_CHECK_INT(n, 4);
    LT_CHECK(alts[0].iface == 0 && alts[0].alt == 0 && alts[0].istr == 4);
    LT_CHECK(alts[3].alt == 3 && alts[3].xfer == 2048 && alts[3].dfuse == 1);
    LT_CHECK_INT(lt_fw_dfuse_alts(cfg, 12, alts, 8), 0);
    lt_fw_sim_free(s);
}

LT_TEST(erase_plan)
{
    lt_fw_region r[4];
    lt_fw_image img;
    lt_u32 a[32], z[32];
    char err[200];
    int n, i;
    const lt_fw_preset *mp = lt_fw_preset_get(LT_FW_PRESET_MIATA);
    LT_CHECK_INT(lt_fw_dfuse_layout("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg", r, 4, NULL, 0), 3);
    lt_fw_sim_demo_image(&img, 1, 200);          /* 0x08000000 .. 0x0803A000 */
    n = lt_fw_dfuse_plan(r, 3, &img, mp->protect, mp->n_protect, a, z, 32, err, sizeof(err));
    LT_CHECK_INT(n, 6);                          /* 4 x 16K, 64K, 128K */
    LT_CHECK(a[0] == 0x08000000UL && a[4] == 0x08010000UL && z[4] == 0x10000UL && a[5] == 0x08020000UL);
    for (i = 0; i < n; i++) { LT_CHECK(a[i] < LT_FW_RUSEFI_CFG); }
    lt_fw_image_free(&img);
    /* an image reaching into the settings sectors */
    {
        lt_u8 d[16];
        memset(d, 0x11, sizeof(d));
        lt_fw_image_init(&img);
        (void)lt_fw_image_add(&img, 0x080BFFF8UL, d, 16, NULL, 0);
        (void)lt_fw_image_finish(&img, NULL, 0);
        LT_CHECK_INT(lt_fw_dfuse_plan(r, 3, &img, mp->protect, mp->n_protect, a, z, 32, err, sizeof(err)), -1);
        LT_CHECK(strstr(err, "settings") != NULL);
        LT_CHECK_INT(lt_fw_dfuse_plan(r, 3, &img, NULL, 0, a, z, 32, err, sizeof(err)), 2);
        LT_CHECK(a[1] == 0x080C0000UL);
        lt_fw_image_free(&img);
        lt_fw_image_init(&img);
        (void)lt_fw_image_add(&img, 0x080FFFF8UL, d, 16, NULL, 0);
        (void)lt_fw_image_finish(&img, NULL, 0);
        LT_CHECK_INT(lt_fw_dfuse_plan(r, 3, &img, NULL, 0, a, z, 32, err, sizeof(err)), -1);
        LT_CHECK(strstr(err, "outside") != NULL);
        lt_fw_image_free(&img);
    }
}

LT_TEST(presets)
{
    LT_CHECK_INT(lt_fw_preset_for_signature("rusEFI master.2025.01.01.miata-ecu.123"), LT_FW_PRESET_MIATA);
    LT_CHECK_INT(lt_fw_preset_for_signature("rusEFI 2025062409.2025.06.26.epicECU.2089451084"), LT_FW_PRESET_RUSEFI);
    LT_CHECK_INT(lt_fw_preset_for_signature("FOME 2024.uaefi"), LT_FW_PRESET_RUSEFI);
    LT_CHECK_INT(lt_fw_preset_for_signature("speeduino 202501"), LT_FW_PRESET_GENERIC);
    LT_CHECK_INT(lt_fw_preset_for_signature(NULL), LT_FW_PRESET_GENERIC);
    LT_CHECK(lt_fw_preset_get(LT_FW_PRESET_MIATA)->url != NULL);
    LT_CHECK(lt_fw_preset_get(LT_FW_PRESET_GENERIC)->n_protect == 0);
    LT_CHECK(lt_fw_usb_hint(LT_USB_EACCES, 1) != NULL && strstr(lt_fw_usb_hint(LT_USB_EACCES, 1), "Zadig") != NULL);
    LT_CHECK(strstr(lt_fw_udev_rule, "df11") != NULL);
}

int main(void)
{
    LT_RUN(ihex_basic);
    LT_RUN(ihex_errors);
    LT_RUN(srec_basic);
    LT_RUN(bin_and_names);
    LT_RUN(merge_out_of_order);
    LT_RUN(dfu_crc_value);
    LT_RUN(roundtrips);
    LT_RUN(dfuse_damage);
    LT_RUN(fuzz_truncations);
    LT_RUN(facts);
    LT_RUN(layout);
    LT_RUN(descriptor_alts);
    LT_RUN(erase_plan);
    LT_RUN(presets);
    LT_TEST_MAIN_END
}
