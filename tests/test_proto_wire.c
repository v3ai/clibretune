/* test_proto_wire.c - pure wire primitives, ECU memory shadow, discovery classify.
 * Vectors from docs/spec/03-protocol.md. */
#include "lt.h"
#include "proto/proto.h"
#include "lt_test.h"

#include <string.h>

static lt_u8 *bytes_of(lt_buf *b)
{
    return (lt_u8 *)b->data;
}

LT_TEST(crc32_check_value)
{
    lt_u8 d[288];
    lt_u32 a, b;
    int i;

    LT_CHECK(lt_proto_crc32("123456789", 9) == 0xCBF43926UL);
    LT_CHECK(lt_proto_crc32("", 0) == 0);
    for (i = 0; i < 288; i++) {
        d[i] = (lt_u8)(i % 251);
    }
    a = lt_proto_crc32(d, 288);
    d[144] ^= 1;
    b = lt_proto_crc32(d, 288);
    LT_CHECK(a != b);
}

LT_TEST(envelope_roundtrip_be)
{
    lt_buf f;
    lt_u8 p[1] = { 0x51 };
    const lt_u8 *pl;
    size_t pn, used;
    lt_u32 crc = lt_proto_crc32(p, 1);

    lt_buf_init(&f);
    lt_env_encode(LT_ENV_BIG, p, 1, &f);
    LT_CHECK(f.len == 7);
    LT_CHECK(bytes_of(&f)[0] == 0 && bytes_of(&f)[1] == 1 && bytes_of(&f)[2] == 0x51);
    LT_CHECK(lt_rd_be(bytes_of(&f) + 3, 4) == crc);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, &used) == 0);
    LT_CHECK(pn == 1 && pl[0] == 0x51 && used == 7);
    /* flipped byte -> error */
    { lt_u8 *fb = bytes_of(&f); fb[2] ^= 0x01; }
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, &used) ==
             LT_ECRC);
    LT_CHECK(strstr(lt_err_get(), "CRC mismatch: expected 0x") != NULL);
    lt_buf_free(&f);
}

LT_TEST(envelope_roundtrip_le)
{
    lt_buf f;
    lt_u8 p[1] = { 0x51 };
    lt_u8 rc[1] = { 0x82 };
    const lt_u8 *pl;
    size_t pn;

    lt_buf_init(&f);
    lt_env_encode(LT_ENV_LITTLE, p, 1, &f);
    LT_CHECK(bytes_of(&f)[0] == 0x01 && bytes_of(&f)[1] == 0x00 && bytes_of(&f)[2] == 0x51);
    LT_CHECK(lt_rd_le(bytes_of(&f) + 3, 4) == lt_proto_crc32(p, 1));
    LT_CHECK(lt_env_decode(LT_ENV_LITTLE, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, NULL) == 0);
    lt_buf_clear(&f);
    lt_env_encode(LT_ENV_LITTLE, rc, 1, &f);
    LT_CHECK(lt_env_decode(LT_ENV_LITTLE, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, NULL) == 0);
    LT_CHECK(pn == 1 && pl[0] == 0x82);
    lt_buf_free(&f);
}

LT_TEST(envelope_cross_order_misparses)
{
    lt_buf f;
    lt_u8 p[1] = { 0x51 };
    const lt_u8 *pl;
    size_t pn;

    lt_buf_init(&f);
    lt_env_encode(LT_ENV_BIG, p, 1, &f);
    /* BE frame read as LE gives N = 256 -> frame too short */
    LT_CHECK(lt_env_decode(LT_ENV_LITTLE, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, NULL) ==
             LT_EDATA);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, bytes_of(&f), 5, &pl, &pn, NULL) == LT_EDATA);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, bytes_of(&f), 6, &pl, &pn, NULL) == LT_EDATA);
    lt_buf_free(&f);
}

