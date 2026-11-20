/* fw_sim.c - simulated bootloaders behind lt_fw_io, for the tests and the demo mode:
 *   DfuSe   an STM32F407 ROM bootloader: DFU state machine, sector map in the interface
 *           strings, erase/program times reported through bwPollTimeout (polling early is
 *           counted), flash semantics (programming needs erased bytes), injected stalls and
 *           error statuses, UPLOAD read back, leave (manifest) = the device goes away;
 *   OpenBLT an XCP slave on bulk endpoints 0x01/0x81 with the bootloader area protected,
 *           a delayed PROGRAM_CLEAR answer and optional seed/key protection.
 * Time comes from the caller's clock so tests can run on a virtual clock.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw.h"

#define SIM_BASE ((lt_u32)0x08000000UL)
#define SIM_SIZE ((lt_u32)0x100000UL)
#define SIM_XFER 2048
#define SIM_LOADER_END ((lt_u32)0x08008000UL)

/* DFU states and status codes */
#define S_IDLE 2
#define S_DNLOAD_SYNC 3
#define S_DNBUSY 4
#define S_DNLOAD_IDLE 5
#define S_MANIFEST_SYNC 6
#define S_MANIFEST 7
#define S_UPLOAD_IDLE 9
#define S_ERROR 10
#define E_TARGET 0x01
#define E_PROG 0x06
#define E_ADDRESS 0x08
#define E_STALLED 0x0F

struct lt_fw_sim {
    lt_fw_sim_opts o;
    lt_fw_clock_fn clock;
    void *cu;
    lt_u8 *flash;
    int alt, left;
    /* DfuSe */
    int state, status;
    lt_u32 ptr, poll;
    double busy_until;
    lt_u8 pbuf[SIM_XFER];
    int plen;
    lt_u16 pblock;
    int n_dnload, n_busyend, early, erases;
    /* OpenBLT */
    lt_u8 resp[300];
    int resp_len, resp_pos;
    double resp_at;
    int connected, pgm;
    lt_u32 mta;
};

static double now(const lt_fw_sim *s) { return s->clock != NULL ? s->clock(s->cu) : 0.0; }

/* STM32F407: 4 x 16K, 1 x 64K, 7 x 128K */
static int sector_of(lt_u32 addr, lt_u32 *start, lt_u32 *size)
{
    lt_u32 off;
    if (addr < SIM_BASE || addr - SIM_BASE >= SIM_SIZE) { return -1; }
    off = addr - SIM_BASE;
    if (off < (lt_u32)0x10000UL) {
        *size = 0x4000u;
        *start = SIM_BASE + off / 0x4000u * 0x4000u;
        return (int)(off / 0x4000u);
    }
    if (off < (lt_u32)0x20000UL) { *size = (lt_u32)0x10000UL; *start = SIM_BASE + (lt_u32)0x10000UL; return 4; }
    *size = (lt_u32)0x20000UL;
    *start = SIM_BASE + off / *size * *size;
    return (int)(4u + off / *size);
}

static int in_flash(lt_u32 addr, lt_u32 n)
{
    return addr >= SIM_BASE && addr - SIM_BASE <= SIM_SIZE && n <= SIM_SIZE - (addr - SIM_BASE);
}

static void erase_sector(lt_fw_sim *s, lt_u32 addr)
{
    lt_u32 st = 0, sz = 0;
    if (sector_of(addr, &st, &sz) < 0) { return; }
    memset(s->flash + (st - SIM_BASE), 0xFF, sz);
    s->erases++;
}

/* program n bytes: only erased bytes can be written (as on real flash) */
static int program(lt_fw_sim *s, lt_u32 addr, const lt_u8 *d, lt_u32 n)
{
    lt_u32 i;
    if (!in_flash(addr, n)) { return E_ADDRESS; }
    for (i = 0; i < n; i++) {
        if (s->flash[addr - SIM_BASE + i] != 0xFF && s->flash[addr - SIM_BASE + i] != d[i]) { return E_PROG; }
    }
    memcpy(s->flash + (addr - SIM_BASE), d, n);
    return 0;
}

/* ------------------------------------------------------------------ DfuSe */
static const char *const dfu_strings[4] = {
    "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg",
    "@Option Bytes  /0x1FFFC000/01*016 e",
    "@OTP Memory /0x1FFF7800/01*512 e,01*016 e",
    "@Device Feature/0xFFFF0000/01*004 e"
};

