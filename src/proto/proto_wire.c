/* proto_wire.c - pure wire primitives: CRC, envelope, command builder,
 * response codes, Speeduino calibration encoders, rusEFI text parser.
 * Spec: docs/spec/03-protocol.md sections 3, 4, 5, 10, 11.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "proto.h"

#include <string.h>

lt_u32 lt_proto_crc32(const void *data, size_t n)
{
    return lt_crc32(0, data, n) & LT_U32_MASK;
}

/* ---- response codes ---- */
lt_bool lt_rc_is_ok(lt_u8 code)
{
    return (code <= 0x02 || (code >= 0x04 && code <= 0x06)) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_rc_is_error(lt_u8 code)
{
    return ((code & 0x80) != 0 || code == 0x03) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_rc_carries_message(lt_u8 code)
{
    return (code == 0x03 || code == 0x94 || code == 0x95) ? LT_TRUE : LT_FALSE;
}

const char *lt_rc_message(lt_u8 code)
{
    switch (code) {
    case 0x00: return "OK";
    case 0x01: return "OK (table response)";
    case 0x02: return "Burn complete";
    case 0x03: return "Controller refused settings (power cycle required to clear)";
    case 0x04: case 0x05: case 0x06: return "OK";
    case 0x80: return "Controller Reported an Under-run";
    case 0x81: return "Controller Reported an Over-run";
    case 0x82: return "Controller Reported a CRC Mismatch";
    case 0x83: return "Controller Reported a Unrecognized Command";
    case 0x84: return "Controller Reported a Out of Range";
    case 0x85: return "Controller reporting BUSY";
    case 0x86: return "Controller Reported Flash Locked";
    case 0x87: return "Controller Reported Sequence Failure 1";
    case 0x88: return "Controller Reported Sequence Failure 2";
    case 0x89: return "Controller Reported CAN Queue full";
    case 0x8A: return "Controller Reported CAN Timeout";
    case 0x8B: return "Controller Reported CAN Failure";
    case 0x8C: return "Controller Reported Parity Error";
    case 0x8D: return "Controller Reported Framing Error";
    case 0x8E: return "Controller Reported Serial Noise";
    case 0x8F: return "Controller Reported txmode range error";
    case 0x90: return "Controller Reported Unknown Serial Error";
    case 0x91: return "Too Many Bad Requests for unavailable CAN ID";
    case 0x92: return "A Controller is responding, but not at the project assigned CAN ID";
    case 0x93: return "High speed runtime table not set";
    case 0x94: return "Controller reported a generic error";
    case 0x95: return "Controller reported a critical error";
    default: break;
    }
    return "Controller reported an undefined error code";
}

/* ---- envelope ---- */
static void put_u16(lt_env_order o, lt_u8 *p, lt_u32 v)
{
    if (o == LT_ENV_LITTLE) {
        lt_wr_le(p, 2, v);
    } else {
        lt_wr_be(p, 2, v);
    }
}

static void put_u32(lt_env_order o, lt_u8 *p, lt_u32 v)
{
    if (o == LT_ENV_LITTLE) {
        lt_wr_le(p, 4, v);
    } else {
        lt_wr_be(p, 4, v);
    }
}

void lt_env_encode(lt_env_order order, const lt_u8 *payload, size_t n, lt_buf *out)
{
    lt_u8 hdr[2];
    lt_u8 crc[4];

    if (n > LT_MAX_PACKET_SIZE) {
        return;
    }
    put_u16(order, hdr, (lt_u32)n);
    put_u32(order, crc, lt_proto_crc32(payload, n));
    lt_buf_append(out, hdr, 2);
    if (n > 0) {
        lt_buf_append(out, payload, n);
    }
    lt_buf_append(out, crc, 4);
}

int lt_env_decode(lt_env_order order, lt_bool permissive, const lt_u8 *data, size_t len,
                  const lt_u8 **payload, size_t *plen, size_t *consumed)
{
    size_t n;
    lt_u32 recv, recv_be, recv_le, crc_a, crc_b, crc_c, crc_d;
    const lt_u8 *pl;
    lt_bool ok;

    if (len < 6) {
        lt_err_set("invalid response: frame too short (%lu bytes)", (unsigned long)len);
        return LT_EDATA;
    }
    n = (size_t)(order == LT_ENV_LITTLE ? lt_rd_le(data, 2) : lt_rd_be(data, 2));
    if (len < 2 + n + 4) {
        lt_err_set("invalid response: frame needs %lu bytes, have %lu", (unsigned long)(2 + n + 4),
                   (unsigned long)len);
        return LT_EDATA;
    }
    pl = data + 2;
    recv_be = lt_rd_be(pl + n, 4);
    recv_le = lt_rd_le(pl + n, 4);
    recv = (order == LT_ENV_LITTLE) ? recv_le : recv_be;
    crc_a = lt_proto_crc32(pl, n);
    ok = (recv == crc_a) ? LT_TRUE : LT_FALSE;
    if (!ok) {
        if (!permissive) {
            lt_err_set("CRC mismatch: expected 0x%08lx, got 0x%08lx", (unsigned long)crc_a,
                       (unsigned long)recv);
            return LT_ECRC;
        }
        crc_b = (n > 1) ? lt_proto_crc32(pl + 1, n - 1) : 0;
        crc_c = lt_crc32(lt_crc32(0, data, 2), pl, n) & LT_U32_MASK;
        crc_d = (n > 1) ? (lt_crc32(lt_crc32(0, data, 2), pl + 1, n - 1) & LT_U32_MASK) : 0;
        if (recv_le == crc_a || recv_be == crc_b || recv_le == crc_b || recv_be == crc_c ||
            recv_le == crc_c || recv_be == crc_d || recv_le == crc_d) {
            ok = LT_TRUE;
        }
        if (!ok) {
            lt_err_set("CRC mismatch: expected 0x%08lx, got 0x%08lx", (unsigned long)crc_a,
                       (unsigned long)recv_be);
            return LT_ECRC;
        }
    }
    *payload = pl;
    *plen = n;
    if (consumed) {
        *consumed = 2 + n + 4;
    }
    return LT_OK;
}

/* ---- command strings ---- */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Try to decode an escape at s[i] (s[i] == '\\'). Returns bytes consumed (0 =
 * not an escape) and stores the byte. */
static size_t escape_at(const char *s, size_t n, size_t i, lt_u8 *out)
{
    int c, h1, h2;

    if (i + 1 >= n) {
        return 0;
    }
    c = (unsigned char)s[i + 1];
    switch (c) {
    case 'x':
    case 'X':
        if (i + 3 < n) {
            h1 = hexval((unsigned char)s[i + 2]);
            h2 = hexval((unsigned char)s[i + 3]);
            if (h1 >= 0 && h2 >= 0) {
                *out = (lt_u8)(h1 * 16 + h2);
                return 4;
            }
        }
        return 0;
    case 'n': *out = 0x0A; return 2;
    case 'r': *out = 0x0D; return 2;
    case 't': *out = 0x09; return 2;
    case '\\': *out = 0x5C; return 2;
    case '0': *out = 0x00; return 2;
    default: break;
    }
    return 0;
}

void lt_cmd_parse_string(const char *s, size_t n, lt_buf *out)
{
    size_t i = 0, k;
    lt_u8 b;

    while (i < n) {
        if (s[i] == '\\') {
            k = escape_at(s, n, i, &b);
            if (k > 0) {
                lt_buf_append(out, &b, 1);
                i += k;
                continue;
            }
        }
        lt_buf_append(out, s + i, 1);
        i++;
    }
}

void lt_cmd_build(lt_buf *out, const char *fmt, size_t n, lt_u32 page_id, lt_u32 offset,
                  lt_u32 count, const lt_u8 *data, size_t ndata, lt_bool little_endian)
{
    size_t i = 0, k;
    lt_u8 b[2];
    lt_u32 v;

    while (i < n) {
        if (fmt[i] == '%') {
            if (i + 2 < n && fmt[i + 1] == '2' &&
                (fmt[i + 2] == 'i' || fmt[i + 2] == 'o' || fmt[i + 2] == 'c')) {
                v = fmt[i + 2] == 'i' ? page_id : (fmt[i + 2] == 'o' ? offset : count);
                if (little_endian) {
                    lt_wr_le(b, 2, v);
                } else {
                    lt_wr_be(b, 2, v);
                }
                lt_buf_append(out, b, 2);
                i += 3;
                continue;
            }
            if (i + 1 < n && fmt[i + 1] == 'v') {
                if (ndata > 0 && data) {
                    lt_buf_append(out, data, ndata);
                }
                i += 2;
                continue;
            }
        } else if (fmt[i] == '\\') {
            k = escape_at(fmt, n, i, &b[0]);
            if (k > 0) {
                lt_buf_append(out, &b[0], 1);
                i += k;
                continue;
            }
        }
        lt_buf_append(out, fmt + i, 1);
        i++;
    }
}

/* ---- Speeduino calibration (spec 11) ---- */
lt_s32 lt_cal_firmware_stored_temperature(lt_s32 raw_f10)
{
    lt_s32 v = (((raw_f10 / 10) - 32) * 5) / 9 + 40;

    return v < 0 ? 0 : v;
}

void lt_cal_encode_temperature(const double temps_c[32], lt_u8 out[64])
{
    int i, delta, best_delta;
    double f10, base, err, best_err, c;
    long raw, rb;
    lt_s32 stored;

    for (i = 0; i < 32; i++) {
        c = temps_c[i];
        f10 = (c * 9.0 / 5.0 + 32.0) * 10.0;
        if (!(f10 == f10)) {
            f10 = 0.0;   /* NaN input: treat as 0 */
        }
        base = lt_round(f10);
        if (base > 32767.0) {
            base = 32767.0;
        }
        if (base < -32768.0) {
            base = -32768.0;
        }
        rb = (long)base;
        best_delta = 0;
        best_err = 1e30;
        for (delta = -18; delta <= 18; delta++) {
            raw = rb + delta;
            if (raw > 32767) {
                raw = 32767;
            }
            if (raw < -32768) {
                raw = -32768;
            }
            stored = lt_cal_firmware_stored_temperature((lt_s32)raw);
            err = (double)stored - 40.0 - c;
            if (err < 0) {
                err = -err;
            }
            if (err < best_err) {
                best_err = err;
                best_delta = delta;
            }
        }
        raw = rb + best_delta;
        if (raw > 32767) {
            raw = 32767;
        }
        if (raw < -32768) {
            raw = -32768;
        }
        lt_wr_le(out + i * 2, 2, (lt_u32)((unsigned long)raw & 0xFFFFUL));
    }
}

void lt_cal_encode_o2(const double afr[1024], lt_u8 out[1024])
{
    int i;
    double v;

    for (i = 0; i < 1024; i++) {
        v = lt_round(afr[i] * 10.0);
        if (!(v == v) || v < 0.0) {
            v = 0.0;
        }
        if (v > 255.0) {
            v = 255.0;
        }
        out[i] = (lt_u8)v;
    }
}

void lt_cal_temperature_bins(lt_bool modern, double bins[32])
{
    int x;

    for (x = 0; x < 32; x++) {
        bins[x] = (double)(x * (modern ? 33 : 32));
    }
}

/* ---- rusEFI console text (spec 10) ---- */
static const char *const noise_prefix[] = {
    "wave_chart", "outpin", "t|d_", "map|u", "maf|u", "maf|d", "hpfp|d", "hpfp|u",
    "hpfp2|d", "pfp|u", "wave", "VVT|"
};

static void trim_range(const char *s, size_t n, size_t *b, size_t *e)
{
    size_t i = 0, j = n;

    while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        i++;
    }
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' ||
                     s[j - 1] == '\n')) {
        j--;
    }
    *b = i;
    *e = j;
}