LT_TEST(envelope_permissive_scopes)
{
    lt_u8 payload[3] = { 0x00, 0x42, 0x43 };
    lt_u8 fr[2 + 3 + 4];
    lt_u8 lenb[2] = { 0, 3 };
    lt_u32 c;
    const lt_u8 *pl;
    size_t pn;

    /* scope C: BE length + payload */
    c = lt_crc32(lt_crc32(0, lenb, 2), payload, 3);
    fr[0] = 0;
    fr[1] = 3;
    memcpy(fr + 2, payload, 3);
    lt_wr_be(fr + 5, 4, c);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, fr, sizeof(fr), &pl, &pn, NULL) == LT_ECRC);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_TRUE, fr, sizeof(fr), &pl, &pn, NULL) == 0);
    LT_CHECK(pn == 3 && memcmp(pl, payload, 3) == 0);
    /* scope B: payload without status byte */
    c = lt_proto_crc32(payload + 1, 2);
    lt_wr_be(fr + 5, 4, c);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_TRUE, fr, sizeof(fr), &pl, &pn, NULL) == 0);
    /* scope D */
    c = lt_crc32(lt_crc32(0, lenb, 2), payload + 1, 2);
    lt_wr_le(fr + 5, 4, c);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_TRUE, fr, sizeof(fr), &pl, &pn, NULL) == 0);
    /* garbage fails even permissively */
    lt_wr_be(fr + 5, 4, 0x12345678UL);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_TRUE, fr, sizeof(fr), &pl, &pn, NULL) == LT_ECRC);
}

LT_TEST(envelope_empty_and_big_payload)
{
    lt_buf f;
    lt_u8 *big = (lt_u8 *)lt_malloc(65535);
    const lt_u8 *pl;
    size_t pn;

    memset(big, 0x5A, 65535);
    lt_buf_init(&f);
    lt_env_encode(LT_ENV_BIG, NULL, 0, &f);
    LT_CHECK(f.len == 6);
    LT_CHECK(lt_env_decode(LT_ENV_BIG, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, NULL) == 0);
    LT_CHECK(pn == 0);
    lt_buf_clear(&f);
    lt_env_encode(LT_ENV_LITTLE, big, 65535, &f);
    LT_CHECK(f.len == 65535 + 6);
    LT_CHECK(lt_env_decode(LT_ENV_LITTLE, LT_FALSE, bytes_of(&f), f.len, &pl, &pn, NULL) == 0);
    LT_CHECK(pn == 65535);
    lt_buf_clear(&f);
    lt_env_encode(LT_ENV_LITTLE, big, 65536UL, &f);   /* refused */
    LT_CHECK(f.len == 0);
    lt_buf_free(&f);
    lt_free(big);
}

LT_TEST(response_codes)
{
    int c;

    LT_CHECK(lt_rc_is_ok(0) && lt_rc_is_ok(1) && lt_rc_is_ok(2));
    LT_CHECK(!lt_rc_is_ok(3) && lt_rc_is_ok(4) && lt_rc_is_ok(5) && lt_rc_is_ok(6));
    LT_CHECK(!lt_rc_is_ok(7) && !lt_rc_is_error(7));
    LT_CHECK(lt_rc_is_error(3) && lt_rc_is_error(0x80) && lt_rc_is_error(0x95));
    LT_CHECK(!lt_rc_is_error(0) && !lt_rc_is_error(6));
    LT_CHECK(lt_rc_carries_message(3) && lt_rc_carries_message(0x94) &&
             lt_rc_carries_message(0x95));
    LT_CHECK(!lt_rc_carries_message(0x84) && !lt_rc_carries_message(0));
    LT_CHECK_STR(lt_rc_message(0x84), "Controller Reported a Out of Range");
    LT_CHECK_STR(lt_rc_message(0x00), "OK");
    LT_CHECK_STR(lt_rc_message(0x01), "OK (table response)");
    LT_CHECK_STR(lt_rc_message(0x02), "Burn complete");
    LT_CHECK_STR(lt_rc_message(0x85), "Controller reporting BUSY");
    LT_CHECK_STR(lt_rc_message(0x94), "Controller reported a generic error");
    LT_CHECK_STR(lt_rc_message(0xAB), "Controller reported an undefined error code");
    LT_CHECK_STR(lt_rc_message(0x07), "Controller reported an undefined error code");
    for (c = 0x80; c <= 0x95; c++) {
        LT_CHECK(strcmp(lt_rc_message((lt_u8)c), "Controller reported an undefined error code") !=
                 0);
    }
    LT_CHECK(strcmp(lt_rc_message(0x96), "Controller reported an undefined error code") == 0);
}