static int sim_config(void *ctx, lt_u8 *buf, int cap)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    lt_u8 d[64];
    int n = 0, a;
    if (s->left) { return LT_USB_ENODEV; }
    d[n++] = 9; d[n++] = 2; d[n++] = 0; d[n++] = 0; d[n++] = 1; d[n++] = 1; d[n++] = 0; d[n++] = 0xC0; d[n++] = 0x32;
    if (s->o.kind == LT_FW_SIM_DFUSE) {
        for (a = 0; a < 4; a++) {
            d[n++] = 9; d[n++] = 4; d[n++] = 0; d[n++] = (lt_u8)a; d[n++] = 0;
            d[n++] = 0xFE; d[n++] = 0x01; d[n++] = 0x02; d[n++] = (lt_u8)(4 + a);
        }
        d[n++] = 9; d[n++] = 0x21; d[n++] = 0x0B; d[n++] = 0xFF; d[n++] = 0x00;
        d[n++] = (lt_u8)(SIM_XFER & 0xFF); d[n++] = (lt_u8)(SIM_XFER >> 8); d[n++] = 0x1A; d[n++] = 0x01;
    } else {
        d[n++] = 9; d[n++] = 4; d[n++] = 0; d[n++] = 0; d[n++] = 2; d[n++] = 0xFF; d[n++] = 0; d[n++] = 0; d[n++] = 0;
        d[n++] = 7; d[n++] = 5; d[n++] = 0x81; d[n++] = 2; d[n++] = 64; d[n++] = 0; d[n++] = 0;
        d[n++] = 7; d[n++] = 5; d[n++] = 0x01; d[n++] = 2; d[n++] = 64; d[n++] = 0; d[n++] = 0;
    }
    d[2] = (lt_u8)n;
    if (n > cap) { n = cap; }
    memcpy(buf, d, (size_t)n);
    return n;
}

static int sim_string(void *ctx, int index, char *buf, size_t cap)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    const char *t = "";
    if (s->left) { return LT_USB_ENODEV; }
    if (s->o.kind == LT_FW_SIM_DFUSE && index >= 4 && index < 8) { t = dfu_strings[index - 4]; }
    else if (index == 2) { t = s->o.kind == LT_FW_SIM_DFUSE ? "STM32  BOOTLOADER" : "OpenBLT User Program"; }
    else { return LT_USB_EPIPE; }
    lt_strlcpy(buf, t, cap);
    return (int)strlen(buf);
}

static int sim_claim(void *ctx, int iface, int alt)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    if (s->left) { return LT_USB_ENODEV; }
    if (iface != 0) { return LT_USB_EINVAL; }
    if (alt >= 0) { s->alt = alt; }
    return 0;
}

static int dfu_stall(lt_fw_sim *s)
{
    s->state = S_ERROR;
    s->status = E_STALLED;
    return LT_USB_EPIPE;
}

/* run the operation of the last DNLOAD; returns a status code */
static int dfu_execute(lt_fw_sim *s)
{
    if (s->pblock == 0) {
        if (s->plen == 5 && s->pbuf[0] == 0x21) {
            lt_u32 a = lt_load_u32le(s->pbuf + 1);
            if (!in_flash(a, 1)) { return E_TARGET; }
            s->ptr = a;
            return 0;
        }
        if (s->plen == 5 && s->pbuf[0] == 0x41) {
            lt_u32 a = lt_load_u32le(s->pbuf + 1), st = 0, sz = 0;
            if (sector_of(a, &st, &sz) < 0) { return E_TARGET; }
            erase_sector(s, a);
            return 0;
        }
        if (s->plen == 1 && s->pbuf[0] == 0x41) {
            memset(s->flash, 0xFF, SIM_SIZE);
            s->erases += 12;
            return 0;
        }
        return E_STALLED;
    }
    return program(s, s->ptr + (lt_u32)(s->pblock - 2) * (lt_u32)s->plen, s->pbuf, (lt_u32)s->plen);
}

static lt_u32 dfu_op_ms(const lt_fw_sim *s)
{
    if (s->pblock == 0 && s->plen == 5 && s->pbuf[0] == 0x41) {
        lt_u32 st = 0, sz = 0;
        if (sector_of(lt_load_u32le(s->pbuf + 1), &st, &sz) < 0) { return 1; }
        return (sz / 0x4000u) * s->o.erase_ms_16k + 1u;
    }
    if (s->pblock == 0 && s->plen == 1) { return 64u * s->o.erase_ms_16k + 1u; }
    if (s->pblock == 0) { return 1; }
    return s->o.write_ms + 1u;
}