static void add_line(lt_buf *out, const char *a, size_t an, const char *b, size_t bn,
                     lt_bool tagged)
{
    if (out->len > 0) {
        lt_buf_putc(out, '\n');
    }
    if (tagged) {
        lt_buf_putc(out, '[');
        lt_buf_append(out, a, an);
        lt_buf_puts(out, "] ");
    }
    lt_buf_append(out, b, bn);
}

char *lt_parse_rusefi_text_output(const char *raw)
{
    lt_buf out;
    const char **starts;
    size_t *lens;
    size_t np = 0, i, j, len, rb, re, tb, te, cb, ce, k;
    const char *p, *q;
    lt_bool skip, printable;

    lt_buf_init(&out);
    if (!raw || !raw[0]) {
        return lt_strdup("");
    }
    len = strlen(raw);
    /* split on backtick */
    starts = (const char **)lt_calloc(len + 2, sizeof(char *));
    lens = (size_t *)lt_calloc(len + 2, sizeof(size_t));
    p = raw;
    for (;;) {
        q = strchr(p, '`');
        starts[np] = p;
        if (!q) {
            lens[np] = strlen(p);
            np++;
            break;
        }
        lens[np] = (size_t)(q - p);
        np++;
        p = q + 1;
    }
    for (i = 0; i + 1 < np; i += 2) {
        trim_range(starts[i], lens[i], &tb, &te);
        trim_range(starts[i + 1], lens[i + 1], &cb, &ce);
        if (te == tb && lens[i + 1] == 0) {
            continue;
        }
        if (te - tb == 3 && (lt_strnicmp(starts[i] + tb, "msg", 3) == 0 ||
                             lt_strnicmp(starts[i] + tb, "emu", 3) == 0)) {
            if (ce > cb) {
                add_line(&out, NULL, 0, starts[i + 1] + cb, ce - cb, LT_FALSE);
            }
            continue;
        }
        skip = LT_FALSE;
        for (k = 0; k < LT_COUNTOF(noise_prefix); k++) {
            size_t pl = strlen(noise_prefix[k]);
            if (te - tb >= pl && strncmp(starts[i] + tb, noise_prefix[k], pl) == 0) {
                skip = LT_TRUE;
                break;
            }
        }
        if (skip) {
            continue;
        }
        if (te > tb && ce > cb && (ce - cb) > 2) {
            printable = LT_TRUE;
            for (j = cb; j < ce; j++) {
                unsigned char ch = (unsigned char)starts[i + 1][j];
                if (!(ch >= 0x20 && ch <= 0x7E)) {
                    printable = LT_FALSE;
                    break;
                }
            }
            if (printable) {
                add_line(&out, starts[i] + tb, te - tb, starts[i + 1] + cb, ce - cb, LT_TRUE);
            }
        }
    }
    if (out.len == 0) {
        trim_range(raw, len, &rb, &re);
        if (re > rb) {
            lt_buf_append(&out, raw + rb, re - rb);
        }
    }
    lt_free((void *)starts);
    lt_free(lens);
    if (out.len == 0) {
        lt_buf_free(&out);
        return lt_strdup("");
    }
    return lt_buf_take(&out);
}