LT_TEST(cmd_parse_string_vectors)
{
    lt_buf b;

    lt_buf_init(&b);
    lt_cmd_parse_string("\\x0f", 4, &b);
    LT_CHECK(b.len == 1 && bytes_of(&b)[0] == 0x0F);
    lt_buf_clear(&b);
    lt_cmd_parse_string("\\x00\\x0f\\x14", 12, &b);
    LT_CHECK(b.len == 3 && bytes_of(&b)[0] == 0 && bytes_of(&b)[1] == 0x0F &&
             bytes_of(&b)[2] == 0x14);
    lt_buf_clear(&b);
    lt_cmd_parse_string("r\\x00\\x0f\\x00\\x00\\x00\\x14", 25, &b);
    LT_CHECK(b.len == 7 && bytes_of(&b)[0] == 'r' && bytes_of(&b)[2] == 0x0F &&
             bytes_of(&b)[6] == 0x14);
    lt_buf_clear(&b);
    lt_cmd_parse_string("\\n\\r\\t\\\\\\0", 10, &b);
    LT_CHECK(b.len == 5 && bytes_of(&b)[0] == 10 && bytes_of(&b)[1] == 13 &&
             bytes_of(&b)[2] == 9 && bytes_of(&b)[3] == 0x5C && bytes_of(&b)[4] == 0);
    lt_buf_clear(&b);
    lt_cmd_parse_string("Hello", 5, &b);
    LT_CHECK(b.len == 5 && memcmp(b.data, "Hello", 5) == 0);
    lt_buf_clear(&b);
    lt_cmd_parse_string("\\x4", 3, &b);        /* too short: literal */
    LT_CHECK(b.len == 3 && memcmp(b.data, "\\x4", 3) == 0);
    lt_buf_clear(&b);
    lt_cmd_parse_string("\\xZZ1", 5, &b);      /* bad hex: literal backslash */
    LT_CHECK(b.len == 5);
    lt_buf_clear(&b);
    lt_cmd_parse_string("a\\q", 3, &b);
    LT_CHECK(b.len == 3);
    lt_buf_free(&b);
}

static void build(lt_buf *b, const char *fmt, size_t fl, lt_u32 pg, lt_u32 off, lt_u32 cnt,
                  const lt_u8 *d, size_t nd, lt_bool le)
{
    lt_buf_clear(b);
    lt_cmd_build(b, fmt, fl, pg, off, cnt, d, nd, le);
}

LT_TEST(cmd_build_vectors)
{
    lt_buf b;
    static const lt_u8 data[3] = { 0xAA, 0xBB, 0xCC };
    static const lt_u8 e1[] = { 0x72, 0x00, 0x30, 0x00, 0x00, 0x82, 0x00 };
    static const lt_u8 e2[] = { 0x72, 0x00, 0x06, 0x02, 0x04, 0x00, 0x10 };
    static const lt_u8 e3[] = { 0x52, 0x00, 0x00, 0x00, 0x64, 0x01, 0x00 };
    static const lt_u8 e4[] = { 0x52, 0x01, 0x00, 0x00, 0x01, 0x00, 0x02 };
    static const lt_u8 e5[] = { 0x43, 0x00, 0x00, 0x00, 0x32, 0x00, 0x03, 0xAA, 0xBB, 0xCC };
    static const lt_u8 e6[] = { 0x42, 0x00, 0x00 };
    const char *f;

    lt_buf_init(&b);
    f = "r\\0\\x30%2o%2c";
    build(&b, f, strlen(f), 0, 0, 130, NULL, 0, LT_TRUE);
    LT_CHECK(b.len == sizeof(e1) && memcmp(b.data, e1, sizeof(e1)) == 0);
    f = "r\\x00\\x06%2o%2c";
    build(&b, f, strlen(f), 0, 0x0204, 16, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == sizeof(e2) && memcmp(b.data, e2, sizeof(e2)) == 0);
    f = "R%2i%2o%2c";
    build(&b, f, strlen(f), 0, 100, 256, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == sizeof(e3) && memcmp(b.data, e3, sizeof(e3)) == 0);
    build(&b, f, strlen(f), 1, 0x100, 0x200, NULL, 0, LT_TRUE);
    LT_CHECK(b.len == sizeof(e4) && memcmp(b.data, e4, sizeof(e4)) == 0);
    f = "C%2i%2o%2c%v";
    build(&b, f, strlen(f), 0, 50, 3, data, 3, LT_FALSE);
    LT_CHECK(b.len == sizeof(e5) && memcmp(b.data, e5, sizeof(e5)) == 0);
    f = "B%2i";
    build(&b, f, strlen(f), 0, 0, 0, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == sizeof(e6) && memcmp(b.data, e6, sizeof(e6)) == 0);
    build(&b, "", 0, 0, 0, 0, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == 0);
    /* real NUL byte standing for $tsCanId then a 4 char \x30 */
    build(&b, "r\0\\x30%2o%2c", 12, 0, 0, 130, NULL, 0, LT_TRUE);
    LT_CHECK(b.len == 7 && memcmp(b.data, e1, 7) == 0);
    /* %2 followed by other letter: literal '%', then '2', then 'x' */
    build(&b, "%2x", 3, 1, 2, 3, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == 3 && memcmp(b.data, "%2x", 3) == 0);
    /* %v with no data, trailing lone % and \ */
    build(&b, "A%v%", 4, 0, 0, 0, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == 2 && b.data[0] == 'A' && b.data[1] == '%');
    build(&b, "\\", 1, 0, 0, 0, NULL, 0, LT_FALSE);
    LT_CHECK(b.len == 1 && b.data[0] == '\\');
    lt_buf_free(&b);
}