static int dfu_control(lt_fw_sim *s, lt_u8 rt, lt_u8 req, lt_u16 value, lt_u8 *data, lt_u16 len)
{
    double t = now(s);
    if (rt == 0x21 && req == 1) {                                  /* DNLOAD */
        s->n_dnload++;
        if (s->state != S_IDLE && s->state != S_DNLOAD_IDLE) { return dfu_stall(s); }
        if (s->o.stall_dnload > 0 && s->n_dnload == s->o.stall_dnload) { return dfu_stall(s); }
        if (len > SIM_XFER || (value == 1)) { return dfu_stall(s); }
        if (len == 0) {
            s->state = S_MANIFEST_SYNC;
            return 0;
        }
        memcpy(s->pbuf, data, len);
        s->plen = len;
        s->pblock = value;
        s->state = S_DNLOAD_SYNC;
        return len;
    }
    if (rt == 0xA1 && req == 3) {                                  /* GETSTATUS */
        lt_u32 poll = 0;
        if (len < 6) { return dfu_stall(s); }
        if (s->state == S_DNLOAD_SYNC) {
            poll = dfu_op_ms(s);
            s->busy_until = t + (double)poll;
            s->state = S_DNBUSY;
        } else if (s->state == S_DNBUSY) {
            if (t + 0.001 < s->busy_until) {
                s->early++;
                poll = (lt_u32)(s->busy_until - t) + 1u;
            } else {
                int st, rep = s->o.err_repeat > 0 ? s->o.err_repeat : 1;
                if (s->pblock >= 2) { s->n_busyend++; }
                if (s->pblock >= 2 && s->o.err_status > 0 && s->n_busyend >= s->o.err_status &&
                    s->n_busyend < s->o.err_status + rep) {
                    st = E_PROG;                                   /* injected program failure */
                } else {
                    st = dfu_execute(s);
                }
                if (st != 0) { s->state = S_ERROR; s->status = st; }
                else { s->state = S_DNLOAD_IDLE; }
            }
        } else if (s->state == S_MANIFEST_SYNC) {
            s->state = S_MANIFEST;
            s->left = 1;                                           /* jumps to the firmware */
        }
        data[0] = (lt_u8)s->status;
        data[1] = (lt_u8)(poll & 0xFFu);
        data[2] = (lt_u8)((poll >> 8) & 0xFFu);
        data[3] = (lt_u8)((poll >> 16) & 0xFFu);
        data[4] = (lt_u8)s->state;
        data[5] = 0;
        return 6;
    }
    if (rt == 0x21 && req == 4) {                                  /* CLRSTATUS */
        if (s->state != S_ERROR) { return dfu_stall(s); }
        s->state = S_IDLE;
        s->status = 0;
        return 0;
    }
    if (rt == 0x21 && req == 6) {                                  /* ABORT */
        if (s->state == S_DNBUSY || s->state == S_ERROR) { return dfu_stall(s); }
        s->state = S_IDLE;
        return 0;
    }
    if (rt == 0xA1 && req == 5) {                                  /* GETSTATE */
        if (len < 1) { return dfu_stall(s); }
        data[0] = (lt_u8)s->state;
        return 1;
    }
    if (rt == 0xA1 && req == 2) {                                  /* UPLOAD */
        lt_u32 a;
        if (s->state != S_IDLE && s->state != S_UPLOAD_IDLE) { return dfu_stall(s); }
        if (value < 2 || len == 0 || len > SIM_XFER) { return dfu_stall(s); }
        a = s->ptr + (lt_u32)(value - 2) * (lt_u32)len;
        if (!in_flash(a, len)) { return dfu_stall(s); }
        memcpy(data, s->flash + (a - SIM_BASE), len);
        s->state = S_UPLOAD_IDLE;
        return len;
    }
    return dfu_stall(s);
}

/* ------------------------------------------------------------------ OpenBLT */
static void xr(lt_fw_sim *s, const lt_u8 *p, int n, double delay)
{
    s->resp[0] = (lt_u8)n;
    memcpy(s->resp + 1, p, (size_t)n);
    s->resp_len = n + 1;
    s->resp_pos = 0;
    s->resp_at = now(s) + delay;
}

static void xr_err(lt_fw_sim *s, int code)
{
    lt_u8 e[2];
    e[0] = 0xFE;
    e[1] = (lt_u8)code;
    xr(s, e, 2, 0.0);
}