LT_TEST(signature_from_payload_vectors)
{
    char s[64];
    static const lt_u8 e1[] = { 0x80 };
    static const lt_u8 e2[] = { 0x84, 'x' };
    static const lt_u8 e3[] = { 0x03, 'n', 'o' };
    static const lt_u8 e4[] = { 0x00 };

    LT_CHECK(!lt_proto_signature_from_payload(e1, 1, s, sizeof(s)));
    LT_CHECK(!lt_proto_signature_from_payload(e2, 2, s, sizeof(s)));
    LT_CHECK(!lt_proto_signature_from_payload(e3, 3, s, sizeof(s)));
    LT_CHECK(!lt_proto_signature_from_payload(e4, 1, s, sizeof(s)));
    LT_CHECK(!lt_proto_signature_from_payload(e4, 0, s, sizeof(s)));
    LT_CHECK(!lt_proto_signature_from_payload((const lt_u8 *)"   ", 3, s, sizeof(s)));
    LT_CHECK(lt_proto_signature_from_payload((const lt_u8 *)"\0speeduino 202501.4 ", 20, s,
                                             sizeof(s)));
    LT_CHECK_STR(s, "speeduino 202501.4");
    LT_CHECK(lt_proto_signature_from_payload((const lt_u8 *)"rusEFI master.2024 ", 19, s,
                                             sizeof(s)));
    LT_CHECK_STR(s, "rusEFI master.2024");
}

LT_TEST(write_status_and_strip_status)
{
    char m[64];
    int code = -1;
    static const lt_u8 a1[] = { 0x84 };
    static const lt_u8 a2[] = { 0x94, 'f', 'l', 'a', 's', 'h', ' ', 'w' };
    static const lt_u8 a3[] = { 0x00, 0xAB, 0xCD };
    static const lt_u8 a4[] = { 0x83, 'z' };
    lt_u8 buf[140];
    size_t dl = 99;

    LT_CHECK(lt_proto_check_write_status(a1, 1, m, sizeof(m), &code) == LT_EPROTO);
    LT_CHECK(code == 0x84);
    LT_CHECK_STR(m, "Controller Reported a Out of Range");
    LT_CHECK(lt_proto_check_write_status(a2, 8, m, sizeof(m), &code) == LT_EPROTO);
    LT_CHECK_STR(m, "flash w");
    LT_CHECK(lt_proto_check_write_status(a4, 2, m, sizeof(m), &code) == LT_EPROTO);
    LT_CHECK_STR(m, "Controller Reported a Unrecognized Command");
    LT_CHECK(lt_proto_check_write_status(a3, 0, m, sizeof(m), &code) == 0);
    LT_CHECK(lt_proto_check_write_status(a3, 1, m, sizeof(m), &code) == 0);
    LT_CHECK(lt_proto_check_write_status(a3, 3, m, sizeof(m), &code) == 0);

    memset(buf, 0xAB, sizeof(buf));
    LT_CHECK(lt_proto_strip_status(buf, 128, 130, "OCH", &dl) == LT_EPROTO);
    LT_CHECK(strstr(lt_err_get(), "OCH response: 128 bytes, expected 130") != NULL);
    buf[0] = 0;
    LT_CHECK(lt_proto_strip_status(buf, 101, 130, "OCH", &dl) == LT_EPROTO);
    LT_CHECK(lt_proto_strip_status(buf, 0, 130, "OCH", &dl) == LT_EPROTO);
    LT_CHECK(lt_proto_strip_status(buf, 131, 130, "OCH", &dl) == 1 && dl == 130);
    LT_CHECK(lt_proto_strip_status(buf, 130, 130, "OCH", &dl) == 0 && dl == 130);
    buf[0] = 9;
    LT_CHECK(lt_proto_strip_status(buf, 131, 130, "OCH", &dl) == 1 && dl == 130); /* warn only */
    {
        static const lt_u8 p1[] = { 0x00, 0x01, 0x02 };
        static const lt_u8 p2[] = { 0x09, 0x01, 0x02 };

        LT_CHECK(lt_proto_strip_status(p1, 3, 0, "x", &dl) == 1 && dl == 2);
        LT_CHECK(lt_proto_strip_status(p2, 3, 0, "x", &dl) == 0 && dl == 3);
        LT_CHECK(lt_proto_strip_status(p2, 0, 0, "x", &dl) == 0 && dl == 0);
    }
}

LT_TEST(response_complete_vectors)
{
    LT_CHECK(!lt_proto_response_complete(0, 0));
    LT_CHECK(!lt_proto_response_complete(130, 0));
    LT_CHECK(!lt_proto_response_complete(50, 0));
    LT_CHECK(!lt_proto_response_complete(129, 130));
    LT_CHECK(lt_proto_response_complete(130, 130));
    LT_CHECK(lt_proto_response_complete(131, 130));
    LT_CHECK(!lt_proto_response_complete(1, 130));
}

LT_TEST(calibration_encoders)
{
    double t[32], bins[32], afr[1024];
    lt_u8 w[64];
    lt_u8 o2[1024];
    int c, i;
    long raw;

    LT_CHECK(lt_cal_firmware_stored_temperature(2120) == 140);   /* 100C + 40 */
    LT_CHECK(lt_cal_firmware_stored_temperature(-400) == 0);
    for (i = 0; i < 32; i++) {
        t[i] = 100.0;
    }
    lt_cal_encode_temperature(t, w);
    raw = (short)lt_rd_le(w, 2);
    LT_CHECK(raw >= 2120 - 18 && raw <= 2120 + 18);
    for (i = 0; i < 32; i++) {
        t[i] = -40.0;
    }
    lt_cal_encode_temperature(t, w);
    raw = (short)lt_rd_le(w, 2);
    LT_CHECK(raw >= -400 - 18 && raw <= -400 + 18);
    /* whole degree round trip exactness, -40..210 */
    for (c = -40; c <= 210; c++) {
        t[0] = (double)c;
        for (i = 1; i < 32; i++) {
            t[i] = 20.0;
        }
        lt_cal_encode_temperature(t, w);
        raw = (short)lt_rd_le(w, 2);
        if (lt_cal_firmware_stored_temperature((lt_s32)raw) != (c + 40 > 0 ? c + 40 : 0)) {
            LT_CHECK(0);
        }
    }
    LT_CHECK(1);
    /* tenths within 0.5 degC */
    for (c = -400; c <= 2100; c += 7) {
        double v = (double)c / 10.0, back;

        t[0] = v;
        lt_cal_encode_temperature(t, w);
        raw = (short)lt_rd_le(w, 2);
        back = (double)lt_cal_firmware_stored_temperature((lt_s32)raw) - 40.0;
        if (!(back - v <= 0.5 + 1e-9 && v - back <= 0.5 + 1e-9)) {
            LT_CHECK(0);
        }
    }
    for (i = 0; i < 1024; i++) {
        afr[i] = 14.7;
    }
    afr[0] = 9.0;
    afr[1] = 100.0;
    afr[2] = -1.0;
    lt_cal_encode_o2(afr, o2);
    LT_CHECK(o2[0] == 90 && o2[1] == 255 && o2[2] == 0 && o2[3] == 147);
    lt_cal_temperature_bins(LT_FALSE, bins);
    LT_CHECK(bins[0] == 0 && bins[31] == 992);
    lt_cal_temperature_bins(LT_TRUE, bins);
    LT_CHECK(bins[31] == 1023);
    LT_CHECK(LT_CAL_CLT == 0 && LT_CAL_IAT == 1 && LT_CAL_O2 == 2);
}