static void xcp_cmd(lt_fw_sim *s, const lt_u8 *c, int n)
{
    lt_u8 r[300];
    int cto = s->o.max_cto > 0 ? s->o.max_cto : 63;
    memset(r, 0, sizeof(r));
    r[0] = 0xFF;
    if (n < 1) { return; }
    if (c[0] == 0xFF) {
        if (s->o.connect_ignore > 0) { s->o.connect_ignore--; return; }
        s->connected = 1;
        r[1] = 0x10; r[2] = 0x00; r[3] = (lt_u8)cto; r[4] = (lt_u8)cto; r[5] = 0; r[6] = 1; r[7] = 1;
        xr(s, r, 8, 0.0);
        return;
    }
    if (!s->connected) { return; }
    switch (c[0]) {
    case 0xFD:
        r[2] = s->o.pgm_protected ? 0x10 : 0x00;
        xr(s, r, 6, 0.0);
        return;
    case 0xD2:
        if (s->o.pgm_protected) { xr_err(s, 0x25); return; }
        s->pgm = 1;
        r[3] = (lt_u8)cto;
        xr(s, r, 7, 0.0);
        return;
    case 0xF6:
        if (n < 8) { xr_err(s, 0x21); return; }
        s->mta = lt_load_u32le(c + 4);
        xr(s, r, 1, 0.0);
        return;
    case 0xD1: {
        lt_u32 len, a, st = 0, sz = 0;
        if (n < 8 || !s->pgm) { xr_err(s, 0x29); return; }
        len = lt_load_u32le(c + 4);
        if (s->mta < SIM_LOADER_END || len == 0 || !in_flash(s->mta, len)) { xr_err(s, 0x22); return; }
        for (a = s->mta; a < s->mta + len; a = st + sz) {
            if (sector_of(a, &st, &sz) < 0) { break; }
            erase_sector(s, a);
        }
        xr(s, r, 1, (double)s->o.busy_ms);
        return;
    }
    case 0xD0:
    case 0xC9: {
        lt_u32 cnt = c[0] == 0xC9 ? (lt_u32)(cto - 1) : (n >= 2 ? c[1] : 0u);
        const lt_u8 *d = c[0] == 0xC9 ? c + 1 : c + 2;
        if (!s->pgm) { xr_err(s, 0x29); return; }
        if (c[0] == 0xD0 && cnt > (lt_u32)(cto - 2)) { xr_err(s, 0x21); return; }
        if ((int)cnt > n - (c[0] == 0xC9 ? 1 : 2)) { xr_err(s, 0x21); return; }
        if (cnt > 0) {
            if (s->mta < SIM_LOADER_END) { xr_err(s, 0x22); return; }
            if (program(s, s->mta, d, cnt) != 0) { xr_err(s, 0x31); return; }
            s->mta += cnt;
        }
        xr(s, r, 1, 0.0);
        return;
    }
    case 0xF5: {
        lt_u32 cnt = n >= 2 ? c[1] : 0u;
        if (cnt == 0 || cnt > (lt_u32)(cto - 1) || !in_flash(s->mta, cnt)) { xr_err(s, 0x22); return; }
        memcpy(r + 1, s->flash + (s->mta - SIM_BASE), cnt);
        s->mta += cnt;
        xr(s, r, (int)cnt + 1, 0.0);
        return;
    }
    case 0xCF:
        s->left = 1;
        xr(s, r, 1, 0.0);
        return;
    case 0xFE:
        s->connected = 0;
        xr(s, r, 1, 0.0);
        return;
    default:
        xr_err(s, 0x20);
        return;
    }
}

static int sim_bulk(void *ctx, lt_u8 ep, lt_u8 *data, int len, int timeout_ms)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    LT_UNUSED(timeout_ms);
    if (s->o.kind != LT_FW_SIM_OPENBLT) { return LT_USB_EPIPE; }
    if (ep == 0x01) {
        if (s->left) { return LT_USB_ENODEV; }
        if (len < 1 || data[0] + 1 > len) { return LT_USB_EINVAL; }
        xcp_cmd(s, data + 1, data[0]);
        return len;
    }
    if (ep == 0x81) {
        int n;
        if (s->resp_len == 0 || now(s) < s->resp_at) { return s->left ? LT_USB_ENODEV : LT_USB_ETIMEOUT; }
        n = s->resp_len - s->resp_pos;
        if (n > 64) { n = 64; }
        if (n > len) { n = len; }
        memcpy(data, s->resp + s->resp_pos, (size_t)n);
        s->resp_pos += n;
        if (s->resp_pos >= s->resp_len) { s->resp_len = 0; s->resp_pos = 0; }
        return n;
    }
    return LT_USB_EINVAL;
}