LT_TEST(rusefi_text_parser)
{
    char *s;

    s = lt_parse_rusefi_text_output("");
    LT_CHECK_STR(s, "");
    lt_free(s);
    s = lt_parse_rusefi_text_output("msg`Hello from ECU`");
    LT_CHECK_STR(s, "Hello from ECU");
    lt_free(s);
    s = lt_parse_rusefi_text_output("msg`one`msg`two`msg`three`");
    LT_CHECK_STR(s, "one\ntwo\nthree");
    lt_free(s);
    s = lt_parse_rusefi_text_output(
        "msg`RPM=1200`wave_chart`some chart data here`msg`emu: running`");
    LT_CHECK_STR(s, "RPM=1200\nemu: running");
    lt_free(s);
    s = lt_parse_rusefi_text_output("msg`Status OK`t|d_123`456`msg`Done`");
    LT_CHECK_STR(s, "Status OK\nDone");
    lt_free(s);
    s = lt_parse_rusefi_text_output("Some plain text response");
    LT_CHECK_STR(s, "Some plain text response");
    lt_free(s);
    s = lt_parse_rusefi_text_output("emu`RPM=1200`emu`shape update for ch0`");
    LT_CHECK_STR(s, "RPM=1200\nshape update for ch0");
    lt_free(s);
    s = lt_parse_rusefi_text_output("foo`bar baz`");
    LT_CHECK_STR(s, "[foo] bar baz");
    lt_free(s);
    s = lt_parse_rusefi_text_output("MSG`Upper`");
    LT_CHECK_STR(s, "Upper");
    lt_free(s);
    s = lt_parse_rusefi_text_output("  trailing  ");
    LT_CHECK_STR(s, "trailing");
    lt_free(s);
    s = lt_parse_rusefi_text_output("msg`Boot v1`wave_chart`x y z w`outpin`aaaa`msg`Ready`"
                                    "wave`junk data`");
    LT_CHECK_STR(s, "Boot v1\nReady");
    lt_free(s);
}

LT_TEST(ports_sort)
{
    char *n[6];
    int i;
    static const char *const in[] = { "/dev/ttyUSB1", "/dev/ttyACM1", "/dev/ttyUSB0",
                                      "/dev/ttyACM0", "someport", "/dev/ttyACM10" };
    static const char *const want[] = { "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM10",
                                        "/dev/ttyUSB0", "/dev/ttyUSB1", "someport" };

    for (i = 0; i < 6; i++) {
        n[i] = lt_strdup(in[i]);
    }
    lt_ports_sort(n, 6);
    for (i = 0; i < 6; i++) {
        LT_CHECK_STR(n[i], want[i]);
        lt_free(n[i]);
    }
}

LT_TEST(disc_classify_vectors)
{
    char sig[96];
    static const lt_u8 boot[] = { 0xE0, 0x05, 0x3E };
    static const lt_u8 bs3[] = { 0x01, 0xAA, 0xBB, 0xCC };
    static const lt_u8 gps[] = "$GPGGA,123519,4807.038";
    static const lt_u8 echo[] = { 'Q', 'Q', 'Q', 'Q' };
    static const lt_u8 junk[] = { 0x80, 0x81, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96 };

    LT_CHECK(lt_disc_classify((const lt_u8 *)"Speeduino 2024.10  ", 19, 'S', sig, sizeof(sig)) ==
             LT_DISC_SIGNATURE);
    LT_CHECK_STR(sig, "Speeduino 2024.10");
    LT_CHECK(lt_disc_classify(boot, 3, 'Q', sig, sizeof(sig)) == LT_DISC_BOOTLOADER);
    LT_CHECK(lt_disc_classify(bs3, 4, 'Q', sig, sizeof(sig)) == LT_DISC_BIGSTUFF3);
    LT_CHECK(lt_disc_classify(gps, 22, 'Q', sig, sizeof(sig)) == LT_DISC_GPS);
    LT_CHECK(lt_disc_classify(echo, 4, 'Q', sig, sizeof(sig)) == LT_DISC_ECHO);
    LT_CHECK(lt_disc_classify(echo, 4, 'S', sig, sizeof(sig)) == LT_DISC_UNKNOWN);
    LT_CHECK(lt_disc_classify(junk, 9, 'Q', sig, sizeof(sig)) == LT_DISC_UNKNOWN);
    /* short printable text is not a signature (needs >= 8 bytes) */
    LT_CHECK(lt_disc_classify((const lt_u8 *)"abcdefg", 7, 'Q', sig, sizeof(sig)) ==
             LT_DISC_UNKNOWN);
}