static int sim_control(void *ctx, lt_u8 rt, lt_u8 req, lt_u16 value, lt_u16 index, lt_u8 *data,
                       lt_u16 len, int timeout_ms)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    LT_UNUSED(timeout_ms);
    if (s->left) { return LT_USB_ENODEV; }
    if (s->o.kind != LT_FW_SIM_DFUSE || index != 0) { return LT_USB_EPIPE; }
    return dfu_control(s, rt, req, value, data, len);
}

/* ------------------------------------------------------------------ setup */
static lt_u32 xs_next(lt_u32 *x)
{
    lt_u32 v = *x;
    v ^= (v << 13) & LT_U32_MASK;
    v ^= v >> 17;
    v ^= (v << 5) & LT_U32_MASK;
    *x = v & LT_U32_MASK;
    return *x;
}

lt_fw_sim *lt_fw_sim_new(const lt_fw_sim_opts *o, lt_fw_clock_fn clock, void *clock_user)
{
    lt_fw_sim *s = (lt_fw_sim *)lt_calloc(1, sizeof(*s));
    lt_u32 i, x = 0x1234567UL;
    if (s == NULL) { return NULL; }
    s->flash = (lt_u8 *)lt_malloc(SIM_SIZE);
    if (s->flash == NULL) { lt_free(s); return NULL; }
    if (o != NULL) { s->o = *o; }
    if (s->o.erase_ms_16k == 0 && o == NULL) { s->o.erase_ms_16k = 30; }
    s->clock = clock;
    s->cu = clock_user;
    s->state = S_IDLE;
    /* an old firmware everywhere, recognisable settings in sectors 10 and 11 */
    for (i = 0; i < SIM_SIZE; i++) {
        s->flash[i] = (lt_u8)(xs_next(&x) & 0x7Fu);
    }
    for (i = LT_FW_RUSEFI_CFG - SIM_BASE; i < SIM_SIZE; i++) { s->flash[i] = (lt_u8)(0x5A ^ (i & 0xFFu)); }
    return s;
}

void lt_fw_sim_free(lt_fw_sim *s)
{
    if (s == NULL) { return; }
    lt_free(s->flash);
    lt_free(s);
}

void lt_fw_sim_io(lt_fw_sim *s, lt_fw_io *io)
{
    memset(io, 0, sizeof(*io));
    io->ctx = s;
    io->control = sim_control;
    io->bulk = sim_bulk;
    io->config_desc = sim_config;
    io->string_desc = sim_string;
    io->claim = sim_claim;
}

lt_u8 *lt_fw_sim_flash(lt_fw_sim *s, lt_u32 *base, lt_u32 *size)
{
    if (base != NULL) { *base = SIM_BASE; }
    if (size != NULL) { *size = SIM_SIZE; }
    return s->flash;
}

int lt_fw_sim_early_polls(const lt_fw_sim *s) { return s->early; }
int lt_fw_sim_erases(const lt_fw_sim *s) { return s->erases; }
int lt_fw_sim_left(const lt_fw_sim *s) { return s->left; }

void lt_fw_sim_demo_image(lt_fw_image *img, int with_loader, lt_u32 app_kb)
{
    lt_u8 *b;
    lt_u32 i, x = 0xC0FFEEUL, size = app_kb * 1024u, crc;
    lt_fw_image_init(img);
    if (size < 0x400u) { size = 0x400u; }
    b = (lt_u8 *)lt_malloc(size > 0x6000u ? size : 0x6000u);
    if (b == NULL) { return; }
    if (with_loader) {
        for (i = 0; i < 0x6000u; i++) { b[i] = (lt_u8)xs_next(&x); }
        lt_store_u32le(b, 0x20020000UL);
        lt_store_u32le(b + 4, 0x08000189UL);
        (void)lt_fw_image_add(img, SIM_BASE, b, 0x6000u, NULL, 0);
        memset(b, 0xFF, 0x2000u);
        (void)lt_fw_image_add(img, SIM_BASE + 0x6000UL, b, 0x2000u, NULL, 0);   /* fill up to the app */
    }
    for (i = 0; i < size; i++) { b[i] = (lt_u8)xs_next(&x); }
    lt_store_u32le(b, 0x20020000UL);
    lt_store_u32le(b + 4, 0x08008201UL);
    lt_store_u32le(b + 0x20, size);
    crc = lt_crc32(0, b, 0x1C);
    crc = lt_crc32(crc, b + 0x20, (size_t)size - 0x20u);
    lt_store_u32le(b + 0x1C, crc);
    (void)lt_fw_image_add(img, LT_FW_RUSEFI_APP, b, size, NULL, 0);
    lt_free(b);
    img->format = LT_FW_FMT_BIN;
    (void)lt_fw_image_finish(img, NULL, 0);
}