/* ---- ECU memory shadow ---- */
LT_TEST(mem_basic_load_write_read)
{
    unsigned long sizes[2] = { 256, 64 };
    lt_ecu_mem *m = lt_ecu_mem_new(sizes, 2);
    lt_u8 img[256];
    static const lt_u8 w[4] = { 1, 2, 3, 4 };
    const lt_u8 *r;
    size_t len = 0;
    int i;
    int pages[4];

    for (i = 0; i < 256; i++) {
        img[i] = (lt_u8)i;
    }
    LT_CHECK(lt_ecu_mem_page_count(m) == 2);
    LT_CHECK(lt_ecu_mem_page_size(m, 0) == 256 && lt_ecu_mem_page_size(m, 5) == 0);
    LT_CHECK(lt_ecu_mem_state(m, 0) == LT_PAGE_NOT_LOADED);
    LT_CHECK(lt_ecu_mem_page(m, 0, &len) == NULL);
    LT_CHECK(lt_ecu_mem_write(m, 0, 0, w, 4) == LT_ENOENT);
    LT_CHECK(!lt_ecu_mem_fully_loaded(m));
    LT_CHECK(lt_ecu_mem_pages_to_load(m, pages, 4) == 2 && pages[0] == 0 && pages[1] == 1);
    lt_ecu_mem_load_page(m, 0, img, 256);
    LT_CHECK(lt_ecu_mem_state(m, 0) == LT_PAGE_CLEAN);
    LT_CHECK(lt_ecu_mem_page(m, 0, &len) != NULL && len == 256);
    LT_CHECK(lt_ecu_mem_write(m, 0, 10, w, 4) == 0);
    r = lt_ecu_mem_read(m, 0, 10, 4);
    LT_CHECK(r && memcmp(r, w, 4) == 0);
    LT_CHECK(lt_ecu_mem_state(m, 0) == LT_PAGE_DIRTY);
    LT_CHECK(lt_ecu_mem_is_dirty(m, 0, 10) && lt_ecu_mem_is_dirty(m, 0, 13));
    LT_CHECK(!lt_ecu_mem_is_dirty(m, 0, 14) && !lt_ecu_mem_is_dirty(m, 0, 9));
    LT_CHECK(lt_ecu_mem_dirty_bytes(m) == 4 && lt_ecu_mem_has_dirty(m));
    /* bounds: no partial writes, never grows */
    LT_CHECK(lt_ecu_mem_write(m, 0, 254, w, 4) == LT_ERANGE);
    LT_CHECK(lt_ecu_mem_read(m, 0, 254, 4) == NULL);
    LT_CHECK(lt_ecu_mem_read(m, 0, 252, 4) != NULL);
    LT_CHECK(lt_ecu_mem_dirty_bytes(m) == 4);
    LT_CHECK(lt_ecu_mem_write(m, 9, 0, w, 1) == LT_ENOENT);
    LT_CHECK(lt_ecu_mem_dirty_pages(m, pages, 4) == 1 && pages[0] == 0);
    lt_ecu_mem_free(m);
}

LT_TEST(mem_ranges_pending_burn_revert)
{
    unsigned long sizes[3] = { 32, 32, 32 };
    lt_ecu_mem *m = lt_ecu_mem_new(sizes, 3);
    lt_u8 img[32];
    static const lt_u8 w[3] = { 9, 9, 9 };
    lt_range rg[8];
    size_t n;
    int pages[4];
    lt_pagesrc ps;
    const lt_u8 *v;
    size_t len;

    memset(img, 0, sizeof(img));
    lt_ecu_mem_load_page(m, 0, img, 32);
    lt_ecu_mem_load_page(m, 1, img, 32);
    lt_ecu_mem_load_page(m, 2, img, 32);
    LT_CHECK(lt_ecu_mem_fully_loaded(m));
    lt_ecu_mem_write(m, 1, 2, w, 3);
    lt_ecu_mem_write(m, 1, 5, w, 1);     /* adjacent: merges into 2..5 */
    lt_ecu_mem_write(m, 1, 10, w, 2);
    lt_ecu_mem_write(m, 1, 31, w, 1);    /* last byte */
    n = lt_ecu_mem_dirty_ranges(m, 1, rg, 8);
    LT_CHECK(n == 3);
    LT_CHECK(rg[0].start == 2 && rg[0].len == 4);
    LT_CHECK(rg[1].start == 10 && rg[1].len == 2);
    LT_CHECK(rg[2].start == 31 && rg[2].len == 1);
    LT_CHECK(lt_ecu_mem_dirty_ranges(m, 0, rg, 8) == 0);
    LT_CHECK(lt_ecu_mem_dirty_ranges(m, 1, NULL, 0) == 3);
    /* pagesrc writes mark dirty */
    ps = lt_ecu_mem_pagesrc(m);
    LT_CHECK(ps.write(ps.ud, 2, 0, w, 2) == 0);
    v = ps.page(ps.ud, 2, &len);
    LT_CHECK(v && len == 32 && v[0] == 9);
    LT_CHECK(lt_ecu_mem_dirty_pages(m, pages, 4) == 2 && pages[0] == 1 && pages[1] == 2);
    lt_ecu_mem_mark_pending(m);
    LT_CHECK(!lt_ecu_mem_has_dirty(m));
    LT_CHECK(lt_ecu_mem_state(m, 1) == LT_PAGE_PENDING && lt_ecu_mem_state(m, 0) == LT_PAGE_CLEAN);
    LT_CHECK(lt_ecu_mem_has_pending_burn(m));
    lt_ecu_mem_mark_burned(m);
    LT_CHECK(lt_ecu_mem_state(m, 1) == LT_PAGE_CLEAN && !lt_ecu_mem_has_pending_burn(m));
    /* revert: dirty pages must be re-read */
    lt_ecu_mem_write(m, 0, 0, w, 1);
    lt_ecu_mem_revert(m);
    LT_CHECK(!lt_ecu_mem_has_dirty(m));
    LT_CHECK(lt_ecu_mem_state(m, 0) == LT_PAGE_NOT_LOADED);
    LT_CHECK(lt_ecu_mem_state(m, 1) == LT_PAGE_CLEAN);
    LT_CHECK(lt_ecu_mem_page(m, 0, &len) == NULL);
    lt_ecu_mem_set_state(m, 2, LT_PAGE_ERROR);
    LT_CHECK(lt_ecu_mem_state(m, 2) == LT_PAGE_ERROR && !lt_ecu_mem_fully_loaded(m));
    lt_ecu_mem_free(m);
    m = lt_ecu_mem_new(NULL, 0);
    LT_CHECK(lt_ecu_mem_page_count(m) == 0 && lt_ecu_mem_fully_loaded(m));
    lt_ecu_mem_free(m);
}

int main(void)
{
    LT_RUN(crc32_check_value);
    LT_RUN(envelope_roundtrip_be);
    LT_RUN(envelope_roundtrip_le);
    LT_RUN(envelope_cross_order_misparses);
    LT_RUN(envelope_permissive_scopes);
    LT_RUN(envelope_empty_and_big_payload);
    LT_RUN(response_codes);
    LT_RUN(cmd_parse_string_vectors);
    LT_RUN(cmd_build_vectors);
    LT_RUN(signature_from_payload_vectors);
    LT_RUN(write_status_and_strip_status);
    LT_RUN(response_complete_vectors);
    LT_RUN(calibration_encoders);
    LT_RUN(rusefi_text_parser);
    LT_RUN(ports_sort);
    LT_RUN(disc_classify_vectors);
    LT_RUN(mem_basic_load_write_read);
    LT_RUN(mem_ranges_pending_burn_revert);
    LT_TEST_MAIN_END
}
